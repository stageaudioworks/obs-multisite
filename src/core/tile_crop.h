// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// tile_crop.h — one tile of a composited feed, as a view.
//
// Phase 10 has the encoder declare how it composited a feed and the decoder
// expose each region as its own source. The OBS plugin gets that almost for
// free: obs_source_frame carries a pointer per plane, so a tile is offset
// pointers and a smaller size. The appliance decodes the whole picture and
// hands one frame to an HDMI output, so the same trick has to be spelled out
// once here.
//
// The returned frame is a VIEW, in the sense fix_planes() in video_output.h
// describes: its plane pointers alias the original buffer and its `data` is
// empty. It is valid only while the original frame is, and must only be handed
// to something that reads plane/stride/width/height — an output's present().
// Nothing is copied and nothing is allocated, however many tiles there are.
//
#include "model.h"
#include "cmaf_decoder.h"

#include <cstring>

namespace multisite {

// The rectangle tile_rect() picks out of `frame`, as a frame that aliases the
// same planes. An absent or out-of-range tile is the whole picture — tile_rect
// guarantees that, and it is the safer answer: something recoverable by hand.
inline DecodedVideoFrame tile_view(const DecodedVideoFrame& frame,
                                   const TileLayout& layout, int index) {
    const TileLayout::Rect r = layout.tile_rect(index, frame.width, frame.height);
    if (frame.native) {
        // A GPU frame has no planes to offset: same buffer, with the tile as
        // the crop. width/height stay the buffer's own size.
        DecodedVideoFrame v = frame;
        v.crop_x = r.x; v.crop_y = r.y; v.crop_w = r.w; v.crop_h = r.h;
        return v;
    }
    DecodedVideoFrame v;
    v.width      = r.w;
    v.height     = r.h;
    v.pts_ns     = frame.pts_ns;
    v.full_range = frame.full_range;
    v.stride[0]  = frame.stride[0];
    v.stride[1]  = frame.stride[1];
    v.stride[2]  = frame.stride[2];
    // Chroma is half resolution in both directions, which is why tile_rect
    // guarantees even edges: an odd offset has no chroma sample to start from,
    // and the colour would shear away from the luma.
    v.plane[0] = frame.plane[0] + (size_t)r.y * frame.stride[0] + r.x;
    v.plane[1] = frame.plane[1] + (size_t)(r.y / 2) * frame.stride[1] + (r.x / 2);
    v.plane[2] = frame.plane[2] + (size_t)(r.y / 2) * frame.stride[2] + (r.x / 2);
    return v;
}

// The same rectangle, copied out into a frame that owns its buffer. A view is
// enough for an output, which reads plane pointers, but not for everything:
// JpegEncoder::encode() walks `data` and rejects a frame whose `data` is empty.
// This is the copy that is. Tightly packed, because the buffer is new and there
// is nothing to share it with.
inline DecodedVideoFrame tile_copy(const DecodedVideoFrame& frame,
                                   const TileLayout& layout, int index) {
    const TileLayout::Rect r = layout.tile_rect(index, frame.width, frame.height);
    DecodedVideoFrame out;
    out.width      = r.w;
    out.height     = r.h;
    out.pts_ns     = frame.pts_ns;
    out.full_range = frame.full_range;
    // tile_rect rounds every edge to an even number, so these are exact and
    // there is never a half-sample left over at the right or bottom edge.
    const int cw = r.w / 2, ch = r.h / 2;
    out.stride[0] = r.w;
    out.stride[1] = cw;
    out.stride[2] = cw;
    const size_t ysz = (size_t)r.w * (size_t)r.h;
    const size_t csz = (size_t)cw * (size_t)ch;
    out.data.assign(ysz + 2 * csz, 0);
    for (int y = 0; y < r.h; ++y)
        std::memcpy(out.data.data() + (size_t)y * r.w,
                    frame.plane[0] + (size_t)(r.y + y) * frame.stride[0] + r.x,
                    (size_t)r.w);
    for (int y = 0; y < ch; ++y) {
        std::memcpy(out.data.data() + ysz + (size_t)y * cw,
                    frame.plane[1] + (size_t)(r.y / 2 + y) * frame.stride[1] +
                        r.x / 2, (size_t)cw);
        std::memcpy(out.data.data() + ysz + csz + (size_t)y * cw,
                    frame.plane[2] + (size_t)(r.y / 2 + y) * frame.stride[2] +
                        r.x / 2, (size_t)cw);
    }
    out.plane[0] = out.data.data();
    out.plane[1] = out.plane[0] + ysz;
    out.plane[2] = out.plane[1] + csz;
    return out;
}

} // namespace multisite
