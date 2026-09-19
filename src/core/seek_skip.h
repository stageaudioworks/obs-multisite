// Copyright (C) 2026 Stage Audio Works
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>

namespace multisite {

// A seek asks for a moment INSIDE a fragment. Segments are the unit of
// transfer; they need not be the unit of seeking, so the frames before that
// moment are dropped and the seek lands to about a second instead of six.
//
// ONE ARM PER STREAM. That is the whole point of this class, and it is the
// bug it exists to stop coming back.
//
// The skip used to run in the delivery loop, where frames arrive in
// PRESENTATION order. There, the first frame to reach the target really is the
// earliest moment at or after it, and every frame behind it has already gone
// past — so a single shared base and a single disarm were correct by ordering,
// for free, without anyone having to think about it.
//
// It now runs where the frames are MADE, to drop them before the ~3 MB copy
// and the queue. There they arrive in DECODE and TRACK order, and audio and
// video no longer take turns. Measured on a finished recording, audio's first
// pts after a re-anchor ran 300-319 ms ahead of video's, every time. With one
// shared base, claimed by whichever stream spoke first — always audio — every
// video frame was measured from an origin ~311 ms too early, crossed the
// target that much sooner, and kept 311 ms of picture the audio had already
// thrown away. The picture ran that far behind the sound, by a DIFFERENT
// amount after each seek, which is why it reads as drift rather than offset.
//
// So: each stream measures from its own first frame, and disarms only itself.
// Do not "simplify" this back to one base. The ordering that made one base
// safe does not exist on this side of the queue.
class SeekSkip {
public:
    static constexpr int64_t kUnsetPts = INT64_MIN;

    // Arm for a fragment a seek landed inside. `skip_ns` is how far into that
    // fragment the seek asked for.
    void arm(int64_t skip_ns) {
        // The bases and the flags FIRST: m_skip_ns is what makes drops() look
        // at them, so it is written last.
        m_base_v    = kUnsetPts;
        m_base_a    = kUnsetPts;
        m_arrived_v = false;
        m_arrived_a = false;
        m_skip_ns   = skip_ns;
    }

    void disarm() { m_skip_ns = -1; }
    bool armed() const { return m_skip_ns.load() >= 0; }

    // Is this frame before the moment the seek asked for? Consumes the arming
    // for ITS OWN stream when that stream reaches the moment, so each stream
    // skips exactly its own share.
    bool drops(int64_t pts_ns, bool is_video) {
        if (m_skip_ns.load() < 0) return false;
        std::atomic<int64_t>& base    = is_video ? m_base_v : m_base_a;
        std::atomic<bool>&    arrived = is_video ? m_arrived_v : m_arrived_a;
        if (arrived.load()) return false;

        int64_t b = base.load();
        if (b == kUnsetPts) { base.store(pts_ns); b = pts_ns; }
        if (pts_ns - b < m_skip_ns.load()) return true;

        arrived.store(true);
        // Both streams home: back to the cheap path. A stream that carries no
        // frames at all never sets its flag, which is harmless — drops()
        // returns false for the stream that HAS arrived either way.
        if (m_arrived_v.load() && m_arrived_a.load()) m_skip_ns = -1;
        return false;
    }

    // Where each arm started measuring, and whether it has landed. For the
    // log: a seek is landing correctly when both arms report a base and the
    // difference between them is the interleave gap, not the skip.
    int64_t base(bool is_video) const {
        return (is_video ? m_base_v : m_base_a).load();
    }
    bool arrived(bool is_video) const {
        return (is_video ? m_arrived_v : m_arrived_a).load();
    }

private:
    std::atomic<int64_t> m_skip_ns{-1};
    std::atomic<int64_t> m_base_v{kUnsetPts};
    std::atomic<int64_t> m_base_a{kUnsetPts};
    std::atomic<bool>    m_arrived_v{false};
    std::atomic<bool>    m_arrived_a{false};
};

}  // namespace multisite
