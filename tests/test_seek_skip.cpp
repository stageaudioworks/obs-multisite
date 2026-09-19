// SPDX-License-Identifier: GPL-3.0-or-later
// test_seek_skip.cpp — a sub-segment seek drops each stream's own share.
//
// This is the regression of 2026-09-19, found on real hardware and not by the
// suite, because moving the skip from the delivery loop to the producer also
// moved it out of test_playout_timeline's reach. The operator's report was
// "audio sync seems to drift"; the log showed audio's first pts running
// 300-319 ms ahead of video's after every re-anchor, and the picture running
// that far behind the sound by a different amount after each seek.
//
// The frames here are synthetic. The point is the interleave: audio speaks
// first and speaks more often, which is the only fact the old shared base got
// wrong.
#include "../src/core/seek_skip.h"

#include <cstdio>

using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

static constexpr int64_t MS = 1000000LL;

int main() {
    std::printf("== not armed: nothing is dropped ==\n");
    {
        SeekSkip s;
        CHECK(!s.armed(), "a fresh skip is not armed");
        CHECK(!s.drops(0, true),  "video passes");
        CHECK(!s.drops(0, false), "audio passes");
    }

    std::printf("== each stream measures from its OWN first frame ==\n");
    {
        // The shape from the log: the fragment starts at 469.355 s, audio is
        // handed over first and video's first frame is 311 ms later. A seek
        // asks for 2 s into the fragment.
        const int64_t a0 = 469355 * MS;
        const int64_t v0 = a0 + 311 * MS;
        SeekSkip s;
        s.arm(2000 * MS);

        CHECK(s.drops(a0, false), "the fragment's first audio frame is dropped");
        CHECK(s.drops(v0, true),  "the fragment's first video frame is dropped");
        CHECK(s.base(false) == a0, "the audio arm measures from audio");
        CHECK(s.base(true)  == v0, "the video arm measures from video");

        // Just short of the target in each stream's own terms.
        CHECK(s.drops(a0 + 1999 * MS, false), "audio 1.999s in is still dropped");
        CHECK(s.drops(v0 + 1999 * MS, true),  "video 1.999s in is still dropped");

        // And the moment itself.
        CHECK(!s.drops(a0 + 2000 * MS, false), "audio 2.000s in plays");
        CHECK(!s.drops(v0 + 2000 * MS, true),  "video 2.000s in plays");
    }

    std::printf("== one stream arriving does not disarm the other ==\n");
    {
        // THE BUG. With a single shared base claimed by audio, this video
        // frame — only 1.7 s into the picture — measured 2.011 s from the
        // audio origin, crossed the target, and went to air. 311 ms of picture
        // the audio had already discarded.
        const int64_t a0 = 469355 * MS;
        const int64_t v0 = a0 + 311 * MS;
        SeekSkip s;
        s.arm(2000 * MS);
        s.drops(a0, false);                       // audio claims its own base
        s.drops(v0, true);                        // video claims its own base

        CHECK(!s.drops(a0 + 2000 * MS, false), "audio reaches the moment first");
        CHECK(s.arrived(false),  "the audio arm has landed");
        CHECK(!s.arrived(true),  "the video arm has NOT landed");
        CHECK(s.drops(v0 + 1700 * MS, true),
              "video 1.7s in is STILL dropped after audio has landed");
        CHECK(!s.drops(v0 + 2000 * MS, true), "video lands at its own 2.000s");
        CHECK(s.arrived(true), "the video arm has landed");
    }

    std::printf("== a stream that has landed keeps playing ==\n");
    {
        SeekSkip s;
        s.arm(1000 * MS);
        s.drops(0, true);
        CHECK(!s.drops(1000 * MS, true), "video lands");
        CHECK(!s.drops(1020 * MS, true), "and the next frame plays");
        CHECK(!s.drops(0, true),
              "even a frame behind the target: the arm is spent, not re-armed");
    }

    std::printf("== both landed disarms the skip entirely ==\n");
    {
        SeekSkip s;
        s.arm(1000 * MS);
        s.drops(0, false);
        s.drops(0, true);
        s.drops(1000 * MS, false);
        CHECK(s.armed(), "still armed while video is outstanding");
        s.drops(1000 * MS, true);
        CHECK(!s.armed(), "disarmed once both streams have landed");
    }

    std::printf("== re-arming forgets the last seek ==\n");
    {
        SeekSkip s;
        s.arm(1000 * MS);
        s.drops(0, true);
        s.drops(1000 * MS, true);
        CHECK(s.arrived(true), "landed on the first seek");
        s.arm(500 * MS);
        CHECK(!s.arrived(true), "the arm is clear again");
        CHECK(s.base(true) == SeekSkip::kUnsetPts, "and so is the base");
        CHECK(s.drops(9000 * MS, true), "the next fragment starts over");
        CHECK(s.base(true) == 9000 * MS, "measuring from ITS first frame");
    }

    std::printf("== a seek to the very start of a fragment drops nothing ==\n");
    {
        // skip_to_ms is only armed when > 0, but 0 must be harmless: the
        // 0-sentinel trap has caught this project more than once.
        SeekSkip s;
        s.arm(0);
        CHECK(!s.drops(5000 * MS, true),  "the first video frame plays");
        CHECK(!s.drops(5000 * MS, false), "the first audio frame plays");
    }

    std::printf("%s\n", g_fail ? "FAILED" : "all passed");
    return g_fail ? 1 : 0;
}
