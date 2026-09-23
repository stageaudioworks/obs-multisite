// SPDX-License-Identifier: GPL-3.0-or-later
#include "reporter.h"
#include "../core/collector_client.h"

#include "api.h"
#include "log.h"
#include "player.h"
#include "sysinfo.h"
#include "../core/heartbeat_reporter.h"
#include "../vendor/nlohmann/json.hpp"


#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

using json = nlohmann::json;

namespace multisite_player {

namespace {

// The HTTP call and the collector's paths live in core/collector_client. This
// file carried a copy of both — its own comment said "same bounds as the OBS
// host", which is the duplication named out loud — and so did the plugin (#11).
// Both hosts now call the one copy.

long long wall_now_s() { return (long long)std::time(nullptr); }

long long now_ns() {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
}

long long proc_id() { return (long long)::getpid(); }

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

// Wall-clock now, ISO-8601 UTC — a stamp for when this was sent, never mixed
// with the media times inside status (CONTEXT.md).
std::string sent_at_iso() {
    const std::time_t t = std::time(nullptr);
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    gmtime_r(&t, &tmv);
    char buf[32];
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmv) == 0)
        return std::string();
    return buf;
}

// The status document the page already polls, plus the three fields that do
// most of the diagnostic work (brief) and that the page's own document does
// not carry: the serving edge, the endpoint in use, and the observed download
// rate. storage_health(false) issues no request — passively observed figures
// only, the same ones a status poll is told to use.
std::string enriched_status(Player& player) {
    json j;
    try {
        j = json::parse(player_status_json(player));
        if (!j.is_object()) j = json::object();
    } catch (...) {
        j = json::object();
    }
    // Decoder-shaped payload: the player filters through the decoder list
    // (see heartbeat_filter_status), so it declares that shape.
    j["role"] = "decoder";
    const Player::StorageHealth sh = player.storage_health(false);
    if (!sh.colo.empty()) j["colo"] = sh.colo;
    if (!sh.endpoint.empty()) j["storage_host"] = sh.endpoint;
    j["download_bytes_per_s"] = sh.bytes_per_s;
    j["download_samples"] = sh.rate_samples;
    return j.dump();
}

multisite::HeartbeatHost host_block(const Config& cfg) {
    multisite::HeartbeatHost h;
    const SystemInfo sys = system_info();
    h.cpu_pct = sys.cpu_percent;   // -1 until two readings exist: omit, never 0
    if (sys.mem_total_bytes > 0)
        h.mem_pct = 100.0 * (double)(sys.mem_total_bytes - sys.mem_available_bytes) /
                    (double)sys.mem_total_bytes;
    // A failed statvfs leaves total at 0: unknown, not full. Sending 0 free
    // would claim a full disk, so it is omitted instead.
    const DiskInfo disk = disk_info(cfg.cache_dir);
    if (disk.total_bytes > 0) h.disk_free_bytes = disk.free_bytes;
    // 0 means the box cannot report one (sysinfo.h) — a Pi is never at 0°C.
    if (sys.cpu_temp_c > 0) { h.temp_c = sys.cpu_temp_c; h.has_temp = true; }
    h.throttled = sys.throttled || sys.under_voltage;
    return h;
}

} // namespace

// The worker's state lives here rather than in reporter.h so that header stays
// free of <thread> and <mutex>: anything including player.h would otherwise
// inherit them.
struct Reporter::Worker {
    std::thread thread;
    std::atomic<bool> running{false};
    std::mutex life_mtx;   // start/stop ordering; the loop never takes it
    std::mutex mtx;        // everything below
    std::chrono::steady_clock::time_point next_due =
        std::chrono::steady_clock::now();
    int central_s = 0;
    bool limited = false;
    int retry_after_s = 0;
    std::string last_result;
    Player* player = nullptr;
    // Pairing (device-code flow). The worker asks and polls; the page shows
    // the code from pair_view() and stops it with pair_cancel(). Guarded by
    // mtx like the rest — the worker never holds it across the network.
    // Claimed credentials are captured beside the pairing, not in it: the
    // page acknowledges Done on its own cadence, and persistence must not
    // depend on the phase surviving until the next tick (the Apply-wipe that
    // cost an evening on the OBS side).
    multisite::Pairing pairing;
    bool begin_pending = false;
    bool claim_saved = true;
    std::string claimed_id, claimed_token, claimed_url, claimed_update_token;
    std::string note;
};

namespace {

// Write a secret to a file only root can read, whole or not at all: written
// beside it and renamed over, so a reader never sees half a token.
bool write_private_file(const std::string& path, const std::string& content,
                        std::string& err) {
    const std::string tmp = path + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { err = std::strerror(errno); return false; }
    const bool wrote = ::write(fd, content.data(), content.size()) ==
                       (ssize_t)content.size();
    const bool synced = ::fsync(fd) == 0;
    ::close(fd);
    if (!wrote || !synced || ::rename(tmp.c_str(), path.c_str()) != 0) {
        err = std::strerror(errno);
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

} // namespace

Reporter::Reporter() : m_worker(new Worker()) {}
Reporter::~Reporter() { stop(); }

void Reporter::serve_once() {
    Player& player = *m_worker->player;
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lk(m_worker->mtx);
        if (now < m_worker->next_due) return;
    }

    const Config cfg = player.config();
    Status s;
    player.status(s);
    const bool active = s.playing || s.paused || s.buffering;

    // WHO this heartbeat is comes from the player's identity — the object its
    // storage reads through — as one snapshot, so the two cannot name
    // different appliances and the three strings cannot come from different
    // pairings (#11). Whether to heartbeat at all stays a setting.
    multisite::Enrolment en;
    if (auto ident = player.cloud_identity()) en = ident->enrolment();
    const bool configured = en.paired();
    std::string outcome;
    int wait_s = multisite::kHeartbeatIdleIntervalS;
    if (!cfg.reporter_enabled) {
        outcome = "disabled";
        wait_s = 5;   // re-read settings promptly; no sockets either way
    } else if (!configured) {
        outcome = "not configured";
        wait_s = 5;
    } else {
        const SystemInfo sys = system_info();
        multisite::HeartbeatIdentity ident{
            en.id, multisite::heartbeat_player_kind(cfg.reporter_kind),
            player_version(), sys.uptime_s};
        const multisite::HeartbeatHost host = host_block(cfg);
        const std::string body = multisite::heartbeat_build(
            ident, &host,
            multisite::heartbeat_filter_status("decoder",
                                               enriched_status(player)),
            sent_at_iso());
        const multisite::HttpResult r = multisite::http_post_json(
            multisite::collector_url(en.url, multisite::kHeartbeatPath),
            en.token, body);
        const int central = m_worker->central_s;
        if (!r.reached) {
            // Fire-and-forget (brief): dropped, never queued. NOT
            // rate-limited — a dead link is not the collector slowing us down.
            outcome = "not reached (dropped)";
            wait_s = multisite::heartbeat_next_interval_s(active, central,
                                                          false, 0);
            std::lock_guard<std::mutex> lk(m_worker->mtx);
            m_worker->limited = false;
            m_worker->retry_after_s = 0;
        } else if (r.code == 200) {
            outcome = "accepted (200)";
            const int adopted =
                multisite::heartbeat_parse_server_interval(r.body);
            wait_s = multisite::heartbeat_next_interval_s(active, adopted,
                                                          false, 0);
            std::lock_guard<std::mutex> lk(m_worker->mtx);
            m_worker->central_s = adopted;
            m_worker->limited = false;
            m_worker->retry_after_s = 0;
        } else if (r.code == 429) {
            outcome = "rate limited — backing off";
            const int adopted =
                multisite::heartbeat_parse_server_interval(r.body);
            wait_s = multisite::heartbeat_next_interval_s(active, adopted,
                                                          true,
                                                          r.retry_after_s);
            std::lock_guard<std::mutex> lk(m_worker->mtx);
            m_worker->central_s = adopted;
            m_worker->limited = true;
            m_worker->retry_after_s = r.retry_after_s;
        } else if (const char* w = rejection_word(r.code)) {
            outcome = w;
            wait_s = multisite::heartbeat_next_interval_s(active, central,
                                                          false, 0);
        } else {
            outcome = "http " + std::to_string(r.code) + " (dropped)";
            wait_s = multisite::heartbeat_next_interval_s(active, central,
                                                          false, 0);
        }
    }

    std::lock_guard<std::mutex> lk(m_worker->mtx);
    m_worker->next_due = now + std::chrono::seconds(wait_s);
    if (outcome != m_worker->last_result) {
        m_worker->last_result = outcome;
        // Said once per change (standards §8): a dead collector must not fill
        // the journal at heartbeat cadence.
        plog_info("heartbeat %s: %s",
                  multisite::heartbeat_player_kind(cfg.reporter_kind).c_str(),
                  outcome.c_str());
    }
}

void Reporter::loop() {
    multisite::collector_http_init();
    while (m_worker->running.load()) {
        serve_pairing();
        // The credential lifecycle (Phase 12) rides this thread: storage and
        // monitoring are the same subsystem, and this worker already owns the
        // collector connection, the clock and the network. It fetches only when
        // the identity says one is due, so a steady state costs nothing.
        if (m_worker->player) m_worker->player->serve_cloud_credentials();
        serve_once();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

// Pairing traffic. Interactive — an operator is watching the code — so a
// begin POSTs at once rather than waiting for a beat, while polls keep the
// cadence the collector named. Decided under lock, network outside it.
void Reporter::serve_pairing() {
    const long long now = wall_now_s();
    enum class Action { None, Start, Poll, SaveClaim };
    Action act = Action::None;
    {
        std::lock_guard<std::mutex> lk(m_worker->mtx);
        if (m_worker->begin_pending) {
            act = Action::Start;
        } else {
            m_worker->pairing.tick(now);
            if (m_worker->pairing.poll_due(now)) {
                act = Action::Poll;
            } else if (!m_worker->claim_saved &&
                       !m_worker->claimed_id.empty()) {
                act = Action::SaveClaim;
            }
        }
    }
    if (act == Action::None) return;

    Player& player = *m_worker->player;
    Config cfg = player.config();
    if (cfg.reporter_url.empty()) {
        // Begin guards this too; the URL was cleared mid-flight.
        std::lock_guard<std::mutex> lk(m_worker->mtx);
        m_worker->begin_pending = false;
        m_worker->pairing.cancel();
        m_worker->note = "enter the collector URL first";
        return;
    }

    if (act == Action::Start) {
        // A device id is minted once and kept. The save is best-effort: this
        // attempt carries the minted id either way, and a restart simply
        // mints again for the next attempt.
        std::string dev = cfg.reporter_device_id;
        if (dev.empty()) {
            dev = multisite::heartbeat_mint_device_id(
                hostname(), multisite::heartbeat_player_kind(cfg.reporter_kind),
                now_ns(), proc_id());
            Config with_dev = cfg;
            with_dev.reporter_device_id = dev;
            std::string err;
            if (!player.store_config(with_dev, err))
                plog_warn("pairing: could not save the device id: %s",
                          err.c_str());
        }
        const multisite::HttpResult r = multisite::http_post_json(
            multisite::collector_url(cfg.reporter_url, multisite::kPairStartPath),
            std::string(),
            multisite::heartbeat_pair_start_body(
                dev, multisite::heartbeat_player_kind(cfg.reporter_kind),
                hostname(), board_serial()));
        std::lock_guard<std::mutex> lk(m_worker->mtx);
        m_worker->begin_pending = false;
        if (!r.reached) {
            m_worker->pairing.cancel();
            m_worker->note = "collector not reached";
            return;
        }
        m_worker->note.clear();
        m_worker->pairing.on_start_reply(r.body, (int)r.code, now);
        return;
    }

    if (act == Action::Poll) {
        std::string token;
        {
            std::lock_guard<std::mutex> lk(m_worker->mtx);
            token = m_worker->pairing.poll_token();
        }
        const multisite::HttpResult r = multisite::http_post_json(
            multisite::collector_url(cfg.reporter_url, multisite::kPairPollPath),
            std::string(),
            multisite::heartbeat_pair_poll_body(token));
        std::lock_guard<std::mutex> lk(m_worker->mtx);
        if (!r.reached) {
            m_worker->note = "collector not reached";
            return;
        }
        m_worker->note.clear();
        m_worker->pairing.on_poll_reply(r.body, (int)r.code, now);
        if (m_worker->pairing.phase() == multisite::Pairing::Phase::Done) {
            m_worker->claimed_id = m_worker->pairing.appliance_id();
            m_worker->claimed_token = m_worker->pairing.appliance_token();
            m_worker->claimed_url = m_worker->pairing.collector_url();
            m_worker->claimed_update_token = m_worker->pairing.update_token();
        }
        return;
    }

    // SaveClaim: persist the captured claim. Retried every tick until it
    // lands, independent of the pairing's phase.
    Config claimed = cfg;
    std::string update_token;
    {
        std::lock_guard<std::mutex> lk(m_worker->mtx);
        update_token = m_worker->claimed_update_token;
        if (claimed.reporter_device_id.empty())
            claimed.reporter_device_id = multisite::heartbeat_mint_device_id(
                hostname(), multisite::heartbeat_player_kind(cfg.reporter_kind),
                now_ns(), proc_id());
        claimed.reporter_appliance_id = m_worker->claimed_id;
        claimed.reporter_token = m_worker->claimed_token;
        if (!m_worker->claimed_url.empty())
            claimed.reporter_url = m_worker->claimed_url;
    }
    // The update token, when there is one and somewhere to put it. Never a
    // reason to hold the claim back: a box that paired but could not hand its
    // update token over still plays, and pairing again writes it again.
    if (!update_token.empty() && !cfg.update_token_file.empty()) {
        std::string uerr;
        if (write_private_file(cfg.update_token_file, update_token + "\n", uerr))
            plog_info("pairing: update token handed over to %s",
                      cfg.update_token_file.c_str());
        else
            plog_warn("pairing: could not write the update token to %s: %s",
                      cfg.update_token_file.c_str(), uerr.c_str());
    }
    std::string err;
    if (player.store_config(claimed, err)) {
        std::lock_guard<std::mutex> lk(m_worker->mtx);
        m_worker->claim_saved = true;
        m_worker->claimed_update_token.clear();
    } else {
        plog_warn("pairing: approved, but the claim would not save: %s",
                  err.c_str());
    }
}

void Reporter::start(Player& player) {
    std::lock_guard<std::mutex> lk(m_worker->life_mtx);
    m_worker->player = &player;
    if (m_worker->running.exchange(true)) return;
    if (m_worker->thread.joinable()) m_worker->thread.join();
    m_worker->thread = std::thread([this] { loop(); });
}

void Reporter::stop() {
    std::lock_guard<std::mutex> lk(m_worker->life_mtx);
    m_worker->running = false;
    if (m_worker->thread.joinable()) m_worker->thread.join();
}

std::string Reporter::last_result() const {
    std::lock_guard<std::mutex> lk(m_worker->mtx);
    return m_worker->last_result;
}

bool Reporter::pair_begin() {
    Player* p = nullptr;
    {
        std::lock_guard<std::mutex> lk(m_worker->life_mtx);
        p = m_worker->player;
    }
    if (!p) return false;
    if (p->config().reporter_url.empty()) return false;
    std::lock_guard<std::mutex> lk(m_worker->mtx);
    m_worker->pairing.cancel();
    m_worker->begin_pending = true;
    m_worker->claim_saved = false;
    m_worker->claimed_id.clear();
    m_worker->claimed_token.clear();
    m_worker->claimed_url.clear();
    m_worker->claimed_update_token.clear();
    m_worker->note.clear();
    return true;
}

void Reporter::pair_cancel() {
    std::lock_guard<std::mutex> lk(m_worker->mtx);
    m_worker->pairing.cancel();
    m_worker->begin_pending = false;
    m_worker->note.clear();
}

PairView Reporter::pair_view() const {
    std::lock_guard<std::mutex> lk(m_worker->mtx);
    PairView v;
    switch (m_worker->pairing.phase()) {
        case multisite::Pairing::Phase::Waiting: v.phase = 1; break;
        case multisite::Pairing::Phase::Done:    v.phase = 2; break;
        case multisite::Pairing::Phase::Expired: v.phase = 3; break;
        case multisite::Pairing::Phase::Failed:  v.phase = 4; break;
        default:                                 v.phase = 0; break;
    }
    v.user_code = m_worker->pairing.user_code();
    v.verification_url = m_worker->pairing.verification_url();
    v.error = m_worker->pairing.error();
    v.note = m_worker->note;
    v.saved = m_worker->claim_saved;
    return v;
}

} // namespace multisite_player
