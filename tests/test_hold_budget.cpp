// SPDX-License-Identifier: GPL-3.0-or-later
// test_hold_budget.cpp — a start that waits for the codec config is not cut
// short by its own audio.
//
// The case from 2026-10-03: VideoToolbox HEVC gives its config with the first
// keyframe, the start then spends ~5 s writing to the bucket, and 30 fps video
// plus three 48 kHz AAC tracks is held meanwhile. Counting every packet against
// a limit meant for video stopped the output at 5.3 s.
#include "../src/core/hold_budget.h"

#include <cstdio>

using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

// `seconds` of 30 fps video (20 kB a frame) and `tracks` AAC tracks (1024
// samples a packet at 48 kHz, ~400 bytes), as OBS delivers them.
static void hold(HoldBudget& b, double seconds, int tracks) {
    const int video = (int)(seconds * 30);
    const int audio = (int)(seconds * 48000.0 / 1024.0);
    for (int i = 0; i < video; ++i) b.add(true, 20000);
    for (int t = 0; t < tracks; ++t)
        for (int i = 0; i < audio; ++i) b.add(false, 400);
}

int main() {
    {
        HoldBudget b;
        hold(b, 10, 3);
        CHECK(!b.exceeded(10000), "10 s held with three audio tracks is within the budget");
    }
    {
        HoldBudget b;
        hold(b, 5.4, 3);
        CHECK(!b.exceeded(5400), "the 5.4 s that stopped the output on 2026-10-03 is within it");
    }
    {
        HoldBudget b;
        hold(b, 31, 0);
        CHECK(b.exceeded(14000), "more than 900 video packets is not");
    }
    {
        HoldBudget b;
        hold(b, 2, 1);
        CHECK(b.exceeded(15001), "nor is more than 15 s, however little is held");
    }
    {
        HoldBudget b;
        for (int i = 0; i < 70; ++i) b.add(true, 1u << 20);   // 70 MB of video
        CHECK(b.exceeded(3000), "nor more than 64 MB");
        b.reset();
        CHECK(!b.exceeded(0), "and reset starts again from nothing");
    }
    std::printf(g_fail ? "FAILED: %d\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
