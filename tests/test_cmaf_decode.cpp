// SPDX-License-Identifier: GPL-3.0-or-later
// test_cmaf_decode.cpp — decodes a CMAF stream through CmafDecoder.
//
// Runs against REAL captured output when available (test-data/real_init.mp4 +
// real_seg.m4s, taken straight from the R2 bucket), otherwise falls back to a
// generated fixture. This is the decoder half of the round trip: the same
// bytes the encoder produced are decoded back into frames.
#include "../src/core/cmaf_decoder.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

static std::vector<uint8_t> readfile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {});
}

int main(int argc, char** argv) {
    CHECK(native_sw_format_ok(AV_PIX_FMT_NV12) && native_sw_format_ok(AV_PIX_FMT_P010),
          "8- and 10-bit 4:2:0 hardware frames are drawn natively");
    CHECK(!native_sw_format_ok(AV_PIX_FMT_NV16) && !native_sw_format_ok(AV_PIX_FMT_YUV444P) &&
          !native_sw_format_ok(AV_PIX_FMT_P210) && !native_sw_format_ok(AV_PIX_FMT_NONE),
          "other VideoToolbox formats are copied back, not drawn natively");

    std::string init_path = argc > 1 ? argv[1] : "test-data/real_init.mp4";
    std::string seg_path  = argc > 2 ? argv[2] : "test-data/real_seg.m4s";

    auto init = readfile(init_path);
    auto seg  = readfile(seg_path);
    if (init.empty() || seg.empty()) {
        std::printf("  [skip] no fixture at %s / %s\n",
                    init_path.c_str(), seg_path.c_str());
        return 0;
    }
    std::printf("fixture: init=%zu bytes, segment=%zu bytes\n",
                init.size(), seg.size());

    std::atomic<int> vframes{0};
    std::atomic<int> aframes{0};
    std::atomic<int> vwidth{0}, vheight{0};
    std::atomic<long long> last_v_pts{-1};
    std::atomic<bool> pts_monotonic{true};
    std::atomic<bool> planes_valid{true};

    CmafDecoder dec;
    dec.on_video([&](const DecodedVideoFrame& f) {
        ++vframes;
        vwidth = f.width; vheight = f.height;
        // Planes must be populated and strides sane, or OBS would read garbage.
        if (!f.plane[0] || !f.plane[1] || !f.plane[2] ||
            f.stride[0] < f.width || f.data.empty())
            planes_valid = false;
        long long prev = last_v_pts.load();
        if (prev >= 0 && f.pts_ns < prev) pts_monotonic = false;
        last_v_pts = f.pts_ns;
    });
    dec.on_audio([&](const DecodedAudioFrame& f) {
        ++aframes;
        if (f.interleaved.size() != (size_t)f.frames * f.channels)
            planes_valid = false;
    });

    CHECK(dec.start(init), "decoder started with the init segment");
    CHECK(dec.push_fragment(seg), "the fragment is accepted for decoding");

    // Fragments are dequeued immediately and decoded in place, so waiting on
    // queued_bytes() would stop far too early. Wait until frame production
    // stops instead.
    int stable = 0, last = -1;
    for (int i = 0; i < 400 && stable < 8; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        int now = vframes.load() + aframes.load();
        if (now == last) ++stable; else { stable = 0; last = now; }
    }
    dec.stop();

    // A stopped decoder refuses a fragment rather than accepting one it will
    // never decode. This is the boundary the feed loop relies on: it must never
    // be parked here indefinitely (BUGS.md entry 0).
    CHECK(!dec.push_fragment(seg),
          "a stopped decoder refuses a fragment instead of blocking");

    CHECK(dec.ok(), dec.ok() ? "decoder reported no error" : dec.error().c_str());
    CHECK(vframes.load() > 0, "decoded video frames from the fragment");
    CHECK(vwidth.load() > 0 && vheight.load() > 0, "video dimensions reported");
    CHECK(planes_valid.load(), "frame planes and strides are valid");
    CHECK(pts_monotonic.load(), "video timestamps are non-decreasing");
    std::printf("     (decoded %d video frames at %dx%d, %d audio frames)\n",
                vframes.load(), vwidth.load(), vheight.load(), aframes.load());
    CHECK(aframes.load() > 0, "decoded audio frames");

    // ── Decoder preference (the Pi 4 hardware selection) ────────────────────
    //
    // A preference must be exactly that: a hint. A name that does not exist has
    // to fall through to the same decoder the default path chose, and a name
    // that does exist has to be the one that actually runs. Deliberately
    // self-calibrating: whatever codec this fixture turns out to be, the
    // software decoder's own reported name is used as the known-good
    // preference in the third run, so the test never assumes H.264.
    struct RunResult { int frames; std::string codec; };
    auto decode_with = [&](const std::string& pref) -> RunResult {
        std::atomic<int> frames{0};
        CmafDecoder d;
        if (!pref.empty())
            d.set_preferred_video_decoders({ pref });
        d.on_video([&](const DecodedVideoFrame&) { ++frames; });
        d.start(init);
        d.push_fragment(seg);
        int stable = 0, last = -1;
        for (int i = 0; i < 400 && stable < 8; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            int now = frames.load();
            if (now == last) ++stable; else { stable = 0; last = now; }
        }
        d.stop();
        return RunResult{ frames.load(), d.video_codec() };
    };

    const RunResult plain = decode_with("");
    const RunResult bogus = decode_with("definitely_not_a_real_decoder");
    CHECK(bogus.frames > 0, "an unknown decoder preference still decodes");
    CHECK(bogus.codec == plain.codec,
          "an unknown preference falls back to the default decoder");
    const RunResult named = decode_with(plain.codec);
    CHECK(named.frames > 0, "a known decoder preference still decodes");
    CHECK(named.codec == plain.codec,
          "a known preference selects the named decoder");
    std::printf("     (default decoder: %s)\n", plain.codec.c_str());

    // ── VideoToolbox (macOS hardware decode) ────────────────────────────────
    //
    // "videotoolbox" is the codec's software decoder with a VideoToolbox device
    // attached. On macOS it must actually be used and give the same picture;
    // elsewhere the name falls through to software like any missing decoder.
    struct Picture { int frames = 0; bool all_i420 = true; int w = 0, h = 0;
                     std::vector<std::vector<uint8_t>> y; std::string codec;
                     std::vector<long long> pts; };
    auto grab = [&](const std::string& pref) {
        Picture p;
        std::mutex m;
        CmafDecoder d;
        if (!pref.empty()) d.set_preferred_video_decoders({ pref });
        d.on_video([&](const DecodedVideoFrame& f) {
            std::lock_guard<std::mutex> lk(m);
            ++p.frames; p.w = f.width; p.h = f.height;
            p.pts.push_back(f.pts_ns);
            if (!f.plane[0] || !f.plane[1] || !f.plane[2] ||
                f.stride[1] < f.width / 2) p.all_i420 = false;
            if (f.plane[0]) {
                std::vector<uint8_t> y((size_t)f.width * f.height);
                for (int r = 0; r < f.height; ++r)
                    std::memcpy(&y[(size_t)r * f.width], f.plane[0] + (size_t)r * f.stride[0], f.width);
                p.y.push_back(std::move(y));
            }
        });
        d.start(init);
        d.push_fragment(seg);
        int stable = 0, last = -1;
        for (int i = 0; i < 400 && stable < 8; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            std::lock_guard<std::mutex> lk(m);
            if (p.frames == last) ++stable; else { stable = 0; last = p.frames; }
        }
        d.stop();
        p.codec = d.video_codec();
        return p;
    };
    {
        const Picture sw = grab(""), vt = grab("videotoolbox");
        CHECK(vt.frames > 0, "the videotoolbox preference still decodes");
#ifdef __APPLE__
        // VideoToolbox only takes H.264 and HEVC; the AV1 fixture falls through.
        if (sw.codec != "h264" && sw.codec != "hevc")
            CHECK(vt.codec == sw.codec, "a non-H.264/HEVC stream falls through to software");
        else {
        CHECK(vt.codec.find("videotoolbox") != std::string::npos, "videotoolbox used");
        CHECK(vt.frames == sw.frames, "same picture: same frame count");
        CHECK(vt.all_i420 && vt.w == sw.w && vt.h == sw.h,
              "same picture: I420 at the same size");
        bool close = vt.y.size() == sw.y.size() && !vt.y.empty();
        for (size_t i = 0; close && i < vt.y.size(); ++i) {
            double sum = 0;
            for (size_t k = 0; k < vt.y[i].size(); ++k)
                sum += std::abs((int)vt.y[i][k] - (int)sw.y[i][k]);
            if (sum / vt.y[i].size() >= 2.0) close = false;
        }
        CHECK(close, "same picture: mean Y difference under 2.0 per pixel");
        // The hardware frame is copied to system memory before scaling, and
        // that copy once dropped the timestamp: every picture came out at
        // pts 0, so playout saw each as seconds overdue and re-anchored
        // continually (2.5 fps instead of 30).
        CHECK(vt.pts == sw.pts && vt.pts.size() > 1 && vt.pts.front() != vt.pts.back(),
              "same picture: videotoolbox frames keep their timestamps");
        }
#else
        CHECK(vt.codec == sw.codec, "without VideoToolbox the preference falls through to software");
#endif
    }

    // ── Native frames: hardware frames stay on the GPU ──────────────────────
    {
        std::mutex m;
        int frames = 0; bool all_native = true, sized = true;
        std::vector<DecodedVideoFrame> kept;
        std::vector<long long> pts;
        {
            CmafDecoder d;
            d.set_preferred_video_decoders({ "videotoolbox" });
            d.set_native_frames(true);
            d.on_video([&](const DecodedVideoFrame& f) {
                std::lock_guard<std::mutex> lk(m);
                ++frames; pts.push_back(f.pts_ns);
                if (!f.native || !f.data.empty()) all_native = false;
                if (f.width <= 0 || f.height <= 0) sized = false;
                if (kept.size() < 2) kept.push_back(f);   // copies outlive the decoder
            });
            d.start(init);
            d.push_fragment(seg);
            int stable = 0, last = -1;
            for (int i = 0; i < 400 && stable < 8; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
                std::lock_guard<std::mutex> lk(m);
                if (frames == last) ++stable; else { stable = 0; last = frames; }
            }
            d.stop();
        }   // decoder destroyed here
        const Picture sw = grab("");
        CHECK(frames > 0, "native mode still decodes");
#ifdef __APPLE__
        if (sw.codec == "h264" || sw.codec == "hevc") {
            CHECK(all_native && sized, "native frames: data empty, native set");
            CHECK(frames == sw.frames, "native frames: same count as the I420 decode");
            CHECK(!kept.empty() && kept[0].width == sw.w && kept[0].height == sw.h,
                  "native frames: same size as the I420 decode");
            CHECK(pts == sw.pts, "native frames keep their timestamps");
            // The copy kept after the decoder is gone must still hold a live
            // AVFrame whose CVPixelBuffer is readable.
            bool alive = !kept.empty();
            for (auto& k : kept) {
                auto* av = static_cast<AVFrame*>(k.native.get());
                if (!av || av->format != AV_PIX_FMT_VIDEOTOOLBOX || !av->data[3] ||
                    av->width != sw.w) alive = false;
            }
            CHECK(alive, "native frame lifetime: a copy outlives the decoder");
        }
#else
        CHECK(all_native == false, "without VideoToolbox the flag is ignored (frames are I420)");
#endif
    }

    // ── A wedged decode thread must not freeze stop() (BUGS.md entry 0) ──────
    //
    // The stall was a decode thread parked while the feed loop waited on it.
    // push() is bounded for that; stop() is bounded for the other half — a
    // thread wedged where it will never return, which used to park teardown's
    // join for ever. The wedge is planted in a frame callback, which runs on
    // the decode thread, so stop() faces exactly that un-joinable thread.
    {
        CmafDecoder d;
        d.set_stop_grace_ms_for_testing(200);

        // Heap state, held by the callback: the abandoned thread outlives this
        // block, so anything it touches must outlive it too.
        struct Gate {
            std::mutex              m;
            std::condition_variable cv;
            std::atomic<bool>       release{false};
            std::atomic<bool>       in_cb{false};
        };
        auto gate = std::make_shared<Gate>();
        d.on_video([gate](const DecodedVideoFrame&) {
            gate->in_cb = true;
            std::unique_lock<std::mutex> lk(gate->m);
            gate->cv.wait(lk, [&] { return gate->release.load(); });
        });
        d.start(init);
        d.push_fragment(seg);
        for (int i = 0; i < 300 && !gate->in_cb.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(gate->in_cb.load(), "decoder reached the frame callback (the wedge)");

        const auto t0 = std::chrono::steady_clock::now();
        d.stop();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        CHECK(ms < 3000,
              "stop() returns instead of hanging on a wedged decode thread");

        // Let the planted thread go, so it does not outlive the process.
        { std::lock_guard<std::mutex> lk(gate->m); gate->release = true; }
        gate->cv.notify_all();
    }

    std::printf("\n%s\n", g_fail == 0 ? "CMAF DECODE TESTS PASSED"
                                      : "CMAF DECODE TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
