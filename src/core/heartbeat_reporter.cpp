// SPDX-License-Identifier: GPL-3.0-or-later
#include "heartbeat_reporter.h"
#include "crypto.h"
#include "../vendor/nlohmann/json.hpp"

using json = nlohmann::json;

namespace multisite {
namespace {

// Clamp for heartbeat_next_interval_s: 10 s stops a mistyped central
// interval turning a monitor into a load test; 3600 s stops one turning it
// into silence. Both bounds are arbitrary and deliberately round — the point
// is that a bound exists, not what it is.
constexpr int kMinIntervalS = 10;
constexpr int kMaxIntervalS = 3600;

int clamp_interval(int v) {
    if (v < kMinIntervalS) return kMinIntervalS;
    if (v > kMaxIntervalS) return kMaxIntervalS;
    return v;
}

int int_field(const json& j, const char* key, int fallback) {
    if (!j.contains(key)) return fallback;
    const json& v = j[key];
    if (v.is_number_integer()) return v.get<int>();
    return fallback;
}

std::string str_field(const json& j, const char* key) {
    if (!j.contains(key)) return std::string();
    const json& v = j[key];
    if (!v.is_string()) return std::string();
    return v.get<std::string>();
}

} // namespace

bool heartbeat_kind_known(const std::string& kind) {
    for (const char* k : kReporterKinds)
        if (kind == k) return true;
    return false;
}

// The brief's field selections, verbatim. Encoder names are
// encoder_status_json's, decoder names decoder_status_json's
// (src/obs/web/commands.cpp). Deliberately absent from both: cache_dir,
// site_name, bucket secrets, clock_skew_ms, seek_target_ms, UI chrome
// (other_page, have_source, locked variants beyond the listed `locked`),
// event-listing fields, and LAN visibility (lan_configured/lan_active) —
// the screenshot test.
const std::vector<std::string>& heartbeat_encoder_fields() {
    static const std::vector<std::string> k = {
        "role", "version", "uptime_s", "now_ms", "configured", "live",
        "room_id", "event_id", "event_name", "bucket", "storage_host",
        "colo", "link_health", "link_known", "last_error", "pending",
        "confirmed", "retries", "bytes", "upload_bytes_per_s",
        "upload_samples", "video_encoder_id", "marker_labels", "locked",
        "site_name",
    };
    return k;
}

const std::vector<std::string>& heartbeat_decoder_fields() {
    static const std::vector<std::string> k = {
        "role", "version", "now_ms", "room_id", "room_state", "event_id",
        "live_event_id", "live_elsewhere", "playing", "stopped", "paused",
        "buffering", "loading", "ended", "at_end", "was_live",
        "interrupted", "playhead_ms", "live_ms", "earliest_ms",
        "started_ms", "end_ms", "total_ms", "behind_live_s",
        "buffered_ahead_s", "cached_segments", "cached_spans",
        "link_health", "link_known", "last_error", "colo", "storage_host",
        "download_bytes_per_s", "download_samples", "audio_channels",
        "audio_track_label", "channel_labels", "markers", "configured",
        "site_name",
    };
    return k;
}

std::string heartbeat_link_word(int v) {
    switch (v) {
        case 0: return "Healthy";
        case 1: return "Degraded";
        case 2: return "Offline";
        default: return std::string();
    }
}

std::string heartbeat_link_word_str(const std::string& s) {
    if (s == "Healthy" || s == "Degraded" || s == "Offline") return s;
    return std::string();
}

std::string heartbeat_filter_status(const std::string& role,
                                    const std::string& status_json) {
    const std::vector<std::string>* keep = nullptr;
    if (role == "encoder")
        keep = &heartbeat_encoder_fields();
    else if (role == "decoder")
        keep = &heartbeat_decoder_fields();
    else
        return "{}";

    json src;
    try {
        src = json::parse(status_json);
    } catch (...) {
        return "{}";
    }
    if (!src.is_object()) return "{}";

    json out = json::object();
    for (const auto& f : *keep) {
        if (!src.contains(f)) continue;
        if (f == "link_health") {
            // The words are the contract (brief acceptance); the appliance
            // counts 0/1/2 instead, so it is mapped here rather than at two
            // call sites that would drift (standards §2).
            const json& v = src[f];
            if (v.is_string()) {
                const std::string w = heartbeat_link_word_str(v.get<std::string>());
                if (!w.empty()) out[f] = w;
            } else if (v.is_number_integer()) {
                const std::string w = heartbeat_link_word(v.get<int>());
                if (!w.empty()) out[f] = w;
            }
            continue;
        }
        out[f] = src[f];
    }
    return out.dump();
}

int heartbeat_next_interval_s(bool active, int central_interval_s,
                              bool rate_limited, int retry_after_s) {
    // Central adoption wins outright: the fleet backs off as one.
    if (central_interval_s > 0) return clamp_interval(central_interval_s);
    int base = active ? kHeartbeatActiveIntervalS : kHeartbeatIdleIntervalS;
    if (rate_limited) {
        // A 429 without a Retry-After still lengthens: doubling the base is
        // the honest "we were told to slow down" without inventing precision
        // the collector never gave us.
        const int asked = retry_after_s > 0 ? retry_after_s : base * 2;
        if (asked > base) base = asked;
    }
    return clamp_interval(base);
}

int heartbeat_parse_server_interval(const std::string& body) {
    try {
        json j = json::parse(body);
        if (!j.is_object()) return 0;
        if (!j.contains("interval_s")) return 0;
        const json& v = j["interval_s"];
        if (!v.is_number_integer()) return 0;
        const int n = v.get<int>();
        return n > 0 ? n : 0;
    } catch (...) {
        return 0;
    }
}

std::string heartbeat_build(const HeartbeatIdentity& id,
                            const HeartbeatHost* host_or_null,
                            const std::string& filtered_status_json,
                            const std::string& sent_at_iso) {
    json status;
    try {
        status = json::parse(filtered_status_json);
        if (!status.is_object()) status = json::object();
    } catch (...) {
        status = json::object();
    }

    json j;
    j["v"] = kHeartbeatVersion;
    j["sent_at"] = sent_at_iso;
    json app;
    app["id"] = id.id;
    app["kind"] = id.kind;
    app["product_version"] = id.product_version;
    app["uptime_s"] = id.uptime_s;
    j["appliance"] = std::move(app);
    if (host_or_null) {
        // Best effort (brief): unknown readings are omitted, never zeroed —
        // a fabricated 0% CPU reads as idle hardware rather than unmeasured.
        json h = json::object();
        if (host_or_null->cpu_pct >= 0) h["cpu_pct"] = host_or_null->cpu_pct;
        if (host_or_null->mem_pct >= 0) h["mem_pct"] = host_or_null->mem_pct;
        if (host_or_null->disk_free_bytes >= 0)
            h["disk_free_bytes"] = host_or_null->disk_free_bytes;
        if (host_or_null->has_temp) h["temp_c"] = host_or_null->temp_c;
        h["throttled"] = host_or_null->throttled;
        j["host"] = std::move(h);
    }
    j["status"] = std::move(status);
    return j.dump();
}

PairStartReply heartbeat_parse_pair_start(const std::string& body) {
    PairStartReply r;
    try {
        json j = json::parse(body);
        if (!j.is_object()) return r;
        r.user_code = str_field(j, "user_code");
        r.verification_url = str_field(j, "verification_url");
        r.poll_token = str_field(j, "poll_token");
        if (r.user_code.empty() || r.poll_token.empty()) return r;
        const int iv = int_field(j, "interval_s", 5);
        const int ex = int_field(j, "expires_in", 900);
        r.interval_s = iv > 0 ? iv : 5;
        r.expires_in = ex > 0 ? ex : 900;
        r.ok = true;
    } catch (...) {
    }
    return r;
}
PairPollReply heartbeat_parse_pair_poll(const std::string& body, int http_code) {
    PairPollReply r;
    // 428 (or 202 on some collectors) with a pending body means keep waiting
    // at interval_s; only 200 with credentials resolves.
    if (http_code != 200) {
        const int iv = heartbeat_parse_server_interval(body);
        // A pending answer honours the interval it names, defaulting to the
        // pair/start cadence rather than the heartbeat one.
        r.pending = true;
        r.interval_s = iv > 0 ? iv : 5;
        return r;
    }
    try {
        json j = json::parse(body);
        if (!j.is_object()) return r;
        r.appliance_id = str_field(j, "appliance_id");
        r.appliance_token = str_field(j, "appliance_token");
        r.collector_url = str_field(j, "collector_url");
        r.update_token = str_field(j, "update_token");
        // A collector_url that IS the verification page is not an API base, and
        // adopting it breaks every later call: the host appends /v1/pair/poll
        // and /v1/credentials, so ".../pair" becomes ".../pair/v1/pair/poll" —
        // a 404 on every request after that.
        //
        // Seen on a real collector 2026-09-22: the reply's collector_url came
        // back as the human-facing ".../pair" page, and the OBS encoder's
        // second pairing attempt failed 404 against ".../pair/v1/pair/start"
        // while the first had succeeded. (The saved settings were NOT
        // corrupted — the field is used in memory before any write — which is
        // why this is fixed where the value is PARSED rather than where it is
        // stored: one guard covers all three hosts and every use.)
        if (!r.collector_url.empty()) {
            std::string u = r.collector_url;
            while (!u.empty() && u.back() == '/') u.pop_back();
            const size_t slash = u.find_last_of('/');
            const std::string tail = slash == std::string::npos ? u
                                                                : u.substr(slash + 1);
            if (tail == "pair") {
                r.collector_url.clear();
            }
        }
        if (r.appliance_id.empty() || r.appliance_token.empty()) return r;
        const int iv = int_field(j, "interval_s", 30);
        r.interval_s = iv > 0 ? iv : 30;
        r.ok = true;
    } catch (...) {
    }
    return r;
}

// ── Pairing ──────────────────────────────────────────────────────────────────

// A local device id, minted once per machine and role (PROJECT-SCOPE §8.5:
// "the plugin mints a device id locally. No network."). An identifier, not a
// secret — uniqueness matters, unpredictability does not — so time, process
// and host hashed together are enough, with no entropy source to fail.
std::string heartbeat_mint_device_id(const std::string& hostname,
                                     const std::string& kind,
                                     long long now_ns, long long pid) {
    const std::string raw = hostname + "|" + kind + "|" +
                            std::to_string(now_ns) + "|" + std::to_string(pid);
    const std::string hex = crypto::sha256_hex(
        reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
    return "dev_" + hex.substr(0, 16);
}

std::string heartbeat_pair_start_body(const std::string& device_id,
                                      const std::string& kind,
                                      const std::string& hostname,
                                      const std::string& serial) {
    json j;
    j["device_id"] = device_id;
    j["kind"] = kind;
    j["hostname"] = hostname;
    if (!serial.empty()) j["serial"] = serial;
    return j.dump();
}

std::string heartbeat_player_kind(const std::string& configured) {
    static const char* const kPlayerKinds[] = {
        "pi-player", "x86-player", "outpost-light", "outpost-pro",
    };
    for (const char* k : kPlayerKinds)
        if (configured == k) return configured;
    return "pi-player";
}

std::string heartbeat_clean_serial(const std::string& raw) {
    std::string s = raw;
    while (!s.empty() && (s.back() == '\0' || s.back() == '\n' ||
                          s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    if (s.empty() || s.size() > 64) return std::string();
    for (char c : s) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                        (c >= 'A' && c <= 'Z') || c == '-' || c == '_';
        if (!ok) return std::string();
    }
    return s;
}

std::string heartbeat_pair_poll_body(const std::string& poll_token) {
    json j;
    j["poll_token"] = poll_token;
    return j.dump();
}

void Pairing::on_start_reply(const std::string& body, int http_code,
                             long long now_s) {
    const PairStartReply r = heartbeat_parse_pair_start(body);
    if (http_code != 200 || !r.ok) {
        m_phase = Phase::Failed;
        m_error = http_code == 200 ? "the collector answered but named no code"
                                   : "http " + std::to_string(http_code);
        return;
    }
    m_phase = Phase::Waiting;
    m_error.clear();
    m_user_code = r.user_code;
    m_verification_url = r.verification_url;
    m_poll_token = r.poll_token;
    m_poll_interval_s = r.interval_s;
    m_deadline_s = now_s + r.expires_in;
    m_next_poll_s = now_s + r.interval_s;
}

bool Pairing::poll_due(long long now_s) const {
    return m_phase == Phase::Waiting && now_s >= m_next_poll_s &&
           now_s < m_deadline_s;
}

void Pairing::on_poll_reply(const std::string& body, int http_code,
                            long long now_s) {
    if (m_phase != Phase::Waiting) return;
    const PairPollReply r = heartbeat_parse_pair_poll(body, http_code);
    if (r.ok) {
        m_phase = Phase::Done;
        m_appliance_id = r.appliance_id;
        m_appliance_token = r.appliance_token;
        m_collector_url = r.collector_url;
        m_update_token = r.update_token;
        m_heartbeat_interval_s = r.interval_s;
        return;
    }
    if (r.pending) {
        // Still waiting: honour the interval it names, and the deadline the
        // start reply set — a poll that says "wait longer" must not also
        // extend how long we wait in total.
        if (r.interval_s > 0) m_poll_interval_s = r.interval_s;
        m_next_poll_s = now_s + m_poll_interval_s;
        tick(now_s);
        return;
    }
    m_phase = Phase::Failed;
    m_error = "http " + std::to_string(http_code);
}

void Pairing::tick(long long now_s) {
    if (m_phase == Phase::Waiting && now_s >= m_deadline_s) {
        m_phase = Phase::Expired;
        m_error = "the code expired before anyone approved it";
    }
}

void Pairing::cancel() {
    m_phase = Phase::Idle;
    m_error.clear();
    m_user_code.clear();
    m_verification_url.clear();
    m_poll_token.clear();
    m_appliance_id.clear();
    m_appliance_token.clear();
    m_collector_url.clear();
    m_update_token.clear();
}

} // namespace multisite
