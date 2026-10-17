// SPDX-License-Identifier: GPL-3.0-or-later
// test_audio_levels.cpp — the meter tapped where the sound leaves the box.
//
// A meter is arithmetic with a lot of ways to be quietly wrong: a peak that
// misses the negative half of a waveform, a channel that lands one slot over,
// a NaN from a corrupt decode read as full scale, a window that never closes so
// the bars never fall, a hold long enough that letting go of a loud moment is
// invisible. None of that needs a sound card to check, and the meter is written
// in a header with no ALSA in it precisely so this runs everywhere —
// alsa_output.cpp is compiled only where ALSA exists, so a test that needed the
// hardware would not run on the laptop where the interface was written.
#include "audio_levels.h"
#include "audio_output.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>

using namespace multisite_player;

static int g_fail = 0;
#define CHECK(c, m) do { if (!(c)) { std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while (0)

static bool near(float a, float b, float eps = 1e-6f) {
    return std::fabs(a - b) <= eps;
}

namespace {
// Keeps every sample it is asked to write.
struct CapturingOutput : AudioOutput {
    std::vector<float> written;
    bool open(const Config&, int, int, std::string&) override { return true; }
    void close() override {}
    bool ok() const override { return true; }
    std::string description() const override { return "capture"; }
    void write(const multisite::DecodedAudioFrame& f) override {
        written.insert(written.end(), f.interleaved.begin(), f.interleaved.end());
    }
    double delay_s() const override { return 0; }
    void flush() override {}
    std::vector<AudioDevice> devices() const override { return {}; }
};
} // namespace

int main() {
    {
        // The deliver path hands the card audible(); muted, it must be all zero
        // and the same shape, and unmuted it must be the sound itself.
        multisite::DecodedAudioFrame loud;
        loud.sample_rate = 48000; loud.channels = 2; loud.frames = 4;
        loud.interleaved = {0.5f, -0.5f, 0.25f, 1.0f, -1.0f, 0.1f, 0.2f, 0.3f};
        CapturingOutput card;
        multisite::DecodedAudioFrame scratch;
        card.write(audible(loud, false, scratch));
        bool zero = card.written.size() == loud.interleaved.size();
        for (float s : card.written) if (s != 0.0f) zero = false;
        CHECK(zero, "muted: every sample written to the card is zero, same length");
        card.written.clear();
        card.write(audible(loud, true, scratch));
        CHECK(card.written == loud.interleaved, "unmuted: the card gets the sound as it is");
    }

    // ── The window ───────────────────────────────────────────────────────────
    // A meter reports what happened since somebody last looked and then starts
    // again. A running maximum would be a bar that never falls, which is a meter
    // nobody can read: the whole point is seeing a loud moment and its end.
    std::printf("== the window closes when it is read ==\n");
    {
        AudioMeter m;
        const float loud[] = { 0.5f, -0.25f };
        m.observe(loud, 1, 2, 2, 1000);

        MeterReading a = m.read(1000 + AudioMeter::kHoldNs / 2);
        CHECK(a.live, "a reading taken inside the hold is live");
        CHECK(a.peak.size() == 2, "one entry per card channel");
        CHECK(near(a.peak[0], 0.5f) && near(a.peak[1], 0.25f),
              "peak is the magnitude, so the negative half counts");

        MeterReading b = m.read(1000 + AudioMeter::kHoldNs / 2);
        CHECK(b.live, "still live — the hold is measured from the last frame");
        CHECK(near(b.peak[0], 0.0f) && near(b.peak[1], 0.0f),
              "a second read reports nothing, because nothing arrived");
    }

    // ── Falling ──────────────────────────────────────────────────────────────
    // Past the hold the reading is flat. This is the difference between "the
    // sound stopped" and "the meter shows the last thing it heard forever".
    std::printf("== the meter falls once the frames stop ==\n");
    {
        AudioMeter m;
        const float v[] = { 0.9f };
        m.observe(v, 1, 1, 1, 1000);

        MeterReading held = m.read(1000 + AudioMeter::kHoldNs - 1);
        CHECK(held.live && near(held.peak[0], 0.9f), "just inside the hold");
        MeterReading dropped = m.read(1000 + AudioMeter::kHoldNs + 1);
        CHECK(!dropped.live, "just outside it, the meter is not live");
        CHECK(near(dropped.peak[0], 0.0f), "and the bars are flat");

        // A clock that went backwards — an NTP step — must not read as a live
        // meter showing something from the past. No unsigned wraparound.
        AudioMeter n;
        const float v2[] = { 0.9f };
        n.observe(v2, 1, 1, 1, 5000000000ULL);
        MeterReading back = n.read(1000);
        CHECK(!back.live, "a clock that stepped back does not look live");
    }

    // ── Width ────────────────────────────────────────────────────────────────
    // The meter is the card's width, not the feed's. A six-track feed into a
    // stereo card is a real case here, and metering the feed would draw four
    // bars of a signal that is not leaving the box.
    std::printf("== the card decides the width, the feed the content ==\n");
    {
        AudioMeter m;
        // Six feed channels, of which the card takes the first two.
        const float six[] = { 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f };
        m.observe(six, 1, 6, 2, 1000);

        MeterReading r = m.read(1000);
        CHECK(r.peak.size() == 2, "two bars for a two-channel card");
        CHECK(near(r.peak[0], 0.1f) && near(r.peak[1], 0.2f),
              "the first two channels, which is what pcm_convert sends");

        // A card wider than the feed: the unreached channels read as silence
        // rather than as the previous frame's content standing in for them.
        AudioMeter w;
        w.observe(six, 1, 2, 4, 1000);
        MeterReading wide = w.read(1000);
        CHECK(wide.peak.size() == 4, "four bars for a four-channel card");
        CHECK(near(wide.peak[0], 0.1f) && near(wide.peak[1], 0.2f) &&
              near(wide.peak[2], 0.0f) && near(wide.peak[3], 0.0f),
              "the two the feed reaches, then silence");
    }

    // ── Rubbish in the buffer ────────────────────────────────────────────────
    std::printf("== a bad sample is not a loud one ==\n");
    {
        AudioMeter m;
        const float nan = std::numeric_limits<float>::quiet_NaN();
        // Frame 0: one NaN and one real sample.
        const float mixed[] = { nan, 0.25f };
        m.observe(mixed, 1, 2, 2, 1000);
        MeterReading r = m.read(1000);
        // A NaN treated as full scale would peg the meter on one corrupt frame
        // and send somebody hunting a clipping problem that does not exist.
        CHECK(near(r.peak[0], 0.0f), "NaN contributes nothing to the peak");
        CHECK(near(r.peak[1], 0.25f), "the real sample beside it stands");

        // An infinity is different: it is what a clipped or broken upmix
        // produces, and it must read as full scale rather than be discarded —
        // dropping it would hide exactly the overshoot the meter is for.
        AudioMeter c;
        const float over[] = { std::numeric_limits<float>::infinity(),
                               -std::numeric_limits<float>::infinity() };
        c.observe(over, 1, 2, 2, 1000);
        MeterReading clip = c.read(1000);
        CHECK(std::isinf(clip.peak[0]) && std::isinf(clip.peak[1]),
              "an infinity reads as full scale in both directions");
    }

    // ── The card going away ──────────────────────────────────────────────────
    std::printf("== closing the card clears the meter ==\n");
    {
        AudioMeter m;
        const float v[] = { 0.8f };
        m.observe(v, 1, 1, 1, 1000);
        m.forget();
        MeterReading r = m.read(1000);
        CHECK(!r.live, "not live after the card was closed");
        CHECK(near(r.peak[0], 0.0f), "and flat, rather than the last thing heard");
    }


        // A clock that went backwards — an NTP step — must not read as a live
    // ── dBFS ─────────────────────────────────────────────────────────────────
    std::printf("== linear to dBFS, floored ==\n");
    {
        CHECK(near(meter_dbfs(1.0f), 0.0f, 1e-3f), "full scale is 0 dBFS");
        CHECK(near(meter_dbfs(0.5f), -6.0206f, 1e-3f), "half is -6.02 dBFS");
        CHECK(near(meter_dbfs(0.0f), -120.0f), "silence floors at -120, not -inf");
        CHECK(near(meter_dbfs(-1.0f), -120.0f), "a negative magnitude is silence");
        // A scale must not have a hole at the bottom, so a very small number
        // floors rather than becoming -infinity and breaking the bar.
        CHECK(meter_dbfs(1e-9f) == -120.0f, "a tiny value floors too");
        CHECK(meter_dbfs(std::numeric_limits<float>::quiet_NaN()) == -120.0f,
              "a NaN reads as silence");
    }

    // ── The reason ───────────────────────────────────────────────────────────
    // Fault-first. A card that failed to open on a muted box is broken, and
    // saying "muted" would send somebody away satisfied while the stream is
    // off the network for a reason they did not choose.
    std::printf("== the reason, and which one wins ==\n");
    {
        CHECK(meter_reason(true, false, false, false) == MeterReason::CardClosed,
              "a closed card is a closed card");
        CHECK(meter_reason(false, false, false, false) == MeterReason::CardClosed,
              "even when the box is also muted — the fault wins");
        CHECK(meter_reason(false, true, true, true) == MeterReason::Muted,
              "open and muted is muted, whatever the feed is carrying");
        CHECK(meter_reason(true, true, false, false) == MeterReason::Idle,
              "open, on, and nothing arriving is idle");
        CHECK(meter_reason(true, true, true, false) == MeterReason::FeedSilent,
              "frames arriving but silent is the feed's fault, not the box's");
        CHECK(meter_reason(true, true, true, true) == MeterReason::Playing,
              "frames arriving with signal is playing");

        // Every reason has a token for the wire and a sentence for the operator,
        // and the tokens are distinct — the page keys off them, so two states
        // sharing one would be two states the page cannot tell apart.
        const MeterReason all[] = {
            MeterReason::CardClosed, MeterReason::Muted, MeterReason::Playing,
            MeterReason::Idle, MeterReason::FeedSilent,
        };
        bool words = true, tokens = true, unique = true;
        for (MeterReason r : all) {
            if (!meter_reason_text(r) || !*meter_reason_text(r)) words = false;
            if (!to_string(r) || !*to_string(r)) tokens = false;
        }
        for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i)
            for (size_t k = i + 1; k < sizeof(all) / sizeof(all[0]); ++k)
                if (std::string(to_string(all[i])) == std::string(to_string(all[k])))
                    unique = false;
        CHECK(tokens, "every reason has a token for the wire");
        CHECK(unique, "and no two reasons share one");
        CHECK(words, "and a sentence for the operator");
    }

    // ── The readout, which is not the meter ─────────────────────────────────
    {
        // The status panel's readout answers "where is the sound going", not
        // "is the event carrying any" — so it has no opinion about the feed.
        CHECK(output_reason(true, true) == MeterReason::Playing,
              "an open card with the sound on is playing, whatever the event");
        CHECK(output_reason(true, false) == MeterReason::CardClosed,
              "a card that would not open outranks everything");
        CHECK(output_reason(false, true) == MeterReason::Muted,
              "an open card with the sound off is muted");
        // Both wrong at once: the card is the fault and is named first, which
        // is what sends somebody to look at the card instead of at a setting.
        CHECK(output_reason(false, false) == MeterReason::CardClosed,
              "and it still outranks a mute — the fault is the card");
    }

    if (g_fail) { std::printf("\n%d check(s) FAILED\n", g_fail); return 1; }
    std::printf("\nall checks passed\n");
    return 0;
}

