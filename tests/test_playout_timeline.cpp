// SPDX-License-Identifier: GPL-3.0-or-later
// test_playout_timeline.cpp — the delivery loop's decision about a frame.
//
// Every case here is a bug that shipped. Seeking was broken five times in one
// afternoon, each time by a change that looked right in isolation, and the
// operator found all five because nothing in the suite went near this path.
//
// The frames are synthetic and there is no decoder, no network and no OBS: the
// point is the sequence of decisions, which is where all five went wrong.
//
// NOTE, 2026-09-19: this class used to hold a media clock as well — a mapping
// from pts to time of day, pinned from a fragment's recorded wall time. Most of
// the tests here were about that pairing, because getting it wrong was this
// project's most persistent fault. A position is now the frame's own pts, so
// there is nothing to pair and those tests describe machinery that no longer
// exists. They are gone rather than adapted. See BUGS #2b and #2c.
#include "../src/core/playout_timeline.h"

#include <cstdio>

using namespace multisite;
using Action = PlayoutTimeline::Action;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

static constexpr int64_t S = 1000000000LL;   // one second in ns

int main() {
    std::printf("== a frame from a timeline already left is discarded ==\n");
    {
        // b786129: the old decoder kept feeding frames across a seek, and they
        // were put to air from the position the operator had just left.
        PlayoutTimeline t;
        t.adopt(1, 1);
        CHECK(t.consider(1, 10 * S) == Action::Play, "a current frame plays");
        CHECK(t.consider(0, 10 * S) == Action::Discard,
              "a frame stamped with an older timeline is discarded");
        t.adopt(2, 2);
        CHECK(t.consider(1, 10 * S) == Action::Discard,
              "and after a seek, the previous timeline's frames are too");
        CHECK(t.consider(2, 10 * S) == Action::Play, "while the new one's play");
    }

    std::printf("== the sub-segment skip drops only what precedes the moment ==\n");
    {
        // 97598d4: the skip's per-fragment base was removed, so the skip never
        // completed and every frame was dropped — seeks stopped working.
        PlayoutTimeline t;
        t.adopt(1, 1);
        t.begin_fragment(3 * S);          // land three seconds into it
        CHECK(t.consider(1, 100 * S) == Action::DropForSkip,
              "the fragment's first frame is before the moment asked for");
        CHECK(t.consider(1, 102 * S) == Action::DropForSkip, "so is two in");
        CHECK(t.consider(1, 103 * S) == Action::Play,
              "the frame AT the moment plays");
        CHECK(t.consider(1, 104 * S) == Action::Play,
              "and the skip does not fire again for the rest of the fragment");
    }

    std::printf("== the skip measures from THIS fragment, not the last ==\n");
    {
        // The base is reset per fragment, and removing that reset once stopped
        // seeks dead: measured from an older fragment's start, every frame in
        // the new one already looked past the moment.
        PlayoutTimeline t;
        t.adopt(1, 1);
        t.begin_fragment(2 * S);
        CHECK(t.consider(1, 50 * S) == Action::DropForSkip, "first fragment: drops");
        CHECK(t.consider(1, 52 * S) == Action::Play, "arrives two seconds in");

        t.begin_fragment(2 * S);          // a second seek, a later fragment
        CHECK(t.consider(1, 500 * S) == Action::DropForSkip,
              "the next fragment's skip measures from ITS own first frame");
        CHECK(t.consider(1, 502 * S) == Action::Play, "and arrives correctly");
    }

    std::printf("== staleness is decided BEFORE the skip ==\n");
    {
        // db028a2: the check ran below the skip, so it guarded nothing. A stale
        // frame could claim the skip's base and send the whole fragment to air.
        PlayoutTimeline t;
        t.adopt(5, 5);
        t.begin_fragment(2 * S);
        CHECK(t.consider(4, 10 * S) == Action::Discard,
              "a stale frame is discarded and never reaches the skip");
        CHECK(t.consider(5, 100 * S) == Action::DropForSkip,
              "so the skip's base is claimed by a CURRENT frame");
        CHECK(t.consider(5, 102 * S) == Action::Play, "and it lands correctly");
    }

    std::printf("== two discontinuities, not one ==\n");
    {
        // A resume restarts the PLAYOUT clock — wall time moved on while media
        // time did not, so frames in flight are stale — but changes nothing
        // about the media: no seek, no new fragment. Conflating the two is what
        // made every displayed position walk backwards across a hold.
        PlayoutTimeline t;
        t.adopt(1, 1);
        t.begin_fragment(2 * S);
        CHECK(t.consider(1, 10 * S) == Action::DropForSkip, "mid-skip");

        t.adopt(2, 1);                    // a resume: playout only
        CHECK(t.consider(1, 11 * S) == Action::Discard,
              "frames from before the hold are still discarded");
        CHECK(t.consider(2, 11 * S) == Action::Play,
              "and the skip was cleared with the playout timeline, so fresh "
              "frames play rather than being dropped for a seek long finished");

        t.adopt(3, 2);                    // a seek: both
        CHECK(t.consider(2, 11 * S) == Action::Discard,
              "a seek discards the previous timeline's frames too");
    }

    std::printf("== re-adopting the same ids changes nothing ==\n");
    {
        // The loop hands these in on every frame, so adopt() has to be cheap
        // and idempotent, not a reset in disguise.
        PlayoutTimeline t;
        t.adopt(7, 7);
        t.begin_fragment(2 * S);
        CHECK(t.consider(7, 10 * S) == Action::DropForSkip, "mid-skip");
        t.adopt(7, 7);
        CHECK(t.consider(7, 11 * S) == Action::DropForSkip,
              "re-adopting the SAME ids does not restart the skip");
        CHECK(t.consider(7, 12 * S) == Action::Play, "which arrives as it should");
    }

    std::printf("\n%s\n", g_fail == 0 ? "ALL PLAYOUT TIMELINE TESTS PASSED"
                                      : "SOME TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
