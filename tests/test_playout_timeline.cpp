// SPDX-License-Identifier: GPL-3.0-or-later
// test_playout_timeline.cpp — driving the delivery loop's decision across a seek.
//
// Every case here is a bug that shipped. Seeking was broken five times in one
// afternoon, each time by a change that looked right in isolation, and the
// operator found all five because nothing in the suite went near this path.
//
// The frames are synthetic and there is no decoder, no network and no OBS: the
// point is the sequence of decisions, which is where all five went wrong.
#include "../src/core/playout_timeline.h"

#include <cstdio>

using namespace multisite;
using Action = PlayoutTimeline::Action;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

static constexpr int64_t S  = 1000000000LL;   // a second, in ns
static constexpr int64_t FR = S / 30;         // a frame at 30fps

int main() {
    std::printf("== a frame in hand across a seek is not believed ==\n");
    {
        // 281cf6e / db028a2. The delivery loop pops a frame and then waits up
        // to 400ms for it to fall due. A seek in that window leaves it holding
        // a frame from the position just left.
        PlayoutTimeline t;
        t.set_restart_wall_ms(1789127372572);
        const uint64_t in_hand = t.epoch();          // popped…
        t.restart();                                 // …operator seeks…
        t.set_restart_wall_ms(1789130908856);
        // …and only now does the held frame come due. Its pts is from the old
        // position: the exact pair seen in the field, 7033.567s vs 3576.003s.
        CHECK(t.consider(in_hand, (int64_t)(7033.567 * S)) == Action::Discard,
              "the stale frame is discarded");
        CHECK(!t.have_clock(),
              "and it did NOT pin the media clock on its way past");

        // The real first frame of the new position defines the clock.
        CHECK(t.consider(t.epoch(), (int64_t)(3576.003 * S)) == Action::Play,
              "the frame from the new position plays");
        CHECK(t.pin_base_pts_ns() == (int64_t)(3576.003 * S),
              "and the clock is pinned to ITS pts, not the stale one");
        CHECK(t.pin_wall_ms() == 1789130908856,
              "paired with the wall time of the fragment it came from");
    }

    std::printf("== the pin does not share the skip's base ==\n");
    {
        // eea902c. The pin used seg_first_pts_ns, which the feed loop resets on
        // every fragment while running seconds ahead of delivery — so the pin
        // matched fragment N's wall time to fragment N+k's pts. In the field:
        // anchored on 1290.008s, pinned on 1293.333s, 3.3s of error in every
        // displayed time, learned once and sticky.
        PlayoutTimeline t;
        t.restart();
        t.begin_fragment(-1);
        t.set_restart_wall_ms(1789128648256);
        const int64_t first = (int64_t)(1290.008 * S);
        CHECK(t.consider(t.epoch(), first) == Action::Play, "first frame plays");
        // The feed loop now races ahead and starts several more fragments.
        t.begin_fragment(-1);
        t.begin_fragment(-1);
        t.consider(t.epoch(), (int64_t)(1293.333 * S));
        CHECK(t.pin_base_pts_ns() == first,
              "later fragments do not move the pin's base");
        CHECK(t.clock_offset_ms() == 1789128648256 - 1290008,
              "so the clock offset is the one the first frame implied");
    }

    std::printf("== a sub-segment skip completes ==\n");
    {
        // 97598d4. The skip's base was made per-seek instead of per-fragment,
        // so it never moved, the target was never reached, and EVERY frame was
        // dropped: frames_out froze and seeks stopped working entirely.
        PlayoutTimeline t;
        t.restart();
        t.begin_fragment(3 * S);                 // land 3s into this fragment
        t.set_restart_wall_ms(1789130054456);

        const int64_t frag0 = 500 * S;
        int dropped = 0, played = 0;
        for (int i = 0; i < 180; ++i) {          // one 6s fragment at 30fps
            const Action a = t.consider(t.epoch(), frag0 + i * FR);
            if (a == Action::DropForSkip) ++dropped; else if (a == Action::Play) ++played;
        }
        CHECK(played > 0, "the skip completes and frames play");
        // 91, not 90: a frame is 33333333ns, so 90 of them is 2.99999997s —
        // still short of the 3s asked for. The skip lands on the first frame at
        // or past the target, never before it, which is the right way round for
        // a cue: showing a frame early shows something the operator cut away
        // from.
        CHECK(dropped == 91, "every frame before the target is dropped");
        CHECK(frag0 + (int64_t)dropped * FR - frag0 >= 3 * S,
              "and the first frame played is at or past the target");
        CHECK(t.consider(t.epoch(), frag0 + 179 * FR) == Action::Play,
              "and it stays completed for the rest of the fragment");
    }

    std::printf("== a seek reports the moment asked for, not the fragment ==\n");
    {
        // restart_wall_ms is the START of the landing fragment, so the pts it
        // pairs with must be that fragment's start too — even though the first
        // frames are dropped to land part-way in. Pairing it with the first
        // frame that SURVIVED the skip reported every time up to a whole
        // segment early. From the field: asked for ...649732, landed correctly,
        // and reported ...647789 — 1943ms early, exactly the skip.
        PlayoutTimeline t;
        t.restart();
        const int64_t frag_wall = 1789130647789;   // start of the fragment
        const int64_t skip      = 1943 * 1000000LL;
        const int64_t frag0     = (int64_t)(3312.003 * S);
        t.begin_fragment(skip);
        t.set_restart_wall_ms(frag_wall);

        CHECK(t.consider(t.epoch(), frag0) == Action::DropForSkip,
              "the fragment's first frame is dropped to reach the target");
        CHECK(!t.have_clock(), "a dropped frame does not pin the clock");

        // Play forward until the skip is satisfied.
        int64_t played = 0;
        for (int i = 1; i < 200 && played == 0; ++i) {
            const int64_t pts = frag0 + i * FR;
            if (t.consider(t.epoch(), pts) == Action::Play) played = pts;
        }
        CHECK(played != 0, "the target is reached");
        CHECK(t.pin_base_pts_ns() == frag0,
              "the clock is pinned to the FRAGMENT's start, not the frame shown");
        // The moment reported for the frame on air is the moment asked for.
        const int64_t reported = t.wall_ms_for(played);
        const int64_t asked    = frag_wall + skip / 1000000;
        CHECK(reported >= asked && reported - asked < 40,
              "so the reported time is the requested one, within a frame");
    }

    std::printf("== the skip re-bases per fragment ==\n");
    {
        // Why the per-fragment reset is load-bearing: a skip armed for a later
        // fragment measures from THAT fragment's start, not from wherever the
        // previous one happened to begin.
        PlayoutTimeline t;
        t.restart();
        t.begin_fragment(-1);
        t.set_restart_wall_ms(1000000);
        t.consider(t.epoch(), 100 * S);          // fragment A plays
        t.begin_fragment(1 * S);                 // fragment B, skip 1s in
        CHECK(t.consider(t.epoch(), 106 * S) == Action::DropForSkip,
              "B's first frame is measured from B, not from A");
        CHECK(t.consider(t.epoch(), 107 * S) == Action::Play,
              "and one second into B it plays");
    }

    std::printf("== a restart clears the clock, so it is learned again ==\n");
    {
        PlayoutTimeline t;
        t.restart();
        t.set_restart_wall_ms(5000000);
        t.consider(t.epoch(), 10 * S);
        CHECK(t.have_clock(), "clock pinned");
        CHECK(t.wall_ms_for(11 * S) == 5000000 + 1000,
              "a frame a second later reads a second later");
        t.restart();
        CHECK(!t.have_clock(), "a seek unpins it rather than carrying it over");
    }

    std::printf("== adopting an id from another thread is the restart ==\n");
    {
        // The delivery loop owns the timeline but a seek happens on the UI
        // thread, so the id lives in an atomic and is handed in each pass.
        PlayoutTimeline t;
        t.adopt(7);
        t.set_restart_wall_ms(1000000);
        CHECK(t.consider(7, 10 * S) == Action::Play, "a frame on id 7 plays");
        CHECK(t.have_clock(), "and pins the clock");
        CHECK(t.consider(6, 10 * S) == Action::Discard,
              "a frame stamped with the previous id is discarded");
        t.adopt(7);
        CHECK(t.have_clock(), "re-adopting the SAME id is not a restart");
        t.adopt(8);
        CHECK(!t.have_clock(), "a new id is");
        CHECK(t.consider(7, 10 * S) == Action::Discard,
              "and frames from the old id no longer play");
    }

    std::printf("== a resume re-anchors playout WITHOUT re-pinning the clock ==\n");
    {
        // A hold restarts the PLAYOUT clock — wall time moved on while media
        // time did not — but changes nothing about the media: no seek, no new
        // fragment. Clearing the pin here is what made every displayed clock
        // walk backwards, because the mapping was then rebuilt from the current
        // position's pts against the wall time of the fragment fed at the LAST
        // decoder restart. Two different fragments, one pairing.
        PlayoutTimeline t;
        t.adopt(1, 1);
        t.set_restart_wall_ms(1000000);          // this fragment starts here
        CHECK(t.consider(1, 10 * S) == Action::Play, "the first frame plays");
        CHECK(t.have_clock(), "and pins the clock");
        const int64_t origin = t.clock_offset_ms();

        // Play on, then hold and resume: playout epoch moves, media does not.
        CHECK(t.consider(1, 40 * S) == Action::Play, "playback continues");
        t.adopt(2, 1);
        CHECK(t.have_clock(),
              "a resume keeps the media clock — nothing about the media moved");
        CHECK(t.clock_offset_ms() == origin,
              "and the origin does NOT move, which is the whole bug");
        CHECK(t.consider(1, 50 * S) == Action::Discard,
              "while frames from before the hold are still discarded");
        CHECK(t.consider(2, 50 * S) == Action::Play, "and fresh ones play");
        CHECK(t.clock_offset_ms() == origin,
              "the origin still has not moved after the resume's first frame");

        // A seek DOES restart the media timeline.
        t.adopt(3, 2);
        CHECK(!t.have_clock(), "but a seek unpins it: a new fragment is coming");
    }

    std::printf("\n%s\n", g_fail == 0 ? "ALL PLAYOUT TIMELINE TESTS PASSED"
                                      : "SOME TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
