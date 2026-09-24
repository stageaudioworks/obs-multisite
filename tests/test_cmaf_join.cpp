// SPDX-License-Identifier: GPL-3.0-or-later
// test_cmaf_join.cpp — one event from two muxers, as a decoder sees it.
//
// An event can be written by more than one CmafMuxer: the headless encoder
// starts a new one after a signal drop, and any encoder does after a restart
// mid-event. A decoder following the event has the first muxer's init segment
// and then simply more segments, so the second muxer's media time has to run on
// from where the first stopped. By default the muxer shifts its first
// timestamp to zero (for the AAC priming delay), which sends a decoder's clock
// back to the start of the event at the join; keep_input_timestamps is the way
// out. Found by multisite-outpost's pipeline test, 2026-09-24.
//
// Frames are encoded here with FFmpeg's own MPEG-4 encoder, which every build
// has; the question is the container's timing, not the codec's.
#include "../src/core/cmaf_decoder.h"
#include "../src/core/cmaf_muxer.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if (!(c)) { std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else std::printf("  [ok]   %s\n", m); } while (0)

constexpr int kW = 320, kH = 240, kFps = 30;
constexpr int64_t kFrameNs = 1000000000LL / kFps;

struct Part {
    std::vector<uint8_t> init;
    std::vector<std::vector<uint8_t>> segs;
    double first_seg_start_s = -1;
};

// `frames` pictures from `first_frame` on, through a fresh encoder and a fresh
// muxer, one keyframe a second and one-second segments.
static bool encode_part(int first_frame, int frames, bool keep, Part& out) {
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    if (!codec) return false;
    AVCodecContext* c = avcodec_alloc_context3(codec);
    c->width = kW;
    c->height = kH;
    c->pix_fmt = AV_PIX_FMT_YUV420P;
    c->time_base = {1, kFps};
    c->framerate = {kFps, 1};
    c->gop_size = kFps;
    c->max_b_frames = 0;
    c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(c, codec, nullptr) < 0) { avcodec_free_context(&c); return false; }

    CmafTrack v;
    v.kind = CmafTrack::Video;
    v.codec_id = AV_CODEC_ID_MPEG4;
    v.width = kW;
    v.height = kH;
    v.fps_num = kFps;
    v.fps_den = 1;
    v.extradata.assign(c->extradata, c->extradata + c->extradata_size);
    CmafMuxer mux({v}, 1.0, keep);
    out.init = mux.init_segment();
    mux.on_segment([&](uint64_t, std::vector<uint8_t> b, double, double start) {
        if (out.first_seg_start_s < 0) out.first_seg_start_s = start;
        out.segs.push_back(std::move(b));
    });

    AVFrame* f = av_frame_alloc();
    f->format = c->pix_fmt;
    f->width = kW;
    f->height = kH;
    av_frame_get_buffer(f, 0);
    AVPacket* pkt = av_packet_alloc();
    auto drain = [&] {
        while (avcodec_receive_packet(c, pkt) == 0) {
            CmafPacket p;
            p.track = 0;
            p.data.assign(pkt->data, pkt->data + pkt->size);
            p.pts_ns = pkt->pts * kFrameNs;
            p.dts_ns = pkt->dts * kFrameNs;
            p.duration_ns = kFrameNs;
            p.keyframe = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
            mux.push(p);
            av_packet_unref(pkt);
        }
    };
    for (int i = 0; i < frames; ++i) {
        av_frame_make_writable(f);
        for (int y = 0; y < kH; ++y)
            std::memset(f->data[0] + y * f->linesize[0], (y + i * 4) & 0xff, kW);
        std::memset(f->data[1], 128, (size_t)f->linesize[1] * kH / 2);
        std::memset(f->data[2], 128, (size_t)f->linesize[2] * kH / 2);
        f->pts = first_frame + i;   // the event's own frame count
        avcodec_send_frame(c, f);
        drain();
    }
    avcodec_send_frame(c, nullptr);
    drain();
    mux.flush();
    av_packet_free(&pkt);
    av_frame_free(&f);
    avcodec_free_context(&c);
    return mux.ok();
}

struct Seen {
    int frames = 0;
    int64_t first = -1, last = -1;
    bool backwards = false;
};

// The first part's init segment, then every segment of both, as a decoder
// following the event receives them.
static Seen play(const Part& a, const Part& b) {
    Seen s;
    std::mutex m;
    CmafDecoder dec;
    dec.on_video([&](const DecodedVideoFrame& f) {
        std::lock_guard<std::mutex> lk(m);
        ++s.frames;
        if (s.first < 0) s.first = f.pts_ns;
        if (s.last >= 0 && f.pts_ns < s.last) s.backwards = true;
        s.last = f.pts_ns;
    });
    if (!dec.start(a.init)) return s;
    uint64_t seq = 0;
    for (const auto& x : a.segs) dec.push_fragment(x, seq++);
    for (const auto& x : b.segs) dec.push_fragment(x, seq++);
    int stable = 0, last = -1;
    for (int i = 0; i < 400 && stable < 8; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        int now;
        { std::lock_guard<std::mutex> lk(m); now = s.frames; }
        if (now == last) ++stable; else { stable = 0; last = now; }
    }
    dec.stop();
    return s;
}

int main() {
    if (!avcodec_find_encoder(AV_CODEC_ID_MPEG4)) {
        std::printf("  [skip] this FFmpeg has no MPEG-4 encoder\n");
        return 0;
    }

    std::printf("Kept: the second muxer runs on from 3 s\n");
    {
        Part a, b;
        CHECK(encode_part(0, 90, true, a) && encode_part(90, 60, true, b), "both parts written");
        CHECK(a.first_seg_start_s == 0.0, "the first part starts at 0");
        CHECK(b.first_seg_start_s > 2.99 && b.first_seg_start_s < 3.01,
              "the second part's segment says 3 s");
        const Seen s = play(a, b);
        std::printf("  played %d frames, pts %.3f..%.3f s\n", s.frames, s.first / 1e9, s.last / 1e9);
        CHECK(s.frames == 150, "all 150 frames play through one decoder");
        CHECK(!s.backwards, "time never goes backwards at the join");
        CHECK(s.first == 0 && s.last > 4900000000LL, "0 s to 5 s, as recorded");
    }

    std::printf("Not kept (the default): the second muxer starts again at 0\n");
    {
        Part a, b;
        encode_part(0, 90, false, a);
        encode_part(90, 60, false, b);
        const Seen s = play(a, b);
        std::printf("  played %d frames, pts %.3f..%.3f s\n", s.frames, s.first / 1e9, s.last / 1e9);
        // The behaviour the option exists to avoid, asserted so this test
        // notices if the default ever changes under the OBS encoder.
        CHECK(s.backwards, "time goes backwards at the join, which is why the option exists");
    }

    std::printf(g_fail ? "cmaf_join: %d failed\n" : "cmaf_join: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
