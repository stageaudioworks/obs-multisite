// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// mpp_pts.h — getting a frame's time back out of Rockchip MPP.
//
// MPP carries a packet's pts through to the frame decoded from it, and hands
// back 0 when a frame has none. But 0 is also the real time of the first frame
// of every event (CONTEXT.md, "Sentinels"): read `0` as missing and that first
// frame is restamped with whatever packet was fed last, which is the decoder's
// pipeline depth later — 0.233 s on a ROCK 5B, measured 2026-09-23 by
// multisite-outpost's pipeline test playing a 30 fps recording back on the
// board. The seventh sentinel trap.
//
// So every pts goes in one nanosecond late, and comes out one nanosecond early.
// A zero from MPP then means only "none", and a real zero is 1 on the way in.
// Pure, so tests/test_mpp_pts.cpp covers it off the board.

#include <cstdint>

namespace multisite {

constexpr int64_t kMppPtsBias = 1;   // ns; far below one frame at any rate

// What to stamp on the MPP packet for a packet at `pts_ns`.
inline int64_t mpp_pts_to_packet(int64_t pts_ns) { return pts_ns + kMppPtsBias; }

// The frame's time, from what MPP handed back on it. `fallback_ns` is used only
// when MPP carried no time at all.
inline int64_t mpp_pts_from_frame(int64_t frame_pts, int64_t fallback_ns) {
    return frame_pts != 0 ? frame_pts - kMppPtsBias : fallback_ns;
}

} // namespace multisite
