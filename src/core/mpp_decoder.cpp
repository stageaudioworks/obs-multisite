// SPDX-License-Identifier: GPL-3.0-or-later
#include "mpp_decoder.h"

#include <cstdio>
#include <cstring>

#ifdef MULTISITE_HAVE_MPP
// POSIX-only, and MPP only exists on Linux — so this stays inside the guard and
// the core still builds on Windows without it.
//
// The header lives under a rockchip/ subdirectory of the include prefix
// (/usr/local/include/rockchip/rk_mpi.h), which is where librockchip_mpp
// installs it and what its pkg-config Cflags point above — so it is included by
// that path, not as a bare rk_mpi.h.
#include <unistd.h>
#include <rockchip/rk_mpi.h>
#endif

namespace multisite {

void nv12_to_i420(const uint8_t* y, int y_stride,
                  const uint8_t* uv, int uv_stride,
                  int width, int height,
                  std::vector<uint8_t>& out,
                  uint8_t* plane[3], int stride[3]) {
    const int cw = (width + 1) / 2;
    const int ch = (height + 1) / 2;
    const size_t y_size = (size_t)width * (size_t)height;
    const size_t c_size = (size_t)cw * (size_t)ch;

    out.assign(y_size + 2 * c_size, 0);
    uint8_t* Y = out.data();
    uint8_t* U = Y + y_size;
    uint8_t* V = U + c_size;

    // Only the real rows are read: MPP's buffer is height-padded (1088 for a
    // 1080 picture), and copying the padding would corrupt the chroma offsets.
    for (int r = 0; r < height; ++r) {
        std::memcpy(Y + (size_t)r * (size_t)width,
                    y + (size_t)r * (size_t)y_stride, (size_t)width);
    }
    for (int r = 0; r < ch; ++r) {
        const uint8_t* src = uv + (size_t)r * (size_t)uv_stride;
        uint8_t* du = U + (size_t)r * (size_t)cw;
        uint8_t* dv = V + (size_t)r * (size_t)cw;
        for (int c = 0; c < cw; ++c) {
            du[c] = src[(size_t)c * 2];
            dv[c] = src[(size_t)c * 2 + 1];
        }
    }

    plane[0] = Y; plane[1] = U; plane[2] = V;
    stride[0] = width; stride[1] = cw; stride[2] = cw;
}

#ifdef MULTISITE_HAVE_MPP

namespace {

// FFmpeg's codec name -> MPP's coding type. The RK3588 decoder block takes all
// four; anything else returns false and the caller keeps FFmpeg.
bool coding_of(const std::string& codec, MppCodingType& out) {
    if (codec == "h264")      { out = MPP_VIDEO_CodingAVC;  return true; }
    if (codec == "hevc")      { out = MPP_VIDEO_CodingHEVC; return true; }
    if (codec == "av1")       { out = MPP_VIDEO_CodingAV1;  return true; }
    if (codec == "vp9")       { out = MPP_VIDEO_CodingVP9;  return true; }
    return false;
}

} // namespace

struct MppVideoDecoder::Impl {
    MppCtx ctx = nullptr;
    MppApi* mpi = nullptr;
    MppCodingType coding = MPP_VIDEO_CodingUnused;
    std::string name;
    bool opened = false;
    int errored = 0;       // frames MPP flagged as errored and we dropped
    int refused = 0;       // packets MPP would not take before the deadline
    int width = 0, height = 0;
};

// How long a packet may wait for room in MPP's input queue. The queue is four
// tasks deep and a whole fragment is fed from memory at once, so a full queue is
// the normal state, not a fault; this only bounds a decoder that has wedged.
static constexpr int kPutWaitMs = 1000;

bool mpp_decode_available() {
    // The library being linked is not enough: the board must expose the MPP
    // service, and a container without the vendor kernel will not.
    return ::access("/dev/mpp_service", F_OK) == 0;
}

MppVideoDecoder::MppVideoDecoder() : d(std::make_unique<Impl>()) {}

MppVideoDecoder::~MppVideoDecoder() {
    if (d && d->ctx) {
        if (d->mpi) d->mpi->reset(d->ctx);
        mpp_destroy(d->ctx);
        d->ctx = nullptr;
    }
}

bool MppVideoDecoder::open(const std::string& codec, std::string& error) {
    if (!coding_of(codec, d->coding)) {
        error = "MPP does not decode '" + codec + "'";
        return false;
    }
    if (mpp_create(&d->ctx, &d->mpi) != MPP_OK || !d->ctx || !d->mpi) {
        error = "mpp_create failed";
        d->ctx = nullptr;
        return false;
    }
    if (mpp_init(d->ctx, MPP_CTX_DEC, d->coding) != MPP_OK) {
        error = "mpp_init failed for " + codec;
        mpp_destroy(d->ctx);
        d->ctx = nullptr;
        return false;
    }
    // One packet in, at most one picture out, with the parser splitting a
    // fragment's worth of bytes into access units for us — the fragments are
    // ~a second each, so without this the decoder would hold the whole thing
    // waiting for a boundary that never arrives.
    RK_U32 need_split = 1;
    d->mpi->control(d->ctx, MPP_DEC_SET_PARSER_SPLIT_MODE, &need_split);

    d->name = codec + "_rkmpp";
    d->opened = true;
    return true;
}

// Pull whatever frames are ready and append them as I420. Returns true once the
// end-of-stream frame has come out, which is what flush() waits for.
static bool drain(MppCtx ctx, MppApi* mpi, int& width, int& height,
                  std::vector<DecodedVideoFrame>& out,
                  int64_t pts_ns, uint64_t seq, int& errored) {
    bool saw_eos = false;
    for (;;) {
        MppFrame frame = nullptr;
        if (mpi->decode_get_frame(ctx, &frame) != MPP_OK || !frame) break;
        if (mpp_frame_get_eos(frame)) saw_eos = true;

        if (mpp_frame_get_info_change(frame)) {
            // The stream told us its size; there is nothing to allocate with an
            // internal buffer group, so just acknowledge and carry on.
            width = (int)mpp_frame_get_width(frame);
            height = (int)mpp_frame_get_height(frame);
            mpi->control(ctx, MPP_DEC_SET_INFO_CHANGE_READY, nullptr);
            mpp_frame_deinit(&frame);
            continue;
        }

        // A frame MPP flagged carries a blank buffer — all-zero YUV, which
        // renders as a green field. Two flags, not one: a bad reference frame
        // gets err_info, but a non-reference frame decoded against one gets
        // `discard` instead and would otherwise pass through as green.
        const RK_U32 err_info = mpp_frame_get_errinfo(frame);
        const RK_U32 discard = mpp_frame_get_discard(frame);
        if (err_info || discard) {
            if (errored < 24)
                std::fprintf(stderr, "mpp: frame dropped (err=0x%x discard=0x%x)\n",
                             err_info, discard);
            ++errored;
            mpp_frame_deinit(&frame);
            continue;
        }

        MppBuffer buf = mpp_frame_get_buffer(frame);
        if (buf) {
            const int w = (int)mpp_frame_get_width(frame);
            const int h = (int)mpp_frame_get_height(frame);
            const int hs = (int)mpp_frame_get_hor_stride(frame);
            const int vs = (int)mpp_frame_get_ver_stride(frame);
            const auto* base = static_cast<const uint8_t*>(mpp_buffer_get_ptr(buf));
            if (base && w > 0 && h > 0) {
                DecodedVideoFrame f;
                f.width = w;
                f.height = h;
                f.seq = seq;
                // MPP carries the packet's pts back on the frame. Use the
                // frame's own, not the one just fed: MPP pipelines, so frames
                // come back a packet or two behind, and stamping them with the
                // current packet's time is what pulls the picture off the
                // playout clock.
                const int64_t fp = (int64_t)mpp_frame_get_pts(frame);
                f.pts_ns = (fp != 0) ? fp : pts_ns;
                f.full_range = false;
                // NV12: Y plane, then interleaved UV. The vertical stride
                // (1088 for 1080) is why the conversion is told both strides.
                nv12_to_i420(base, hs, base + (size_t)hs * (size_t)vs, hs,
                             w, h, f.data, f.plane, f.stride);
                out.push_back(std::move(f));
            }
        }
        mpp_frame_deinit(&frame);
    }
    return saw_eos;
}

bool MppVideoDecoder::decode(const uint8_t* data, size_t size, int64_t pts_ns,
                             uint64_t seq, std::vector<DecodedVideoFrame>& out,
                             std::string& error) {
    if (!d->opened) { error = "decoder not open"; return false; }

    MppPacket packet = nullptr;
    if (mpp_packet_init(&packet, const_cast<uint8_t*>(data), size) != MPP_OK || !packet) {
        error = "mpp_packet_init failed";
        return false;
    }
    mpp_packet_set_pts(packet, (RK_S64)pts_ns);

    // Put the packet, taking frames while the queue is full. A full queue means
    // the hardware is still busy, not that a frame is ready, so when a drain
    // finds nothing this waits and tries again rather than giving up. Giving up
    // drops the packet, and every frame that referenced it then decodes blank.
    // MPP copies the bytes on a successful put, so `data` need not outlive this.
    MPP_RET ret = MPP_NOK;
    for (int waited_ms = 0;;) {
        ret = d->mpi->decode_put_packet(d->ctx, packet);
        if (ret == MPP_OK) break;
        const size_t before = out.size();
        drain(d->ctx, d->mpi, d->width, d->height, out, pts_ns, seq, d->errored);
        if (out.size() == before) {
            if (waited_ms >= kPutWaitMs) break;
            ::usleep(1000);
            ++waited_ms;
        }
    }
    mpp_packet_deinit(&packet);

    if (ret != MPP_OK) {
        if (d->refused < 24)
            std::fprintf(stderr, "mpp: packet refused after %d ms (ret=%d)\n",
                         kPutWaitMs, (int)ret);
        ++d->refused;
        error = "mpp decode_put_packet refused the packet";
        return false;
    }
    drain(d->ctx, d->mpi, d->width, d->height, out, pts_ns, seq, d->errored);
    return true;
}

void MppVideoDecoder::flush(std::vector<DecodedVideoFrame>& out) {
    if (!d->opened) return;
    MppPacket packet = nullptr;
    if (mpp_packet_init(&packet, nullptr, 0) != MPP_OK || !packet) return;
    mpp_packet_set_eos(packet);
    bool put = false;
    for (int waited_ms = 0; waited_ms < kPutWaitMs; ++waited_ms) {
        if (d->mpi->decode_put_packet(d->ctx, packet) == MPP_OK) { put = true; break; }
        drain(d->ctx, d->mpi, d->width, d->height, out, 0, 0, d->errored);
        ::usleep(1000);
    }
    mpp_packet_deinit(&packet);
    if (!put) return;

    // get_frame does not block, and the last frames of the fragment are still in
    // the hardware when EOS goes in. Wait for the EOS frame itself, or the
    // decoder is destroyed with them inside and every fragment loses its tail.
    for (int waited_ms = 0; waited_ms < kPutWaitMs; ++waited_ms) {
        if (drain(d->ctx, d->mpi, d->width, d->height, out, 0, 0, d->errored))
            return;
        ::usleep(1000);
    }
    std::fprintf(stderr, "mpp: end of stream not seen within %d ms\n", kPutWaitMs);
}

const std::string& MppVideoDecoder::name() const { return d->name; }
bool MppVideoDecoder::ok() const { return d->opened; }

#else  // no MPP in this build — the class exists, reports itself unavailable

struct MppVideoDecoder::Impl { std::string name; };

bool mpp_decode_available() { return false; }

MppVideoDecoder::MppVideoDecoder() : d(std::make_unique<Impl>()) {}
MppVideoDecoder::~MppVideoDecoder() = default;

bool MppVideoDecoder::open(const std::string&, std::string& error) {
    error = "this build has no librockchip_mpp";
    return false;
}

bool MppVideoDecoder::decode(const uint8_t*, size_t, int64_t, uint64_t,
                             std::vector<DecodedVideoFrame>&, std::string& error) {
    error = "this build has no librockchip_mpp";
    return false;
}

void MppVideoDecoder::flush(std::vector<DecodedVideoFrame>&) {}
const std::string& MppVideoDecoder::name() const { return d->name; }
bool MppVideoDecoder::ok() const { return false; }

#endif

} // namespace multisite
