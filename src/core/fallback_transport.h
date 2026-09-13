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
// Deliberately per-request, not a single up-front "which one is healthy"
// decision: a segment that aged out of the LAN's bounded retention window
// (see lan_object_server.h) should fall back to cloud for THAT segment alone,
// not flip the whole session to cloud because one old fragment was asked for.
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
        if (r.success) { m_last_via_primary = true; return r; }
        m_last_via_primary = false;
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

    // Which path actually answered the last get() — the decoder dock's
    // "via LAN" / "via cloud" indicator (§8.7, "Visibility"). Reflects the
    // most recent attempt only; a decoder polls every few seconds, so this
    // is never stale for long.
    bool last_get_was_primary() const { return m_last_via_primary.load(); }

private:
    Transport& m_primary;    // LAN
    Transport& m_secondary;  // cloud
    std::atomic<bool> m_last_via_primary{false};
};

} // namespace multisite
