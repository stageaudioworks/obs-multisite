// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// position_interp.h — where the playhead reads BETWEEN state samples.
//
// A dock that polls state twice a second draws a scrub bar that visibly steps,
// which is not what a tool sitting next to a video player should look like. So
// the playhead is extrapolated from the last sample using wall time.
//
// This is its own function, and tested, because the first attempt at it shipped
// a frozen timer and an apparently dead Play button. That version clamped the
// interpolated value against an absolute end-of-recording bound, so one bad
// input pinned the readout to a constant that no amount of correct state could
// shift — while the backend was healthy and the refresh was running the whole
// time. The two properties below are what prevent that recurring, and they are
// the reason this is not written inline at the call site.
//
//  1. Extrapolation is bounded RELATIVE to the sample it came from. If state
//     refreshes stop, the readout settles a fraction of a second past the last
//     real number rather than running away — and, crucially, it can never be
//     pinned to a value derived from anything but that sample.
//  2. The absolute bound only ever caps; it cannot become the answer on its
//     own, because it is applied after the relative clamp rather than instead
//     of it.
//
#include <cstdint>

namespace multisite {

// base_ms       the last authoritative playhead
// base_wall_ms  the wall-clock time that sample was taken
// now_wall_ms   wall-clock time now
// bound_ms      never report past this (end of recording, live edge); 0 = none
// max_extrap_ms never extrapolate further than this beyond base_ms
inline long long interpolate_position(long long base_ms, long long base_wall_ms,
                                      long long now_wall_ms, long long bound_ms,
                                      long long max_extrap_ms) {
    long long elapsed = now_wall_ms - base_wall_ms;
    // A clock that steps backwards (NTP, sleep/wake) must not rewind the
    // playhead: the sample is still the best answer available.
    if (elapsed < 0) elapsed = 0;
    if (elapsed > max_extrap_ms) elapsed = max_extrap_ms;

    long long head = base_ms + elapsed;
    if (bound_ms > 0 && head > bound_ms) head = bound_ms;
    return head;
}

// The dock's playhead between samples, anchored rather than re-based on every
// sample so the bar glides instead of twitching by a sample's jitter.
//
// The anchor used to move only when the on-screen SEGMENT changed. A hold does
// not change it, so on resume the wall time since the anchor included the whole
// hold: capped at one segment, the readout jumped a segment forward, and then
// sat there until the next segment began — "elapsed only starts counting after a
// couple of seconds". While held it showed the segment's start rather than
// where the picture stopped.
//
// So the anchor moves on a new segment AND whenever the playhead starts or
// stops running; and while it is not running it simply follows the sample.
struct PlayheadAnchor {
    unsigned long long seq = 0;
    long long media_ms = 0;
    long long wall_ms  = 0;
    bool      running  = false;
};

inline void reanchor_playhead(PlayheadAnchor& a, unsigned long long seq,
                              long long sample_ms, long long sample_wall_ms,
                              bool running) {
    if (seq != a.seq || running != a.running || !running)
        a = PlayheadAnchor{seq, sample_ms, sample_wall_ms, running};
}

// seg_ms caps the advance at one segment, so a stalled refresh cannot run the
// playhead past the segment it belongs to; bound_ms (0 = none) is the end of a
// recording.
inline long long anchored_playhead(const PlayheadAnchor& a, long long now_wall_ms,
                                   long long seg_ms, long long bound_ms) {
    if (!a.running) return a.media_ms;
    long long adv = now_wall_ms - a.wall_ms;
    if (adv < 0) adv = 0;
    if (seg_ms > 0 && adv > seg_ms) adv = seg_ms;
    long long head = a.media_ms + adv;
    if (bound_ms > 0 && head > bound_ms) head = bound_ms;
    return head;
}

} // namespace multisite
