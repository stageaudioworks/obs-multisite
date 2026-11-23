// SPDX-License-Identifier: GPL-3.0-or-later
// test_segment_cut.cpp — a keyframe one frame short of the target still cuts,
// including exactly one frame short as the muxer's arithmetic computes it.
//
// OBS truncates the keyframe interval to whole frames, so at the
// broadcast rates (23.976, 29.97, 59.94) the interval keyframe arrives just
// before the target segment length. The old rule — cut at or past the target —
// skipped it, and every segment ran to the next keyframe, twice as long as set.
//
// The keyframe times here are computed exactly as OBS computes them, so the
// cases are the ones a real encoder produces, not round numbers.
#include "../src/core/segment_cut.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

// The segment length the muxer actually produces: walk OBS's keyframes and
// return how far in the first cut lands.
static double first_segment_s(int keyint_sec, int fps_num, int fps_den,
                              double target_s) {
    // obs-x264.c and obs-ffmpeg-video-encoders.c: integer, truncating.
    int keyint_frames = keyint_sec * fps_num / fps_den;
    double frame_s = (double)fps_den / fps_num;
    for (int k = 1; k <= 4; ++k) {
        double at = k * keyint_frames * frame_s;
        if (keyframe_starts_segment(at, target_s, frame_s)) return at;
    }
    return -1.0;
}

static bool near(double a, double b) { return a > b - 0.001 && a < b + 0.001; }

int main() {
    // The plugin asks for keyint_sec = (int)(segment_duration_s + 0.5).
    const int keyint = 6;
    const double target = 6.0;

    std::printf("== integer rates: the keyframe is on the target, as before ==\n");
    CHECK(near(first_segment_s(keyint, 30, 1, target), 6.0),     "30 fps cuts at 6.000 s");
    CHECK(near(first_segment_s(keyint, 60, 1, target), 6.0),     "60 fps cuts at 6.000 s");
    CHECK(near(first_segment_s(keyint, 25, 1, target), 6.0),     "25 fps cuts at 6.000 s");

    std::printf("== broadcast rates: the truncated keyframe is the cut ==\n");
    CHECK(near(first_segment_s(keyint, 30000, 1001, target), 5.9726),
          "29.97 fps cuts at 5.973 s (179 frames), not 11.945 s");
    CHECK(near(first_segment_s(keyint, 60000, 1001, target), 5.9893),
          "59.94 fps cuts at 5.989 s (359 frames), not 11.979 s");
    CHECK(near(first_segment_s(keyint, 24000, 1001, target), 5.9643),
          "23.976 fps cuts at 5.964 s (143 frames), not 11.929 s");

    std::printf("== other segment lengths the plugin allows ==\n");
    CHECK(near(first_segment_s(2, 30000, 1001, 2.0), 1.9686),
          "2 s at 29.97 fps cuts at 1.969 s (59 frames)");
    CHECK(near(first_segment_s(15, 30000, 1001, 15.0), 14.981),
          "15 s at 29.97 fps cuts at 14.981 s (449 frames)");

    std::printf("== a keyframe well short of the target does not cut ==\n");
    {
        const double f = 1001.0 / 30000.0;
        CHECK(!keyframe_starts_segment(5.9, target, f),
              "an early keyframe 100 ms short is not the interval one");
        CHECK(!keyframe_starts_segment(target - 2 * f, target, f),
              "two frames short does not cut");
    }

    std::printf("== exactly one frame short, as the muxer computes it ==\n");
    {
        // Seen 2026-11-23 on a VideoToolbox H.264 encoder at 30 fps: the
        // first frame the muxer received was stamped 0.067 s, the encoder's
        // 6 s keyframe 6.033 s — 179 frames, exactly one frame short. The
        // muxer subtracts two nanosecond timestamps converted to seconds,
        // which lands 7e-10 s under target − frame, so the cut was skipped:
        // segment 0 ran 11.9 s, and every seek in that event landed one
        // segment (6 s) late, because positions map by segment number.
        const double first = (double)66666667 / 1e9;
        const double key   = (double)6033333333 / 1e9;
        CHECK(keyframe_starts_segment(key - first, target, 1.0 / 30.0),
              "a keyframe exactly one frame short (from ns timestamps) cuts");
        // VideoToolbox's interval is 179 frames on every segment, each exactly
        // one frame short, so whether a cut happens must not depend on how the
        // nanoseconds round. In the real event only the first segment fell the
        // wrong way (the later ones start on a keyframe and difference out
        // cleanly); here every keyframe is rounded independently, which the
        // old rule missed a third of the time. With the slack, none.
        int missed = 0;
        int64_t prev_ns = 66666667;
        for (int k = 1; k <= 1200; ++k) {
            const int64_t key_ns = (int64_t)llround((2.0 + 179.0 * k) * 1e9 / 30.0);
            if (!keyframe_starts_segment((double)key_ns / 1e9 - (double)prev_ns / 1e9,
                                         target, 1.0 / 30.0)) ++missed;
            prev_ns = key_ns;
        }
        std::printf("      %d of 1200 VideoToolbox-interval keyframes missed\n", missed);
        CHECK(missed == 0, "every 179-frame keyframe over two hours cuts");
        // The slack that admits it must not admit a second frame.
        CHECK(!keyframe_starts_segment(key - first - 1.0 / 30.0, target, 1.0 / 30.0),
              "two frames short still does not cut");
    }

    std::printf("== frame rate unknown: the strict rule ==\n");
    CHECK(!keyframe_starts_segment(5.9726, target, 0.0), "no tolerance without a frame rate");
    CHECK(keyframe_starts_segment(6.0, target, 0.0), "at the target still cuts");

    std::printf(g_fail ? "\nFAILED: %d\n" : "\nall ok\n", g_fail);
    return g_fail ? 1 : 0;
}
