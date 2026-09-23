// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// heartbeat_reporter.h — the monitoring heartbeat's shape and schedule, shared
// by every reporter host.
//
// The wire contract lives in multisite-cloud (`docs/reporter-brief.md`,
// TELEMETRY.md §§1-3) and is restated here so this header stands alone: POST
// {collector_url}/v1/heartbeat, every 30 s while a role is active and every
// 5 min while idle, fire-and-forget. A 429 or Retry-After lengthens the
// interval; an {"interval_s": n} body adopts the central interval.
//
// This header is deliberately the ONLY authority for which status fields go
// on the wire (standards §2: a value derived in two places will drift). The
// rule from the brief is verbatim-under-existing-names: when the plugin gains
// a field, the collector gets it for free only if the field was already
// listed here — a rename here is a permanent translation layer later, so
// there isn't one. The screenshot test applies: cache_dir, secrets, endpoint
// details beyond storage_host, UI chrome and LAN visibility fields are never
// listed.
//
// site_name WAS on that list and is not now (2026-09-22, the operator's call).
// Unlike cache_dir it had no stated reason for being there, and it is the one
// thing a fleet dashboard most needs — which site is this. It also passes the
// brief's own test: it is already in the church's bucket, carried as every
// cue's `author` and naming each site's cue file. It goes on the wire verbatim,
// under the name the dock already uses. The brief in multisite-cloud
// (docs/reporter-brief.md, "Explicitly excluded") still lists it and needs the
// same change, or the two repos describe different contracts.
//
// Licensing: this lives under this repo's GPL. It must not import, link or
// copy anything from the proprietary control-plane repository; the HTTP+JSON
// contract above is the entire coupling.
//
// Portable: no OBS, no Qt, no curl, no clock. Hosts own the thread, the HTTP
// and the time; the arithmetic here is covered by tests/test_heartbeat_reporter.cpp.
//
#include <string>
#include <vector>

namespace multisite {

// Payload version on the wire. Bumped only for a change the collector must
// distinguish; additive status fields do not move it.
inline constexpr int kHeartbeatVersion = 1;

// Base schedule: 30 s while a role is active (encoder live; decoder/player
// playing, paused or buffering), 5 min while idle. Both from the brief.
inline constexpr int kHeartbeatActiveIntervalS = 30;
inline constexpr int kHeartbeatIdleIntervalS = 300;

// Appliance kinds on the wire. This pass wires obs-encoder, obs-decoder and
// pi-player; x86-player and relay are accepted strings with no host yet, so a
// future host adds wiring rather than a protocol change. The campus player
// sends pi-player unless its config names another player kind: outpost-light
// and outpost-pro are the same player on other hardware, set by whatever
// installed it there.
inline constexpr const char* kReporterKinds[] = {
    "obs-encoder", "obs-decoder", "pi-player", "x86-player", "relay",
    "outpost-light", "outpost-pro",
};
bool heartbeat_kind_known(const std::string& kind);

// The kind the campus player reports: `configured` when it is a known kind the
// player can be, otherwise pi-player. The OBS kinds and relay are refused
// because the player is neither, and a typo must not pair a box as something
// it is not.
std::string heartbeat_player_kind(const std::string& configured);

// A board serial as read from the device tree (NUL-terminated, sometimes with
// a trailing newline), cleaned to what may go on the wire: letters, digits,
// '-' and '_', at most 64 characters. Anything else yields "", which the
// pairing body then omits — no serial is better than a mangled one.
std::string heartbeat_clean_serial(const std::string& raw);

// The verbatim field selections from the brief. Encoder names come from
// encoder_status_json, decoder names from decoder_status_json
// (src/obs/web/commands.cpp). The Pi player filters through the decoder list:
// its status document is decoder-shaped (src/appliance/api.cpp:status_json).
const std::vector<std::string>& heartbeat_encoder_fields();
const std::vector<std::string>& heartbeat_decoder_fields();

// A relay reports what neither of the others can: how many places a service is
// going to, how many are actually on air, and what that costs in upload. It
// reads the bucket like a decoder but its status document is not decoder-
// shaped, so filtering it through the decoder list would have sent almost
// nothing — every relay-only field dropped, and the fields that did survive
// describing a playhead a relay does not have.
const std::vector<std::string>& heartbeat_relay_fields();

// Keep only the listed fields of a status document. `role` is "encoder",
// "decoder" (pi-player hosts pass "decoder") or "relay". Anything else, or a body that
// is not a JSON object, yields "{}" rather than throwing: a malformed status
// must not be the thing that stops a heartbeat, and the collector answers
// malformed bodies with 400 rather than us refusing to send.
// link_health is the one deliberate normalisation, not a rename: the OBS
// status carries words already and they pass through untouched, while the
// appliance carries 0/1/2 (api.cpp) and is mapped to the same words — the
// brief's acceptance pins the words, so the mapping lives here, once, tested.
std::string heartbeat_filter_status(const std::string& role,
                                    const std::string& status_json);

// 0/1/2 (appliance) to Healthy/Degraded/Offline. Anything else yields "" so
// the caller omits the field rather than sending a word the collector never
// promised to read.
std::string heartbeat_link_word(int v);
// Passthrough for a word the status already carries: the three known words
// survive, anything else yields "" for the same reason.
std::string heartbeat_link_word_str(const std::string& s);

// How long until the next heartbeat, in seconds. `central_interval_s` is the
// collector's {"interval_s": n} when one arrived, else 0. `rate_limited` is a
// 429 or a Retry-After on the last POST; `retry_after_s` its seconds when the
// header named one, else 0. Central adoption wins outright; otherwise the
// base (30 active / 300 idle) holds unless rate-limited, which never shortens
// it. Clamped to [10, 3600] so a hostile or mistyped interval cannot turn a
// monitor into a load test or into silence.
int heartbeat_next_interval_s(bool active, int central_interval_s,
                              bool rate_limited, int retry_after_s);

// The collector's {"interval_s": n} in a heartbeat response body, or 0 when
// absent or unparseable. Never throws.
int heartbeat_parse_server_interval(const std::string& body);

struct HeartbeatIdentity {
    std::string id;               // operator-provisioned appliance id
    std::string kind;             // one of kReporterKinds
    std::string product_version;  // PLUGIN_VERSION / player_version(), verbatim
    double      uptime_s = 0;
};

// Appliance-only host block, read from the OS at report time (sysinfo.h).
// Absent sensors read absent, never fabricated: cpu_pct < 0 (no second
// reading yet) and temp_c marked absent are omitted, and a negative
// disk_free_bytes is omitted. The OBS plugin always passes null.
struct HeartbeatHost {
    double    cpu_pct = -1;       // whole box, 0-100; < 0 = not known yet
    double    mem_pct = -1;       // < 0 = not known
    long long disk_free_bytes = -1;  // buffer filesystem; < 0 = unknown
    double    temp_c = 0;
    bool      has_temp = false;   // false = box cannot report one, omit
    bool      throttled = false;  // under-voltage or thermal flags
};

// Build the POST body. `filtered_status_json` is heartbeat_filter_status()'s
// output; `sent_at_iso` is the host's wall-clock time in ISO-8601 (a wall
// time — never mixed with the media times inside status, see CONTEXT.md).
// Never throws; a malformed status embeds as {}.
std::string heartbeat_build(const HeartbeatIdentity& id,
                            const HeartbeatHost* host_or_null,
                            const std::string& filtered_status_json,
                            const std::string& sent_at_iso);

// ── Pairing (preferred) — TELEMETRY.md §4, host-owned HTTP ─────────────────
// Parsers only; the POSTs belong to the hosts. Never throw.
struct PairStartReply {
    bool        ok = false;
    std::string user_code;
    std::string verification_url;
    std::string poll_token;
    int         interval_s = 5;
    int         expires_in = 900;
};
PairStartReply heartbeat_parse_pair_start(const std::string& body);

struct PairPollReply {
    bool        ok = false;       // credentials arrived
    bool        pending = false;  // 428-style: keep waiting
    std::string appliance_id;
    std::string appliance_token;
    std::string collector_url;
    // Optional, and only ever for appliance hardware: a second token the
    // collector scopes to fetching that hardware's software updates. The
    // player never uses it; it hands it to whatever installed it there
    // (Config::update_token_file) and forgets it.
    std::string update_token;
    int         interval_s = 30;
};
PairPollReply heartbeat_parse_pair_poll(const std::string& body, int http_code);

// ── Pairing (device-code flow, TELEMETRY.md §4) ────────────────────────────
// One authority for the pairing state machine, shared by the OBS and Pi hosts
// (standards §2). Pure: no clock, no network — the host POSTs what
// request bodies this builds, feeds replies back in, and asks when to poll.
// Times are wall-clock seconds; the host supplies them so tests control time.
std::string heartbeat_mint_device_id(const std::string& hostname,
                                     const std::string& kind,
                                     long long now_ns, long long pid);
// `serial` is omitted from the body when empty rather than sent as "".
std::string heartbeat_pair_start_body(const std::string& device_id,
                                      const std::string& kind,
                                      const std::string& hostname,
                                      const std::string& serial = std::string());
std::string heartbeat_pair_poll_body(const std::string& poll_token);

class Pairing {
public:
    enum class Phase { Idle, Waiting, Done, Expired, Failed };

    Pairing() = default;

    // Begin from a /v1/pair/start reply. `now_s` starts the expiry clock.
    // A useless reply (no code/token) moves to Failed, never to Waiting.
    void on_start_reply(const std::string& body, int http_code, long long now_s);
    // True when a poll is due: waiting, past the interval, inside expiry.
    bool poll_due(long long now_s) const;
    // Feed a /v1/pair/poll reply. Claimed credentials land in the state;
    // anything else stays Waiting (pending) or moves to Failed.
    void on_poll_reply(const std::string& body, int http_code, long long now_s);
    // Re-evaluate expiry without network (a tick with nothing due).
    void tick(long long now_s);
    void cancel();

    Phase phase() const { return m_phase; }
    const std::string& user_code() const { return m_user_code; }
    const std::string& verification_url() const { return m_verification_url; }
    const std::string& poll_token() const { return m_poll_token; }
    int poll_interval_s() const { return m_poll_interval_s; }
    const std::string& error() const { return m_error; }
    const std::string& appliance_id() const { return m_appliance_id; }
    const std::string& appliance_token() const { return m_appliance_token; }
    const std::string& collector_url() const { return m_collector_url; }
    const std::string& update_token() const { return m_update_token; }
    int heartbeat_interval_s() const { return m_heartbeat_interval_s; }

private:
    Phase m_phase = Phase::Idle;
    std::string m_user_code, m_verification_url, m_poll_token;
    int m_poll_interval_s = 5;
    long long m_deadline_s = 0;
    long long m_next_poll_s = 0;
    std::string m_error;
    std::string m_appliance_id, m_appliance_token, m_collector_url, m_update_token;
    int m_heartbeat_interval_s = 30;
};

} // namespace multisite
