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

    std::printf("%s\n", g_fail ? "FAILED" : "all passed");
    return g_fail ? 1 : 0;
}
