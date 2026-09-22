// SPDX-License-Identifier: GPL-3.0-or-later
#include "cloud_identity.h"

#include "s3_transport.h"   // s3_config_from_credentials

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

// ISO-8601 UTC, the form the collector sends: YYYY-MM-DDTHH:MM:SS[.fff]Z.
// Hand-parsed rather than via strptime/timegm because those are POSIX-only and
// this file is in the portable core; the grammar here is tiny and fixed, which
// is exactly the case where writing it down beats a locale-dependent library
// call. Days-from-civil is the standard Howard Hinnant algorithm, valid for the
// whole range that matters here.
long long cloud_parse_iso8601_ms(const std::string& iso) {
    // The shortest acceptable form is YYYY-MM-DDTHH:MM:SSZ (20 chars).
    if (iso.size() < 20) return 0;
    auto num = [&iso](size_t at, size_t len, int& out) -> bool {
        if (at + len > iso.size()) return false;
        int v = 0;
        for (size_t i = 0; i < len; ++i) {
            const char c = iso[at + i];
            if (c < '0' || c > '9') return false;
            v = v * 10 + (c - '0');
        }
        out = v;
        return true;
    };

    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (!num(0, 4, year) || iso[4] != '-' ||
        !num(5, 2, month) || iso[7] != '-' ||
        !num(8, 2, day) ||
        (iso[10] != 'T' && iso[10] != ' ') ||
        !num(11, 2, hour) || iso[13] != ':' ||
        !num(14, 2, minute) || iso[16] != ':' ||
        !num(17, 2, second))
        return 0;
    if (month < 1 || month > 12 || day < 1 || day > 31) return 0;

    // Fractional seconds, optional and of any length: take the first three
    // digits as milliseconds and ignore the rest.
    long long ms = 0;
    if (iso.size() > 19 && iso[19] == '.') {
        int frac = 0, digits = 0;
        size_t at = 20;
        while (at < iso.size() && digits < 3 &&
               iso[at] >= '0' && iso[at] <= '9') {
            frac = frac * 10 + (iso[at] - '0');
            ++digits;
            ++at;
        }
        // "0.5" means 500 ms, not 5: scale by what was actually read.
        for (int i = digits; i < 3; ++i) frac *= 10;
        ms = frac;
    }

    // days_from_civil (Hinnant): a count of days from 1970-01-01, exact for
    // every date this will ever see.
    const int y = year - (month <= 2 ? 1 : 0);
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = (int)(y - era * 400);
    const int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long long days = era * 146097LL + doe - 719468LL;

    const long long secs = days * 86400LL + hour * 3600LL + minute * 60LL + second;
    return secs * 1000LL + ms;
}

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
        // Temporary credentials arrive as a key PAIR plus a session token
        // (measured 2026-09-22). All three are needed to sign; a token alone
        // signs with nothing.
        c.access_key_id     = str_field(j, "access_key_id");
        c.secret_access_key = str_field(j, "secret_access_key");
        c.session_token     = str_field(j, "session_token");
        // The expiry is an ISO-8601 string, not an integer: reading it as an
        // integer gave 0 and marked every set already-expired. Accept an
        // integer too, since a collector version may yet send one, but prefer
        // the string the service actually sends.
        if (j.contains("expires_at") && j["expires_at"].is_string())
            c.expires_at_ms = cloud_parse_iso8601_ms(j["expires_at"].get<std::string>());
        else
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
    std::lock_guard<std::mutex> lk(m_mtx);
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
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!paired_locked()) return CloudAction::Idle;
    // A 403 stopped this device deliberately. Fetching again would be retrying
    // a revocation, which is how a device becomes a stuck one.
    if (m_unpaired) return CloudAction::Idle;
    if (m_next_fetch_at_ms == 0) return CloudAction::Fetch;
    if (now_ms >= m_next_fetch_at_ms) return CloudAction::Fetch;
    return CloudAction::Idle;
}

void CloudIdentity::on_credentials(const CredentialsReply& r, long long now_ms) {
    std::lock_guard<std::mutex> lk(m_mtx);
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
    std::lock_guard<std::mutex> lk(m_mtx);
    m_url.clear();
    m_id.clear();
    m_token.clear();
    m_creds = Credentials{};
    m_unpaired = false;
    m_next_fetch_at_ms = 0;
    m_error.clear();
}

S3Config s3_config_from_credentials(const Credentials& c) {
    S3Config s3;
    s3.bucket = c.bucket;
    // Exactly as received. Brokered credentials name their own host; guessing
    // one from a region would be the "never guess a bucket" rule's twin.
    s3.endpoint_host = c.endpoint;
    s3.region = "auto";
    // Temporary credentials are a key PAIR plus a session token: the pair signs
    // and the token proves the pair is temporary. All three travel, or the
    // signature is made with nothing.
    s3.access_key_id     = c.access_key_id;
    s3.secret_access_key = c.secret_access_key;
    s3.session_token     = c.session_token;
    return s3;
}

} // namespace multisite
