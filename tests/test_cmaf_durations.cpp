// SPDX-License-Identifier: GPL-3.0-or-later
// test_cmaf_durations.cpp — what a segment's recorded duration says.
//
// 2026-10-04, a test run stopped 33 ms after a segment cut: the muxer
// published a last segment of one keyframe that the manifest said lasted 6 s,
// and relay plus could make nothing of it. The duration was the last frame's
// start less the first's: 0 for one frame, which a fallback made the whole
// target, and one frame short on every other segment (5.967 s for 6 s).
//
// Frames come from FFmpeg's MPEG-4 encoder, as in test_cmaf_join: the question
// is the container's timing, not the codec's.
#include "../src/core/cmaf_muxer.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if (!(c)) { std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else std::printf("  [ok]   %s\n", m); } while (0)

constexpr int kW = 160, kH = 120, kFps = 30;
constexpr int64_t kFrameNs = 1000000000LL / kFps;

// `frames` pictures, one keyframe a second, one-second segments: the
// durations the muxer published.
static std::vector<double> durations(int frames) {
    std::vector<double> out;
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    if (!codec) return out;
    AVCodecContext* c = avcodec_alloc_context3(codec);
    c->width = kW;
    c->height = kH;
    c->pix_fmt = AV_PIX_FMT_YUV420P;
    c->time_base = {1, kFps};
    c->framerate = {kFps, 1};
    c->gop_size = kFps;
    c->max_b_frames = 0;
    c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(c, codec, nullptr) < 0) { avcodec_free_context(&c); return out; }

    CmafTrack v;
    v.kind = CmafTrack::Video;
    v.codec_id = AV_CODEC_ID_MPEG4;
    v.width = kW;
    v.height = kH;
    v.fps_num = kFps;
    v.fps_den = 1;
    v.extradata.assign(c->extradata, c->extradata + c->extradata_size);
    CmafMuxer mux({v}, 1.0);
    mux.on_segment([&](uint64_t, std::vector<uint8_t>, double dur, double st) { std::printf("    seg dur=%.4f start=%.4f\n", dur, st); out.push_back(dur); });

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
            p.keyframe = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
            mux.push(p);   // no packet duration, as OBS often sends: the frame rate is used
            av_packet_unref(pkt);
        }
    };
    for (int i = 0; i < frames; ++i) {
        av_frame_make_writable(f);
        std::memset(f->data[0], (i * 4) & 0xff, (size_t)f->linesize[0] * kH);
        std::memset(f->data[1], 128, (size_t)f->linesize[1] * kH / 2);
        std::memset(f->data[2], 128, (size_t)f->linesize[2] * kH / 2);
        f->pts = i;
        avcodec_send_frame(c, f);
        drain();
    }
    avcodec_send_frame(c, nullptr);
    drain();
    mux.flush();
    av_packet_free(&pkt);
    av_frame_free(&f);
    avcodec_free_context(&c);
    return out;
}

static bool near(double a, double b) { return std::fabs(a - b) < 0.001; }

int main() {
    std::printf("stopped one frame after a cut (61 frames)\n");
    auto d = durations(61);
    CHECK(d.size() == 2, "the one-frame last segment is not published");
    CHECK(d.size() >= 2 && near(d[0], 1.0) && near(d[1], 1.0),
          "a segment cut at a keyframe lasts until it: 1.000 s, not 0.967 s");

    std::printf("stopped two-thirds of a second after a cut (80 frames)\n");
    d = durations(80);
    CHECK(d.size() == 3, "a last segment of 20 frames is kept");
    CHECK(d.size() == 3 && near(d[2], 20.0 / kFps),
          "and lasts its 20 frames, 0.667 s: not 19 of them");

    std::printf("an event of one frame\n");
    d = durations(1);
    CHECK(d.size() == 1, "its only segment is kept, however short");
    CHECK(d.size() == 1 && near(d[0], 1.0 / kFps), "and lasts one frame");

    std::printf("\n%s\n", g_fail == 0 ? "CMAF DURATION TESTS PASSED" : "CMAF DURATION TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
