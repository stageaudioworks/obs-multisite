// SPDX-License-Identifier: GPL-3.0-or-later
#include "reporter.h"
#include "../core/cloud_storage.h"   // CloudRole

#include "broadcast_controller.h"
#include "decoder_settings.h"
#include "multisite_ui.h"
#include "plugin_log.h"
#include "plugin_role.h"
#include "web/commands.h"
#include "web/ui_thread.h"
#include "../core/heartbeat_reporter.h"
#include "../core/collector_client.h"


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

// The HTTP call and the collector's paths live in core/collector_client. This
// file had a private copy of both — the same timeouts, the same 64 KB cap, the
// same empty-token rule, line for line — which is the "one quantity in two
// places" this project keeps paying for (#11). It is the host's thread that
// calls them; what they do is written once.

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

// ONE CLOUD IDENTITY PER ROLE (Phase 12), owned by this worker and read by that
// role's storage — see reporter.h.
//
// It used to be one identity for the whole plugin, fed from the encoder's
// pairing and falling back to the decoder's, on the assumption that "the
// machine is one appliance to the collector whatever it is doing locally".
// The pairing flow does not work that way: each role pairs on its own and is
// its own appliance. A Mac running both had encoder apl_5d62… and decoder
// apl_2a7b… — so the decoder heartbeated as one appliance and read storage
// with credentials fetched under the other, with the encoder's READ-WRITE
// role. That is the exact split this subsystem exists to make impossible, and
// it handed a decoder write access it has no use for.
//
// Now each role's storage and its heartbeat come from that role's pairing and
// nothing else. No fallback between them: a role that is not paired is not
// paired, and says so.
struct RoleIdentity {
    // Made before the loop starts and never replaced, so a reader's
    // shared_ptr stays valid.
    std::shared_ptr<multisite::CloudIdentity> id;
    // Which pairing it currently holds, so adoption is idempotent without
    // re-enrolling (which would throw away fetched credentials).
    std::string src;
    // The bucket the last fetch named. The decoder's sources are asked to
    // rebuild when this CHANGES, not on every refresh: the session token
    // rotates each time, and rebuilding for that would interrupt a campus once
    // per TTL.
    std::string last_bucket;
};
RoleIdentity g_id_encoder;
RoleIdentity g_id_decoder;

RoleIdentity& role_identity(multisite::CloudRole r) {
    return r == multisite::CloudRole::Encoder ? g_id_encoder : g_id_decoder;
}
const char* role_word(multisite::CloudRole r) {
    return r == multisite::CloudRole::Encoder ? "encoder" : "decoder";
}

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
        const multisite::HttpResult r =
            multisite::http_post_json(
                multisite::collector_url(cfg.url, multisite::kPairStartPath),
                std::string(),
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
        const multisite::HttpResult r =
            multisite::http_post_json(
                multisite::collector_url(cfg.url, multisite::kPairPollPath),
                std::string(),
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

    // WHO this heartbeat is comes from the role's identity — the same object
    // that role's storage reads through — so the two cannot name different
    // appliances (#11). It used to read its own copy of the pairing from the
    // settings, while storage read the identity: two answers to one question.
    // Whether to heartbeat at all stays a setting: monitoring on/off is a
    // separate choice from pairing, and a paired box may run with it off.
    const multisite::CloudRole role = s.role == "encoder"
        ? multisite::CloudRole::Encoder : multisite::CloudRole::Decoder;
    std::string url, id, token;
    if (auto ident = role_identity(role).id; ident && ident->paired()) {
        // Same thread as the identity's writer (this worker), so the
        // references are read with nothing racing them.
        url = ident->collector_url();
        id = ident->appliance_id();
        token = ident->appliance_token();
    }
    const bool configured = !url.empty() && !id.empty() && !token.empty();

    bool enabled = false, active = false;
    std::string status;
    if (s.role == "encoder") {
        enabled = BroadcastController::instance().settings_copy().reporter_enabled;
        active = BroadcastController::instance().is_live();
        if (enabled && configured) status = encoder_status_json();
    } else {
        enabled = decoder_settings_copy().reporter_enabled;
        DecoderSnapshot snap;
        const bool have = decoder_snapshot(snap);
        active = have && (snap.playing || snap.paused || snap.buffering);
        if (enabled && configured) status = decoder_status_json();
    }

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
        const multisite::HttpResult r = multisite::http_post_json(
            multisite::collector_url(url, multisite::kHeartbeatPath), token, body);
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

// One credential step (Phase 12), on this worker's thread. Fetches only when
// the identity says one is due, so a steady state costs nothing. The fetch runs
// with nothing held — the identity's own state is written after it returns.
void serve_cloud_credentials(multisite::CloudRole role) {
    RoleIdentity& ri = role_identity(role);
    auto id = ri.id;
    if (!id) return;
    const char* who = role_word(role);

    const long long now_ms = (long long)std::chrono::duration_cast<
        std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

    if (id->tick(now_ms) != multisite::CloudAction::Fetch) return;

    const std::string url = multisite::collector_url(
        id->collector_url(), multisite::kCredentialsPath);
    const multisite::HttpResult r =
        multisite::http_get_json(url, id->appliance_token());
    if (!r.reached) {
        // Unreachable: an empty reply keeps last-good and schedules a retry.
        id->on_credentials(multisite::CredentialsReply{}, now_ms);
        mlog_info("cloud credentials (%s): collector not reached — keeping the "
                  "last known set", who);
        return;
    }

    const multisite::CredentialsReply reply =
        multisite::cloud_parse_credentials(r.body, (int)r.code);
    if (reply.ok) {
        // The role and the appliance, because "which pairing did this come
        // from?" is the question the single identity could not answer.
        mlog_info("cloud credentials (%s, %s): fetched %s (role %s, expires in "
                  "%lld s)", who, id->appliance_id().c_str(),
                  reply.creds.bucket.c_str(),
                  reply.creds.read_write ? "read-write" : "read-only",
                  (reply.creds.expires_at_ms - now_ms) / 1000);
    } else if (reply.unpaired) {
        mlog_warn("cloud credentials (%s): the collector says this device is "
                  "UNPAIRED — stopping fetches. A running event continues on "
                  "the last known credentials until they expire.", who);
    } else {
        mlog_warn("cloud credentials (%s): HTTP %ld, keeping the last known set",
                  who, r.code);
    }
    id->on_credentials(reply, now_ms);

    // A decoder source builds its transports at load, so one that loaded before
    // this fetch saw no credentials and refused. Ask every live source to
    // re-apply its settings, which rebuilds them now the identity is populated.
    // This is the OBS twin of the appliance's m_transport_wanted: a session
    // reads through the credentials it was built with, so either the session is
    // rebuilt or the credentials are not there.
    //
    // Only for a fetch that produced the bucket for the FIRST time, or moved it
    // to a different one. The session token rotates on every refresh, so keying
    // on it would tear every decoder down once per TTL — the same needless
    // rebuild the appliance had, and the reason its gate keys on the provider
    // rather than on the credentials. A refreshed token signs just as well
    // through a transport that already exists.
    //
    // The DECODER's identity only: the encoder output reads its identity when
    // Go Live builds the session, so there is nothing running to wake.
    if (role == multisite::CloudRole::Decoder && reply.ok &&
        reply.creds.bucket != ri.last_bucket) {
        const bool first = ri.last_bucket.empty();
        ri.last_bucket = reply.creds.bucket;
        mlog_info("cloud credentials (decoder): bucket %s '%s' — asking live "
                  "decoder sources to read through it",
                  first ? "is" : "moved to",
                  reply.creds.bucket.c_str());
        decoder_reconfigure_all();
    }
}

void loop() {
    multisite::collector_http_init();
    g_id_encoder.id = std::make_shared<multisite::CloudIdentity>();
    g_id_decoder.id = std::make_shared<multisite::CloudIdentity>();
    while (g_running.load()) {
        serve_pairing(g_encoder);
        serve_pairing(g_decoder);
        reporter_adopt_saved_pairing();
        serve_cloud_credentials(multisite::CloudRole::Encoder);
        serve_cloud_credentials(multisite::CloudRole::Decoder);
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

std::shared_ptr<multisite::CloudIdentity>
reporter_cloud_identity(multisite::CloudRole role) {
    return role_identity(role).id;
}

namespace {
// One role's saved pairing into that role's identity — never another role's.
void adopt_role(multisite::CloudRole role, const Slot& slot) {
    RoleIdentity& ri = role_identity(role);
    auto id = ri.id;
    if (!id) return;
    const RoleCfg cfg = read_role_cfg(slot);
    if (cfg.url.empty() || cfg.id.empty() || cfg.token.empty()) {
        // The pairing was CLEARED: the identity follows it. It used to keep the
        // old enrolment for the life of the process, so storage went on reading
        // under a pairing the operator had removed — and now that the heartbeat
        // reads the identity too, it would have gone on reporting under it.
        if (!ri.src.empty() || id->paired()) {
            mlog_info("cloud identity (%s): pairing cleared — no longer "
                      "reading or reporting as %s", role_word(role),
                      id->appliance_id().c_str());
            id->reset();
            ri.src.clear();
            ri.last_bucket.clear();
        }
        return;
    }

    // Idempotent, and NEVER re-enrolling once a claim is held: a fetch already
    // collected this run is newer than the settings, which may not have been
    // written back yet. Re-enrolling would throw those credentials away.
    const std::string src = cfg.url + "\n" + cfg.id + "\n" + cfg.token;
    if (ri.src == src) return;
    if (id->paired() && id->collector_url() == cfg.url &&
        id->appliance_id() == cfg.id && id->appliance_token() == cfg.token) {
        ri.src = src;
        return;
    }
    ri.src = src;
    id->set_enrolment(cfg.url, cfg.id, cfg.token);
}
} // namespace

void reporter_adopt_saved_pairing() {
    adopt_role(multisite::CloudRole::Encoder, g_encoder);
    adopt_role(multisite::CloudRole::Decoder, g_decoder);
}

} // namespace multisite_obs
