// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// audio_levels.h — how loud the sound is, taken where it leaves the box.
//
// The meter is tapped at the point the samples are handed to the sound card,
// not at the point they arrive from the network. That is the whole design of
// this header and it is worth saying why, because the obvious place to put a
// meter — on the decoded feed — answers a different question.
//
// On this box the sound leaves through a card the AES67 daemon reads, so "is
// there audio?" is not a property of the event, it is a property of what the
// card was given. A muted box, a card that failed to open, and a feed that has
// genuinely gone quiet all look identical on a meter of the feed, and they are
// three completely different things to an operator at eleven o'clock at night.
// So the bars fall to silence when the card is closed or the sound is muted,
// because that is what the card is being given, and they are accompanied by
// words saying which of the reasons it is.
//
// The arithmetic is the part that can be quietly wrong — a peak that misses the
// negative half of a waveform, a NaN from a bad decode read as full scale, a
// channel counted twice — and it lives in a header with no ALSA in it, so it
// runs anywhere, including a laptop where alsa_output.cpp is not compiled at
// all. `tests/test_audio_levels.cpp` checks it.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "../core/cmaf_decoder.h"

namespace multisite_player {

// Why the bars read what they read, in words an operator can act on.
//
// This travels with the numbers rather than being a separate "audio is present"
// indicator, because it is the same fact: a meter reading zero has one cause
// and it is the one named here. A separate lamp beside the bars is a second
// thing to be wrong.
enum class MeterReason {
    // The card is not open. Nothing is being handed to it at all, so the bars
    // are flat and this is a fault to fix, not a state to wait out.
    CardClosed,
    // Switched off by hand. The card is open and is deliberately being fed
    // silence, so the stream stays up for whoever is subscribed to it and the
    // flat bars are exactly what was asked for.
    Muted,
    // Card open, sound on, frames arriving, and there is signal in them.
    Playing,
    // Card open and being fed, but nothing is being delivered at the moment —
    // delivered, stopped, held, or between events. The card is held up with
    // silence by the output's own keep-alive, so this is not a fault either.
    Idle,
    // Card open, sound on, frames arriving, and they are silent. The event
    // itself carries no sound: a muted microphone upstream, or a track that was
    // never in the feed.
    FeedSilent,
};

inline const char* to_string(MeterReason r) {
    switch (r) {
        case MeterReason::CardClosed: return "card-closed";
        case MeterReason::Muted:      return "muted";
        case MeterReason::Playing:    return "playing";
        case MeterReason::Idle:       return "idle";
        case MeterReason::FeedSilent: return "feed-silent";
    }
    return "card-closed";
}

// A sentence for the interface, not a token. The words are the point of the
// reason, so they are written here rather than assembled in the web page where
// they would have to be kept in step with the states by hand.
inline const char* meter_reason_text(MeterReason r) {
    switch (r) {
        case MeterReason::CardClosed: return "sound card is not open";
        case MeterReason::Muted:      return "muted — fed silence";
        case MeterReason::Playing:    return "playing";
        case MeterReason::Idle:       return "nothing playing — output held up";
        case MeterReason::FeedSilent: return "the feed is silent";
    }
    return "sound card is not open";
}

// Decide the reason from the three things that can be true at once.
//
// Order matters, and it is fault-first. A card that has failed to open on a box
// that is also muted is broken, and saying "muted" would send somebody away
// satisfied while the stream is off the network for a reason they did not
// choose. "Muted" only stands once the card is known to be open and therefore
// doing what it was told.
inline MeterReason meter_reason(bool audio_enabled, bool card_open,
                                bool live, bool any_signal) {
    if (!card_open)     return MeterReason::CardClosed;
    if (!audio_enabled) return MeterReason::Muted;
    if (!live)          return MeterReason::Idle;
    return any_signal ? MeterReason::Playing : MeterReason::FeedSilent;
}

// The same decision, for the readout in the status panel.
//
// `live` and `any_signal` are not part of it: that readout is about where the
// sound is going, and a box playing a genuinely silent event must say "playing"
// there. The meters are what show that the event carries no signal, and a
// readout that changed with the programme material would be the wrong
// instrument for a panel an operator glances at.
//
// The order is fault-first and is the same order as meter_reason: a card that
// would not open on a box whose sound is also switched off is a fault with two
// causes, and somebody sent away by "muted" will not find it because they never
// looked for it. The card's own error travels beside this in the status.
inline MeterReason output_reason(bool audio_enabled, bool card_open) {
    if (!card_open)     return MeterReason::CardClosed;
    if (!audio_enabled) return MeterReason::Muted;
    return MeterReason::Playing;
}

// Linear full-scale to dBFS, floored rather than reported as -infinity: the
// interface draws a scale, and a scale cannot have a hole in it at the bottom.
inline float meter_dbfs(float linear) {
    if (!(linear > 0.0f)) return -120.0f;
    const float db = 20.0f * std::log10(linear);
    return db < -120.0f ? -120.0f : db;
}

// One reading, ready for a bar meter.
struct MeterReading {
    // Peak since the previous reading, linear (0..1.0 and beyond on a clipped
    // master), one entry per channel OF THE CARD. The list is kept at the card's
    // width even when the reading is flat, so a page that is already drawing
    // eight bars does not have to re-lay-out when the sound stops.
    std::vector<float> peak;
    // A frame reached the card recently enough that the numbers mean something.
    // False means "showing a fall", never "there is no sound card".
    bool live = false;
};

class AudioMeter {
public:
    // How long a reading stands after the last frame reached the card.
    //
    // Long enough that an interface polling twice a second cannot see the meter
    // flicker between reads — the delivery loop hands a frame over roughly every
    // 20 ms, so this is twenty frames of grace — and short enough that letting
    // go of a loud moment is visible within half a second. Under it the meter
    // falls; over it the meter is flat and the reason says why.
    static constexpr uint64_t kHoldNs = 400000000ULL;   // 400 ms

    // Called with exactly the frame that was handed to the card, and with the
    // card's own channel count.
    //
    // The card's width is passed separately and deliberately: a six-track feed
    // into a stereo card is a real case on this project, and the channels past
    // the second one are dropped before the card sees them. Metering the feed's
    // channels would show four bars of a signal that is not leaving the box,
    // which is worse than showing nothing. Channels are taken from the front,
    // matching pcm_convert's policy — the same first-N-channels the card is
    // actually given — and a card wider than the feed reads as silence on the
    // channels the feed does not reach.
    void observe(const float* interleaved, uint32_t frames, int src_channels,
                 int card_channels, uint64_t now_ns) {
        if (!interleaved || frames == 0 || src_channels <= 0 ||
            card_channels <= 0)
            return;

        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_peak.size() != (size_t)card_channels)
            m_peak.assign((size_t)card_channels, 0.0f);

        const int n = src_channels < card_channels ? src_channels
                                                   : card_channels;
        for (uint32_t f = 0; f < frames; ++f) {
            const float* in = interleaved + (size_t)f * (size_t)src_channels;
            for (int c = 0; c < n; ++c) {
                const float v = in[c];
                // A NaN is a decoder fault, not a level. Treating it as full
                // scale would peg the meter on a corrupt frame and leave an
                // operator hunting a clipping problem that does not exist.
                if (!(v == v)) continue;
                const float a = v < 0.0f ? -v : v;
                if (a > m_peak[(size_t)c]) m_peak[(size_t)c] = a;
            }
        }
        m_last_obs_ns.store(now_ns, std::memory_order_relaxed);
    }

    // Read and begin a new window: a meter reports what happened since the last
    // time somebody looked, then starts again. Not a running maximum, because a
    // meter that never falls is a meter nobody can read.
    MeterReading read(uint64_t now_ns) {
        std::lock_guard<std::mutex> lk(m_mtx);
        MeterReading out;
        out.peak.assign(m_peak.size(), 0.0f);

        const uint64_t last = m_last_obs_ns.load(std::memory_order_relaxed);
        out.live = last != 0 && now_ns >= last && now_ns - last < kHoldNs;
        if (out.live) out.peak = m_peak;

        std::fill(m_peak.begin(), m_peak.end(), 0.0f);
        return out;
    }

    // The card was closed: the next reading is flat and says so, rather than
    // reporting the last thing heard before it went away.
    void forget() {
        std::lock_guard<std::mutex> lk(m_mtx);
        std::fill(m_peak.begin(), m_peak.end(), 0.0f);
        m_last_obs_ns.store(0, std::memory_order_relaxed);
    }

private:
    mutable std::mutex m_mtx;
    std::vector<float> m_peak;                       // guarded by m_mtx
    std::atomic<uint64_t> m_last_obs_ns{0};
};

// Muting no longer closes the card, so something still has to be handed over
// every frame's worth of time or the card under-runs — and a card that has
// under-run stops producing samples, which on this box is the same thing as the
// AES67 stream going off the network. Silence of the frame's own shape keeps
// the output running and everything downstream in step, and because the meter is
// tapped where this is handed over, the bars fall exactly as they should.
inline multisite::DecodedAudioFrame silence_like(
        const multisite::DecodedAudioFrame& f) {
    multisite::DecodedAudioFrame s;
    s.sample_rate = f.sample_rate;
    s.channels    = f.channels;
    s.frames      = f.frames;
    s.track_index = f.track_index;
    s.pts_ns      = f.pts_ns;
    const int ch = f.channels > 0 ? f.channels : 1;
    s.interleaved.assign((size_t)f.frames * (size_t)ch, 0.0f);
    return s;
}

// What the output is handed: the frame itself, or silence of its shape while
// muted. `scratch` holds the silence; the one place this is decided, so the
// card and the meter cannot disagree.
inline const multisite::DecodedAudioFrame& audible(
        const multisite::DecodedAudioFrame& f, bool enabled,
        multisite::DecodedAudioFrame& scratch) {
    if (enabled) return f;
    scratch = silence_like(f);
    return scratch;
}

} // namespace multisite_player
