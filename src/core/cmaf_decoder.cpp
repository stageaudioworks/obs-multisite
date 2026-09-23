// SPDX-License-Identifier: GPL-3.0-or-later
#include "cmaf_decoder.h"
#include "mpp_decoder.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace multisite {

// Monotonic, so a clock step cannot fool the wedge watchdog below.
static int64_t mono_ms() {
    return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Each fragment is decoded as an independent, SEEKABLE in-memory unit
// (init + fragment concatenated).
//
// This matters for A/V ordering. A non-seekable stream forces FFmpeg to return
// packets in file order, and a CMAF fragment stores each track's samples
// contiguously ([all video][all audio]) — so video floods out first and audio
// only appears a whole fragment later, which OBS reports as audio lagging by
// seconds. With a seekable buffer FFmpeg can order packets by DTS, so video and
// audio interleave correctly. Fragments are self-contained (each begins with a
// keyframe), so decoding them independently is safe and bounds memory to one
// fragment at a time.
static constexpr size_t kAvioBufSize = 1 << 16;
static constexpr size_t kMaxQueuedFragments = 4;

// The MPP codec name for an FFmpeg codec id, or empty when MPP does not take
// it. The RK3588 decoder block handles H.264, HEVC, VP9 and AV1.
static std::string mpp_codec_name(AVCodecID id) {
    switch (id) {
        case AV_CODEC_ID_H264: return "h264";
        case AV_CODEC_ID_HEVC: return "hevc";
        case AV_CODEC_ID_AV1:  return "av1";
        case AV_CODEC_ID_VP9:  return "vp9";
        default:               return {};
    }
}

// Seekable read context over a fixed byte buffer.
struct MemReader {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t pos = 0;

    static int read(void* opaque, uint8_t* buf, int buf_size) {
        auto* r = static_cast<MemReader*>(opaque);
        if (r->pos >= r->size) return AVERROR_EOF;
        size_t n = std::min((size_t)buf_size, r->size - r->pos);
        std::memcpy(buf, r->data + r->pos, n);
        r->pos += n;
        return (int)n;
    }
    static int64_t seek(void* opaque, int64_t offset, int whence) {
        auto* r = static_cast<MemReader*>(opaque);
        if (whence == AVSEEK_SIZE) return (int64_t)r->size;
        int64_t np = 0;
        if (whence == SEEK_SET)      np = offset;
        else if (whence == SEEK_CUR) np = (int64_t)r->pos + offset;
        else if (whence == SEEK_END) np = (int64_t)r->size + offset;
        else return -1;
        if (np < 0 || np > (int64_t)r->size) return -1;
        r->pos = (size_t)np;
        return np;
    }
};

struct CmafDecoder::Impl {
    std::vector<uint8_t> init;

    // A queued fragment and the media segment it came from. The segment number
    // rides along so decoded frames can carry it (see DecodedVideoFrame::seq).
    struct Queued { std::vector<uint8_t> bytes; uint64_t seq = 0; };
    std::deque<Queued> frags;
    mutable std::mutex      q_mtx;
    std::condition_variable q_cv;
    std::atomic<bool>       running{false};
    std::atomic<size_t>     queued_bytes_v{0};
    // When the decode thread last produced anything. The feed loop's
    // back-pressure wait is bounded by this rather than by a wall clock, so a
    // decoder that is merely slow is never mistaken for one that is wedged —
    // and one that has genuinely stopped is never waited on for ever (bug #0).
    std::atomic<int64_t>    last_progress_ms{0};
    // How long a decoder may produce nothing, while the feed loop is waiting to
    // hand it more, before it is called wedged. A fragment decodes in well
    // under a second even in software; ten seconds is a fault, not slowness.
    static constexpr int64_t kWedgeMs = 10000;

    // How long stop() waits for the decode thread to return before abandoning
    // it. A healthy worker leaves its loop as soon as `running` goes false —
    // it only has to finish the one fragment it is inside — so this is
    // generous on purpose; it exists purely so a thread wedged INSIDE an FFmpeg
    // call cannot freeze whoever is tearing the decoder down (bug #0).
    static constexpr int64_t kStopGraceMs = 5000;
    // Overridable only by the testing hook below.
    std::atomic<int64_t>    stop_grace_ms{kStopGraceMs};
    std::atomic<bool>       worker_done{false};
    std::mutex              done_mtx;
    std::condition_variable done_cv;

    SwsContext* sws = nullptr;
    int sws_w = 0, sws_h = 0, sws_fmt = -1;
    SwrContext* swr = nullptr;

    int width = 0, height = 0, audio_tracks = 0;
    std::string video_codec;
    // The segment the decode thread is currently working on, stamped onto every
    // frame it emits.
    uint64_t cur_seq = 0;
    int decode_threads = 0;
    std::vector<std::string> prefer_video_decoders;

    VideoFrameCallback on_video;
    AudioFrameCallback on_audio;

    std::thread worker;
    bool ok_flag = true;
    std::string err;

    void fail(const std::string& m) { if (ok_flag) { ok_flag = false; err = m; } }

    bool push(std::vector<uint8_t> bytes, uint64_t seq) {
        std::unique_lock<std::mutex> lk(q_mtx);
        // Bounded on purpose (bug #0): wait while the queue is full, but only
        // as long as the decoder is still producing. A wedged decode thread
        // used to park the feed loop here for ever, with the picture frozen and
        // downloads still climbing — the exact shape of the stall.
        while (frags.size() >= kMaxQueuedFragments && running.load()) {
            q_cv.wait_for(lk, std::chrono::milliseconds(250));
            if (!running.load()) break;
            if (mono_ms() - last_progress_ms.load() > kWedgeMs) {
                fail("the decoder stopped consuming fragments");
                return false;
            }
        }
        if (!running.load()) return false;
        queued_bytes_v += bytes.size();
        frags.push_back(Queued{ std::move(bytes), seq });
        lk.unlock();
        q_cv.notify_all();
        return true;
    }

    void emit_video(AVFrame* f, AVRational tb) {
        if (!on_video) return;

        // A hardware decoder (the Pi 4's h264_v4l2m2m) may hand back a frame
        // still living in the decoder's own memory. Bring it into system
        // memory before the scaler sees it: sws_getContext cannot read a
        // hardware pixel format and would return null, which reads as "no
        // picture" rather than as anything an operator could act on.
        AVFrame* sw = nullptr;
        const AVPixFmtDescriptor* pd =
            av_pix_fmt_desc_get((AVPixelFormat)f->format);
        if (pd && (pd->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
            AVFrame* tmp = av_frame_alloc();
            if (tmp && av_hwframe_transfer_data(tmp, f, 0) >= 0) { sw = tmp; f = tmp; }
            else av_frame_free(&tmp);
        }

        auto in_fmt = (AVPixelFormat)f->format;
        if (!sws || sws_w != f->width || sws_h != f->height || sws_fmt != in_fmt) {
            if (sws) sws_freeContext(sws);
            sws = sws_getContext(f->width, f->height, in_fmt,
                                 f->width, f->height, AV_PIX_FMT_YUV420P,
                                 SWS_BILINEAR, nullptr, nullptr, nullptr);
            sws_w = f->width; sws_h = f->height; sws_fmt = in_fmt;
        }
        if (!sws) { if (sw) av_frame_free(&sw); return; }

        DecodedVideoFrame out;
        out.width = f->width; out.height = f->height;
        out.seq = cur_seq;
        out.full_range = (f->color_range == AVCOL_RANGE_JPEG);
        int lines[4];
        av_image_fill_linesizes(lines, AV_PIX_FMT_YUV420P, f->width);
        size_t total = av_image_get_buffer_size(AV_PIX_FMT_YUV420P,
                                                f->width, f->height, 1);
        out.data.resize(total);
        uint8_t* dst[4] = { nullptr, nullptr, nullptr, nullptr };
        av_image_fill_pointers(dst, AV_PIX_FMT_YUV420P, f->height,
                               out.data.data(), lines);
        sws_scale(sws, f->data, f->linesize, 0, f->height, dst, lines);
        for (int i = 0; i < 3; ++i) { out.plane[i] = dst[i]; out.stride[i] = lines[i]; }
        int64_t pts = (f->pts == AV_NOPTS_VALUE) ? 0 : f->pts;
        out.pts_ns = (int64_t)(pts * av_q2d(tb) * 1e9);
        width = f->width; height = f->height;
        on_video(out);
        if (sw) av_frame_free(&sw);
        last_progress_ms = mono_ms();
    }

    // An MPP frame arrives already I420 with its plane pointers set, so it needs
    // none of emit_video's hardware-transfer or scaling — only the bookkeeping
    // the rest of the class expects (the last-known size, and the progress mark
    // the wedge watchdog reads).
    void emit_mpp_video(const DecodedVideoFrame& f) {
        if (!on_video) return;
        width = f.width; height = f.height;
        on_video(f);
        last_progress_ms = mono_ms();
    }

    void emit_audio(AVFrame* f, AVRational tb, int track_index) {
        if (!on_audio) return;
        const int out_ch = f->ch_layout.nb_channels > 0 ? f->ch_layout.nb_channels : 2;
        if (!swr) {
            swr = swr_alloc();
            av_opt_set_chlayout(swr, "in_chlayout", &f->ch_layout, 0);
            av_opt_set_chlayout(swr, "out_chlayout", &f->ch_layout, 0);
            av_opt_set_int(swr, "in_sample_rate", f->sample_rate, 0);
            av_opt_set_int(swr, "out_sample_rate", f->sample_rate, 0);
            av_opt_set_sample_fmt(swr, "in_sample_fmt", (AVSampleFormat)f->format, 0);
            av_opt_set_sample_fmt(swr, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);
            if (swr_init(swr) < 0) { swr_free(&swr); return; }
        }
        DecodedAudioFrame out;
        out.sample_rate = f->sample_rate;
        out.channels = out_ch;
        out.track_index = track_index;
        out.seq = cur_seq;
        out.interleaved.resize((size_t)f->nb_samples * out_ch);
        uint8_t* dstp = reinterpret_cast<uint8_t*>(out.interleaved.data());
        int got = swr_convert(swr, &dstp, f->nb_samples,
                              (const uint8_t**)f->data, f->nb_samples);
        if (got <= 0) return;
        out.frames = (uint32_t)got;
        out.interleaved.resize((size_t)got * out_ch);
        int64_t pts = (f->pts == AV_NOPTS_VALUE) ? 0 : f->pts;
        out.pts_ns = (int64_t)(pts * av_q2d(tb) * 1e9);
        on_audio(out);
        last_progress_ms = mono_ms();
    }

    // Decode one fragment (init + fragment) as a seekable unit.
    void decode_unit(const std::vector<uint8_t>& frag, uint64_t seq) {
        cur_seq = seq;
        std::vector<uint8_t> unit;
        unit.reserve(init.size() + frag.size());
        unit.insert(unit.end(), init.begin(), init.end());
        unit.insert(unit.end(), frag.begin(), frag.end());

        MemReader reader{ unit.data(), unit.size(), 0 };
        unsigned char* iobuf = (unsigned char*)av_malloc(kAvioBufSize);
        if (!iobuf) { fail("av_malloc"); return; }
        AVIOContext* avio = avio_alloc_context(iobuf, (int)kAvioBufSize, 0,
                                               &reader, &MemReader::read,
                                               nullptr, &MemReader::seek);
        if (!avio) { av_free(iobuf); fail("avio_alloc_context"); return; }

        AVFormatContext* fmt = avformat_alloc_context();
        if (!fmt) { av_freep(&avio->buffer); avio_context_free(&avio);
                    fail("avformat_alloc_context"); return; }
        fmt->pb = avio;
        fmt->flags |= AVFMT_FLAG_CUSTOM_IO;

        if (avformat_open_input(&fmt, nullptr, nullptr, nullptr) < 0) {
            avio_context_free(&avio);
            fail("avformat_open_input (invalid init or fragment?)");
            return;
        }
        if (avformat_find_stream_info(fmt, nullptr) < 0) {
            avformat_close_input(&fmt);
            if (avio) { if (avio->buffer) av_freep(&avio->buffer); avio_context_free(&avio); }
            fail("avformat_find_stream_info");
            return;
        }

        // Open a decoder per stream for this unit.
        //
        // Every decoder is opened through one helper, so the thread policy
        // lives in exactly one place and the hardware preference below has a
        // single "open it; if it refuses, try the next one" step to lean on.
        std::vector<AVCodecContext*> ctxs(fmt->nb_streams, nullptr);
        std::vector<int> audio_idx(fmt->nb_streams, -1);
        int video_stream = -1, an = 0;
        // The hardware video decoder for this unit, when the board has MPP. A
        // unit is self-contained — init + fragment, keyframe first — so a fresh
        // decoder per unit is correct, and matches the FFmpeg path below, which
        // also opens per unit.
        std::unique_ptr<MppVideoDecoder> mpp;
        // AVCC -> Annex-B, for MPP only. The MP4/CMAF demuxer hands out
        // length-prefixed NAL units with the parameter sets in the init's
        // extradata; MPP's parser wants start codes. The filter also re-inserts
        // the SPS/PPS, so MPP sees a complete stream. AV1 is OBU already.
        AVBSFContext* mpp_bsf = nullptr;

        auto open_codec = [](AVCodecParameters* par,
                             const AVCodec* codec) -> AVCodecContext* {
            if (!codec) return nullptr;
            AVCodecContext* c = avcodec_alloc_context3(codec);
            if (!c) return nullptr;
            avcodec_parameters_to_context(c, par);
            // FFmpeg defaults AVCodecContext to a single thread. On a campus
            // player that is three of the Pi's four cores left idle while
            // video decode sets the pace for the whole playout clock: the
            // symptom is a few frames a second and a picture that constantly
            // falls behind. 0 means "pick a sensible number for this machine".
#if defined(MULTISITE_TSAN_BUILD)
            // Under ThreadSanitizer, decode on this thread alone.
            //
            // Not because the threading is wrong — it is FFmpeg's and it is
            // correct — but because TSan cannot see that it is. The distro's
            // libavcodec/libavutil are prebuilt without instrumentation, so
            // the atomic refcount that hands a frame buffer from FFmpeg's
            // worker to whoever unrefs it is invisible, and TSan reports the
            // handoff as a race between av_mallocz on `av:h264:df2` and our
            // av_frame_unref. It is a false positive every time.
            //
            // The alternative was a suppression for libavutil/libavcodec, and
            // that is worse: TSan matches a suppression against ANY frame in
            // either stack, so it would equally hide a real race between two
            // of OUR threads over an AVFrame — a mistake this code could
            // actually make. Removing their threads instead leaves ours fully
            // checked and suppresses nothing. Only sanitizer builds take this
            // path; shipping builds decode on every core as below.
            c->thread_count = 1;
            c->thread_type  = 0;
#else
            c->thread_count = 0;
            c->thread_type  = FF_THREAD_FRAME | FF_THREAD_SLICE;
#endif
            if (avcodec_open2(c, codec, nullptr) < 0) {
                avcodec_free_context(&c);
                return nullptr;
            }
            return c;
        };

        for (unsigned i = 0; i < fmt->nb_streams; ++i) {
            AVCodecParameters* par = fmt->streams[i]->codecpar;
            const AVCodec* dec = nullptr;

            // Hardware preference (the Pi 4's h264_v4l2m2m): tried in order,
            // before FFmpeg's own choice, and accepted only for the codec it
            // actually decodes. A name that is missing, that belongs to
            // another codec, or that refuses to open falls through to the
            // software decoder below — so a preference can change which
            // decoder runs, and never whether playback works.
            if (par->codec_type == AVMEDIA_TYPE_VIDEO && !mpp) {
                // Rockchip MPP first where the board has it. On a vendor kernel
                // the h264_v4l2m2m preference below can never open — there is no
                // V4L2 codec node, only /dev/mpp_service — so this is the only
                // hardware path there. A failure is not fatal: the FFmpeg paths
                // below then run exactly as they did.
                const std::string name = mpp_codec_name(par->codec_id);
                if (!name.empty() && mpp_decode_available()) {
                    auto d = std::make_unique<MppVideoDecoder>();
                    std::string e;
                    if (d->open(name, e)) {
                        video_codec = d->name();
                        mpp = std::move(d);
                        const char* bname = (par->codec_id == AV_CODEC_ID_H264)
                                                ? "h264_mp4toannexb"
                                                : (par->codec_id == AV_CODEC_ID_HEVC)
                                                      ? "hevc_mp4toannexb" : nullptr;
                        if (bname) {
                            const AVBitStreamFilter* f = av_bsf_get_by_name(bname);
                            if (f && av_bsf_alloc(f, &mpp_bsf) >= 0) {
                                avcodec_parameters_copy(mpp_bsf->par_in, par);
                                mpp_bsf->time_base_in = fmt->streams[i]->time_base;
                                if (av_bsf_init(mpp_bsf) < 0) av_bsf_free(&mpp_bsf);
                            }
                        }
                    }
                }
            }
            if (par->codec_type == AVMEDIA_TYPE_VIDEO && !mpp) {
                for (const std::string& name : prefer_video_decoders) {
                    const AVCodec* cand = avcodec_find_decoder_by_name(name.c_str());
                    if (!cand || cand->id != par->codec_id) continue;
                    AVCodecContext* c = open_codec(par, cand);
                    if (!c) continue;
                    ctxs[i] = c;
                    dec = cand;
                    break;
                }
            }
            if (!ctxs[i] && !(par->codec_type == AVMEDIA_TYPE_VIDEO && mpp)) {
                const AVCodec* sw = avcodec_find_decoder(par->codec_id);
                AVCodecContext* c = open_codec(par, sw);
                if (!c) continue;
                ctxs[i] = c;
                dec = sw;
            }

            if (par->codec_type == AVMEDIA_TYPE_VIDEO && video_codec.empty()) {
                // Core has no logger of its own — it is shared with the OBS
                // plugin. Record it and let the caller report it.
                video_codec = dec->name ? dec->name : "?";
                decode_threads = ctxs[i]->thread_count;
            }
            if (par->codec_type == AVMEDIA_TYPE_VIDEO && video_stream < 0) video_stream = (int)i;
            else if (par->codec_type == AVMEDIA_TYPE_AUDIO) audio_idx[i] = an++;
        }
        audio_tracks = an;

        // Read every packet in the fragment, then decode in TIMESTAMP order.
        //
        // A CMAF fragment stores each track's samples contiguously, so the
        // demuxer hands over ~a second of video before the first audio of the
        // same period. Decoding in that order makes frames emerge out of
        // presentation order, and audio then arrives late at every fragment
        // boundary (OBS reports "audio is lagging" and resets it). Sorting
        // first costs nothing extra — the fragment is already in memory — and
        // removes the skew entirely instead of papering over it downstream.
        struct Sortable { AVPacket* pkt; double t; };
        std::vector<Sortable> plist;
        {
            AVPacket* rp = av_packet_alloc();
            while (running.load() && av_read_frame(fmt, rp) >= 0) {
                AVRational tb = fmt->streams[rp->stream_index]->time_base;
                int64_t ts = (rp->dts != AV_NOPTS_VALUE) ? rp->dts : rp->pts;
                double t = (ts == AV_NOPTS_VALUE) ? 0.0 : ts * av_q2d(tb);
                AVPacket* keep = av_packet_alloc();
                av_packet_move_ref(keep, rp);
                plist.push_back({ keep, t });
            }
            av_packet_free(&rp);
        }
        std::stable_sort(plist.begin(), plist.end(),
                         [](const Sortable& a, const Sortable& b) {
                             return a.t < b.t;
                         });

        AVFrame* frm = av_frame_alloc();
        for (auto& sp : plist) {
            if (!running.load()) break;
            if (mpp && (int)sp.pkt->stream_index == video_stream) {
                // Video through MPP, audio through FFmpeg, both fed from the
                // one demuxer. The packet's own time is what MPP cannot know,
                // so it is converted here and carried onto every frame.
                AVRational tb = fmt->streams[video_stream]->time_base;
                int64_t ts = (sp.pkt->pts != AV_NOPTS_VALUE) ? sp.pkt->pts : sp.pkt->dts;
                int64_t pts_ns = (ts == AV_NOPTS_VALUE)
                                     ? 0 : (int64_t)(ts * av_q2d(tb) * 1e9);
                AVPacket* use = sp.pkt;
                AVPacket* conv = nullptr;
                if (mpp_bsf) {
                    conv = av_packet_alloc();
                    if (!conv || av_bsf_send_packet(mpp_bsf, sp.pkt) < 0
                        || av_bsf_receive_packet(mpp_bsf, conv) < 0) {
                        av_packet_free(&conv);
                        continue;   // filtered away, or the filter failed
                    }
                    use = conv;
                }
                std::vector<DecodedVideoFrame> got;
                std::string e;
                if (mpp->decode(use->data, use->size, pts_ns, seq, got, e))
                    for (auto& fr : got) emit_mpp_video(fr);
                if (conv) av_packet_free(&conv);
                continue;
            }
            AVCodecContext* c = ctxs[sp.pkt->stream_index];
            if (c && avcodec_send_packet(c, sp.pkt) >= 0) {
                while (avcodec_receive_frame(c, frm) >= 0) {
                    AVRational tb = fmt->streams[sp.pkt->stream_index]->time_base;
                    if ((int)sp.pkt->stream_index == video_stream) emit_video(frm, tb);
                    else emit_audio(frm, tb, audio_idx[sp.pkt->stream_index]);
                    av_frame_unref(frm);
                }
            }
        }
        for (auto& sp : plist) av_packet_free(&sp.pkt);
        // Flush the decoders so no frames are left behind in this unit.
        for (unsigned i = 0; i < ctxs.size() && running.load(); ++i) {
            if (!ctxs[i]) continue;
            avcodec_send_packet(ctxs[i], nullptr);
            while (avcodec_receive_frame(ctxs[i], frm) >= 0) {
                AVRational tb = fmt->streams[i]->time_base;
                if ((int)i == video_stream) emit_video(frm, tb);
                else emit_audio(frm, tb, audio_idx[i]);
                av_frame_unref(frm);
            }
        }
        // Frames still held inside MPP at the end of the unit. The FFmpeg
        // decoders were flushed above; MPP has its own end-of-stream.
        if (mpp) {
            std::vector<DecodedVideoFrame> got;
            mpp->flush(got);
            for (auto& fr : got) emit_mpp_video(fr);
        }
        if (mpp_bsf) av_bsf_free(&mpp_bsf);
        av_frame_free(&frm);
        for (auto*& c : ctxs) if (c) avcodec_free_context(&c);
        avformat_close_input(&fmt);
        if (avio) { if (avio->buffer) av_freep(&avio->buffer); avio_context_free(&avio); }
    }

    void run() {
        // Marks the thread as returned however it leaves — including by an
        // exception out of FFmpeg — so stop()'s bounded wait is never fooled
        // into thinking a finished thread is wedged.
        struct Done {
            Impl* s;
            ~Done() {
                { std::lock_guard<std::mutex> lk(s->done_mtx); s->worker_done = true; }
                s->done_cv.notify_all();
            }
        } done{ this };

        last_progress_ms = mono_ms();
        while (running.load()) {
            Queued q;
            {
                std::unique_lock<std::mutex> lk(q_mtx);
                q_cv.wait(lk, [this] { return !frags.empty() || !running.load(); });
                if (!running.load()) break;
                q = std::move(frags.front());
                frags.pop_front();
                queued_bytes_v -= std::min(queued_bytes_v.load(), q.bytes.size());
            }
            q_cv.notify_all();
            decode_unit(q.bytes, q.seq);
            // A fragment that produced no frames is still progress.
            last_progress_ms = mono_ms();
        }
    }

    ~Impl() {
        if (sws) sws_freeContext(sws);
        if (swr) swr_free(&swr);
    }
};

CmafDecoder::CmafDecoder() : d(std::make_unique<Impl>()) {}
CmafDecoder::~CmafDecoder() { stop(); }

void CmafDecoder::on_video(VideoFrameCallback cb) { d->on_video = std::move(cb); }
void CmafDecoder::on_audio(AudioFrameCallback cb) { d->on_audio = std::move(cb); }

void CmafDecoder::set_preferred_video_decoders(std::vector<std::string> names) {
    if (d) d->prefer_video_decoders = std::move(names);
}

bool CmafDecoder::start(const std::vector<uint8_t>& init_segment) {
    if (init_segment.empty()) { d->fail("empty init segment"); return false; }
    d->init = init_segment;
    d->running = true;
    d->worker = std::thread([this] { d->run(); });
    return true;
}

bool CmafDecoder::push_fragment(const std::vector<uint8_t>& bytes, uint64_t seq) {
    if (!d->running.load()) return false;
    return d->push(bytes, seq);
}

size_t CmafDecoder::queued_bytes() const { return d->queued_bytes_v.load(); }

void CmafDecoder::stop() {
    if (!d) return;
    d->running = false;
    d->q_cv.notify_all();
    if (!d->worker.joinable()) return;

    // Bounded: a decode thread wedged inside FFmpeg must not freeze whoever is
    // tearing this decoder down. push()'s wedge bound (above) stops the FEED
    // loop hanging on a full queue; this stops stop()'s join hanging on a
    // thread that will never return. Both are needed — they are different
    // threads parked in different places.
    bool stopped = false;
    {
        std::unique_lock<std::mutex> lk(d->done_mtx);
        stopped = d->done_cv.wait_for(
            lk, std::chrono::milliseconds(d->stop_grace_ms.load()),
            [this] { return d->worker_done.load(); });
    }
    if (stopped) { d->worker.join(); return; }

    d->fail("the decoder thread did not stop within 5s — abandoning it");
    // Detach and LEAK on purpose. The detached thread still holds `d`, so `d`
    // must outlive it: freeing it here would be a use-after-free in a thread we
    // can no longer control. A few hundred bytes leaked per wedge is the price
    // of not taking the process down. The caller is expected to discard this
    // decoder and build a fresh one, which is exactly what the Pi player does.
    d->worker.detach();
    (void)d.release();
}

bool CmafDecoder::ok() const { return d->ok_flag; }
const std::string& CmafDecoder::error() const { return d->err; }
void CmafDecoder::set_stop_grace_ms_for_testing(int64_t ms) {
    if (d && ms > 0) d->stop_grace_ms = ms;
}
int CmafDecoder::video_width() const { return d->width; }
int CmafDecoder::video_height() const { return d->height; }
int CmafDecoder::audio_track_count() const { return d->audio_tracks; }
std::string CmafDecoder::video_codec() const { return d->video_codec; }
int CmafDecoder::decode_threads() const { return d->decode_threads; }

} // namespace multisite
