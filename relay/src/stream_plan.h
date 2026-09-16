// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// stream_plan.h — the single decision point: given what the main site is
// actually publishing and what the operator chose, either produce the exact
// ffmpeg invocation, or say in plain words why this cannot be sent.
//
// This is deliberately ONE pure function with no I/O. The UI calls it to warn
// an operator before they press Start; the relay calls it again before it
// spawns anything. If those two ever disagreed, the UI would promise something
// the relay then refuses — so they share this code rather than each having
// their own idea of what is possible.
//
// What may be sent depends on the protocol, which is why the two live in one
// function rather than two. SRT means MPEG-TS, which carries HEVC properly — a
// standardised stream type decoders have handled for a decade.
//
// HEVC over RTMP used to be refused here on the grounds that FLV cannot carry
// it. That is no longer true, and was never quite the whole story: FLV *can*
// carry it, through Enhanced RTMP — the ExVideoTagHeader, a FourCC of `hvc1`
// and a VideoPacketType — which is a released specification (E-RTMP v2, whose
// contributors include Google, Meta, Twitch, FFmpeg and OBS) and which ffmpeg
// has written since 6.1 ("Support HEVC,VP9,AV1 codec in enhanced flv format").
// YouTube documents H.264, H.265 and AV1 for RTMP/RTMPS ingest, and recommends
// H.265 over RTMP(S) for HDR.
//
// What made the old refusal look measured was the toolchain rather than the
// destination: this image used to ship Debian bookworm's ffmpeg 5.1, whose FLV
// muxer has no HEVC in its codec-tag table at all and errors out rather than
// muxing anything. So a test of "HEVC to RTMP" from here could not have been
// testing an Enhanced RTMP stream. The image is trixie now (ffmpeg 7.1), which
// writes the extended tag, and the tag itself is verified at the byte level —
// see the note on the codec gate in stream_plan.cpp.
//
// AV1 is allowed to a streaming site and refused over SRT, for quite different
// reasons. Over RTMP ffmpeg carries it — FLV since 6.1, Enhanced RTMP, FourCC
// av01, verified at the byte level like HEVC's — so the question there is what
// the destination takes: YouTube documents AV1 ingest and nobody else obviously
// does. **That is measured now, not read**: an AV1 event has gone from a real
// encoder through this relay to YouTube and played there for over ten minutes
// without a fault. YouTube is settled; nowhere else is. Over MPEG-TS ffmpeg does
// not carry AV1 at all: no AV1 stream type, so it goes out as private data and
// the demuxer reads it back as opaque bytes. There is an AOMedia mapping for
// AV1-in-MPEG-2-TS; ffmpeg implements it in neither direction. So AV1 over SRT
// would mean carrying a patch to ffmpeg and finding a receiving end that
// understands the result — a different size of job, for a path whose only known
// destination is reachable over RTMP anyway.
//
#include "destination.h"
#include "model.h"

#include <string>
#include <vector>

namespace multisite_relay {

struct StreamPlan {
    bool ok = false;

    // Why not, in the words a volunteer should read. Empty when ok.
    std::string problem;
    // What the operator could do about it, when there is something. Kept
    // separate from `problem` so the UI can style it as guidance.
    std::string remedy;

    // The full ffmpeg argument vector, argv[0] included. Only when ok.
    std::vector<std::string> args;

    // What was actually selected, for the log line and the status panel.
    // "Sending: 1920x1080 H.264, audio 'Sermon ISO' (stereo)". Sending a mic
    // ISO to the public stream by accident is the failure this exists to make
    // impossible to do silently.
    std::string summary;

    std::string audio_label;
    int         audio_index = -1;
};

// Which protocols could carry this event at all, for the one warning line
// the room shows above every destination.
//
// This stopped having a single answer when SRT arrived: the two protocols do
// not accept the same video, so "can this event be streamed?" now depends on
// where it is going. Asked here rather than inline in the status page so the
// banner and the tests get the same answer, and so an event that half works
// says so instead of being declared unsendable.
struct RoomSendability {
    bool any = false;        // at least one protocol can carry it
    bool rtmp_ok = false;
    bool srt_ok = false;
    // Why nothing can carry it. Empty when something can.
    std::string problem;
    // Why some can and some cannot, or why "yes" comes with a caveat. Empty
    // when there is nothing worth saying. Kept apart from `problem` because it
    // is information rather than an obstacle: there IS somewhere to send this.
    // Today that means an HEVC event, which both protocols carry but only a
    // destination speaking Enhanced RTMP will accept over RTMP.
    std::string note;
};

RoomSendability sendability(const multisite::Manifest& manifest);

// `input` is what ffmpeg reads. In the relay this is always "pipe:0": the
// feeder owns the write end and hands over one fragment at a time, and a pipe
// that goes quiet is what the machine reads as a stall.
StreamPlan plan_stream(const multisite::Manifest& manifest,
                       const Destination& dest,
                       const std::string& input);

// The full output URL as ffmpeg receives it: for RTMP the address with the
// stream key appended as a path component, and for SRT the address with the
// stream id, passphrase, latency and mode appended as query parameters.
// Exposed for testing that the secrets never land anywhere they should not;
// callers should prefer plan_stream().
std::string output_url(const Destination& d);

// Everything in `args` with the secrets taken out, for logging. A stream key
// in a log file is a stream key on someone's pastebin — and under SRT the
// secrets are buried inside a URL rather than sitting in an argument of their
// own, so this scrubs within each argument rather than dropping whole ones.
std::vector<std::string> redact(const std::vector<std::string>& args,
                                const Destination& d);

// The same, for a single line of text — specifically ffmpeg's own stderr.
// Redacting the command line is not enough: ffmpeg echoes the output URL back
// in its error messages ("Error opening output srt://host?passphrase=..."),
// and that line is what reaches the operator's screen and the container log.
std::string redact(const std::string& text, const Destination& d);

} // namespace multisite_relay
