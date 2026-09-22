// SPDX-License-Identifier: GPL-3.0-or-later
// test_stream_plan.cpp — what may be sent onward, and what must be refused.
//
// The failure this guards against is not a crash. It is a relay that looks
// healthy while putting a mic ISO, a click track, or a codec the destination
// cannot decode out to the public. Every refusal here is a stream that would
// otherwise have gone out wrong with no error anywhere.
#include "relay_send.h"

#include <cstdio>
#include <string>

using namespace multisite_relay;
using multisite::AudioTrack;
using multisite::Manifest;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

static AudioTrack track(int idx, const std::string& label, int ch = 2) {
    AudioTrack t;
    t.idx = idx; t.label = label; t.codec = "aac";
    t.channels = ch; t.sample_rate = 48000;
    return t;
}

// The ordinary multi-track event: a stereo main mix, a mono sermon ISO and
// a mono click, exactly as PROJECT-SCOPE.md 4.3 describes.
static Manifest ordinary() {
    Manifest m;
    m.video.codec = "h264"; m.video.width = 1920; m.video.height = 1080;
    m.audio_tracks = { track(0, "Main Mix", 2),
                       track(1, "Sermon ISO", 1),
                       track(2, "Click", 1) };
    return m;
}

static Destination dest(const std::string& label = "") {
    Destination d;
    d.name = "YouTube";
    d.room_id = "main-auditorium";
    d.url = "rtmp://a.rtmp.youtube.com/live2";
    d.stream_key = "secret-key-1234";
    d.audio.label = label;
    return d;
}

// An SRT destination as it comes back out of the store: normalized, because
// that is the only way one ever reaches plan_stream().
static Destination srt_dest(
        const std::string& url = "srt://ingest.example.com:9000",
        const std::string& key = "secret-key-1234") {
    Destination d;
    d.name = "Partner";
    d.room_id = "main-auditorium";
    d.url = url;
    d.stream_key = key;
    normalize(d);
    return d;
}

static bool has_arg(const std::vector<std::string>& a, const std::string& v) {
    for (const auto& x : a) if (x == v) return true;
    return false;
}

static bool any_contains(const std::vector<std::string>& a,
                         const std::string& v) {
    for (const auto& x : a) if (x.find(v) != std::string::npos) return true;
    return false;
}

static bool has_pair(const std::vector<std::string>& a,
                     const std::string& k, const std::string& v) {
    for (size_t i = 0; i + 1 < a.size(); ++i)
        if (a[i] == k && a[i + 1] == v) return true;
    return false;
}

int main() {
    std::printf("stream plan\n");

    // ── The ordinary case ────────────────────────────────────────────────────
    {
        auto p = plan_stream(ordinary(), dest("Sermon ISO"), "/tmp/f.fifo");
        CHECK(p.ok, "a normal H.264 multi-track event can be sent");
        CHECK(p.audio_index == 1, "the chosen track resolves to its position");
        CHECK(has_pair(p.args, "-map", "0:a:1"), "ffmpeg is told to take a:1");
        CHECK(has_pair(p.args, "-map", "0:v:0"), "and the video");
        CHECK(has_pair(p.args, "-c", "copy"), "copy remux, no re-encoding");
        CHECK(p.summary.find("Sermon ISO") != std::string::npos,
              "the summary names what was actually sent");
    }

    // No choice made: the main mix, so a stereo-only church configures nothing.
    {
        auto p = plan_stream(ordinary(), dest(), "/tmp/f.fifo");
        CHECK(p.ok && p.audio_index == 0,
              "choosing nothing sends the first track");
    }

    // ── HEVC over RTMP ───────────────────────────────────────────────────────
    // Used to be refused here, on the grounds that FLV cannot carry HEVC. It
    // can, through Enhanced RTMP, and has since ffmpeg 6.1 — so the refusal was
    // costing an HEVC site its public stream for a constraint that had stopped
    // being true. What is asserted now is that it goes out as a copy, in FLV,
    // and that the summary says HEVC rather than quietly claiming H.264.
    {
        Manifest m = ordinary();
        m.video.codec = "hevc";
        auto p = plan_stream(m, dest("Main Mix"), "/tmp/f.fifo");
        CHECK(p.ok, "HEVC goes to an RTMP destination, as Enhanced RTMP");
        CHECK(has_pair(p.args, "-f", "flv"),
              "in FLV, which is what Enhanced RTMP extends");
        CHECK(has_pair(p.args, "-c", "copy"),
              "still without re-encoding anything");
        CHECK(p.summary.find("HEVC") != std::string::npos,
              "and the summary says HEVC rather than H.264");
    }
    {
        // AV1 over RTMP: allowed, because the only thing standing in the way was
        // what a destination takes, and that is the operator's to weigh rather
        // than ours to refuse. The status line must name AV1 — it once said
        // H.264 for anything that was not HEVC, which was a harmless default
        // until AV1 could actually be sent.
        Manifest m = ordinary();
        m.video.codec = "av1";
        auto p = plan_stream(m, dest("Main Mix"), "/tmp/f.fifo");
        CHECK(p.ok, "AV1 goes to an RTMP destination");
        CHECK(has_pair(p.args, "-f", "flv"),
              "in FLV, which Enhanced RTMP extends");
        CHECK(has_pair(p.args, "-c", "copy"), "as a copy, like the rest");
        CHECK(p.summary.find("AV1") != std::string::npos,
              "and the status line says AV1 rather than defaulting to H.264");
    }
    {
        // ...and not over SRT, where the block is ffmpeg rather than a
        // destination: no AV1 stream type in the MPEG-TS muxer, in either
        // direction. A refusal here is a fact, not a policy.
        Manifest m = ordinary();
        m.video.codec = "av1";
        auto p = plan_stream(m, srt_dest(), "pipe:0");
        CHECK(!p.ok, "AV1 is refused over SRT");
        CHECK(p.problem.find("av1") != std::string::npos,
              "naming what the event actually is");
        CHECK(p.remedy.find("H.264") != std::string::npos &&
              p.remedy.find("HEVC") != std::string::npos,
              "and the way out named is both codecs a destination takes");
        CHECK(p.remedy.find("nothing here that can carry it") != std::string::npos,
              "and it does not offer a second protocol as an escape, "
              "because neither can carry AV1");
    }
    {
        Manifest m = ordinary();
        m.video.codec = "vp9";   // nothing in this pipeline produces it
        auto p = plan_stream(m, dest("Main Mix"), "/tmp/f.fifo");
        CHECK(!p.ok, "an unknown codec is still refused rather than guessed at");
    }
    {
        // Packed multi-channel: mix, ISOs and click in one 8-channel stream.
        Manifest m;
        m.video.codec = "h264";
        AudioTrack packed = track(0, "Production", 8);
        packed.channel_labels = { "Main L", "Main R", "Sermon", "Click",
                                  "Spare", "Spare", "Spare", "Spare" };
        m.audio_tracks = { packed };
        auto p = plan_stream(m, dest(), "/tmp/f.fifo");
        CHECK(!p.ok, "a packed multi-channel feed is refused, not downmixed");
        CHECK(p.problem.find("click") != std::string::npos,
              "and says what is inside it, because that is the danger");
    }
    {
        Manifest m = ordinary();
        auto p = plan_stream(m, dest("Sermon Mic"), "/tmp/f.fifo");
        CHECK(!p.ok, "a saved track name that no longer exists is refused");
        CHECK(p.remedy.find("Main Mix") != std::string::npos,
              "and the operator is told what there is instead");
    }
    {
        // The dangerous one: published positions disagreeing with the file.
        Manifest m = ordinary();
        m.audio_tracks[1].idx = 2;
        auto p = plan_stream(m, dest("Sermon ISO"), "/tmp/f.fifo");
        CHECK(!p.ok, "a manifest whose track positions do not line up is refused");
    }
    {
        Manifest m = ordinary();
        m.audio_tracks.clear();
        CHECK(!plan_stream(m, dest(), "/tmp/f.fifo").ok, "no audio is refused");
    }
    {
        Destination d = dest("Main Mix");
        d.allow_transcode = true;
        CHECK(!plan_stream(ordinary(), d, "/tmp/f.fifo").ok,
              "asking to re-encode is refused, not quietly ignored");
    }

    // ── The stream key ───────────────────────────────────────────────────────
    {
        Destination d = dest("Main Mix");
        auto p = plan_stream(ordinary(), d, "/tmp/f.fifo");
        bool key_present = false;
        for (const auto& a : p.args)
            if (a.find("secret-key-1234") != std::string::npos) key_present = true;
        CHECK(key_present, "the key does reach ffmpeg");

        auto safe = redact(p.args, d);
        bool leaked = false;
        for (const auto& a : safe)
            if (a.find("secret-key-1234") != std::string::npos) leaked = true;
        CHECK(!leaked, "but never survives redaction for the log");
        CHECK(validate(d).empty(), "a complete destination validates");
    }
    {
        Destination d = dest();
        d.stream_key.clear();
        CHECK(!validate(d).empty(), "a missing stream key is caught when saving");
        d = dest(); d.url = "https://youtube.com/watch";
        CHECK(!validate(d).empty(), "so is a web address pasted in by mistake");
    }


    // ── SRT ─────────────────────────────────────────────────────────────────
    std::printf("\nSRT\n");
    {
        auto p = plan_stream(ordinary(), srt_dest(), "pipe:0");
        CHECK(p.ok, "an ordinary event can go out over SRT");
        CHECK(has_pair(p.args, "-f", "mpegts"), "as MPEG-TS, not FLV");
        CHECK(!has_arg(p.args, "flv"), "and nothing FLV comes along with it");
        CHECK(has_pair(p.args, "-c", "copy"),
              "still a copy remux, exactly as RTMP is");
        CHECK(has_pair(p.args, "-mpegts_flags", "+resend_headers"),
              "with the stream tables resent, so a late arrival can decode it");
    }
    {
        // HEVC over SRT: MPEG-TS is a real, long established stream type, so
        // nothing special is needed here — and it is no longer the only way to
        // send HEVC, which is what the wording used to say.
        Manifest m = ordinary();
        m.video.codec = "hevc";
        auto p = plan_stream(m, srt_dest(), "pipe:0");
        CHECK(p.ok, "HEVC goes over SRT as it always did");
        CHECK(p.summary.find("HEVC") != std::string::npos,
              "and the summary says so rather than claiming H.264");
    }
    {
        // The SRT half of AV1 is covered above, next to the RTMP half it used to
        // share a refusal with. What is left here is the content-vs-transport
        // rule, which is why this block moved on.
        Manifest m;
        m.video.codec = "h264";
        AudioTrack packed = track(0, "Production", 8);
        packed.channel_labels = { "Main L", "Main R", "Sermon", "Click" };
        m.audio_tracks = { packed };
        CHECK(!plan_stream(m, srt_dest(), "pipe:0").ok,
              "a packed multi-channel feed is refused over SRT too");
    }

    // ── Pulling a pasted address apart ──────────────────────────────────────
    // What a church is actually handed varies. All of it has to end up meaning
    // the same thing, with the secrets out of the address so what is left can
    // be shown back to the browser.
    {
        Destination d = srt_dest(
            "srt://ingest.example.com:9000?streamid=abc123"
            "&passphrase=hunter2hunter2&latency=1500000&pbkeylen=32", "");
        CHECK(d.stream_key == "abc123", "a pasted stream id is lifted out");
        CHECK(d.srt_passphrase == "hunter2hunter2",
              "so is a pasted passphrase");
        CHECK(d.srt_latency_ms == 1500,
              "and a pasted latency, converted out of ffmpeg's millionths");
        CHECK(d.url.find("streamid") == std::string::npos &&
              d.url.find("hunter2") == std::string::npos,
              "leaving no secret in the address the browser is shown");
        CHECK(d.url.find("pbkeylen=32") != std::string::npos,
              "but an option we do not manage is left exactly where it was");
        CHECK(validate(d).empty(), "and the result is a destination we accept");
    }
    {
        // A field the operator filled in themselves is the more deliberate of
        // the two, and wins.
        Destination d;
        d.name = "Partner"; d.room_id = "r";
        d.url = "srt://ingest.example.com:9000?streamid=from-the-url";
        d.stream_key = "typed-in-the-box";
        normalize(d);
        CHECK(d.stream_key == "typed-in-the-box",
              "what was typed beats what was pasted");
    }
    {
        Destination d = srt_dest("srt://ingest.example.com:9000", "abc");
        const std::string u = output_url(d);
        CHECK(u.find("streamid=abc") != std::string::npos,
              "the stream id goes back on as a query parameter, not a path");
        CHECK(u.find("latency=2000000") != std::string::npos,
              "with our own generous default, in ffmpeg's units");
    }

    // ── Listener mode ───────────────────────────────────────────────────────
    {
        Destination d = srt_dest("srt://:9000", "ignored");
        CHECK(is_listener(d), "an address with no host at all is a listener");
        CHECK(d.url == "srt://0.0.0.0:9000",
              "written out as something ffmpeg will bind");
        CHECK(d.stream_key.empty(),
              "and a stream id is dropped, because a listener is never the "
              "end that sends one");
        CHECK(validate(d).empty(), "a listener with no host still validates");

        auto p = plan_stream(ordinary(), d, "pipe:0");
        CHECK(p.ok, "and can be planned");
        CHECK(any_contains(p.args, "mode=listener"),
              "ffmpeg is told to listen");
        CHECK(any_contains(p.args, "listen_timeout=-1"),
              "and to wait indefinitely, because nobody attached yet is not a "
              "failure");
        CHECK(p.summary.find("waiting") != std::string::npos,
              "which is what the operator is told as well");
    }
    {
        CHECK(!is_listener(srt_dest("srt://ingest.example.com:9000", "abc")),
              "a caller is not mistaken for a listener");
        CHECK(!is_listener(dest()), "and neither is an RTMP destination");
    }

    // ── SRT refusals at save time ───────────────────────────────────────────
    {
        CHECK(!validate(srt_dest("srt://ingest.example.com")).empty(),
              "an SRT address with no port is caught when saving");
        CHECK(!validate(srt_dest("srt://ingest.example.com:99999")).empty(),
              "so is a port that is not a port");

        Destination d = srt_dest();
        d.srt_passphrase = "short";
        CHECK(!validate(d).empty(),
              "so is a passphrase libsrt would reject as too short");

        d = srt_dest();
        d.srt_latency_ms = 2;
        const std::string why = validate(d);
        CHECK(!why.empty(), "so is a latency of two milliseconds");
        CHECK(why.find("millionths") != std::string::npos,
              "and the reason explains the unit that caused it");

        // These would be cut short by ffmpeg's own reading of the address,
        // and the connection refused with nothing said anywhere.
        d = srt_dest("srt://ingest.example.com:9000", "abc&def");
        CHECK(!validate(d).empty(),
              "a stream id containing an & is refused rather than truncated");
    }

    // ── SRT secrets ─────────────────────────────────────────────────────────
    {
        // Under SRT both secrets live INSIDE one URL argument, alongside
        // things worth keeping in the log, so redaction has to reach into an
        // argument rather than drop it whole.
        Destination d = srt_dest();
        d.srt_passphrase = "hunter2hunter2";
        auto p = plan_stream(ordinary(), d, "pipe:0");
        CHECK(any_contains(p.args, "secret-key-1234") &&
              any_contains(p.args, "hunter2hunter2"),
              "both secrets do reach ffmpeg");

        auto safe = redact(p.args, d);
        CHECK(!any_contains(safe, "secret-key-1234"),
              "neither the stream id survives redaction");
        CHECK(!any_contains(safe, "hunter2hunter2"),
              "nor the passphrase");
        CHECK(any_contains(safe, "ingest.example.com"),
              "while the address itself is still legible in the log");
    }


    // ── What the room says it can do ────────────────────────────────────────
    // One warning line sits above every destination. It used to be answered by
    // a single RTMP probe, which was right while RTMP was all there was and
    // became wrong the moment SRT could carry something RTMP could not: an
    // HEVC event was declared unsendable on the page while an SRT
    // destination was busy sending it.
    std::printf("\nroom sendability\n");
    {
        auto r = sendability(ordinary());
        CHECK(r.any && r.rtmp_ok && r.srt_ok,
              "an ordinary H.264 event can go anywhere");
        CHECK(r.problem.empty() && r.note.empty(),
              "and the operator is told nothing, because there is nothing to say");
    }
    {
        // An HEVC event used to be the case this whole mechanism existed for:
        // RTMP could not take it, SRT could, and the banner had to say so.
        // HEVC goes to both now, so what is left to say is the caveat that
        // comes with the RTMP half — and it is still information rather than
        // an obstacle, which is why it lands in `note` and not `problem`.
        Manifest m = ordinary();
        m.video.codec = "hevc";
        auto r = sendability(m);
        CHECK(r.any && r.rtmp_ok && r.srt_ok,
              "an HEVC event can now go to either kind of destination");
        CHECK(r.problem.empty(),
              "so nothing tells the operator the event cannot be streamed");
        CHECK(!r.note.empty(),
              "but they are told what the RTMP half depends on");
        CHECK(r.note.find("Enhanced RTMP") != std::string::npos,
              "which is the destination speaking Enhanced RTMP");
        CHECK(r.note.find("YouTube") != std::string::npos,
              "named, because a volunteer needs somewhere concrete to point at");
        CHECK(r.note.find("SRT") == std::string::npos,
              "and SRT is not offered as a way out, because none is needed");
    }
    {
        // Sendable somewhere, and impossible somewhere else. AV1 is allowed to a
        // streaming site — where the destination question is the operator's to
        // weigh — and refused over SRT, where there is nothing to send. So the
        // banner is a caveat rather than an obstacle, and has to name both
        // halves or it will read as one or the other.
        Manifest m = ordinary();
        m.video.codec = "av1";
        auto r = sendability(m);
        CHECK(r.any && r.rtmp_ok && !r.srt_ok,
              "an AV1 event is sendable to a streaming site and not over SRT");
        CHECK(r.problem.empty(),
              "so the operator is not told the event cannot be streamed");
        CHECK(!r.note.empty(), "but they are told what the RTMP half depends on");
        CHECK(r.note.find("YouTube") != std::string::npos,
              "named, because that is the destination known to take it");
        CHECK(r.note.find("SRT") != std::string::npos,
              "and the SRT half is stated too, as a fact rather than a choice");
    }
    {
        // A content problem, as opposed to a transport one, stops both.
        Manifest m = ordinary();
        m.audio_tracks.clear();
        auto r = sendability(m);
        CHECK(!r.any && !r.problem.empty(),
              "an event with no sound in it can go nowhere either");
    }

    std::printf("== ffmpeg's own stderr is redacted too ==\n");
    {
        // Verified against real ffmpeg output: it echoes the output URL in
        // its error messages, so the secrets come back out through the
        // channel that reports the failure even when the command line was
        // clean. RTMP has always been exposed this way; SRT added a second
        // secret to leak.
        Destination d;
        d.name = "Partner";
        d.url = "srt://feed.example.org:9000";
        d.stream_key = "mystreamid";
        d.srt_passphrase = "SUPERSECRET123";

        const std::string srt_err =
            "[out#0/mpegts @ 0x1] Error opening output "
            "srt://feed.example.org:9000?streamid=mystreamid"
            "&passphrase=SUPERSECRET123&latency=2000000: Protocol not found";
        const std::string safe = redact(srt_err, d);
        CHECK(safe.find("SUPERSECRET123") == std::string::npos,
              "the SRT passphrase is gone from ffmpeg's error line");
        CHECK(safe.find("mystreamid") == std::string::npos,
              "the SRT stream id is gone from ffmpeg's error line");
        CHECK(safe.find("Protocol not found") != std::string::npos,
              "the part an operator needs survives redaction");
        CHECK(safe.find("feed.example.org") != std::string::npos,
              "the address stays, so the message still says where");

        Destination r;
        r.name = "YouTube";
        r.url = "rtmp://a.rtmp.youtube.com/live2";
        r.stream_key = "abcd-efgh-ijkl";
        const std::string rtmp_err =
            "[out#0/flv @ 0x1] Error opening output "
            "rtmp://a.rtmp.youtube.com/live2/abcd-efgh-ijkl: Connection refused";
        const std::string rsafe = redact(rtmp_err, r);
        CHECK(rsafe.find("abcd-efgh-ijkl") == std::string::npos,
              "the RTMP stream key is gone from ffmpeg's error line");
        CHECK(rsafe.find("Connection refused") != std::string::npos,
              "the reason still reaches the operator");
    }

    std::printf("\n%s\n", g_fail == 0 ? "ALL STREAM PLAN TESTS PASSED"
                                      : "SOME TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
