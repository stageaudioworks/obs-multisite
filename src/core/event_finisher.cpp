// SPDX-License-Identifier: GPL-3.0-or-later
#include "event_finisher.h"

#include "log.h"
#include "model.h"
#include "retry_uploader.h"
#include "session.h"   // now_ms()

#include <chrono>

namespace multisite {

EventFinisher::EventFinisher(FinisherConfig cfg) : m_cfg(std::move(cfg)) {}

EventFinisher::~EventFinisher() { stop(); }

void EventFinisher::start() {
    if (m_running.exchange(true)) return;
    m_thread = std::thread([this] { loop(); });
}

void EventFinisher::stop() {
    m_running = false;
    if (m_thread.joinable()) m_thread.join();
}

EventFinisher::Status EventFinisher::status() const {
    Status st;
    for (const auto& id : SpoolQueue::finishing_events(m_cfg.spool_root)) {
        ++st.events;
        st.segments += SpoolQueue(m_cfg.spool_root, 0, id).pending_count();
    }
    return st;
}

void EventFinisher::abandon() {
    m_abandon = true;
    {
        // Waits for an upload in progress to notice and stop, so nothing is
        // being written into a folder as it is removed.
        std::lock_guard<std::mutex> lk(m_work_mtx);
        for (const auto& id : SpoolQueue::finishing_events(m_cfg.spool_root)) {
            log_warn("upload: abandoned the rest of event %s on the operator's "
                     "instruction", id.c_str());
            SpoolQueue::discard_event(m_cfg.spool_root, id);
        }
    }
    m_abandon = false;
}

void EventFinisher::loop() {
    while (m_running.load()) {
        run_once();
        for (int slept = 0; slept < m_cfg.rescan_ms && m_running.load(); slept += 100)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void EventFinisher::run_once() {
    // Called directly (a test), there is no thread to say it is running.
    const bool was_running = m_running.exchange(true);
    for (const auto& id : SpoolQueue::finishing_events(m_cfg.spool_root)) {
        if (m_abandon.load() || !m_running.load()) break;
        if (m_cfg.may_upload && !m_cfg.may_upload()) break;
        std::lock_guard<std::mutex> lk(m_work_mtx);
        if (!finish(id)) break;   // could not finish it now: try again next pass
    }
    if (!was_running) m_running = false;
}

// The room's live.json says "ended" for this event — unless it already does,
// or it names another event: a newer broadcast in the same room owns the
// pointer now, and must never be told it has ended. True when there is
// nothing left to do. ponytail: a new event starting between this read and
// the write is not guarded; its own heartbeat rewrites the pointer within
// seconds, which bounds it.
bool EventFinisher::end_live_pointer(Transport& tx, const std::string& event_id) {
    auto e = tx.get(event_prefix_for(event_id) + "event.json");
    if (!e.success) return e.http_status == 404;   // no descriptor: no room to fix
    std::string room;
    try { room = EventInfo::from_json(std::string(e.body.begin(), e.body.end())).room_id; }
    catch (...) { return true; }
    if (room.empty()) return true;
    const std::string key = live_pointer_key(room);
    auto r = tx.get(key);
    if (!r.success) return r.http_status == 404;
    LivePointer lp;
    try { lp = LivePointer::from_json(std::string(r.body.begin(), r.body.end())); }
    catch (...) { return true; }
    if (lp.event_id != event_id || lp.status == "ended") return true;
    lp.status = "ended";
    lp.updated_at_ms = now_ms();
    const std::string j = lp.to_json();
    if (!tx.put(key, std::vector<uint8_t>(j.begin(), j.end()), "application/json", {}).success)
        return false;
    log_info("upload: room %s's live.json now says event %s has ended",
             room.c_str(), event_id.c_str());
    return true;
}

bool EventFinisher::finish(const std::string& event_id) {
    std::shared_ptr<Transport> tx = m_cfg.transport ? m_cfg.transport() : nullptr;
    if (!tx) return false;
    const std::string prefix = event_prefix_for(event_id);

    // The event's manifest as the bucket has it, to carry on from. Without one
    // (the event ended before its first manifest landed) it is built from
    // event.json. If neither can be read for a reason other than "not there",
    // try again later rather than publish a manifest that loses what exists.
    Manifest m;
    {
        auto r = tx->get(prefix + "manifest.json");
        if (r.success) {
            try { m = Manifest::from_json(std::string(r.body.begin(), r.body.end())); }
            catch (...) { return false; }
        } else if (r.http_status == 404) {
            auto e = tx->get(prefix + "event.json");
            if (!e.success) return false;
            try {
                const EventInfo ev = EventInfo::from_json(std::string(e.body.begin(), e.body.end()));
                m.event_id            = ev.event_id;
                m.name                = ev.name;
                m.started_at_ms       = ev.started_at_ms;
                m.first_available_seq = ev.first_seq;
                m.video               = ev.video;
                m.audio_tracks        = ev.audio_tracks;
            } catch (...) { return false; }
        } else {
            return false;
        }
    }
    m.event_id = event_id;
    m.status   = "ended";

    std::mutex mm;
    auto publish = [&] {
        std::string j;
        {
            std::lock_guard<std::mutex> lk(mm);
            m.updated_at_ms = now_ms();
            j = m.to_json();
        }
        return tx->put(prefix + "manifest.json",
                       std::vector<uint8_t>(j.begin(), j.end()),
                       "application/json", {}).success;
    };

    SpoolQueue q(m_cfg.spool_root, 0, event_id);
    q.set_targets(1);   // the primary only (see the header)
    const size_t total = q.pending_count();
    // Once per event, not once per pass: a pass that cannot finish is retried
    // every few seconds, and the log should say what is happening, not count.
    if (m_announced != event_id) {
        m_announced = event_id;
        if (total > 0)
            log_info("upload: finishing event %s in the background — %zu segment(s) left",
                     event_id.c_str(), total);
        else
            log_info("upload: telling storage that event %s has ended, which End "
                     "could not", event_id.c_str());
    }

    UploaderConfig ucfg;
    ucfg.base_backoff_ms = m_cfg.base_backoff_ms;
    ucfg.max_backoff_ms  = m_cfg.max_backoff_ms;
    ucfg.jitter          = m_cfg.jitter;
    ucfg.tags.clear();   // R2 refuses object tags; expiry is the lifecycle rule's
    ucfg.may_upload = [this] {
        return !m_abandon.load() && (!m_cfg.may_upload || m_cfg.may_upload());
    };
    RetryUploader up(q, *tx, ucfg);
    // Listed the moment it lands, in the order it was captured — the same
    // write-ordering rule the live session keeps (§4.7).
    up.set_confirm_callback([&](const SpooledSegment& seg) {
        {
            std::lock_guard<std::mutex> lk(mm);
            ManifestSegment ms;
            ms.seq        = seg.seq;
            ms.duration_s = seg.duration_s;
            ms.checksum   = seg.checksum;
            ms.at_ms      = m.started_at_ms + (int64_t)(seg.pts_offset_s * 1000.0);
            m.push(ms, m_cfg.manifest_window);
        }
        publish();
    });
    up.set_skipped_callback([&](uint64_t seq) {
        { std::lock_guard<std::mutex> lk(mm); m.add_gap(seq); }
        publish();
    });

    up.start();
    while (m_running.load() && !m_abandon.load() && q.pending_count() > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    up.stop();
    // stop() cancels the transport so its join never waits out a request, and
    // that cancel is sticky. Everything below goes through the same transport,
    // so re-arm it — the trap Session::end() documents. Missing this, every
    // ending was published into a cancelled transport and failed at once: the
    // backlog uploaded, and the "ended" manifest and live.json never did
    // (seen live 2026-10-03, retried every few seconds for ever).
    tx->resume_pending();
    if (q.pending_count() > 0) return false;   // stopped, abandoned, or yielding

    // The last manifest is the one that says the event is complete; a failed
    // per-segment one is superseded by the next, but nothing follows this. And
    // the room's live.json, which End may not have reached either. Until both
    // land the folder stays, and the next pass tries again.
    if (!publish() || !end_live_pointer(*tx, event_id)) return false;
    SpoolQueue::discard_event(m_cfg.spool_root, event_id);
    if (total > 0)
        log_info("upload: event %s is complete in storage — the %zu segment(s) left "
                 "at its end are uploaded", event_id.c_str(), total);
    else
        log_info("upload: event %s is marked ended in storage", event_id.c_str());
    return true;
}

} // namespace multisite
