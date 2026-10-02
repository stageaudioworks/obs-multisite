// Copyright (C) 2026 Stage Audio Works
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace multisite {

// How much an output may hold while it waits for the video encoder's codec
// config, before it gives up and stops.
//
// Some encoders (Apple's VideoToolbox HEVC) give their config only with their
// first keyframe, so the start finishes then: it writes event.json, init.mp4
// and the manifest to the bucket, which from a church's uplink takes seconds,
// and everything encoded meanwhile is held. These bounds exist for an encoder
// that never gives a config, so that ends as a stopped output with a reason
// rather than as memory climbing.
//
// The count is of VIDEO packets. It used to count every packet, under a
// comment calling 900 "about 30 s of 30 fps video" — but three AAC tracks
// add about 141 packets a second to video's 30, so 900 went in 5.3 s, while a
// start was still writing to the bucket. The output stopped with "the video
// encoder never produced a codec config" when it had, and the event it had
// just created was left live with no segments (2026-10-02 and -03, VideoToolbox
// HEVC, three audio tracks).
class HoldBudget {
public:
    static constexpr std::size_t kMaxVideoPackets = 900;        // ~30 s of 30 fps
    static constexpr std::size_t kMaxBytes        = 64u << 20;
    static constexpr int64_t     kMaxMs           = 15000;

    void add(bool is_video, std::size_t bytes) {
        if (is_video) ++video_;
        bytes_ += bytes;
    }
    bool exceeded(int64_t held_ms) const {
        return video_ > kMaxVideoPackets || bytes_ > kMaxBytes || held_ms > kMaxMs;
    }
    void reset() { video_ = 0; bytes_ = 0; }

private:
    std::size_t video_ = 0;
    std::size_t bytes_ = 0;
};

} // namespace multisite
