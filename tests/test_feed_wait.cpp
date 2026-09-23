// SPDX-License-Identifier: GPL-3.0-or-later
// test_feed_wait.cpp — a hold keeps the fragment; only a jump drops it (#10).
#include "../src/core/feed_wait.h"

#include <cstdio>

using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

int main() {
    std::printf("== a hold is not a jump ==\n");
    {
        // THE BUG. Holding while the feed loop waited dropped the fragment it
        // had already taken from the session, and a whole segment vanished.
        CHECK(feed_wait_step(false, true, false) != FeedWait::Drop,
              "a hold never drops the fragment in hand");
        CHECK(feed_wait_step(false, true, false) == FeedWait::Keep,
              "a hold keeps it, and keeps waiting");
        CHECK(feed_wait_step(false, true, true) == FeedWait::Keep,
              "even when the lead would allow a push, nothing is pushed while held");
    }

    std::printf("== a jump drops it, held or not ==\n");
    {
        CHECK(feed_wait_step(true, false, false) == FeedWait::Drop,
              "a seek while playing drops the stale fragment");
        CHECK(feed_wait_step(true, true, false) == FeedWait::Drop,
              "a seek made DURING a hold still moves the head, so it still drops");
        CHECK(feed_wait_step(true, false, true) == FeedWait::Drop,
              "a jump outranks a lead that would allow the push");
    }

    std::printf("== playing, not jumped ==\n");
    {
        CHECK(feed_wait_step(false, false, true) == FeedWait::Push,
              "pushes when the lead allows");
        CHECK(feed_wait_step(false, false, false) == FeedWait::Wait,
              "waits while ahead of playout");
    }

    std::printf("== held time does not count as falling behind ==\n");
    {
        const uint64_t s = 1000000000ULL;          // 1 s in ns
        const uint64_t lead = 2500000000ULL;       // kFeedLeadNs
        // Started at t=100 s, 10 s of media pushed, held from t=108 s to 138 s.
        const uint64_t start = 100 * s, pushed = 10 * s;
        const uint64_t paused_at = 108 * s, now = 138 * s;
        const uint64_t moved = feed_start_after_hold(start, paused_at, now);
        CHECK(moved == 130 * s, "the start moves forward by exactly the hold");
        CHECK(now - moved == paused_at - start,
              "elapsed after the hold equals elapsed when it began");
        // Unshifted, the gate would allow 38 s + 2.5 s of media: 30 s too much.
        CHECK(!(pushed + 20 * s <= (now - moved) + lead),
              "20 s more media is not allowed straight after a 30 s hold");
        CHECK(pushed + 20 * s <= (now - start) + lead,
              "(which the unshifted clock would have allowed — the bug)");
        CHECK(feed_start_after_hold(start, 0, now) == start,
              "no recorded hold moves nothing");
        CHECK(feed_start_after_hold(start, now + s, now) == start,
              "a hold 'starting' in the future moves nothing");
    }

    std::printf("%s\n", g_fail ? "FAILED" : "all passed");
    return g_fail ? 1 : 0;
}
