// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// playout_timeline.h — what a decoded frame is allowed to do, and in what order.
//
// Between popping a frame and putting it to air, the delivery loop asks three
// questions about it, and the ORDER is the whole point:
//
//   1. Does this frame still belong to the timeline we are on? A seek may have
//      happened while it was in hand.
//   2. Is it before the moment a sub-segment seek asked for? If so it is
//      dropped, and it must not be used for anything else either.
//   3. Only then: it defines the media clock, the displayed position, and the
//      picture.
//
// Those lived as three separate blocks in the loop, and every bug in this area
// came from their order or their shared state rather than from their logic:
//
//   b786129  the old decoder kept feeding frames across a seek
//   97598d4  the skip's per-fragment base was removed, so the skip never
//            completed and every frame was dropped — seeks stopped working
//   eea902c  the media-clock pin shared the skip's base, so it paired one
//            fragment's wall time with another fragment's pts
//   281cf6e  frames were stamped with a timeline epoch…
//   db028a2  …but the check ran below the pin, so it guarded nothing
//
// Each of those was a correct-looking change to one block. Putting the order
// inside one tested function is what stops the next one: a caller cannot get
// the sequence wrong because it no longer chooses the sequence.
//
// STATUS: this is the specification, extracted from deliver_loop() and pinned
// by test_playout_timeline. deliver_loop does NOT yet call it — the state it
// replaces is spread across six atomics written by four threads, and rewiring
// that is a concurrency change to code that took five attempts to get working
// and can only be verified by running a real event. Until that is done, the two
// can drift, which is the known cost of shipping it this way rather than a
// thing to discover later. Wiring it is the next change here; a diff of the
// rules below against deliver_loop is how to check they still agree.
//
// Deliberately free of OBS and FFmpeg so it can be driven directly by a test.
//
#include <cstdint>

namespace multisite {

class PlayoutTimeline {
public:
    static constexpr int64_t kNoClock = INT64_MIN;

    // A seek, a jump, or a decoder restart. Frames already in flight belong to
    // the position being left and must not be believed about anything, AND the
    // media clock has to be learned again because a new fragment is coming.
    void restart() { adopt(m_epoch + 1, m_media_epoch + 1); }

    // Adopt a timeline id maintained outside this object. The delivery loop
    // owns a PlayoutTimeline but the seek that invalidates it happens on
    // another thread, so the id is an atomic the loop reads and hands here; a
    // value it has not seen before IS the restart. Keeping the counter outside
    // is what lets this object stay lock-free and single-threaded while still
    // reacting to a seek made anywhere.
    void adopt(uint64_t timeline_id) { adopt(timeline_id, timeline_id); }

    // TWO discontinuities, and they are not the same thing — conflating them is
    // what made every displayed clock walk backwards across a hold.
    //
    //   timeline_id  the PLAYOUT clock restarts: frames in flight are from
    //                before and must be discarded. A resume does this, because
    //                wall time moved on while media time did not.
    //   media_id     the MEDIA timeline restarts: a new fragment is coming, so
    //                the pts->wall mapping must be learned again. A seek, a
    //                jump or a decoder restart does this. A RESUME DOES NOT:
    //                nothing about the media changed, and no new fragment is
    //                fed.
    //
    // When a resume cleared the pin, the mapping was rebuilt from the current
    // position's pts against `restart_wall_ms` — which still held the wall time
    // of the fragment fed at the LAST decoder restart, because a resume feeds
    // none. The pair described two different fragments, so the origin moved by
    // however far had been played since:
    //
    //   fragment wall 1789455501964, first pts 505.533 -> origin 1789454996431
    //   fragment wall 1789455501964, first pts 510.133 -> origin 1789454991831
    //   fragment wall 1789455501964, first pts 516.633 -> origin 1789454985331
    //
    // Same wall, advancing pts, origin walking back by exactly the pts advance.
    void adopt(uint64_t timeline_id, uint64_t media_id) {
        if (media_id != m_media_epoch) {
            m_media_epoch     = media_id;
            m_pin_base_pts    = kUnset;
            m_restart_wall_ms = 0;
            m_offset_ms       = kNoClock;
        }
        if (timeline_id != m_epoch) {
            m_epoch          = timeline_id;
            m_skip_base_pts  = kUnset;
            m_skip_ns        = -1;
        }
    }

    // Stamp for frames leaving the queue now. Compare with what comes back.
    uint64_t epoch() const { return m_epoch; }

    // A fragment has been handed to the decoder. `skip_ns` is how far into it
    // the seek asked to land, or negative for none.
    //
    // The skip's base is reset PER FRAGMENT: it measures from the start of the
    // fragment it was armed for, which is what bounds how far one skip can run.
    // Removing this reset once stopped seeks dead.
    void begin_fragment(int64_t skip_ns) {
        m_skip_base_pts = kUnset;
        if (skip_ns > 0) m_skip_ns = skip_ns;
    }

    // The event's start on the wall clock, which IS the media clock's origin.
    //
    // Preferred over pinning a fragment, and it makes the pin's whole class of
    // bug impossible. The encoder writes each segment's at_ms as
    // `event_start + pts_offset`, so `origin = at_ms - pts` is always just
    // event_start — there is nothing a fragment can tell us that this does not
    // say more directly, and without needing the right fragment to be paired
    // with the right pts. The decoder's fallback used to derive the same
    // quantity as `event_start + seq * nominal_duration`, a second formula that
    // agreed only while every segment was exactly nominal; measured at 67 ms
    // per 6 s segment out, 1.11%, which put a cue about forty seconds wrong by
    // the end of an hour. See BUGS #2b.
    //
    // Set once per media timeline. Later calls are ignored, the same as the
    // fragment pin, so a segment arriving mid-playback cannot move the clock.
    void set_event_start_ms(int64_t wall_ms) {
        if (wall_ms > 0 && m_offset_ms == kNoClock) m_offset_ms = wall_ms;
    }

    // Wall-clock start of the first fragment after a restart. The pin's base is
    // NOT reset here — it is reset only in restart(), alongside this — so the
    // wall time and the pts it is paired with always describe the same fragment.
    void set_restart_wall_ms(int64_t wall_ms) {
        if (m_restart_wall_ms == 0) m_restart_wall_ms = wall_ms;
    }

    enum class Action {
        Discard,      // from a timeline already left; believe nothing about it
        DropForSkip,  // before the moment asked for; drop it, change nothing
        Play,         // good: it may define the clock and go to air
    };

    // The ordered decision. Call once per frame, and act on what it returns.
    Action consider(uint64_t frame_epoch, int64_t pts_ns) {
        // 1. Staleness, before anything reads the frame.
        if (frame_epoch != m_epoch) return Action::Discard;

        // 2. Claim the pin's base — on the FIRST frame of the fragment, before
        // the skip can drop it.
        //
        // It pairs with restart_wall_ms, which is that fragment's START, so it
        // has to be the pts of the fragment's start too. Claiming it after the
        // skip instead paired the fragment's start time with a pts up to a
        // whole segment later, and every displayed time then read that much
        // early. Measured: a seek asking for ...649732 landed correctly and
        // reported ...647789, 1943ms early — exactly the skip.
        //
        // Still after the staleness check, never before it: a frame from a
        // position already left must not define this.
        if (m_pin_base_pts == kUnset) m_pin_base_pts = pts_ns;

        // 3. The sub-segment skip.
        if (m_skip_ns >= 0) {
            if (m_skip_base_pts == kUnset) m_skip_base_pts = pts_ns;
            if (pts_ns - m_skip_base_pts < m_skip_ns) return Action::DropForSkip;
            m_skip_ns = -1;                      // arrived
        }

        // 4. Pin the media clock, once — only if the event's own start was not
        // available. Kept as a fallback for a session that cannot report one.
        if (m_offset_ms == kNoClock && m_restart_wall_ms > 0)
            m_offset_ms = m_restart_wall_ms - m_pin_base_pts / 1000000;
        return Action::Play;
    }

    bool    have_clock()   const { return m_offset_ms != kNoClock; }
    int64_t clock_offset_ms() const { return m_offset_ms; }
    // Wall-clock time of a frame, once the clock is pinned.
    int64_t wall_ms_for(int64_t pts_ns) const {
        return m_offset_ms == kNoClock ? 0 : m_offset_ms + pts_ns / 1000000;
    }
    // What the pin was built from, for the log line that made these bugs
    // visible in the first place.
    int64_t pin_wall_ms()    const { return m_restart_wall_ms; }
    int64_t pin_base_pts_ns() const { return m_pin_base_pts; }

private:
    static constexpr int64_t kUnset = INT64_MIN;
    uint64_t m_epoch = 0;
    uint64_t m_media_epoch = 0;
    int64_t  m_skip_ns        = -1;
    int64_t  m_skip_base_pts  = kUnset;
    int64_t  m_pin_base_pts   = kUnset;
    int64_t  m_restart_wall_ms = 0;
    int64_t  m_offset_ms      = kNoClock;
};

} // namespace multisite
