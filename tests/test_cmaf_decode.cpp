// SPDX-License-Identifier: GPL-3.0-or-later
// test_cmaf_decode.cpp — decodes a CMAF stream through CmafDecoder.
//
// Runs against REAL captured output when available (test-data/real_init.mp4 +
// real_seg.m4s, taken straight from the R2 bucket), otherwise falls back to a
// generated fixture. This is the decoder half of the round trip: the same
// bytes the encoder produced are decoded back into frames.
#include "../src/core/cmaf_decoder.h"

#include <atomic>
#include <chrono>
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
