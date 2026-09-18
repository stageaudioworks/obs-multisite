// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// mirror_read_transport.h — read from the primary bucket, and from the second
// one only when the primary cannot serve the request.
// See PROJECT-SCOPE.md §10 Phase 9.
//
// Deliberately not the same shape as FallbackTransport, even though both are
// "prefer one, fall back to another". FallbackTransport's two ends are LAN and
// cloud, and its WRITE semantics follow from that: listing, deleting and
// writing are cloud-only concepts there, so they always go to the cloud. Here
// both ends are clouds and the caller is a decoder, so the interesting question
// is not "which end can hold a stream" but "which end is answering". Sharing
// the class would have meant carrying LAN's assumptions into a place they are
// not true.
//
// Why the primary is preferred at all: on a metered provider the second
// bucket's egress is a real cost, and a working primary should carry the load.
// So the second is reached for an object the primary could not serve — a
// connection failure or an ordinary miss — and for nothing else.
//
#include "transport.h"

#include <atomic>

namespace multisite {

class MirrorReadTransport : public Transport {
public:
    MirrorReadTransport(Transport& primary, Transport& secondary)
        : m_primary(primary), m_secondary(secondary) {}

    // When the caller has decided the primary has stopped advancing, reads go
    // the other way round. Not a health judgement — the caller makes that, and
    // it is the only thing that can: a primary that answers 200 with a stale
    // manifest looks perfectly healthy to a transport.
    void prefer_secondary(bool on) override { m_prefer_secondary = on; }
    bool preferring_secondary() const override { return m_prefer_secondary.load(); }

    GetResult get(const std::string& key) override {
        if (m_prefer_secondary.load()) return get_secondary_first(key);
        GetResult r = m_primary.get(key);
        if (r.success) {
            note_primary_reachable(true);
            m_served_secondary = false;
            return r;
        }
        // The primary answered with an ordinary miss (404) is NOT the same as
        // it being unreachable — an event that only ever reached the second
        // bucket while the first was down must still be readable, so a miss
        // falls through too. What differs is only what we conclude about the
        // primary's health.
        if (m_primary.last_request_reached_server()) note_primary_reachable(true);
        else                                        note_primary_reachable(false);

        GetResult r2 = m_secondary.get(key);
        if (r2.success) m_served_secondary = true;
        return r2;
    }

    int64_t object_size(const std::string& key) override {
        int64_t n = m_primary.object_size(key);
        if (n >= 0) return n;
        return m_secondary.object_size(key);
    }

    // The recordings list is a whole-bucket question and both targets answer it
    // the same way once they agree; the primary is asked first because it is
    // the one that is normally complete.
    ListResult list(const std::string& prefix, const std::string& delimiter = "",
                    const std::string& continuation_token = "",
                    int max_keys = 1000) override {
        ListResult r = m_primary.list(prefix, delimiter, continuation_token, max_keys);
        if (r.success) return r;
        return m_secondary.list(prefix, delimiter, continuation_token, max_keys);
    }

    // A decoder never writes or deletes; the primary is the honest answer if
    // anything ever does, and it is the target an operator means by "storage".
    PutResult put(const std::string& key, const std::vector<uint8_t>& body,
                  const std::string& content_type,
                  const std::map<std::string, std::string>& tags) override {
        return m_primary.put(key, body, content_type, tags);
    }
    DeleteResult remove(const std::string& key) override {
        return m_primary.remove(key);
    }

    void cancel_pending() override {
        m_primary.cancel_pending();
        m_secondary.cancel_pending();
    }
    void resume_pending() override {
        m_primary.resume_pending();
        m_secondary.resume_pending();
    }

    // The store's clock, from whichever end is answering. Preferred from the
    // primary, because that is the one whose skew the operator is shown.
    int64_t server_clock_skew_ms() const override {
        const int64_t s = m_primary.server_clock_skew_ms();
        return s != 0 ? s : m_secondary.server_clock_skew_ms();
    }

    // Reporting, for the dock and the appliance page: whether the last read
    // came from the second bucket, and whether the primary is answering at all.
    // Both are state, not events — a request for something the primary
    // legitimately does not have must not read as the primary being down, which
    // is the same distinction FallbackTransport makes for LAN.
    bool last_read_was_secondary() const { return m_served_secondary.load(); }
    bool primary_reachable() const { return m_primary_ok.load(); }

private:
    GetResult get_secondary_first(const std::string& key) {
        GetResult r = m_secondary.get(key);
        if (r.success) { m_served_secondary = true; return r; }
        // The other direction still falls ALL the way back, so a failover that
        // turns out to be the wrong call degrades into the old behaviour rather
        // than into an unreadable event.
        GetResult r2 = m_primary.get(key);
        m_served_secondary = false;
        note_primary_reachable(r2.success || m_primary.last_request_reached_server());
        return r2;
    }

    void note_primary_reachable(bool ok) { m_primary_ok = ok; }

    Transport& m_primary;
    Transport& m_secondary;
    std::atomic<bool> m_served_secondary{false};
    std::atomic<bool> m_primary_ok{true};
    std::atomic<bool> m_prefer_secondary{false};
};

} // namespace multisite
