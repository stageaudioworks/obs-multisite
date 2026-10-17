// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// track_merge.h — several audio tracks, one wide frame.
//
// The decoder emits one frame per track. "All tracks" plays them together on
// one output, each on its own channels, so frames that share a pts are joined
// side by side (track 0's channels first). Nothing here touches a card or a
// clock; tests/test_track_merge.cpp drives it with synthetic frames.
#include "cmaf_decoder.h"

#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace multisite_player {

class TrackMerger {
public:
    // Forgets everything pending and starts for a new track layout.
    void reset(std::vector<int> channels_per_track, int sample_rate);

    // Feed one decoded frame; returns the merged frames now complete (usually
    // zero or one). A track whose frame for a pts is missing is silent in it:
    // as soon as the track has moved past that pts, or 100 ms of stream time
    // after it, whichever comes first.
    std::vector<multisite::DecodedAudioFrame> push(const multisite::DecodedAudioFrame& f);

    int width() const { return m_width; }

    // Groups whose frames had different lengths (the longer was truncated).
    uint64_t size_mismatches() const { return m_mismatches; }

private:
    bool next(multisite::DecodedAudioFrame& out);
    multisite::DecodedAudioFrame take_group(int64_t pts);

    std::vector<int> m_channels;
    std::vector<std::deque<multisite::DecodedAudioFrame>> m_q;
    int      m_width = 0;
    int64_t  m_newest = 0;
    uint64_t m_mismatches = 0;
};

// The Player's use of a TrackMerger: pushed from the decode thread (and the
// detached wedged-decoder thread), cleared from the API/poll threads on a
// teardown. One mutex covers the merger and what it was last set up for.
class LockedTrackMerger {
public:
    // Re-sets up if the layout or the decoder (identity only, never
    // dereferenced) changed, then pushes. `new_mismatch` is true once per
    // setup, the first time frame lengths disagreed.
    std::vector<multisite::DecodedAudioFrame> push(
        const std::vector<int>& layout, const void* decoder,
        const multisite::DecodedAudioFrame& f, bool* new_mismatch = nullptr);
    // Drops a half-merged group and forgets the decoder.
    void clear();

private:
    std::mutex       m_mtx;
    TrackMerger      m_merger;
    std::vector<int> m_layout;
    const void*      m_decoder = nullptr;
    bool             m_logged_mismatch = false;
};

}  // namespace multisite_player
