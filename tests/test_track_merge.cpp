// SPDX-License-Identifier: GPL-3.0-or-later
// test_track_merge.cpp — several audio tracks become one wide frame.
#include "track_merge.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>

using multisite::DecodedAudioFrame;
using multisite_player::TrackMerger;

static int g_fail = 0;
#define CHECK(c, m) do { if (!(c)) { std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while (0)

constexpr int kFrames = 480;                       // 10 ms at 48 kHz
constexpr int64_t kStep = 10'000'000;

// A frame of `ch` channels whose samples are track*100 + channel + frame/1000.
static DecodedAudioFrame frame(int track, int ch, int64_t n, uint32_t frames = kFrames) {
    DecodedAudioFrame f;
    f.track_index = track; f.channels = ch; f.sample_rate = 48000;
    f.frames = frames; f.pts_ns = n * kStep;
    f.interleaved.resize((size_t)frames * ch);
    for (uint32_t i = 0; i < frames; ++i)
        for (int c = 0; c < ch; ++c)
            f.interleaved[(size_t)i * ch + c] = (float)(track * 100 + c) + i / 1000.0f;
    return f;
}

int main() {
    {
        TrackMerger m; m.reset({2, 2}, 48000);
        CHECK(m.width() == 4, "width is the sum of the tracks");
        CHECK(m.push(frame(0, 2, 0)).empty(), "one track alone waits for the other");
        auto out = m.push(frame(1, 2, 0));
        CHECK(out.size() == 1, "two stereo tracks -> one frame");
        if (out.size() == 1) {
            const auto& o = out[0];
            CHECK(o.channels == 4 && o.frames == kFrames && o.pts_ns == 0, "4-ch, same length and pts");
            bool exact = o.interleaved.size() == (size_t)kFrames * 4;
            for (int i = 0; exact && i < kFrames; ++i)
                for (int c = 0; c < 4; ++c) {
                    const int t = c / 2, tc = c % 2;
                    if (o.interleaved[(size_t)i * 4 + c] != (float)(t * 100 + tc) + i / 1000.0f) exact = false;
                }
            CHECK(exact, "track 0 first, samples exact");
        }
    }
    {
        TrackMerger m; m.reset({2, 1}, 48000);   // mixed widths
        m.push(frame(0, 2, 0));
        auto out = m.push(frame(1, 1, 0));
        CHECK(out.size() == 1 && out[0].channels == 3 &&
              out[0].interleaved[2] == 100.0f, "stereo + mono -> 3 channels");
    }
    {
        // Track 1 skips pts 1; track 0 keeps going. Output for pts 1 has silence.
        TrackMerger m; m.reset({2, 2}, 48000);
        std::vector<DecodedAudioFrame> all;
        auto add = [&](std::vector<DecodedAudioFrame> v) { for (auto& f : v) all.push_back(std::move(f)); };
        add(m.push(frame(0, 2, 0))); add(m.push(frame(1, 2, 0)));
        add(m.push(frame(0, 2, 1)));
        add(m.push(frame(0, 2, 2))); add(m.push(frame(1, 2, 2)));
        CHECK(all.size() == 3, "skipped pts is still emitted");
        if (all.size() == 3) {
            CHECK(all[1].pts_ns == kStep, "in order");
            bool silent = true, live = false;
            for (int i = 0; i < kFrames; ++i) {
                if (all[1].interleaved[(size_t)i * 4 + 2] != 0.0f || all[1].interleaved[(size_t)i * 4 + 3] != 0.0f) silent = false;
                if (all[1].interleaved[(size_t)i * 4] != 0.0f) live = true;
            }
            CHECK(silent && live, "missing track is zeros in ch 2-3, track 0 intact");
        }
    }
    {
        // Track 1 stops after pts 0. Track 0 continues; delay never grows.
        TrackMerger m; m.reset({2, 2}, 48000);
        m.push(frame(0, 2, 0)); m.push(frame(1, 2, 0));
        int64_t last_in = 0, worst = 0; size_t n = 1;
        for (int64_t k = 1; k < 200; ++k) {
            last_in = k * kStep;
            for (auto& o : m.push(frame(0, 2, k))) { ++n; worst = std::max(worst, last_in - o.pts_ns); }
        }
        CHECK(n >= 190, "track 0 keeps flowing after track 1 ends");
        CHECK(worst <= 100'000'000 + kStep, "lag is bounded (~100 ms), not growing");
    }
    {
        TrackMerger m; m.reset({2, 2}, 48000);
        m.push(frame(0, 2, 0, 480));
        auto out = m.push(frame(1, 2, 0, 400));
        CHECK(out.size() == 1 && out[0].frames == 400, "unequal sizes: truncated to the shorter");
        CHECK(m.size_mismatches() == 1, "mismatch counted");
    }
    {
        // A seek back flushes and restarts.
        TrackMerger m; m.reset({1, 1}, 48000);
        m.push(frame(0, 1, 500)); m.push(frame(1, 1, 500));
        m.push(frame(0, 1, 501));
        auto out = m.push(frame(0, 1, 0));
        CHECK(out.size() == 1 && out[0].pts_ns == 501 * kStep, "a seek flushes what was pending");
        out = m.push(frame(1, 1, 0));
        CHECK(out.size() == 1 && out[0].pts_ns == 0, "and merging resumes at the new position");
    }
    {
        // Teardown resets with an empty layout: a pending half-group from the old
        // event must not come out on the next event's frames.
        TrackMerger m; m.reset({1, 1}, 48000);
        m.push(frame(0, 1, 900));                 // old event, track 1 never arrived
        m.reset({}, 0);
        m.reset({1, 1}, 48000);
        m.push(frame(0, 1, 0));
        auto out = m.push(frame(1, 1, 0));
        CHECK(out.size() == 1 && out[0].pts_ns == 0, "after a reset nothing stale is flushed");
    }
    {
        // The Player pushes from the decode thread while teardown clears from
        // another. Run under TSan this is the race; without it, it must at
        // least never crash or emit a frame of the wrong width.
        multisite_player::LockedTrackMerger m;
        const std::vector<int> layout{1, 1};
        const int id = 0;
        std::atomic<bool> done{false};
        std::thread pusher([&] {
            for (int n = 0; n < 2000; ++n) {
                m.push(layout, &id, frame(0, 1, n));
                for (auto& o : m.push(layout, &id, frame(1, 1, n)))
                    if (o.channels != 2) { std::printf("  [FAIL] wrong width\n"); ++g_fail; }
            }
            done = true;
        });
        while (!done) m.clear();
        pusher.join();
        CHECK(g_fail == 0, "concurrent push and clear is safe");
    }
    std::printf(g_fail ? "FAILED\n" : "OK\n");
    return g_fail ? 1 : 0;
}
