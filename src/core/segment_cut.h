// Copyright (C) 2026 Stage Audio Works
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>

namespace multisite {

// Whether a video keyframe `elapsed_s` into the open segment should start a
// new one, for a target segment length of `target_s` at `frame_s` seconds per
// frame. `keyframe_gap_s` is how long since the previous video keyframe, or 0
// when there has not been one.
//
// THE RULE: cut at the keyframe NEAREST the target. A keyframe short of the
// target cuts when it is closer to the target than the next keyframe is
// expected to be — and the next is expected one gap later, the encoder's
// keyframe interval being steady. So it cuts once
//
//     target − elapsed  ≤  gap / 2
//
// "At or past the target" was the first rule, and OBS never puts the keyframe
// at the target. Every short keyframe was skipped and the segment ran to the
// next one, twice the length set:
//
//   • obs-x264 and the ffmpeg encoders truncate the interval to whole frames
//     (`keyint_sec * fps_num / fps_den`), so at 29.97 fps a 6 s interval is
//     179 frames, 5.973 s, 27 ms short.
//   • VideoToolbox takes the interval as a maximum and spaces keyframes 177 to
//     180 frames apart at 30 fps (5.90-6.00 s): two broadcasts on 2026-11-23
//     ran half and then nearly all of their segments to 11.8 s.
//
// "Within one frame of the target" was the second rule; it caught the first
// case and not the second, which falls two and three frames short. Half the
// gap catches both, and any encoder that lands its keyframes early by less
// than half an interval.
//
// Why half the gap is safe:
//   • It can never cut twice in one interval. A keyframe that cuts is nearer
//     the target than the one after it, so that one is past the halfway point
//     of the NEXT segment's span, not inside this one's.
//   • An encoder asked for keyframes more often than segments — 2 s keyframes,
//     6 s segments — cuts at the third, not the second: 4 s is 2 s short, more
//     than half the 2 s gap.
//   • An extra keyframe (a scene cut an encoder inserts) has a short gap, and
//     so a small tolerance: one at 3.1 s cuts nothing.
//   • The tolerance is capped at half the target, so no segment is shorter
//     than half the length set, whatever the gap says.
//
// One frame is kept as a floor, with 1 ms of slack: the muxer measures
// `elapsed_s` as the difference of two nanosecond timestamps in seconds,
// which came out 7e-10 s under target − frame for a keyframe exactly one
// frame short. With no gap known (the first keyframe of an event) and no
// frame rate, the strict rule applies.
inline bool keyframe_starts_segment(double elapsed_s, double target_s,
                                    double frame_s, double keyframe_gap_s) {
    constexpr double kSlack_s = 0.001;
    double tolerance = frame_s > 0 ? frame_s + kSlack_s : 0.0;
    if (keyframe_gap_s > 0)
        tolerance = std::max(tolerance, keyframe_gap_s / 2.0);
    tolerance = std::min(tolerance, target_s / 2.0);
    return elapsed_s >= target_s - tolerance;
}

} // namespace multisite
