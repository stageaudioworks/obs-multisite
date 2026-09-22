// SPDX-License-Identifier: GPL-3.0-or-later
#include "cloud_identity.h"

#include "../vendor/nlohmann/json.hpp"

using json = nlohmann::json;

namespace multisite {
namespace {

// Refresh at half the remaining TTL. Not a constant fraction of the ORIGINAL
// TTL measured from issue: recomputing from `now` each time means a device that
// was offline for a while still refreshes promptly once it is back, rather than
// on a schedule that no longer lines up with the expiry it is refreshing.
constexpr long long kRefreshNumerator = 1;
constexpr long long kRefreshDenominator = 2;

// A floor, so a collector that returns a very short TTL cannot turn the client
// into a load test. Chooses a fast refresh over a tight loop; if the TTL is
// shorter than this the credentials may still expire, which the caller sees as
// a failed fetch rather than as hammering.
constexpr long long kMinRefreshMs = 5000;

std::string str_field(const json& j, const char* key) {
    if (!j.contains(key)) return std::string();
    const json& v = j[key];
    if (!v.is_string()) return std::string();
    return v.get<std::string>();
}

long long int_field(const json& j, const char* key, long long fallback) {
    if (!j.contains(key)) return fallback;
    const json& v = j[key];
    if (v.is_number_integer()) return v.get<long long>();
    return fallback;
}

bool bool_field(const json& j, const char* key, bool fallback) {
    if (!j.contains(key)) return fallback;
    const json& v = j[key];
    if (v.is_boolean()) return v.get<bool>();
    return fallback;
}

} // namespace

CredentialsReply cloud_parse_credentials(const std::string& body,
                                         int http_code) {
    CredentialsReply out;
    // 403 is the unpaired signal and is decided before the body is looked at:
    // the collector may say nothing useful in it, and the status alone carries
    // the meaning.
    if (http_code == 403) {
        out.unpaired = true;
        return out;
    }
    if (http_code != 200) return out;
    try {
        json j = json::parse(body);
        if (!j.is_object()) return out;
        Credentials c;
        c.bucket        = str_field(j, "bucket");
        c.endpoint      = str_field(j, "endpoint");
        c.session_token = str_field(j, "session_token");
        c.expires_at_ms = int_field(j, "expires_at", 0);
        // An explicit word, not an inferrable one: the collector says which
        // role the device was granted. Absent means the safer read.
        const std::string role = str_field(j, "role");
        c.read_write = (role == "encoder" || role == "readwrite");
        // A bucket is what makes the set usable. Without one the reply is
        // useless however well-formed, and treating it as success would clear
        // a working last-good set.
        if (c.bucket.empty()) return out;
        out.creds = c;
        out.ok = true;
    } catch (...) {
        // A malformed body is a failed fetch, never a thrown exception.
    }
    return out;
}

long long cloud_next_refresh_ms(const Credentials& c, long long now_ms) {
    if (!c.present()) return 0;
    const long long remaining = c.expires_at_ms - now_ms;
    if (remaining <= 0) return 0;
    const long long half = (remaining * kRefreshNumerator) / kRefreshDenominator;
    return half < kMinRefreshMs ? kMinRefreshMs : half;
}

void CloudIdentity::set_enrolment(const std::string& collector_url,
                                  const std::string& appliance_id,
                                  const std::string& appliance_token) {
    m_url   = collector_url;
    m_id    = appliance_id;
    m_token = appliance_token;
    // A fresh enrolment is a fresh device: the old unpaired/error state and the
    // old credentials belong to the identity that was just replaced.
    m_unpaired = false;
    m_error.clear();
    m_creds = Credentials{};
    // 0 means "fetch on the next tick", which is what boot wants.
    m_next_fetch_at_ms = 0;
}

CloudAction CloudIdentity::tick(long long now_ms) const {
    if (!paired()) return CloudAction::Idle;
    // A 403 stopped this device deliberately. Fetching again would be retrying
    // a revocation, which is how a device becomes a stuck one.
    if (m_unpaired) return CloudAction::Idle;
    if (m_next_fetch_at_ms == 0) return CloudAction::Fetch;
    if (now_ms >= m_next_fetch_at_ms) return CloudAction::Fetch;
    return CloudAction::Idle;
}

void CloudIdentity::on_credentials(const CredentialsReply& r, long long now_ms) {
    if (r.unpaired) {
        // STOP, and do not clear the last-good set: a running event is covered
        // by it until expiry (the decided lapse behaviour). What stops is
        // fetching and any NEW event that would need fresh credentials.
        m_unpaired = true;
        m_error = "the device was unpaired by the collector";
        m_next_fetch_at_ms = 0;   // no schedule; tick() returns Idle anyway
        if (m_creds.present()) m_creds.from_last_good = true;
        return;
    }

    if (r.ok) {
        m_creds = r.creds;
        m_creds.from_last_good = false;
        m_error.clear();
        m_next_fetch_at_ms = now_ms + cloud_next_refresh_ms(m_creds, now_ms);
        return;
    }

    // Any other failure: keep what we have, mark it stale, and try again on the
    // schedule the old set's TTL allows. A collector outage must not stop a
    // live event, and must not clear credentials the event depends on.
    m_error = "could not refresh credentials";
    if (m_creds.present()) {
        m_creds.from_last_good = true;
        const long long wait = cloud_next_refresh_ms(m_creds, now_ms);
        // Retry even if the old set has no time left, so a transient outage is
        // recovered from rather than ending in permanent silence. A short
        // retry, not the TTL: the credentials may already be expired.
        m_next_fetch_at_ms = now_ms + (wait > 0 ? wait : kMinRefreshMs);
    } else {
        // Nothing to fall back on. Try again soon rather than never.
        m_next_fetch_at_ms = now_ms + kMinRefreshMs;
    }
}

void CloudIdentity::reset() {
    m_url.clear();
    m_id.clear();
    m_token.clear();
    m_creds = Credentials{};
    m_unpaired = false;
    m_next_fetch_at_ms = 0;
    m_error.clear();
}

} // namespace multisite
