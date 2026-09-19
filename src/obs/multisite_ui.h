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

// One cue, wherever it came from: the name the operator typed, its id, who set
// it, and the clock time it refers to. Shared by the encoder and decoder sides
// so one Cues dock draws the same list whatever role this machine is.
struct CueEntry {
    std::string label;
    std::string id;
    std::string author;       // site name; empty reads as the main site
    // Time of day. Display only, and only for a live event — an operator
    // watching live thinks in clock time. Never used to place a cue.
    long long   at_ms = 0;
    // WHERE IN THE PROGRAMME the cue sits, in milliseconds from the start of
    // the event. This is what places it: on the timeline, in the list, and when
    // an operator jumps to it.
    //
    // It replaces two worse answers that were both here. at_ms needed a
    // wall->media conversion that drifted 1.11% (BUGS #2b). seq placed the cue
    // to the nearest SEGMENT — up to six seconds — which is exactly the
    // coarseness BUGS #3 had just removed from timeline clicks. This is exact
    // and needs no conversion. -1 when the cue predates the field and no event
    // start is known to convert its at_ms.
    long long   at_media_ms = -1;
    // The media segment it sits on. Still carried for merge and de-duplication
    // (cues are ordered by seq, which no clock skew can reorder), not for
    // placing anything.
    unsigned long long seq = 0;
};

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

    // LAN / direct delivery (PROJECT-SCOPE.md §8.7).
    bool        lan_running = false;
    int         lan_port = 0;
    unsigned long long lan_cached_segments = 0;
    // The second bucket (PROJECT-SCOPE.md §10 Phase 9) — see Session::Status.
    bool     mirror_configured = false;
    unsigned long long mirror_behind = 0;
    bool     mirror_waiting_on_primary = false;
    bool     mirror_unreachable = false;
    bool     mirror_complete = false;
    std::string lan_error;

    // This machine's clock against the store's, from the HTTP Date header — 0
    // when nothing has been observed. A large value means THIS box is the one
    // that is out; see Transport::server_clock_skew_ms.
    long long   clock_skew_ms = 0;
};

struct EncoderControls {
    virtual ~EncoderControls() = default;
    virtual void drop_marker(const std::string& label) = 0;
    // The cues this event has, merged across every site that has set one.
    virtual void cues(std::vector<CueEntry>& out) const = 0;
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
    // There is no bucket to list, so this list can never fill: no cloud storage
    // is configured for this machine. Distinct from "no recordings" — an event
    // published over the LAN only is on the machine that recorded it and can
    // never appear here, and the dock is where somebody finds that out.
    bool        no_catalog = false;
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
    // Drop a cue with an operator-typed name (shared cues). Fails with a
    // reason when this box has no site name or nowhere to write it.
    virtual void add_cue(const std::string& label, std::string& error) = 0;
    virtual void seek(unsigned long long seq) = 0;
    // Seek by MEDIA time (segment number x one segment) — what a click on the
    // timeline is in. The dock used to turn that into a segment number itself
    // and drop the remainder, so every click landed up to 6 s from where it was
    // made; the session owns the conversion because it owns the segment length.
    virtual void seek_media(long long media_ms) = 0;
    // Re-read settings (including the machine-wide storage config) and
    // restart. Needed when credentials are entered in the dock after a source
    // already exists.
    virtual void reconfigure() = 0;
    // Load/Play separation: loading buffers, playing goes to air.
    virtual void play() = 0;
    virtual void stop_playback() = 0;
    virtual bool is_playing() const = 0;
    // Seek by TIME OF DAY. Kept for the Companion module, whose published
    // action sends one and which is a separate repo on its own release cycle;
    // changing what `ms` means underneath it would break every button already
    // programmed into a Stream Deck. Everything inside this project uses
    // seek_media() — a position, in media time. See BUGS #2b.
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
    // The segment of the frame actually ON SCREEN, when the host knows it. The
    // timeline is anchored to this so its playhead is always the picture.
    unsigned long long playhead_seq = 0;
    // One segment's nominal length, the multiplier that turns a segment number
    // into the media time the bar draws.
    double      segment_duration_s = 6.0;
    double      behind_live_s = 0.0, buffered_ahead_s = 0.0;
    // How much of the buffer is actually down, against the start gate that must
    // be met before Play will go to air. Before playback there is no head to
    // measure ahead of, so buffered_ahead_s reads zero and the dock had nothing
    // to show but a static "Ready" while the link was clearly working. The span
    // is the longest contiguous run on disk, in seconds, so it grows as the
    // buffer fills whether the event is live or a recording.
    double      buffered_span_s = 0.0;
    int         start_buffer_s = 0;
    bool        paused = false;
    size_t      cached = 0;
    std::string current_marker;
    std::string last_error;
    int         audio_channels = 0;
    // MEDIA times: milliseconds from the start of the event. How far into the
    // programme, which is what a position means to an operator and the only
    // unit that needs no conversion to be true. These were wall-clock times,
    // and turning a wall clock back into a position is what drifted 1.11% and
    // put a click up to seven seconds out (BUGS #2b, #2c).
    //
    // started_ms is the exception and is still a time of day: it is the
    // event's identity — when it was recorded — not a position within it.
    long long   playhead_ms = 0, live_ms = 0, earliest_ms = 0;
    long long   started_ms = 0;     // time of day the event began. Metadata.
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
    // Ready to go to air, and what the wait is counting towards. Both come from
    // DecoderSession's start plan, so the display cannot disagree with start() —
    // it did, and mislabelled every finished recording.
    bool        ready_to_play = false;
    // The session's own classification, so no surface has to reconstruct it.
    bool        plays_as_recording = false;
    double      gate_s = 0.0;          // what the start gate wants, in seconds
    double      ready_buffer_s = 0.0;  // what is present towards it, in seconds
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
    // Non-zero while playback is heading somewhere it has not arrived at: the
    // position shown is where it is GOING, not where the picture is. Media
    // time, like the rest.
    long long   seek_target_ms = 0;
    bool        buffering = false;      // playing, but nothing decoded yet
    long long   end_ms = 0;          // media time of the end of what exists
    // Total length of the recording, once it has an end. 0 while live.
    long long   total_ms = 0;
    // Contiguous downloaded ranges in media time, so the timeline can show
    // exactly what is on disk.
    std::vector<std::pair<long long, long long>> cached_spans;
    // …and the same ranges as SEGMENT numbers, which is what the timeline
    // draws: clock times and segment numbers disagree on a restarted event.
    std::vector<std::pair<unsigned long long, unsigned long long>> cached_seq_spans;
    // label, id, who set it, and the clock time the marker refers to
    std::vector<CueEntry> markers;
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
    // LAN / direct delivery (PROJECT-SCOPE.md §8.7, "Visibility"). lan_active
    // means the MOST RECENT fetch actually came from LAN, not cloud — with
    // both configured this can flip request to request (a segment aged out
    // of the LAN retention window falls back to cloud for that one alone),
    // so it describes what just happened, not a sticky mode.
    bool        lan_configured = false;
    // Reads are coming from the SECOND bucket, because the end that was being
    // read stopped advancing (PROJECT-SCOPE.md §10 Phase 9). Only ever set on a
    // machine with a second bucket configured.
    bool        reading_secondary = false;
    bool        lan_active = false;

    // This machine's clock against the store's, from the HTTP Date header — 0
    // when nothing has been observed. A large value means THIS box is the one
    // that is out.
    long long   clock_skew_ms = 0;
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
void decoder_seek_media(long long media_ms);

// ── Cues ─────────────────────────────────────────────────────────────────────
// One merged list for the Cues dock, whichever role this machine is. Prefer the
// encoder's own view while broadcasting; fall back to the decoder's merged view
// (which already includes cues from every site). Returns false when neither is
// present, so the dock can say "no event" rather than show an empty list.
bool encoder_cues(std::vector<CueEntry>& out);
bool decoder_cues(std::vector<CueEntry>& out);
// Drop a cue: to the encoder while broadcasting (it owns the event), otherwise
// through the active decoder's own site object. `error` explains a refusal.
bool decoder_add_cue(const std::string& label, std::string& error);

// Event list, from the same source the dock's snapshot follows.
void decoder_refresh_events();
bool decoder_event_listing(EventListing& out);
void decoder_pin_event(const std::string& event_id);
void decoder_unpin_event();

} // namespace multisite_obs
