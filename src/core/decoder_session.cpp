// SPDX-License-Identifier: GPL-3.0-or-later
#include "decoder_session.h"
#include "log.h"
#include "session.h"     // for now_ms()

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace multisite {

static std::string seq_name(uint64_t seq) {
    char b[16];
    std::snprintf(b, sizeof(b), "%08llu", (unsigned long long)seq);
    return b;
}

DecoderSession::DecoderSession(DecoderConfig cfg, Transport& transport)
    : m_cfg(std::move(cfg)), m_tx(transport) {
    m_cache = std::make_unique<SegmentCache>(m_cfg.cache_dir, "pending");
    m_pinned_event_id = m_cfg.pinned_event_id;
}

// ── Pinning ──────────────────────────────────────────────────────────────────
// Changing what is being played is the same upheaval as the room switching
// events: the cache, the playhead and the decoder timeline all belong to the
// old event. poll() performs that reset when it sees the target change, so
// these only record the intent.
void DecoderSession::pin_event(const std::string& event_id) {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_pinned_event_id = event_id;
}
void DecoderSession::unpin() {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_pinned_event_id.clear();
    // Following the room again is an explicit choice: do not immediately
    // re-hold a finished event the operator has just let go of.
    m_end_hold_done = true;
}
std::string DecoderSession::pinned_event() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_pinned_event_id;
}
bool DecoderSession::is_pinned() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return !m_pinned_event_id.empty();
}
std::string DecoderSession::live_event_id() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_live_event_id;
}
bool DecoderSession::live_elsewhere() const {
    if (!m_room_is_live.load()) return false;
    std::lock_guard<std::mutex> lk(m_mtx);
    return !m_live_event_id.empty() && m_live_event_id != m_event_id;
}

std::string DecoderSession::event_prefix() const {
    return event_prefix_for(m_event_id);
}
std::string DecoderSession::segment_key(uint64_t seq) const {
    return event_prefix() + "segments/" + seq_name(seq) + ".m4s";
}

std::string DecoderSession::checksum_for(uint64_t seq) const {
    for (const auto& s : m_manifest.segments)
        if (s.seq == seq) return s.checksum;
    return "";     // outside the manifest window: verify not possible
}

// ── Discovery ────────────────────────────────────────────────────────────────
RoomState DecoderSession::poll(int64_t now_override) {
    // Network fetches happen WITHOUT the state lock held.
    //
    // This function used to hold m_mtx across three HTTP requests, and
    // pump_downloads held it across every segment download. Every UI query —
    // the dock refreshes several times a second — then had to wait for
    // whatever download was in flight, which made the interface sluggish and
    // made a timeline click appear to lock OBS up entirely. The lock now
    // protects state only, and is never held across I/O.
    const int64_t now = now_override ? now_override : now_ms();

    // If the last poll found the manifest stalled without the event having
    // ended, the end being read has stopped being written to — which is what a
    // write-side failover looks like from here: 200s and a manifest that never
    // moves again. Nothing below this can detect it (every request succeeds),
    // so the decision is taken here and the transport is told which end to
    // prefer. Exactly once, so a properly stalled event does not flap.
    if (m_manifest_stalled.load() && !m_switched_target.load() &&
        !m_tx.preferring_secondary()) {
        m_tx.prefer_secondary(true);
        m_switched_target = true;
    }

    // 1. Which event is live in this room? Read even when an event is pinned:
    //    the answer is not what to play, but it is what tells an operator
    //    watching a recording that an event has started.
    std::string pinned;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        pinned = m_pinned_event_id;
    }

    LivePointer live;
    std::string live_error;
    auto lp = m_tx.get(live_pointer_key(m_cfg.room_id));
    // A request we aborted ourselves is not the store failing.
    if (!m_tx.last_request_cancelled())
        m_link.observe(link_reachable_result(lp.success, lp.http_status), now);
    if (!lp.success) {
        // Our own cancellation says nothing about the room. Record no error and
        // leave the event unresolved rather than treating an empty body as a
        // failed read.
        if (!m_tx.last_request_cancelled())
            live_error = "live.json: HTTP " + std::to_string(lp.http_status) +
                         " " + lp.error;
    } else {
        try {
            live = LivePointer::from_json(
                std::string(lp.body.begin(), lp.body.end()));
        } catch (...) {
            live_error = "live.json is not valid JSON";
            live = LivePointer{};
        }
    }

    // A pinned event plays whatever the room is doing — including when the
    // room pointer cannot be read at all. Only an unpinned session depends on
    // live.json to know what to play.
    const std::string target = pinned.empty() ? live.event_id : pinned;
    if (target.empty()) {
        // A cancelled read gives no answer, and no answer is not "offline" —
        // stopping a source must not blank the room it was watching.
        if (m_tx.last_request_cancelled()) return m_room.load();
        std::lock_guard<std::mutex> lk(m_mtx);
        m_live_event_id = live.event_id;
        m_room_is_live = false;
        if (!live_error.empty()) {
            std::lock_guard<std::mutex> elk(m_err_mtx); m_last_error = live_error;
        }
        m_room = RoomState::Offline;
        return m_room;
    }

    // 2. Note an event change under the lock, then release it before fetching.
    std::string event_id;
    bool need_markers = false;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_live_event_id = live.event_id;
        if (target != m_event_id) {
            m_event_id = target;
            m_cache->set_event(m_event_id);
            // Clear the head itself, not only the "is it seated" flag. A
            // sequence number means nothing outside the event it came from,
            // and head() has no m_head_set guard — so switching events left
            // it reporting the old event's position against the new event's
            // manifest (head=1711 against a live edge of 865 in one log).
            // behind_live_s() answers 0 when the head is past the live edge,
            // so a stale head read as "caught up" rather than as wrong.
            m_head = 0;
            m_head_set = false;
            m_init_sent = false;
            m_play = PlayState::Stopped;
            m_markers = MarkerList{};
            m_markers_checked_ms = 0;
            m_saw_live = false;     // a new event has not been seen live yet
            m_end_hold_done = false;   // ...and it has not been seen to end yet
            ++m_discontinuity;      // new event: new init segment and timeline
        }
        event_id = m_event_id;
        if (now - m_markers_checked_ms > 5000) {
            m_markers_checked_ms = now;
            need_markers = true;
        }
    }
    const std::string prefix = event_prefix_for(event_id);

    // 3. Manifest, fetched without the lock.
    auto mf = m_tx.get(prefix + "manifest.json");
    if (!m_tx.last_request_cancelled())
        m_link.observe(link_reachable_result(mf.success, mf.http_status), now);
    if (!mf.success) {
        // Same as live.json: an abort is not an answer, and the empty body that
        // comes with it must not be handed to the parser — doing so threw, and
        // the throw reported "manifest.json is not valid JSON" for a request we
        // cancelled ourselves.
        if (m_tx.last_request_cancelled()) return m_room.load();
        std::lock_guard<std::mutex> lk(m_mtx);
        { std::lock_guard<std::mutex> elk(m_err_mtx); m_last_error = "manifest.json: HTTP " + std::to_string(mf.http_status) +
                       " " + mf.error; }
        m_room = RoomState::Offline;
        return m_room;
    }
    Manifest fetched;
    try {
        fetched = Manifest::from_json(
            std::string(mf.body.begin(), mf.body.end()));
    } catch (...) {
        std::lock_guard<std::mutex> lk(m_mtx);
        { std::lock_guard<std::mutex> elk(m_err_mtx); m_last_error = "manifest.json is not valid JSON"; }
        m_room = RoomState::Offline;
        return m_room;
    }

    // 3a. Is this a protocol we still understand?
    //
    // Checked here rather than at live.json because this is the document whose
    // misreading does damage: segment sequence, checksums and timing all come
    // from it, and a field that changed meaning would show as a stutter or a
    // failed checksum long after the cause. live.json yields only an event id,
    // and getting that wrong is caught here anyway.
    //
    // Refusing plainly is the whole point of the field. Offline is the honest
    // state — there is nothing to play — and the error says why, because "the
    // picture never came up" and "this build is too old" need different
    // answers from whoever is standing in the room.
    if (!protocol_readable(fetched.protocol_version)) {
        std::lock_guard<std::mutex> lk(m_mtx);
        { std::lock_guard<std::mutex> elk(m_err_mtx);
          m_last_error = "this event needs a newer version of the plugin — it was "
                         "recorded with storage protocol " +
                         std::to_string(fetched.protocol_version) +
                         " and this build reads " +
                         std::to_string(kProtocolVersion); }
        m_room = RoomState::Offline;
        return m_room;
    }

    // 4. Cues (small, and only every few seconds), also unlocked.
    //
    // One object per author: the encoder writes markers.json, and a satellite
    // that drops a cue writes its own cues/{site}.json (see add_cue). Read both
    // and merge, so a cue set at any site reaches every site. A listing that
    // fails — a LAN-only satellite, or a key without ListBucket — simply leaves
    // the encoder's markers.json, which is all a decoder saw before cues could
    // be authored at a satellite at all.
    MarkerList markers;
    bool have_markers = false;
    if (need_markers) {
        std::vector<MarkerList> parts;

        auto mk = m_tx.get(prefix + "markers.json");
        if (!m_tx.last_request_cancelled())
        m_link.observe(link_reachable_result(mk.success, mk.http_status), now);
        if (mk.success) {
            try {
                parts.push_back(MarkerList::from_json(
                    std::string(mk.body.begin(), mk.body.end())));
            } catch (...) {
                // A malformed markers file must not disturb playback.
            }
        }

        auto ls = m_tx.list(prefix + "cues/", "", "", 1000);
        if (ls.success) {
            for (const auto& e : ls.keys) {
                if (e.key.size() < 6 ||
                    e.key.compare(e.key.size() - 5, 5, ".json") != 0)
                    continue;
                auto cg = m_tx.get(e.key);
                if (!cg.success) continue;
                try {
                    parts.push_back(MarkerList::from_json(
                        std::string(cg.body.begin(), cg.body.end())));
                } catch (...) {
                    // One site's unreadable cue file must not hide the others.
                }
            }
        }

        if (!parts.empty()) {
            markers = merge_markers(std::move(parts));
            have_markers = true;
        }
        // No cue object at all simply means none has been dropped yet.
    }

    // 5. event.json for the start time, if the manifest lacks it (older
    //    encoders). Fetched unlocked, once per event.
    int64_t started_at = fetched.started_at_ms;
    double  seg_hint = 0.0;
    if (started_at <= 0) {
        auto ev = m_tx.get(prefix + "event.json");
        if (!m_tx.last_request_cancelled())
        m_link.observe(link_reachable_result(ev.success, ev.http_status), now);
        if (ev.success) {
            try {
                EventInfo info = EventInfo::from_json(
                    std::string(ev.body.begin(), ev.body.end()));
                started_at = info.started_at_ms;
                seg_hint   = info.segment_duration_s;
            } catch (...) {}
        }
    }

    // 6. Commit the new state.
    std::lock_guard<std::mutex> lk(m_mtx);
    m_manifest = std::move(fetched);
    if (started_at > 0) m_manifest.started_at_ms = started_at;
    m_started_at_ms = m_manifest.started_at_ms;   // lock-free for the UI
    if (have_markers) m_markers = std::move(markers);

    m_latest_seq          = m_manifest.latest_seq;
    m_first_available_seq = m_manifest.first_available_seq;
    m_manifest_updated_ms = m_manifest.updated_at_ms;
    if (m_manifest.stream_duration_hint() > 0.1)
        m_segment_duration_s = m_manifest.stream_duration_hint();
    else if (seg_hint > 0.1)
        m_segment_duration_s = seg_hint;

    // The TRUE segment length, from the times the encoder recorded. See
    // segment_ms() for why the nominal cannot be used to place a position.
    {
        const ManifestSegment* first = nullptr;
        const ManifestSegment* last  = nullptr;
        for (const auto& sg : m_manifest.segments) {
            if (sg.at_ms <= 0) continue;
            if (!first) first = &sg;
            last = &sg;
        }
        if (first && last && last->seq > first->seq && last->at_ms > first->at_ms)
            m_measured_segment_ms =
                (last->at_ms - first->at_ms) / (int64_t)(last->seq - first->seq);
    }

    // Stale detection: an encoder that died leaves live.json pointing at an
    // event whose manifest stops advancing. Stop treating it as live — but
    // it is still a recording of everything that happened up to the moment the
    // encoder went, and that is exactly the material someone wants afterwards.
    // Reporting it as Offline (as this once did) made a crashed event
    // permanently unwatchable, since nothing offline can be played.
    const int64_t age = now - m_manifest_updated_ms;
    // Recorded for the next poll to act on. An ENDED event is not stalled — it
    // is finished, and switching ends for it would be wrong and would also lose
    // the recording.
    m_manifest_stalled = (m_manifest.status != "ended" &&
                          m_manifest_updated_ms > 0 &&
                          age > m_cfg.stale_after_ms);
    if (m_manifest.status == "ended") {
        m_room = RoomState::Ended;
    } else if (m_manifest_updated_ms > 0 && age > m_cfg.stale_after_ms) {
        { std::lock_guard<std::mutex> elk(m_err_mtx); m_last_error = "manifest last updated " + std::to_string(age / 1000) +
                       "s ago — the encoder stopped without ending the event"; }
        m_room = RoomState::Interrupted;
    } else {
        m_room = RoomState::Live;
        m_saw_live = true;          // remembered for the rest of this event
        { std::lock_guard<std::mutex> elk(m_err_mtx); m_last_error.clear(); }
    }
    // Whether the ROOM is live, which is not the same as whether the event
    // being played is: a pinned recording sits alongside a live event.
    m_room_is_live = (!live.event_id.empty() && live.status == "live" &&
                      (m_cfg.stale_after_ms <= 0 ||
                       now - live.updated_at_ms <= (int64_t)m_cfg.stale_after_ms));

    // An event that has FINISHED while it was being watched must not be yanked
    // away by the next one starting in the room.
    //
    // Choosing an event pins it, and the docs promise a pinned recording is
    // never stolen. Following the room's live feed and then watching it end was
    // the same commitment in practice, but the session stayed unpinned — so
    // when live.json moved on to the next event, the target changed and play
    // jumped out of the recording the operator was part-way through. Hold it
    // exactly as if it had been pinned: live_elsewhere() then reports the new
    // event and the dock offers the switch instead of taking it. Done ONCE, at
    // the moment of ending, so Back to live still follows the room afterwards.
    // ENDED only — NOT is_vod(), which also covers Interrupted, and that
    // distinction is load-bearing once there are two buckets: an interrupted
    // manifest is exactly the shape a write-side failover leaves behind on the
    // end that was abandoned, so holding it here would pin the session to the
    // stale copy and defeat the switch to the end that is still being written.
    // A genuinely stopped encoder still holds, because its event ends up Ended
    // or its live pointer simply stops naming a new one.
    if (m_cfg.hold_finished_event && m_room.load() == RoomState::Ended) {
        if (!m_end_hold_done && m_pinned_event_id.empty() && !m_event_id.empty())
            m_pinned_event_id = m_event_id;
        m_end_hold_done = true;
    }
    return m_room;
}

// ── Downloading ──────────────────────────────────────────────────────────────

int DecoderSession::pump_downloads(int max) {
    // Same discipline as poll(): the lock is held only to decide WHAT to
    // fetch and to record the result. The downloads themselves — potentially
    // several megabytes each — happen with no lock held, so the UI stays
    // responsive while the buffer fills.
    std::string prefix;
    uint64_t from = 0, to = 0;
    std::vector<std::pair<uint64_t, std::string>> wanted;   // seq, checksum
    bool need_init = false;

    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_event_id.empty()) return 0;
        prefix = event_prefix_for(m_event_id);
        need_init = !m_cache->has_init();

        if (m_head_set.load()) {
            from = m_head.load();
        } else if (is_vod(m_room.load()) || !m_pinned_event_id.empty()) {
            // A finished recording plays from the beginning, so fetch from the
            // beginning. Downloading from the live edge while playback intends
            // to start at segment zero left the first segment missing and
            // start() refused to begin.
            from = m_first_available_seq.load();
        } else {
            const uint64_t back = start_reserve_segments();
            from = (m_latest_seq.load() > back) ? (m_latest_seq.load() - back)
                                                : m_first_available_seq.load();
        }
        from = std::max(from, m_first_available_seq.load());

        // Buffer target in minutes of programme, converted to segments. When
        // far behind live this is a wide window on purpose: bank as much as
        // the link can manage rather than trickling at playback speed.
        const double seg = segment_duration_s();
        uint64_t want_ahead = (uint64_t)std::max(
            1.0, ((double)std::max(1, m_cfg.buffer_minutes) * 60.0) / seg);
        // The start gate must be reachable: never download less than the
        // reserve start() is waiting for, or playback could never begin.
        want_ahead = std::max(want_ahead, start_reserve_segments());
        if (want_ahead > (uint64_t)m_cfg.max_cached_segments)
            want_ahead = (uint64_t)m_cfg.max_cached_segments;
        to = std::min<uint64_t>(m_latest_seq.load(), from + want_ahead);

        // One copy of the index, rather than a lock acquisition per segment
        // across a window that can be hundreds of segments wide.
        const auto have = m_cache->cached_seqs();
        for (uint64_t sq = from; sq <= to && (int)wanted.size() < max; ++sq) {
            if (have.count(sq)) continue;
            std::string sum;
            for (const auto& ms : m_manifest.segments)
                if (ms.seq == sq) { sum = ms.checksum; break; }
            wanted.emplace_back(sq, sum);
        }
    }

    // ── unlocked from here ───────────────────────────────────────────────────
    if (need_init) {
        auto r = m_tx.get(prefix + "init.mp4");
        if (!m_tx.last_request_cancelled())
            m_link.observe(link_reachable_result(r.success, r.http_status));
        if (!r.success) {
            // A cancelled request is not an error to report, but it is not an
            // init segment either. It used to fall through to the store below
            // and write its empty body as init.mp4 — which has_init() then
            // counted as present, so it was never fetched again and playback
            // waited on it for ever (a Stop or seek during the download, on a
            // Mac following a ROCK 5B encoder, 2026-09-26).
            if (!m_tx.last_request_cancelled()) {
                std::lock_guard<std::mutex> lk(m_mtx);
                std::lock_guard<std::mutex> elk(m_err_mtx);
                m_last_error = "init.mp4: HTTP " + std::to_string(r.http_status) + " " + r.error;
            }
            return 0;
        }
        if (!m_cache->store_init(r.body)) return 0;
    }

    int fetched = 0;
    uint64_t dl = 0, dlfail = 0, ckfail = 0;
    std::string err;
    for (const auto& w : wanted) {
        char name[16];
        std::snprintf(name, sizeof(name), "%08llu",
                      (unsigned long long)w.first);
        auto r = m_tx.get(prefix + "segments/" + name + ".m4s");
        if (!m_tx.last_request_cancelled())
            m_link.observe(link_reachable_result(r.success, r.http_status));
        if (!r.success) {
            // A 404 usually just means "not published yet" — expected at the
            // live edge, so it is not counted as a failure. Neither is our own
            // cancellation: stopping a source aborts whatever is in flight, and
            // counting that as a download failure is how a Stop button came to
            // report itself as a broken connection.
            if (r.http_status != 404 && !m_tx.last_request_cancelled()) {
                ++dlfail;
                err = "segment " + std::to_string(w.first) + ": HTTP " +
                      std::to_string(r.http_status) + " " + r.error;
            }
            continue;
        }
        if (!m_cache->store(w.first, r.body, w.second)) {
            // Checksum mismatch: don't cache it, so the next pass re-fetches
            // rather than feeding corruption to the decoder.
            ++ckfail;
            err = "segment " + std::to_string(w.first) +
                  " failed checksum verification";
            continue;
        }
        ++dl;
        ++fetched;
    }

    // ── commit ───────────────────────────────────────────────────────────────
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_stats.downloaded        += dl;
        m_stats.download_failures += dlfail;
        m_stats.checksum_failures += ckfail;
        if (!err.empty()) {
            std::lock_guard<std::mutex> elk(m_err_mtx);
            m_last_error = err;
        }

        // Bound disk use, but keep plenty of scrub-back room.
        if (m_head_set.load() && m_head.load() > (uint64_t)m_cfg.keep_behind_segments)
            m_cache->prune_below(m_head.load() - (uint64_t)m_cfg.keep_behind_segments);
        if ((int)m_cache->count() > m_cfg.max_cached_segments) {
            const uint64_t lowest = m_cache->lowest_seq();
            const size_t excess =
                m_cache->count() - (size_t)m_cfg.max_cached_segments;
            m_cache->prune_below(lowest + (uint64_t)excess);
        }
    }
    return fetched;
}

// ── Playback ─────────────────────────────────────────────────────────────────
uint64_t DecoderSession::start_reserve_segments() const {
    const double seg = segment_duration_s();
    const uint64_t by_time =
        (uint64_t)std::ceil(std::max(0, m_cfg.start_buffer_seconds) / seg);
    return std::max((uint64_t)std::max(0, m_cfg.prebuffer_segments), by_time);
}

bool DecoderSession::plays_as_recording_locked() const {
    return is_vod(m_room.load()) || !m_pinned_event_id.empty();
}

DecoderSession::StartPlan DecoderSession::start_plan_locked() const {
    StartPlan p;
    const RoomState rs = m_room.load();
    if (rs != RoomState::Live && !is_vod(rs)) return p;      // nothing to plan
    p.known    = true;

    // The seat belongs to the event currently loaded. If what the host wants to
    // play has ALREADY changed — a pin, which poll() has not applied yet — then
    // any answer here is about the old event, and acting on it plays the old
    // head against the new one.
    //
    // That window is up to a whole poll interval wide, and it is exactly what
    // "load another recording and it jumps with random times" was: the dock was
    // told READY for the wrong event, so it showed no loading, left Play
    // enabled, and the press landed on a seat that did not belong to it.
    const std::string wanted =
        m_pinned_event_id.empty() ? m_live_event_id : m_pinned_event_id;
    if (!wanted.empty() && wanted != m_event_id) return p;   // not known: not ready
    p.has_init = m_cache->has_init();

    if (m_head_set.load()) {
        // Already seated: whatever the buffer did to get here, playback is
        // running from a position that exists.
        p.want = m_head.load();
        p.need = p.have = 1;
        return p;
    }

    if (plays_as_recording_locked()) {
        // A recording starts at its beginning, and one segment is the whole
        // requirement. Treating it as live instead sits it `reserve` segments
        // back from an edge — which for a short event is simply "two segments
        // in", and is why Stop-then-Play came back two segments ahead.
        p.want = m_first_available_seq.load();
        p.need = 1;
    } else {
        // Live: sit `reserve` segments behind the edge and wait for that whole
        // window to be contiguous. This is what stops the picture chasing the
        // live edge — it banks a full start-buffer window before the first
        // frame instead of starting a couple of segments behind and stalling.
        const uint64_t reserve = start_reserve_segments();
        p.want = (m_latest_seq.load() > reserve) ? (m_latest_seq.load() - reserve)
                                                 : m_first_available_seq.load();
        p.need = std::max<uint64_t>(1, reserve);
    }
    p.want = std::max(p.want, m_first_available_seq.load());

    const auto idx = m_cache->cached_seqs();
    for (uint64_t s = p.want; idx.count(s); ++s) ++p.have;
    return p;
}

DecoderSession::StartPlan DecoderSession::start_plan() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return start_plan_locked();
}

bool DecoderSession::plays_as_recording() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return plays_as_recording_locked();
}

bool DecoderSession::can_start_now() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return start_plan_locked().ready();
}

double DecoderSession::start_gate_s() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    const StartPlan p = start_plan_locked();
    if (!p.known) return 0.0;
    return (double)p.need * m_segment_duration_s.load();
}

double DecoderSession::ready_buffer_s() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    const StartPlan p = start_plan_locked();
    if (!p.known) return 0.0;
    return (double)p.have * m_segment_duration_s.load();
}

bool DecoderSession::start() {
    std::lock_guard<std::mutex> lk(m_mtx);
    const StartPlan p = start_plan_locked();
    if (!p.ready()) return false;           // buffer still accumulating
    if (!m_head_set.load()) {
        m_head = p.want;
        m_head_set = true;
    }
    m_play = PlayState::Playing;
    return true;
}

void DecoderSession::pause() {
    std::lock_guard<std::mutex> lk(m_mtx);
    // The head stays exactly where it is; pump_downloads keeps filling ahead,
    // so resuming loses nothing. Pausing from Stopped is also honoured: the
    // operator's intent is "hold", and it must not silently no-op just
    // because playback had not begun yet.
    m_play = PlayState::Paused;
}

void DecoderSession::resume() {
    std::lock_guard<std::mutex> lk(m_mtx);
    // Resume from ANY state, not only from Paused. Requiring an exact prior
    // state made resume a silent no-op whenever the state had moved on for
    // some other reason, which left the picture frozen with nothing in the log.
    if (m_head_set.load()) {
        m_play = PlayState::Playing;
    } else {
        // Never started: leave it Stopped so the host's start() path runs and
        // establishes the head properly.
        m_play = PlayState::Stopped;
    }
}

void DecoderSession::jump_to_live() {
    std::lock_guard<std::mutex> lk(m_mtx);
    uint64_t back = (uint64_t)std::max(0, m_cfg.prebuffer_segments);
    uint64_t want = (m_latest_seq.load() > back) ? (m_latest_seq.load() - back)
                                          : m_first_available_seq.load();
    if (!m_head_set.load() || m_head.load() != std::max(want, m_first_available_seq.load())) {
        ++m_discontinuity;
        m_init_sent = false;        // decoder restarts, so it needs init again
    }
    m_head = std::max(want, m_first_available_seq.load());
    m_head_set = true;
    if (m_play == PlayState::Paused) m_play = PlayState::Playing;
}

void DecoderSession::request_init() {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_init_sent = false;
}

bool DecoderSession::seek(uint64_t seq) {
    std::lock_guard<std::mutex> lk(m_mtx);
    // Only within what the store still retains.
    if (seq < m_first_available_seq.load() || seq > m_latest_seq.load()) return false;
    if (!m_head_set.load() || seq != m_head.load()) {
        ++m_discontinuity;
        m_init_sent = false;        // decoder restarts, so it needs init again
    }
    m_head = seq;
    m_head_set = true;
    return true;
}

std::optional<PlayableSegment> DecoderSession::next_segment() {
    // Reading a segment is a multi-megabyte disk read, so it happens with no
    // lock held. Holding the state lock across it blocked the UI every six
    // seconds — which is what made clicking the timeline appear to lock OBS up.
    uint64_t want = 0;
    bool need_init = false;
    int64_t skip_ms = 0;
    PlayableSegment out;

    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_play.load() != PlayState::Playing || !m_head_set.load())
            return std::nullopt;

        // Never serve past the live edge: the head must not run off the end of
        // what the encoder has actually published.
        if (m_head.load() > m_latest_seq.load()) return std::nullopt;

        want = m_head.load();
        if (want < m_first_available_seq.load()) {
            // The encoder itself declared this segment gone — evicted from its
            // local spool under disk pressure before it could ever be
            // uploaded, so no amount of waiting will make it appear. That is
            // a deliberate, encoder-side decision (unlike an ordinary gap,
            // where holding position is correct because the segment may
            // still be coming); treat it the same as a seek, not a stall.
            m_head = m_first_available_seq.load();
            ++m_discontinuity;
            m_stats.gap_skips++;
            want = m_head.load();
            if (want > m_latest_seq.load()) return std::nullopt;
        }
        if (!m_cache->has(want)) {
            // Waiting on a segment: hold position rather than skipping, so
            // nothing is silently dropped from the programme.
            m_stats.gaps_waited++;
            return std::nullopt;
        }

        out.seq = want;
        out.duration_s = segment_duration_s();
        out.event_started_at_ms = m_started_at_ms.load();
        for (const auto& sg : m_manifest.segments) {
            if (sg.seq != want) continue;
            if (sg.duration_s > 0.1) out.duration_s = sg.duration_s;
            if (sg.at_ms > 0) out.starts_at_ms = sg.at_ms;
        }
        if (out.starts_at_ms == 0 && m_started_at_ms.load() > 0) {
            // No at_ms for this segment: it has aged out of the manifest's
            // rolling window. Estimate it, and SAY it is an estimate — this
            // number is what the media clock is pinned to, so an estimate that
            // cannot be distinguished from a measurement puts every displayed
            // time and every cue on arithmetic rather than on the event.
            out.starts_at_ms = m_started_at_ms.load() + (int64_t)want * segment_ms();
            out.starts_at_estimated = true;
        }
        need_init = !m_init_sent;
        // Reading the init segment can fail — most often just after an event
        // change, when the head is ready but init.mp4 is still downloading.
        // Take the skip only once the segment is certain to be served, or a
        // held-back segment loses it.
        skip_ms = m_pending_skip_ms;
    }

    // ── unlocked: the actual disk reads ──────────────────────────────────────
    auto media = m_cache->load(want);
    if (!media) {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_stats.gaps_waited++;
        return std::nullopt;
    }
    out.media = std::move(*media);

    std::vector<uint8_t> init;
    if (need_init) {
        if (auto i = m_cache->load_init()) init = std::move(*i);
        if (init.empty()) {
            // The decoder needs the init segment and it is not there yet.
            // Serving the fragment anyway hands the caller something it cannot
            // decode, and advancing the head throws that segment away for
            // good — programme silently lost while init.mp4 downloads. Hold
            // position, exactly as for a segment that has not arrived.
            std::lock_guard<std::mutex> lk(m_mtx);
            m_stats.gaps_waited++;
            return std::nullopt;
        }
    }

    // ── commit ───────────────────────────────────────────────────────────────
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        // Re-check: the position may have moved while the file was read (a
        // seek, or a jump to live). If so, discard this one rather than
        // serving content from the old position.
        if (m_head.load() != want) return std::nullopt;
        if (need_init) {
            out.init = std::move(init);
            m_init_sent = true;
        }
        out.skip_to_ms = skip_ms;
        m_pending_skip_ms = 0;          // applies to this segment only
        ++m_head;
        m_stats.served++;
    }
    return out;
}

std::string DecoderSession::last_error() const {
    std::lock_guard<std::mutex> lk(m_err_mtx);
    return m_last_error;
}

std::vector<Marker> DecoderSession::markers() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_markers.markers;
}

bool DecoderSession::jump_to_marker(const std::string& marker_id) {
    uint64_t target = 0;
    int64_t  at = 0;
    bool found = false;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        for (const auto& mk : m_markers.markers)
            if (mk.id == marker_id) { target = mk.seq; at = mk.at_ms; found = true; break; }
    }
    if (!found) return false;

    // Land where the cue was PLACED, not at the start of the segment it happened
    // to fall in.
    //
    // A cue is dropped at a moment — a word, a beat — and the marker records
    // that moment (`at_ms`) as well as the segment it landed in. Seeking to the
    // segment alone put the picture up to a segment early, which is what
    // "selecting a cue doesn't land exactly on the right time" was: the exact
    // position was in hand and being thrown away.
    //
    // The sub-segment machinery already exists for time seeks — segments are the
    // unit of TRANSFER, not of seeking — so a cue is simply a time someone
    // chose. The segment seek remains the fallback for a cue carrying no time.
    if (at > 0 && seek_to_wall_ms(at) != 0) return true;
    return seek(target);          // seek() bounds-checks and raises a jump
}

std::string DecoderSession::current_event_id() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_event_id;
}

bool DecoderSession::add_cue(const std::string& label, std::string& error,
                             uint64_t operator_seq, int64_t operator_at_ms) {
    // One line per cue, set or not. The encoder has always logged its own;
    // a site's cue left nothing, so the log could not say a cue was dropped,
    // which route it took, or why one was refused (seen live 2026-09-24).
    std::string said;
    const bool ok = add_cue_impl(label, error, operator_seq, operator_at_ms, said);
    if (ok) log_info("cue \"%s\" set — %s", label.c_str(), said.c_str());
    else    log_warn("cue \"%s\" not saved — %s", label.c_str(), error.c_str());
    return ok;
}

bool DecoderSession::add_cue_impl(const std::string& label, std::string& error,
                                  uint64_t operator_seq, int64_t operator_at_ms,
                                  std::string& said) {
    std::string author;
    std::string prefix;
    std::string event_id;
    uint64_t    seq = 0;
    bool        have_hub = false;
    std::function<CueTarget(const std::string&)> cue_target;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        author = m_cfg.author_name;
        if (author.empty()) { error = "no site name is set"; return false; }
        if (!m_cfg.can_author_cues) {
            error = "this box is set to receive only";
            return false;
        }
        if (m_event_id.empty()) { error = "no event is loaded"; return false; }
        prefix = event_prefix();
        // Where the operator is WATCHING, not the live edge.
        //
        // On a recorded event the live edge IS the end of it, so stamping the
        // live edge put every cue dropped while watching a past event at the
        // end. "Here" is the playhead — which is also right live, where a
        // campus sitting behind the live edge means its own position and not
        // the encoder's. Not yet positioned (nothing served) falls back to the
        // live edge, which is the best answer there is.
        seq = m_head_set.load() ? m_head.load() : m_latest_seq.load();

        if (operator_seq > 0) {
            // The host said which segment is on screen. Exact, and independent
            // of the media clock — which is re-pinned to a fresh offset when
            // playback jumps, so a clock reading names a place inconsistently.
            seq = operator_seq;
        } else if (operator_at_ms > 0) {
            // Otherwise a wall time, when one is offered: the segment that time
            // falls IN — the greatest start at or before it — falling back to
            // the event's own arithmetic when the rolling manifest does not
            // reach back that far.
            uint64_t best = 0;
            bool found = false;
            for (const auto& s : m_manifest.segments) {
                if (s.at_ms > 0 && s.at_ms <= operator_at_ms &&
                    (!found || s.seq > best)) {
                    best = s.seq;
                    found = true;
                }
            }
            if (found) {
                seq = best;
            } else {
                const int64_t started = m_started_at_ms.load();
                const int64_t dur_ms = segment_ms();   // measured, see BUGS #2b
                if (started > 0 && dur_ms > 0 && operator_at_ms > started)
                    seq = (uint64_t)((operator_at_ms - started) / dur_ms);
            }
        }
        have_hub = static_cast<bool>(m_cfg.cue_hub);
        cue_target = m_cfg.cue_target;
        event_id = m_event_id;
    }

    // The wall-clock time of the CONTENT at that position, so a cue set on a
    // recording carries the event's own time and not today's. Live, this is
    // within a segment of now, which is what the encoder's own cues carry.
    int64_t at_ms = operator_at_ms > 0 ? operator_at_ms : wall_clock_ms(seq);
    if (at_ms <= 0) at_ms = now_ms();

    // MULTISITE CLOUD: the collector's cue credential writes exactly the key it
    // names (cue_credentials.h). Asked for first; without one, the LAN hub if
    // there is one, and otherwise a plain refusal — never the read-only
    // transport, whose refused put would be reported as the bucket failing.
    std::shared_ptr<Transport> cue_writer;
    std::string cue_key;
    if (cue_target) {
        CueTarget t = cue_target(event_id);
        if (t.tx) {
            // The collector chose the key; check it is this event's before
            // trusting a write to it. cloud_parse_cue_credentials refused any
            // other shape already, so this is belt and braces.
            if (t.object_key.compare(0, prefix.size() + 5, prefix + "cues/") != 0) {
                error = "the cue permission names a file outside this event";
                return false;
            }
            cue_writer = std::move(t.tx);
            cue_key = t.object_key;
        } else if (!have_hub) {
            error = "Cue not saved: " + (t.why.empty()
                ? std::string("this player has no permission to write cues yet")
                : t.why) + ".";
            return false;
        }
        // else: no writer, but a hub — fall through to it.
    }

    // A LAN satellite hands the cue to the encoder, which owns the event and
    // writes it under this site's name — so the box stays read-only and needs
    // no bucket credentials at all.
    if (!cue_writer && have_hub) {
        std::string merged_json;
        if (!m_cfg.cue_hub(author, label, merged_json, error)) return false;
        MarkerList merged;
        bool parsed = false;
        try {
            merged = MarkerList::from_json(merged_json);
            parsed = true;
        } catch (...) {
            // The hub accepted the cue but its list came back unreadable. The
            // cue is set; keep what we had rather than blanking the list.
        }
        std::lock_guard<std::mutex> lk(m_mtx);
        if (parsed) m_markers = std::move(merged);
        said = "handed to the encoder's LAN hub, as " + author;
        return true;
    }

    const std::string key = cue_writer
        ? cue_key
        : prefix + "cues/" + cue_author_token(author) + ".json";

    // Read this box's OWN cue object, append, write it back — never anyone
    // else's. That is the property the per-author layout exists to give: this
    // read-modify-write cannot race another site's cue, so no cue is ever
    // clobbered by a second author.
    MarkerList mine;
    auto g = m_tx.get(key);
    if (!m_tx.last_request_cancelled())
        m_link.observe(link_reachable_result(g.success, g.http_status), now_ms());
    if (g.success) {
        try {
            mine = MarkerList::from_json(std::string(g.body.begin(), g.body.end()));
        } catch (...) {
            // An unreadable file of our own is replaced, not appended to.
        }
    }
    Marker mk;
    mk.seq    = seq;                     // the playhead, clamped to the event
    mk.at_ms  = at_ms;
    mk.type   = "cue";
    mk.label  = label;
    mk.id     = make_event_id(mk.at_ms);
    mk.author = author;
    mine.markers.push_back(mk);

    // The read above went through m_tx, the read-only transport, in every
    // case. The put goes through the cue credential when there is one.
    const std::string body = mine.to_json();
    Transport& writer = cue_writer ? *cue_writer : m_tx;
    auto p = writer.put(key, std::vector<uint8_t>(body.begin(), body.end()),
                        "application/json", {});
    if (!p.success) {
        error = p.error.empty() ? "the cue could not be written" : p.error;
        return false;
    }

    said = "segment " + std::to_string(seq) + ", written to " + key +
           (cue_writer ? " with this event's cue permission" : "") +
           ", as " + author;

    // Show it here at once, rather than up to a poll later.
    std::lock_guard<std::mutex> lk(m_mtx);
    m_markers = merge_markers({ m_markers, mine });
    return true;
}

std::optional<Marker> DecoderSession::current_marker() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    std::optional<Marker> best;
    const uint64_t head = m_head.load();
    for (const auto& mk : m_markers.markers)
        if (mk.seq <= head && (!best || mk.seq >= best->seq)) best = mk;
    return best;
}

std::vector<AudioTrack> DecoderSession::audio_layout() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_manifest.audio_tracks;
}

TileLayout DecoderSession::video_layout() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_manifest.video.layout;
}

uint64_t DecoderSession::discontinuity_id() const {
    return m_discontinuity.load();      // read by the feed loop constantly
}

// The estimate needs no lock at all, and is exact whenever segments are evenly
// spaced. Only try the manifest for a precise value, and use try_lock so a UI
// query is never blocked by the download thread.
int64_t DecoderSession::segment_ms() const {
    const int64_t measured = m_measured_segment_ms.load();
    if (measured > 0) return measured;
    return (int64_t)(segment_duration_s() * 1000.0);
}

int64_t DecoderSession::wall_clock_ms(uint64_t seq) const {
    const int64_t started = m_started_at_ms.load();
    if (std::unique_lock<std::mutex> lk(m_mtx, std::try_to_lock); lk.owns_lock()) {
        for (const auto& s : m_manifest.segments)
            if (s.seq == seq && s.at_ms > 0) return s.at_ms;
    }
    if (started <= 0) return 0;
    // Measured, not nominal: `seq * 6000` against a real 6067 is 1.11% and
    // about forty seconds by the end of an hour (BUGS #2b).
    return started + (int64_t)seq * segment_ms();
}


bool DecoderSession::at_end() const {
    return m_head_set.load() && m_head.load() > m_latest_seq.load();
}

int64_t DecoderSession::playhead_media_ms() const {
    if (!m_head_set.load()) return 0;
    const int64_t t = media_ms_for_seq(m_head.load());
    // Once playback runs past the last segment the head points at a position
    // that does not exist, and the reported position ran beyond the end of the
    // recording. Clamp it.
    const int64_t end = end_media_ms();
    if (end > 0 && t > end) return end;
    return t;
}

int64_t DecoderSession::live_media_ms() const {
    return media_ms_for_seq(m_latest_seq.load());
}

int64_t DecoderSession::earliest_media_ms() const {
    return media_ms_for_seq(m_first_available_seq.load());
}

int64_t DecoderSession::end_media_ms() const {
    return media_ms_for_seq(m_latest_seq.load()) + segment_ms();
}




int64_t DecoderSession::event_started_ms() const {
    return m_started_at_ms.load();
}

double DecoderSession::behind_live_s() const {
    // Lock-free: read by the UI several times a second.
    if (!m_head_set.load()) return 0.0;
    const uint64_t head = m_head.load(), live = m_latest_seq.load();
    if (live < head) return 0.0;
    return (double)(live - head) * (double)segment_ms() / 1000.0;
}

std::vector<std::pair<uint64_t, uint64_t>> DecoderSession::cached_ranges() const {
    auto seqs = m_cache->cached_seqs();
    std::vector<std::pair<uint64_t, uint64_t>> out;
    for (uint64_t s : seqs) {
        if (!out.empty() && s == out.back().second + 1) out.back().second = s;
        else out.push_back({ s, s });
    }
    return out;
}

// Seek by TIME, which is how an operator thinks. Finds the segment containing
// the requested moment and records how far into it to start, so accuracy is not
// limited to the segment boundary.
int64_t DecoderSession::seek_to_media_ms(int64_t media_ms) {
    if (media_ms < 0) media_ms = 0;
    // MEASURED, not nominal. This decides which segment a click lands on, and
    // using the configured value here put every seek late in proportion to how
    // far into the event it was: the segment picked is media_ms / seg_ms, so an
    // error of d ms per segment lands seq * d late. Measured on a recording
    // whose segments are really 6033 ms against a nominal 6000 — a click at
    // 269.9 s (seq 44) landed 1539 ms late, predicted 44 * 33 = 1474; a click
    // at 1205.9 s (seq 200) landed 6710 ms late, predicted 200 * 33 = 6700.
    //
    // It also broke the arrival indication, which is how it was found: the
    // check allows 2.5 s between where playback reached and where it was sent,
    // and a seek near the end of an hour overshot by more than that, so
    // "SEEKING..." never cleared however long the picture had been playing.
    //
    // segment_ms() takes the figure from the encoder's own recorded times. See
    // BUGS #2b — this is the fifth place the same derivation was written.
    const int64_t seg_ms = segment_ms();
    if (seg_ms <= 0) return 0;

    const uint64_t seq  = (uint64_t)(media_ms / seg_ms);
    const int64_t  skip = media_ms - (int64_t)seq * seg_ms;

    // Seat the head and set the offset under ONE lock, the same way seek() sets
    // the head: the feed loop takes this lock to serve the next segment, so a
    // head that moved before its skip was set could be served from the start of
    // the segment — the very fault this is fixing, reintroduced by a race.
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (seq < m_first_available_seq.load() || seq > m_latest_seq.load()) {
            std::lock_guard<std::mutex> elk(m_err_mtx);
            m_last_error = "that moment is outside what storage still holds";
            return 0;
        }
        if (!m_head_set.load() || seq != m_head.load()) {
            ++m_discontinuity;
            m_init_sent = false;        // decoder restarts, so it needs init again
        }
        m_head = seq;
        m_head_set = true;
        m_pending_skip_ms = skip;
    }
    return media_ms;
}

int64_t DecoderSession::seek_to_wall_ms(int64_t wall_ms) {
    uint64_t target = 0;
    int64_t  seg_start = 0;
    // Five different things make a seek impossible, and the hosts reported all
    // of them as "that moment is no longer available in storage". For a seek
    // past the end of a recording — which is what an operator scrubbing near
    // the right-hand edge actually hits — that sentence is not merely vague
    // but wrong: nothing has been removed. Say which bound was hit, here,
    // where it is known.
    auto fail = [this](const char* why) -> int64_t {
        std::lock_guard<std::mutex> elk(m_err_mtx);
        m_last_error = why;
        return 0;
    };
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_manifest.started_at_ms <= 0)
            return fail("nothing is loaded yet, so there is no timeline to "
                        "move within");
        // Measured, not nominal: this places a position, and the nominal is a
        // request to the encoder rather than a description of what it made.
        // See segment_ms() and BUGS #2b.
        const int64_t seg_ms_v = segment_ms();

        // Prefer an exact match from the manifest window.
        bool found = false;
        for (const auto& s : m_manifest.segments) {
            if (s.at_ms <= 0) continue;
            const int64_t end = s.at_ms + (int64_t)(s.duration_s * 1000.0);
            if (wall_ms >= s.at_ms && wall_ms < end) {
                target = s.seq; seg_start = s.at_ms; found = true; break;
            }
        }
        if (!found) {
            // Outside the window: derive from the event start.
            const int64_t offset = wall_ms - m_manifest.started_at_ms;
            if (offset < 0)
                return fail("that is before this event started");
            target = (uint64_t)(offset / seg_ms_v);
            seg_start = m_manifest.started_at_ms + (int64_t)target * seg_ms_v;
        }
        // The two ends are different problems and must not read the same. The
        // floor has genuinely gone — retention removed it. The ceiling has
        // not: it is simply the end of what exists so far.
        if (target < m_first_available_seq.load())
            return fail("that moment has passed out of storage — it is older "
                        "than the retention rule keeps");
        if (target > m_latest_seq.load())
            return fail(is_vod(m_room.load())
                            ? "that is past the end of this recording"
                            : "that is ahead of what has been broadcast yet");
    }

    if (!seek(target))                        // seek() raises the discontinuity
        return fail("that moment is not available to play");
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_pending_skip_ms = wall_ms - seg_start;
        if (m_pending_skip_ms < 0) m_pending_skip_ms = 0;
    }
    return wall_ms;
}

double DecoderSession::buffered_ahead_s() const {
    // Lock-free apart from one copy of the cache index. The original version
    // called has() up to ten thousand times — each a filesystem check — from
    // the UI thread.
    if (!m_head_set.load()) return 0.0;
    const uint64_t head = m_head.load();
    const double seg = segment_duration_s();
    const auto idx = m_cache->cached_seqs();
    int n = 0;
    for (uint64_t s = head; idx.count(s); ++s) ++n;
    return (double)n * seg;
}

} // namespace multisite
