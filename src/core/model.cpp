// SPDX-License-Identifier: GPL-3.0-or-later
#include "model.h"
#include "../vendor/nlohmann/json.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace multisite {

// The protocol version a document claims, read the same way everywhere.
//
// An absent field means the document predates versioning, which is version 1.
// So does a field holding something that is not a positive number — a string, a
// null, a zero — because this arrives from a bucket any encoder version may
// have written, and the oldest protocol is the safest one to read an unclear
// document under. Deliberately never throws: a malformed version must not be
// the thing that stops an event playing.
static int read_protocol_version(const json& j) {
    if (!j.contains("protocol_version")) return 1;
    const json& v = j["protocol_version"];
    if (!v.is_number_integer()) return 1;
    const int n = v.get<int>();
    return n > 0 ? n : 1;
}

// ── TileLayout ────────────────────────────────────────────────────────────────
std::string TileLayout::to_string() const {
    return std::to_string(cols) + "x" + std::to_string(rows);
}

TileLayout TileLayout::parse(const std::string& s) {
    TileLayout t;                       // 1x1 unless the string says otherwise
    const size_t x = s.find('x');
    if (x == std::string::npos || x == 0 || x + 1 >= s.size()) return t;
    // Hand-rolled rather than sscanf so that trailing junk ("2x2 "), a negative
    // ("-2x2") or a huge count cannot slip through as a plausible layout. This
    // arrives from a bucket that any encoder version may have written.
    const std::string a = s.substr(0, x), b = s.substr(x + 1);
    auto digits_only = [](const std::string& v) {
        if (v.empty() || v.size() > 2) return false;
        for (char c : v) if (c < '0' || c > '9') return false;
        return true;
    };
    if (!digits_only(a) || !digits_only(b)) return t;
    const int c = std::stoi(a), r = std::stoi(b);
    // An upper bound because this indexes sources and allocates crops. Four
    // across is already past what a satellite can usefully show.
    if (c < 1 || r < 1 || c > 4 || r > 4) return t;
    t.cols = c; t.rows = r;
    return t;
}

TileLayout::Rect TileLayout::tile_rect(int index, int frame_w, int frame_h) const {
    auto even_down = [](int v) { return v > 0 ? v & ~1 : 0; };
    Rect whole{ 0, 0, even_down(frame_w), even_down(frame_h) };
    if (index < 0 || index >= count() || frame_w <= 0 || frame_h <= 0) return whole;
    if (!is_split()) return whole;

    const int col = index % cols, row = index / cols;
    // Width is computed from the edges rather than as a per-tile constant, so
    // the rightmost tile absorbs any remainder instead of leaving a strip of
    // the picture in no tile at all.
    const int x0 = even_down((int)((int64_t)frame_w * col / cols));
    const int x1 = even_down((int)((int64_t)frame_w * (col + 1) / cols));
    const int y0 = even_down((int)((int64_t)frame_h * row / rows));
    const int y1 = even_down((int)((int64_t)frame_h * (row + 1) / rows));
    Rect r{ x0, y0, x1 - x0, y1 - y0 };
    if (r.w <= 0 || r.h <= 0) return whole;
    return r;
}

static json video_to_json(const VideoInfo& v) {
    json j = { {"codec", v.codec}, {"width", v.width},
               {"height", v.height}, {"fps", v.fps} };
    // Written only when it says something. A 1x1 field in every event.json ever
    // written would be noise, and its absence already means 1x1.
    if (v.layout.is_split()) j["layout"] = v.layout.to_string();
    return j;
}
static VideoInfo video_from_json(const json& j) {
    VideoInfo v;
    v.codec  = j.value("codec", "h264");
    v.width  = j.value("width", 0);
    v.height = j.value("height", 0);
    v.fps    = j.value("fps", 0.0);
    v.layout = TileLayout::parse(j.value("layout", "1x1"));
    return v;
}
static json tracks_to_json(const std::vector<AudioTrack>& ts) {
    json a = json::array();
    for (const auto& t : ts) {
        json e = { {"idx", t.idx}, {"label", t.label}, {"codec", t.codec},
                   {"channels", t.channels}, {"sample_rate", t.sample_rate} };
        if (!t.channel_labels.empty()) e["channel_labels"] = t.channel_labels;
        a.push_back(e);
    }
    return a;
}
static std::vector<AudioTrack> tracks_from_json(const json& j) {
    std::vector<AudioTrack> out;
    if (!j.is_array()) return out;
    for (const auto& t : j) {
        AudioTrack a;
        a.idx        = t.value("idx", 0);
        a.label      = t.value("label", "");
        a.codec      = t.value("codec", "aac");
        a.channels   = t.value("channels", 2);
        a.sample_rate= t.value("sample_rate", 48000);
        if (t.contains("channel_labels") && t["channel_labels"].is_array())
            for (const auto& cl : t["channel_labels"])
                a.channel_labels.push_back(cl.get<std::string>());
        out.push_back(a);
    }
    return out;
}

// ── LivePointer ───────────────────────────────────────────────────────────────
std::string LivePointer::to_json() const {
    json j = { {"protocol_version", protocol_version},
               {"room_id", room_id}, {"event_id", event_id},
               {"status", status}, {"updated_at_ms", updated_at_ms} };
    return j.dump();
}
LivePointer LivePointer::from_json(const std::string& s) {
    json j = json::parse(s);
    LivePointer p;
    p.protocol_version = read_protocol_version(j);
    p.room_id       = j.value("room_id", "");
    p.event_id      = j.value("event_id", "");
    p.status        = j.value("status", "live");
    p.updated_at_ms = j.value("updated_at_ms", (int64_t)0);
    return p;
}

// ── EventInfo ─────────────────────────────────────────────────────────────────
std::string EventInfo::to_json() const {
    json j;
    j["protocol_version"]   = protocol_version;
    j["event_id"]           = event_id;
    j["room_id"]            = room_id;
    j["name"]               = name;
    j["started_at_ms"]      = started_at_ms;
    j["first_seq"]          = first_seq;
    j["segment_duration_s"] = segment_duration_s;
    j["init"]               = init;
    j["video"]              = video_to_json(video);
    j["audio_tracks"]       = tracks_to_json(audio_tracks);
    return j.dump();
}
EventInfo EventInfo::from_json(const std::string& s) {
    json j = json::parse(s);
    EventInfo e;
    e.protocol_version   = read_protocol_version(j);
    e.event_id           = j.value("event_id", "");
    e.room_id            = j.value("room_id", "");
    e.name               = j.value("name", "");
    e.started_at_ms      = j.value("started_at_ms", (int64_t)0);
    e.first_seq          = j.value("first_seq", (uint64_t)0);
    e.segment_duration_s = j.value("segment_duration_s", 6.0);
    e.init               = j.value("init", "init.mp4");
    if (j.contains("video")) e.video = video_from_json(j["video"]);
    if (j.contains("audio_tracks")) e.audio_tracks = tracks_from_json(j["audio_tracks"]);
    return e;
}

// ── Object-key layout ─────────────────────────────────────────────────────────
std::string live_pointer_key(const std::string& room_id) {
    return "rooms/" + room_id + "/live.json";
}
std::string room_events_prefix(const std::string& room_id) {
    return "rooms/" + room_id + "/events/";
}
std::string room_event_key(const std::string& room_id, const std::string& event_id) {
    return room_events_prefix(room_id) + event_id + ".json";
}
std::string event_prefix_for(const std::string& event_id) {
    return "events/" + event_id + "/";
}

std::string event_id_from_index_key(const std::string& key) {
    // Accepts "rooms/{room}/events/{id}.json" (an index key) or
    // "events/{id}/" (a common prefix from listing the flat namespace).
    if (key.empty()) return "";
    size_t last = key.find_last_not_of('/');
    if (last == std::string::npos) return "";
    const bool had_slash = (last + 1 < key.size());
    size_t start = key.find_last_of('/', last);
    std::string tail = (start == std::string::npos)
                     ? key.substr(0, last + 1)
                     : key.substr(start + 1, last - start);
    if (!had_slash) {
        // An index key ends in .json; a prefix does not.
        const std::string ext = ".json";
        if (tail.size() > ext.size() &&
            tail.compare(tail.size() - ext.size(), ext.size(), ext) == 0)
            tail = tail.substr(0, tail.size() - ext.size());
        else
            return "";
    }
    return tail;
}

// ── RoomEventEntry ────────────────────────────────────────────────────────────
std::string RoomEventEntry::to_json() const {
    json j = { {"protocol_version", protocol_version},
               {"event_id", event_id}, {"room_id", room_id},
               {"name", name}, {"started_at_ms", started_at_ms} };
    return j.dump();
}
RoomEventEntry RoomEventEntry::from_json(const std::string& s) {
    json j = json::parse(s);
    RoomEventEntry e;
    e.protocol_version = read_protocol_version(j);
    e.event_id      = j.value("event_id", "");
    e.room_id       = j.value("room_id", "");
    e.name          = j.value("name", "");
    e.started_at_ms = j.value("started_at_ms", (int64_t)0);
    return e;
}

// ── Manifest ──────────────────────────────────────────────────────────────────
void Manifest::push(const ManifestSegment& s, size_t window) {
    // De-duplicate: a crash between publishing the manifest and clearing the
    // spool causes a harmless re-upload, which must not double-list the segment.
    for (auto& existing : segments) {
        if (existing.seq == s.seq) {
            existing = s;                       // refresh in place
            if (s.seq > latest_seq) latest_seq = s.seq;
            return;
        }
    }
    segments.push_back(s);
    if (s.seq > latest_seq) latest_seq = s.seq;
    while (segments.size() > window) segments.erase(segments.begin());
    window_start_seq = segments.empty() ? 0 : segments.front().seq;
}

double Manifest::stream_duration_hint() const {
    // The median, and emphatically NOT the last listed segment's duration.
    //
    // The last segment of a finished recording is the partial fragment the
    // broadcast ended on — the one sample in the list guaranteed not to be
    // typical. Taking it set the whole time base to that fragment's length:
    // in one recording, 4.1 s against a real 6.0 s.
    //
    // That is not a cosmetic error, because this value maps sequence numbers
    // onto clock times for every segment outside the manifest's rolling
    // window. A third off the grid meant the timeline axis, "behind live",
    // the buffered and rewindable figures and the recording's total length
    // were all short by the same third — while the playing clock, which comes
    // from real frame timestamps, was not. Position could therefore read past
    // the total length of the very recording it was playing.
    //
    // A live event hid it: its last listed segment is an ordinary one, so the
    // fault only appeared once somebody loaded a finished recording.
    //
    // A median cannot be shifted by one atypical sample, which is what the
    // comment here always claimed and the code never did.
    std::vector<double> d;
    d.reserve(segments.size());
    for (const auto& s : segments)
        if (s.duration_s > 0.1) d.push_back(s.duration_s);
    if (d.empty()) return 0.0;
    std::sort(d.begin(), d.end());
    // Upper median on an even count: with exactly one full segment and one
    // short final one, the full one is the better estimate of the grid.
    return d[d.size() / 2];
}

std::string Manifest::to_json() const {
    json j;
    j["protocol_version"]    = protocol_version;
    j["event_id"]            = event_id;
    j["status"]              = status;
    j["name"]                = name;
    j["updated_at_ms"]       = updated_at_ms;
    j["started_at_ms"]       = started_at_ms;
    j["first_available_seq"] = first_available_seq;
    j["window_start_seq"]    = window_start_seq;
    j["latest_seq"]          = latest_seq;
    j["init"]                = init;
    j["video"]               = video_to_json(video);
    j["audio_tracks"]        = tracks_to_json(audio_tracks);
    json segs = json::array();
    for (const auto& s : segments)
        segs.push_back({ {"seq", s.seq}, {"duration_s", s.duration_s},
                         {"checksum", s.checksum}, {"at_ms", s.at_ms} });
    j["segments"] = segs;
    return j.dump();
}
Manifest Manifest::from_json(const std::string& s) {
    json j = json::parse(s);
    Manifest m;
    m.protocol_version    = read_protocol_version(j);
    m.event_id            = j.value("event_id", "");
    m.status              = j.value("status", "live");
    m.name                = j.value("name", "");
    m.updated_at_ms       = j.value("updated_at_ms", (int64_t)0);
    m.started_at_ms       = j.value("started_at_ms", (int64_t)0);
    m.first_available_seq = j.value("first_available_seq", (uint64_t)0);
    m.window_start_seq    = j.value("window_start_seq", (uint64_t)0);
    m.latest_seq          = j.value("latest_seq", (uint64_t)0);
    m.init                = j.value("init", "init.mp4");
    if (j.contains("video")) m.video = video_from_json(j["video"]);
    if (j.contains("audio_tracks")) m.audio_tracks = tracks_from_json(j["audio_tracks"]);
    if (j.contains("segments") && j["segments"].is_array()) {
        for (const auto& seg : j["segments"]) {
            ManifestSegment ms;
            ms.seq        = seg.value("seq", (uint64_t)0);
            ms.duration_s = seg.value("duration_s", 6.0);
            ms.checksum   = seg.value("checksum", "");
            ms.at_ms      = seg.value("at_ms", (int64_t)0);
            m.segments.push_back(ms);
        }
    }
    return m;
}

// ── Markers ───────────────────────────────────────────────────────────────────
std::string MarkerList::to_json() const {
    json arr = json::array();
    for (const auto& mk : markers)
        arr.push_back({ {"seq", mk.seq}, {"at_ms", mk.at_ms},
                        {"type", mk.type}, {"label", mk.label}, {"id", mk.id},
                        {"author", mk.author} });
    return json({ {"protocol_version", protocol_version},
                  {"markers", arr} }).dump();
}
MarkerList MarkerList::from_json(const std::string& s) {
    json j = json::parse(s);
    MarkerList ml;
    ml.protocol_version = read_protocol_version(j);
    if (j.contains("markers") && j["markers"].is_array()) {
        for (const auto& mk : j["markers"]) {
            Marker m;
            m.seq   = mk.value("seq", (uint64_t)0);
            m.at_ms = mk.value("at_ms", (int64_t)0);
            m.type  = mk.value("type", "cue");
            m.label = mk.value("label", "");
            m.id    = mk.value("id", "");
            m.author = mk.value("author", "");
            ml.markers.push_back(m);
        }
    }
    return ml;
}

// ── Cues ─────────────────────────────────────────────────────────────────────
std::string cues_prefix_for(const std::string& event_id) {
    return event_prefix_for(event_id) + "cues/";
}

std::string cue_author_token(const std::string& display_name) {
    std::string t;
    bool last_dash = false;
    for (char c : display_name) {
        const unsigned char u = (unsigned char)c;
        if (std::isalnum(u)) {
            t.push_back((char)std::tolower(u));
            last_dash = false;
        } else if (c == ' ' || c == '-' || c == '_' || c == '.' || c == '/') {
            if (!t.empty() && !last_dash) { t.push_back('-'); last_dash = true; }
        }
        // Anything else (quotes, brackets, non-ASCII) is dropped rather than
        // turned into a separator: a name with punctuation in it should not
        // leave a trail of dashes in the object key.
    }
    while (!t.empty() && t.back() == '-') t.pop_back();
    if (t.empty()) t = "site";
    return t;
}

std::string cue_object_key(const std::string& event_id,
                           const std::string& display_name) {
    return cues_prefix_for(event_id) + cue_author_token(display_name) + ".json";
}

MarkerList merge_markers(std::vector<MarkerList> parts) {
    std::vector<Marker> all;
    for (auto& p : parts)
        for (auto& m : p.markers) all.push_back(m);
    // By the EVENT's own position first, then wall time, then id.
    //
    // seq is what makes this clock-independent: it is the segment the cue was
    // dropped at, counted by the encoder, so it means the same thing at every
    // site. Ordering by at_ms first would let one box whose clock is a minute
    // out drag its cue to the wrong place on everybody else's timeline —
    // exactly the failure a shared cue list must not have. at_ms stays as the
    // tie-break within a segment (and as the displayed time), so two cues
    // dropped at the same moment still order deterministically.
    std::stable_sort(all.begin(), all.end(),
                     [](const Marker& a, const Marker& b) {
                         if (a.seq   != b.seq)   return a.seq   < b.seq;
                         if (a.at_ms != b.at_ms) return a.at_ms < b.at_ms;
                         return a.id < b.id;
                     });
    MarkerList out;
    for (auto& m : all) {
        if (!m.id.empty()) {
            bool dup = false;
            for (const auto& e : out.markers)
                if (e.id == m.id) { dup = true; break; }
            if (dup) continue;
        }
        out.markers.push_back(m);
    }
    return out;
}

} // namespace multisite
