// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// decoder_session.h — the satellite's receive-and-play controller.
//
// Implements timeslipping: the PLAYBACK HEAD is independent of the LIVE EDGE.
// Downloads run ahead into the local cache regardless of where playback sits,
// so a campus can pause (to hold for its own welcome), sit deliberately behind
// live, scrub backwards, or jump to live — without losing anything.
//
// Deliberately driven by explicit calls (poll / pump_downloads / advance)
// rather than hidden threads, so the whole state machine is testable
// deterministically. The OBS layer wraps this in threads.
//
#include "model.h"
#include "segment_cache.h"
#include "transport.h"
#include "link_health.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace multisite {

// NOTE: the OBS layer carries this across as an int, so new states are
// appended rather than inserted.
enum class RoomState {
    Unknown,    // nothing fetched yet
    Offline,    // nothing to play: no live event, or nothing could be fetched
    Live,       // event is live and updating
    Ended,      // the encoder finished cleanly
    Interrupted,// the encoder died mid-event: still marked live, not advancing
};

// Whether a state means "this event is not growing any more, so play it as
// video-on-demand". A recording and an interrupted event differ in how they
// should be DESCRIBED, not in how they play.
inline bool is_vod(RoomState s) {
    return s == RoomState::Ended || s == RoomState::Interrupted;
}

enum class PlayState { Stopped, Playing, Paused };

struct DecoderConfig {
    std::string room_id = "main-auditorium";
    std::string cache_dir;
    // This box's own name — "Campus B", "Main site" — stamped on every cue it
    // drops, so every other site can see who set one. Empty disables cue
    // authoring: the dock says so rather than failing on a click.
    std::string author_name;
    // Whether this box may write a cue. True when the transport can accept a
    // write (a scoped cloud credential) or a LAN hub is configured; a
    // read-only satellite leaves it false.
    bool        can_author_cues = false;
    // Publishes a cue when this box has no bucket of its own — a LAN satellite,
    // where the encoder accepts it on the satellite's behalf (see
    // LanObjectServer / Session::add_cue_from). Called INSTEAD of writing the
    // bucket directly, and returns the hub's merged cue list as JSON. Empty
    // means this box writes its own cue object.
    std::function<bool(const std::string& author, const std::string& label,
                       std::string& merged_json, std::string& error)> cue_hub;
    // Segments to buffer before playback starts. Higher = more resilient.
    int    prebuffer_segments = 2;
    // Seconds of programme that must be banked (contiguously cached) before
    // playback starts, so the picture never has to chase the live edge. The
    // buffer accumulates for this long first, instead of starting a couple of
    // segments behind live and stalling after one. 0 restores "start as soon
    // as the first segment is ready".
    int    start_buffer_seconds = 60;
    // How far ahead of the playhead to keep downloading, in MINUTES of
    // programme. This is the reliability figure that matters: it is how long
    // the campus could keep broadcasting if its connection died. Downloading
    // runs as fast as the link allows until this much is banked, rather than
    // trickling along at playback speed — which is what made jumping back
    // twenty minutes appear not to buffer at all.
    int    buffer_minutes = 10;
    // Hard ceiling, so a small disk cannot be filled by a long buffer.
    int    max_cached_segments = 2000;
    // Keep this many segments behind the head on disk (scrub-back room).
    int    keep_behind_segments = 200;
    // Treat the room as no-longer-live if the manifest hasn't updated within
    // this.
    int    stale_after_ms = 600000;      // 10 minutes
    int    max_download_retries = 5;
    // Hold an event that has finished while it was being watched, rather than
    // following live.json on to whatever the room does next. On by default,
    // because being pulled out of a recording someone is part-way through is
    // worse than being told the next event has started — the operator gets the
    // offer (live_elsewhere) and presses Back to live. An unattended box that
    // exists to relay the room continuously wants the opposite, so the
    // appliance turns this off.
    bool   hold_finished_event = true;
    // Play this specific event instead of whatever live.json names. Set when
    // an operator picks a past event from the event list; empty means
    // "follow the room", which is the live behaviour.
    std::string pinned_event_id;
};

// A decoded-ready fragment handed to the host: init + media, in order.
struct PlayableSegment {
    uint64_t seq = 0;
    double   duration_s = 6.0;
    std::vector<uint8_t> init;      // only populated on the first hand-off
    std::vector<uint8_t> media;
    // Wall-clock time of the start of this segment, so the host can report the
    // playing time precisely rather than per-segment.
    int64_t  starts_at_ms = 0;
    // Milliseconds into this segment at which playback should begin. Set after
    // a seek to a time that falls mid-segment; the host drops earlier frames.
    // Segments are the unit of transfer, but they need not be the unit of
    // seeking.
    int64_t  skip_to_ms = 0;
};

class DecoderSession {
public:
    DecoderSession(DecoderConfig cfg, Transport& transport);

    // ── Discovery ────────────────────────────────────────────────────────────
    // Fetch live.json + manifest.json. Call periodically (e.g. every 3s).
    // `now_ms_override` exists for deterministic tests.
    RoomState poll(int64_t now_ms_override = 0);

    RoomState room_state() const { return m_room.load(); }
    const std::string& event_id() const { return m_event_id; }

    // ── Pinning ──────────────────────────────────────────────────────────────
    // Play one specific event and stop following the room. Pinning an event
    // that is not the live one is how a past event is watched.
    //
    // A pinned session deliberately does NOT switch when a new event starts:
    // being yanked out of a recording someone is watching, because a rehearsal
    // began in the room, would be far worse than staying put. live_elsewhere()
    // reports that something is on air so the host can offer the jump instead
    // of taking it.
    void pin_event(const std::string& event_id);
    void unpin();                       // follow the room again
    std::string pinned_event() const;
    bool is_pinned() const;

    // The event live.json currently names, whether or not it is being played.
    std::string live_event_id() const;
    // True when the room is live but a different event is pinned.
    bool live_elsewhere() const;
    // Why the last operation failed. Returned by value under its own small
    // lock, so it never contends with the download path.
    std::string last_error() const;

    // ── Downloading ──────────────────────────────────────────────────────────
    // Fetch up to `max` missing segments in the download-ahead window.
    // Returns how many were newly cached. Runs independently of playback, so
    // the cache keeps filling while paused.
    int pump_downloads(int max = 4);

    // ── Playback (timeslipping) ──────────────────────────────────────────────
    bool start();                      // begins once prebuffer is satisfied

    // ── Where playback would begin, and what it is waiting for ───────────────
    // ONE computation, shared by start() and by everything the operator is
    // shown. The dock used to decide "ready" from the start-buffer setting and
    // got it wrong for recordings, which need only their first segment — two
    // answers to one question, and the display's was the wrong one.
    struct StartPlan {
        // "Geometry is meaningful" — live, or a finished recording. Kept apart
        // from readiness on purpose: the gate and the buffered figure are worth
        // showing from the moment an event is resolved, before the init segment
        // has even landed, which is exactly when an operator starts watching.
        bool     known    = false;
        bool     has_init = false;
        uint64_t want     = 0;    // the segment playback would begin at
        uint64_t need     = 0;    // how many contiguous segments that requires
        uint64_t have     = 0;    // how many are actually present from `want`
        bool ready() const { return known && has_init && have >= need; }
    };
    StartPlan start_plan() const;
    // The same answer, in the units an operator reads.
    bool   can_start_now() const;
    double start_gate_s() const;      // what the gate wants, in seconds
    double ready_buffer_s() const;    // what is present towards it, in seconds
    void pause();                      // freezes the head; cache keeps filling
    void resume();                      // continues from the paused position
    void jump_to_live();               // snap the head to the live edge

    // "I threw the decoder away; the next segment must carry the init segment
    // again." A fresh decoder cannot decode a bare fragment, and the session
    // is the only thing that knows whether it has already handed the init out.
    void request_init();
    bool seek(uint64_t seq);           // move within what's retained

    // Hand the next segment to the decoder, if one is due and cached.
    std::optional<PlayableSegment> next_segment();

    PlayState  play_state()  const { return m_play.load(); }
    uint64_t   playback_head() const { return m_head.load(); }
    uint64_t   live_edge()    const { return m_latest_seq.load(); }
    uint64_t   earliest_available() const { return m_first_available_seq.load(); }

    // ── Markers ──────────────────────────────────────────────────────────────
    // Cues published by the main site (markers.json), refreshed on poll.
    std::vector<Marker> markers() const;

    // Drop a cue with an operator-typed name at the current live edge. Writes
    // only THIS box's own cue object (one writer per object, so no other
    // site's cues can be clobbered) and reflects it locally at once, rather
    // than making the dock wait for the next poll. Returns false with `error`
    // set when there is no site name, no live event, or the write fails.
    // `operator_seq` is the media segment the host is SHOWING. That is the
    // honest answer to "here", and the only reliable one after a seek: the
    // media clock is re-pinned to a fresh offset when playback jumps, so a
    // clock reading names a place inconsistently. `operator_at_ms` is a wall
    // time, used when the host has no segment number; 0 for both means the
    // session's own head.
    bool add_cue(const std::string& label, std::string& error,
                 uint64_t operator_seq = 0, int64_t operator_at_ms = 0);

    // Move playback to a marker. Returns false if the marker is unknown or its
    // segment is no longer retained.
    bool jump_to_marker(const std::string& marker_id);

    // The marker at or before the playback head — i.e. "where are we in the
    // event", for display.
    std::optional<Marker> current_marker() const;

    // Increments whenever playback jumps (seek, jump-to-live, event change).
    // The host must tear down and restart its decoder when this changes: the
    // next fragment will carry an unrelated baseMediaDecodeTime, which would
    // otherwise decode as out-of-order timestamps and a glitched picture.
    uint64_t discontinuity_id() const;

    // What the main site says its audio contains: track labels and, for a
    // packed multi-channel feed, what each channel carries. Without this the
    // names the encoder operator typed would never be seen by anyone.
    std::vector<AudioTrack> audio_layout() const;

    // How the encoder composited this feed. 1x1 — one whole picture — for every
    // event that predates tiling and for every room that sends one camera,
    // because an absent field parses to that.
    TileLayout video_layout() const;

    // A finished recording behaves as video-on-demand: it has an end, a
    // duration, and a position within it — "behind live" is meaningless.
    bool    event_ended() const { return is_vod(m_room.load()); }
    // Distinguishes the two ways an event stops: ended cleanly, or the encoder
    // died. Both play; only the wording differs.
    bool    was_interrupted() const { return m_room.load() == RoomState::Interrupted; }
    // Whether this event was seen LIVE at any point since it was loaded.
    // "The broadcast just ended" and "this is a recording of a past event"
    // are different things to an operator, and only this distinguishes them.
    bool    was_live_this_session() const { return m_saw_live.load(); }
    // Wall-clock time just past the last frame of the recording.
    int64_t end_wall_ms() const;
    // True once playback has run past the last segment there is.
    bool    at_end() const;

    // Wall-clock time of a position in the programme. Uses the exact time
    // recorded for a segment when it is still in the manifest window, and
    // estimates from the event start otherwise. 0 if unknown.
    int64_t wall_clock_ms(uint64_t seq) const;
    int64_t playhead_wall_ms() const;
    int64_t live_wall_ms() const;
    int64_t earliest_wall_ms() const;
    int64_t event_started_ms() const;

    // How far behind live the campus currently is, in seconds.
    double behind_live_s() const;
    // Nominal segment length. Needed by callers that reason in wall time about
    // where the live edge is, since the newest segment's content runs to its
    // start plus this.
    double segment_duration_s() const { return m_segment_duration_s.load(); }

    // Seconds of contiguous cached content ahead of the head — i.e. how long
    // playback could continue with no network at all.
    double buffered_ahead_s() const;

    // The configured start gate: seconds of programme that must be on disk
    // before Play will go to air. Exposed so the UI can show the buffer filling
    // against a real target rather than an open-ended spinner.
    int start_buffer_seconds() const { return m_cfg.start_buffer_seconds; }

    // The contiguous cached ranges, as [first,last] sequence pairs, so a UI can
    // draw what is actually on disk rather than approximate it.
    std::vector<std::pair<uint64_t, uint64_t>> cached_ranges() const;

    // Seek to a wall-clock time. Returns the exact position reached, or 0 if
    // the time is outside what storage still retains. Sub-segment accuracy is
    // handled by the host: `skip_to_ms` in the served segment tells it how far
    // into that segment to begin.
    int64_t seek_to_wall_ms(int64_t wall_ms);

    // Seek by MEDIA time — segment number times one segment, which is the axis
    // the decoder dock draws and the axis a click on it is in. Returns the
    // media time reached, or 0 if it is outside what storage still holds.
    //
    // The dock used to convert a click to a segment number itself and throw the
    // remainder away, so every click landed up to one segment (6 s) from where
    // it was made. Segments are the unit of TRANSFER, not of seeking: the
    // in-segment offset is set here and applied by the host as `skip_to_ms`.
    int64_t seek_to_media_ms(int64_t media_ms);

    const SegmentCache& cache() const { return *m_cache; }

    struct Stats {
        uint64_t downloaded = 0;
        uint64_t download_failures = 0;
        uint64_t checksum_failures = 0;
        uint64_t served = 0;
        uint64_t gaps_waited = 0;
        // Segments the encoder itself declared permanently gone (dropped from
        // its local spool under disk pressure) that playback skipped past,
        // rather than waiting on forever. Distinct from gaps_waited, which is
        // the ordinary "not published yet" case.
        uint64_t gap_skips = 0;
    };
    const Stats& stats() const { return m_stats; }

    // ── Connection health ────────────────────────────────────────────────────
    // Whether the store has been answering, as measured from this session's own
    // poll and download traffic. Distinct from room_state(): an empty room with
    // a good connection reads Healthy + Offline, while a dead link reads
    // Offline + Offline. This is the figure an operator acts on mid-event.
    LinkHealth link_health() const { return m_link.health(); }
    // True once the session has observed at least one request, so a UI can
    // tell "measured healthy" from "nothing to report yet".
    bool       link_known() const { return m_link.known(); }
    // Wall-clock ms of the last link-state change (0 if never changed).
    int64_t    link_changed_ms() const { return m_link.last_change_ms(); }

private:
    DecoderConfig m_cfg;
    Transport&    m_tx;
    std::unique_ptr<SegmentCache> m_cache;

    std::string m_event_id;
    std::string m_pinned_event_id;     // guarded by m_mtx
    // Set once when the event being watched is seen to finish without having
    // been pinned, so that the next event starting does not steal the playback.
    // Cleared when the loaded event changes and set by unpin(), so it fires
    // exactly at the moment of ending and never fights an operator who has
    // deliberately asked to follow the room again.
    bool        m_end_hold_done = false;
    std::string m_live_event_id;       // what live.json last named
    std::atomic<bool> m_room_is_live{false};   // …and whether it was advancing
    std::atomic<RoomState> m_room{RoomState::Unknown};
    std::atomic<PlayState> m_play{PlayState::Stopped};
    std::string m_last_error;
    mutable std::mutex m_err_mtx;      // guards m_last_error only

    Manifest   m_manifest;
    MarkerList m_markers;
    int64_t    m_markers_checked_ms = 0;
    int64_t    m_manifest_updated_ms = 0;

    // The values the UI reads many times a second are atomics, not
    // mutex-protected fields.
    //
    // Keeping the lock off the network path was not enough on Windows: the
    // download thread acquires the lock in a tight loop, and Windows mutexes
    // favour the thread already running, so the UI thread was starved for
    // seconds at a time (measured at 12.6 s). Reads that only need a scalar
    // now take no lock at all, which removes the contention rather than
    // hoping to win the race for it.
    std::atomic<uint64_t> m_latest_seq{0};
    std::atomic<uint64_t> m_first_available_seq{0};
    std::atomic<double>   m_segment_duration_s{6.0};
    std::atomic<int64_t>  m_started_at_ms{0};
    std::atomic<bool>     m_saw_live{false};
    // The write-side failover case (PROJECT-SCOPE.md §10 Phase 9): the target
    // being read has stopped advancing, but it still answers 200, so no
    // per-request fallback would ever fire. Set by poll() when it sees a
    // stalled manifest, acted on by the NEXT poll — which then reads the other
    // end and finds out whether that was the right call. Once only: a genuinely
    // stalled event must not flap between ends.
    std::atomic<bool>     m_manifest_stalled{false};
    std::atomic<bool>     m_switched_target{false};
    std::atomic<uint64_t> m_head{0};
    std::atomic<bool>     m_head_set{false};
    bool     m_init_sent = false;
    std::atomic<uint64_t> m_discontinuity{0};
    int64_t  m_pending_skip_ms = 0;   // applied to the next served segment

    Stats m_stats;
    LinkTracker m_link;
    mutable std::mutex m_mtx;

    std::string event_prefix() const;
    std::string segment_key(uint64_t seq) const;
    std::string checksum_for(uint64_t seq) const;
    // How many segments the start gate requires ahead of the playhead: the
    // larger of the prebuffer cushion and the start_buffer_seconds window,
    // converted at the current segment duration.
    uint64_t start_reserve_segments() const;
    // The plan, with m_mtx already held — start() reads it and then seats the
    // head, so it cannot take the lock itself.
    StartPlan start_plan_locked() const;
    // Whether the event loaded now is played as a RECORDING: a cleanly finished
    // one, an interrupted one, or anything PINNED — a chosen past event is a
    // recording whatever its manifest's status says. One answer, because the
    // start position, the download window and the timeline all have to agree
    // about it and did not.
    bool plays_as_recording_locked() const;

};

} // namespace multisite
