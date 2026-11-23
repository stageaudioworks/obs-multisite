// Copyright (C) 2026 Stage Audio Works
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// segment_timeline.h — where each segment STARTS, in media time, for the whole
// event. The one answer to "which segment holds this moment?" and "what moment
// does this segment begin at?".
//
// Positions used to be `seq * segment length`, with the length measured as an
// average (BUGS #2b). That is exact only while every segment is the same
// length, and they are not: the muxer cuts at the encoder's keyframes, and
// VideoToolbox spaces those 177-180 frames apart, so a 6 s target produced a
// mix of 6 s and 12 s segments (2026-11-23, four of the first eight). One
// 12 s segment put every later position — the playhead, the live edge, and
// which segment a seek landed in — 6 s out.
//
// The encoder knows each segment's start (the muxer's pts offset). It says so
// in two places a decoder can read:
//   • the manifest, for its rolling window of recent segments (at_ms − event
//     start, and duration_s);
//   • the group index, groups/{g}.json, for every segment of the event
//     (GroupMember::start_ms, parity.h).
// The decoder notes every start it learns here. A segment with no recorded
// start is placed from its nearest recorded neighbours; with none at all,
// from the average — exactly the old arithmetic, so an event recorded before
// start_ms existed behaves as it always did.
//
// Media time starts at 0, which is a real value: segment 0 starts at 0 ms.
// Absent is -1 (CONTEXT.md, Sentinels).
//
// Thread-safe; every call takes one short lock of its own and nothing else, so
// it can be called with the decoder's lock held or not.
//
#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <mutex>

namespace multisite {

class SegmentTimeline {
public:
    // A recorded start, and the segment's length when known (-1 if not). The
    // length also places the NEXT segment: segments are contiguous in media
    // time — the muxer starts each one at the keyframe that ended the last —
    // and a segment lost before upload still occupied its span.
    void note(uint64_t seq, int64_t start_ms, int64_t duration_ms = -1) {
        if (start_ms < 0) return;
        std::lock_guard<std::mutex> lk(m_mtx);
        Entry& e = m_known[seq];
        e.start_ms = start_ms;
        if (duration_ms >= 0) e.duration_ms = duration_ms;
    }

    void clear() {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_known.clear();
    }

    size_t known() const {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_known.size();
    }

    // Is this segment's start recorded (or exactly implied by the one before)?
    bool exact(uint64_t seq) const {
        std::lock_guard<std::mutex> lk(m_mtx);
        return exact_locked(seq) >= 0;
    }

    // The media time `seq` starts at. `avg_ms` is the measured average
    // length, used only where nothing better is known.
    int64_t start_of(uint64_t seq, int64_t avg_ms) const {
        std::lock_guard<std::mutex> lk(m_mtx);
        return start_locked(seq, avg_ms);
    }

    // The segment holding `media_ms`: the last one, up to `max_seq`, whose
    // start is at or before it. Starts rise with seq, so a binary search.
    uint64_t seq_at(int64_t media_ms, int64_t avg_ms, uint64_t max_seq) const {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (media_ms < 0) media_ms = 0;
        uint64_t lo = 0, hi = max_seq;          // answer is in [lo, hi]
        while (lo < hi) {
            const uint64_t mid = lo + (hi - lo + 1) / 2;
            if (start_locked(mid, avg_ms) <= media_ms) lo = mid;
            else hi = mid - 1;
        }
        return lo;
    }

private:
    struct Entry {
        int64_t start_ms = -1;
        int64_t duration_ms = -1;
    };
    mutable std::mutex m_mtx;
    std::map<uint64_t, Entry> m_known;

    int64_t exact_locked(uint64_t seq) const {
        auto it = m_known.find(seq);
        if (it != m_known.end()) return it->second.start_ms;
        if (seq > 0) {
            auto prev = m_known.find(seq - 1);
            if (prev != m_known.end() && prev->second.duration_ms >= 0)
                return prev->second.start_ms + prev->second.duration_ms;
        }
        return -1;
    }

    int64_t start_locked(uint64_t seq, int64_t avg_ms) const {
        if (avg_ms < 0) avg_ms = 0;
        const int64_t ex = exact_locked(seq);
        if (ex >= 0) return ex;
        // The nearest recorded neighbours on each side.
        auto above = m_known.upper_bound(seq);
        const bool has_above = above != m_known.end();
        const bool has_below = above != m_known.begin();
        const int64_t gap_after = has_above ? (int64_t)(above->first - seq) : 0;
        if (has_below) {
            auto below = std::prev(above);
            // Extend from below by the average, but never past where the
            // segment above is recorded to start: positions must keep rising.
            int64_t base = below->second.start_ms;
            uint64_t from = below->first;
            if (below->second.duration_ms >= 0) {
                base += below->second.duration_ms;
                ++from;
            }
            int64_t est = base + (int64_t)(seq - from) * avg_ms;
            if (has_above) {
                // Between two recorded starts: spread what is left evenly, so
                // an estimate never overtakes the start that follows it.
                const int64_t span  = above->second.start_ms - base;
                const int64_t steps = (int64_t)(above->first - from);
                if (steps > 0 && span >= 0)
                    est = base + span * (int64_t)(seq - from) / steps;
            }
            return est;
        }
        if (has_above) {
            const int64_t est = above->second.start_ms - gap_after * avg_ms;
            return std::max<int64_t>(0, est);
        }
        return (int64_t)seq * avg_ms;            // nothing known: the old answer
    }
};

} // namespace multisite
