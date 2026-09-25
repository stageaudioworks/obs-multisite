// SPDX-License-Identifier: GPL-3.0-or-later
// test_cmaf_tracks.cpp — audio tracks of different widths, decoded together.
//
// An event's audio tracks need not match: PROJECT-SCOPE.md 4.3's own example
// is a stereo main mix beside mono ISOs, and a MultisiteOS encoder sends its
// stereo programme beside every channel packed (4.3.1). CmafDecoder converted
// every track's audio with one resampler, built for whichever track's first
// frame came out first, so every other track was read with the wrong channel
// count: the packed track came back as the programme's samples smeared across
// eight channels. Found by multisite-outpost's packed-track test, 2026-09-25.
//
// Each channel of each track carries its own tone, so what comes back says
// which channel it was.
//
// The eight-channel track is octagonal, not 7.1. As 7.1 its fourth channel is
// the LFE, which AAC codes as low frequencies only: a tone there does not come
// back at all, which the last check shows. A packed track that must carry
// full-range sound on every channel cannot be 7.1.
#include "../src/core/cmaf_decoder.h"
#include "../src/core/cmaf_muxer.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if (!(c)) { std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else std::printf("  [ok]   %s\n", m); } while (0)

constexpr int kW = 320, kH = 240, kFps = 30, kRate = 48000, kSeconds = 3;
constexpr int64_t kFrameNs = 1000000000LL / kFps;

// The tone on channel `ch` of track `t`: all different, none a harmonic of
// another that matters at this resolution.
static double tone_hz(int t, int ch) { return 300.0 + 150.0 * (t * 8 + ch); }
static const int kChannels[] = {2, 1, 8};   // a stereo mix, a mono ISO, a packed eight
constexpr int kTracks = 3;

struct Stream {
    std::vector<uint8_t> init;
    std::vector<std::vector<uint8_t>> segs;
};

static bool g_packed_as_7_1 = false;   // the trap, for the last check

static AVCodecContext* open_aac(int channels) {
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    AVCodecContext* c = avcodec_alloc_context3(codec);
    c->sample_rate = kRate;
    c->sample_fmt = AV_SAMPLE_FMT_FLTP;
    if (channels == 8 && !g_packed_as_7_1) c->ch_layout = AV_CHANNEL_LAYOUT_OCTAGONAL;
    else av_channel_layout_default(&c->ch_layout, channels);
    c->bit_rate = 64000 * channels;
    c->time_base = {1, kRate};
    c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(c, codec, nullptr) < 0) avcodec_free_context(&c);
    return c;
}

static bool encode(Stream& out) {
    const AVCodec* vcodec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    AVCodecContext* vc = avcodec_alloc_context3(vcodec);
    vc->width = kW;
    vc->height = kH;
    vc->pix_fmt = AV_PIX_FMT_YUV420P;
    vc->time_base = {1, kFps};
    vc->framerate = {kFps, 1};
    vc->gop_size = kFps;
    vc->max_b_frames = 0;
    vc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(vc, vcodec, nullptr) < 0) return false;
    AVCodecContext* ac[kTracks];
    for (int t = 0; t < kTracks; ++t)
        if (!(ac[t] = open_aac(kChannels[t]))) return false;

    std::vector<CmafTrack> tracks(1 + kTracks);
    tracks[0].kind = CmafTrack::Video;
    tracks[0].codec_id = AV_CODEC_ID_MPEG4;
    tracks[0].width = kW;
    tracks[0].height = kH;
    tracks[0].fps_num = kFps;
    tracks[0].fps_den = 1;
    tracks[0].extradata.assign(vc->extradata, vc->extradata + vc->extradata_size);
    for (int t = 0; t < kTracks; ++t) {
        CmafTrack& a = tracks[1 + t];
        a.kind = CmafTrack::Audio;
        a.codec_id = AV_CODEC_ID_AAC;
        a.sample_rate = kRate;
        a.channels = kChannels[t];
        a.frame_size = 1024;
        a.extradata.assign(ac[t]->extradata, ac[t]->extradata + ac[t]->extradata_size);
    }
    CmafMuxer mux(tracks, 1.0, true);
    out.init = mux.init_segment();
    mux.on_segment([&](uint64_t, std::vector<uint8_t> b, double, double) { out.segs.push_back(std::move(b)); });

    AVPacket* pkt = av_packet_alloc();
    auto drain_video = [&] {
        while (avcodec_receive_packet(vc, pkt) == 0) {
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
    auto drain_audio = [&](int t) {
        while (avcodec_receive_packet(ac[t], pkt) == 0) {
            if (pkt->pts >= 0) {
                CmafPacket p;
                p.track = 1 + t;
                p.data.assign(pkt->data, pkt->data + pkt->size);
                p.pts_ns = p.dts_ns = pkt->pts * 1000000000LL / kRate;
                p.duration_ns = 1024 * 1000000000LL / kRate;
                p.keyframe = true;
                mux.push(p);
            }
            av_packet_unref(pkt);
        }
    };

    AVFrame* vf = av_frame_alloc();
    vf->format = vc->pix_fmt;
    vf->width = kW;
    vf->height = kH;
    av_frame_get_buffer(vf, 0);
    AVFrame* af[kTracks];
    for (int t = 0; t < kTracks; ++t) {
        af[t] = av_frame_alloc();
        af[t]->format = AV_SAMPLE_FMT_FLTP;
        af[t]->nb_samples = 1024;
        av_channel_layout_copy(&af[t]->ch_layout, &ac[t]->ch_layout);
        av_frame_get_buffer(af[t], 0);
    }
    int64_t audio_frames = 0;
    for (int i = 0; i < kSeconds * kFps; ++i) {
        // Sound up to this picture, in 1024s, every track the same frames.
        while (audio_frames * 1000000000LL / kRate <= (int64_t)i * kFrameNs) {
            for (int t = 0; t < kTracks; ++t) {
                av_frame_make_writable(af[t]);
                for (int ch = 0; ch < kChannels[t]; ++ch) {
                    float* s = (float*)af[t]->extended_data[ch];
                    for (int n = 0; n < 1024; ++n)
                        s[n] = 0.3f * (float)std::sin(2 * M_PI * tone_hz(t, ch) *
                                                      (double)(audio_frames + n) / kRate);
                }
                af[t]->pts = audio_frames;
                avcodec_send_frame(ac[t], af[t]);
                drain_audio(t);
            }
            audio_frames += 1024;
        }
        av_frame_make_writable(vf);
        for (int y = 0; y < kH; ++y) std::memset(vf->data[0] + y * vf->linesize[0], (y + i) & 0xff, kW);
        std::memset(vf->data[1], 128, (size_t)vf->linesize[1] * kH / 2);
        std::memset(vf->data[2], 128, (size_t)vf->linesize[2] * kH / 2);
        vf->pts = i;
        avcodec_send_frame(vc, vf);
        drain_video();
    }
    avcodec_send_frame(vc, nullptr);
    drain_video();
    mux.flush();
    av_packet_free(&pkt);
    av_frame_free(&vf);
    for (int t = 0; t < kTracks; ++t) { av_frame_free(&af[t]); avcodec_free_context(&ac[t]); }
    avcodec_free_context(&vc);
    return mux.ok() && !out.segs.empty();
}

// Power at `hz` over `x` (Goertzel).
static double power_at(const std::vector<float>& x, double hz) {
    const double w = 2 * M_PI * hz / kRate, k = 2 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (float v : x) { const double s = v + k * s1 - s2; s2 = s1; s1 = s; }
    return (s1 * s1 + s2 * s2 - k * s1 * s2) / (x.empty() ? 1.0 : (double)x.size());
}

// Which (track, channel) tone `x` carries, as t * 8 + ch; -1 for none.
static int which_tone(const std::vector<float>& x) {
    int best = -1;
    double best_p = 1e-3;
    for (int t = 0; t < kTracks; ++t)
        for (int ch = 0; ch < kChannels[t]; ++ch) {
            const double p = power_at(x, tone_hz(t, ch));
            if (p > best_p) { best_p = p; best = t * 8 + ch; }
        }
    return best;
}

struct Decoded {
    int tracks = 0;
    std::map<int, int> channels;
    std::map<int, std::vector<std::vector<float>>> samples;
};

static Decoded decode(const Stream& s) {
    Decoded out;
    std::mutex m;
    CmafDecoder dec;
    dec.on_audio([&](const DecodedAudioFrame& a) {
        std::lock_guard<std::mutex> lk(m);
        out.channels[a.track_index] = a.channels;
        auto& chs = out.samples[a.track_index];
        if ((int)chs.size() != a.channels) chs.assign((size_t)a.channels, {});
        if (a.pts_ns < 500000000) return;   // clear of the encoders' start
        for (uint32_t i = 0; i < a.frames; ++i)
            for (int c = 0; c < a.channels; ++c)
                chs[(size_t)c].push_back(a.interleaved[(size_t)i * a.channels + c]);
    });
    if (!dec.start(s.init)) return out;
    uint64_t seq = 0;
    for (const auto& x : s.segs) dec.push_fragment(x, seq++);
    size_t last = 0;
    for (int i = 0, stable = 0; i < 400 && stable < 8; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        size_t now = 0;
        {
            std::lock_guard<std::mutex> lk(m);
            for (auto& kv : out.samples) for (auto& c : kv.second) now += c.size();
        }
        if (now == last) ++stable; else { stable = 0; last = now; }
    }
    out.tracks = dec.audio_track_count();
    dec.stop();
    return out;
}

static bool in_order(Decoded& d, int t) {
    bool right = (int)d.samples[t].size() == kChannels[t];
    std::printf("  track %d, channel -> tone:", t + 1);
    for (int ch = 0; ch < (int)d.samples[t].size(); ++ch) {
        const int got = which_tone(d.samples[t][(size_t)ch]);
        std::printf(" %d->%d.%d", ch + 1, got < 0 ? 0 : got / 8 + 1, got < 0 ? 0 : got % 8 + 1);
        if (got != t * 8 + ch) right = false;
    }
    std::printf("\n");
    return right;
}

int main() {
    if (!avcodec_find_encoder(AV_CODEC_ID_MPEG4) || !avcodec_find_encoder(AV_CODEC_ID_AAC)) {
        std::printf("  [skip] this FFmpeg lacks the MPEG-4 or AAC encoder\n");
        return 0;
    }
    std::printf("A stereo mix, a mono ISO and a packed eight\n");
    Stream s;
    CHECK(encode(s), "a stream with a stereo, a mono and an eight-channel track");
    Decoded got = decode(s);
    CHECK(got.tracks == kTracks, "three audio tracks");
    for (int t = 0; t < kTracks; ++t) {
        char what[160];
        std::snprintf(what, sizeof what, "track %d comes back with its own %d channel%s", t + 1,
                      kChannels[t], kChannels[t] == 1 ? "" : "s");
        CHECK(got.channels[t] == kChannels[t], what);
        std::snprintf(what, sizeof what, "and every channel of track %d is its own, in order", t + 1);
        CHECK(in_order(got, t), what);
    }

    std::printf("The same eight as 7.1: its fourth channel is the LFE\n");
    g_packed_as_7_1 = true;
    Stream s71;
    encode(s71);
    got = decode(s71);
    CHECK(got.channels[2] == 8 && got.samples[2].size() == 8 &&
              which_tone(got.samples[2][3]) == -1,
          "a full-range tone on channel 4 of a 7.1 track does not come back");

    std::printf(g_fail ? "cmaf_tracks: %d failed\n" : "cmaf_tracks: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
