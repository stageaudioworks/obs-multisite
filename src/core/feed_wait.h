// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// feed_wait.h — what the feed loop does with the fragment in its hand while it
// waits for playout to catch up.
//
// The feed loop takes a fragment from the session with next_segment(), which
// ADVANCES the session's head as it hands it over, then parks until its lead
// allows the push. While parked, three things can happen, and they are not the
// same thing:
//
//   * a JUMP — a seek, a jog, a stop. The head has moved somewhere else, and the
//     fragment in hand belongs to the position just left. Drop it; the next
//     next_segment() serves where we are going.
//   * a HOLD. The head has NOT moved. The fragment in hand is still exactly the
//     next thing to play — and because next_segment() already advanced past
//     it, dropping it loses it for good.
//   * nothing: wait, or push when the lead allows.
//
// These used to be one condition. `paused` sat beside the jump tests, a hold was
// treated as a jump, and the fragment was dropped — so every hold that landed
// while the feed loop was parked here (which is most of the time; the loop runs
// ahead of playout and parks until it catches up) lost exactly one segment. The
// decoder went from the end of one segment straight to the start of the one
// after next, stamping those frames ~6 s in the future; the deliver loop waited
// ~6 s for the first of them and nothing else could get out. That is #10.
//
// Pure, so tests/test_feed_wait.cpp pins it.

#include <cstdint>

namespace multisite {

enum class FeedWait {
    Push,   // the lead allows it: push the fragment now
    Wait,   // playing and ahead of playout: keep waiting
    Keep,   // HELD: keep the fragment in hand and wait — never drop it
    Drop,   // JUMPED: the fragment belongs to a position left behind
};

// `jumped` is anything that moved the head: a changed discontinuity, or a
// teardown that cleared the decoder. It outranks a hold on purpose — a seek
// made DURING a hold still moves the head, so the fragment in hand is stale.
inline FeedWait feed_wait_step(bool jumped, bool paused, bool lead_allows) {
    if (jumped) return FeedWait::Drop;
    if (paused) return FeedWait::Keep;
    return lead_allows ? FeedWait::Push : FeedWait::Wait;
}

// Where the feed's pacing clock starts once a hold ends.
//
// The lead gate compares media pushed against WALL time since the decoder
// started. A hold stops the media and not the wall, so without this every
// second held read as a second fallen behind: after a 30 s hold the gate let
// the feed run until the decoder's queue refused it, and it stayed pinned there
// — four fragments, about 24 s, ahead instead of 2.5 s — until the next seek.
// Moving the start forward by the hold makes held time not count.
//
// `pause_started_ns` of 0 means no hold was recorded; a start later than
// `now_ns` is a clock oddity. Neither moves anything.
inline uint64_t feed_start_after_hold(uint64_t feed_start_ns,
                                      uint64_t pause_started_ns,
                                      uint64_t now_ns) {
    if (pause_started_ns == 0 || pause_started_ns > now_ns) return feed_start_ns;
    return feed_start_ns + (now_ns - pause_started_ns);
}

}  // namespace multisite
