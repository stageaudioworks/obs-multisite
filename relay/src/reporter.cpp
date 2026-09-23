// SPDX-License-Identifier: GPL-3.0-or-later
#include "reporter.h"

#include "log.h"
#include "service.h"

#include "../../src/core/cloud_identity.h"
#include "../../src/core/collector_client.h"
#include "../../src/core/heartbeat_reporter.h"
#include "../../src/vendor/nlohmann/json.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>

#ifndef MULTISITE_RELAY_VERSION
#define MULTISITE_RELAY_VERSION "dev"
#endif

using json = nlohmann::json;

namespace multisite_relay {
namespace {

// The relay's kind on the wire. Fixed, unlike the player's — a relay is only
// ever a relay, so there is no configured word to get wrong.
constexpr const char* kRelayKind = "relay";

long long wall_now_s() { return (long long)std::time(nullptr); }

long long now_ns() {
    return (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

long long now_ms() {
    return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string hostname() {
    char buf[256] = {0};
    if (::gethostname(buf, sizeof(buf) - 1) != 0) return "relay";
    return buf[0] ? std::string(buf) : std::string("relay");
}

std::string sent_at_iso() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

// Words for the codes the collector actually uses to refuse us, so the page
// says what to do rather than showing a number.
const char* rejection_word(long code) {
    switch (code) {
        case 400: return "rejected: 400 — the collector did not accept the body";
        case 401: return "rejected: 401 — token unknown";
        case 403: return "rejected: 403 — this relay is no longer paired";
        case 404: return "rejected: 404 — no collector at that address";
        default:  return nullptr;
    }
}

} // namespace

// ── The worker ──────────────────────────────────────────────────────────────

struct Reporter::Worker {
    std::atomic<bool> running{false};
    std::thread thread;
    Service* service = nullptr;

    multisite::CloudIdentity identity;

    mutable std::mutex mtx;
    std::chrono::steady_clock::time_point next_due{};
    std::string last_result;
    int  central_s = 0;          // the collector's adopted interval, if any
    bool limited = false;
    int  retry_after_s = 0;

    // Pairing, and the claim captured from it. The claim is persisted on a
    // later tick and retried until it lands, so a database that is briefly
    // unwritable cannot lose an approval an operator just gave.
    multisite::Pairing pairing;
    bool begin_pending = false;
    std::string begin_url;
    std::string note;
    std::string claimed_id, claimed_token, claimed_url;
    bool claim_saved = true;     // nothing to save until something is claimed
};

Reporter::Reporter() : m_worker(new Worker()) {
    // ADR-0002: a relay only reads, and is the one box deliberately exposed to
    // the internet. Set before anything can fetch, so there is no window in
    // which a read-write set would be adopted.
    m_worker->identity.set_require_read_only(true);
}

Reporter::~Reporter() { stop(); }

multisite::CloudIdentity& Reporter::cloud_identity() {
    return m_worker->identity;
}

void Reporter::start(Service& service) {
    if (m_worker->running.exchange(true)) return;
    m_worker->service = &service;
    m_worker->thread = std::thread([this] { loop(); });
}

void Reporter::stop() {
    if (!m_worker->running.exchange(false)) return;
    if (m_worker->thread.joinable()) m_worker->thread.join();
}

std::string Reporter::last_result() const {
    std::lock_guard<std::mutex> lk(m_worker->mtx);
    return m_worker->last_result;
}

void Reporter::loop() {
    multisite::collector_http_init();
    while (m_worker->running.load()) {
        serve_pairing();
        // Storage and monitoring are one subsystem here for the same reason
        // they are on the player: this worker already owns the collector
        // connection, the clock and the network. It fetches only when the
        // identity says one is due, so a steady state costs nothing.
        serve_credentials();
        serve_heartbeat();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

// ── Credentials ─────────────────────────────────────────────────────────────

void Reporter::serve_credentials() {
    const multisite::Enrolment en = m_worker->identity.enrolment();
    if (!en.paired()) return;
    if (m_worker->identity.tick(now_ms()) != multisite::CloudAction::Fetch)
        return;

    const std::string bucket_before = m_worker->identity.credentials().bucket;
    const multisite::HttpResult r = multisite::http_get_json(
        multisite::collector_url(en.url, multisite::kCredentialsPath), en.token);

    multisite::CredentialsReply reply;
    if (!r.reached) {
        // Unreached is a failed fetch, not an unpairing: last-good carries a
        // running relay through a collector outage.
        reply.ok = false;
    } else {
        reply = multisite::cloud_parse_credentials(r.body, (int)r.code);
    }

    const std::string error_before = m_worker->identity.error();
    m_worker->identity.on_credentials(reply, now_ms());
    const std::string error_after = m_worker->identity.error();

    if (error_after != error_before && !error_after.empty())
        rlog_warn("cloud credentials (relay, %s): %s", en.id.c_str(),
                  error_after.c_str());

    // A different bucket means the relay is reading somewhere else, so the
    // feeder has to be rebuilt around it. Same bucket on a refresh does not:
    // the session token rotates constantly and rebuilding on every refresh
    // would restart the feed every few minutes.
    if (multisite::credentials_move_storage(bucket_before, reply)) {
        rlog_info("cloud storage moved to %s — rebuilding",
                  reply.creds.bucket.c_str());
        if (m_worker->service) m_worker->service->reload();
    }
}

// ── Heartbeat ───────────────────────────────────────────────────────────────

void Reporter::serve_heartbeat() {
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lk(m_worker->mtx);
        if (now < m_worker->next_due) return;
    }

    const multisite::Enrolment en = m_worker->identity.enrolment();
    std::string outcome;
    int wait_s = multisite::kHeartbeatIdleIntervalS;

    if (!en.paired()) {
        // Nothing is sent by an unpaired relay: zero sockets, zero bytes. A
        // self-hosted relay with typed keys is the ordinary case and must not
        // be talking to anything.
        outcome = "not paired";
        wait_s = 5;              // re-read the enrolment promptly
    } else {
        const ServiceStatus st = m_worker->service->status();
        // Active means a service is actually going out somewhere — not that
        // the room is live, and not that destinations exist. A relay with
        // everything stopped is idle however busy the main site is.
        bool active = false;
        int sending = 0, blocked = 0, reconnecting = 0, restarts = 0;
        double behind = -1;
        for (const auto& d : st.destinations) {
            // d.live IS relay_state_is_sending() — one rule, read here rather
            // than restated, so the cadence and the page cannot disagree.
            if (d.live) { active = true; ++sending; }
            if (d.state == "blocked") ++blocked;
            if (d.state == "reconnecting") ++reconnecting;
            restarts += d.restarts;
            if (d.behind_live_s > behind) behind = d.behind_live_s;
        }

        json s;
        s["role"] = kRelayKind;
        s["version"] = MULTISITE_RELAY_VERSION;
        s["now_ms"] = now_ms();
        s["room_id"] = st.room_id;
        s["room_state"] = st.room_state;
        s["event_id"] = st.event_id;
        s["destinations"] = (int)st.destinations.size();
        s["sending"] = sending;
        s["blocked"] = blocked;
        s["reconnecting"] = reconnecting;
        s["out_kbps"] = st.total_out_kbps;
        if (behind >= 0) s["behind_live_s"] = behind;
        s["restarts"] = restarts;
        s["last_error"] = st.storage_error;
        s["paired"] = true;
        // What ADR-0002 bought, stated rather than assumed: an operator can
        // see that the most exposed box in the system holds read access only.
        s["read_only"] = !m_worker->identity.credentials().read_write;
        s["configured"] = st.configured;

        multisite::HeartbeatIdentity ident{
            en.id, kRelayKind, MULTISITE_RELAY_VERSION, 0.0};
        // No host block: that is the appliance's Pi sensors, and a VPS has
        // nothing to say through it that the collector expects.
        const std::string body = multisite::heartbeat_build(
            ident, nullptr,
            multisite::heartbeat_filter_status(kRelayKind, s.dump()),
            sent_at_iso());

        const multisite::HttpResult r = multisite::http_post_json(
            multisite::collector_url(en.url, multisite::kHeartbeatPath),
            en.token, body);

        int central;
        { std::lock_guard<std::mutex> lk(m_worker->mtx); central = m_worker->central_s; }

        if (!r.reached) {
            // Fire-and-forget: dropped, never queued. Not rate-limited — a
            // dead link is not the collector slowing us down.
            outcome = "not reached (dropped)";
            wait_s = multisite::heartbeat_next_interval_s(active, central, false, 0);
            std::lock_guard<std::mutex> lk(m_worker->mtx);
            m_worker->limited = false;
            m_worker->retry_after_s = 0;
        } else if (r.code == 200) {
            outcome = "accepted (200)";
            const int adopted = multisite::heartbeat_parse_server_interval(r.body);
            wait_s = multisite::heartbeat_next_interval_s(active, adopted, false, 0);
            std::lock_guard<std::mutex> lk(m_worker->mtx);
            m_worker->central_s = adopted;
            m_worker->limited = false;
            m_worker->retry_after_s = 0;
        } else if (r.code == 429) {
            outcome = "rate limited — backing off";
            const int adopted = multisite::heartbeat_parse_server_interval(r.body);
            wait_s = multisite::heartbeat_next_interval_s(active, adopted, true,
                                                          r.retry_after_s);
            std::lock_guard<std::mutex> lk(m_worker->mtx);
            m_worker->central_s = adopted;
            m_worker->limited = true;
            m_worker->retry_after_s = r.retry_after_s;
        } else if (const char* w = rejection_word(r.code)) {
            outcome = w;
            wait_s = multisite::heartbeat_next_interval_s(active, central, false, 0);
        } else {
            outcome = "http " + std::to_string(r.code) + " (dropped)";
            wait_s = multisite::heartbeat_next_interval_s(active, central, false, 0);
        }
    }

    std::lock_guard<std::mutex> lk(m_worker->mtx);
    m_worker->next_due = now + std::chrono::seconds(wait_s);
    if (outcome != m_worker->last_result) {
        // Said once per change (standards §8), not once per beat.
        m_worker->last_result = outcome;
        rlog_info("heartbeat relay: %s", outcome.c_str());
    }
}

// ── Pairing ─────────────────────────────────────────────────────────────────

bool Reporter::pair_begin(const std::string& collector_url) {
    if (collector_url.empty()) return false;
    std::lock_guard<std::mutex> lk(m_worker->mtx);
    m_worker->begin_pending = true;
    m_worker->begin_url = collector_url;
    m_worker->note.clear();
    m_worker->claim_saved = true;
    m_worker->claimed_id.clear();
    m_worker->claimed_token.clear();
    m_worker->claimed_url.clear();
    return true;
}

void Reporter::pair_cancel() {
    std::lock_guard<std::mutex> lk(m_worker->mtx);
    m_worker->begin_pending = false;
    m_worker->pairing.cancel();
    m_worker->note.clear();
}

PairView Reporter::pair_view() const {
    std::lock_guard<std::mutex> lk(m_worker->mtx);
    PairView v;
    switch (m_worker->pairing.phase()) {
        case multisite::Pairing::Phase::Idle:    v.phase = 0; break;
        case multisite::Pairing::Phase::Waiting: v.phase = 1; break;
        case multisite::Pairing::Phase::Done:    v.phase = 2; break;
        case multisite::Pairing::Phase::Expired: v.phase = 3; break;
        case multisite::Pairing::Phase::Failed:  v.phase = 4; break;
    }
    v.user_code = m_worker->pairing.user_code();
    v.verification_url = m_worker->pairing.verification_url();
    v.error = m_worker->pairing.error();
    v.note = m_worker->note;
    v.saved = m_worker->claim_saved;
    return v;
}

// Interactive — an operator is watching a code on screen — so a begin POSTs at
// once rather than waiting for a beat, while polls keep the cadence the
// collector named. Decided under lock, network outside it.
void Reporter::serve_pairing() {
    const long long now = wall_now_s();
    enum class Action { None, Start, Poll, SaveClaim };
    Action act = Action::None;
    std::string url;
    {
        std::lock_guard<std::mutex> lk(m_worker->mtx);
        url = m_worker->begin_url;
        if (m_worker->begin_pending) {
            act = Action::Start;
        } else {
            m_worker->pairing.tick(now);
            if (m_worker->pairing.poll_due(now))
                act = Action::Poll;
            else if (!m_worker->claim_saved && !m_worker->claimed_id.empty())
                act = Action::SaveClaim;
        }
    }
    if (act == Action::None) return;

    if (url.empty()) {
        std::lock_guard<std::mutex> lk(m_worker->mtx);
        m_worker->begin_pending = false;
        m_worker->pairing.cancel();
        m_worker->note = "enter the collector address first";
        return;
    }

    ConfigStore& cfg = m_worker->service->config();

    if (act == Action::Start) {
        // Minted once and kept, so re-pairing the same relay is recognisable
        // as the same box. Best-effort: this attempt carries the minted id
        // either way, and a restart simply mints again for the next attempt.
        auto pc = cfg.pairing();
        if (pc.device_id.empty()) {
            pc.device_id = multisite::heartbeat_mint_device_id(
                hostname(), kRelayKind, now_ns(), (long long)::getpid());
            cfg.set_pairing(pc);
        }
        const multisite::HttpResult r = multisite::http_post_json(
            multisite::collector_url(url, multisite::kPairStartPath),
            std::string(),
            multisite::heartbeat_pair_start_body(pc.device_id, kRelayKind,
                                                 hostname()));
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
        { std::lock_guard<std::mutex> lk(m_worker->mtx);
          token = m_worker->pairing.poll_token(); }
        const multisite::HttpResult r = multisite::http_post_json(
            multisite::collector_url(url, multisite::kPairPollPath),
            std::string(), multisite::heartbeat_pair_poll_body(token));
        std::lock_guard<std::mutex> lk(m_worker->mtx);
        if (!r.reached) { m_worker->note = "collector not reached"; return; }
        m_worker->note.clear();
        m_worker->pairing.on_poll_reply(r.body, (int)r.code, now);
        if (m_worker->pairing.phase() == multisite::Pairing::Phase::Done) {
            m_worker->claimed_id = m_worker->pairing.appliance_id();
            m_worker->claimed_token = m_worker->pairing.appliance_token();
            m_worker->claimed_url = m_worker->pairing.collector_url();
            m_worker->claim_saved = false;
        }
        return;
    }

    // SaveClaim: persist what was approved, and only then tell the identity.
    // Retried every tick until it lands, so a momentarily unwritable database
    // cannot lose an approval an operator has already given at the collector.
    ConfigStore::PairingConfig claimed = cfg.pairing();
    {
        std::lock_guard<std::mutex> lk(m_worker->mtx);
        claimed.appliance_id = m_worker->claimed_id;
        claimed.appliance_token = m_worker->claimed_token;
        claimed.collector_url =
            m_worker->claimed_url.empty() ? url : m_worker->claimed_url;
    }
    cfg.set_pairing(claimed);
    if (!cfg.paired()) {
        rlog_warn("pairing: approved, but the claim would not save");
        return;
    }
    // One snapshot into the identity: a url from one pairing with a token from
    // another is a device the collector has never heard of.
    m_worker->identity.set_enrolment(claimed.collector_url,
                                     claimed.appliance_id,
                                     claimed.appliance_token);
    rlog_info("paired as %s", claimed.appliance_id.c_str());
    { std::lock_guard<std::mutex> lk(m_worker->mtx); m_worker->claim_saved = true; }
    // Storage may now come from somewhere else entirely.
    m_worker->service->reload();
}

} // namespace multisite_relay
