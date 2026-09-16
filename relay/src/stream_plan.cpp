// SPDX-License-Identifier: GPL-3.0-or-later
#include "stream_plan.h"

#include <algorithm>

namespace multisite_relay {

using multisite::AudioTrack;
using multisite::Manifest;

namespace {

// SRT's retransmit buffer, and deliberately far larger than ffmpeg's own
// 120ms default. 120ms is enough to recover a lost packet only on a path
// short enough that the retransmit arrives almost immediately; on anything
// longer SRT gives up and the loss reaches the destination as a glitch.
//
// Two seconds is §1 applied to the one place in this system where it is
// cheapest: the relay already sits three minutes behind the event, so two
// seconds is invisible, and it buys recovery across a path many times longer
// than the default can manage.
constexpr int kDefaultSrtLatencyMs = 2000;

// ffmpeg gives its SRT timing options in millionths of a second.
constexpr int64_t kUsPerMs = 1000;

std::string channels_word(int ch) {
    if (ch == 1) return "mono";
    if (ch == 2) return "stereo";
    return std::to_string(ch) + " channels";
}

std::string join_labels(const std::vector<AudioTrack>& tracks) {
    std::string out;
    for (size_t i = 0; i < tracks.size(); ++i) {
        if (i) out += (i + 1 == tracks.size()) ? " or " : ", ";
        out += "\"" + tracks[i].label + "\"";
    }
    return out;
}

// A packed multi-channel feed is one stream carrying the main mix, the ISOs
// and the click in fixed channel positions (PROJECT-SCOPE.md 4.3.1). It is
// identified by published channel labels, not by a mode flag — there isn't
// one — and only ever set beyond stereo.
bool is_packed(const AudioTrack& t) {
    return !t.channel_labels.empty() || t.channels > 2;
}

// Replaces every occurrence of `secret` in `s` with `with`. Substring rather
// than whole-argument, because under SRT a secret sits inside a URL alongside
// things worth keeping in the log.
void scrub(std::string& s, const std::string& secret, const char* with) {
    if (secret.empty()) return;
    for (size_t at = s.find(secret); at != std::string::npos;
         at = s.find(secret, at + std::string(with).size()))
        s.replace(at, secret.size(), with);
}

} // namespace

std::string output_url(const Destination& d) {
    if (protocol_of(d) == Protocol::Rtmp) {
        if (d.stream_key.empty()) return d.url;
        std::string u = d.url;
        if (!u.empty() && u.back() == '/') u.pop_back();
        return u + "/" + d.stream_key;
    }

    // SRT. normalize() has already lifted our own parameters out of the
    // address and left anything else on it, so appending here can never
    // produce a duplicate of something the operator pasted.
    std::string u = d.url;
    bool has_query = u.find('?') != std::string::npos;
    auto add = [&u, &has_query](const std::string& k, const std::string& v) {
        u += has_query ? "&" : "?";
        has_query = true;
        u += k + "=" + v;
    };

    if (d.srt_mode == SrtMode::Listener) {
        add("mode", "listener");
        // Wait indefinitely for the far end. A listener that nobody has
        // connected to yet is not a failure and must not be given up on:
        // a broadcast partner may well attach five minutes into the event.
        add("listen_timeout", "-1");
    } else if (!d.stream_key.empty()) {
        add("streamid", d.stream_key);
    }
    if (!d.srt_passphrase.empty()) add("passphrase", d.srt_passphrase);

    const int ms = d.srt_latency_ms > 0 ? d.srt_latency_ms
                                        : kDefaultSrtLatencyMs;
    add("latency", std::to_string((int64_t)ms * kUsPerMs));
    return u;
}

std::string redact(const std::string& text, const Destination& d) {
    std::string s = text;
    scrub(s, d.stream_key, "<key>");
    scrub(s, d.srt_passphrase, "<passphrase>");
    return s;
}

std::vector<std::string> redact(const std::vector<std::string>& args,
                                const Destination& d) {
    std::vector<std::string> out;
    out.reserve(args.size());
    for (const auto& a : args) {
        std::string s = a;
        scrub(s, d.stream_key, "<key>");
        scrub(s, d.srt_passphrase, "<passphrase>");
        out.push_back(std::move(s));
    }
    return out;
}

RoomSendability sendability(const Manifest& manifest) {
    // Two representative destinations, differing only in the one thing that
    // decides this. Nothing is spawned and nothing is connected to: the whole
    // question is answered by plan_stream, which is the same code that will
    // refuse a real destination later. Sharing it is the point — a banner
    // that disagreed with what happens on Start would be worse than none.
    auto probe = [](const char* url) {
        Destination d;
        d.name = "probe";
        d.url = url;
        d.stream_key = "x";
        return d;
    };
    const StreamPlan rtmp = plan_stream(manifest, probe("rtmp://example.invalid/live"), "pipe:0");
    const StreamPlan srt  = plan_stream(manifest, probe("srt://example.invalid:9000"), "pipe:0");

    auto sentence = [](const StreamPlan& p) {
        return p.problem + (p.remedy.empty() ? "" : " " + p.remedy);
    };

    RoomSendability r;
    r.rtmp_ok = rtmp.ok;
    r.srt_ok = srt.ok;
    r.any = rtmp.ok || srt.ok;

    if (!r.any) {
        // Both refused. Either for the same reason — no sound, packed audio,
        // a track that has gone — or, for AV1, for reasons that read the same
        // to the person in front of it. One message either way.
        r.problem = sentence(rtmp);
        return r;
    }
    if (!rtmp.ok) {
        // A protocol that refuses something the other takes. Nothing does
        // today — every remaining refusal is about the content rather than the
        // transport — but the banner should not need rewriting on the day
        // something does. plan_stream's own remedy already names the way out,
        // so it is repeated rather than reworded: two different sentences
        // about one situation is how an operator ends up believing there are
        // two situations.
        r.note = sentence(rtmp);
    } else if (!srt.ok) {
        r.note = sentence(srt);
    }

    // An HEVC event is sendable everywhere now, so the honest line here is a
    // caveat rather than an obstacle. Over RTMP it goes out as Enhanced RTMP,
    // which YouTube takes; a destination that has never heard of it will not,
    // and it will drop the stream rather than complain usefully. Saying so is
    // the difference between an operator learning it here and learning it from
    // a stream that dies on them.
    std::string vcodec = manifest.video.codec;
    std::transform(vcodec.begin(), vcodec.end(), vcodec.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    if (vcodec == "hevc") {
        r.note = "This event is HEVC. It goes out unchanged either way; over "
                 "RTMP it needs somewhere that takes Enhanced RTMP, which "
                 "YouTube does. Somewhere that does not will drop the stream "
                 "as soon as it starts.";
    }
    return r;
}

StreamPlan plan_stream(const Manifest& manifest,
                       const Destination& dest,
                       const std::string& input) {
    StreamPlan p;
    const Protocol proto = protocol_of(dest);

    // ── Video ────────────────────────────────────────────────────────────────
    // The codec gate. HEVC now passes on both protocols: over MPEG-TS it always
    // did, and over RTMP it is carried by Enhanced RTMP, which ffmpeg writes
    // from 6.1 on and which YouTube takes.
    //
    // Verified at the byte level rather than taken from the documentation: a
    // copy remux of an HEVC input through the exact argument vector below
    // produces an Extended VideoTagHeader — byte 0 is 0x90, i.e.
    // isExVideoHeader=1 | VideoFrameType.KeyFrame | VideoPacketType 0, then
    // the FourCC "hvc1", which is the value E-RTMP defines for HEVC. The same
    // run with H.264 produces byte 0x17: the legacy tag, codec 7, which is
    // what makes the difference between the two visible rather than assumed.
    // What is NOT yet verified is a real push to a real destination — see the
    // note in stream_plan.h.
    //
    // AV1 is refused on both, for a different reason on each. Over RTMP ffmpeg
    // can carry it (FLV since 6.1, Enhanced RTMP, FourCC av01 — verified), so
    // the unknown is the destination: YouTube documents AV1 ingest and nothing
    // else obviously does. Over MPEG-TS ffmpeg cannot carry it at all — no AV1
    // stream type in the muxer, private data on the wire, `bin_data` on the way
    // back in — so an SRT destination is not a question of what the far end
    // takes but of whether there is anything to send.
    std::string vc = manifest.video.codec;
    std::transform(vc.begin(), vc.end(), vc.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    if (vc.empty()) {
        p.problem = "The main site has not said what kind of video this is, "
                    "so it cannot be sent on safely.";
        p.remedy  = "Check that the broadcast is running a current version of "
                    "the encoder.";
        return p;
    }
    const bool video_ok = vc == "h264" || vc == "hevc";
    if (!video_ok) {
        // Neither protocol can carry this one, so the remedy must not offer a
        // protocol as an escape. It used to: an AV1 event sent to an RTMP
        // destination was told to try SRT, which cannot carry AV1 either —
        // ffmpeg has no AV1 in MPEG-TS, in either direction. Sending somebody
        // from one refusal to the next is worse than the first refusal.
        p.problem = "This event is being recorded as " + vc + " video, and " +
                    (proto == Protocol::Srt
                       ? "this kind of connection cannot carry it."
                       : "there is no way to send it on from here.");
        p.remedy  = "Set the main site's encoder to H.264 or HEVC for events "
                    "you want to stream publicly: both go out unchanged, to a "
                    "streaming site or over SRT. For this codec there is "
                    "nothing here that can carry it — SRT included — and "
                    "re-encoding on the way out is not built.";
        return p;
    }
    if (dest.allow_transcode) {
        // Refused rather than ignored: silently treating "re-encode this" as
        // "copy it" is exactly the quiet wrong answer the brief rules out.
        p.problem = "This destination is set to re-encode the video.";
        p.remedy  = "Re-encoding is not built yet. Turn it off to send the "
                    "event as-is.";
        return p;
    }

    // ── Audio ────────────────────────────────────────────────────────────────
    const auto& tracks = manifest.audio_tracks;
    if (tracks.empty()) {
        p.problem = "This event has no sound in it.";
        p.remedy  = "Check that the main site has at least one audio track "
                    "switched on.";
        return p;
    }

    // Packed multi-channel: one stream carrying the mix, the ISOs and the
    // click together. Sending it on unchanged would put a mic ISO or the click
    // track out to the public. This has nothing to do with the transport —
    // SRT would carry it perfectly well — so it is refused on both.
    for (const auto& t : tracks) {
        if (!is_packed(t)) continue;
        p.problem = "The main site is sending its sound as one "
                    + channels_word(t.channels) +
                    " track with the mix, the microphones and the click all "
                    "inside it.";
        p.remedy  = "Picking one pair out of that is not built yet. Set the "
                    "main site to send separate audio tracks instead.";
        return p;
    }

    // Resolve the operator's choice. By label, always — the UI never shows an
    // index, and a saved destination must keep meaning the same thing even if
    // the encoder operator reorders their tracks between events.
    int index = -1;
    std::string label;
    if (dest.audio.label.empty()) {
        index = 0;                       // the main mix, by convention
        label = tracks[0].label;
    } else {
        for (size_t i = 0; i < tracks.size(); ++i) {
            if (tracks[i].label != dest.audio.label) continue;
            index = (int)i;
            label = tracks[i].label;
            break;
        }
        if (index < 0) {
            p.problem = "This event does not have a sound feed called \"" +
                        dest.audio.label + "\".";
            p.remedy  = "It is sending " + join_labels(tracks) +
                        ". Choose one of those instead.";
            return p;
        }
    }

    const AudioTrack& chosen = tracks[(size_t)index];

    // The manifest publishes each track's position among the audio streams as
    // `idx`. ffmpeg's -map 0:a:N counts the same way, so the two agree — but
    // only if the encoder wrote them in order. A feed where they disagree
    // would send the wrong track while looking perfectly correct, so it is
    // refused rather than trusted.
    if (chosen.idx != index) {
        p.problem = "The main site's description of its sound feeds does not "
                    "line up with the recording itself.";
        p.remedy  = "Sending it could put the wrong microphone on air, so it "
                    "has been stopped. Please report this.";
        return p;
    }

    std::string ac = chosen.codec;
    std::transform(ac.begin(), ac.end(), ac.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    if (ac != "aac") {
        p.problem = "The sound feed \"" + label + "\" is " + ac +
                    ", and streaming sites need AAC.";
        p.remedy  = "Set the main site's encoder to AAC audio.";
        return p;
    }

    // ── The invocation ───────────────────────────────────────────────────────
    // Copy remux only: no decode, no encode, no quality loss, almost no CPU.
    //
    // Both muxers convert the video from the length-prefixed form fMP4 uses to
    // the start-code form they need, without being asked — it is part of what
    // the muxer does, not a filter we have to add. Adding the bitstream filter
    // by hand as well produced a stream that no destination would decode, so
    // it is deliberately absent.
    p.args = {
        "ffmpeg",
        "-hide_banner",
        // The feeder paces the pipe, so ffmpeg must not add pacing of its own.
        // Measured in the Stage 0 rig: a pipe fed one fragment per segment
        // holds a steady 1.0x with no -re, because being starved between
        // writes IS the clock.
        "-i", input,
        "-map", "0:v:0",
        "-map", "0:a:" + std::to_string(index),
        "-c", "copy",
    };
    if (proto == Protocol::Srt) {
        // The tables that say what is in the stream go out repeatedly rather
        // than only at the start. Anything that attaches partway through — a
        // listener's far end connecting late, a receiver reconnecting after
        // its own outage — otherwise sits on a stream it cannot interpret
        // until we happen to send them again.
        p.args.insert(p.args.end(), {
            "-mpegts_flags", "+resend_headers",
            "-f", "mpegts",
        });
    } else {
        // FLV cannot rewrite its header over a socket; without this ffmpeg
        // logs two alarming failures per run that mean nothing.
        p.args.insert(p.args.end(), {
            "-flvflags", "no_duration_filesize",
            "-f", "flv",
        });
    }
    p.args.push_back(output_url(dest));

    p.ok = true;
    p.audio_index = index;
    p.audio_label = label;

    std::string res;
    if (manifest.video.width > 0 && manifest.video.height > 0) {
        res = std::to_string(manifest.video.width) + "x" +
              std::to_string(manifest.video.height) + " ";
    }
    const std::string codec_word = (vc == "hevc") ? "HEVC" : "H.264";
    p.summary = "sending " + res + codec_word + " video with the \"" + label +
                "\" sound feed (" + channels_word(chosen.channels) + ")";
    if (proto == Protocol::Srt)
        p.summary += dest.srt_mode == SrtMode::Listener
                       ? ", over SRT, waiting for the far end to connect"
                       : ", over SRT";
    return p;
}

} // namespace multisite_relay
