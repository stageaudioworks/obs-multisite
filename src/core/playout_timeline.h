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
    // Conflating the two is what made every displayed clock walk backwards
    // across a hold: a resume re-learned the pts->wall mapping from the current
    // position's pts against a fragment wall nobody had refreshed, so the
    // origin moved by however far had been played. That mapping is gone now
    // (see the note above consider()), but the distinction still matters —
    // frames in flight are stale after a resume, and the per-fragment state is
    // not.
    void adopt(uint64_t timeline_id, uint64_t media_id) {
        if (media_id != m_media_epoch) m_media_epoch = media_id;
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

    // NOTE: this class no longer holds a media clock.
    //
    // It used to map a pts to a time of day, pinned from a fragment's recorded
    // wall time and the first pts seen. Every position an operator saw was
    // built on that pairing, and getting it wrong was this project's most
    // persistent fault — eea902c paired one fragment's wall time with another
    // fragment's pts, a resume re-pinned it against a stale fragment and walked
    // the origin 24 s backwards, and the wall times themselves were estimated
    // as seq * a NOMINAL segment length, drifting 1.11%.
    //
    // A position is now the frame's own pts: how far into the programme it
    // sits. Nothing has to be paired with anything, so none of those faults has
    // a place to live. See BUGS #2b and #2c.

    enum class Action {
        Discard,      // from a timeline already left; believe nothing about it
        DropForSkip,  // before the moment asked for; drop it, change nothing
        Play,         // good: it may define the clock and go to air
    };

    // The ordered decision. Call once per frame, and act on what it returns.
    Action consider(uint64_t frame_epoch, int64_t pts_ns) {
        // 1. Staleness, before anything reads the frame.
        if (frame_epoch != m_epoch) return Action::Discard;

        // 2. The sub-segment skip.
        if (m_skip_ns >= 0) {
            if (m_skip_base_pts == kUnset) m_skip_base_pts = pts_ns;
            if (pts_ns - m_skip_base_pts < m_skip_ns) return Action::DropForSkip;
            m_skip_ns = -1;                      // arrived
        }

        return Action::Play;
    }

private:
    static constexpr int64_t kUnset = INT64_MIN;
    uint64_t m_epoch = 0;
    uint64_t m_media_epoch = 0;
    int64_t  m_skip_ns        = -1;
    int64_t  m_skip_base_pts  = kUnset;
};

} // namespace multisite
