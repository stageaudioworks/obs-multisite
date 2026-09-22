// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// destination.h — one place an event is sent to, and what the operator chose
// for it.
//
// A destination is deliberately dumb data: a URL, a key, and a selection. All
// the judgement about whether it CAN be served (codec, audio shape) lives in
// stream_plan.h, so the same checks run in the UI before an operator presses
// Start and again in the relay before ffmpeg is spawned. Those must never
// disagree.
//
// Two protocols, told apart by the address alone. There is no protocol column
// and no radio button: rtmp:// and srt:// are unmistakable, and asking a
// volunteer to say which one they pasted is asking them to get it wrong.
//
#include <cstdint>
#include <string>

namespace multisite {

// How the operator picked the audio. Selecting by LABEL is what the UI does —
// "Main mix", "Sermon ISO" — because a volunteer must never be asked for a
// track number. The index is kept as a fallback for a feed whose labels the
// encoder operator never set, and as what actually reaches ffmpeg.
struct AudioSelection {
    // Preferred: match the published label from the manifest. Empty means
    // "whatever is first", which is the right default for a stereo-only church
    // that has never thought about tracks.
    std::string label;
    // Resolved at start time from the label. -1 until then.
    int         resolved_index = -1;
};

enum class Protocol {
    Rtmp,   // rtmp:// or rtmps:// — FLV, H.264 only
    Srt,    // srt://  — MPEG-TS, H.264 or HEVC
};

// Which end opens the connection. Caller is what every streaming service and
// CDN wants: we connect outward, exactly as RTMP does, and nothing has to be
// opened on the machine the relay runs on. Listener is for the other case — a
// broadcast partner or a hardware decoder that pulls from us — and does mean
// opening an inbound port, so it is never the default and never inferred from
// anything but an address that plainly says so.
enum class SrtMode { Caller, Listener };

struct Destination {
    int64_t     id = 0;
    std::string name;              // "YouTube", "Facebook" — operator's words
    std::string room_id;           // which feed this sends
    // The address with our secrets taken out of it: rtmp://a.rtmp.youtube.com/
    // live2, or srt://ingest.example.com:9000. Query parameters we do not
    // manage ourselves are left on it untouched, because a service that asks
    // for one of SRT's several dozen other options should not have to fight
    // us about it.
    std::string url;
    // RTMP: the stream key, appended to the address. SRT: the streamid.
    // Secret either way; never logged, never sent to the UI.
    std::string stream_key;
    AudioSelection audio;

    // ── SRT only ─────────────────────────────────────────────────────────────
    SrtMode     srt_mode = SrtMode::Caller;
    // SRT's own encryption. Secret, and held to libsrt's own 10–79 character
    // rule at save time rather than at connect time, where the failure reads
    // like a rejected stream.
    std::string srt_passphrase;
    // SRT's retransmit buffer, in milliseconds. 0 means "use our default",
    // which is deliberately far larger than ffmpeg's own — see stream_plan.cpp.
    int         srt_latency_ms = 0;

    // Explicit, per the brief: a destination that needs re-encoding must be
    // switched on deliberately by an operator who has been told the cost.
    // Stage 1 has no encoder at all, so this being true is itself refused.
    bool        allow_transcode = false;

    // Whether the operator has asked for this to be running. Distinct from
    // whether it IS running: a destination can be enabled and waiting for the
    // encoder to go live.
    bool        enabled = false;

    // How far behind the live edge to sit, in seconds. This is the buffer that
    // absorbs a dropout at the main site so it never reaches the public
    // stream. 0 means "use the room default".
    int         delay_s = 0;
};

// Which protocol this address is, from the scheme. Anything that is not srt://
// is treated as RTMP, because validate() has already refused everything else
// by the time this matters.
Protocol protocol_of(const Destination& d);

// True when the far end connects to us rather than the other way round. Worth
// its own name because it changes what "healthy but sending nothing" means:
// a listener with nobody attached is waiting, not broken.
bool is_listener(const Destination& d);

// Tidy a destination the way it was typed into the way it is stored, in place.
//
// Events publish SRT in every shape there is: a bare srt://host:port with
// the rest on the form beside it, or the whole thing in one line with the
// stream id, passphrase and latency already in the query, or a listener
// address with no host at all. All of those arrive in the same box, so this
// is the one place they are made to agree — and it is what keeps the promise
// that `url` can be shown back to the browser, because the secrets have been
// lifted out of it and into the fields that are never returned.
//
// A value typed into its own field wins over the same value in the pasted
// address: the field is the more deliberate of the two.
void normalize(Destination& d);

// Reasons a destination cannot be saved at all. Returns an empty string when
// it is fine. Phrased for the person reading it in the browser. Expects to be
// called after normalize().
std::string validate(const Destination& d);

// Whether the difference between two versions of a destination is one that
// forces the stream to be rebuilt. Renaming it is not; changing where it goes,
// which sound it carries, or how far behind it sits, is.
//
// This distinction is the whole reason it exists: a destination that is on air
// must not be interrupted because something unrelated to it was edited.
bool affects_stream(const Destination& a, const Destination& b);

} // namespace multisite
