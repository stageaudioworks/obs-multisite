// SPDX-License-Identifier: GPL-3.0-or-later
//
// packed_aac.cpp — an AAC encoder for OBS whose eight channels are OCTAGONAL,
// not 7.1.
//
// OBS's own `ffmpeg_aac` takes its channel layout from the global mix
// (obs-ffmpeg-audio-encoders.c: av_channel_layout_default(audio_output_get_channels)).
// At eight channels that is 7.1, whose fourth channel is the LFE — and AAC
// codes an LFE element with the low bands only. In packed multi-channel mode
// (PROJECT-SCOPE.md §4.3.1) the fourth channel is the click, which is
// full-range by design, so it came back as a thud (BUGS.md entry 3).
//
// There is no setting for this and no octagonal speaker enum in libobs, so the
// only place it can be fixed is an encoder that sets the codec's layout itself.
// libobs still talks to this one in 7.1 — that is the channel COUNT and the
// order of the eight planes — while the AAC bitstream declares octagonal, so
// all eight are coded full-range and the PCE says so.
//
// Positions are ignored on purpose: CONTEXT.md's packed mode treats the layout
// as a channel-count carrier, and the decoder routes by the published
// channel_labels. This encoder therefore changes nothing about channel order —
// byte for byte, plane i in is plane i out.
//
// Used only where the global mix is wider than stereo; every other layout keeps
// OBS's own encoder (see broadcast_controller.cpp).
#include "packed_aac.h"
#include "plugin_log.h"

#include <obs-module.h>
#include <media-io/audio-io.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
}

#include <cstring>
#include <vector>

namespace multisite_obs {
namespace {

struct PackedAac {
    obs_encoder_t*  encoder = nullptr;
    const AVCodec*  codec = nullptr;
    AVCodecContext* ctx = nullptr;
    AVFrame*        frame = nullptr;
    uint8_t*        samples[MAX_AV_PLANES] = {};
    // Holds the current packet until the next encode() call: libobs copies it
    // before then, which is the contract obs-ffmpeg's encoder relies on too.
    std::vector<uint8_t> packet;
    enum speaker_layout  speakers = SPEAKERS_UNKNOWN;
    int     channels = 0;
    int     sample_rate = 0;
    int     frame_size = 1024;
    size_t  plane_bytes = 0;   // one plane's worth of one frame
    int64_t total_samples = 0;
};

const char* packed_getname(void*) {
    return obs_module_text("PackedAAC");
}

void packed_destroy(void* data) {
    auto* e = static_cast<PackedAac*>(data);
    if (!e) return;
    if (e->samples[0]) av_freep(&e->samples[0]);
    if (e->frame) av_frame_free(&e->frame);
    if (e->ctx) avcodec_free_context(&e->ctx);
    delete e;
}

void* packed_create(obs_data_t* settings, obs_encoder_t* encoder) {
    auto* e = new PackedAac;
    e->encoder = encoder;
    audio_t* audio = obs_encoder_audio(encoder);
    if (!audio) {
        mlog_error("packed AAC: the encoder has no audio source");
        delete e;
        return nullptr;
    }

    const struct audio_output_info* aoi = audio_output_get_info(audio);
    e->channels    = (int)audio_output_get_channels(audio);
    e->sample_rate = (int)audio_output_get_sample_rate(audio);
    // Whatever OBS is already mixing, unchanged. Asking for a different layout
    // here would make libobs resample and remap the mix — for packed audio that
    // is how channels get summed into each other.
    e->speakers = aoi ? aoi->speakers : SPEAKERS_UNKNOWN;
    // The layout asked of libobs must describe the same number of channels the
    // mix has, or libobs would convert (and for packed audio that sums the
    // channels into each other). Unknown or mismatched falls back to the count.
    if (get_audio_channels(e->speakers) != e->channels)
        e->speakers = ms_layout_for_channels(e->channels);

    e->codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!e->codec) {
        mlog_error("packed AAC: this FFmpeg has no AAC encoder");
        delete e;
        return nullptr;
    }
    e->ctx = avcodec_alloc_context3(e->codec);
    if (!e->ctx) {
        mlog_error("packed AAC: could not allocate the codec context");
        delete e;
        return nullptr;
    }

    const int kbps = (int)obs_data_get_int(settings, "bitrate");
    e->ctx->bit_rate = (int64_t)(kbps > 0 ? kbps : 128) * 1000;
    e->ctx->sample_rate = e->sample_rate;
    e->ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    e->ctx->time_base = {1, e->sample_rate};
    // The AudioSpecificConfig must be in the container's init segment, and for
    // eight channels FFmpeg writes a PCE there rather than a one-byte config.
    e->ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    e->ctx->strict_std_compliance = -2;

    // THE POINT OF THIS ENCODER. Octagonal is eight full-range channels with
    // no LFE; 7.1 (what OBS's own encoder would pick here) has one at channel
    // 4 and AAC codes it band-limited. Every other width keeps the usual
    // arrangement, so this encoder is a drop-in wherever it is used.
    if (e->channels == 8) {
        // Spelled out rather than assigned from AV_CHANNEL_LAYOUT_OCTAGONAL:
        // that macro is a designated initializer, which is C++20 and this
        // project builds as C++17 (and MSVC would reject it).
        AVChannelLayout octagonal = {};
        octagonal.order = AV_CHANNEL_ORDER_NATIVE;
        octagonal.nb_channels = 8;
        octagonal.u.mask = AV_CH_LAYOUT_OCTAGONAL;
        av_channel_layout_copy(&e->ctx->ch_layout, &octagonal);
    } else {
        av_channel_layout_default(&e->ctx->ch_layout, e->channels);
    }

    int ret = avcodec_open2(e->ctx, e->codec, nullptr);
    if (ret < 0) {
        mlog_error("packed AAC: the AAC encoder would not open (%d)", ret);
        packed_destroy(e);
        return nullptr;
    }
    if (!e->ctx->extradata || e->ctx->extradata_size == 0)
        mlog_warn("packed AAC: the encoder produced no AudioSpecificConfig — "
                  "this track may not decode");

    e->frame = av_frame_alloc();
    if (!e->frame) {
        mlog_error("packed AAC: could not allocate a frame");
        packed_destroy(e);
        return nullptr;
    }
    e->frame->format = e->ctx->sample_fmt;
    e->frame->ch_layout = e->ctx->ch_layout;
    e->frame->sample_rate = e->ctx->sample_rate;
    e->frame_size = e->ctx->frame_size > 0 ? e->ctx->frame_size : 1024;
    e->plane_bytes = (size_t)e->frame_size * av_get_bytes_per_sample(e->ctx->sample_fmt);

    ret = av_samples_alloc(e->samples, nullptr, e->channels, e->frame_size,
                           e->ctx->sample_fmt, 0);
    if (ret < 0) {
        mlog_error("packed AAC: could not allocate the sample buffer (%d)", ret);
        packed_destroy(e);
        return nullptr;
    }

    char layout[64] = {};
    av_channel_layout_describe(&e->ctx->ch_layout, layout, sizeof layout);
    mlog_info("packed AAC: %d channels as %s (OBS mixes %d as speaker layout "
              "%d), %d kbps, %d Hz",
              e->channels, layout, e->channels, (int)e->speakers, kbps,
              e->sample_rate);
    return e;
}

bool packed_encode(void* data, struct encoder_frame* frame,
                   struct encoder_packet* packet, bool* received_packet) {
    auto* e = static_cast<PackedAac*>(data);
    for (int i = 0; i < e->channels; ++i)
        std::memcpy(e->samples[i], frame->data[i], e->plane_bytes);

    e->frame->nb_samples = e->frame_size;
    e->frame->pts = e->total_samples;
    int ret = avcodec_fill_audio_frame(
        e->frame, e->channels, e->ctx->sample_fmt, e->samples[0],
        e->frame_size * (int)e->plane_bytes * e->channels, 1);
    if (ret < 0) {
        mlog_error("packed AAC: could not fill a frame (%d)", ret);
        return false;
    }
    e->total_samples += e->frame_size;

    AVPacket pkt = {};
    ret = avcodec_send_frame(e->ctx, e->frame);
    if (ret == 0) ret = avcodec_receive_packet(e->ctx, &pkt);
    const bool got = (ret == 0);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) ret = 0;
    if (ret < 0) {
        mlog_error("packed AAC: encode failed (%d)", ret);
        return false;
    }

    *received_packet = got;
    if (!got) return true;

    e->packet.assign(pkt.data, pkt.data + pkt.size);
    // Codec time base is samples at the encoder's rate; libobs wants the
    // packet's time base stated explicitly.
    const AVRational tb = {1, e->sample_rate};
    // The cast is needed here and not in obs-ffmpeg's C: this file is C++,
    // where an enum will not take an OR of two flags implicitly.
    const auto round = (enum AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
    packet->pts = av_rescale_q_rnd(pkt.pts, e->ctx->time_base, tb, round);
    packet->dts = av_rescale_q_rnd(pkt.dts, e->ctx->time_base, tb, round);
    packet->data = e->packet.data();
    packet->size = e->packet.size();
    packet->type = OBS_ENCODER_AUDIO;
    packet->keyframe = true;
    packet->timebase_num = 1;
    packet->timebase_den = e->sample_rate;
    av_packet_unref(&pkt);
    return true;
}

bool packed_extra_data(void* data, uint8_t** extra_data, size_t* size) {
    auto* e = static_cast<PackedAac*>(data);
    if (!e->ctx->extradata || e->ctx->extradata_size <= 0) return false;
    *extra_data = e->ctx->extradata;
    *size = (size_t)e->ctx->extradata_size;
    return true;
}

void packed_audio_info(void* data, struct audio_convert_info* info) {
    auto* e = static_cast<PackedAac*>(data);
    info->format = AUDIO_FORMAT_FLOAT_PLANAR;
    info->samples_per_sec = (uint32_t)e->sample_rate;
    info->speakers = e->speakers;
}

size_t packed_frame_size(void* data) {
    return static_cast<PackedAac*>(data)->frame_size;
}

uint32_t packed_priming(void* data) {
    return static_cast<PackedAac*>(data)->ctx->initial_padding;
}

void packed_defaults(obs_data_t* settings) {
    obs_data_set_default_int(settings, "bitrate", 128);
}

// Positional rather than designated initialisation: this file is C++17, and
// designated initializers are C++20 (MSVC rejects them). The order is
// obs_encoder_info's own, required fields first.
obs_encoder_info packed_aac_info = {
    "multisite_packed_aac",   // id
    OBS_ENCODER_AUDIO,        // type
    "aac",                    // codec
    packed_getname,
    packed_create,
    packed_destroy,
    packed_encode,
    packed_frame_size,        // get_frame_size — required
    packed_defaults,          // get_defaults
    nullptr,                  // get_properties
    nullptr,                  // update
    packed_extra_data,        // get_extra_data
    nullptr,                  // get_sei_data
    packed_audio_info,        // get_audio_info
    nullptr,                  // get_video_info
    nullptr,                  // type_data
    nullptr,                  // free_type_data
    0,                        // caps
    nullptr,                  // get_defaults2
    nullptr,                  // get_properties2
    nullptr,                  // encode_texture
    nullptr,                  // encode_texture2
    packed_priming,           // get_priming_samples
};

} // namespace

void register_packed_aac() {
    obs_register_encoder(&packed_aac_info);
    // Say so once at start-up: whether an OBS log has this line is the first
    // question when a packed track's click comes back as a thud again.
    mlog_info("packed AAC encoder registered (%s)",
              "multisite_packed_aac, eight full-range channels");
}

} // namespace multisite_obs
