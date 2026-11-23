// SPDX-License-Identifier: GPL-3.0-or-later
// test_segment_timeline.cpp — where each segment starts, and which segment
// holds a moment, when segments are not all one length (segment_timeline.h).
//
// The starts in §2 are the ones measured on 2026-11-23 from a VideoToolbox
// broadcast with a 6 s target: keyframes 177-180 frames apart left a mix of
// 6 s and 12 s segments, and `media_ms / length` put seeks 6 s late per long
// segment before them.
#include "../src/core/segment_timeline.h"

#include <cstdint>
#include <cstdio>

using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

int main() {
    std::printf("== 1. Nothing recorded: the old arithmetic, unchanged ==\n");
    {
        SegmentTimeline t;
        CHECK(t.start_of(0, 6000) == 0 && t.start_of(7, 6000) == 42000,
              "seq * average, as before — an event older than the record");
        CHECK(t.seq_at(0, 6000, 100) == 0 && t.seq_at(41999, 6000, 100) == 6 &&
              t.seq_at(42000, 6000, 100) == 7, "and its inverse");
        CHECK(t.seq_at(10000000, 6000, 100) == 100, "clamped to the last segment");
        CHECK(!t.exact(3), "and nothing claims to be exact");
    }

    std::printf("== 2. 6 s and 12 s segments, every start recorded ==\n");
    {
        // Video starts of the first eight segments, 2026-11-23.
        const int64_t starts[] = { 67, 11933, 23733, 29700, 35667, 41667, 53533, 65367 };
        const int64_t avg = (65367 - 67) / 7;     // what "measured" would say
        SegmentTimeline t;
        for (uint64_t s = 0; s < 8; ++s) t.note(s, starts[s]);
        bool all = true;
        for (uint64_t s = 0; s < 8; ++s) all = all && t.start_of(s, avg) == starts[s];
        CHECK(all, "every start is the recorded one");
        CHECK(t.seq_at(30000, avg, 7) == 3, "30 s is in segment 3 (29.7-35.7 s)");
        CHECK(t.seq_at(50000, avg, 7) == 5, "50 s is in segment 5 (41.7-53.5 s)");
        CHECK(t.seq_at(70000, avg, 7) == 7, "70 s is in segment 7");
        CHECK(t.seq_at(23733, avg, 7) == 2 && t.seq_at(23732, avg, 7) == 1,
              "a segment's own start is in it; a millisecond before is not");
        CHECK(t.seq_at(0, avg, 7) == 0, "0 ms is segment 0, before its first frame or not");
        // What the old division picked, with the nominal 6 s — the fault.
        CHECK(50000 / 6000 == 8 && 30000 / 6000 == 5,
              "(the division put 50 s in segment 8 and 30 s in segment 5: wrong)");
    }

    std::printf("== 3. A recorded length places the next segment ==\n");
    {
        SegmentTimeline t;
        t.note(0, 0, 11867);
        CHECK(t.exact(0) && t.start_of(0, 6000) == 0,
              "segment 0 starts at 0 ms — a real value, not a missing one");
        CHECK(t.exact(1) && t.start_of(1, 6000) == 11867,
              "segment 1 starts where segment 0 ended");
        CHECK(!t.exact(2), "but two on is not implied");
    }

    std::printf("== 4. Between recorded starts: estimates never overtake ==\n");
    {
        SegmentTimeline t;
        t.note(0, 0);
        t.note(10, 90000);              // the ten between ran long on average
        bool rising = true;
        for (uint64_t s = 1; s <= 10; ++s)
            rising = rising && t.start_of(s, 6000) > t.start_of(s - 1, 6000);
        CHECK(rising, "starts rise all the way to the next recorded one");
        CHECK(t.start_of(5, 6000) == 45000, "spread evenly over the span");
        CHECK(t.start_of(9, 6000) < 90000, "and the last estimate is before it");
    }

    std::printf("== 5. Only later starts recorded (the manifest's window) ==\n");
    {
        SegmentTimeline t;
        t.note(100, 610000, 6000);
        t.note(101, 616000, 6000);
        CHECK(t.start_of(98, 6000) == 598000, "earlier ones count back by the average");
        CHECK(t.start_of(0, 9000) == 0, "and never before the start of the event");
        CHECK(t.start_of(102, 6000) == 622000, "the one after the last is implied");
        CHECK(t.start_of(104, 6000) == 634000, "and beyond that, the average");
    }

    std::printf("== 6. clear() forgets an event ==\n");
    {
        SegmentTimeline t;
        t.note(3, 99999);
        t.clear();
        CHECK(t.known() == 0 && t.start_of(3, 6000) == 18000, "back to the arithmetic");
        t.note(2, -1);
        CHECK(t.known() == 0, "a start of -1 is absent, and is not recorded");
    }

    std::printf("\n%s (%d failure(s))\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
