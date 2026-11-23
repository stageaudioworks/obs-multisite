// Copyright (C) 2026 Stage Audio Works
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace multisite {

// Whether a video keyframe `elapsed_s` into the open segment should start a
// new one, for a target segment length of `target_s` at `frame_s` seconds per
// frame.
//
// "At or past the target" is not the rule, because OBS never puts the
// keyframe at the target. Its encoders turn the whole-second keyframe interval
// into frames by truncating — `keyint_sec * fps_num / fps_den` in obs-x264 and
// in the ffmpeg-based encoders — so at 29.97 fps a 6 s interval is 179 frames,
// 5.973 s. That keyframe falls 27 ms short of the target, was skipped, and
// every segment ran to the next one: ~11.95 s, twice what the operator set.
//
// Truncation loses less than one frame, so a keyframe within one frame of the
// target is the one the interval meant. At integer rates the keyframe lands
// exactly on the target and nothing changes.
//
// "Within one frame" includes EXACTLY one frame, and that boundary needs a
// little slack. The muxer measures `elapsed_s` as the difference of two
// nanosecond timestamps converted to seconds, which can come out a hair under
// target − frame: a first frame stamped 0.067 s and a keyframe 179 frames
// later at 6.033 s measured 7e-10 s short, the cut was skipped, segment 0 ran
// 11.9 s, and every seek in that event landed one segment late (2026-11-23,
// VideoToolbox H.264 at 30 fps). One millisecond is far below a frame at any
// rate the plugin allows, so it admits that keyframe and never a second one.
//
// `frame_s <= 0` (frame rate unknown) keeps the strict rule.
inline bool keyframe_starts_segment(double elapsed_s, double target_s,
                                    double frame_s) {
    constexpr double kSlack_s = 0.001;
    double tolerance = frame_s > 0 ? frame_s + kSlack_s : 0.0;
    return elapsed_s >= target_s - tolerance;
}

} // namespace multisite
