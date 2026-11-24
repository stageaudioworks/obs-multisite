// SPDX-License-Identifier: GPL-3.0-or-later
// test_segment_cut.cpp — the muxer cuts at the keyframe NEAREST the target
// segment length (segment_cut.h), not the first at or past it.
//
// Encoders put keyframes early. OBS's x264 and ffmpeg encoders truncate the
// interval to whole frames (179 frames at 29.97 fps for 6 s); VideoToolbox
// spaces them 177-180 frames apart at 30 fps. Each early keyframe the rule
// passed over made a segment twice the length set. The keyframe times in §2
// and §3 are the ones measured from two VideoToolbox broadcasts on 2026-11-23.
#include "../src/core/segment_cut.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

// The segment lengths the muxer makes from a list of keyframe times, as
// cmaf_muxer.cpp does: a segment opens at a keyframe, and each later keyframe
// is offered to the rule with its time since the segment opened and since the
// keyframe before it. `use_gap` false is the rule before this one (one frame
// of tolerance, no gap), for showing what it did with the same keyframes.
static std::vector<double> segments(const std::vector<double>& keys, double target,
                                    double frame_s, bool use_gap = true) {
    std::vector<double> out;
    if (keys.empty()) return out;
    double start = keys[0], prev = keys[0];
    for (size_t i = 1; i < keys.size(); ++i) {
        const double gap = use_gap ? keys[i] - prev : 0.0;
        if (keyframe_starts_segment(keys[i] - start, target, frame_s, gap)) {
            out.push_back(keys[i] - start);
            start = keys[i];
        }
        prev = keys[i];
    }
    return out;
}

static bool all_between(const std::vector<double>& v, double lo, double hi) {
    for (double d : v) if (d < lo || d > hi) return false;
    return !v.empty();
}
static int longer_than(const std::vector<double>& v, double s) {
    int n = 0;
    for (double d : v) if (d > s) ++n;
    return n;
}

// OBS's keyframes: every `keyint_sec * fps_num / fps_den` frames, truncated
// (obs-x264.c, obs-ffmpeg-video-encoders.c), for `n` intervals from 0.
static std::vector<double> obs_keys(int keyint_sec, int fps_num, int fps_den, int n) {
    const int frames = keyint_sec * fps_num / fps_den;
    std::vector<double> k;
    for (int i = 0; i <= n; ++i) k.push_back((double)i * frames * fps_den / fps_num);
    return k;
}

int main() {
    const double target = 6.0;

    std::printf("== 1. OBS's truncated intervals: every keyframe cuts ==\n");
    {
        struct { int num, den; double seg; const char* what; } rates[] = {
            { 30, 1, 6.0, "30 fps: 6.000 s" },
            { 25, 1, 6.0, "25 fps: 6.000 s" },
            { 30000, 1001, 5.9726, "29.97 fps: 5.973 s (179 frames), not 11.945 s" },
            { 60000, 1001, 5.9893, "59.94 fps: 5.989 s (359 frames), not 11.979 s" },
            { 24000, 1001, 5.9643, "23.976 fps: 5.964 s (143 frames), not 11.929 s" },
        };
        for (auto& r : rates) {
            auto segs = segments(obs_keys(6, r.num, r.den, 20), target, (double)r.den / r.num);
            CHECK(segs.size() == 20 && all_between(segs, r.seg - 0.001, r.seg + 0.001), r.what);
        }
        CHECK(all_between(segments(obs_keys(2, 30000, 1001, 20), 2.0, 1001.0 / 30000), 1.968, 1.969),
              "2 s segments at 29.97 fps: 1.969 s (59 frames)");
        CHECK(all_between(segments(obs_keys(15, 30000, 1001, 8), 15.0, 1001.0 / 30000), 14.98, 14.982),
              "15 s segments at 29.97 fps: 14.981 s (449 frames)");
    }

    std::printf("== 2. VideoToolbox, measured: 177-180 frames apart ==\n");
    {
        // Event 01M84BQWVNGXAQ1MNGWGP6RNVQ, video keyframe pts in seconds.
        const std::vector<double> keys = { 0.066667, 6.033333, 11.933333, 17.833333,
            23.733333, 29.700000, 35.666667, 41.666667, 47.566667, 53.533333,
            59.433333, 65.366667 };
        auto segs = segments(keys, target, 1.0 / 30.0);
        CHECK(segs.size() == keys.size() - 1 && all_between(segs, 5.89, 6.01),
              "every keyframe cuts: eleven segments of 5.90-6.00 s");
        auto before = segments(keys, target, 1.0 / 30.0, false);
        std::printf("      the one-frame rule made %d of %zu segments longer than 7 s\n",
                    longer_than(before, 7.0), before.size());
        CHECK(longer_than(before, 7.0) > 0,
              "(the one-frame rule, on the same keyframes, made 12 s segments)");
    }

    std::printf("== 3. VideoToolbox, measured: every keyframe 177 frames apart ==\n");
    {
        // Event 01M84EJ9HHMVA0W4XCT50HJ159: all sixteen segments ran 11.8 s.
        std::vector<double> keys;
        for (int i = 0; i < 32; ++i) keys.push_back(0.066667 + i * 177.0 / 30.0);
        auto segs = segments(keys, target, 1.0 / 30.0);
        CHECK(segs.size() == 31 && all_between(segs, 5.899, 5.901),
              "every keyframe cuts: 5.9 s segments, not 11.8 s");
        CHECK(longer_than(segments(keys, target, 1.0 / 30.0, false), 11.0) == 15,
              "(the one-frame rule made every segment 11.8 s)");
    }

    std::printf("== 4. It never cuts twice in one interval ==\n");
    {
        // Two hours of keyframes 177-180 frames apart in a scrambled order:
        // every keyframe is a cut, and no segment is short.
        std::vector<double> keys = { 0.0 };
        const int spacing[] = { 179, 177, 180, 178, 177, 180, 179, 178, 180, 177 };
        for (int i = 0; i < 1200; ++i) keys.push_back(keys.back() + spacing[i % 10] / 30.0);
        auto segs = segments(keys, target, 1.0 / 30.0);
        CHECK(segs.size() == 1200 && all_between(segs, 5.89, 6.01),
              "1200 keyframes, 1200 segments, each 5.90-6.00 s");
    }

    std::printf("== 5. Keyframes more often than segments ==\n");
    {
        // 2 s keyframes at 29.97 fps (59 frames, 1.969 s) with 6 s segments:
        // the third keyframe cuts (5.906 s), never the second (3.937 s).
        auto segs = segments(obs_keys(2, 30000, 1001, 30), target, 1001.0 / 30000);
        CHECK(segs.size() == 10 && all_between(segs, 5.905, 5.907),
              "2 s keyframes make 5.906 s segments, every third keyframe");
        // One keyframe a second, as test_cmaf_durations feeds the muxer.
        auto ones = segments(obs_keys(1, 30, 1, 30), target, 1.0 / 30.0);
        CHECK(ones.size() == 5 && all_between(ones, 5.999, 6.001),
              "1 s keyframes make 6 s segments");
    }

    std::printf("== 6. An extra keyframe mid-segment does not cut ==\n");
    {
        // A scene cut an encoder inserted at 3.1 s, between 6 s keyframes.
        auto segs = segments({ 0.0, 3.1, 5.9, 11.8 }, target, 1.0 / 30.0);
        CHECK(segs.size() == 2 && std::fabs(segs[0] - 5.9) < 0.001,
              "the inserted keyframe is passed over; 5.9 s cuts");
        CHECK(!keyframe_starts_segment(2.9, target, 1.0 / 30.0, 20.0),
              "a 20 s gap does not allow a 2.9 s segment: capped at half the target");
        CHECK(keyframe_starts_segment(3.0, target, 1.0 / 30.0, 20.0),
              "the cap is half the target, exactly");
    }

    std::printf("== 7. With no gap known: one frame of tolerance ==\n");
    {
        // The first keyframe after the event opened on a non-key packet.
        // Seen 2026-11-23: first frame at 0.067 s, keyframe at 6.033 s, the
        // difference of two ns timestamps 7e-10 s under target − frame.
        const double first = (double)66666667 / 1e9;
        const double key   = (double)6033333333 / 1e9;
        CHECK(keyframe_starts_segment(key - first, target, 1.0 / 30.0, 0.0),
              "a keyframe exactly one frame short (from ns timestamps) cuts");
        CHECK(!keyframe_starts_segment(key - first - 1.0 / 30.0, target, 1.0 / 30.0, 0.0),
              "two frames short does not, with no gap to go on");
        CHECK(!keyframe_starts_segment(5.9726, target, 0.0, 0.0),
              "no frame rate and no gap: the strict rule");
        CHECK(keyframe_starts_segment(6.0, target, 0.0, 0.0), "at the target still cuts");
    }

    std::printf(g_fail ? "\nFAILED: %d\n" : "\nall ok\n", g_fail);
    return g_fail ? 1 : 0;
}
