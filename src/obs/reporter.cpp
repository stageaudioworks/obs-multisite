// SPDX-License-Identifier: GPL-3.0-or-later
#include "reporter.h"

#include "broadcast_controller.h"
#include "decoder_settings.h"
#include "multisite_ui.h"
#include "plugin_log.h"
#include "plugin_role.h"
#include "web/commands.h"
#include "web/ui_thread.h"
#include "../core/heartbeat_reporter.h"

#include <curl/curl.h>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include <time.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>

namespace multisite_obs {

namespace {

// A monitor must not hold anything hostage: short connect, bounded total.
constexpr long kConnectTimeoutMs = 5000;
constexpr long kRequestTimeoutMs = 15000;
// The only thing read from a response is interval_s, so a body bigger than
// this is a server misbehaving and is cut off rather than buffered.
constexpr size_t kMaxBodyBytes = 65536;

// Machine uptime, in seconds — what the envelope's uptime_s is. CLOCK_MONOTONIC
// starts at boot on both POSIX targets here; GetTickCount64 is milliseconds
// since boot. Steady-clock epoch is unspecified, so it is not used.
double system_uptime_s() {
#ifdef _WIN32
    return static_cast<double>(GetTickCount64()) / 1000.0;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return static_cast<double>(ts.tv_sec) +
           static_cast<double>(ts.tv_nsec) / 1e9;
#endif
}

// Wall-clock now, ISO-8601 UTC. A wall time — never mixed with the media
// times inside status (CONTEXT.md); it only stamps when this was sent.
std::string sent_at_iso() {
    const std::time_t t = std::time(nullptr);
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    char buf[32];
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmv) == 0)
        return std::string();
    return buf;
}

size_t append_capped(char* data, size_t size, size_t nmemb, void* user) {
    auto* body = static_cast<std::string*>(user);
    const size_t n = size * nmemb;
    const size_t room = n > kMaxBodyBytes ? 0
        : kMaxBodyBytes > body->size() ? kMaxBodyBytes - body->size() : 0;
    body->append(data, room < n ? room : n);
    return n;
}

size_t capture_retry_after(char* data, size_t size, size_t nmemb, void* user) {
    const size_t n = size * nmemb;
    // Header lines arrive one call each. Only the delay-seconds form is read;
    // an HTTP-date form yields 0 and the 429 is then handled by doubling,
    // which is the honest "told to slow down" without parsing dates.
    static const char kPrefix[] = "retry-after:";
    if (n > sizeof(kPrefix) - 1) {
        bool match = true;
        for (size_t i = 0; i < sizeof(kPrefix) - 1; ++i) {
            const char a = data[i] >= 'A' && data[i] <= 'Z'
                ? static_cast<char>(data[i] + ('a' - 'A')) : data[i];
            if (a != kPrefix[i]) { match = false; break; }
        }
        if (match)
            *static_cast<int*>(user) = atoi(data + sizeof(kPrefix) - 1);
    }
    return n;
}

struct PostResult {
    bool        reached = false;  // a response arrived (any status)
    long        code = 0;
    std::string body;
    int         retry_after_s = 0;
};

PostResult post_json(const std::string& url, const std::string& token,
                     const std::string& payload) {
    PostResult r;
    CURL* curl = curl_easy_init();
    if (!curl) return r;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    // Pairing's first request has no token yet — the token is what it comes
    // back with. An empty Bearer header would be worse than none: some
    // servers read its presence as a (bad) credential.
    const std::string auth = "Authorization: Bearer " + token;
    if (!token.empty())
        headers = curl_slist_append(headers, auth.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)payload.size());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_capped);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, capture_retry_after);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &r.retry_after_s);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, kConnectTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, kRequestTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    const CURLcode cc = curl_easy_perform(curl);
    if (cc == CURLE_OK &&
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r.code) == CURLE_OK)
        r.reached = true;
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return r;
}

std::string join_url(const std::string& base) {
    std::string u = base;
    while (!u.empty() && u.back() == '/') u.pop_back();
    return u + "/v1/heartbeat";
}

std::string join_path(const std::string& base, const char* path) {
    std::string u = base;
    while (!u.empty() && u.back() == '/') u.pop_back();
    return u + path;
}

long long wall_now_s() { return (long long)std::time(nullptr); }

std::string host_name() {
#ifdef _WIN32
    char buf[256];
    DWORD n = sizeof(buf);
    if (GetComputerNameA(buf, &n)) return buf;
    return std::string();
#else
    char buf[256];
    buf[sizeof(buf) - 1] = 0;
    if (::gethostname(buf, sizeof(buf) - 1) == 0) return buf;
    return std::string();
#endif
}

long long proc_id() {
#ifdef _WIN32
    return (long long)_getpid();
#else
    return (long long)::getpid();
#endif
}

// The brief's status words for rejection codes, so a misconfigured reporter
// says which half is wrong rather than logging a number.
const char* rejection_word(long code) {
    switch (code) {
        case 400: return "rejected: 400 — malformed body";
        case 401: return "rejected: 401 — token unknown";
        case 403: return "rejected: 403 — token does not match the appliance id";
        case 413: return "rejected: 413 — body too large";
        case 429: return "rate limited — backing off";
        default:  return nullptr;
    }
}

struct Slot {
    std::string kind;   // "obs-encoder" — the wire kind
    std::string role;   // "encoder" — the core filter's role
    std::chrono::steady_clock::time_point next_due =
        std::chrono::steady_clock::now();
    int  central_s = 0;
    bool limited = false;
    int  retry_after_s = 0;
    std::string last_result;
    // Pairing (device-code flow). The worker owns the state machine; the
    // docks only show the code and cancel. Everything here is guarded by
    // g_mtx — the worker never holds it across the network. Transitions are
    // logged as they happen: pairing is interactive and silent state changes
    // are undebuggable from a log file after the fact (standards §8).
    multisite::Pairing pairing;
    int pair_phase_logged = 0;
    bool pair_begin_pending = false;
    // Claimed credentials, copied out of the pairing the moment it resolves.
    // The dock acknowledges Done (cancelling the pairing) on its own refresh
    // cadence, which routinely wins the race against the next worker tick —
    // so persistence keys off THESE, not off the pairing still being Done.
    // A save that only ran while the phase was Done could be orphaned by an
    // acknowledgment landing first, which is exactly how a claim once
    // vanished: approved, displayed, and never written.
    bool claim_saved = true;
    std::string claimed_id, claimed_token, claimed_url;
    std::string pair_note;
    std::string pair_note_logged;
};

std::mutex g_mtx;
Slot g_encoder{"obs-encoder", "encoder"};
Slot g_decoder{"obs-decoder", "decoder"};

// Pairing phase transitions, logged once each (defined below, called from
// serve_pairing with g_mtx held).
void log_pair_phase(Slot& s);
std::atomic<bool> g_running{false};
std::thread g_thread;
std::once_flag g_started;

bool role_wanted(const std::string& kind) {
    const Role r = plugin_role();
    if (kind == "obs-encoder") return r != Role::DecoderOnly;
    return r != Role::EncoderOnly;
}

// The role's pairing knobs as one snapshot, so begin/poll/claim all read the
// same shape whichever role this slot is.
struct RoleCfg {
    bool enabled = false;
    std::string url, id, token, device;
};

RoleCfg read_role_cfg(const Slot& s) {
    RoleCfg c;
    if (s.role == "encoder") {
        const BroadcastSettings cfg =
            BroadcastController::instance().settings_copy();
        c.enabled = cfg.reporter_enabled;
        c.url = cfg.reporter_url; c.id = cfg.reporter_appliance_id;
        c.token = cfg.reporter_token; c.device = cfg.reporter_device_id;
    } else {
        const DecoderSettings cfg = decoder_settings_copy();
        c.enabled = cfg.reporter_enabled;
        c.url = cfg.reporter_url; c.id = cfg.reporter_appliance_id;
        c.token = cfg.reporter_token; c.device = cfg.reporter_device_id;
    }
    return c;
}

// Persist pairing fields. Marshalled to OBS's UI thread: the settings file
// write goes through the same path every Apply uses, which is the one
// combination observed to reach disk — a worker-thread direct write updated
// memory while the file stayed empty, with no error anywhere (the save's
// bool is ignored inside set_decoder_settings). Until that asymmetry is
// understood, persistence stays on the thread whose saves measurably land.
bool save_role_cfg(const Slot& s, const RoleCfg& c) {
    if (s.role == "encoder") {
        BroadcastSettings cfg =
            BroadcastController::instance().settings_copy();
        cfg.reporter_enabled = c.enabled;
        cfg.reporter_url = c.url; cfg.reporter_appliance_id = c.id;
        cfg.reporter_token = c.token; cfg.reporter_device_id = c.device;
        return run_on_ui_thread([cfg] {
            BroadcastController::instance().set_settings(cfg);
        });
    }
    DecoderSettings cfg = decoder_settings_copy();
    cfg.reporter_enabled = c.enabled;
    cfg.reporter_url = c.url; cfg.reporter_appliance_id = c.id;
    cfg.reporter_token = c.token; cfg.reporter_device_id = c.device;
    return run_on_ui_thread([cfg] { set_decoder_settings(cfg); });
}

// Pairing traffic for one role. Interactive — an operator is watching the
// code — so a begin POSTs at once rather than waiting for a beat, while polls
// keep the cadence the collector named. Runs ahead of the heartbeat serve;
// the two never share state beyond the settings they both read.
void serve_pairing(Slot& s) {
    const long long now = wall_now_s();
    enum class Action { None, Start, Poll, SaveClaim };
    Action act = Action::None;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (s.pair_begin_pending) {
            act = Action::Start;
        } else {
            s.pairing.tick(now);
            if (s.pairing.poll_due(now)) {
                act = Action::Poll;
            } else if (!s.claim_saved && !s.claimed_id.empty()) {
                act = Action::SaveClaim;
            }
        }
    }
    if (act == Action::None) return;

    RoleCfg cfg = read_role_cfg(s);
    if (cfg.url.empty()) {
        // The dock guards this too; the URL was cleared mid-flight.
        std::lock_guard<std::mutex> lk(g_mtx);
        s.pair_begin_pending = false;
        s.pairing.cancel();
        s.pair_note = "enter the collector URL first";
        return;
    }

    if (act == Action::Start) {
        // A device id is minted once and kept (PROJECT-SCOPE §8.5). The save
        // is best-effort: this attempt carries the minted id either way, and
        // a restart simply mints again for the next attempt.
        std::string dev = cfg.device;
        if (dev.empty()) {
            const auto ns = std::chrono::steady_clock::now()
                                .time_since_epoch().count();
            dev = multisite::heartbeat_mint_device_id(host_name(), s.kind,
                                                      (long long)ns, proc_id());
            RoleCfg with_dev = cfg;
            with_dev.device = dev;
            save_role_cfg(s, with_dev);
        }
        mlog_info("heartbeat %s: pairing started against %s",
                  s.kind.c_str(), cfg.url.c_str());
        const PostResult r =
            post_json(join_path(cfg.url, "/v1/pair/start"), std::string(),
                      multisite::heartbeat_pair_start_body(dev, s.kind,
                                                           host_name()));
        std::lock_guard<std::mutex> lk(g_mtx);
        s.pair_begin_pending = false;
        if (!r.reached) {
            s.pairing.cancel();
            s.pair_note = "collector not reached";
            log_pair_phase(s);
            return;
        }
        s.pair_note.clear();
        s.pairing.on_start_reply(r.body, (int)r.code, now);
        log_pair_phase(s);
        return;
    }

    if (act == Action::Poll) {
        std::string token;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            token = s.pairing.poll_token();
        }
        const PostResult r =
            post_json(join_path(cfg.url, "/v1/pair/poll"), std::string(),
                      multisite::heartbeat_pair_poll_body(token));
        std::lock_guard<std::mutex> lk(g_mtx);
        if (!r.reached) {
            s.pair_note = "collector not reached";
            log_pair_phase(s);
            return;
        }
        s.pair_note.clear();
        s.pairing.on_poll_reply(r.body, (int)r.code, now);
        if (s.pairing.phase() == multisite::Pairing::Phase::Done) {
            mlog_info("heartbeat %s: pairing approved, saving the claim",
                      s.kind.c_str());
            // Kept beside the pairing, not in it: the dock acknowledges Done
            // (cancelling) on its own cadence, and the save must not depend
            // on the phase surviving until the next tick.
            s.claimed_id = s.pairing.appliance_id();
            s.claimed_token = s.pairing.appliance_token();
            s.claimed_url = s.pairing.collector_url();
        }
        log_pair_phase(s);
        return;
    }

    // SaveClaim: persist the captured claim. Retried every tick until it
    // lands, independent of the pairing's phase — the acknowledgment may
    // already have cancelled it, and that must not orphan the credentials.
    RoleCfg claimed = cfg;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        claimed.id = s.claimed_id;
        claimed.token = s.claimed_token;
        if (!s.claimed_url.empty())
            claimed.url = s.claimed_url;
    }
    if (save_role_cfg(s, claimed)) {
        // Verify, don't trust: read back what the settings actually hold now.
        // Memory and disk have silently disagreed before, and the next Apply
        // writes whatever the widgets show — so say which one won, in the log.
        const RoleCfg check = read_role_cfg(s);
        mlog_info("heartbeat %s: claim save attempted, settings now hold "
                  "id='%s'",
                  s.kind.c_str(), check.id.c_str());
        std::lock_guard<std::mutex> lk(g_mtx);
        s.claim_saved = true;
        log_pair_phase(s);
    } else {
        mlog_warn("heartbeat %s: pairing approved but the claim would not "
                  "save — retrying", s.kind.c_str());
    }
}

// Pairing phase transitions, logged once each: without these the only record
// of a pairing is the dock label, which is gone by the time anyone reads the
// log. Called with g_mtx held, after every mutation above.
void log_pair_phase(Slot& s) {
    int phase = 0;
    switch (s.pairing.phase()) {
        case multisite::Pairing::Phase::Waiting: phase = 1; break;
        case multisite::Pairing::Phase::Done:    phase = 2; break;
        case multisite::Pairing::Phase::Expired: phase = 3; break;
        case multisite::Pairing::Phase::Failed:  phase = 4; break;
        default: break;
    }
    if (phase != s.pair_phase_logged) {
        s.pair_phase_logged = phase;
        if (phase == 1)
            mlog_info("heartbeat %s: code %s — waiting for approval",
                      s.kind.c_str(), s.pairing.user_code().c_str());
        else if (phase == 2)
            mlog_info("heartbeat %s: claimed %s", s.kind.c_str(),
                      s.pairing.appliance_id().c_str());
        else if (phase == 3)
            mlog_info("heartbeat %s: pairing code expired", s.kind.c_str());
        else if (phase == 4)
            mlog_warn("heartbeat %s: pairing failed: %s", s.kind.c_str(),
                      !s.pair_note.empty() ? s.pair_note.c_str()
                                           : s.pairing.error().c_str());
    }
    if (s.pair_note != s.pair_note_logged) {
        s.pair_note_logged = s.pair_note;
        if (!s.pair_note.empty())
            mlog_warn("heartbeat %s: pairing note: %s", s.kind.c_str(),
                      s.pair_note.c_str());
    }
}

// One role, one tick. All inputs are snapshots — settings, status, role — so
// Apply and the role selector take effect without a restart and nothing here
// holds a lock across the network.
void serve(Slot& s) {    const auto now = std::chrono::steady_clock::now();
    if (now < s.next_due) return;

    std::string url, id, token;
    bool enabled = false, active = false;
    std::string status;
    if (s.role == "encoder") {
        const BroadcastSettings cfg =
            BroadcastController::instance().settings_copy();
        enabled = cfg.reporter_enabled;
        url = cfg.reporter_url; id = cfg.reporter_appliance_id;
        token = cfg.reporter_token;
        active = BroadcastController::instance().is_live();
        if (enabled && !url.empty() && !id.empty() && !token.empty())
            status = encoder_status_json();
    } else {
        const DecoderSettings cfg = decoder_settings_copy();
        enabled = cfg.reporter_enabled;
        url = cfg.reporter_url; id = cfg.reporter_appliance_id;
        token = cfg.reporter_token;
        DecoderSnapshot snap;
        const bool have = decoder_snapshot(snap);
        active = have && (snap.playing || snap.paused || snap.buffering);
        if (enabled && !url.empty() && !id.empty() && !token.empty())
            status = decoder_status_json();
    }

    const bool configured = !url.empty() && !id.empty() && !token.empty();
    std::string outcome;
    int wait_s = multisite::kHeartbeatIdleIntervalS;
    if (!role_wanted(s.kind) || !enabled) {
        outcome = "disabled";
        wait_s = 5;   // re-read settings promptly; no sockets either way
    } else if (!configured) {
        outcome = "not configured";
        wait_s = 5;
    } else {
        multisite::HeartbeatIdentity ident{id, s.kind, PLUGIN_VERSION,
                                           system_uptime_s()};
        const std::string body = multisite::heartbeat_build(
            ident, nullptr,
            multisite::heartbeat_filter_status(s.role, status), sent_at_iso());
        const PostResult r = post_json(join_url(url), token, body);
        if (!r.reached) {
            // Fire-and-forget (brief): dropped, never queued, base cadence
            // kept. NOT rate-limited — a dead link is not the collector
            // asking us to slow down.
            outcome = "not reached (dropped)";
            s.limited = false;
            s.retry_after_s = 0;
            wait_s = multisite::heartbeat_next_interval_s(active, s.central_s,
                                                          false, 0);
        } else if (r.code == 200) {
            outcome = "accepted (200)";
            s.central_s = multisite::heartbeat_parse_server_interval(r.body);
            s.limited = false;
            s.retry_after_s = 0;
            wait_s = multisite::heartbeat_next_interval_s(active, s.central_s,
                                                          false, 0);
        } else if (r.code == 429) {
            outcome = "rate limited — backing off";
            s.central_s = multisite::heartbeat_parse_server_interval(r.body);
            s.limited = true;
            s.retry_after_s = r.retry_after_s;
            wait_s = multisite::heartbeat_next_interval_s(active, s.central_s,
                                                          true, s.retry_after_s);
        } else if (const char* w = rejection_word(r.code)) {
            outcome = w;
            wait_s = multisite::heartbeat_next_interval_s(active, s.central_s,
                                                          false, 0);
        } else {
            outcome = "http " + std::to_string(r.code) + " (dropped)";
            wait_s = multisite::heartbeat_next_interval_s(active, s.central_s,
                                                          false, 0);
        }
    }

    s.next_due = now + std::chrono::seconds(wait_s);
    std::lock_guard<std::mutex> lk(g_mtx);
    if (outcome != s.last_result) {
        s.last_result = outcome;
        // Said once per change (standards §8), not once per tick: a dead
        // collector must not fill the log at heartbeat cadence.
        mlog_info("heartbeat %s: %s", s.kind.c_str(), outcome.c_str());
    }
}

void loop() {
    static std::once_flag curl_once;
    std::call_once(curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    while (g_running.load()) {
        serve_pairing(g_encoder);
        serve_pairing(g_decoder);
        serve(g_encoder);
        serve(g_decoder);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

} // namespace

void reporter_start() {
    std::call_once(g_started, [] {
        g_running = true;
        g_thread = std::thread(loop);
    });
}

void reporter_stop() {
    g_running = false;
    if (g_thread.joinable()) g_thread.join();
}

std::string reporter_last_result(const std::string& kind) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (kind == "obs-encoder") return g_encoder.last_result;
    if (kind == "obs-decoder") return g_decoder.last_result;
    return std::string();
}

static Slot& slot_for(const std::string& kind) {
    return kind == "obs-encoder" ? g_encoder : g_decoder;
}

bool reporter_pair_begin(const std::string& kind) {
    // The typed URL is what gets used — the dock Applies first, so this is
    // the committed value rather than a field still being edited.
    RoleCfg cfg = read_role_cfg(slot_for(kind));
    if (cfg.url.empty()) return false;
    std::lock_guard<std::mutex> lk(g_mtx);
    Slot& s = slot_for(kind);
    s.pairing.cancel();
    s.pair_begin_pending = true;
    s.claim_saved = false;
    s.claimed_id.clear();
    s.claimed_token.clear();
    s.claimed_url.clear();
    s.pair_note.clear();
    // Reset the change-detection guards too. Without this, EVERY attempt after
    // the first is silent: log_pair_phase only prints when the note or phase
    // DIFFERS from last time, so a second attempt that fails the same way as
    // the first produced no line at all — which is how "pairing started" was
    // seen twice with nothing after it, and the actual reason (unreachable,
    // 404, no code in the body) was unknowable from the log. Found 2026-09-21
    // on a real encoder whose pairing looked like it did nothing.
    s.pair_note_logged.clear();
    s.pair_phase_logged = 0;
    return true;
}

PairView reporter_pair_view(const std::string& kind) {
    std::lock_guard<std::mutex> lk(g_mtx);
    const Slot& s = slot_for(kind);
    PairView v;
    switch (s.pairing.phase()) {
        case multisite::Pairing::Phase::Waiting: v.phase = 1; break;
        case multisite::Pairing::Phase::Done:    v.phase = 2; break;
        case multisite::Pairing::Phase::Expired: v.phase = 3; break;
        case multisite::Pairing::Phase::Failed:  v.phase = 4; break;
        default:                                 v.phase = 0; break;
    }
    v.user_code = s.pairing.user_code();
    v.verification_url = s.pairing.verification_url();
    v.error = s.pairing.error();
    v.note = s.pair_note;
    v.saved = s.claim_saved;
    return v;
}

void reporter_pair_cancel(const std::string& kind) {
    std::lock_guard<std::mutex> lk(g_mtx);
    Slot& s = slot_for(kind);
    s.pairing.cancel();
    s.pair_begin_pending = false;
    s.pair_note.clear();
}

} // namespace multisite_obs
