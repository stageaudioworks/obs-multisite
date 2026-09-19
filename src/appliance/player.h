// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// player.h — the appliance's engine: everything between the bucket and the
// HDMI socket.
//
// This is the headless twin of the OBS source. It owns exactly the same
// receive core — S3Transport, DecoderSession, EventCatalog, CmafDecoder — and
// differs only in where decoded frames end up: an HDMI output the box drives
// itself, rather than frames handed to OBS.
//
// The threading model is the one the OBS source arrived at, because the
// reasons for it were learned the hard way:
//
//   poll     — refreshes live.json / manifest.json and drives download-ahead
//   feed     — hands cached fragments to the decoder in order
//   decode   — inside CmafDecoder, emits frames
//   deliver  — releases frames when they are due, in presentation order
//
// Pacing happens on the deliver thread and nowhere else. Video and audio share
// the decode thread, so sleeping inside a decoder callback would delay every
// frame decoded after it.
//
// Pause is enforced at DELIVERY, not at feeding: by the time a fragment is fed
// its whole six seconds are already decoded, so gating the feed would let the
// picture run on for a segment after the operator pressed Hold.
//
#include "config.h"
#include "audio_levels.h"   // what the card was given, for the meters
#include "audio_output.h"
#include "video_output.h"
#include "splash.h"

#include "../core/decoder_session.h"
#include "../core/event_catalog.h"
#include "../core/cmaf_decoder.h"
#include "../core/s3_transport.h"
#include "../core/lan_transport.h"
#include "../core/fallback_transport.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace multisite_player {

// One row of the event list, in the plain terms the UI needs.
struct EventEntry {
    std::string event_id;
    // Operator-facing title from the encoder's event name; empty when the
    // event predates naming or the encoder left it blank.
    std::string name;
    long long   started_ms = 0;
    double      duration_s = 0;
    // 0 unknown, 1 live, 2 recording, 3 interrupted — matching
    // multisite::EventState, so the browser never sees a core enum.
    int         state = 0;
};

struct EventListing {
    std::vector<EventEntry> events;
    bool        loading = false;
    bool        listed_once = false;
    bool        fallback_scan = false;
    int         skipped = 0;
    std::string error;
    // No cloud storage is configured, so there is no catalogue and this list can
    // never fill — an event published over the LAN only lives on the machine
    // that recorded it. The page says so rather than showing an empty list that
    // reads as "no recordings exist".
    bool        no_catalog = false;
};

// Everything the web UI draws, captured in one consistent read. The browser
// polls this; nothing else it asks for can disagree with it, because it all
// comes from the same snapshot.
struct Status {
    // ── What is on air ───────────────────────────────────────────────────────
    std::string room_id;
    int         room_state = 0;         // matches multisite::RoomState
    std::string event_id;
    std::string pinned_event_id;
    bool        live_elsewhere = false;
    std::string live_event_id;

    bool        playing = false;
    bool        paused = false;
    bool        buffering = false;      // playing, but nothing decoded yet
    bool        loading = false;        // switching events
    bool        locked = false;

    // ── Where in the programme ───────────────────────────────────────────────
    // Clock times throughout. The UI never mentions a sequence number.
    long long   playhead_ms = 0, live_ms = 0, earliest_ms = 0, started_ms = 0;
    long long   end_ms = 0, total_ms = 0;
    long long   seek_target_ms = 0;     // where playback is heading, if moving
    double      behind_live_s = 0.0;
    double      delay_from_live_s = 0.0;
    bool        ended = false;          // a recording, not a live feed
    bool        at_end = false;
    bool        was_live = false;       // seen live since it was loaded
    bool        interrupted = false;    // the encoder died rather than finished

    // THE answer to "is what we are playing a recording rather than a live
    // feed", straight from DecoderSession::plays_as_recording(). Do not
    // re-derive it from `ended` and `interrupted` above: that expression is
    // missing the pinned case, which is exactly how the OBS dock came to span
    // its timeline wrongly for a pinned interrupted event. Anything that needs
    // this question answered — the page included — reads this field.
    bool        plays_as_recording = false;

    // ── Reliability readout ──────────────────────────────────────────────────
    double      buffered_ahead_s = 0.0;
    size_t      cached_segments = 0;
    std::vector<std::pair<long long, long long>> cached_spans;
    unsigned long long downloaded = 0, download_failures = 0;
    unsigned long long checksum_failures = 0, gaps_waited = 0;
    unsigned long long frames_out = 0, frames_dropped = 0;
    // Connection health, 0 healthy / 1 degraded / 2 offline, and whether a
    // request has been observed yet. Mirrors the OBS dock's meaning.
    int         link_health = 0;
    bool        link_known = false;
    std::string last_error;

    // ── The feed's own description of itself ─────────────────────────────────
    int         video_width = 0, video_height = 0;
    // How the encoder composited the picture: "1x1", "2x1", "2x2". "1x1" is
    // one picture, and is every event written before tiling existed.
    std::string video_layout = "1x1";
    int         audio_channels = 0;
    std::vector<std::string> channel_labels;

    // at_media_ms is where in the programme the cue sits, and is what places
    // it. at_ms is a time of day, for display while following a live event
    // only. -1 means the cue predates at_media_ms and could not be converted.
    struct MarkerEntry { std::string label; std::string id; std::string author;
                         long long at_ms = 0; long long at_media_ms = -1; };
    std::vector<MarkerEntry> markers;
    std::string current_marker;

    // ── The box itself ───────────────────────────────────────────────────────
    bool        configured = false;     // storage credentials present
    std::string output_description;     // e.g. "HDMI-A-1 1920x1080@50"
    std::string audio_description;
    bool        video_output_ok = false;
    bool        audio_output_ok = false;
    // "closed" / "open" / "failed", and the reason for the last of those. The
    // state is a word so the page can say which of the three it is; the error
    // is the card's own message, kept so the page can show it without the
    // operator being asked to go and find the journal.
    std::string audio_state;
    std::string audio_error;
};

// What the sound card is being given, with a word for why it reads as it does.
// Taken where the samples leave the player rather than where they arrive, so
// muting and a card that will not open both read as silence — and the reason
// says which of them it is.
struct AudioMeterView {
    std::vector<float> peak;        // linear, one per card channel
    std::vector<float> db;          // dBFS, same order
    bool        live = false;       // a frame reached the card recently
    MeterReason reason = MeterReason::CardClosed;
    std::string reason_text;        // the reason, as a sentence
};

// Where the sound output stands. Reported as a word rather than derived by the
// interface from three booleans, because the three states mean genuinely
// different things to whoever is looking at the page:
//
//   Closed — nothing has asked for it yet, or the settings say not to.
//   Open   — the card is open and the player writes to it.
//   Failed — the card would not open. Distinct from both of the others: for
//            Closed the answer is "switch it on", for Failed it is "go and look
//            at the card", and a silent box is exactly when somebody needs to
//            tell those two apart without reading the journal.
enum class AudioState { Closed, Open, Failed };
const char* to_string(AudioState s);

class Player {
public:
    // `config_path` is where edited settings are written back to. The player
    // itself never needed it — settings arrive from the web interface already
    // saved — but the AES67 reconciler does: pointing the sound back at the
    // card the daemon reads is a settings change like any other, and one that
    // has to survive the power cut that caused it.
    Player(Config cfg, VideoOutput& video, AudioOutput& audio,
           std::string config_path = std::string());
    ~Player();

    // Bring up the transport and the worker threads. Safe to call on a box
    // with no credentials yet: it simply reports itself unconfigured and waits
    // for somebody to fill them in over the web UI.
    void start();
    void stop();

    // Apply edited settings. Anything that changes what is being received
    // rebuilds the session; the picture goes away and comes back. Returns the
    // config actually in force.
    void reconfigure(const Config& cfg);
    Config config() const;

    // ── What the sound output is doing ───────────────────────────────────────
    // Whether the card is open, and the card's own words if it would not open.
    // Both are here rather than only in Status so that the audio-levels poll can
    // say *why* the meters are flat without building a whole Status for it.
    AudioState audio_state() const {
        return (AudioState)m_audio_open_state.load();
    }
    std::string audio_error() const {
        std::lock_guard<std::mutex> lk(m_audio_err_mtx);
        return m_audio_last_error;
    }
    // The width the card is open at, 0 when it is not open. What the meters use
    // to decide how many bars there are.
    int audio_opened_channels() const { return m_audio_opened_channels.load(); }

    // The lock the web interface's AES67 switch and the reconciler share. Both
    // write the same source on the same daemon, and a repair that arrives a
    // moment after somebody switched the stream off must not put it back on.
    std::mutex& aes67_mutex() { return m_aes67_mtx; }

    // The meters. Read and clear, so the interface's polling interval is the
    // meter's window: whatever happened since somebody last looked.
    AudioMeterView audio_meter() const;

    // ── Is the storage reachable, from where, and how fast? ─────────────────
    // The three questions asked when a campus stutters, and the ones nothing
    // here could answer: an operator could see the picture was wrong but not
    // whether the bucket was reachable, which Cloudflare edge was serving it,
    // or what the link was managing.
    struct StorageHealth {
        bool        configured = false;   // ANY way to reach a room — cloud, LAN, or both
        std::string endpoint;
        std::string bucket;
        std::string room;
        // Everything below is about the CLOUD leg specifically, and stays at
        // its default (false/empty/0) on a LAN-only box with no cloud
        // credentials at all — there is no bucket to report on.
        bool        reachable = false;    // the endpoint answered at all
        bool        readable = false;     // …and our key could read the feed
        long        http_status = 0;
        std::string error;                // empty when readable
        int64_t     round_trip_ms = 0;
        std::string colo;                 // Cloudflare edge, e.g. "JNB"
        std::string server;               // the Server header
        double      bytes_per_s = 0.0;    // observed, from real segment traffic
        uint64_t    rate_samples = 0;     // 0 means show no figure at all
        // LAN / direct delivery (PROJECT-SCOPE.md §8.7). lan_active is which
        // path the most recent fetch actually took — see FallbackTransport —
        // not a sticky mode, so it can change from one poll to the next.
        bool        lan_configured = false;
        bool        lan_active = false;
    };
    // `probe` issues one read-only request for the room's live pointer; without
    // it only the passively-observed figures are filled in, which is what a
    // status poll several times a minute should use.
    StorageHealth storage_health(bool probe);

    // How far this box's clock is from the store's, in milliseconds, measured
    // from the HTTP Date header on ordinary traffic. 0 means "nothing observed
    // yet", not "in step".
    long long clock_skew_ms() const;

    // ── Operator controls (the same set the Qt dock offers) ──────────────────
    void play();
    void stop_playback();
    void pause();
    void resume();
    void toggle_pause();
    void jump_to_live();
    // Go to a position in the programme: milliseconds from the event's start.
    // Was seek_to_time(wall_ms) and took a time of day, which had to be
    // converted back to a position by machinery that drifted (BUGS #2b).
    void seek_to_media(long long media_ms);
    void jog(double seconds);
    void set_delay_from_live(double seconds);
    void jump_to_marker(const std::string& id);
    // Drop a cue with an operator-typed name, under this box's site name.
    // Fails with a reason when the box is locked, has no site name, or has
    // nowhere to write the cue.
    bool add_cue(const std::string& label, std::string& error);
    void pin_event(const std::string& event_id);
    void unpin_event();
    void set_locked(bool locked);
    bool locked() const { return m_locked.load(); }

    // ── Event list ───────────────────────────────────────────────────────────
    // Asks for a refresh on the worker. Listing plus a manifest per event is
    // far too much to do while a browser request waits.
    void refresh_events();
    void event_listing(EventListing& out) const;

    void status(Status& out) const;

    // The most recent decoded picture, for the preview. Deliberately separate
    // from the output path: the preview may be one frame a second, may lag,
    // and may be looked at while the output is held — lining up a cue is
    // exactly when those must not be the same thing.
    //
    // Returns false when nothing has been decoded yet. `version` lets a caller
    // wait for a frame it has not already sent.
    // The outputs in use, so the interface can list what this box can do
    // without a second path to the hardware.
    const VideoOutput& video() const { return m_video; }
    const AudioOutput& audio() const { return m_audio; }

    // `show_tile` returns the region that is being sent to the output rather
    // than the whole frame that arrived, so the browser can be shown either.
    bool latest_frame(multisite::DecodedVideoFrame& out, uint64_t& version,
                      bool show_tile = false) const;
    uint64_t frame_version() const { return m_frame_version.load(); }

private:
    void poll_loop();
    void feed_loop();
    void deliver_loop();
    // Makes sure the sound card is open, is open on the device the settings now
    // name, and is open at the rate the feed actually runs at. Called both when
    // a frame arrives and when the box is idle — see the note at its
    // definition for why the idle case is not an optimisation.
    void ensure_audio_open(const Config& cfg, int feed_channels, int feed_rate);

    // Asks the delivery thread to let the current card go and open the one the
    // settings now name. A counter, so two requests arriving together are two
    // reopens rather than one — see the note on m_audio_reopen_requests.
    void request_audio_reopen();

    // The AES67 reconciler: keeps an enabled stream up, and does nothing at all
    // to one that was switched off. Runs on its own thread because every step
    // is a REST call or a systemctl call to another process, and the delivery
    // loop is not the place for either.
    void aes67_loop();
    // One pass, for the tests and for the loop above: gather the daemon's state,
    // decide, act. Returns the action taken, as a word, for the log.
    const char* aes67_reconcile_once();

    // Puts the idle screen up when there is no programme going out, and
    // takes it down again when there is. Driven from the poll loop rather
    // than a timer of its own: it only ever needs to act a few times a
    // minute, and never while frames are flowing.
    void update_screen();
    // Everything the identity screen needs to say right now.
    SplashInfo splash_info() const;

    void rebuild_session();          // under m_obj_mtx
    void teardown_decoder();
    void flush_delivery();
    void note_error(const std::string& what);

    std::shared_ptr<multisite::DecoderSession> session_ref() const;
    std::shared_ptr<multisite::CmafDecoder>    decoder_ref() const;

    // One frame on its way to the output, already stamped with the monotonic
    // time it is due.
    struct PendingFrame {
        bool     is_video = true;
        uint64_t due_ns = 0;
        multisite::DecodedVideoFrame video;
        multisite::DecodedAudioFrame audio;
    };
    void enqueue(PendingFrame&& f);
    void on_video(const multisite::DecodedVideoFrame& f);
    void on_audio(const multisite::DecodedAudioFrame& f);
    int64_t anchor_pts(int64_t pts_ns, bool is_video);

    Config       m_cfg;
    mutable std::mutex m_cfg_mtx;
    // Where a settings change the player makes on its own is written back to.
    // Blank means "in memory only", which is what a Player built without a path
    // — a test, say — gets.
    std::string  m_config_path;
    VideoOutput& m_video;
    AudioOutput& m_audio;

    // Cloud, null unless cfg.cloud_configured(). Kept independently of
    // whichever transport DecoderSession actually holds (see rebuild_session)
    // because storage_health() reports on this one specifically.
    std::shared_ptr<multisite::S3Transport>    m_transport;
    // LAN (PROJECT-SCOPE.md §8.7), null unless cfg.lan_configured().
    std::shared_ptr<multisite::LanTransport>       m_lan_transport;
    // Only constructed when BOTH of the above exist; DecoderSession and
    // EventCatalog are built against this when it exists, m_lan_transport
    // alone when there is no cloud leg, or m_transport alone when there is
    // no LAN leg — see rebuild_session().
    std::shared_ptr<multisite::FallbackTransport>  m_fallback_transport;
    std::shared_ptr<multisite::DecoderSession> m_session;
    std::shared_ptr<multisite::CmafDecoder>    m_decoder;
    std::shared_ptr<multisite::EventCatalog>   m_catalog;
    mutable std::mutex m_obj_mtx;

    std::thread m_poll_thread, m_feed_thread, m_deliver_thread;
    std::thread m_aes67_thread;
    std::atomic<bool> m_running{false};

    // One reconcile pass at a time. The loop takes this, and so does the route
    // that switches the stream on: an operator's click must not be interleaved
    // with a repair to the same source, or the last writer wins and it can be
    // the repair — leaving a stream the operator just switched off back on air.
    std::mutex m_aes67_mtx;
    // What the last pass decided and did, so a repeat is silent: a box with no
    // PTP master must not print the same sentence every few seconds.
    std::string m_aes67_last_action;
    uint64_t    m_aes67_last_log_ns = 0;
    // Last PTP state the probe saw, so the periodic status line can report the
    // lock accuracy WITHOUT taking m_aes67_mtx — the probe holds that across
    // HTTP calls, and a status line must never wait on the network. This is the
    // number BUGS.md entry 1 is missing: how tightly a Pi's interface actually
    // holds the clock over a long event.
    std::atomic<bool>   m_aes67_ptp_known{false};
    std::atomic<bool>   m_aes67_ptp_locked{false};
    std::atomic<double> m_aes67_ptp_jitter{0.0};

    // Playout clock: due time = base + (media pts − first media pts).
    std::atomic<uint64_t> m_playout_base_ns{0};
    std::atomic<int64_t>  m_first_pts_ns{-1};
    std::atomic<uint64_t> m_last_resync_log_ns{0};
    std::atomic<bool>     m_decoder_started{false};
    uint64_t              m_seen_discontinuity = 0;

    // An operator action asks for the next poll to happen now rather than
    // whenever the interval comes round: waiting out three seconds before even
    // looking is what makes Load feel like a dropped click.
    std::atomic<bool> m_poll_now{false};
    int      m_last_room = -1;
    uint64_t m_feed_start_ns = 0;      // monotonic time this decoder started
    uint64_t m_pushed_media_ns = 0;    // media duration handed over so far
    bool     m_logged_av_offset = false;
    int64_t  m_last_video_pts_ns = 0;

    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_playing{false};
    std::atomic<bool> m_locked{false};
    std::atomic<bool> m_flushing{false};
    std::atomic<bool> m_awaiting_frames{false};
    std::atomic<long long> m_seek_target_ms{0};
    std::atomic<bool>      m_loading_event{false};

    // Clock reading of the frame currently on screen, advanced per frame so
    // the displayed time moves continuously rather than once per segment.
    std::atomic<long long> m_playing_at_ms{0};
    // The segment of the frame currently going to the output. Used to place a
    // cue exactly, without depending on the media clock (see DecoderSession).
    std::atomic<uint64_t>  m_on_screen_seq{0};
    std::atomic<long long> m_seg_starts_at_ms{0};
    std::atomic<int64_t>   m_seg_first_pts_ns{-1};
    std::atomic<int64_t>   m_skip_until_pts_ns{-1};

    // Which tile of a composited feed goes to the screen. The layout is read
    // from the session at every poll — it cannot change mid-event, but a
    // different event can declare a different one — and the selection from the
    // config. Both are cached as atomics so the delivery loop, which runs
    // thirty times a second, never takes a lock to decide the crop. -1 means
    // the whole picture.
    std::atomic<int> m_tile_cols{1}, m_tile_rows{1}, m_tile_sel{-1};

    std::deque<PendingFrame> m_dq;
    mutable std::mutex       m_dq_mtx;
    std::condition_variable  m_dq_cv;

    std::atomic<uint64_t> m_frames_out{0}, m_frames_dropped{0};
    // Split by stream. A dropped picture repeats the last one; a dropped audio
    // frame is audible. The total could not say which was happening, and which
    // one climbs is the difference between the card stalling and the decoder
    // falling behind.
    std::atomic<uint64_t> m_dropped_video{0}, m_dropped_audio{0};
    // How long present() actually takes. The delivery thread scales the
    // picture and waits for the vertical blank inline, so if the display path
    // is slow it does not just make the picture late — it throttles the whole
    // playout clock, and the symptom shows up as "playout fell behind".
    // Totals since the last status line; read and reset there.
    std::atomic<uint64_t> m_present_ns{0}, m_present_max_ns{0}, m_presents{0};
    // Read only by the poll thread, which is the only writer too.
    uint64_t m_last_frames_out = 0;
    // Same: how many status-line updates in a row have looked like the stall
    // described in BUGS.md entry 0 (frames_out frozen, downloads still
    // succeeding). Reset the moment either sign disappears.
    uint64_t m_last_downloaded_stat = 0;
    int      m_stall_intervals = 0;
    // Cleared when a decoder is created, set by the first frame out of it.
    std::atomic<bool> m_logged_stream{true};
    // Said once per run, not once per discarded frame.
    std::atomic<bool> m_logged_audio_tracks{false};
    // Monotonic time of the last frame that reached the display. What
    // separates "playing" from "nothing is arriving", which is the difference
    // between leaving the picture alone and putting the splash back up.
    std::atomic<uint64_t> m_last_frame_ns{0};
    // What the idle screen currently says. Re-rendering identical text would
    // page-flip the display for no reason.
    std::string m_idle_signature;
    bool        m_idle_showing = false;
    // Boot splash: show the identity screen for the first few seconds even if
    // auto-play is about to put an event on the screen.
    std::atomic<uint64_t> m_boot_splash_until_ns{0};
    std::atomic<bool>     m_boot_splash_drawn{false};
    // The sound card is opened by the delivery thread, once the first decoded
    // frame reveals the feed's rate and channel count. Setting this asks it to
    // let the current device go and open the newly chosen one — otherwise a
    // device picked in the interface would not take effect until the next
    // reboot, which for a box with no keyboard is no use at all.
    //
    // A counter rather than a flag, because two things ask for a reopen and they
    // can arrive together: an operator saving the settings, and an AES67
    // reconcile tick that has decided the card is open at the wrong width. With
    // a flag the second request is silently swallowed by `exchange(false)` and
    // the master goes on air at the old width — which is the whole of the fault
    // this counter was added for.
    std::atomic<int> m_audio_open_state{(int)AudioState::Closed};
    std::atomic<int> m_audio_reopen_requests{0};
    // The width the card was opened at, kept beside the rate for the same
    // reason: a two-channel card against an eight-channel feed is heard as the
    // wrong eight channels and reported by nothing, so it has to be compared
    // and corrected rather than assumed.
    std::atomic<int> m_audio_opened_channels{0};
    // When a failed open may be tried again, and how many times in a row it has
    // failed. The first is a deadline on the monotonic clock; the second only
    // spaces the retries out, so a card that is never going to open is retried
    // every few seconds instead of every tick, and the log gets one line per
    // attempt rather than one per delivery loop.
    std::atomic<uint64_t> m_audio_retry_at_ns{0};
    std::atomic<int>      m_audio_fail_count{0};
    // The card's own message from the last failed open, for the status page.
    mutable std::mutex m_audio_err_mtx;
    // The meters, tapped where the samples are handed to the card. Mutable
    // because reading one also starts the next window.
    mutable AudioMeter m_meter;
    std::string        m_audio_last_error;
    // The rate the feed last reported, and the rate the card was actually
    // opened at. The first is what lets the device be opened before anything
    // has played — a box sitting idle between services still has to put its
    // sound on the network — and the second is what catches a feed that turns
    // out not to be the assumed 48 kHz, so it is reopened rather than played
    // back at the wrong pitch.
    std::atomic<int> m_audio_rate{0};
    std::atomic<int> m_audio_opened_rate{0};

    // Latest decoded picture, kept for the preview.
    mutable std::mutex m_frame_mtx;
    multisite::DecodedVideoFrame m_last_frame;
    std::atomic<uint64_t> m_frame_version{0};

    mutable std::mutex m_events_mtx;
    EventListing       m_events;
    std::atomic<bool>  m_events_refreshing{false};
    std::atomic<bool>  m_events_wanted{false};

    mutable std::mutex m_err_mtx;
    std::string        m_last_error;
    std::atomic<double> m_delay_from_live_s{0.0};
};

} // namespace multisite_player
