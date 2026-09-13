// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// multisite_ui.h — the interfaces the frontend controls act through.
//
// The encoder output and decoder sources register themselves here; hotkeys and
// Tools-menu items then work on whatever is currently live, without holding
// raw pointers to objects that come and go.
//
#include <string>
#include <utility>
#include <vector>

namespace multisite_obs {

// Implemented by the encoder output while broadcasting.
struct EncoderStats {
    std::string event_id;
    unsigned long long confirmed = 0;
    unsigned long long pending = 0;
    unsigned long long retries = 0;
    unsigned long long bytes = 0;
    int         link_health = 0;      // 0 healthy, 1 degraded, 2 offline
    std::string last_error;
    // What the link is doing, measured from the segment uploads themselves.
    // The colo is the Cloudflare edge serving this bucket — a main site in
    // Johannesburg uploading to Amsterdam explains a queue that will not
    // drain, and nothing else on screen would ever say so.
    std::string colo;
    std::string storage_host;
    double      upload_bytes_per_s = 0.0;
    unsigned long long upload_samples = 0;   // 0 = show no figure at all
    // Set for the life of the broadcast when it began by resuming an
    // interrupted event rather than starting fresh (PROJECT-SCOPE.md §5.1).
    std::string resumed_event_id;
    long long   resumed_event_started_ms = 0;
    unsigned long long resumed_already_confirmed = 0;
};

struct EncoderControls {
    virtual ~EncoderControls() = default;
    virtual void drop_marker(const std::string& label) = 0;
    virtual std::string marker_labels() const = 0;   // comma-separated
    virtual void log_status() = 0;
    // Live figures for the dock: queue depth, retries, link health.
    virtual EncoderStats stats() const = 0;
};

// Implemented by each decoder source.
struct DecoderSnapshot;   // defined below

// One row of the event list. `state` matches multisite::EventState, kept as an
// int for the same reason room_state is: this header stays free of the core.
struct EventEntry {
    std::string event_id;
    // Operator-facing title, from the encoder's event name. Empty for events
    // recorded before naming existed, or where the encoder left it blank.
    std::string name;
    long long   started_ms = 0;
    long long   duration_s = 0;
    int         state = 0;        // 0 unknown, 1 live, 2 recording, 3 interrupted
};

// Everything the dock needs to draw the event list, including why it might be
// empty — "no recordings" and "this key cannot list the bucket" look identical
// otherwise.
struct EventListing {
    std::vector<EventEntry> events;
    bool        loading = false;
    bool        listed_once = false;   // false until the first refresh completes
    bool        fallback_scan = false; // events predating the room index
    int         skipped = 0;
    std::string error;
};

struct DecoderControls {
    virtual ~DecoderControls() = default;
    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual void toggle_pause() = 0;
    virtual void jump_to_live() = 0;
    virtual void log_status() = 0;
    virtual void snapshot(DecoderSnapshot& out) const = 0;
    virtual void jump_to_marker(const std::string& id) = 0;
    virtual void seek(unsigned long long seq) = 0;
    // Re-read settings (including the machine-wide storage config) and
    // restart. Needed when credentials are entered in the dock after a source
    // already exists.
    virtual void reconfigure() = 0;
    // Load/Play separation: loading buffers, playing goes to air.
    virtual void play() = 0;
    virtual void stop_playback() = 0;
    virtual bool is_playing() const = 0;
    // Seek by clock time (roughly one-second accuracy).
    virtual void seek_to_time(long long wall_ms) = 0;
    // Jog by a number of seconds, positive or negative.
    virtual void jog(double seconds) = 0;
    // Sit at a constant delay behind live, e.g. five minutes.
    virtual void set_delay_from_live(double seconds) = 0;
    virtual void set_locked(bool locked) = 0;
    virtual bool locked() const = 0;

    // ── Event list ───────────────────────────────────────────────────────────
    // Ask for a refresh; it runs on the source's worker, never on the UI
    // thread — listing plus one manifest per event is far too much to do while
    // the operator waits.
    virtual void refresh_events() = 0;
    virtual void event_listing(EventListing& out) const = 0;
    // Play one specific event. Pinning does NOT follow the room afterwards: a
    // event starting must not drag an operator out of the recording they are
    // watching. `unpin_event` returns to following live.json.
    virtual void pin_event(const std::string& event_id) = 0;
    virtual void unpin_event() = 0;
};

void register_encoder_controls(EncoderControls* e);
void unregister_encoder_controls(EncoderControls* e);
void register_decoder_controls(DecoderControls* d);
void unregister_decoder_controls(DecoderControls* d);

void register_ui();
void unregister_ui();

// Reaches whichever encoder is currently broadcasting, if any.
bool encoder_stats(EncoderStats& out);
void forward_marker_to_encoder(const std::string& label);

// Snapshot of the decoder sources, for the decoder dock.
struct DecoderSnapshot {
    std::string room_id;
    int         room_state = 0;       // matches RoomState
    // Connection health, measured from this campus's own downloads. 0 healthy,
    // 1 degraded, 2 offline — the same meaning as the encoder's Link row. Only
    // meaningful once link_known is true (a request has been observed).
    int         link_health = 0;
    bool        link_known = false;
    unsigned long long head = 0, live_edge = 0, first_available = 0;
    double      behind_live_s = 0.0, buffered_ahead_s = 0.0;
    bool        paused = false;
    size_t      cached = 0;
    std::string current_marker;
    std::string last_error;
    int         audio_channels = 0;
    // Wall-clock times, so the UI never has to mention sequence numbers.
    long long   playhead_ms = 0, live_ms = 0, earliest_ms = 0, started_ms = 0;
    bool        playing = false;
    // Stopped is narrower than !playing: a source that is loading an event is
    // also not playing, but it is downloading hard. Stopped means idle all the
    // way down — no downloads, no decoder, nothing on air — so the UI can say
    // so instead of showing a stale "behind live" that will never move.
    bool        stopped = false;
    bool        locked = false;
    // A finished recording is video-on-demand: it has an end and a position
    // within it, and "behind live" means nothing.
    bool        ended = false;
    bool        at_end = false;
    bool        was_live = false;   // seen live at some point since loading
    // The encoder died rather than ending: the recording is complete up to
    // that point and plays, but it stops where the encoder stopped.
    bool        interrupted = false;
    // The event actually loaded right now. The dock marks the matching row in
    // the list from this rather than from anything cached with the listing:
    // which row is playing is current state, and a value computed during a
    // listing refresh goes stale the moment the played event changes.
    std::string event_id;
    // Set when a specific past event is being played rather than the room's
    // live one.
    std::string pinned_event_id;
    // Something IS live in this room, but it is not what is playing — the
    // pinned-playback case. The dock offers a jump rather than taking it.
    bool        live_elsewhere = false;
    std::string live_event_id;

    // ── What has been asked for but not yet happened ─────────────────────────
    // The dock answers a click from these, rather than waiting for the network
    // to confirm it. Without them a Load or a jog looked like a dropped click
    // for several seconds and then snapped into place.
    bool        loading = false;        // an event is being switched to
    // Non-zero while playback is heading somewhere it has not arrived at:
    // the time shown is where it is GOING, not where the picture is.
    long long   seek_target_ms = 0;
    bool        buffering = false;      // playing, but nothing decoded yet
    long long   end_ms = 0;
    // Total length of the recording, once it has an end. 0 while live.
    long long   total_ms = 0;
    // Contiguous downloaded ranges as clock times, so the timeline can show
    // exactly what is on disk.
    std::vector<std::pair<long long, long long>> cached_spans;
    // label, id, and the clock time the marker refers to
    struct MarkerEntry { std::string label; std::string id; long long at_ms; };
    std::vector<MarkerEntry> markers;
    // What the main site says each audio channel carries, when it publishes a
    // packed multi-channel feed.
    std::vector<std::string> channel_labels;
    std::string audio_track_label;
    // The link, measured from this campus's own segment downloads. The colo is
    // the Cloudflare edge serving the bucket; a campus a long way from it can
    // otherwise only see that the picture is late, never why.
    std::string colo;
    std::string storage_host;
    double      download_bytes_per_s = 0.0;
    unsigned long long download_samples = 0;   // 0 = show no figure at all
};
bool decoder_snapshot(DecoderSnapshot& out);
void decoder_pause_all();
void decoder_resume_all();
void decoder_jump_live_all();
void decoder_reconfigure_all();
void decoder_play_all();
void decoder_stop_all();
void decoder_seek_time(long long wall_ms);
void decoder_jog(double seconds);
void decoder_set_delay(double seconds);
void decoder_set_locked(bool locked);
void decoder_jump_to_marker(const std::string& id);
void decoder_seek(unsigned long long seq);

// Event list, from the same source the dock's snapshot follows.
void decoder_refresh_events();
bool decoder_event_listing(EventListing& out);
void decoder_pin_event(const std::string& event_id);
void decoder_unpin_event();

} // namespace multisite_obs
