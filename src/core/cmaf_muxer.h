// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// cmaf_muxer.h — wraps FFmpeg's fragmented-MP4 muxer to produce CMAF output:
// one init segment (ftyp+moov) plus a series of media fragments (moof+mdat),
// each carrying the video plus every enabled audio track, multiplexed together
// and cut on video keyframes at the target duration.
//
// Encoder-side only: it consumes already-encoded packets (H.264/HEVC video,
// AAC audio) — it does not encode. Codec-agnostic: the codec is whatever the
// track config says, so HEVC/AV1 need only a different codec id + extradata.
//
#include <cstdint>
#include <vector>
#include <string>
#include <functional>
#include <memory>

namespace multisite {

struct CmafTrack {
    enum Kind { Video, Audio };
    Kind        kind = Video;
    int         codec_id = 0;              // AVCodecID (e.g. AV_CODEC_ID_H264)
    std::vector<uint8_t> extradata;        // SPS/PPS (video) or AudioSpecificConfig (aac)
    // video
    int         width = 0, height = 0;
    int         fps_num = 30, fps_den = 1;
    // audio
    int         sample_rate = 48000, channels = 2;
    int         frame_size = 1024;         // samples per AAC frame
    // metadata carried into event.json / manifest.json
    int         obs_track_idx = 0;
    std::string label;
};

struct CmafPacket {
    int      track = 0;        // index into the tracks vector
    std::vector<uint8_t> data;
    int64_t  pts_ns = 0;       // nanoseconds (OBS timebase)
    int64_t  dts_ns = 0;
    int64_t  duration_ns = 0;
    bool     keyframe = false; // video IDR
};

// Fires when a media fragment is complete: (seq-relative index, bytes,
// duration_s, first_pts_s).
using CmafSegmentCallback =
    std::function<void(uint64_t, std::vector<uint8_t>, double, double)>;

class CmafMuxer {
public:
    // `keep_input_timestamps`: the packets' times are already the event's
    // media time and are written as given. Off (the OBS encoder's case), the
    // first timestamp is shifted to zero, which absorbs the AAC encoder's
    // priming delay; but that also means a second muxer for the same event —
    // a part after a signal drop, or a resume after a restart — starts its
    // media time at zero again, and a decoder following the event sees time
    // go backwards at the join. On, no packet may have a negative time.
    CmafMuxer(std::vector<CmafTrack> tracks, double target_segment_s,
              bool keep_input_timestamps = false);
    ~CmafMuxer();

    // The init segment (ftyp+moov). Available immediately after construction.
    const std::vector<uint8_t>& init_segment() const;

    void on_segment(CmafSegmentCallback cb);
    void push(const CmafPacket& pkt);   // feed encoded packets in DTS order
    void flush();                        // emit the final partial fragment

    bool ok() const;
    const std::string& error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace multisite
