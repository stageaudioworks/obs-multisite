// SPDX-License-Identifier: GPL-3.0-or-later
#include "cmaf_muxer.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
}

#include <cstring>
#include <algorithm>

// FFmpeg 7.0 (libavformat 61) made the AVIO write callback's buffer const.
// Support both so the same source builds against FFmpeg 6.x and 7.x.
#if LIBAVFORMAT_VERSION_MAJOR >= 61
#  define MS_AVIO_WRITE_BUF const uint8_t*
#else
#  define MS_AVIO_WRITE_BUF uint8_t*
#endif

namespace multisite {

// Common muxer timebase for feeding packets (nanoseconds → 1/1e9).
static const AVRational NS_TB = {1, 1000000000};

// CMAF/DASH media segments are expected to begin with a `styp` box declaring
// the segment brands. FFmpeg's movenc only writes it when it manages segment
// files itself, so with caller-controlled cut points we emit it ourselves.
static void append_styp(std::vector<uint8_t>& out) {
    static const char* brands[] = { "msdh", "cmfs", "iso6" };
    const uint32_t size = 8 + 4 + 4 + 4 * 3;   // hdr + major + minor + 3 brands
    auto u32 = [&out](uint32_t v) {
        out.push_back((uint8_t)(v >> 24)); out.push_back((uint8_t)(v >> 16));
        out.push_back((uint8_t)(v >> 8));  out.push_back((uint8_t)v);
    };
    auto tag = [&out](const char* t) {
        out.push_back((uint8_t)t[0]); out.push_back((uint8_t)t[1]);
        out.push_back((uint8_t)t[2]); out.push_back((uint8_t)t[3]);
    };
    u32(size);
    tag("styp");
    tag("msdh");        // major brand
    u32(0);             // minor version
    for (const char* b : brands) tag(b);
}

struct CmafMuxer::Impl {
    std::vector<CmafTrack> tracks;
    double target_s = 6.0;
    CmafSegmentCallback cb;

    AVFormatContext* fmt = nullptr;
    AVIOContext*     avio = nullptr;
    std::vector<uint8_t> buf;        // current output buffer (init, then per-segment)
    std::vector<uint8_t> init;       // captured init segment
    std::vector<int>     stream_index; // track → AVStream index

    bool     header_written = false;
    bool     ok = true;
    std::string err;

    // segmentation state
    uint64_t seg_index = 0;
    bool     seg_open = false;
    double   seg_start_pts_s = 0.0;
    double   last_video_pts_s = 0.0;
    int      video_track = -1;
    bool     keep_input_timestamps = false;

    static int write_cb(void* opaque, MS_AVIO_WRITE_BUF data, int size) {
        auto* self = static_cast<Impl*>(opaque);
        self->buf.insert(self->buf.end(), data, data + size);
        return size;
    }

    void fail(const std::string& m) { ok = false; if (err.empty()) err = m; }

    bool setup() {
        avformat_alloc_output_context2(&fmt, nullptr, "mp4", nullptr);
        if (!fmt) { fail("alloc_output_context2 mp4"); return false; }

        const size_t kBuf = 1 << 16;
        unsigned char* iobuf = (unsigned char*)av_malloc(kBuf);
        // Non-seekable, write-only → forces movenc into streaming (no back-seeks).
        avio = avio_alloc_context(iobuf, (int)kBuf, 1, this,
                                  nullptr, &Impl::write_cb, nullptr);
        if (!avio) { fail("avio_alloc_context"); return false; }
        avio->seekable = 0;
        fmt->pb = avio;
        fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
        // Shift timestamps so the stream starts at 0 — handles the audio
        // encoder priming delay (negative initial DTS) without an edit-list
        // conflict against the already-written moov. Not when the caller's
        // times are already the event's media time (see the header).
        fmt->avoid_negative_ts = keep_input_timestamps ? AVFMT_AVOID_NEG_TS_DISABLED
                                                       : AVFMT_AVOID_NEG_TS_MAKE_ZERO;

        stream_index.assign(tracks.size(), -1);
        for (size_t i = 0; i < tracks.size(); ++i) {
            const CmafTrack& t = tracks[i];
            AVStream* st = avformat_new_stream(fmt, nullptr);
            if (!st) { fail("new_stream"); return false; }
            AVCodecParameters* p = st->codecpar;
            p->codec_id = (AVCodecID)t.codec_id;
            if (t.kind == CmafTrack::Video) {
                p->codec_type = AVMEDIA_TYPE_VIDEO;
                p->width = t.width; p->height = t.height;
                st->time_base = {1, 90000};
                st->avg_frame_rate = {t.fps_num, t.fps_den};
                if (video_track < 0) video_track = (int)i;
            } else {
                p->codec_type = AVMEDIA_TYPE_AUDIO;
                p->sample_rate = t.sample_rate;
                p->frame_size = t.frame_size;
                av_channel_layout_default(&p->ch_layout, t.channels);
                st->time_base = {1, t.sample_rate};
            }
            if (!t.extradata.empty()) {
                p->extradata = (uint8_t*)av_mallocz(t.extradata.size() +
                                                    AV_INPUT_BUFFER_PADDING_SIZE);
                if (!p->extradata) { fail("extradata alloc"); return false; }
                memcpy(p->extradata, t.extradata.data(), t.extradata.size());
                p->extradata_size = (int)t.extradata.size();
            }
            stream_index[i] = st->index;
        }

        // CMAF-style fragmentation, caller-controlled cut points.
        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "movflags",
                    keep_input_timestamps
                        // frag_discont: the first fragment carries its real
                        // decode time (tfdt) rather than one measured from
                        // this muxer's first packet; no edit list, so nothing
                        // shifts it back.
                        ? "empty_moov+frag_custom+default_base_moof+cmaf+frag_discont"
                        : "empty_moov+frag_custom+default_base_moof+cmaf", 0);
        if (keep_input_timestamps) av_dict_set(&opts, "use_editlist", "0", 0);
        int r = avformat_write_header(fmt, &opts);
        av_dict_free(&opts);
        if (r < 0) { fail("write_header"); return false; }

        // After write_header, buf holds the init segment (ftyp+moov).
        init = buf; buf.clear();
        header_written = true;
        return true;
    }

    void flush_fragment() {
        if (!seg_open) return;
        // av_write_frame(fmt, NULL) flushes the current fragment (moof+mdat).
        av_write_frame(fmt, nullptr);
        double dur = std::max(0.0, last_video_pts_s - seg_start_pts_s);
        if (dur <= 0) dur = target_s;
        // styp first, then the moof+mdat FFmpeg produced.
        std::vector<uint8_t> seg;
        seg.reserve(buf.size() + 32);
        append_styp(seg);
        seg.insert(seg.end(), buf.begin(), buf.end());
        buf.clear();
        uint64_t idx = seg_index++;
        double start = seg_start_pts_s;
        seg_open = false;
        if (cb) cb(idx, std::move(seg), dur, start);
    }

    void push(const CmafPacket& in) {
        if (!ok || !header_written) return;
        if (in.track < 0 || in.track >= (int)stream_index.size()) return;

        bool is_video = (in.track == video_track);
        double pts_s = (double)in.pts_ns / 1e9;

        // Cut a segment at a video keyframe once we've reached the target.
        if (is_video && in.keyframe && seg_open &&
            (pts_s - seg_start_pts_s) >= target_s) {
            flush_fragment();
        }
        if (!seg_open) {
            seg_open = true;
            seg_start_pts_s = pts_s;
        }

        AVStream* st = fmt->streams[stream_index[in.track]];
        AVPacket* pkt = av_packet_alloc();
        pkt->stream_index = st->index;
        pkt->data = const_cast<uint8_t*>(in.data.data());
        pkt->size = (int)in.data.size();
        pkt->pts = av_rescale_q(in.pts_ns, NS_TB, st->time_base);
        pkt->dts = av_rescale_q(in.dts_ns, NS_TB, st->time_base);
        pkt->duration = in.duration_ns > 0
            ? av_rescale_q(in.duration_ns, NS_TB, st->time_base) : 0;
        if (in.keyframe) pkt->flags |= AV_PKT_FLAG_KEY;

        int r = av_write_frame(fmt, pkt);
        pkt->data = nullptr; pkt->size = 0; // we own the bytes
        av_packet_free(&pkt);
        if (r < 0) { fail("write_frame"); return; }

        if (is_video) last_video_pts_s = pts_s;
    }

    void finish() {
        flush_fragment();
        if (header_written && fmt) av_write_trailer(fmt); // ignore trailer bytes
    }

    ~Impl() {
        if (fmt) {
            avformat_free_context(fmt);
            fmt = nullptr;
        }
        if (avio) {
            if (avio->buffer) av_freep(&avio->buffer);
            avio_context_free(&avio);
        }
    }
};

CmafMuxer::CmafMuxer(std::vector<CmafTrack> tracks, double target_segment_s,
                     bool keep_input_timestamps)
    : d(std::make_unique<Impl>()) {
    d->tracks = std::move(tracks);
    d->target_s = target_segment_s;
    d->keep_input_timestamps = keep_input_timestamps;
    d->setup();
}
CmafMuxer::~CmafMuxer() = default;

const std::vector<uint8_t>& CmafMuxer::init_segment() const { return d->init; }
void CmafMuxer::on_segment(CmafSegmentCallback cb) { d->cb = std::move(cb); }
void CmafMuxer::push(const CmafPacket& pkt) { d->push(pkt); }
void CmafMuxer::flush() { d->finish(); }
bool CmafMuxer::ok() const { return d->ok; }
const std::string& CmafMuxer::error() const { return d->err; }

} // namespace multisite
