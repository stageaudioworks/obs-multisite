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
    log_info("upload: finishing event %s in the background — %zu segment(s) left",
             event_id.c_str(), total);

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
    if (q.pending_count() > 0) return false;   // stopped, abandoned, or yielding

    // The last manifest is the one that says the event is complete; a failed
    // per-segment one is superseded by the next, but nothing follows this.
    for (int attempt = 0; attempt < 5 && !publish(); ++attempt)
        std::this_thread::sleep_for(std::chrono::seconds(1 + attempt));
    SpoolQueue::discard_event(m_cfg.spool_root, event_id);
    log_info("upload: event %s is complete in storage — the %zu segment(s) left "
             "at its end are uploaded", event_id.c_str(), total);
    return true;
}

} // namespace multisite
