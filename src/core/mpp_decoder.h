// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// mpp_decoder.h — hardware video decode through Rockchip's MPP.
//
// On a Rockchip vendor kernel the decode door is librockchip_mpp over
// /dev/mpp_service; the /dev/videoN V4L2 path is mainline-only. On a ROCK 5B
// that means the player's h264_v4l2m2m preference can never open, and the
// picture falls back to software. This is the hardware path for those boards.
//
// Built only where librockchip_mpp is found (MULTISITE_HAVE_MPP). Everywhere
// else the class still exists and reports itself unavailable, so callers need
// no #ifdef and the software path is untouched — the same "a preference can
// change which decoder runs, never whether playback works" rule the FFmpeg
// preference list follows.
//
#include "cmaf_decoder.h"   // DecodedVideoFrame

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace multisite {

// True when this build has librockchip_mpp and this machine exposes the MPP
// service. Cheap; safe to call before deciding whether to open a decoder.
bool mpp_decode_available();

// NV12 — what MPP hands back — into the I420 planes a DecodedVideoFrame
// carries.
//
// MPP's buffer is stride-aligned and height-padded: a 1920x1080 picture comes
// back with a stride of 1920 and 1088 rows. The source strides are therefore
// passed separately, and only `height` real rows are read. This is pure memory
// movement, so it is tested without a board.
void nv12_to_i420(const uint8_t* y, int y_stride,
                  const uint8_t* uv, int uv_stride,
                  int width, int height,
                  std::vector<uint8_t>& out,
                  uint8_t* plane[3], int stride[3]);

// One MPP decode context for one video stream. Holds no policy: the caller
// decides when to open, feed and flush, exactly as it does for the FFmpeg
// decoder.
class MppVideoDecoder {
public:
    MppVideoDecoder();
    ~MppVideoDecoder();
    MppVideoDecoder(const MppVideoDecoder&) = delete;
    MppVideoDecoder& operator=(const MppVideoDecoder&) = delete;

    // Opens for a codec by its FFmpeg name — "h264", "hevc", "av1", or "vp9".
    // The RK3588 decoder block takes all four; the mapping to MPP's coding
    // types lives in the implementation. False, with `error` set, when this
    // build or machine has no MPP, or the codec is not one it decodes — the
    // caller then keeps FFmpeg's decoder (which is how AV1 plays on a machine
    // with no MPP at all, through libdav1d). Never throws.
    bool open(const std::string& codec, std::string& error);

    // Decode one packet, appending any frames it produces. `seq` and `pts_ns`
    // are carried onto every frame, as the FFmpeg path does.
    bool decode(const uint8_t* data, size_t size, int64_t pts_ns, uint64_t seq,
                std::vector<DecodedVideoFrame>& out, std::string& error);

    // Drain frames still held inside the decoder at end of stream.
    void flush(std::vector<DecodedVideoFrame>& out);

    // The decoder actually in use, for the status line — "h264_rkmpp",
    // "hevc_rkmpp", "av1_rkmpp" or "vp9_rkmpp". Empty when not open.
    const std::string& name() const;

    bool ok() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace multisite
