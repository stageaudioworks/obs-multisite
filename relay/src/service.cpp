// SPDX-License-Identifier: GPL-3.0-or-later
#include "service.h"

#include "cloud_identity.h"
#include "s3_transport.h"
#include "log.h"
#include "relay_send.h"

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace multisite_relay {

using namespace multisite;

Service::~Service() { stop(); }

std::string Service::start(const std::string& db_path) {
    std::string e = m_cfg.open(db_path);
    if (!e.empty()) return e;

    // Hand the saved pairing to the identity BEFORE the first reload, so a
    // paired relay comes up already knowing who it is and fetches on its first
    // tick rather than sitting unconfigured until something else happens.
    const auto pc = m_cfg.pairing();
    if (m_cfg.paired())
        m_reporter.cloud_identity().set_enrolment(pc.collector_url,
                                                  pc.appliance_id,
                                                  pc.appliance_token);
    // Always started: it sends nothing at all until paired, so an ordinary
    // self-hosted relay pays one sleeping thread and no sockets.
    m_reporter.start(*this);

    reload();
    m_running = true;
    m_thread = std::thread([this] { supervise(); });
    return {};
}

void Service::stop() {
    if (m_running.exchange(false) && m_thread.joinable()) m_thread.join();
    // Before the sessions: the reporter can call reload(), which takes the
    // lock the teardown below is about to hold.
    m_reporter.stop();
    std::lock_guard<std::mutex> lk(m_mtx);
    m_rebroadcast.reset();
    if (m_rebroadcast_feeder) m_rebroadcast_feeder->stop();
    m_rebroadcast_feeder.reset();
    m_sessions.clear();          // stops each supervisor and its child
    if (m_feeder) m_feeder->stop();
    m_feeder.reset();
}

// Whether two sets of storage settings describe the same bucket. Rebuilding
// the downloader means dropping the cache and every stream with it, so it must
// happen when the bucket really changed and never merely because something was
// saved.
static bool same_storage(const multisite::S3Config& a,
                         const multisite::S3Config& b) {
    return a.endpoint_host == b.endpoint_host
        && a.r2_account_id == b.r2_account_id
        && a.bucket == b.bucket
        && a.access_key_id == b.access_key_id
        && a.secret_access_key == b.secret_access_key
        && a.region == b.region
        && a.use_https == b.use_https;
}

static bool same_lan(const ConfigStore::LanConfig& a,
                     const ConfigStore::LanConfig& b) {
    return a.host == b.host && a.port == b.port && a.auth_token == b.auth_token;
}

// Where the relay's bucket credentials come from.
//
// When the provider is Multisite Cloud they come from the pairing and NOTHING
// ELSE: the typed fields are not consulted, not even as a fallback. A relay
// reading a bucket nobody paired it to is the state Phase 12 exists to make
// unrepresentable, and ADR-0001 is explicit that a role which is not paired is
// not paired rather than quietly something else. So an empty S3Config here
// means "paired, but no credentials yet", which reads as not-configured and
// says so, rather than silently falling back to whatever was typed before.
multisite::S3Config Service::effective_storage() const {
    if (!m_cfg.storage_is_paired()) return m_cfg.storage();
    const auto creds = const_cast<Reporter&>(m_reporter)
                           .cloud_identity().credentials();
    if (!creds.present()) return multisite::S3Config{};
    return multisite::s3_config_from_credentials(creds);
}

void Service::reload() {
    const auto storage = effective_storage();
    const auto lan = m_cfg.lan();
    const auto room = m_cfg.room();
    const auto dests = m_cfg.destinations();

    std::lock_guard<std::mutex> lk(m_mtx);

    // Does the downloader itself have to be rebuilt? Only if the bucket, the
    // LAN path, or the room changed. Adding a destination must NOT reach this
    // far: doing so tore down every stream that was already on air, which is
    // precisely what an operator does mid-event when they decide to add
    // Facebook.
    const bool feeder_stale =
        !m_feeder || !same_storage(storage, m_feeder_storage) ||
        !same_lan(lan, m_feeder_lan) || room.room_id != m_feeder_room;

    if (!feeder_stale) {
        sync_destinations_locked(dests, room);
        return;
    }

    // Sessions hold a reference to the feeder, so they must go before it does.
    m_sessions.clear();
    if (m_feeder) { m_feeder->stop(); m_feeder.reset(); }

    // Cloud alone used to be the whole gate here; a LAN-only relay has no
    // bucket credentials at all, and configured() is what admits that case.
    if (!m_cfg.configured()) {
        m_storage_error = "Storage has not been set up yet.";
        return;
    }
    // Paired, but nothing usable has arrived yet. Distinguished from "not set
    // up" on purpose: one is waiting and one needs an operator, and telling
    // them apart is the difference between watching and intervening.
    if (m_cfg.storage_is_paired() && storage.bucket.empty() &&
        lan.host.empty()) {
        const std::string why =
            const_cast<Reporter&>(m_reporter).cloud_identity().error();
        m_storage_error = why.empty()
            ? "Waiting for storage details from Multisite Cloud."
            : why;
        return;
    }
    m_storage_error.clear();

    FeederConfig fc;
    fc.storage = storage;
    fc.lan.host = lan.host;
    fc.lan.port = lan.port;
    fc.lan.auth_token = lan.auth_token;
    fc.room_id = room.room_id;
    fc.cache_dir = "/data/cache";
    if (const char* c = ::getenv("RELAY_CACHE_DIR")) fc.cache_dir = c;

    // The download window has to cover the furthest-behind destination, with
    // room to run ahead of it. A destination sitting three minutes back whose
    // segments were only ever fetched at the live edge would find nothing to
    // send.
    int deepest = room.default_delay_s;
    for (const auto& d : dests)
        deepest = std::max(deepest, d.delay_s > 0 ? d.delay_s : room.default_delay_s);
    fc.buffer_minutes = std::max(10, deepest / 60 + 5);

    m_feeder = std::make_unique<RoomFeeder>(fc);
    m_feeder->start();
    m_feeder_storage = storage;
    m_feeder_lan = lan;
    m_feeder_room = room.room_id;

    sync_destinations_locked(dests, room);
    rlog_info("watching room \"%s\" with %zu destination(s)",
              room.room_id.c_str(), dests.size());
}

// Bring the running sessions into line with the database, disturbing as little
// as possible. A destination that has not changed keeps its stream, its
// position and its uptime; only what actually differs is acted on.
void Service::sync_destinations_locked(const std::vector<Destination>& dests,
                                       const RoomSettings& room) {
    if (!m_feeder) return;

    std::set<int64_t> seen;
    for (const auto& d : dests) {
        Destination copy = d;
        if (copy.delay_s <= 0) copy.delay_s = room.default_delay_s;
        seen.insert(d.id);

        auto it = m_sessions.find(d.id);
        if (it == m_sessions.end()) {
            auto s = std::make_unique<RelaySession>(copy, *m_feeder);
            s->start_thread();
            m_sessions.emplace(d.id, std::move(s));
            rlog_info("destination \"%s\" added", copy.name.c_str());
            continue;
        }
        // update() decides for itself whether anything here is worth
        // interrupting a live stream for.
        it->second->update(copy);
    }

    for (auto it = m_sessions.begin(); it != m_sessions.end(); ) {
        if (seen.count(it->first)) { ++it; continue; }
        rlog_info("destination removed");
        it = m_sessions.erase(it);          // its destructor stops the stream
    }
}

void Service::set_enabled(int64_t id, bool on) {
    m_cfg.set_enabled(id, on);
    std::lock_guard<std::mutex> lk(m_mtx);
    auto it = m_sessions.find(id);
    if (it != m_sessions.end()) it->second->set_enabled(on);
}

std::string Service::check_storage() {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_feeder) return "Storage has not been set up yet.";
    return m_feeder->check_storage();
}

void Service::supervise() {
    while (m_running) {
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            if (m_feeder) {
                // Keep the download window over the furthest-behind reader, so
                // nothing a destination still needs is pruned underneath it.
                bool any = false;
                uint64_t lowest = 0;
                for (auto& kv : m_sessions) {
                    if (!kv.second->wants_content()) continue;
                    if (!kv.second->has_position()) continue;
                    const uint64_t h = kv.second->head();
                    lowest = any ? std::min(lowest, h) : h;
                    any = true;
                }
                if (any) m_feeder->set_lowest_reader(lowest);
            }
            if (m_rebroadcast_feeder && m_rebroadcast &&
                m_rebroadcast->has_position())
                m_rebroadcast_feeder->set_lowest_reader(m_rebroadcast->head());

            // A rebroadcast that has played to the end tidies itself away, so
            // the operator is not left looking at a finished job wondering
            // whether it is still doing something.
            if (m_rebroadcast &&
                m_rebroadcast->status().state == std::string("stopped")) {
                rlog_info("rebroadcast finished");
                m_rebroadcast.reset();
                if (m_rebroadcast_feeder) m_rebroadcast_feeder->stop();
                m_rebroadcast_feeder.reset();
                m_rebroadcast_event.clear();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

RoomFeeder* Service::feeder() {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_feeder.get();
}

std::vector<EventSummary> Service::events(bool force) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_feeder) return {};
    return m_feeder->events(force);
}

std::string Service::check_event_is_finished(const std::string& event_id) {
    if (event_id.empty()) return "No event was chosen.";
    // Past events are found by listing the bucket, which LAN does not serve —
    // see RoomFeeder::events(). A LAN-only relay can still carry the live
    // event; it just has nothing to offer here.
    if (!m_cfg.storage_configured())
        return "Past events need cloud storage, which this relay does not "
               "have configured.";
    for (const auto& e : events()) {
        if (e.event_id != event_id) continue;
        // The rule the brief asks for, and the reason for it: an event still
        // being recorded has no end yet, so a download would be a snapshot of
        // an unfinished thing and a rebroadcast would run into the live edge.
        if (e.state == EventState::Live)
            return "That event is still going out. Wait until it has "
                   "finished.";
        return {};
    }
    return "That event is no longer in storage.";
}

std::vector<multisite::Marker> Service::event_cues(const std::string& event_id,
                                                   std::string& error) const {
    const auto storage = m_cfg.storage();
    if (storage.bucket.empty()) {
        error = "Storage has not been set up yet.";
        return {};
    }
    // A throwaway feeder, used only for its store readers. Nothing is started,
    // so no download thread runs and the cache directory it names is never
    // written to.
    FeederConfig fc;
    fc.storage = storage;
    fc.room_id = m_cfg.room().room_id;
    fc.cache_dir = (::getenv("RELAY_CACHE_DIR")
                        ? std::string(::getenv("RELAY_CACHE_DIR"))
                        : std::string("/data/cache")) + "/cues";
    RoomFeeder probe(fc);
    return probe.event_cues(event_id, error);
}

std::string Service::start_rebroadcast(const std::string& event_id,
                                       int64_t dest_id,
                                       const std::string& start_cue,
                                       const std::string& end_cue) {
    const std::string problem = check_event_is_finished(event_id);
    if (!problem.empty()) return problem;

    auto dest = m_cfg.destination(dest_id);
    if (!dest) return "That destination no longer exists.";

    // Resolve the in and out cues to segment numbers BEFORE taking the lock:
    // this reads the store, and the setup below holds the lock throughout.
    uint64_t start_seq = 0, end_seq = 0;
    if (!start_cue.empty() || !end_cue.empty()) {
        std::string cue_error;
        const auto cues = event_cues(event_id, cue_error);
        if (!cue_error.empty()) return cue_error;
        auto resolve = [&](const std::string& id, uint64_t& out) {
            for (const auto& m : cues)
                if (m.id == id) { out = m.seq; return true; }
            return false;
        };
        if (!start_cue.empty() && !resolve(start_cue, start_seq))
            return "That in-point cue is no longer in the event.";
        if (!end_cue.empty() && !resolve(end_cue, end_seq))
            return "That out-point cue is no longer in the event.";
        if (start_seq > 0 && end_seq > 0 && start_seq > end_seq)
            return "The in-point is after the out-point.";
    }

    const auto storage = m_cfg.storage();
    const auto room = m_cfg.room();

    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_rebroadcast) return "A rebroadcast is already running.";
    if (!m_feeder) return "Storage has not been set up yet.";

    // A destination cannot carry a live relay and a rebroadcast at once: it is
    // one stream key, and pushing two things to it produces a mess at the far
    // end that is hard to diagnose from here.
    auto live = m_sessions.find(dest_id);
    if (live != m_sessions.end() && live->second->wants_content())
        return "That destination is already sending the live event. Stop it "
               "first.";

    FeederConfig fc;
    fc.storage = storage;
    fc.room_id = room.room_id;
    fc.cache_dir = (::getenv("RELAY_CACHE_DIR")
                        ? std::string(::getenv("RELAY_CACHE_DIR"))
                        : std::string("/data/cache")) + "/rebroadcast";
    fc.buffer_minutes = 5;
    fc.stale_after_ms = 0;          // a finished event never goes stale
    fc.pinned_event_id = event_id;

    m_rebroadcast_feeder = std::make_unique<RoomFeeder>(fc);
    m_rebroadcast_feeder->start();

    Destination d = *dest;
    d.enabled = true;
    d.delay_s = 0;                  // a rebroadcast has no live edge to sit behind
    m_rebroadcast = std::make_unique<RelaySession>(d, *m_rebroadcast_feeder,
                                                   /*from_beginning=*/true,
                                                   start_seq, end_seq);
    m_rebroadcast->start_thread();
    m_rebroadcast_event = event_id;
    rlog_info("rebroadcasting %s to \"%s\"%s", event_id.c_str(), d.name.c_str(),
              (start_seq > 0 || end_seq > 0) ? " (an excerpt)" : "");
    return {};
}

void Service::stop_rebroadcast() {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_rebroadcast) return;
    m_rebroadcast.reset();                  // stops the stream and its child
    if (m_rebroadcast_feeder) m_rebroadcast_feeder->stop();
    m_rebroadcast_feeder.reset();
    m_rebroadcast_event.clear();
    rlog_info("rebroadcast stopped");
}

bool Service::rebroadcasting() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_rebroadcast != nullptr;
}

RelayStatus Service::rebroadcast_status() const {
    RelaySession* s = nullptr;
    { std::lock_guard<std::mutex> lk(m_mtx); s = m_rebroadcast.get(); }
    return s ? s->status() : RelayStatus{};
}

std::string Service::rebroadcast_event() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_rebroadcast_event;
}

ServiceStatus Service::status() const {
    ServiceStatus s;
    const auto room = const_cast<ConfigStore&>(m_cfg).room();
    s.room_id = room.room_id;
    s.configured = const_cast<ConfigStore&>(m_cfg).configured();

    std::lock_guard<std::mutex> lk(m_mtx);
    s.storage_error = m_storage_error;
    if (!m_feeder) {
        s.room_state = "offline";
        // Three different reasons nothing is being read, and they need
        // different things from the operator: fill something in, wait, or go
        // and change something at the collector. Saying "not set up yet" for
        // all three sends someone to the settings page to re-enter details
        // that are already correct.
        if (!m_cfg.configured())
            s.room_state_text = "Storage has not been set up yet";
        else if (!m_storage_error.empty())
            s.room_state_text = "Storage is not usable yet";
        else
            s.room_state_text = "Waiting for storage details";
        return s;
    }

    const auto snap = m_feeder->snapshot();
    s.event_id = snap.event_id;
    s.lan_configured = snap.lan_configured;
    s.lan_active = snap.lan_active;
    switch (snap.room) {
        case RoomState::Live:
            s.room_state = "live";
            s.room_state_text = "An event is on air";
            break;
        case RoomState::Ended:
            s.room_state = "ended";
            s.room_state_text = "The last event has finished";
            break;
        case RoomState::Interrupted:
            s.room_state = "interrupted";
            s.room_state_text = "The main site stopped without ending the event";
            break;
        default:
            s.room_state = "offline";
            s.room_state_text = "Nothing is on air";
            break;
    }
    if (!snap.last_error.empty() && s.storage_error.empty())
        s.storage_error = snap.last_error;

    for (const auto& t : snap.manifest.audio_tracks) s.audio_labels.push_back(t.label);

    if (snap.have_event_info) {
        const auto& v = snap.manifest.video;
        std::string codec = v.codec;
        if (codec == "h264") codec = "H.264";
        else if (codec == "hevc") codec = "HEVC";
        else if (codec == "av1") codec = "AV1";
        if (v.width > 0)
            s.video_summary = std::to_string(v.width) + "x" +
                              std::to_string(v.height) + " " + codec;
        else
            s.video_summary = codec;

        // Answered once for the whole room, so the UI can warn before an
        // operator sets a destination up and wonders why it will not start.
        //
        // Once per protocol, because they do not accept the same video: a
        // single RTMP probe used to answer this, and once SRT could carry
        // HEVC that probe started declaring an event unsendable while an SRT
        // destination was busy sending it.
        const auto send = sendability(snap.manifest);
        s.can_send = send.any;
        s.cannot_send_reason = send.problem;
        s.send_note = send.note;
    }

    for (const auto& kv : m_sessions) {
        auto st = kv.second->status();
        if (st.live) s.total_out_kbps += st.bitrate_kbps;
        s.destinations.push_back(std::move(st));
    }
    return s;
}

} // namespace multisite_relay
