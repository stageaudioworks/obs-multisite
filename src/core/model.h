// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// model.h — the storage-protocol data model: live pointer, event descriptor,
// rolling manifest, and markers. JSON (de)serialisation via nlohmann.
//
#include <string>
#include <vector>
#include <cstdint>

namespace multisite {

// ── Protocol version ─────────────────────────────────────────────────────────
//
// The version of the storage protocol itself, carried in every document the
// encoder writes. It exists so that a decoder meeting a bucket it cannot
// understand says so, instead of half-reading it and failing somewhere further
// on that looks like corruption.
//
// One integer rather than a semver, because a reader has exactly one question —
// "can I still understand this?" — and one number answers it.
//
// **Bump it only for a change that would make an older reader misread a
// bucket.** Never for an addition an older reader can ignore: every field added
// so far has been that kind, which is why `j.value(field, default)` is used
// throughout and why this starts at 1 rather than at the number of times the
// format has grown.
//
// **Absent means 1.** Every bucket written before this field existed parses as
// version 1 and keeps working untouched, exactly as an absent tile layout
// parses as 1x1. There is nothing to migrate and no flag day.
inline constexpr int kProtocolVersion = 1;

// Whether a document claiming version `v` can be read by this build.
//
// Older is always readable: we go on reading what we once wrote, because a
// recording made last year is exactly what someone wants to play back. Newer is
// not readable, and refusing plainly is the whole point of the field — the
// alternative is a satellite that shows nothing and cannot say why.
//
// A version of 0 or less is treated as 1: it means a document that named the
// field but left it empty or unparseable, and the oldest protocol is the safest
// assumption to read it under.
inline constexpr bool protocol_readable(int v) { return v <= kProtocolVersion; }

// rooms/{room}/live.json — points at the current live event.
struct LivePointer {
    // The protocol version this document was written under. See
    // kProtocolVersion. Defaults to the version this build writes; parsing a
    // document without the field yields 1.
    int         protocol_version = kProtocolVersion;
    std::string room_id;
    std::string event_id;
    std::string status = "live";     // "live" | "ended"
    int64_t     updated_at_ms = 0;    // heartbeat; drives decoder stale-detection
    std::string to_json() const;
    static LivePointer from_json(const std::string&);
};

struct AudioTrack {
    int         idx = 0;
    std::string label;
    std::string codec = "aac";
    int         channels = 2;
    int         sample_rate = 48000;
    // Packed multi-channel mode: channel ORDER is the interface between the
    // encoder and the satellite (channel 3 must be the click at both ends), so
    // the mapping is published rather than inferred. Positional names from the
    // speaker layout (FL/FR/LFE/...) are meaningless here and are ignored.
    // Empty for ordinary stereo tracks.
    std::vector<std::string> channel_labels;
};

// How one encoded picture is divided into discrete ones at the satellite.
//
// A room that needs two or four separate pictures composites them at the main
// site and sends one feed; the satellite pulls them apart. Doing that with crop
// filters by hand is what this replaces — the encoder says how it composited,
// and the decoder exposes each region as its own source.
//
// Published rather than inferred, for the same reason AudioTrack::channel_labels
// is: aspect ratio cannot tell 2x1 from 1x1 (a 3840x1080 feed is a legitimate
// ultrawide single picture), and guessing wrong splits a programme in half.
//
// 1x1 is the default and means "one picture", which is every event written
// before this existed. Nothing has to migrate: an absent field parses to 1x1.
struct TileLayout {
    int cols = 1;
    int rows = 1;

    int count() const { return cols * rows; }
    bool is_split() const { return count() > 1; }

    // "2x2" / "2x1" / "1x1". Parsing is deliberately strict — an unrecognised
    // string becomes 1x1 rather than a guess, because showing one whole picture
    // when the layout is unknown is recoverable by hand and showing a wrongly
    // cropped one is not obviously wrong at all.
    std::string to_string() const;
    static TileLayout parse(const std::string& s);

    // Where tile `index` sits in a frame_w x frame_h picture, counted in
    // reading order: left to right, then top to bottom, so tile 0 is always
    // top-left. An out-of-range index returns the whole frame rather than
    // something empty — a source asking for a tile the layout does not have is
    // misconfigured, and a whole picture says so where a black rectangle does
    // not.
    //
    // Every edge is rounded DOWN to an even number. The decoded frame is I420,
    // whose chroma planes are half resolution in both directions, so an odd
    // offset or width has no corresponding chroma sample to start from and the
    // crop would shear the colour away from the luma. Rounding down rather than
    // up keeps the rectangle inside the frame.
    struct Rect { int x = 0, y = 0, w = 0, h = 0; };
    Rect tile_rect(int index, int frame_w, int frame_h) const;
};

struct VideoInfo {
    std::string codec = "h264";
    int width = 0, height = 0;
    double fps = 0.0;
    TileLayout layout;
};

// events/{event_id}/event.json — static descriptor written once at Go Live.
struct EventInfo {
    int         protocol_version = kProtocolVersion;   // see kProtocolVersion
    std::string event_id;
    std::string room_id;
    // Operator-facing title, e.g. "Sun 14 Sep 2025, 10:30" or "Harvest
    // Festival". Empty means "no custom name" and the UI falls back to the
    // start time.
    std::string name;
    int64_t     started_at_ms = 0;
    uint64_t    first_seq = 0;
    double      segment_duration_s = 6.0;
    std::string init = "init.mp4";
    VideoInfo   video;
    std::vector<AudioTrack> audio_tracks;
    std::string to_json() const;
    static EventInfo from_json(const std::string&);
};

// rooms/{room_id}/events/{event_id}.json — a per-room index entry, written once
// at Go Live alongside event.json.
//
// Events live in a flat global events/ namespace, so nothing in an object key
// says which room an event belongs to; only event.json does, inside the body.
// Without this index, listing a room's events would mean listing every event
// ever recorded and fetching event.json for each just to discard most of them.
// The entry is small and duplicates a few fields on purpose: listing one room
// then costs a single request, and the start time needed to label the event is
// already in the key listing.
struct RoomEventEntry {
    int         protocol_version = kProtocolVersion;   // see kProtocolVersion
    std::string event_id;
    std::string room_id;
    // Mirrors EventInfo.name so a listing can label the event without reading
    // the full descriptor.
    std::string name;
    int64_t     started_at_ms = 0;
    std::string to_json() const;
    static RoomEventEntry from_json(const std::string&);
};

struct ManifestSegment {
    uint64_t    seq = 0;
    double      duration_s = 6.0;
    std::string checksum;            // sha256 hex of the segment
    // Wall-clock time of the CONTENT in this segment (event start plus its
    // offset in the programme), not the time it happened to be uploaded.
    // Operators think in clock time — "just after 10:42" — never in sequence
    // numbers, so this is what the UI shows.
    int64_t     at_ms = 0;
};

// events/{event_id}/manifest.json — rolling live-edge window.
struct Manifest {
    // Carried here as well as in event.json because the event catalogue reads
    // manifests directly when listing a room's recordings, without ever
    // fetching the descriptor beside them. A listing should be able to mark an
    // event unreadable rather than offer it and fail on load.
    int         protocol_version = kProtocolVersion;   // see kProtocolVersion
    std::string event_id;
    std::string status = "live";
    // Operator-facing event title, carried here (not only in event.json) so the
    // event list can show it without an extra request.
    std::string name;
    int64_t     updated_at_ms = 0;
    // When this event started, so a satellite can convert any position to a
    // clock time even for segments outside the rolling window.
    int64_t     started_at_ms = 0;
    uint64_t    first_available_seq = 0;  // oldest still-retained (timeslip floor)
    uint64_t    window_start_seq = 0;     // oldest listed here
    uint64_t    latest_seq = 0;           // live edge
    std::string init = "init.mp4";
    VideoInfo   video;
    std::vector<AudioTrack>    audio_tracks;
    std::vector<ManifestSegment> segments;
    std::string to_json() const;
    static Manifest from_json(const std::string&);

    // Append a confirmed segment, trimming the window to `window` entries and
    // advancing latest_seq. Enforces the rolling window.
    void push(const ManifestSegment& s, size_t window);

    // Typical segment duration, taken from the listed segments (decoders use
    // it to estimate how far behind live they are). 0 if unknown.
    double stream_duration_hint() const;
};

struct Marker {
    uint64_t    seq = 0;
    int64_t     at_ms = 0;
    std::string type = "cue";
    std::string label;
    std::string id;
    // Display name of the site that dropped this cue — "Main site", a campus
    // name, whatever the operator called themselves. Empty means the main
    // site: every cue written before authors existed, and any cue the encoder
    // drops without a name of its own.
    std::string author;
};

struct MarkerList {
    int         protocol_version = kProtocolVersion;   // see kProtocolVersion
    std::vector<Marker> markers;
    std::string to_json() const;
    static MarkerList from_json(const std::string&);
};

// ── Object-key layout ────────────────────────────────────────────────────────
// Written by the encoder, read by every satellite. Both sides build keys from
// these rather than from string literals, so a change cannot be applied to one
// end and forgotten at the other.
std::string live_pointer_key(const std::string& room_id);
std::string room_events_prefix(const std::string& room_id);
std::string room_event_key(const std::string& room_id, const std::string& event_id);
std::string event_prefix_for(const std::string& event_id);

// ── Cues ─────────────────────────────────────────────────────────────────────
// Cues live ONE OBJECT PER AUTHOR, so the encoder and every satellite can drop
// cues for the same event without any of them overwriting another's. The
// encoder keeps writing markers.json — its own file, and what older decoders
// still read — while a satellite writes cues/{token}.json. A reader merges
// both, so a cue dropped at any site reaches every site.
std::string cues_prefix_for(const std::string& event_id);
// A URL-safe token for a site name, so "Campus B (north)" becomes a stable,
// repeatable object key. An empty or wholly-unsafe name falls back to "site".
std::string cue_author_token(const std::string& display_name);
std::string cue_object_key(const std::string& event_id,
                           const std::string& display_name);
// Merge cue lists into one, oldest first: by at_ms, then seq, then id, so the
// order is stable when two cues share a millisecond. A cue id appearing in more
// than one list is kept once.
MarkerList merge_markers(std::vector<MarkerList> parts);

// Recover the event id from a room-index key or a listing's common prefix,
// whichever form the caller has. Returns "" if the string is neither.
std::string event_id_from_index_key(const std::string& key);

} // namespace multisite
