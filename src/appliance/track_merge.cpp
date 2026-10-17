// SPDX-License-Identifier: GPL-3.0-or-later
#include "track_merge.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace multisite_player {

using multisite::DecodedAudioFrame;

namespace {
constexpr int64_t kWaitNs = 100'000'000;     // how long a missing track is waited for
constexpr int64_t kJumpNs = 1'000'000'000;   // a backwards step this big is a seek

int64_t frame_ns(const DecodedAudioFrame& f) {
    return f.sample_rate > 0 ? (int64_t)f.frames * 1'000'000'000 / f.sample_rate : 0;
}
}  // namespace

void TrackMerger::reset(std::vector<int> channels_per_track, int sample_rate) {
    m_channels = std::move(channels_per_track);
    m_q.assign(m_channels.size(), {});
    m_width = 0;
    for (int c : m_channels) m_width += c;
    m_newest = 0;
    m_mismatches = 0;
}

DecodedAudioFrame TrackMerger::take_group(int64_t pts) {
    // Which tracks have a frame at this pts, and how long the merged frame is.
    std::vector<DecodedAudioFrame> got(m_q.size());
    std::vector<bool> have(m_q.size(), false);
    uint32_t frames = 0;
    bool first = true, mismatch = false;
    DecodedAudioFrame out;
    for (size_t t = 0; t < m_q.size(); ++t) {
        if (m_q[t].empty()) continue;
        const DecodedAudioFrame& h = m_q[t].front();
        if (std::llabs(h.pts_ns - pts) * 2 >= std::max<int64_t>(frame_ns(h), 1)) continue;
        got[t] = std::move(m_q[t].front());
        m_q[t].pop_front();
        have[t] = true;
        if (first) { frames = got[t].frames; out.seq = got[t].seq; out.pts_ns = got[t].pts_ns;
                     out.sample_rate = got[t].sample_rate; first = false; }
        else if (got[t].frames != frames) { mismatch = true; frames = std::min(frames, got[t].frames); }
    }
    if (mismatch) ++m_mismatches;
    out.track_index = 0;
    out.channels = m_width;
    out.frames = frames;
    out.interleaved.assign((size_t)frames * m_width, 0.0f);   // silence unless filled
    int off = 0;
    for (size_t t = 0; t < m_q.size(); ++t) {
        const int c = m_channels[t];
        if (have[t]) {
            const int n = std::min(c, got[t].channels);
            for (uint32_t i = 0; i < frames; ++i)
                for (int ch = 0; ch < n; ++ch)
                    out.interleaved[(size_t)i * m_width + off + ch] =
                        got[t].interleaved[(size_t)i * got[t].channels + ch];
        }
        off += c;
    }
    return out;
}

bool TrackMerger::next(DecodedAudioFrame& out) {
    // The group to settle is the earliest pending pts.
    int64_t p = 0;
    bool any = false;
    for (auto& q : m_q)
        if (!q.empty() && (!any || q.front().pts_ns < p)) { p = q.front().pts_ns; any = true; }
    if (!any) return false;
    // Ready when every track either has its frame (or has moved past it), or
    // has had long enough to produce one.
    for (auto& q : m_q)
        if (q.empty() && m_newest < p + kWaitNs) return false;
    out = take_group(p);
    return true;
}

std::vector<DecodedAudioFrame> TrackMerger::push(const DecodedAudioFrame& f) {
    std::vector<DecodedAudioFrame> out;
    if (f.track_index < 0 || (size_t)f.track_index >= m_q.size()) return out;

    // A seek: pts went back. What is pending belongs to the old position, so
    // send it on (silence for what is missing) and start the clock over.
    if (m_newest - f.pts_ns > kJumpNs) {
        m_newest = INT64_MAX / 2;   // everything pending is overdue
        DecodedAudioFrame g;
        while (next(g)) out.push_back(std::move(g));
        m_newest = 0;
    }
    m_q[f.track_index].push_back(f);
    m_newest = std::max(m_newest, f.pts_ns);
    DecodedAudioFrame g;
    while (next(g)) out.push_back(std::move(g));
    return out;
}

std::vector<DecodedAudioFrame> LockedTrackMerger::push(
    const std::vector<int>& layout, const void* decoder,
    const DecodedAudioFrame& f, bool* new_mismatch) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (layout != m_layout || decoder != m_decoder) {
        m_merger.reset(layout, f.sample_rate);
        m_layout = layout;
        m_decoder = decoder;
        m_logged_mismatch = false;
    }
    auto out = m_merger.push(f);
    if (new_mismatch) {
        *new_mismatch = m_merger.size_mismatches() && !m_logged_mismatch;
        if (*new_mismatch) m_logged_mismatch = true;
    }
    return out;
}

void LockedTrackMerger::clear() {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_merger.reset({}, 0);
    m_layout.clear();
    m_decoder = nullptr;
}

}  // namespace multisite_player
