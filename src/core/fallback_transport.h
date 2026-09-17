// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// fallback_transport.h — prefer a LAN transport, fall back to cloud per
// request. See PROJECT-SCOPE.md §8.7 ("Preference and fallback").
//
// DecoderSession only ever holds one Transport&, so the choice between LAN
// and cloud has to be made below it, not by it — DecoderSession never learns
// LAN delivery exists, the same design already used on the encoder side
// (Session's three LAN hooks). This class is the decoder-side equivalent of
// that boundary: a plain Transport that happens to try one thing before
// another.
//
// Which transport actually serves a given key is decided per request, not by
// a single up-front "which one is healthy" choice: a segment that aged out of
// the LAN's bounded retention window (see lan_object_server.h) should fall
// back to cloud for THAT segment alone, not flip the whole session to cloud
// because one old fragment was asked for. The HEALTH signal exposed for the
// dock (last_get_was_primary()) is a separate question, tracked by whether
// the primary was actually reachable — see get() below — precisely so an
// expected, harmless miss like that one doesn't masquerade as LAN being down.
//
#include "transport.h"

#include <atomic>

namespace multisite {

class FallbackTransport : public Transport {
public:
    FallbackTransport(Transport& primary, Transport& secondary)
        : m_primary(primary), m_secondary(secondary) {}

    GetResult get(const std::string& key) override {
        GetResult r = m_primary.get(key);
        if (r.success) { m_primary_healthy = true; return r; }
        // A miss on the primary does not by itself mean it's unhealthy — a
        // 404 for markers.json before the first marker, or a segment that
        // aged out of the LAN's retention window, are both ordinary and
        // expected while LAN is working fine. Only a genuine connection-level
        // failure (see Transport::last_request_reached_server) means the
        // primary itself is the problem, so only that updates the health
        // flag the decoder dock's "via LAN" / "via cloud" indicator reads.
        if (!m_primary.last_request_reached_server()) m_primary_healthy = false;
        return m_secondary.get(key);
    }

    // Writes, listing and deletes are cloud-only concepts here (a decoder
    // never writes; event browsing and storage management are inherently
    // whole-bucket operations a LAN endpoint has no way to answer) — always
    // the secondary transport, never attempted against the primary.
    PutResult put(const std::string& key, const std::vector<uint8_t>& body,
                  const std::string& content_type,
                  const std::map<std::string, std::string>& tags) override {
        return m_secondary.put(key, body, content_type, tags);
    }
    int64_t object_size(const std::string& key) override {
        return m_secondary.object_size(key);
    }
    ListResult list(const std::string& prefix, const std::string& delimiter = "",
                    const std::string& continuation_token = "",
                    int max_keys = 1000) override {
        return m_secondary.list(prefix, delimiter, continuation_token, max_keys);
    }
    DeleteResult remove(const std::string& key) override {
        return m_secondary.remove(key);
    }

    void cancel_pending() override {
        m_primary.cancel_pending();
        m_secondary.cancel_pending();
    }

    // Whether the primary (LAN) is currently healthy — the decoder dock's
    // "via LAN" / "via cloud" indicator (§8.7, "Visibility"). NOT "did the
    // very last get() happen to come from LAN": a request for something LAN
    // legitimately doesn't have (see get() above) leaves this unchanged,
    // rather than flipping the indicator to "cloud" for a request that says
    // nothing about whether LAN itself is working.
    bool last_get_was_primary() const { return m_primary_healthy.load(); }

    // The store's clock, never the LAN server's: the LAN endpoint's Date header
    // is the encoder's own clock — the very thing being checked — so it is not
    // a reference. Only the cloud leg can answer this.
    int64_t server_clock_skew_ms() const override {
        return m_secondary.server_clock_skew_ms();
    }

private:
    Transport& m_primary;    // LAN
    Transport& m_secondary;  // cloud
    std::atomic<bool> m_primary_healthy{false};
};

} // namespace multisite
