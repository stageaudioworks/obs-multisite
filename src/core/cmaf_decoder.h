// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// cmaf_decoder.h — decodes a CMAF stream that arrives as an init segment
// followed by media fragments.
//
// FFmpeg wants a continuous byte stream, but we receive discrete objects. This
// wraps a blocking byte queue behind a custom AVIO read callback: push the init
// segment, then push fragments as they're cached, and the decoder consumes them
// as if reading one long file.
//
// Deliberately free of any OBS dependency so it can be tested against real
// captured segments.
//
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace multisite {

struct DecodedVideoFrame {
    int width = 0, height = 0;
    // The media segment this frame came from, so a host can place a cue at the
    // segment it is actually showing rather than inferring one from a clock.
    uint64_t seq = 0;
    // I420 planes (Y, U, V) with their strides.
    std::vector<uint8_t> data;
    uint8_t* plane[3] = { nullptr, nullptr, nullptr };
    int      stride[3] = { 0, 0, 0 };
    int64_t  pts_ns = 0;          // presentation time within the stream
    bool     full_range = false;
    // Set only when the host asked for native frames (set_native_frames) and a
    // hardware decoder produced one: an AVFrame* (AV_PIX_FMT_VIDEOTOOLBOX,
    // data[3] = CVPixelBufferRef), freed with av_frame_free. `data` and `plane`
    // are then empty. Copying the frame only bumps the shared refcount.
    std::shared_ptr<void> native;
    // The tile of a native frame to show; 0 = the whole frame. Planar frames
    // use plane offsets instead (tile_view).
    int crop_x = 0, crop_y = 0, crop_w = 0, crop_h = 0;
};

struct DecodedAudioFrame {
    int      sample_rate = 48000;
    int      channels = 2;
    int      track_index = 0;     // which audio track this came from
    uint64_t seq = 0;             // the media segment this came from
    uint32_t frames = 0;
    std::vector<float> interleaved;   // planar-free, ready for OBS
    int64_t  pts_ns = 0;
};

using VideoFrameCallback = std::function<void(const DecodedVideoFrame&)>;
using AudioFrameCallback = std::function<void(const DecodedAudioFrame&)>;

// A host's chance to name and prioritise a worker thread; `role` is a short
// fixed string ("poll", "feed", "deliver", "aes67", "decode").
using ThreadHook = std::function<void(const char* role)>;

// Whether a hardware frame whose underlying pixel format is `sw_format` (an
// AVPixelFormat) can be passed on as DecodedVideoFrame::native. A host draws
// 8-bit and 10-bit 4:2:0 biplanar frames (NV12, P010); anything else a
// VideoToolbox decoder produces (4:2:2, 4:4:4) goes the copy-back way.
bool native_sw_format_ok(int sw_format);

class CmafDecoder {
public:
    CmafDecoder();
    ~CmafDecoder();

    void on_video(VideoFrameCallback cb);
    void on_audio(AudioFrameCallback cb);

    // Preferred video decoders, tried in order before FFmpeg's own choice.
    // Exists for the Raspberry Pi 4, whose hardware H.264 decoder
    // ("h264_v4l2m2m") is the difference between three of its four cores left
    // idle and a picture that keeps up with the playout clock.
    //
    // A preference is a hint and never a requirement: a name that is missing,
    // that belongs to a different codec, or that refuses to open is skipped,
    // and the software decoder is used instead. It can therefore only change
    // which decoder runs, never whether playback works. Call before start().
    //
    // "videotoolbox" (macOS) names no decoder of its own: it means the codec's
    // own decoder (H.264 or HEVC) with a VideoToolbox device attached, and the
    // reported codec becomes "<codec> (videotoolbox)".
    void set_preferred_video_decoders(std::vector<std::string> names);

    // Off by default. When on, a frame that a hardware decoder (VideoToolbox)
    // leaves on the GPU is passed on as DecodedVideoFrame::native, with no
    // copy back to system memory and no scaling. Frames from any other decoder
    // are I420 as ever. Only a host that can draw such a frame may turn it on.
    // Call before start().
    void set_native_frames(bool on);

    // Called first thing on the decode thread with the role "decode", so a host
    // can name it and set its priority (the Mac player sets a QoS class).
    // Runs on that thread only; call before start().
    void set_thread_hook(ThreadHook hook);

    // Push the init segment. Must be called before any fragment. Starts the
    // decode thread, which opens the stream and runs until stop().
    bool start(const std::vector<uint8_t>& init_segment);

    // Queue a media fragment for decoding. Normally non-blocking. It blocks
    // while the internal buffer is full (back-pressure rather than ballooning),
    // but only while the decoder is still making progress: once nothing has
    // come out of it for a while, it is declared wedged, the fragment is
    // refused and this returns false, so the host can rebuild the pipeline
    // instead of the picture freezing silently behind a feed loop parked in
    // here for ever. Returns true once the fragment is queued.
    //
    // See BUGS.md entry 0 — a player that stalled indefinitely while downloads
    // kept succeeding, with the feed loop stuck in exactly this call.
    //
    // `seq` is the media segment number these bytes came from. It is carried
    // through to the decoded frames so a host can say which segment it is
    // showing without consulting any clock.
    bool push_fragment(const std::vector<uint8_t>& bytes, uint64_t seq = 0);

    // Bytes currently queued but not yet consumed by the decoder.
    size_t queued_bytes() const;

    // Signal end of stream and wait for the decode thread to finish — but only
    // for a bounded grace period. A thread wedged inside FFmpeg cannot be made
    // to return, and waiting on it for ever would freeze whoever is tearing the
    // decoder down; when the grace expires the thread is abandoned (detached)
    // and this decoder is finished with, so the host builds a fresh one. See
    // BUGS.md entry 0.
    void stop();

    // Testing hook: shorten the grace stop() allows before abandoning a wedged
    // decode thread, so the abandoned path can be exercised without a 5s wait.
    // Never called by production code.
    void set_stop_grace_ms_for_testing(int64_t ms);

    bool ok() const;
    const std::string& error() const;

    // Stream properties, valid once the init segment has been parsed.
    int  video_width() const;
    int  video_height() const;
    int  audio_track_count() const;
    // Each audio track's channel count, in track order; empty until a fragment
    // has been opened. Safe to call from any thread.
    std::vector<int> audio_track_channels() const;
    // Which decoder FFmpeg picked, and how many threads it opened it with.
    // Both are only known once a fragment has been opened.
    //
    // decode_threads() is 0 when the decoder manages its own threads instead
    // of using FFmpeg's — libdav1d runs its own pool, so FFmpeg leaves the
    // count alone and there is no number to report. That is NOT single
    // threaded, and reading it as such invites somebody to go and fix a
    // decoder that is already using every core.
    std::string video_codec() const;
    int  decode_threads() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace multisite
