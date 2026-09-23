// SPDX-License-Identifier: GPL-3.0-or-later
// test_mpp_pts.cpp — a frame at time zero keeps time zero through MPP.
//
// Found on a ROCK 5B on 2026-09-23: the first frame of a recording decoded
// through MPP came back at 0.233 s instead of 0, because the decoder read
// MPP's "no pts" (0) and the first frame's real pts (also 0) as the same thing.
#include "../src/core/mpp_pts.h"

#include <cstdio>

using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if (!(c)) { std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else std::printf("  [ok]   %s\n", m); } while (0)

// What MPP does with a pts: carries the packet's back on the frame, or 0.
static int64_t through_mpp(int64_t pts_ns, int64_t fed_last_ns) {
    return mpp_pts_from_frame(mpp_pts_to_packet(pts_ns), fed_last_ns);
}

int main() {
    const int64_t kFedLater = 233333333;   // the packet fed when frame 0 came out

    std::printf("A frame's own time survives the round trip\n");
    CHECK(through_mpp(0, kFedLater) == 0,
          "the first frame of an event is at 0, not at the packet fed last");
    CHECK(through_mpp(33333333, kFedLater) == 33333333, "the second frame is at 1/30 s");
    CHECK(through_mpp(12966666666LL, kFedLater) == 12966666666LL, "a late frame is exact");

    std::printf("A frame MPP gave no time still gets one\n");
    CHECK(mpp_pts_from_frame(0, kFedLater) == kFedLater,
          "MPP's zero means none, and the fed packet's time is used");

    std::printf(g_fail ? "mpp_pts: %d failed\n" : "mpp_pts: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
