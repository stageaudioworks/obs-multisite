// SPDX-License-Identifier: GPL-3.0-or-later
// multisite_source.cpp — the satellite (receive) side, as an OBS source.
//
// Threads:
//   poll_loop     — refreshes live.json/manifest.json, drives download-ahead
//   feed_loop     — hands cached fragments to the decoder in order
//   decode        — inside CmafDecoder (its own thread), emits frames
// video_tick does nothing heavy; frames are pushed to OBS from the decoder
// callbacks with timestamps on OBS's clock, so OBS's async buffering paces
// playout.
//
// Timeslipping controls (Pause / Resume / Jump to live) live in the source
// properties for now; the Qt dock comes in Phase 5.
//
#include <obs-module.h>
#include <media-io/video-io.h>
#include <media-io/audio-io.h>
#include <util/platform.h>

#include "plugin_log.h"
#include "storage_secondary.h"

#include "../core/mirror_read_transport.h"
#include "multisite_ui.h"
#include "decoder_settings.h"

#include "../core/decoder_session.h"
#include "../core/event_catalog.h"
#include "../core/cmaf_decoder.h"
#include "../core/playout_clock.h"
#include "../core/position_interp.h"
#include "../core/playout_timeline.h"

#ifdef MULTISITE_HAVE_FRONTEND_API
#include <obs-frontend-api.h>
#endif
#include "../core/s3_transport.h"
#include "../core/lan_transport.h"
#include "../core/fallback_transport.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <chrono>
#include <ctime>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace multisite_obs {

using namespace multisite;

// setting keys
static constexpr char S_ENDPOINT[] = "endpoint_host";
static constexpr char S_ACCOUNT[]  = "r2_account_id";
static constexpr char S_BUCKET[]   = "bucket";
static constexpr char S_KEYID[]    = "access_key_id";
static constexpr char S_SECRET[]   = "secret_access_key";
static constexpr char S_REGION[]   = "region";
// Still read — never written — for a scene saved before the feed name moved to
// the machine-wide decoder settings; see the one-time migration in src_update.
static constexpr char S_ROOM[]     = "room_id";
static constexpr char S_ATRACK[]   = "audio_track";   // 0-based
static constexpr char S_TILE[]     = "tile_index";    // 0-based, reading order
// Which output a tile is sent to, if any. 0 means "none" so that adding a tile
// source never takes over a screen on its own — an operator who has not asked
// for a projector should not get one.
static constexpr char S_TILEOUT[]  = "tile_output";

// One entry in the delivery queue: either a video or an audio frame, already
// stamped with its OBS presentation time.
struct PendingFrame {
    bool     is_video = true;
    uint64_t timestamp = 0;
    // The timeline this frame's TIMESTAMP was computed against, stamped when
    // the frame is built and never afterwards.
    //
    // It has to travel WITH the frame. The delivery loop used to read the
    // current epoch at pop time and compare it against the current epoch — the
    // same atomic, loaded twice, microseconds apart — so the comparison could
    // not fail and the staleness check it was written to perform did not
    // happen. A frame built before a hold, carrying a timestamp from the
    // pre-hold playout base, was therefore handed straight through: it arrives
    // tens of seconds past due, trips the stall resync, and the resync reassigns
    // the playout base eleven milliseconds after the resume anchored it with a
    // 500 ms cushion. That is BUGS #2 — the cushion never failed to cover the
    // interleave gap, it was overwritten before the gap mattered.
    //
    // WHERE it is stamped is the whole of the fix, and the first attempt got it
    // backwards. Stamping in enqueue_frame — after its up-to-250 ms wait, on the
    // argument that the wait is where a resume overtakes a frame — hands the
    // waiting frame the epoch it wakes up into, which is the NEW one. That is
    // the bug again with a label on it, and the logs showed the resync still
    // firing at 9.1 s and 62.3 s after holds of 11 s and 67 s.
    //
    // The epoch that matters is the one the TIMESTAMP was computed under,
    // because that is what makes the timestamp meaningful or stale. So it is
    // stamped beside playout_due_ns in deliver_video/deliver_audio, and read
    // BEFORE playout_base_ns so that a resume landing mid-read errs towards
    // dropping a good frame rather than passing a stale one.
    uint64_t epoch = 0;
    DecodedVideoFrame video;
    DecodedAudioFrame audio;
    // Where this audio goes. Null means this source. A companion audio-only
    // source carrying an ISO or the click is a different obs_source_t, but its
    // frames travel the SAME queue with timestamps from the SAME playout base
    // — which is what keeps every track locked to the picture and to each
    // other. Giving each track its own decoder would reintroduce exactly the
    // per-track drift that packed multi-channel existed to avoid.
    obs_source_t* target = nullptr;
};

// ── Companion audio sources ──────────────────────────────────────────────────
// An OBS source can emit only one audio stream, so an event carrying a main
// mix plus ISOs plus a click needs one source per track. They all attach to the
// room's existing decoder rather than opening their own: one download, one
// decode, one clock.
//
// Registered globally by room rather than against a particular SourceCtx, so a
// companion survives the video source being reconfigured or replaced.
struct AudioSub {
    obs_source_t* source = nullptr;
    std::string   room_id;
    int           track = 0;          // 0-based index into the event's tracks
};
static std::mutex g_subs_mtx;
static std::vector<AudioSub*> g_subs;

// The same idea for video, for a feed that carries more than one picture. A
// room that needs two or four cameras composites them at the main site and
// sends one feed; each region becomes a source here, already cropped, instead
// of somebody building crop filters by hand at every satellite.
//
// Registered by room for the same reason AudioSub is: a companion has to
// survive the video source being reconfigured or replaced.
struct TileSub {
    obs_source_t* source = nullptr;
    std::string   room_id;
    int           tile = 0;           // 0-based, reading order: 0 is top-left
    // Size of the last region handed over, so OBS has something to lay the
    // source out with. Written by the delivery thread and read by OBS's, which
    // is why they are atomic and why they live here rather than in TileCtx —
    // this is the struct the delivery thread already holds.
    std::atomic<uint32_t> w{0}, h{0};
};
static std::mutex g_tiles_mtx;
static std::vector<TileSub*> g_tiles;

static void register_tile_sub(TileSub* s) {
    std::lock_guard<std::mutex> lk(g_tiles_mtx);
    for (auto* e : g_tiles) if (e == s) return;
    g_tiles.push_back(s);
}
static void unregister_tile_sub(TileSub* s) {
    std::lock_guard<std::mutex> lk(g_tiles_mtx);
    g_tiles.erase(std::remove(g_tiles.begin(), g_tiles.end(), s), g_tiles.end());
}

static void register_audio_sub(AudioSub* s) {
    std::lock_guard<std::mutex> lk(g_subs_mtx);
    for (auto* e : g_subs) if (e == s) return;
    g_subs.push_back(s);
}
static void unregister_audio_sub(AudioSub* s) {
    std::lock_guard<std::mutex> lk(g_subs_mtx);
    g_subs.erase(std::remove(g_subs.begin(), g_subs.end(), s), g_subs.end());
}

// Exposes timeslipping to hotkeys and the Tools menu.
struct SourceCtx : DecoderControls {
    obs_source_t* source = nullptr;

    // Held as shared_ptr behind a short-lived lock. The worker threads and the
    // UI both need these, and some calls block (network polls, and pushing a
    // fragment when the decoder is full). Holding a mutex across a blocking
    // call deadlocked Resume against the feed loop, so callers now take a
    // reference under `obj_mtx` and release it before doing any work — the
    // session and decoder are internally thread-safe.
    std::shared_ptr<S3Transport>    transport;
    // Null unless a LAN host is configured (PROJECT-SCOPE.md §8.7). Kept
    // alongside `transport` (never in place of it) so stop_playback()'s
    // "cancel whatever is in flight" and play()'s re-arm reach BOTH legs —
    // see the calls next to tx->cancel_pending()/resume_pending() below.
    std::shared_ptr<LanTransport>       lan_transport;
    // Only constructed when BOTH transport and lan_transport exist; owns
    // nothing new, just decides per-request which of the two above answers.
    // DecoderSession is built against this when it exists, `lan_transport`
    // alone when there is no cloud leg, or `transport` alone when there is
    // no LAN leg — see build_transports() below.
    std::shared_ptr<FallbackTransport> fallback;
    // The two-cloud read path, when a second bucket is configured — kept so the
    // dock can say which end is being read (Phase 9).
    std::shared_ptr<MirrorReadTransport> mirror_read;
    std::shared_ptr<DecoderSession> session;
    std::shared_ptr<CmafDecoder>    decoder;
    mutable std::mutex              obj_mtx;
    // Serialises src_update / src_destroy. OBS can apply settings from more
    // than one thread, and two overlapping updates could leave two sets of
    // worker threads running — which showed up as every log line appearing
    // twice and as clock adjustments being applied twice.
    std::mutex                      lifecycle_mtx;

    std::thread poll_thread, feed_thread;
    std::atomic<bool> running{false};

    // Guards session/decoder lifetime against the worker threads.
    std::mutex mtx;

    int  poll_interval_ms = 3000;
    uint32_t width = 0, height = 0;

    // Playout clock: OBS timestamps = base + (media pts - first media pts).
    std::atomic<uint64_t> playout_base_ns{0};
    std::atomic<int64_t>  first_pts_ns{-1};
    // The pts of the last frame handed to OBS — what is actually on screen.
    // Logged at a hold so the next resume's anchor can be compared against the
    // picture rather than against a figure from several seconds earlier, which
    // is what made "it skips on resume" hard to settle from the log.
    std::atomic<int64_t>  last_out_pts_ns{-1};
    std::atomic<bool>     decoder_started{false};
    uint64_t              seen_discontinuity = 0;

    // ── Delivery lead measurement ────────────────────────────────────────────
    // Measured at the HANDOUT, not at the source. That distinction is the
    // whole point, and it replaces two earlier metrics that were measured on
    // the source side and were both misleading:
    //
    //   "a/v gap/drift"  compared the two streams' pts spans. Since both are
    //                    timestamped base + (pts - first) from the same base
    //                    and the same anchor, that difference is identically
    //                    the difference already present in the media. It can
    //                    never show a fault we introduce. Its "drift" term
    //                    actually reported the startup offset left by the
    //                    anchor race, which is constant and harmless.
    //   "a/v pts offset" compared an audio pts against whatever video frame
    //                    had most recently been delivered. Video leaves the
    //                    decoder with B-frame reorder plus frame-threading
    //                    latency and audio leaves it 1:1, so those two are
    //                    never contemporaneous. The number was noise.
    //
    // Both were checked against ground truth and cleared: the stored segments
    // hold video and audio aligned to within one AAC frame across minutes of
    // an event, and the decoded rates match nominal exactly. So the media is
    // right and the arithmetic is right, which leaves the only thing neither
    // metric could see — whether a frame still has time in hand when we hand
    // it over.
    //
    // lead = timestamp - now at the moment of obs_source_output_*. Positive
    // means the frame arrived with time to spare. Decaying toward zero on one
    // stream while the other holds is lipsync being introduced downstream of
    // us, and is the same condition that makes the timeline sticky. The
    // minimum matters more than the mean: the mean hides the frame that was
    // late, and the late frame is the one that shows.
    //
    // Windowed — reset after every report, so each line describes the interval
    // it covers rather than an average since startup that can never recover.
    std::atomic<int64_t>  lead_video_sum_ns{0};
    std::atomic<int64_t>  lead_video_min_ns{INT64_MAX};
    std::atomic<uint64_t> lead_video_count{0};
    std::atomic<int64_t>  lead_audio_sum_ns{0};
    std::atomic<int64_t>  lead_audio_min_ns{INT64_MAX};
    std::atomic<uint64_t> lead_audio_count{0};
    std::atomic<bool> checked_layout{false};

    // Resume watchdog: if no frames reach OBS shortly after a resume, dump
    // enough state to explain why instead of leaving a frozen picture and no
    // clue in the log.
    std::atomic<uint64_t> resumed_at_ns{0};
    std::atomic<uint64_t> frames_at_resume{0};

    // ── Resume transient, for BUGS #2 ────────────────────────────────────────
    // MEASUREMENT ONLY. The periodic lead report above averages over its whole
    // interval, which is precisely how a 500 ms event at resume vanishes — it
    // is why three rounds of logs could not separate the candidate causes. This
    // window covers the first second after a resume and nothing else.
    //
    // What each reading rules in or out:
    //   gap vs cushion     the interleave gap is ~344 ms in the field and the
    //                      cushion is 500 ms. If the gap on THIS resume is
    //                      larger, the cushion is simply too small and the
    //                      arithmetic is innocent.
    //   min lead           kMaxDeliveryLeadNs is 400 ms. A min at or below zero
    //                      means frames were already due when handed over, so
    //                      they went out in a burst rather than paced.
    //   pts span vs frames a stream that hands OBS more programme-time than the
    //                      other in the same window is the unbalanced batch D4
    //                      predicts, and is measured here in ms rather than in
    //                      frames so the two are comparable at all.
    std::atomic<uint64_t> rw_end_ns{0};
    std::atomic<bool>     rw_reported{true};
    std::atomic<int>      rw_v_frames{0},     rw_a_frames{0};
    std::atomic<int64_t>  rw_v_lead_min_ns{INT64_MAX}, rw_a_lead_min_ns{INT64_MAX};
    std::atomic<int64_t>  rw_v_pts_lo{-1}, rw_v_pts_hi{-1};
    std::atomic<int64_t>  rw_a_pts_lo{-1}, rw_a_pts_hi{-1};

    // Which stream won the anchor race, and what the other one's first frame
    // turned out to be offset by. One reading per re-anchor.
    std::atomic<bool>     anchor_was_video{false};
    std::atomic<bool>     anchor_gap_pending{false};
    std::atomic<bool>     resume_checked{false};

    // The clock time of the frame currently on screen. Derived from the frame
    // being delivered, so it advances continuously rather than jumping once
    // per segment — which is why the displayed time appeared frozen.
    std::atomic<long long> playing_at_ms{0};
    // The media segment of the frame currently going to air, from the frame
    // itself. A cue is placed with this rather than with a clock reading: the
    // media clock is re-pinned to a fresh offset on every seek, so a time names
    // a place inconsistently (see DecoderSession::add_cue).
    std::atomic<uint64_t>  on_screen_seq{0};
    // Converting a frame's pts into a clock reading.
    //
    // This used to pair the wall time of whichever fragment the FEED thread
    // had most recently pushed with a pts base taken from whatever the
    // DELIVERY thread happened to be emitting. Those are different fragments
    // by design — kFeedLeadNs keeps the feed 2.5 s ahead — so the clock was
    // routinely a fragment out, and after a jump it paired the new position's
    // wall time with the old position's pts base, which is minutes.
    //
    // Instead the offset between the media timeline and the wall clock is
    // latched ONCE per decoder, from the first fragment pushed into it. That
    // pairing is safe: a restart tears the decoder down and clears the
    // delivery queue, so the first frame out afterwards belongs to the first
    // fragment in. Every reading after that is the frame's own pts plus a
    // constant — no shared state between the two threads at all.
    static constexpr long long kOffsetUnset = LLONG_MIN;
    std::atomic<long long> pts_wall_offset_ms{kOffsetUnset};
    // Wall time of the first fragment pushed since the decoder started, and a
    // flag consumed by that fragment. The flag matters: if the first fragment
    // has no wall time, this must stay unlatched and fall back to the
    // segment-granular clock — taking a LATER fragment's wall time would
    // pair it with the first fragment's pts and reintroduce exactly the
    // mispairing being fixed.
    std::atomic<long long> restart_wall_ms{0};
    std::atomic<bool>      restart_wall_pending{true};
    // The skip's base and the media-clock pin's base used to live here as two
    // more atomics. They belong to PlayoutTimeline now — they are written only
    // by the delivery loop, and keeping them out here is what let the feed loop
    // move one of them mid-use.
    // Which timeline the frames in flight belong to. Bumped by every seek and
    // decoder restart, under dq_mtx so it orders against the delivery loop's
    // pop.
    //
    // Clearing the queue is not enough on its own: the delivery loop pops a
    // frame into a local and then waits up to a full delivery lead for it to
    // fall due, so a frame taken from the queue BEFORE a seek is still
    // processed after it — past the clear, past the decoder teardown, and with
    // no trace left that it belongs to the position just left. It then claimed
    // the media-clock pin's base. Seen in the field: a seek anchored on pts
    // 2712.003s and pinned on 1933.464s, 778 seconds apart, because the frame
    // that got there first was from the previous position.
    std::atomic<uint64_t> timeline_epoch{0};
    // Bumped only when the MEDIA timeline restarts — a seek, a jump, a stop, a
    // decoder restart. NOT on a resume: a hold changes nothing about the media,
    // so the pts->wall mapping stays valid and must not be re-learned from a
    // fragment nobody fed. See PlayoutTimeline::adopt.
    std::atomic<uint64_t> media_epoch{0};
    // Whether the wall time the media clock is pinned to was measured or
    // estimated. See the feed loop, and BUGS #2's cue-accuracy note.
    std::atomic<bool> restart_wall_estimated{false};

    // How the encoder composited this feed, read by the delivery thread on
    // every video frame and written by the poll thread when the manifest
    // arrives. Packed into ONE atomic rather than two: cols and rows read
    // separately can tear across a write, and a frame cropped to a layout that
    // never existed is a picture nobody can explain.
    //
    // `split` is separate only so the common case — one picture, no tiles —
    // costs one relaxed bool read per frame rather than an unpack.
    std::atomic<uint32_t> tile_layout_packed{(1u << 8) | 1u};   // 1x1
    std::atomic<bool>     tile_layout_split{false};

    multisite::TileLayout tile_layout_now() const {
        const uint32_t p = tile_layout_packed.load();
        multisite::TileLayout t;
        t.cols = (int)(p >> 8);
        t.rows = (int)(p & 0xff);
        return t;
    }
    void set_tile_layout(const multisite::TileLayout& t) {
        tile_layout_packed = ((uint32_t)t.cols << 8) | (uint32_t)t.rows;
        tile_layout_split  = t.is_split();
    }

    // ── Where the live edge is RIGHT NOW ─────────────────────────────────────
    // "How far behind the main site am I" was (live_seq - head) * segment
    // duration: two integers, so it could only ever move in whole segments and
    // it visibly swung by six seconds as each side stepped. The playhead side
    // is already frame-accurate from the media clock, so only the live edge
    // needed fixing.
    //
    // The main site's content advances in real time; we just learn about it
    // one segment at a time. So record where the edge was and when we saw it
    // move, and carry it forward at real-time rate in between. That keeps the
    // meaning identical — distance to the published live edge — while making
    // the number continuous.
    std::atomic<uint64_t>  live_edge_seq{0};
    std::atomic<long long> live_edge_wall_ms{0};   // end of the newest segment
    std::atomic<long long> live_edge_seen_ms{0};   // monotonic, when we saw it
    // After a timed seek, frames earlier than this point in the segment are
    // dropped, giving roughly one-second accuracy instead of six.
    std::atomic<long long> skip_until_pts_ns{-1};

    // An operator loads an event, lets it buffer, then presses Play on cue.
    // Auto-playing as soon as enough is buffered is wrong for an event.
    std::atomic<bool> playing{false};
    // Stopped is NOT merely "not playing". Loading an event is also not
    // playing, and loading must keep downloading — filling the buffer before
    // the operator goes to air is the entire point of Load. `stopped` is the
    // narrower state Stop puts the source into: nothing downloading, no
    // decoder, nothing on air, waiting for Play or for a recording to be
    // loaded.
    //
    // What it deliberately does NOT discard is the cache. Emptying it would
    // make Play re-satisfy start_buffer_seconds, 60s by default, turning Stop
    // into a minute to undo — and an expensive Stop is the thing that would
    // then need a confirmation dialog in front of it. Keeping the cache means
    // Play re-enters from disk and Stop stays cheap to change your mind about.
    std::atomic<bool> stopped{false};
    // Guards against accidental clicks mid-event.
    std::atomic<bool> controls_locked{false};
    // Set while the queue is being torn down (seek, stop, decoder restart) so
    // the decoder's callbacks return immediately instead of waiting for space
    // that will never come.
    std::atomic<bool> flushing{false};
    // Split by stream. A dropped video frame repeats the previous picture; a
    // dropped audio frame is a hole you can hear. Lumping them together hid
    // which of the two was happening — and nothing read the total anyway.
    std::atomic<uint64_t> frames_dropped{0};
    std::atomic<uint64_t> dropped_video{0};
    std::atomic<uint64_t> dropped_audio{0};
    std::atomic<uint64_t> last_resync_log_ns{0};

    // ── Immediate feedback ───────────────────────────────────────────────────
    // Every operator action here is answered by the network, not by the click:
    // a pin takes effect on the next poll, and a seek shows nothing until a
    // frame has been fetched and decoded. Left alone, the dock sat unchanged
    // for seconds and then snapped — which reads as a dropped click, so the
    // operator presses again.
    //
    // These three record what has been ASKED FOR, so the dock can say so at
    // once and correct itself when the real state arrives.
    //
    // `poll_now` cuts the wait itself: the poll loop otherwise sleeps out the
    // remainder of its interval (3 s by default) before even looking.
    std::atomic<bool>      poll_now{false};
    // Where a seek is heading, until frames actually arrive there. 0 = settled.
    std::atomic<long long> seek_target_ms{0};
    // An event has been chosen but is not yet ready to play.
    std::atomic<bool>      loading_event{false};
    // Cleared once playback has actually arrived where it was sent.
    std::atomic<bool>      awaiting_frames{false};
    // Both indications are bounded. If the thing being waited for never
    // happens — an event with no retained content, a store that has gone away
    // — the dock must fall back to reporting the real state rather than
    // sitting on "LOADING…" for ever.
    std::atomic<uint64_t>  action_started_ns{0};

    // ── Event list ───────────────────────────────────────────────────────────
    // The catalog makes one request per event, so a refresh runs on the poll
    // thread. The dock only ever sets a flag and reads the last result — it
    // must never wait on the network.
    std::shared_ptr<EventCatalog>   catalog;
    std::atomic<bool>               events_refresh_wanted{false};
    std::atomic<bool>               events_refreshing{false};
    std::atomic<bool>               events_listed_once{false};
    mutable std::mutex              events_mtx;
    EventListing                    events_cache;

    // DecoderControls — driven by hotkeys and the Tools menu.
    void pause() override;
    void resume() override;
    void toggle_pause() override;
    void jump_to_live() override;
    void log_status() override;
    void snapshot(DecoderSnapshot& out) const override;
    void jump_to_marker(const std::string& id) override;
    void add_cue(const std::string& label, std::string& error) override;
    void seek(unsigned long long seq) override;
    void seek_media(long long media_ms) override;
    void reconfigure() override;
    void play() override;
    void stop_playback() override;
    bool is_playing() const override { return playing.load(); }
    void seek_to_time(long long wall_ms) override;
    void jog(double seconds) override;
    void set_delay_from_live(double seconds) override;
    void set_locked(bool l) override { controls_locked = l; }
    bool locked() const override { return controls_locked.load(); }
    void refresh_events() override { events_refresh_wanted = true; }
    void event_listing(EventListing& out) const override;
    void pin_event(const std::string& event_id) override;
    void unpin_event() override;

    // Re-anchor after any jump of the playback head, and arm the "going to…"
    // indication. Shared by seek and jump-to-live: jumping to live used to do
    // none of this, so it moved the picture with nothing on the dock to say
    // it had been asked to.
    void after_jump(long long to_wall_ms);

    // Release the decoder and everything derived from the media timeline, so
    // the next fragment rebuilds both. Shared by the seek path and the poll
    // loop's discontinuity handler: they were separate copies, and the seek
    // path was missing the decoder teardown entirely.
    void release_decoder_for_restart();

    // Leave the stopped state: clear the flag and re-arm the transport, which
    // Stop cancelled. Anything that should start downloading again calls this
    // — Play, and loading an event — because the cancel flag is sticky and a
    // source that stayed armed would abort every request the instant it began.
    void resume_downloads();

    // Marker chosen in the properties dialog, acted on by the Jump button.
    std::string pending_marker_id;
    std::string room_id_for_display;
    // Which of the event's audio tracks this source emits. Multi-track is the
    // normal production mode: main mix on one track, ISOs and click on others,
    // each its own stream with its own channel count. Track 1 by default,
    // because a campus that just wants the programme should not have to think
    // about it.
    std::atomic<int> audio_track{0};
    std::atomic<int> audio_channels{0};
    // Wall clock when playback was paused, so the playout clock can be
    // advanced by the same amount on resume (see on_resume).
    std::atomic<uint64_t> pause_started_ns{0};
    // Pause is enforced at DELIVERY, not at segment feeding: by the time a
    // fragment is fed, its whole 6 s is already decoded, so gating the feed
    // would let playback run on for up to a segment after the click. Holding
    // frames here makes pause and resume take effect immediately, and nothing
    // is discarded — the queue is simply not drained while paused.
    std::atomic<bool> paused{false};
    uint64_t feed_start_ns = 0;      // wall clock when this decoder started
    uint64_t pushed_media_ns = 0;    // media duration handed over so far

    // Ordered delivery queue (see the note above kMaxQueuedVideo).
    std::deque<PendingFrame> dq;
    std::mutex               dq_mtx;
    std::condition_variable  dq_cv;
    std::thread              deliver_thread;

    // status for logging
    std::atomic<uint64_t> frames_out{0};
    RoomState last_room = RoomState::Unknown;
    int64_t   last_status_log_ms = 0;
};

// ── Frame delivery ───────────────────────────────────────────────────────────
// OBS's async buffer only holds a fraction of a second (audio buffering
// defaults to ~960 ms), while decoding a 6 s fragment yields 6 s of frames
// almost instantly. So delivery has to be paced — but pacing must NOT happen
// inside the decoder callbacks: video and audio share the decode thread, so
// sleeping on a video frame delays every audio frame decoded after it, which
// makes audio arrive seconds late and OBS reset it (audible as bursts).
//
// Instead the decoder pushes frames into one queue, in decode (timestamp)
// order, and a dedicated thread releases them when they are nearly due. Audio
// and video therefore stay together and neither starves the other.
static constexpr uint64_t kMaxDeliveryLeadNs = 400000000ULL;   // 400 ms
// Bounded PER STREAM, not as one total. A flat item count sounds equivalent
// and is not: audio and video share this queue, and audio outnumbers video
// per TRACK. Measured on a two-track event, audio ran 93.75 frames/s against
// video's 30 — so of a flat 16 slots, audio held about twelve and video less
// than four. That is ~130ms of video in flight when the pacing gate wants
// 400ms, and it showed up exactly that way: audio was handed to OBS sitting
// on the gate at +397ms while video limped in at +233ms, every window, with
// video therefore the stream that would drop first under any hiccup.
//
// The scaling is the real argument. A flat bound makes audio's share grow
// with TRACK COUNT, so the multi-track feature this plugin exists to provide
// squeezes video harder the more you use it — four tracks would leave video
// about two slots. Bounding each stream separately breaks that coupling:
// video's allowance no longer depends on how many audio tracks a campus
// takes.
//
// Sized from the gate rather than guessed: 12 video frames is 400ms at 30fps,
// which is what kMaxDeliveryLeadNs is trying to hold. Audio frames are small
// (an AAC frame is ~21ms of samples) so 48 costs almost nothing and covers
// several tracks at once.
//
// Memory is now predictable, which the flat bound also failed at: the cap is
// 12 video frames regardless of track count. At 1080p an I420 frame is
// ~3.1 MB, so ~37 MB worst case — where a flat 64 would have risked 200 MB
// had a stall filled it with video.
static constexpr size_t   kMaxQueuedVideo = 12;
static constexpr size_t   kMaxQueuedAudio = 48;
// If frames fall further behind wall time than this, the playout clock is
// re-anchored rather than dumping a backlog into OBS.
static constexpr uint64_t kClockResyncThresholdNs = 2000000000ULL;   // 2 s
// Take a reference under the short lock; never call through it while holding
// `obj_mtx`. This is what keeps blocking work (network polls, pushing a
// fragment to a full decoder) off the critical section that Pause/Resume need.
// Live decoder sources, so a companion can find the room's track names. Used
// only to build a properties list — never on the media path.
static std::mutex g_owners_mtx;
static std::vector<SourceCtx*> g_owners;

static void register_owner(SourceCtx* c) {
    std::lock_guard<std::mutex> lk(g_owners_mtx);
    for (auto* e : g_owners) if (e == c) return;
    g_owners.push_back(c);
}
static void unregister_owner(SourceCtx* c) {
    std::lock_guard<std::mutex> lk(g_owners_mtx);
    g_owners.erase(std::remove(g_owners.begin(), g_owners.end(), c), g_owners.end());
}

static std::shared_ptr<DecoderSession> get_session(SourceCtx* ctx) {
    std::lock_guard<std::mutex> lk(ctx->obj_mtx);
    return ctx->session;
}
static std::shared_ptr<CmafDecoder> get_decoder(SourceCtx* ctx) {
    std::lock_guard<std::mutex> lk(ctx->obj_mtx);
    return ctx->decoder;
}

// Push a stamped frame for delivery. Blocks while the queue is full, which
// back-pressures the decoder rather than letting memory grow.
static void enqueue_frame(SourceCtx* ctx, PendingFrame&& item) {
    std::unique_lock<std::mutex> lk(ctx->dq_mtx);

    // A HOLD MUST NOT CONSUME PROGRAMME.
    //
    // pause() stops delivery and stops fetching new segments, but the decoder
    // keeps decoding the fragments it already holds. Those frames used to reach
    // the wait below, time out after 250 ms and be DROPPED — so holding quietly
    // ate programme, and resume continued from wherever the decoder had got to
    // rather than from where the picture stopped. Measured at 0.20 s lost after
    // a 1.4 s hold, 1.09 s after 11 s and 3.90 s after 67 s, each matching the
    // frames dropped across that hold to within a frame or two.
    //
    // A recorder freezes the READ head and keeps the write head going. Parking
    // the producer here freezes the reader properly: the decoder stops pulling,
    // nothing is decoded, nothing is discarded, and the queue still holds the
    // programme either side of the hold.
    //
    // Polled rather than waited on outright. `flushing` and `running` are what
    // a stop or a seek uses to release a parked producer — stop_playback() and
    // after_jump() both set flushing before they join the decoder's worker, so
    // the deadlock this 250 ms timeout was standing in for is already defended
    // by the flag that was always the real defence. Re-checking every 100 ms
    // means even a missed notification cannot wedge the decoder, and a wedged
    // decoder here is a frozen OBS.
    while (ctx->paused.load() && ctx->running.load() && !ctx->flushing.load())
        ctx->dq_cv.wait_for(lk, std::chrono::milliseconds(100));
    if (!ctx->running.load() || ctx->flushing.load()) return;
    // Bounded wait. This used to wait indefinitely for space, which meant the
    // decoder's worker thread could block inside this callback whenever
    // delivery stopped draining — after Stop, or while a seek tore the decoder
    // down. CmafDecoder::stop() then joined a thread that could never finish,
    // and because stop runs on the UI thread, OBS froze solid.
    //
    // Dropping a frame is vastly preferable to hanging the application: the
    // decoder always makes progress, so join always returns.
    // Counted on demand rather than tracked in parallel counters. The queue is
    // at most 60 items and this runs a few hundred times a second, so the scan
    // is free — and counters would have to be kept in step with the delivery
    // loop's erase, every flush, and every dq.clear() on seek, which is how
    // they drift out of step and stall the decoder against a phantom full
    // queue.
    const bool want_video = item.is_video;
    const bool space = ctx->dq_cv.wait_for(
        lk, std::chrono::milliseconds(250), [ctx, want_video] {
            if (!ctx->running.load() || ctx->flushing.load()) return true;
            const size_t n = (size_t)std::count_if(
                ctx->dq.begin(), ctx->dq.end(),
                [want_video](const PendingFrame& f) {
                    return f.is_video == want_video;
                });
            return n < (want_video ? kMaxQueuedVideo : kMaxQueuedAudio);
        });
    if (!ctx->running.load() || ctx->flushing.load()) return;
    if (!space) {
        // Delivery is not keeping up (or is stopped). Drop this frame.
        ctx->frames_dropped++;
        if (item.is_video) ctx->dropped_video++; else ctx->dropped_audio++;
        return;
    }
    // NOTE: item.epoch is stamped where the TIMESTAMP is computed, not here.
    // Stamping here was tried and is wrong — see the field's comment.
    ctx->dq.push_back(std::move(item));
    lk.unlock();
    ctx->dq_cv.notify_all();
}

// Releases frames to OBS when they are nearly due, in queue (timestamp) order,
// so audio and video are handed over together.
static void deliver_loop(SourceCtx* ctx) {
    mlog_info("source: delivery loop started");
    // Owned here, not shared: every piece of state it holds is written only by
    // this thread. The seek that invalidates it happens elsewhere, and reaches
    // this object as a timeline id read from an atomic below — which is what
    // lets it stay lock-free on a path that runs ~124 times a second.
    multisite::PlayoutTimeline tl;
    while (ctx->running.load()) {
        // While paused, deliver nothing: the picture holds on the last frame
        // OBS received and the queue stays put, so resume continues exactly
        // where the operator stopped.
        if (ctx->paused.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        PendingFrame item;
        uint64_t item_epoch = 0;
        {
            std::unique_lock<std::mutex> lk(ctx->dq_mtx);
            ctx->dq_cv.wait_for(lk, std::chrono::milliseconds(50), [ctx] {
                return !ctx->dq.empty() || !ctx->running.load();
            });
            if (!ctx->running.load()) break;
            if (ctx->dq.empty()) continue;
            // Release the earliest-timestamped frame in the window, not simply
            // the first enqueued: video and audio arrive in track order, not
            // presentation order.
            auto it = std::min_element(ctx->dq.begin(), ctx->dq.end(),
                [](const PendingFrame& a, const PendingFrame& b) {
                    return a.timestamp < b.timestamp;
                });
            item = std::move(*it);
            ctx->dq.erase(it);
            item_epoch = item.epoch;      // the frame's own, not today's
        }
        ctx->dq_cv.notify_all();        // let the decoder push again

        // Checked here as well as after the wait below. The stall resync sits
        // between the two and re-bases the playout clock from this frame's
        // pts, so it must not see one from a timeline already left either.
        tl.adopt(ctx->timeline_epoch.load(), ctx->media_epoch.load());
        if (item_epoch != tl.epoch()) continue;

        // If a frame is far past due, the playout clock has drifted behind
        // wall time — normally because playback stalled waiting for a segment
        // (a network hiccup) rather than an explicit pause. Re-anchor instead
        // of flushing a backlog at OBS, which it would report as audio lagging.
        {
            const uint64_t now = os_gettime_ns();
            if (item.timestamp + kClockResyncThresholdNs < now) {
                // Re-anchor by ASSIGNING the clock, never by accumulating on
                // to it. The previous version did `base += behind`, so if two
                // deliveries resynced for the same stall the clock jumped
                // twice — pushing every frame seconds into the future and
                // stalling playback outright (seen as frames_out frozen while
                // "behind live" kept growing). Assignment is idempotent: a
                // second resync for the same stall is a no-op.
                const long long first = ctx->first_pts_ns.load();
                const long long pts = item.is_video ? item.video.pts_ns
                                                    : item.audio.pts_ns;
                const uint64_t behind = now - item.timestamp;
                const uint64_t new_base =
                    now - (uint64_t)(pts - first) + kMaxDeliveryLeadNs;
                ctx->playout_base_ns = new_base;
                item.timestamp = new_base + (uint64_t)(pts - first);
                {
                    std::lock_guard<std::mutex> qlk(ctx->dq_mtx);
                    for (auto& q : ctx->dq) {
                        const long long qp = q.is_video ? q.video.pts_ns
                                                        : q.audio.pts_ns;
                        q.timestamp = new_base + (uint64_t)(qp - first);
                    }
                }
                // Rate-limit the message: a stall should be reported once, not
                // once per frame.
                const uint64_t last = ctx->last_resync_log_ns.load();
                if (now - last > 5000000000ULL) {
                    ctx->last_resync_log_ns = now;
                    mlog_warn("source: playout clock fell %.1fs behind "
                              "(stall?) — re-anchored", (double)behind / 1e9);
                }
            }
        }

        // Hold until nearly due, in slices so shutdown stays responsive.
        // A frame that is already due (or late) is released immediately.
        while (ctx->running.load()) {
            const uint64_t now = os_gettime_ns();
            if (item.timestamp <= now + kMaxDeliveryLeadNs) break;
            uint64_t wait_ns = item.timestamp - now - kMaxDeliveryLeadNs;
            if (wait_ns > 50000000ULL) wait_ns = 50000000ULL;
            std::this_thread::sleep_for(std::chrono::nanoseconds(wait_ns));
        }
        if (!ctx->running.load()) break;

        // One ordered decision: is this frame stale, is it before the moment a
        // seek asked for, and only then may it define the clock and go to air.
        // The order lives in PlayoutTimeline and is pinned by
        // test_playout_timeline, because every bug in this area came from
        // getting it wrong here — a staleness check below the pin guards
        // nothing, and a frame dropped by the skip must not pin either.
        tl.adopt(ctx->timeline_epoch.load(), ctx->media_epoch.load());
        {
            // Inputs from the feed loop. A skip is only ever armed for the
            // fragment a seek landed on, and the queue was cleared and the
            // decoder restarted for that seek, so the frame that claims the
            // base here is genuinely that fragment's first.
            const long long armed = ctx->skip_until_pts_ns.exchange(-1);
            if (armed >= 0) tl.begin_fragment(armed);
            const long long w = ctx->restart_wall_ms.load();
            if (w > 0) tl.set_restart_wall_ms(w);
        }

        const long long item_pts = item.is_video ? item.video.pts_ns
                                                 : item.audio.pts_ns;
        const bool had_clock = tl.have_clock();
        switch (tl.consider(item_epoch, item_pts)) {
            case multisite::PlayoutTimeline::Action::Discard:     continue;
            case multisite::PlayoutTimeline::Action::DropForSkip: continue;
            case multisite::PlayoutTimeline::Action::Play:        break;
        }

        if (!had_clock && tl.have_clock()) {
            // Every displayed clock time is built on this pairing, and it is
            // learned once, so it is logged to stay checkable against the
            // fragment the seek asked for. The pts here must match the one the
            // playout anchored on in the line above it; when those two differ,
            // the clock has been pinned to a position already left.
            mlog_info("source: media clock pinned — fragment wall %lld (%s), "
                      "first pts %.3fs (so pts 0 would be wall %lld)",
                      tl.pin_wall_ms(),
                      ctx->restart_wall_estimated.load()
                          ? "ESTIMATED from seq x nominal duration"
                          : "measured, from the manifest",
                      (double)tl.pin_base_pts_ns() / 1e9,
                      tl.clock_offset_ms());
        }
        // Publish for the dock and the web remote.
        if (tl.have_clock()) {
            ctx->pts_wall_offset_ms = tl.clock_offset_ms();
            ctx->playing_at_ms      = tl.wall_ms_for(item_pts);
        }

        // Has playback actually ARRIVED where it was sent?
        //
        // "Any frame was delivered" is not the same question, and answering
        // that one made the whole indication useless: the decoder still holds
        // already-decoded frames from the old position when a seek is issued,
        // so the very next delivery — milliseconds later, long before the dock
        // ticks — cleared the state and the operator saw nothing at all.
        //
        // Arrival means a frame whose own clock time is at the target.
        {
            const long long target = ctx->seek_target_ms.load();
            if (target > 0) {
                const long long at = ctx->playing_at_ms.load();
                // Tighter than a segment (6 s): frames are dropped within the
                // landing segment to reach the requested moment, so the first
                // frame delivered there reads very close to the target. A whole
                // segment's tolerance would accept a stale frame from the
                // position just left — the mistake that made this invisible.
                const uint64_t started = ctx->action_started_ns.load();
                const bool timed_out = started != 0 &&
                    os_gettime_ns() - started > 30000000000ULL;   // 30 s
                if ((at > 0 && std::llabs(at - target) < 2500) || timed_out) {
                    ctx->seek_target_ms  = 0;
                    ctx->awaiting_frames = false;
                }
            } else {
                ctx->awaiting_frames = false;
            }
        }

        // Stop means stop. Checked again here, after the wait above, because
        // a frame can pass the check in deliver_* microseconds before Stop
        // flips `playing` and then sit in the queue across it — and this is
        // the last point before air. Discarded rather than held: the decoder
        // stays running after Stop by design, so Play re-delivers from the
        // session's own position and nothing here is worth keeping.
        if (!ctx->playing.load()) continue;

        // How much time did this frame have left when we handed it over?
        // Sampled here, immediately before the output call, because every
        // earlier point still has the wait loop and the seek-skip ahead of it.
        {
            const int64_t lead =
                (int64_t)item.timestamp - (int64_t)os_gettime_ns();
            if (item.is_video) {
                ctx->lead_video_sum_ns += lead;
                ctx->lead_video_count++;
                // Plain load/store: the delivery loop is the only writer.
                if (lead < ctx->lead_video_min_ns.load())
                    ctx->lead_video_min_ns = lead;
            } else {
                ctx->lead_audio_sum_ns += lead;
                ctx->lead_audio_count++;
                if (lead < ctx->lead_audio_min_ns.load())
                    ctx->lead_audio_min_ns = lead;
            }

            // The same sample again, but confined to the first second after a
            // resume (BUGS #2). Separate accumulators rather than a shorter
            // reporting interval, because the periodic report has a job of its
            // own and shortening it would bury the steady state in noise.
            if (os_gettime_ns() < ctx->rw_end_ns.load()) {
                const int64_t pts = item.is_video ? item.video.pts_ns
                                                  : item.audio.pts_ns;
                if (item.is_video) {
                    ctx->rw_v_frames++;
                    if (lead < ctx->rw_v_lead_min_ns.load())
                        ctx->rw_v_lead_min_ns = lead;
                    if (ctx->rw_v_pts_lo.load() < 0) ctx->rw_v_pts_lo = pts;
                    ctx->rw_v_pts_hi = pts;
                } else {
                    ctx->rw_a_frames++;
                    if (lead < ctx->rw_a_lead_min_ns.load())
                        ctx->rw_a_lead_min_ns = lead;
                    if (ctx->rw_a_pts_lo.load() < 0) ctx->rw_a_pts_lo = pts;
                    ctx->rw_a_pts_hi = pts;
                }
            }
        }

        if (item.is_video) {
            const DecodedVideoFrame& f = item.video;
            struct obs_source_frame frame = {};
            frame.width  = (uint32_t)f.width;
            frame.height = (uint32_t)f.height;
            frame.format = VIDEO_FORMAT_I420;
            frame.timestamp = item.timestamp;
            // Plane pointers are recomputed here: the vector was copied, so the
            // pointers captured at decode time belong to the original buffer.
            uint8_t* base = const_cast<uint8_t*>(f.data.data());
            size_t off = 0;
            for (int i = 0; i < 3; ++i) {
                frame.data[i]     = base + off;
                frame.linesize[i] = (uint32_t)f.stride[i];
                off += (size_t)f.stride[i] * (i == 0 ? f.height : f.height / 2);
            }
            frame.full_range = f.full_range;
            video_format_get_parameters(VIDEO_CS_709,
                                        f.full_range ? VIDEO_RANGE_FULL
                                                     : VIDEO_RANGE_PARTIAL,
                                        frame.color_matrix,
                                        frame.color_range_min,
                                        frame.color_range_max);
            ctx->on_screen_seq = item.video.seq;
            obs_source_output_video(ctx->source, &frame);
            ctx->frames_out++;

            // ── Tiles ────────────────────────────────────────────────────────
            // Fanned out HERE, at the handout, rather than by enqueuing one
            // frame per tile at delivery. That is deliberate and it is the
            // whole reason tiles are free: the delivery queue is bounded per
            // stream, so four tiles enqueued separately would divide
            // kMaxQueuedVideo by four and starve video exactly the way two
            // audio tracks once did. One frame is queued, one frame is decoded,
            // and the tiles are views of it.
            //
            // The crop is pointer arithmetic, not a copy. obs_source_frame
            // carries a pointer and a stride per plane, so a tile is the same
            // buffer with offset pointers, the original strides, and smaller
            // width and height. Nothing is allocated and nothing is memcpy'd
            // however many tiles there are.
            //
            // The main source keeps showing the whole composited picture. It is
            // what the encoder sent, an operator who has not configured any
            // tiles sees exactly what they saw before, and someone who wants
            // only the top-left adds a tile source for it.
            if (ctx->tile_layout_split.load()) {
                const TileLayout lay = ctx->tile_layout_now();
                // The lock is held across the whole fan-out, not used to take a
                // snapshot and released. The audio path can snapshot because it
                // copies out obs_source_t* and never touches the AudioSub
                // again; this writes each tile's size back, so it needs the
                // TileSub itself to still be there — and tile_destroy() can run
                // on the UI thread at any moment. Holding it also means no
                // allocation on a path that runs thirty times a second.
                //
                // Cheap to hold: g_tiles_mtx is contended only by a source
                // being created, destroyed or reconfigured.
                std::lock_guard<std::mutex> lk(g_tiles_mtx);
                for (auto* t : g_tiles) {
                    if (t->room_id != ctx->room_id_for_display) continue;
                    const auto r = lay.tile_rect(t->tile, f.width, f.height);
                    if (r.w <= 0 || r.h <= 0) continue;
                    struct obs_source_frame tf = frame;   // same description…
                    tf.width  = (uint32_t)r.w;            // …smaller rectangle
                    tf.height = (uint32_t)r.h;
                    // Offsets from the planes the full frame already resolved,
                    // rather than re-deriving them: two walks of the same layout
                    // is two chances to disagree about it.
                    //
                    // Chroma is half resolution in both directions, which is why
                    // tile_rect guarantees even edges — an odd offset has no
                    // chroma sample to start from, and the colour would shear
                    // away from the luma.
                    tf.data[0] = frame.data[0] + (size_t)r.y * f.stride[0] + r.x;
                    tf.data[1] = frame.data[1] + (size_t)(r.y / 2) * f.stride[1] + (r.x / 2);
                    tf.data[2] = frame.data[2] + (size_t)(r.y / 2) * f.stride[2] + (r.x / 2);
                    obs_source_output_video(t->source, &tf);
                    t->w = (uint32_t)r.w;
                    t->h = (uint32_t)r.h;
                }
            }
        } else {
            const DecodedAudioFrame& f = item.audio;
            struct obs_source_audio audio = {};
            audio.data[0] = reinterpret_cast<const uint8_t*>(f.interleaved.data());
            audio.frames  = f.frames;
            audio.speakers = ms_layout_for_channels(f.channels);
            audio.format   = AUDIO_FORMAT_FLOAT;      // interleaved float
            audio.samples_per_sec = (uint32_t)f.sample_rate;
            audio.timestamp = item.timestamp;
            obs_source_output_audio(item.target ? item.target : ctx->source,
                                    &audio);
        }
    }
    mlog_info("source: delivery loop exiting");
}

// Anchors the playout clock on the first frame of EITHER stream and returns
// the reference pts. Audio must not wait for video here: anchoring on video
// only meant every audio frame decoded before the first video frame was
// dropped, which is audible as gaps and bursts (and it recurs on every
// decoder restart).
static int64_t anchor_pts(SourceCtx* ctx, int64_t pts_ns, bool is_video) {
    // Cushion covers the reordering window plus jitter.
    static constexpr uint64_t kPlayoutCushionNs = 500000000ULL;   // 500 ms
    int64_t first = ctx->first_pts_ns.load();
    if (first < 0) {
        ctx->first_pts_ns = pts_ns;
        ctx->playout_base_ns = os_gettime_ns() + kPlayoutCushionNs;
        first = pts_ns;
        ctx->anchor_was_video = is_video;
        ctx->anchor_gap_pending = true;
        mlog_info("source: playout anchored on first %s frame (pts %.3fs)",
                  is_video ? "video" : "audio", (double)pts_ns / 1e9);
        return first;
    }

    // MEASUREMENT (BUGS #2), no behaviour change. The first frame of the OTHER
    // stream after an anchor gives this resume's actual interleave gap, which
    // until now was only ever a field average (~344 ms) quoted in
    // playout_clock.h. A gap wider than the cushion means the cushion is simply
    // too small and there is nothing wrong with the arithmetic; a gap well
    // inside it means the fault is downstream of here and the anchor is not
    // where to look. One line per re-anchor.
    if (ctx->anchor_gap_pending.load() &&
        is_video != ctx->anchor_was_video.load()) {
        ctx->anchor_gap_pending = false;
        const int64_t gap_ns = first - pts_ns;   // >0 when this stream is EARLIER
        const double gap_ms = (double)gap_ns / 1e6;
        const double cushion_ms = (double)kPlayoutCushionNs / 1e6;
        mlog_info("source: interleave gap this anchor: %s leads by %.0f ms "
                  "(cushion %.0f ms, %s)",
                  gap_ns > 0 ? (is_video ? "audio" : "video")
                             : (is_video ? "video" : "audio"),
                  gap_ms < 0 ? -gap_ms : gap_ms, cushion_ms,
                  (gap_ms < 0 ? -gap_ms : gap_ms) > cushion_ms
                      ? "OVER the cushion — it cannot absorb this"
                      : "within the cushion");
    }
    return first;
}

static void deliver_video(SourceCtx* ctx, const DecodedVideoFrame& f) {
    if (!ctx->running.load() || !ctx->playing.load()) return;
    ctx->last_out_pts_ns = f.pts_ns;
    // BEFORE the base, deliberately. A resume landing between these two reads
    // gives this frame the OLD epoch and the NEW base, so it is dropped when it
    // did not have to be — one frame, at a moment the picture is restarting
    // anyway. The other order gives it the NEW epoch and the OLD base, which is
    // a stale timestamp wearing a fresh label: exactly the frame that trips the
    // stall resync and destroys the cushion.
    const uint64_t epoch = ctx->timeline_epoch.load();
    const int64_t first = anchor_pts(ctx, f.pts_ns, true);

    PendingFrame item;
    item.is_video  = true;
    item.epoch     = epoch;
    item.timestamp = multisite::playout_due_ns(
        ctx->playout_base_ns.load(), f.pts_ns, first);
    item.video     = f;              // owns its plane buffer (deep copy)

    ctx->width  = (uint32_t)f.width;
    ctx->height = (uint32_t)f.height;
    enqueue_frame(ctx, std::move(item));
}

static void deliver_audio(SourceCtx* ctx, const DecodedAudioFrame& f) {
    if (!ctx->running.load() || !ctx->playing.load()) return;

    // Who wants this track? This source carries one of them; companion
    // audio-only sources in the same room carry the others. A track nobody has
    // asked for is decoded (it shares the fragment) but not delivered.
    const bool for_us = (f.track_index == ctx->audio_track.load());
    std::vector<obs_source_t*> companions;
    {
        std::lock_guard<std::mutex> lk(g_subs_mtx);
        for (auto* s : g_subs)
            if (s->track == f.track_index && s->room_id == ctx->room_id_for_display)
                companions.push_back(s->source);
    }
    if (!for_us && companions.empty()) return;

    // Anchored once, off the same clock as the video, so every track shares one
    // playout base. The epoch is read before the base for the reason given in
    // deliver_video.
    const uint64_t epoch = ctx->timeline_epoch.load();
    const int64_t first = anchor_pts(ctx, f.pts_ns, false);

    // Packed multi-channel guard. OBS resamples every source to its GLOBAL
    // layout (Settings -> Audio -> Channels). If the stream carries more
    // channels than that layout, OBS downmixes — which for packed audio means
    // the ISOs and click are summed into the programme and silently destroyed.
    // Say so loudly, once, rather than letting it pass.
    if (!ctx->checked_layout.exchange(true)) {
        struct obs_audio_info oai = {};
        if (obs_get_audio_info(&oai)) {
            const int global_ch = (int)get_audio_channels(oai.speakers);
            if (f.channels > global_ch) {
                mlog_error("stream carries %d audio channels but OBS is "
                           "configured for %d — the extra channels will be "
                           "DOWNMIXED and lost. Set Settings -> Audio -> "
                           "Channels to 7.1 on this machine.",
                           f.channels, global_ch);
            } else {
                mlog_info("audio: %d channel(s), OBS global layout %d channel(s)",
                          f.channels, global_ch);
            }
            ctx->audio_channels = f.channels;
        }
        if (ms_layout_for_channels(f.channels) == SPEAKERS_UNKNOWN)
            mlog_error("audio has %d channels, which has no OBS speaker "
                       "layout — 1,2,3,4,5,6 or 8 are supported",
                       f.channels);
    }

    const uint64_t ts = multisite::playout_due_ns(
        ctx->playout_base_ns.load(), f.pts_ns, first);

    for (obs_source_t* dest : companions) {
        PendingFrame item;
        item.is_video  = false;
        item.epoch     = epoch;
        item.timestamp = ts;
        item.audio     = f;
        item.target    = dest;
        enqueue_frame(ctx, std::move(item));
    }
    if (for_us) {
        PendingFrame item;
        item.is_video  = false;
        item.epoch     = epoch;
        item.timestamp = ts;
        item.audio     = f;
        enqueue_frame(ctx, std::move(item));
    }
}

// ── Worker loops ─────────────────────────────────────────────────────────────
static void poll_loop(SourceCtx* ctx) {
    mlog_info("source: poll loop started");
    int64_t next_poll = 0;
    while (ctx->running.load()) {
        // Stopped means stopped all the way down. The thread stays alive so
        // Play can start it again in milliseconds, but it issues no requests:
        // this is what makes Stop stop consuming bandwidth and filling disk,
        // rather than only taking the picture off air. Note this is `stopped`
        // and not `!playing` — loading an event is also not playing, and
        // loading has to download.
        if (ctx->stopped.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            next_poll = 0;        // poll immediately on the way out
            continue;
        }

        const int64_t now = (int64_t)(os_gettime_ns() / 1000000ULL);

        // An operator action (pinning an event, reconfiguring) sets poll_now so
        // the switch happens in the next few milliseconds rather than whenever
        // the interval happens to come round. Waiting out three seconds before
        // even looking is what made Load feel like a dropped click.
        if (now >= next_poll || ctx->poll_now.exchange(false)) {
            next_poll = now + ctx->poll_interval_ms;
            auto sess = get_session(ctx);
            if (!sess) break;
            // poll() does network I/O and can take seconds; never under a lock.
            RoomState st = sess->poll();

            // Pick up how this feed was composited. It comes from the manifest,
            // so it is known before the first frame is decoded and it follows a
            // change of event — a room that sends one camera this week and four
            // next week needs nothing reconfigured at the satellite.
            {
                const TileLayout lay = sess->video_layout();
                if (lay.cols != ctx->tile_layout_now().cols ||
                    lay.rows != ctx->tile_layout_now().rows) {
                    ctx->set_tile_layout(lay);
                    mlog_info("source: feed carries a %s layout — %d picture%s",
                              lay.to_string().c_str(), lay.count(),
                              lay.count() == 1 ? "" : "s");
                }
            }

            // Note where the live edge is and when we noticed it move, so the
            // snapshot can carry it forward at real-time rate between polls
            // instead of reporting a number that steps a segment at a time.
            {
                const uint64_t le = sess->live_edge();
                if (le != ctx->live_edge_seq.load()) {
                    const int64_t at = sess->wall_clock_ms(le);
                    if (at > 0) {
                        ctx->live_edge_seq     = le;
                        // The START of the newest segment, which is what
                        // live_wall_ms() reports and therefore what
                        // set_delay_from_live() measures back from. Using the
                        // end instead would be defensible — that content does
                        // exist — but it would put the readout a segment out
                        // from the delay the operator dialled in, and a
                        // control that disagrees with its own display is worse
                        // than a reference point chosen a beat early.
                        ctx->live_edge_wall_ms = at;
                        ctx->live_edge_seen_ms =
                            (long long)(os_gettime_ns() / 1000000ULL);
                    }
                }
            }

            // Is the chosen event actually READY, not merely selected?
            //
            // Clearing this as soon as poll() returned a state was the bug the
            // operator saw: the switch itself takes a moment, but the wait that
            // matters is the download of the first segments afterwards — and
            // the indication had already gone by then, so Load looked like it
            // did nothing and then everything appeared at once.
            //
            // Load deliberately does not go to air, so no frame will arrive to
            // answer this; having something buffered is what "ready" means.
            if (ctx->loading_event.load() && st != RoomState::Unknown) {
                const bool have_content = sess->buffered_ahead_s() > 0.1 ||
                                          sess->cache().count() > 0;
                const uint64_t started = ctx->action_started_ns.load();
                const bool timed_out = started != 0 &&
                    os_gettime_ns() - started > 30000000000ULL;   // 30 s
                if (have_content || timed_out || st == RoomState::Offline)
                    ctx->loading_event = false;
            }

            // A move made while stopped or held has no frames coming to answer
            // it — nothing is being fed to air — so the delivery path's
            // arrival check never runs and the indication would stay up for
            // ever. Arrival here means the content at the new position is on
            // disk and could be played from.
            if (ctx->seek_target_ms.load() > 0 &&
                (!ctx->playing.load() || ctx->paused.load())) {
                const uint64_t started = ctx->action_started_ns.load();
                const bool timed_out = started != 0 &&
                    os_gettime_ns() - started > 30000000000ULL;   // 30 s
                if (sess->buffered_ahead_s() > 0.1 || timed_out ||
                    st == RoomState::Offline) {
                    ctx->seek_target_ms  = 0;
                    ctx->awaiting_frames = false;
                }
            }

            if (st != ctx->last_room) {
                ctx->last_room = st;
                const char* name =
                    room_state_words(st, sess->was_live_this_session());
                mlog_info("source: room is %s%s", name,
                          st == RoomState::Offline ? " (encoder stopped or unreachable)" : "");
                if ((st == RoomState::Offline || is_vod(st)) &&
                    !sess->last_error().empty())
                    mlog_warn("source: %s", sess->last_error().c_str());
            }
        }

        // Download-ahead runs continuously — this is what keeps filling the
        // cache while playback is paused or behind live.
        int fetched = 0;
        {
            auto sess = get_session(ctx);
            // Downloads block on the network — again, no lock held.
            // A larger batch is safe now that downloads do not hold the
            // state lock: it fills the buffer faster without affecting the UI.
            if (sess) fetched = sess->pump_downloads(8);
        }

        // Resume transient report (BUGS #2). Measurement only: this prints
        // what the first second after a resume actually looked like and
        // changes nothing. Read it as three separate questions —
        //
        //   min lead <= 0        frames were already due when handed over, so
        //                        they left in a burst rather than paced. This
        //                        is the candidate the entry calls "frames
        //                        released immediately because they are already
        //                        due", and it is what OBS reports as lagging.
        //   audio ms >> video ms the streams were handed unequal amounts of
        //                        programme, which is D4's asymmetry showing up
        //                        as a real imbalance rather than a suspicion.
        //   both healthy         the fault is past our handoff, and the anchor
        //                        and the cushion are both exonerated — look at
        //                        what OBS does with timestamps it accepted.
        {
            const uint64_t end = ctx->rw_end_ns.load();
            if (end != 0 && !ctx->rw_reported.load() && os_gettime_ns() > end) {
                ctx->rw_reported = true;
                const int vn = ctx->rw_v_frames.load();
                const int an = ctx->rw_a_frames.load();
                const int64_t vlo = ctx->rw_v_pts_lo.load(), vhi = ctx->rw_v_pts_hi.load();
                const int64_t alo = ctx->rw_a_pts_lo.load(), ahi = ctx->rw_a_pts_hi.load();
                const double v_ms = (vn > 1) ? (double)(vhi - vlo) / 1e6 : 0.0;
                const double a_ms = (an > 1) ? (double)(ahi - alo) / 1e6 : 0.0;
                const int64_t vmin = ctx->rw_v_lead_min_ns.load();
                const int64_t amin = ctx->rw_a_lead_min_ns.load();
                mlog_info("source: first 1s after resume — video %d frame(s), "
                          "%.0f ms of programme, min lead %s%.0f ms; "
                          "audio %d frame(s), %.0f ms of programme, "
                          "min lead %s%.0f ms",
                          vn, v_ms,
                          vn ? "" : "n/a ", vn ? (double)vmin / 1e6 : 0.0,
                          an, a_ms,
                          an ? "" : "n/a ", an ? (double)amin / 1e6 : 0.0);
                if ((vn && vmin <= 0) || (an && amin <= 0))
                    mlog_warn("source: frames were already due at handoff after "
                              "resume (video min %.0f ms, audio min %.0f ms) — "
                              "they went out as a burst, not paced. This is the "
                              "shape OBS reports as audio lagging.",
                              vn ? (double)vmin / 1e6 : 0.0,
                              an ? (double)amin / 1e6 : 0.0);
            }
        }

        // Resume watchdog. A frozen picture after Resume is the failure that
        // is hardest to diagnose from a log, so say plainly whether frames
        // started flowing and, if not, what the state was.
        {
            const uint64_t r = ctx->resumed_at_ns.load();
            if (r != 0 && !ctx->resume_checked.load() &&
                os_gettime_ns() - r > 2000000000ULL) {
                ctx->resume_checked = true;
                const uint64_t delivered =
                    ctx->frames_out.load() - ctx->frames_at_resume.load();
                auto sw = get_session(ctx);
                size_t qdepth = 0;
                { std::lock_guard<std::mutex> qlk(ctx->dq_mtx); qdepth = ctx->dq.size(); }
                const bool at_edge = sw && sw->playback_head() > sw->live_edge();
                if (delivered == 0 && at_edge) {
                    // Not a fault: playback had caught right up, so there is
                    // simply nothing new yet. Say so plainly.
                    mlog_info("source: resumed at the live edge — waiting for "
                              "the main site to publish more (head=%llu "
                              "live=%llu)",
                              (unsigned long long)sw->playback_head(),
                              (unsigned long long)sw->live_edge());
                } else if (delivered == 0) {
                    mlog_error("source: RESUME FAILED — no frames delivered in "
                               "2s. paused=%d play_state=%d head=%llu live=%llu "
                               "queue=%zu buffered=%.1fs decoder=%d",
                               (int)ctx->paused.load(),
                               sw ? (int)sw->play_state() : -1,
                               sw ? (unsigned long long)sw->playback_head() : 0ULL,
                               sw ? (unsigned long long)sw->live_edge() : 0ULL,
                               qdepth,
                               sw ? sw->buffered_ahead_s() : 0.0,
                               (int)ctx->decoder_started.load());
                    if (sw && !sw->last_error().empty())
                        mlog_error("source: last error: %s",
                                   sw->last_error().c_str());
                } else {
                    mlog_info("source: resume delivered %llu frames in 2s — "
                              "playing", (unsigned long long)delivered);
                }
            }
        }

        // Event list refresh, on this thread rather than the operator's. A
        // listing plus one manifest per event is seconds of network work; the
        // dock sets a flag and reads the result whenever it arrives.
        if (ctx->events_refresh_wanted.exchange(false)) {
            std::shared_ptr<EventCatalog> cat;
            { std::lock_guard<std::mutex> lk(ctx->obj_mtx); cat = ctx->catalog; }
            if (cat) {
                ctx->events_refreshing = true;
                cat->refresh();
                EventListing listing;
                listing.listed_once   = true;
                listing.fallback_scan = cat->used_fallback_scan();
                listing.skipped       = cat->skipped();
                listing.error         = cat->last_error();
                for (const auto& e : cat->events()) {
                    EventEntry row;
                    row.event_id   = e.event_id;
                    row.name       = e.name;
                    row.started_ms = (long long)e.started_at_ms;
                    row.duration_s = (long long)e.duration_s;
                    row.state      = (int)e.state;
                    listing.events.push_back(std::move(row));
                }
                const size_t count = listing.events.size();
                {
                    std::lock_guard<std::mutex> lk(ctx->events_mtx);
                    ctx->events_cache = std::move(listing);
                }
                ctx->events_listed_once = true;
                ctx->events_refreshing  = false;
                mlog_info("source: event list refreshed — %zu event(s)%s", count,
                          cat->used_fallback_scan()
                              ? " (scanned events/: these predate the room index)"
                              : "");
            } else {
                // No cloud transport, so there is no catalogue and there never
                // will be one: recordings are listed out of a bucket, and the
                // LAN side only ever knows about the event that is on air. Left
                // unsaid this is an empty box sitting on "Looking for
                // recordings…" for ever, which reads as a broken list rather
                // than as "there is nothing here that could list anything".
                //
                // An event published with cloud delivery off cannot appear in
                // any recordings list anywhere — not on a satellite, not on the
                // machine that recorded it — and the operator is the one who
                // turned that off, so this is where they find out.
                EventListing listing;
                listing.listed_once = true;
                listing.no_catalog  = true;
                {
                    std::lock_guard<std::mutex> lk(ctx->events_mtx);
                    ctx->events_cache = std::move(listing);
                }
                ctx->events_listed_once = true;
            }
        }

        // Periodic status so an operator can see it working.
        if (now - ctx->last_status_log_ms > 30000) {
            ctx->last_status_log_ms = now;
            auto sess2 = get_session(ctx);
            if (sess2) {
                auto& s = sess2->stats();
                mlog_info("source: head=%llu live=%llu behind=%.0fs "
                          "buffered=%.0fs cached=%zu downloaded=%llu "
                          "frames_out=%llu",
                          (unsigned long long)sess2->playback_head(),
                          (unsigned long long)sess2->live_edge(),
                          sess2->behind_live_s(),
                          sess2->buffered_ahead_s(),
                          sess2->cache().count(),
                          (unsigned long long)s.downloaded,
                          (unsigned long long)ctx->frames_out.load());
            }

            // Delivery lead: how much time each stream had in hand at the
            // handout, over the interval since the last report.
            //
            // Healthy looks like both streams sitting near kMaxDeliveryLeadNs
            // with a min that stays comfortably positive. The diagnosis is in
            // the COMPARISON, not the absolute values:
            //
            //   video min decaying while audio holds   video is arriving late
            //                                          — that is the lipsync,
            //                                          and the sticky timeline
            //   both decaying together                 delivery is behind as a
            //                                          whole; look upstream at
            //                                          fetch or decode
            //   both steady and drops zero             we are handing OBS good
            //                                          frames on time, so the
            //                                          fault is past our
            //                                          handoff (async
            //                                          buffering)
            //
            // Drops are absolute totals, not windowed: what matters is whether
            // they are moving at all, and which stream.
            const uint64_t vn = ctx->lead_video_count.exchange(0);
            const uint64_t an = ctx->lead_audio_count.exchange(0);
            if (vn > 0 || an > 0) {
                const int64_t vsum = ctx->lead_video_sum_ns.exchange(0);
                const int64_t asum = ctx->lead_audio_sum_ns.exchange(0);
                const int64_t vmin = ctx->lead_video_min_ns.exchange(INT64_MAX);
                const int64_t amin = ctx->lead_audio_min_ns.exchange(INT64_MAX);
                mlog_info("source: lead video mean=%+.0fms min=%+.0fms (%llu) | "
                          "audio mean=%+.0fms min=%+.0fms (%llu) | "
                          "dropped %llu v / %llu a",
                          vn ? (double)vsum / (double)vn / 1e6 : 0.0,
                          vn ? (double)vmin / 1e6 : 0.0,
                          (unsigned long long)vn,
                          an ? (double)asum / (double)an / 1e6 : 0.0,
                          an ? (double)amin / 1e6 : 0.0,
                          (unsigned long long)an,
                          (unsigned long long)ctx->dropped_video.load(),
                          (unsigned long long)ctx->dropped_audio.load());
            }
        }

        // Always yield briefly, even when there is more to fetch. A tight
        // download loop starves the UI on Windows, where the running thread
        // is favoured for a contended lock; a few milliseconds costs nothing
        // against a segment download but guarantees the interface stays live.
        std::this_thread::sleep_for(
            std::chrono::milliseconds(fetched == 0 ? 100 : 5));
    }
    mlog_info("source: poll loop exiting");
}

static void feed_loop(SourceCtx* ctx) {
    mlog_info("source: feed loop started");
    while (ctx->running.load()) {
        // While held, stop pulling and feeding entirely. Otherwise the decoder
        // and delivery queues fill, push_fragment blocks, and Resume cannot get
        // in — which froze OBS.
        if (ctx->paused.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        // Loading fills the buffer; only Play sends anything to air. This is
        // the difference between a feed that is ready and a feed that is out.
        if (!ctx->playing.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        auto sess = get_session(ctx);
        if (!sess) break;
        std::optional<PlayableSegment> seg;
        {
            if (sess->play_state() == PlayState::Stopped) sess->start();
            seg = sess->next_segment();
        }

        if (!seg) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        // A jump (seek / jump-to-live / new event) means the next fragment
        // belongs to a different timeline. Tear the decoder down so it
        // restarts from the re-sent init segment; feeding across a jump
        // produces out-of-order timestamps and a glitched picture.
        {
            uint64_t d = sess->discontinuity_id();
            if (d != ctx->seen_discontinuity) {
                ctx->seen_discontinuity = d;
                if (ctx->decoder_started.load()) {
                    mlog_info("source: playback jumped — restarting decoder");
                    // Same teardown the seek path performs; shared so the two
                    // cannot drift apart. A seek reaches this having already
                    // done it, and it is idempotent.
                    ctx->release_decoder_for_restart();
                    {
                        std::lock_guard<std::mutex> qlk(ctx->dq_mtx);
                        ctx->dq.clear();         // stale frames from the old timeline
                    }
                    ctx->dq_cv.notify_all();
                }
            }
        }

        // The init segment arrives with the first fragment; it must open the
        // decoder before any media is pushed.
        if (!ctx->decoder_started.load()) {
            if (seg->init.empty()) {
                mlog_error("source: first segment arrived without an init "
                           "segment — cannot start decoding");
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                continue;
            }
            auto dec = std::make_shared<CmafDecoder>();
            dec->on_video([ctx](const DecodedVideoFrame& f) { deliver_video(ctx, f); });
            dec->on_audio([ctx](const DecodedAudioFrame& f) { deliver_audio(ctx, f); });
            if (!dec->start(seg->init)) {
                mlog_error("source: decoder failed to start: %s",
                           dec->error().c_str());
                continue;
            }
            { std::lock_guard<std::mutex> lk(ctx->obj_mtx); ctx->decoder = dec; }
            ctx->decoder_started = true;
            ctx->feed_start_ns = os_gettime_ns();
            ctx->pushed_media_ns = 0;
            // Also on a first start, not only on a restart: this is reached
            // without going through the teardown above.
            ctx->pts_wall_offset_ms   = SourceCtx::kOffsetUnset;
            ctx->restart_wall_ms      = 0;
            ctx->restart_wall_pending = true;
            mlog_info("source: decoder started (init %zu bytes)",
                      seg->init.size());
        }

        // Feed at playout rate, keeping a small lead so the decoder always has
        // work but never runs seconds ahead of the wall clock.
        // Fragments must be decoded comfortably before their content is due,
        // or the first frames of each fragment arrive late (visible as a burst
        // of lateness at every fragment boundary).
        static constexpr uint64_t kFeedLeadNs = 2500000000ULL;   // 2.5 s
        while (ctx->running.load()) {
            const uint64_t elapsed = os_gettime_ns() - ctx->feed_start_ns;
            if (ctx->pushed_media_ns <= elapsed + kFeedLeadNs) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!ctx->running.load()) break;

        // The FIRST fragment since the decoder started defines the media
        // timeline's offset from the wall clock. Later fragments must not
        // touch it: their wall times are correct, but by then the delivery
        // thread is several seconds behind and would pair them with the wrong
        // frames — which is the bug this replaced.
        if (ctx->restart_wall_pending.exchange(false)) {
            ctx->restart_wall_ms = (long long)seg->starts_at_ms;
            // Every displayed time and every cue position hangs off this
            // pairing, so it matters whether the wall time was recorded by the
            // encoder or worked out from seq * nominal duration. The estimate
            // assumes every segment is exactly the nominal length; two pins
            // taken 714 s apart on one recording disagreed by 7.94 s, which is
            // 1.11% and would put a cue a minute out by the end of a service.
            ctx->restart_wall_estimated = seg->starts_at_estimated;
        }
        // Arming the skip is all the feed loop does here now. The base it
        // measures from is claimed by the delivery loop when it picks this up,
        // from a frame it has actually seen — rather than being reset from this
        // thread, seconds ahead, which is how it used to move mid-skip.
        if (seg->skip_to_ms > 0)
            ctx->skip_until_pts_ns = seg->skip_to_ms * 1000000LL;

        // push_fragment blocks when the decoder is full — deliberately not
        // under a lock. It returns false only when the decoder has stopped
        // consuming (BUGS.md entry 0); say so rather than freezing silently.
        if (auto dec = get_decoder(ctx)) {
            if (!dec->push_fragment(seg->media, seg->seq))
                mlog_warn("source: decoder stopped consuming video (%s)",
                          dec->error().c_str());
        }
        ctx->pushed_media_ns += (uint64_t)(seg->duration_s * 1e9);
    }
    mlog_info("source: feed loop exiting");
}

// ── OBS callbacks ────────────────────────────────────────────────────────────
static const char* src_name(void*) { return obs_module_text("Multisite.Source"); }

static uint32_t src_width(void* data) {
    auto* ctx = static_cast<SourceCtx*>(data);
    return ctx->width;
}
static uint32_t src_height(void* data) {
    auto* ctx = static_cast<SourceCtx*>(data);
    return ctx->height;
}

static void stop_workers(SourceCtx* ctx) {
    ctx->flushing = true;             // release any blocked decoder callback
    if (!ctx->running.exchange(false)) { ctx->flushing = false; return; }
    ctx->dq_cv.notify_all();      // release anyone blocked on the queue

    // Abort whatever network request poll_loop or feed_loop happens to be
    // mid-flight on, right now, rather than letting it run to its own
    // timeout. Without this, tearing a source down while a request was in
    // flight blocked whichever thread called stop_workers — usually OBS's
    // own UI thread, during scene teardown or Quit — for as long as that one
    // request had left. Long enough, in practice, for an operator to see OBS
    // stop responding and force-quit it, which OBS then reports as a crash
    // on the next launch. The thread is still joined below either way; this
    // just makes the join fast instead of a race against a 30-second curl
    // timeout.
    {
        std::shared_ptr<S3Transport> tx;
        std::shared_ptr<LanTransport> lan_tx;
        { std::lock_guard<std::mutex> lk(ctx->obj_mtx);
          tx = ctx->transport; lan_tx = ctx->lan_transport; }
        if (tx) tx->cancel_pending();
        if (lan_tx) lan_tx->cancel_pending();
    }

    if (ctx->poll_thread.joinable())    ctx->poll_thread.join();
    if (ctx->feed_thread.joinable())    ctx->feed_thread.join();
    if (ctx->deliver_thread.joinable()) ctx->deliver_thread.join();
    {
        std::lock_guard<std::mutex> lk(ctx->dq_mtx);
        ctx->dq.clear();
    }
    {
        std::shared_ptr<CmafDecoder> old;
        { std::lock_guard<std::mutex> lk(ctx->obj_mtx);
          old = ctx->decoder; ctx->decoder.reset(); }
        if (old) old->stop();          // blocks; outside the lock
    }
    ctx->decoder_started = false;
    ctx->first_pts_ns = -1;
    ctx->flushing = false;
}

static void src_update(void* data, obs_data_t* s) {
    auto* ctx = static_cast<SourceCtx*>(data);
    std::lock_guard<std::mutex> life(ctx->lifecycle_mtx);
    unregister_decoder_controls(ctx);
    stop_workers(ctx);

    // Storage is machine-wide and lives in ONE place: the decoder dock's
    // settings dialog (see decoder_settings.h).
    //
    // These fields used to exist in the source properties as well, with the
    // source's copy winning when filled. Two editable copies of a credential is
    // a trap: changing it in the dock appeared to do nothing, because a value
    // saved in the scene silently overrode it, and there was no indication
    // anywhere that this was happening. The properties fields are gone.
    DecoderSettings shared = decoder_settings();

    // One-time migration for scenes saved before that change. Anything still
    // held in the source is lifted into the machine settings (unless those are
    // already configured, which means the dock is the newer truth), and then
    // cleared from the source so it can never shadow the dock again.
    {
        auto own = [&](const char* key) {
            const char* v = obs_data_get_string(s, key);
            return (v && *v) ? std::string(v) : std::string();
        };
        const std::string legacy_bucket   = own(S_BUCKET);
        const std::string legacy_endpoint = own(S_ENDPOINT);
        const std::string legacy_account  = own(S_ACCOUNT);
        const bool has_legacy = !legacy_bucket.empty() ||
                                !legacy_endpoint.empty() || !legacy_account.empty();

        if (has_legacy) {
            // Specifically cloud, not configured() in general: a LAN-only
            // machine (cloud_configured() false, lan_configured() true)
            // should still absorb legacy per-scene cloud credentials into
            // the shared settings, exactly as one with nothing configured
            // at all would.
            if (!shared.cloud_configured()) {
                DecoderSettings upd  = shared;
                upd.endpoint_host     = legacy_endpoint;
                upd.r2_account_id     = legacy_account;
                upd.bucket            = legacy_bucket;
                upd.access_key_id     = own(S_KEYID);
                upd.secret_access_key = own(S_SECRET);
                if (!own(S_REGION).empty()) upd.region = own(S_REGION);
                set_decoder_settings(upd);
                shared = decoder_settings();
                mlog_info("source: moved this scene's storage settings into the "
                          "machine-wide decoder settings — they are now edited "
                          "in the Multisite Decoder dock");
            } else {
                mlog_info("source: discarding storage settings saved in this "
                          "scene; the decoder dock's settings are used instead");
            }
            // Clear either way: a stale copy in the scene is exactly what made
            // dock edits appear to have no effect.
            for (const char* key : { S_ENDPOINT, S_ACCOUNT, S_BUCKET,
                                     S_KEYID, S_SECRET, S_REGION })
                obs_data_set_string(s, key, "");
        }
    }

    // One-time migration for the feed name, with the same reasoning and shape
    // as storage above: a room typed into an older scene becomes the machine
    // default unless the dock already names a different one, and is then
    // cleared from the scene so it can never shadow the dock again.
    {
        const char* own_room = obs_data_get_string(s, S_ROOM);
        const std::string scene_room = (own_room && *own_room) ? own_room : "";
        if (!scene_room.empty()) {
            const bool dock_is_default =
                shared.room_id.empty() || shared.room_id == "main-auditorium";
            if (dock_is_default && scene_room != shared.room_id) {
                DecoderSettings upd = shared;
                upd.room_id = scene_room;
                set_decoder_settings(upd);
                shared = decoder_settings();
                mlog_info("source: moved this scene's feed name '%s' into the "
                          "machine-wide decoder settings (Multisite Decoder "
                          "dock → Settings)", scene_room.c_str());
            } else if (!dock_is_default && scene_room != shared.room_id) {
                mlog_info("source: discarding feed name '%s' saved in this "
                          "scene; the decoder dock's '%s' is used instead",
                          scene_room.c_str(), shared.room_id.c_str());
            }
            obs_data_set_string(s, S_ROOM, "");
        }
    }

    S3Config s3;
    s3.endpoint_host     = shared.endpoint_host;
    s3.r2_account_id     = shared.r2_account_id;
    s3.bucket            = shared.bucket;
    s3.access_key_id     = shared.access_key_id;
    s3.secret_access_key = shared.secret_access_key;
    s3.region            = shared.region;

    // Room and receive tuning are machine-wide as well, and for the same reason
    // storage is: a second editable copy of one value per scene meant a dock
    // edit could be silently overridden by whatever the scene had saved. Every
    // source on this machine follows the dock's feed name and takes its safety
    // buffer, poll interval and rewind depth from the dock too. The one-time
    // migration above has already lifted a feed name saved in an older scene.
    DecoderConfig dc;
    dc.room_id              = shared.room_id;
    dc.prebuffer_segments   = shared.prebuffer_segments;
    dc.keep_behind_segments = shared.keep_behind_segments;
    dc.buffer_minutes       = shared.buffer_minutes;
    dc.start_buffer_seconds = shared.start_buffer_seconds;
    ctx->poll_interval_ms   = shared.poll_interval_ms;
    ctx->audio_track        = (int)obs_data_get_int(s, S_ATRACK);

    if (!shared.configured()) {
        mlog_warn("source: not configured yet — enter storage details or a "
                  "LAN host in the Multisite Decoder dock (Settings)");
        return;
    }

    // Where downloaded segments are cached: the dock's setting when given,
    // otherwise the fixed location under OBS's plugin config this has always
    // used.
    dc.cache_dir = shared.cache_dir;
    if (dc.cache_dir.empty()) {
        char* cachedir = obs_module_config_path("cache");
        dc.cache_dir = cachedir ? cachedir : "./multisite_cache";
        bfree(cachedir);
    }

    // Cues: who this box is, and whether it may drop one. A box can author only
    // when it knows its own name and has somewhere to put the cue; a read-only
    // credential fails the write loudly rather than silently, so this does not
    // need to guess the key's scope.
    dc.author_name     = shared.site_name;
    dc.can_author_cues = !shared.site_name.empty() && shared.configured();

    {
        // Cloud, LAN, both, or — since shared.configured() already checked
        // at least one is set — exactly one of the two below is always real.
        std::shared_ptr<S3Transport> tx;
        std::shared_ptr<LanTransport> lan_tx;
        std::shared_ptr<FallbackTransport> fb;
        if (shared.cloud_configured())
            tx = std::make_shared<S3Transport>(s3);
        if (shared.lan_configured()) {
            LanTransportConfig lcfg;
            lcfg.host       = shared.lan_host;
            lcfg.port       = shared.lan_port;
            lcfg.auth_token = shared.lan_auth_token;
            lan_tx = std::make_shared<LanTransport>(lcfg);
        }

        // The second bucket, when this machine has one configured
        // (PROJECT-SCOPE.md §10 Phase 9). Composed UNDER the LAN fallback, so
        // the preference order is LAN → primary cloud → second cloud, decided
        // per request: an event's later segments may exist only in the second
        // bucket after a write-side failover, and a 404 from a reachable
        // primary must fall through rather than be read as the primary failing.
        std::shared_ptr<S3Transport> tx2;
        std::shared_ptr<MirrorReadTransport> mirror_tx;
        Transport* cloud = tx.get();
        if (tx) {
            S3Config sc2;
            if (secondary_s3_config(sc2)) {
                tx2 = std::make_shared<S3Transport>(sc2);
                mirror_tx = std::make_shared<MirrorReadTransport>(*tx, *tx2);
                cloud = mirror_tx.get();
                mlog_info("source: second bucket configured — reads will fall "
                          "back to it per request");
            }
        }

        if (lan_tx && !tx) {
            // A cue goes to the encoder's hub only when there is NO bucket to
            // write to. With cloud configured the cue is written directly, so a
            // configured-but-unreachable LAN host — a box tested at home, an
            // encoder that is switched off — can never take cue authoring down
            // with it.
            dc.cue_hub = [lan_tx](const std::string& author,
                                  const std::string& label,
                                  std::string& merged, std::string& error) {
                return lan_tx->publish_cue(author, label, merged, error);
            };
        }

        Transport* active = nullptr;
        if (lan_tx && tx) {
            // Preference and fallback (§8.7): LAN answers when it can, cloud
            // otherwise, decided per request — see fallback_transport.h.
            fb = std::make_shared<FallbackTransport>(*lan_tx, *cloud);
            active = fb.get();
        } else if (lan_tx) {
            active = lan_tx.get();
        } else {
            active = cloud;
        }
        auto ses = std::make_shared<DecoderSession>(dc, *active);

        // Event browsing (§7.5) is inherently cloud-only — there is no such
        // thing as "list every event a LAN endpoint has ever served"; it
        // only ever knows about whichever one is live right now. No catalog
        // at all when cloud isn't configured, rather than one that can only
        // ever come back empty and reads as a room with no history.
        std::shared_ptr<EventCatalog> cat;
        if (tx) {
            // The catalog shares the cloud transport and, deliberately, the
            // same staleness rule as the decoder: the list and the player
            // must never disagree about whether an event is still running.
            CatalogConfig cc;
            cc.room_id        = dc.room_id;
            cc.stale_after_ms = dc.stale_after_ms;
            cat = std::make_shared<EventCatalog>(cc, *cloud);
        }
        std::lock_guard<std::mutex> lk(ctx->obj_mtx);
        ctx->transport     = tx;
        ctx->lan_transport = lan_tx;
        ctx->fallback      = fb;
        ctx->mirror_read   = mirror_tx;
        ctx->session       = ses;
        ctx->catalog       = cat;
    }
    {
        // Start with a clean list: this may be a different room entirely.
        std::lock_guard<std::mutex> lk(ctx->events_mtx);
        ctx->events_cache = EventListing{};
    }
    ctx->events_listed_once = false;
    ctx->events_refresh_wanted = true;   // populate the dock without a click

    // Set BEFORE the workers start: deliver_audio matches companion audio
    // sources against this room, and a companion would otherwise miss the
    // first fragments while it was still empty.
    ctx->room_id_for_display = dc.room_id;

    register_decoder_controls(ctx);      // hotkeys act on this source
    register_owner(ctx);
    ctx->running = true;
    ctx->deliver_thread = std::thread(deliver_loop, ctx);
    ctx->poll_thread    = std::thread(poll_loop, ctx);
    ctx->feed_thread    = std::thread(feed_loop, ctx);
    mlog_info("source: watching room '%s' (prebuffer %d segments, start after "
              "%ds buffered, poll %dms)",
              dc.room_id.c_str(), dc.prebuffer_segments,
              dc.start_buffer_seconds, ctx->poll_interval_ms);
}

static void* src_create(obs_data_t* settings, obs_source_t* source) {
    auto* ctx = new SourceCtx();
    ctx->source = source;
    src_update(ctx, settings);
    return ctx;
}

static void src_destroy(void* data) {
    auto* ctx = static_cast<SourceCtx*>(data);
    unregister_owner(ctx);
    {
        std::lock_guard<std::mutex> life(ctx->lifecycle_mtx);
        unregister_decoder_controls(ctx);
        stop_workers(ctx);
    }
    {
        std::lock_guard<std::mutex> lk(ctx->obj_mtx);
        // session first: it holds a Transport& into whichever of these it
        // was actually built against (see the construction site above), so
        // nothing may be freed before it is.
        ctx->session.reset();
        ctx->fallback.reset();
        ctx->lan_transport.reset();
        ctx->transport.reset();
    }
    delete ctx;
}

static void src_defaults(obs_data_t* s) {
    obs_data_set_default_int(s, S_ATRACK, 0);
    // No defaults for storage, feed name or the receive tuning: none of them
    // are edited here any more, and a default would make every source look as
    // though it carried a value to migrate.
}

// ── DecoderControls: one implementation, shared by buttons and hotkeys ───────
// These are called from the UI thread. They must not block, and must not
// contend with a worker thread that is mid-network-call — so they take a
// reference to the session under a short lock and act through it.
void SourceCtx::pause() {
    auto sess = get_session(this);
    if (!sess) return;
    // Order matters: stop delivery first so the picture holds immediately,
    // then stop pulling new segments.
    paused = true;
    pause_started_ns = os_gettime_ns();
    sess->pause();
    // The pts on screen, and what the session is serving. On resume the log
    // already prints the pts of the first frame after the hold, so the two
    // together say whether the picture moved — which is the question, and it
    // could not be answered from a figure seconds stale.
    size_t queued = 0;
    { std::lock_guard<std::mutex> qlk(dq_mtx); queued = dq.size(); }
    mlog_info("source: PAUSED at segment %llu — on screen %.3fs, %zu frame(s) "
              "queued, cache keeps filling",
              (unsigned long long)sess->playback_head(),
              (double)last_out_pts_ns.load() / 1e9, queued);
}

void SourceCtx::resume() {
    auto sess = get_session(this);
    if (!sess) { mlog_warn("source: resume ignored — no session"); return; }

    // Re-anchor rather than shift.
    //
    // The previous approach advanced the playout clock by the paused duration
    // and rewrote the timestamps of frames already queued. That has several
    // ways to fail quietly — if the pause timestamp was already consumed, if
    // the session was not actually in the Paused state, or if the stall
    // resync fired in between — and any of them leaves frames that never
    // become due, so the picture stays frozen.
    //
    // Jump-to-live works reliably because it treats the position change as a
    // discontinuity: drop what is queued and let the next decoded frame
    // re-anchor the clock. Resume now does the same. The cost is the handful
    // of already-decoded frames still in the queue (under half a second);
    // playback continues from the same point in the programme because the
    // decoder itself is not restarted.
    pause_started_ns = 0;

    // WHERE THE PICTURE STOPPED. This is the whole of the DVR contract, and
    // until now it was not honoured: a hold froze delivery and stopped fetching
    // new segments, but the DECODER carried on decoding the fragments it
    // already held. Those frames arrived at a queue nothing was draining,
    // waited 250 ms in enqueue_frame, and were dropped. Resume then continued
    // from wherever the decoder had reached, so the programme thrown away while
    // holding was simply lost — measured at 0.2 s after a 1.4 s hold, 1.09 s
    // after 11 s, and 3.90 s after 67 s, each matching the frames dropped
    // across that hold to within a frame or two.
    //
    // A recorder freezes the READ head and keeps the write head going. So
    // resume seeks back to the frame that was on screen, which makes the
    // resume position an asserted quantity rather than a consequence of how
    // long the decoder was left running. It costs a decoder restart, which a
    // seek already pays, and it is the same call a jog makes — so it also
    // re-pins the media clock against the fragment it actually landed on,
    // instead of pairing this position's pts with the FIRST fragment's wall
    // time and walking every displayed clock backwards.
    const int64_t resume_pts_ns = last_out_pts_ns.load();

    // Measured before anything clears the queue (BUGS #2 / D4).
    size_t dropped = 0;
    double v_span_ms = 0.0, a_span_ms = 0.0;
    size_t v_n = 0, a_n = 0;
    {
        std::lock_guard<std::mutex> qlk(dq_mtx);
        int64_t v_lo = INT64_MAX, v_hi = INT64_MIN;
        int64_t a_lo = INT64_MAX, a_hi = INT64_MIN;
        for (const auto& q : dq) {
            const int64_t p = q.is_video ? q.video.pts_ns : q.audio.pts_ns;
            if (q.is_video) { ++v_n; if (p < v_lo) v_lo = p; if (p > v_hi) v_hi = p; }
            else            { ++a_n; if (p < a_lo) a_lo = p; if (p > a_hi) a_hi = p; }
        }
        if (v_n > 1) v_span_ms = (double)(v_hi - v_lo) / 1e6;
        if (a_n > 1) a_span_ms = (double)(a_hi - a_lo) / 1e6;
        dropped = dq.size();
    }

    // Re-anchor, but do NOT seek back.
    //
    // Seeking to the held position was tried (option (a)) and is worse than
    // what it replaced. Seeking INTO a fragment means decoding and discarding
    // from that fragment's start to the target, which measured 3.6 s of frozen
    // picture for a 3.3 s skip and would be a whole segment at worst — the same
    // multi-second freeze that got 82b4183 and 12a54e4 reverted. It also landed
    // on the wrong fragment: 940.967 s requested, 948.013 s anchored, seven
    // seconds forward, because the seek resolves through a media->wall mapping
    // that is itself drifting (see the origin walk in BUGS #2).
    //
    // With the producer no longer discarding while held, the decoder has not
    // advanced past the hold, so there is nothing to seek BACK to. What still
    // has to happen is the playout clock: wall time moved on during the hold
    // and media time did not, so the mapping is re-anchored on the next frame.
    {
        std::lock_guard<std::mutex> qlk(dq_mtx);
        dq.clear();
        // A frame already popped into the delivery loop's hand is from before
        // the hold and would put the clock back where the hold started.
        timeline_epoch++;
    }
    first_pts_ns = -1;
    paused = false;                // delivery and feeding resume at once
    sess->resume();
    dq_cv.notify_all();            // release the producer parked above

    resumed_at_ns = os_gettime_ns();
    frames_at_resume = frames_out.load();

    // Arm the resume-transient window (BUGS #2). One second: long enough to
    // cover the 500 ms cushion and the burst behind it, short enough that the
    // steady state does not dilute the reading.
    rw_end_ns        = resumed_at_ns.load() + 1000000000ULL;
    rw_reported      = false;
    rw_v_frames      = 0;  rw_a_frames = 0;
    rw_v_lead_min_ns = INT64_MAX; rw_a_lead_min_ns = INT64_MAX;
    rw_v_pts_lo = -1; rw_v_pts_hi = -1;
    rw_a_pts_lo = -1; rw_a_pts_hi = -1;

    mlog_info("source: queue at resume held video %zu frame(s)/%.0f ms, "
              "audio %zu frame(s)/%.0f ms (caps are counts: 12 video, 48 audio)",
              v_n, v_span_ms, a_n, a_span_ms);

    mlog_info("source: RESUMED at %.0fs behind live (state=%d, %zu queued "
              "frame(s) discarded, clock re-anchoring) — held from %.3fs",
              sess->behind_live_s(), (int)sess->play_state(), dropped,
              (double)resume_pts_ns / 1e9);
}

void SourceCtx::toggle_pause() {
    if (paused.load()) resume(); else pause();
}

void SourceCtx::jump_to_live() {
    auto sess = get_session(this);
    if (!sess) return;
    sess->jump_to_live();
    // The same re-anchor a seek does. Jumping to live is a seek to the live
    // edge; it only ever looked different because it did none of this.
    after_jump((long long)sess->playhead_wall_ms());
    mlog_info("source: JUMPED TO LIVE (segment %llu)",
              (unsigned long long)sess->playback_head());
}

void SourceCtx::log_status() {
    auto sess = get_session(this);
    if (!sess) { mlog_info("source: not configured"); return; }
    auto& st = sess->stats();
    auto cur = sess->current_marker();
    mlog_info("source status: room=%d head=%llu live=%llu behind=%.0fs "
              "buffered=%.0fs cached=%zu downloaded=%llu dl_fail=%llu "
              "checksum_fail=%llu served=%llu frames_out=%llu%s%s",
              (int)sess->room_state(),
              (unsigned long long)sess->playback_head(),
              (unsigned long long)sess->live_edge(),
              sess->behind_live_s(),
              sess->buffered_ahead_s(),
              sess->cache().count(),
              (unsigned long long)st.downloaded,
              (unsigned long long)st.download_failures,
              (unsigned long long)st.checksum_failures,
              (unsigned long long)st.served,
              (unsigned long long)frames_out.load(),
              cur ? " marker=" : "",
              cur ? cur->label.c_str() : "");
    if (!sess->last_error().empty())
        mlog_info("source last error: %s", sess->last_error().c_str());
}

void SourceCtx::snapshot(DecoderSnapshot& out) const {
    out.room_id = room_id_for_display;
    out.paused  = paused.load();
    out.audio_channels = audio_channels.load();
    {
        // This box's clock against the store's, from the Date header on traffic
        // we are already making. Reported so the dock can warn before a skewed
        // clock causes confusion; 0 means "not observed yet".
        std::shared_ptr<S3Transport> tx;
        { std::lock_guard<std::mutex> lk(obj_mtx); tx = transport; }
        if (tx) out.clock_skew_ms = (long long)tx->server_clock_skew_ms();
    }
    std::shared_ptr<DecoderSession> sess;
    { std::lock_guard<std::mutex> lk(obj_mtx); sess = session; }
    if (!sess) return;
    out.room_state       = (int)sess->room_state();
    out.started_ms       = sess->event_started_ms();
    out.ended            = sess->event_ended();
    out.at_end           = sess->at_end();
    out.was_live         = sess->was_live_this_session();
    // The session's own answer to "can Play work", not the dock's guess at it.
    out.ready_to_play    = sess->can_start_now();
    out.plays_as_recording = sess->plays_as_recording();
    out.gate_s           = sess->start_gate_s();
    out.ready_buffer_s   = sess->ready_buffer_s();
    out.interrupted      = sess->was_interrupted();
    out.event_id         = sess->event_id();
    out.pinned_event_id  = sess->pinned_event();
    out.live_elsewhere   = sess->live_elsewhere();
    out.live_event_id    = sess->live_event_id();
    out.end_ms           = sess->end_wall_ms();
    out.total_ms = (out.ended && out.end_ms > 0 && out.started_ms > 0 &&
                    out.end_ms > out.started_ms)
                     ? (out.end_ms - out.started_ms) : 0;
    out.head             = sess->playback_head();
    out.live_edge        = sess->live_edge();
    out.first_available  = sess->earliest_available();
    // The segment on screen, when the host knows it; the serving head
    // otherwise. The timeline is anchored to this, so its playhead is the
    // picture and not a clock reading a seek may have re-pinned.
    out.playhead_seq = on_screen_seq.load();
    if (out.playhead_seq == 0) out.playhead_seq = sess->playback_head();
    out.segment_duration_s = sess->segment_duration_s();
    // Behind live, continuously rather than a segment at a time. Both ends are
    // now real times: the playhead comes from the media clock, and the live
    // edge is carried forward from the last time it was seen to move. The
    // extrapolation is bounded by the same rule the dock's playhead uses — two
    // segments, so a stalled poll settles just past the last known edge rather
    // than running away and reporting a delay that is not there.
    out.behind_live_s    = sess->behind_live_s();      // fallback below
    out.buffered_ahead_s = sess->buffered_ahead_s();
    out.start_buffer_s   = sess->start_buffer_seconds();
    {
        // Longest contiguous run of cached segments. This is the number that
        // grows while the start buffer fills — the pre-Play state where a head
        // has not been seated yet and buffered_ahead_s is still zero.
        double best = 0.0;
        for (const auto& r : sess->cached_ranges())
            if (r.second >= r.first)
                best = std::max(best, (double)(r.second - r.first + 1));
        out.buffered_span_s = best * sess->segment_duration_s();
    }
    out.cached           = sess->cache().count();
    out.last_error       = sess->last_error();
    out.link_health      = (int)sess->link_health();
    out.link_known       = sess->link_known();
    {
        auto layout = sess->audio_layout();
        if (!layout.empty()) {
            out.audio_track_label = layout.front().label;
            out.channel_labels    = layout.front().channel_labels;
        }
    }
    out.playing = playing.load();
    out.stopped = stopped.load();
    out.locked  = controls_locked.load();
    {
        // The convention in this file: take the reference under obj_mtx and
        // release it before doing anything with it, so the UI thread never
        // holds that lock while the download thread wants it.
        std::shared_ptr<S3Transport> tx;
        std::shared_ptr<LanTransport> lan_tx;
        std::shared_ptr<FallbackTransport> fb;
        { std::lock_guard<std::mutex> lk(obj_mtx);
          tx = transport; lan_tx = lan_transport; fb = fallback; }
        if (tx) {
            out.colo                 = tx->last_colo();
            out.storage_host         = tx->host();
            out.download_bytes_per_s = tx->observed_download_bytes_per_s();
            out.download_samples     = tx->download_samples();
        }
        out.lan_configured = (lan_tx != nullptr);
        // With both configured, FallbackTransport tracks which path the most
        // recent request actually took. LAN alone (no fallback object at all
        // — see the construction site) has nothing to ask that of, so ask
        // the LAN transport itself whether it's actually reachable — never
        // just assume "configured" means "working".
        out.lan_active = fb ? fb->last_get_was_primary()
                             : (lan_tx && lan_tx->last_request_reached_server());
        // Which end the reads are actually coming from, when there are two.
        std::shared_ptr<MirrorReadTransport> mread;
        { std::lock_guard<std::mutex> lk(obj_mtx); mread = mirror_read; }
        out.reading_secondary = mread && mread->preferring_secondary();
    }
    out.loading        = loading_event.load();
    out.seek_target_ms = seek_target_ms.load();
    // Playing, but nothing has reached OBS yet — the buffer is still filling.
    out.buffering      = playing.load() && awaiting_frames.load();
    // Prefer the frame-accurate playing clock; fall back to the segment.
    const long long tick = playing_at_ms.load();
    out.playhead_ms = tick > 0 ? tick : (long long)sess->playhead_wall_ms();
    // Whichever clock was used, the position has to lie inside the event that
    // is loaded. playhead_wall_ms() clamps to the end already and says why;
    // the frame clock bypassed that, which is how the dock came to show a
    // position past the recording's own total length. It can also still hold
    // a value from the previous event for the moment between a switch and the
    // first frame out of the new decoder.
    if (out.end_ms > 0 && out.playhead_ms > out.end_ms)
        out.playhead_ms = out.end_ms;
    if (out.started_ms > 0 && out.playhead_ms > 0 &&
        out.playhead_ms < out.started_ms)
        out.playhead_ms = out.started_ms;
    {
        // Downloaded ranges as clock times, for the timeline.
        for (const auto& r : sess->cached_ranges()) {
            const int64_t a = sess->wall_clock_ms(r.first);
            const int64_t b = sess->wall_clock_ms(r.second);
            if (a > 0 && b >= a) out.cached_spans.emplace_back(a, b);
            // The same range as segment numbers, which is what the bar draws.
            out.cached_seq_spans.emplace_back(r.first, r.second);
        }
    }
    out.live_ms     = sess->live_wall_ms();
    out.earliest_ms = sess->earliest_wall_ms();

    // Now that the playhead is known and clamped, express "behind live" as the
    // gap between two real times rather than a count of segments. Only done
    // while there IS a live edge to be behind: a finished recording has none,
    // and the dock does not show the number there anyway.
    if (!out.ended) {
        const long long edge = live_edge_wall_ms.load();
        const long long seen = live_edge_seen_ms.load();
        if (edge > 0 && seen > 0 && out.playhead_ms > 0) {
            // Two segments of extrapolation. Past that the edge has stopped
            // moving for longer than a stall explains, and standing still is a
            // better answer than inventing delay that may not exist.
            const long long cap =
                (long long)(sess->segment_duration_s() * 2000.0);
            const long long now_ms = (long long)(os_gettime_ns() / 1000000ULL);
            const long long edge_now =
                multisite::interpolate_position(edge, seen, now_ms, 0, cap);
            const double behind = (double)(edge_now - out.playhead_ms) / 1000.0;
            // Never report being ahead of live: at the edge the two clocks are
            // within a frame of each other and noise can cross over.
            out.behind_live_s = behind > 0.0 ? behind : 0.0;
        }
    }

    if (auto cur = sess->current_marker()) out.current_marker = cur->label;
    for (const auto& m : sess->markers())
        // The clock time of the cue's CONTENT, derived from the event timeline
        // rather than from whoever dropped it — the same thing the Pi player
        // reports. A satellite whose clock is out then still draws its cue at
        // the right place on every other site's timeline.
        out.markers.push_back({ m.label, m.id, m.author,
                                (long long)sess->wall_clock_ms(m.seq), m.seq });
}

void SourceCtx::event_listing(EventListing& out) const {
    {
        std::lock_guard<std::mutex> lk(events_mtx);
        out = events_cache;
    }
    // Reported outside the cached copy so the dock can show "refreshing" the
    // moment the button is pressed, not one refresh later.
    out.loading = events_refreshing.load() || events_refresh_wanted.load();
}

void SourceCtx::pin_event(const std::string& event_id) {
    auto sess = get_session(this);
    if (!sess) return;
    // Playing a different event is the same upheaval as the room switching:
    // the cache, playhead and decoder timeline all belong to the old event.
    // poll() performs that reset when it notices the target changed, and the
    // discontinuity counter makes the host rebuild its decoder.
    sess->pin_event(event_id);
    playing = false;                  // an operator presses Play on cue
    // Loading is the other way out of a stopped source: it must download to
    // fill the buffer, even though it deliberately does not go to air.
    resume_downloads();
    playing_at_ms = 0;
    // Say so before any network work starts. The dock reads these on its very
    // next tick (500 ms), so the click is acknowledged whatever the store does
    // afterwards.
    loading_event     = true;
    seek_target_ms    = 0;
    action_started_ns = os_gettime_ns();
    poll_now          = true;         // apply the pin now, not in 3 seconds
    mlog_info("source: pinned event %s — playback will not follow a new "
              "event starting", event_id.c_str());
    events_refresh_wanted = true;     // so the list re-marks which row is playing
}

void SourceCtx::unpin_event() {
    auto sess = get_session(this);
    if (!sess) return;
    sess->unpin();
    playing = false;
    resume_downloads();
    playing_at_ms = 0;
    loading_event     = true;
    seek_target_ms    = 0;
    action_started_ns = os_gettime_ns();
    poll_now          = true;
    mlog_info("source: unpinned — following whatever is live in the room");
    events_refresh_wanted = true;
}

void SourceCtx::jump_to_marker(const std::string& id) {
    auto sess = get_session(this);
    if (!sess) return;
    if (!sess->jump_to_marker(id)) {
        mlog_warn("source: could not jump to marker '%s' — it may no longer "
                  "be retained", id.c_str());
        return;
    }
    pause_started_ns = 0;
    paused = false;
    mlog_info("source: jumped to marker (segment %llu)",
              (unsigned long long)sess->playback_head());
}

void SourceCtx::add_cue(const std::string& label, std::string& error) {
    auto sess = get_session(this);
    if (!sess) { error = "no event is loaded"; return; }
    // Writes this box's own cue object (or hands it to the encoder over LAN),
    // then folds it in locally — see DecoderSession::add_cue. The on-screen
    // SEGMENT goes with it: the media clock is re-pinned on every seek, so a
    // clock reading names a place inconsistently, while the segment number is
    // the place the operator is actually looking at.
    sess->add_cue(label, error, on_screen_seq.load());
}

void SourceCtx::resume_downloads() {
    if (!stopped.exchange(false)) return;   // idempotent: nothing to undo
    std::shared_ptr<S3Transport> tx;
    std::shared_ptr<LanTransport> lan_tx;
    { std::lock_guard<std::mutex> lk(obj_mtx); tx = transport; lan_tx = lan_transport; }
    if (tx) tx->resume_pending();
    if (lan_tx) lan_tx->resume_pending();
    poll_now = true;        // refill now rather than waiting out the interval
}

void SourceCtx::play() {
    auto sess = get_session(this);
    if (!sess) { mlog_warn("source: play ignored — not configured"); return; }
    pause_started_ns = 0;
    paused = false;
    resume_downloads();
    playing = true;
    // Going to air takes as long as it takes to decode the first fragment, so
    // say so until a frame has actually landed rather than looking inert.
    awaiting_frames   = true;
    action_started_ns = os_gettime_ns();
    sess->resume();
    resumed_at_ns = os_gettime_ns();
    frames_at_resume = frames_out.load();
    resume_checked = false;
    mlog_info("source: PLAY — %.0fs behind live, %.0fs buffered",
              sess->behind_live_s(), sess->buffered_ahead_s());
}

// Stop has to close the door before it sweeps the floor. The decoder keeps
// running after Stop on purpose — that is what "ready to play again" means —
// so it goes on handing frames to deliver_video/deliver_audio the whole time
// the source is stopped. Clearing the queue alone was therefore a one-shot
// against a tap that was still open: the queue refilled within microseconds,
// the delivery loop put those frames on air, and the picture carried on
// playing with the button reading STOPPED. The giveaway in the log was a
// "playout anchored" line at the same millisecond as "STOPPED" — a frame
// delivered after Stop, re-anchoring the clock that Stop had just reset.
//
// `playing` is the door, checked in both deliver_* functions and once more in
// the delivery loop. It must be cleared BEFORE the queue, or the sweep races
// the refill.
void SourceCtx::stop_playback() {
    playing = false;
    stopped = true;
    paused = false;
    pause_started_ns = 0;

    // `flushing` stays set across the decoder teardown below, not just the
    // queue sweep. stop() joins the decoder's worker thread, and that thread
    // may be parked inside enqueue_frame waiting for queue space; flushing is
    // what releases it. Clearing the flag before the join would be a deadlock
    // on the UI thread — exactly the shape of hang 658cd5f fixed for poll().
    flushing = true;
    dq_cv.notify_all();
    {
        std::lock_guard<std::mutex> qlk(dq_mtx);
        dq.clear();
        // Same invariant as a seek: a frame the delivery loop has already
        // popped predates the stop and must not be believed about anything.
        // The media timeline restarts too: the decoder is torn down here.
        timeline_epoch++;
        media_epoch++;
    }

    // Cancel whatever is in flight. Without this a poll that has just gone out
    // sits on its curl timeout — up to 30s — so a stopped source would carry
    // on holding a connection open and land one more segment after the
    // operator took it off air. Re-armed in play().
    {
        std::shared_ptr<S3Transport> tx;
        std::shared_ptr<LanTransport> lan_tx;
        { std::lock_guard<std::mutex> lk(obj_mtx); tx = transport; lan_tx = lan_transport; }
        if (tx) tx->cancel_pending();
        if (lan_tx) lan_tx->cancel_pending();
    }

    // Drop the decoder. It is the expensive thing to leave running — threads,
    // codec contexts, and a frame buffer per stream — and Play rebuilds it
    // from the next fragment's init segment anyway. Taken out from under the
    // lock and stopped outside it, the same order the restart path uses,
    // because stop() blocks.
    std::shared_ptr<CmafDecoder> old;
    { std::lock_guard<std::mutex> lk(obj_mtx); old = decoder; decoder.reset(); }
    if (old) old->stop();
    decoder_started = false;
    // Tell the session the next segment must carry init.mp4 again.
    //
    // The session sends the init segment once per decoder and then stops
    // (`m_init_sent`), which is right while one decoder runs for the whole
    // event. Releasing the decoder makes that stale: the next one is new and
    // cannot start without it. seek() has always cleared this for exactly that
    // reason; the Stop path released a decoder without it, so Play afterwards
    // sat in "first segment arrived without an init segment" until something
    // else — a seek, usually — happened to clear the flag.
    if (auto sess = get_session(this)) sess->request_init();


    flushing = false;

    // Everything the media timeline is derived from goes with the decoder,
    // or Play would interpret the next fragment's pts against an anchor from
    // before the stop.
    first_pts_ns        = -1;
    pts_wall_offset_ms  = kOffsetUnset;
    restart_wall_ms     = 0;
    restart_wall_pending = true;
    seek_target_ms      = 0;
    awaiting_frames     = false;
    dq_cv.notify_all();

    // Take the picture off air. OBS holds the last frame handed to an async
    // video source indefinitely, so without this the programme stayed on the
    // screen after Stop and the button looked like it had done nothing. A
    // null frame is how a source says it has no picture; "hold the last
    // frame" is what Hold is for, and it is a separate control.
    if (source) obs_source_output_video(source, nullptr);

    mlog_info("source: STOPPED — downloads cancelled, decoder released, "
              "cache kept (%llu segments)",
              (unsigned long long)(get_session(this)
                                       ? get_session(this)->cache().count() : 0));
}

void SourceCtx::release_decoder_for_restart() {
    // Bump first, under the queue lock, so any frame the delivery loop has
    // already popped is stamped with the old timeline and will be discarded.
    // A decoder restart: both clocks start again.
    { std::lock_guard<std::mutex> qlk(dq_mtx); timeline_epoch++; media_epoch++; }
    std::shared_ptr<CmafDecoder> old;
    { std::lock_guard<std::mutex> lk(obj_mtx); old = decoder; decoder.reset(); }
    if (old) old->stop();            // blocks; outside the lock, flushing set
    decoder_started = false;
    // Tell the session the next segment must carry init.mp4 again.
    //
    // The session sends the init segment once per decoder and then stops
    // (`m_init_sent`), which is right while one decoder runs for the whole
    // event. Releasing the decoder makes that stale: the next one is new and
    // cannot start without it. seek() has always cleared this for exactly that
    // reason; the Stop path released a decoder without it, so Play afterwards
    // sat in "first segment arrived without an init segment" until something
    // else — a seek, usually — happened to clear the flag.
    if (auto sess = get_session(this)) sess->request_init();

    first_pts_ns     = -1;           // re-anchor the playout clock
    // The media timeline restarts with the new decoder, so the mapping from
    // pts to wall clock has to be learned again.
    pts_wall_offset_ms   = kOffsetUnset;
    restart_wall_ms      = 0;
    restart_wall_pending = true;
    // The lead window measures the current clock, and the clock is about to be
    // re-anchored.
    lead_video_sum_ns = 0; lead_video_count = 0; lead_video_min_ns = INT64_MAX;
    lead_audio_sum_ns = 0; lead_audio_count = 0; lead_audio_min_ns = INT64_MAX;
}

void SourceCtx::after_jump(long long to_wall_ms) {
    // Treat as a discontinuity: drop what is queued and re-anchor. `flushing`
    // releases the decoder if it is waiting for queue space, so the restart
    // that follows can never block.
    // `flushing` is held across the decoder teardown as well as the queue
    // sweep, because stop() joins a worker that may be parked waiting for
    // queue space and flushing is what releases it.
    flushing = true;
    dq_cv.notify_all();
    {
        std::lock_guard<std::mutex> qlk(dq_mtx);
        dq.clear();
    }

    // Release the decoder HERE, not three seconds later when the poll loop
    // notices the discontinuity. Clearing the queue alone left the old decoder
    // running and still holding decoded frames from the position just left, so
    // those frames were delivered immediately after the seek — the log showed
    // "playout anchored on first video frame" in the same millisecond as the
    // jump, at the OLD pts. The operator saw the picture carry on playing from
    // where they had just left, for as long as it took the poll loop to catch
    // up, and only then cut to where they had asked for.
    release_decoder_for_restart();

    flushing = false;
    pause_started_ns = 0;
    paused = false;
    dq_cv.notify_all();

    // Move the displayed time to where we are GOING, immediately. It used to
    // keep reporting the old position until a frame had been fetched and
    // decoded at the new one, so a jog looked like nothing had happened and
    // then jumped. The dock marks this as provisional until frames arrive, so
    // the operator sees the intent honoured at once without being told the
    // picture has already moved.
    if (to_wall_ms > 0) playing_at_ms = to_wall_ms;
    action_started_ns = os_gettime_ns();
    poll_now          = true;    // fetch what the new position needs now

    // Armed whatever state the decoder is in. A move made while stopped or
    // held still has to fetch and decode at the new position before anything
    // could play from there, and an operator lining up a cue in a loaded
    // recording is exactly who needs telling that the click landed. No frames
    // will arrive to answer it in that state, so the poll loop clears it on
    // the content being ready instead.
    seek_target_ms  = to_wall_ms > 0 ? to_wall_ms : 0;
    awaiting_frames = true;
}

void SourceCtx::seek_to_time(long long wall_ms) {
    auto sess = get_session(this);
    if (!sess) return;
    const int64_t got = sess->seek_to_wall_ms((int64_t)wall_ms);
    if (got == 0) {
        // The session reports which bound was hit — retention floor, before
        // the start, or past the end. Reporting one of those as the others is
        // how a seek past the end of a recording looked like data loss.
        {
            const std::string why = sess->last_error();
            mlog_warn("source: %s", why.empty()
                                        ? "that moment cannot be played"
                                        : why.c_str());
        }
        return;
    }
    after_jump((long long)got);

    mlog_info("source: went to %lld (%.0fs behind live)",
              (long long)got, sess->behind_live_s());
}

void SourceCtx::jog(double seconds) {
    long long from = playing_at_ms.load();
    if (from <= 0) {
        // Nothing has been delivered yet — a recording loaded but not yet
        // played, which is exactly when an operator wants to move to the point
        // they intend to start from. Jog from where the playhead SITS rather
        // than refusing until something has gone to air.
        auto sess = get_session(this);
        if (sess) from = (long long)sess->playhead_wall_ms();
    }
    if (from <= 0) {
        mlog_warn("source: cannot jog until the recording has loaded");
        return;
    }
    seek_to_time(from + (long long)(seconds * 1000.0));
}

void SourceCtx::set_delay_from_live(double seconds) {
    auto sess = get_session(this);
    if (!sess) return;
    const int64_t live = sess->live_wall_ms();
    if (live <= 0) {
        mlog_warn("source: live time not known yet");
        return;
    }
    seek_to_time((long long)live - (long long)(seconds * 1000.0));
    mlog_info("source: holding %.0f minutes behind live", seconds / 60.0);
}

void SourceCtx::reconfigure() {
    // obs_source_update with null settings re-applies the source's current
    // settings on the next tick, which re-runs src_update — picking up the
    // machine-wide storage config. Deferring avoids tearing down worker
    // threads from whichever thread happened to click the button.
    if (source) obs_source_update(source, nullptr);
}

void SourceCtx::seek_media(long long media_ms) {
    auto sess = get_session(this);
    if (!sess) return;
    if (sess->seek_to_media_ms(media_ms) == 0) {
        const std::string why = sess->last_error();
        mlog_warn("source: %s", why.empty()
                                    ? "that moment cannot be played"
                                    : why.c_str());
        return;
    }
    // The same housekeeping a segment seek or a jog gets: flush what is queued,
    // release the decoder, re-anchor, and move the displayed time to where the
    // click is heading rather than where the picture still is.
    after_jump((long long)sess->playhead_wall_ms());
    mlog_info("source: went to %.3fs on the media timeline", media_ms / 1000.0);
}

void SourceCtx::seek(unsigned long long seq) {
    auto sess = get_session(this);
    if (!sess) return;
    if (!sess->seek((uint64_t)seq)) {
        mlog_warn("source: cannot seek to segment %llu (outside the retained "
                  "window)", seq);
        return;
    }
    // A click on the timeline is a jump like any other, and has to be
    // acknowledged like one.
    //
    // This used to move only the session's head and leave everything else to
    // the poll loop, which notices a discontinuity on its NEXT pass — up to a
    // whole poll interval (3 s by default) later. So a click appeared to do
    // nothing at all: no provisional time in the dock, no movement, and then
    // the picture jumped and played some seconds afterwards. Worse, the old
    // decoder kept delivering frames from the position just left for that same
    // window, which is the stale-frame fault the jog path was already fixed
    // for. after_jump does exactly what a jog does — flush what is queued,
    // release the decoder, re-anchor the clock, and set the position the dock
    // shows as provisional — so the click is confirmed the moment it lands
    // instead of being indistinguishable from a dropped one.
    after_jump((long long)sess->playhead_wall_ms());
    mlog_info("source: seeked to segment %llu (%.0fs behind live)", seq,
              sess->behind_live_s());
}

// Property buttons delegate to the same methods the hotkeys use.
static bool on_pause(obs_properties_t*, obs_property_t*, void* data) {
    static_cast<SourceCtx*>(data)->pause();  return false;
}
static bool on_resume(obs_properties_t*, obs_property_t*, void* data) {
    static_cast<SourceCtx*>(data)->resume(); return false;
}
static bool on_jump_live(obs_properties_t*, obs_property_t*, void* data) {
    static_cast<SourceCtx*>(data)->jump_to_live(); return false;
}
static bool on_status(obs_properties_t*, obs_property_t*, void* data) {
    static_cast<SourceCtx*>(data)->log_status(); return false;
}

// Jump to a marker published by the main site. The list is rebuilt whenever the
// properties dialog is opened, so it reflects whatever cues have been dropped.
static bool on_jump_marker(obs_properties_t*, obs_property_t* prop, void* data) {
    auto* ctx = static_cast<SourceCtx*>(data);
    (void)prop;
    // Read the selection under the lock, then release it: jump_to_marker takes
    // the same lock itself.
    const std::string id = ctx->pending_marker_id;
    if (id.empty()) {
        mlog_info("source: pick a marker first");
        return false;
    }
    ctx->jump_to_marker(id);
    return false;
}

static bool on_marker_selected(void* data, obs_properties_t*,
                               obs_property_t*, obs_data_t* settings) {
    auto* ctx = static_cast<SourceCtx*>(data);
    ctx->pending_marker_id = obs_data_get_string(settings, "marker_id");
    return false;
}


// Fill an audio-track list with the labels the main site published, so an
// operator picks "Click" rather than "Track 3". Falls back to numbers before
// an event has been read — the names only exist once a manifest has arrived.
static void fill_audio_tracks(obs_property_t* list,
                              const std::vector<AudioTrack>& layout) {
    if (layout.empty()) {
        for (int i = 0; i < MAX_AUDIO_MIXES; ++i) {
            const std::string n = "Track " + std::to_string(i + 1);
            obs_property_list_add_int(list, n.c_str(), i);
        }
        return;
    }
    for (size_t i = 0; i < layout.size(); ++i) {
        std::string n = std::to_string(i + 1) + ". " +
            (layout[i].label.empty() ? ("Track " + std::to_string(i + 1))
                                     : layout[i].label);
        if (layout[i].channels > 2)
            n += "  (" + std::to_string(layout[i].channels) + " ch, packed)";
        else if (layout[i].channels == 1)
            n += "  (mono)";
        obs_property_list_add_int(list, n.c_str(), (long long)i);
    }
}

static obs_properties_t* src_props(void* data) {
    obs_properties_t* p = obs_properties_create();

    // Storage, feed name and receive tuning deliberately do NOT appear here.
    // They are machine-wide and edited in the decoder dock; having a second
    // editable copy per scene meant a dock edit could be silently overridden by
    // a value saved in the scene file, with nothing on screen to explain it.
    obs_properties_add_text(p, "storage_note",
                            obs_module_text("StorageInDock"), OBS_TEXT_INFO);

    // Which track this source carries. Everything the main site sends arrives
    // in the same fragment, so choosing here costs no extra bandwidth — add a
    // Multisite Audio Track source for each further track a campus needs.
    {
        obs_property_t* at = obs_properties_add_list(
            p, S_ATRACK, obs_module_text("AudioTrack"),
            OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
        std::vector<AudioTrack> layout;
        auto* ctx = static_cast<SourceCtx*>(data);
        if (ctx) {
            if (auto sess = get_session(ctx)) layout = sess->audio_layout();
        }
        fill_audio_tracks(at, layout);
    }
    obs_properties_add_button(p, "btn_pause",  obs_module_text("Pause"),      on_pause);
    obs_properties_add_button(p, "btn_resume", obs_module_text("Resume"),     on_resume);
    obs_properties_add_button(p, "btn_live",   obs_module_text("JumpToLive"), on_jump_live);
    obs_properties_add_button(p, "btn_status", obs_module_text("LogStatus"),  on_status);

    // Markers published by the main site. Populated from the manifest the
    // decoder is already polling, so it lists the operator's own cue names.
    obs_property_t* mk = obs_properties_add_list(
        p, "marker_id", obs_module_text("JumpToMarker"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    if (data) {
        auto* ctx = static_cast<SourceCtx*>(data);
        if (auto sess = get_session(ctx)) {
            for (const auto& m : sess->markers()) {
                // Clock time, not a sequence number: an operator recognises
                // "10:42" and never "segment 147".
                char when[32] = "";
                if (m.at_ms > 0) {
                    const std::time_t t = (std::time_t)(m.at_ms / 1000);
                    std::tm lt{};
#if defined(_WIN32)
                    localtime_s(&lt, &t);
#else
                    localtime_r(&t, &lt);
#endif
                    std::strftime(when, sizeof(when), "%H:%M", &lt);
                }
                std::string label = when[0] ? (std::string(when) + "  " + m.label)
                                            : m.label;
                obs_property_list_add_string(mk, label.c_str(), m.id.c_str());
            }
        }
    }
    obs_property_set_modified_callback2(mk, on_marker_selected, data);
    obs_properties_add_button(p, "btn_jump_marker",
                              obs_module_text("JumpToMarkerButton"),
                              on_jump_marker);
    return p;
}


// ── Companion audio source ───────────────────────────────────────────────────
// One OBS source can emit one audio stream, so a campus that needs the main mix
// AND a click AND an ISO needs a source per track. This is that source: audio
// only, no video, no decoder of its own. It attaches to whatever Multisite
// Source is already following the same room and receives its chosen track from
// that decoder — so the segment is downloaded once, decoded once, and every
// track shares one playout clock.
//
// It produces nothing until a Multisite Source for the same room is present and
// playing. That is the normal setup (a campus always has the picture), and it
// is stated in the properties rather than left to be discovered.
struct AudioCtx {
    AudioSub sub;
    bool     registered = false;
};

// ── Companion tile source ────────────────────────────────────────────────────
// One region of a composited feed, as its own video source. No decoder of its
// own and no download of its own: it attaches to whichever Multisite Source is
// following the same room and receives a cropped view of the frames that source
// has already decoded. Exactly the shape the audio companion uses, for the same
// reason — the segment is fetched once, decoded once, and every source is a
// view of that one decode.
struct TileCtx {
    TileSub  sub;
    bool     registered = false;
    // Which monitor this tile is projected onto, or 0 for none. Kept so a
    // change can be detected; opening a projector every update call would
    // reopen the window on every keystroke in the room field.
    int      projector = 0;
};

// The room field on a companion source, but only when it has a job to do.
//
// Blank already means "whatever room the Multisite Source is following", which
// is the answer in every room that runs one decoder — so the field spends most
// of its life empty, inviting exactly the question "what do I put here?" and
// occasionally getting an answer that breaks the source. The same rule the
// encoder dock applies to the audio label fields applies here: show it only
// when it applies.
//
// It applies when there is more than one room being followed on this machine
// and the companion therefore has to say which. It also applies when this
// source already carries a room that is NOT the one being followed — hiding
// that would leave a source pointing somewhere wrong with no way to correct it.
static void add_companion_room_field(obs_properties_t* p,
                                     const std::string& current) {
    std::vector<std::string> rooms;
    {
        std::lock_guard<std::mutex> lk(g_owners_mtx);
        for (auto* o : g_owners) {
            const std::string& r = o->room_id_for_display;
            if (r.empty()) continue;
            if (std::find(rooms.begin(), rooms.end(), r) == rooms.end())
                rooms.push_back(r);
        }
    }
    const bool several   = rooms.size() > 1;
    const bool overriden = !current.empty() &&
                           (rooms.size() != 1 || current != rooms.front());
    if (several || overriden) {
        obs_properties_add_text(p, S_ROOM, obs_module_text("RoomID"),
                                OBS_TEXT_DEFAULT);
        return;
    }
    // One room, and this source follows it. Say which, rather than ask.
    const std::string room = rooms.empty() ? decoder_settings().room_id
                                           : rooms.front();
    const std::string note =
        std::string(obs_module_text("Companion.Following")) + " " +
        (room.empty() ? std::string(obs_module_text("Companion.NoRoom")) : room);
    obs_properties_add_text(p, "room_note", note.c_str(), OBS_TEXT_INFO);
}

static const char* aud_name(void*) {
    return obs_module_text("Multisite.AudioSource");
}

static void aud_update(void* data, obs_data_t* s) {
    auto* c = static_cast<AudioCtx*>(data);
    DecoderSettings shared = decoder_settings();
    const char* room = obs_data_get_string(s, S_ROOM);
    const std::string want_room = (room && *room) ? std::string(room)
                                                  : shared.room_id;
    const int want_track = (int)obs_data_get_int(s, S_ATRACK);

    // OBS calls update on every keystroke in a text field, so typing a room
    // name arrives as one call per character. Re-registering each time is
    // harmless but logging each time is not: a soak test showed fifteen lines
    // for one room name, which buries the entries that matter.
    if (c->registered && c->sub.room_id == want_room &&
        c->sub.track == want_track)
        return;

    unregister_audio_sub(&c->sub);
    c->sub.room_id = want_room;
    c->sub.track   = want_track;
    register_audio_sub(&c->sub);
    c->registered = true;
    mlog_info("audio source: room '%s', track %d",
              c->sub.room_id.c_str(), c->sub.track + 1);
}

static void* aud_create(obs_data_t* settings, obs_source_t* source) {
    auto* c = new AudioCtx();
    c->sub.source = source;
    aud_update(c, settings);
    return c;
}

static void aud_destroy(void* data) {
    auto* c = static_cast<AudioCtx*>(data);
    unregister_audio_sub(&c->sub);
    c->registered = false;
    delete c;
}

static void aud_defaults(obs_data_t* s) {
    obs_data_set_default_int(s, S_ATRACK, 1);   // track 2: not the main mix
}

static obs_properties_t* aud_props(void* data) {
    obs_properties_t* p = obs_properties_create();
    obs_properties_add_text(p, "audio_note",
                            obs_module_text("AudioSourceNote"), OBS_TEXT_INFO);
    {
        auto* c = static_cast<AudioCtx*>(data);
        add_companion_room_field(p, c ? c->sub.room_id : std::string());
    }

    // Track names come from whichever Multisite Source is following this room.
    std::vector<AudioTrack> layout;
    {
        auto* c = static_cast<AudioCtx*>(data);
        const std::string want = c ? c->sub.room_id : std::string();
        std::lock_guard<std::mutex> lk(g_owners_mtx);
        for (auto* o : g_owners) {
            if (!want.empty() && o->room_id_for_display != want) continue;
            if (auto sess = get_session(o)) {
                layout = sess->audio_layout();
                if (!layout.empty()) break;
            }
        }
    }
    obs_property_t* at = obs_properties_add_list(
        p, S_ATRACK, obs_module_text("AudioTrack"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    fill_audio_tracks(at, layout);
    return p;
}

// ── Companion tile source callbacks ──────────────────────────────────────────
static const char* tile_name(void*) {
    return obs_module_text("Multisite.TileSource");
}

static uint32_t tile_width(void* data) {
    auto* c = static_cast<TileCtx*>(data);
    return c ? c->sub.w.load() : 0;
}
static uint32_t tile_height(void* data) {
    auto* c = static_cast<TileCtx*>(data);
    return c ? c->sub.h.load() : 0;
}

static void tile_update(void* data, obs_data_t* s) {
    auto* c = static_cast<TileCtx*>(data);
    DecoderSettings shared = decoder_settings();
    const char* room = obs_data_get_string(s, S_ROOM);
    const std::string want_room = (room && *room) ? std::string(room)
                                                  : shared.room_id;
    const int want_tile = (int)obs_data_get_int(s, S_TILE);
    const int want_out  = (int)obs_data_get_int(s, S_TILEOUT);

    // Same reason as the audio companion: OBS calls update on every keystroke
    // in a text field, so re-registering is harmless but logging is not.
    const bool same = c->registered && c->sub.room_id == want_room &&
                      c->sub.tile == want_tile;
    if (!same) {
        unregister_tile_sub(&c->sub);
        c->sub.room_id = want_room;
        c->sub.tile    = want_tile;
        register_tile_sub(&c->sub);
        c->registered = true;
        mlog_info("tile source: room '%s', picture %d",
                  c->sub.room_id.c_str(), c->sub.tile + 1);
    }

    // Projector assignment. Only acted on when it CHANGES, because opening a
    // projector is a visible event on somebody's screen and update() runs far
    // more often than the setting actually moves.
    if (want_out != c->projector) {
        c->projector = want_out;
        if (want_out > 0) {
#ifdef MULTISITE_HAVE_FRONTEND_API
            // OBS counts monitors from zero; the setting counts from one so
            // that zero can mean "no projector" without an off-by-one in the
            // list the operator reads.
            obs_frontend_open_projector("Source", want_out - 1, nullptr,
                                        obs_source_get_name(c->sub.source));
            mlog_info("tile source: picture %d projected to output %d",
                      c->sub.tile + 1, want_out);
#else
            // A build without obs-frontend-api (the headless and CI builds)
            // keeps the setting — it is saved in the scene and a full build
            // will honour it — but cannot open a window from here.
            mlog_warn("tile source: this build has no frontend API, so picture "
                      "%d cannot be projected from here; assign it in OBS",
                      c->sub.tile + 1);
#endif
        }
        // Turning it off does not close the window. OBS owns it once opened and
        // closing somebody's projector from under them because a dropdown moved
        // is worse than leaving it: the operator can close it themselves, and
        // the setting still governs the next time it is switched on.
    }
}

static void* tile_create(obs_data_t* settings, obs_source_t* source) {
    auto* c = new TileCtx();
    c->sub.source = source;
    tile_update(c, settings);
    return c;
}

static void tile_destroy(void* data) {
    auto* c = static_cast<TileCtx*>(data);
    unregister_tile_sub(&c->sub);
    c->registered = false;
    delete c;
}

static void tile_defaults(obs_data_t* s) {
    obs_data_set_default_int(s, S_TILE, 0);      // top-left
    obs_data_set_default_int(s, S_TILEOUT, 0);   // no projector
}

static obs_properties_t* tile_props(void* data) {
    obs_properties_t* p = obs_properties_create();
    obs_properties_add_text(p, "tile_note",
                            obs_module_text("TileSourceNote"), OBS_TEXT_INFO);
    {
        auto* c = static_cast<TileCtx*>(data);
        add_companion_room_field(p, c ? c->sub.room_id : std::string());
    }

    // The list is built from the layout the feed actually declares, so an
    // operator picks "top-left" from four rather than typing an index and
    // hoping. A room sending one picture offers one entry, which is its own
    // answer to "why is there nothing to choose".
    TileLayout lay;
    {
        auto* c = static_cast<TileCtx*>(data);
        const std::string want = c ? c->sub.room_id : std::string();
        std::lock_guard<std::mutex> lk(g_owners_mtx);
        for (auto* o : g_owners) {
            if (!want.empty() && o->room_id_for_display != want) continue;
            if (auto sess = get_session(o)) { lay = sess->video_layout(); break; }
        }
    }
    obs_property_t* tl = obs_properties_add_list(
        p, S_TILE, obs_module_text("TilePicture"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    for (int i = 0; i < lay.count(); ++i) {
        std::string label;
        if (!lay.is_split())      label = obs_module_text("Tile.Whole");
        else if (lay.rows == 1)   label = (i == 0) ? obs_module_text("Tile.Left")
                                                   : obs_module_text("Tile.Right");
        else if (lay.cols == 1)   label = (i == 0) ? obs_module_text("Tile.Top")
                                                   : obs_module_text("Tile.Bottom");
        else {
            static const char* k[] = { "Tile.TopLeft", "Tile.TopRight",
                                       "Tile.BottomLeft", "Tile.BottomRight" };
            label = (i < 4) ? obs_module_text(k[i])
                            : std::to_string(i + 1);
        }
        obs_property_list_add_int(tl, label.c_str(), i);
    }

    // Fullscreen projector assignment. The plugin drives OBS's own projector
    // rather than inventing an output of its own, which is what makes a tile
    // work on a DeckLink or a second monitor without this code knowing what
    // either of those is.
    obs_property_t* op = obs_properties_add_list(
        p, S_TILEOUT, obs_module_text("TileOutput"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(op, obs_module_text("TileOutput.None"), 0);
    for (int i = 1; i <= 4; ++i)
        obs_property_list_add_int(
            op, (std::string(obs_module_text("TileOutput.Monitor")) + " " +
                 std::to_string(i)).c_str(), i);
    obs_property_set_long_description(op, obs_module_text("TileOutput.Help"));
    return p;
}

void register_source() {
    struct obs_source_info info = {};
    info.id           = "multisite_source";
    info.type         = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO |
                        OBS_SOURCE_DO_NOT_DUPLICATE;
    info.get_name     = src_name;
    info.create       = src_create;
    info.destroy      = src_destroy;
    info.update       = src_update;
    info.get_defaults = src_defaults;
    info.get_properties = src_props;
    info.get_width    = src_width;
    info.get_height   = src_height;
    info.icon_type    = OBS_ICON_TYPE_MEDIA;
    obs_register_source(&info);

    // The companion audio-only source. Audio flag only: OBS must not give it a
    // video canvas, and it must never be duplicated into a second decoder.
    struct obs_source_info aud = {};
    aud.id             = "multisite_audio_source";
    aud.type           = OBS_SOURCE_TYPE_INPUT;
    aud.output_flags   = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
    aud.get_name       = aud_name;
    aud.create         = aud_create;
    aud.destroy        = aud_destroy;
    aud.update         = aud_update;
    aud.get_defaults   = aud_defaults;
    aud.get_properties = aud_props;
    aud.icon_type      = OBS_ICON_TYPE_AUDIO_INPUT;
    obs_register_source(&aud);

    // The companion tile source. Video flag only — it carries no audio of its
    // own, because a tile is a region of a picture and the programme audio
    // belongs to the feed, not to any one quarter of it. Never duplicated, for
    // the same reason as the others: a duplicate would be a second registration
    // for the same region.
    struct obs_source_info tile = {};
    tile.id             = "multisite_tile_source";
    tile.type           = OBS_SOURCE_TYPE_INPUT;
    tile.output_flags   = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE;
    tile.get_name       = tile_name;
    tile.create         = tile_create;
    tile.destroy        = tile_destroy;
    tile.update         = tile_update;
    tile.get_defaults   = tile_defaults;
    tile.get_properties = tile_props;
    tile.get_width      = tile_width;
    tile.get_height     = tile_height;
    tile.icon_type      = OBS_ICON_TYPE_MEDIA;
    obs_register_source(&tile);

    mlog_info("registered sources: multisite_source, multisite_audio_source, "
              "multisite_tile_source");
}

} // namespace multisite_obs
