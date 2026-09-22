// SPDX-License-Identifier: GPL-3.0-or-later
// multisite_output.cpp — the OBS output.
//
// Pulls encoded packets from OBS (H.264 video + up to 6 AAC audio tracks),
// muxes them into CMAF fragments, and hands each finished fragment to the
// Session, which spools it durably and uploads it with retry.
//
// Nothing here blocks the OBS encode thread on the network: publish_segment()
// only writes to the local durable spool.
//
#include <obs-module.h>
#include "plugin_log.h"
#include "multisite_ui.h"
#include "storage_secondary.h"
#include "broadcast_controller.h"   // the provider choice lives in its settings
#include "reporter.h"               // the plugin's shared CloudIdentity

#include "../core/session.h"
#include "../core/cmaf_muxer.h"
#include "../core/s3_transport.h"
#include "../core/null_transport.h"
#include "../core/lan_object_server.h"
#include "../core/cloud_storage.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace multisite_obs {

using namespace multisite;

// setting keys
static constexpr char S_ENDPOINT[]  = "endpoint_host";
static constexpr char S_ACCOUNT[]   = "r2_account_id";
static constexpr char S_BUCKET[]    = "bucket";
static constexpr char S_KEYID[]     = "access_key_id";
static constexpr char S_SECRET[]    = "secret_access_key";
static constexpr char S_REGION[]    = "region";
static constexpr char S_ROOM[]      = "room_id";
static constexpr char S_SITENAME[]  = "site_name";
static constexpr char S_CACHE[]     = "cache_dir";
static constexpr char S_EVENTNAME[] = "event_name";
static constexpr char S_SEGDUR[]    = "segment_duration_s";
static constexpr char S_TRACKLBL[]  = "track_labels";   // comma-separated
static constexpr char S_TAGS[]      = "send_expiry_tag";
// The key this setting used to use. Read as a fallback so a scene saved
// with tagging switched on keeps sending the tag after the rename.
static constexpr char S_TAGS_OLD[]  = "use_object_tags";
static constexpr char S_CHANLBL[]   = "channel_labels";
// How this feed is composited, if it carries more than one picture. The
// operator has already built the scene that way; this is them saying so, so
// the satellite can pull it apart without anybody building crop filters by
// hand. Declared rather than detected: a 3840x1080 frame is a legitimate
// ultrawide picture as well as a plausible 2x1, and nothing in the video
// distinguishes them.
static constexpr char S_LAYOUT[]    = "tile_layout";
// Set only by the dock, for one Go Live, never by the settings dialog (there
// is deliberately no obs_properties_add_* for it, so it never appears as a
// persisted setting). True means the operator explicitly chose "start new"
// over a resumable event — see PROJECT-SCOPE.md §5.1.
static constexpr char S_FORCE_NEW[] = "force_new_event";
// LAN / direct delivery (PROJECT-SCOPE.md §8.7).
static constexpr char S_LAN_ENABLED[]    = "lan_enabled";
static constexpr char S_LAN_PORT[]       = "lan_port";
static constexpr char S_LAN_TOKEN[]      = "lan_auth_token";
// Off means: use a NullTransport instead of S3Transport (see
// null_transport.h) — LAN delivery keeps working unmodified either way.
// Defaults to true (see out_defaults) so a settings blob saved before this
// existed still uploads to cloud, unlike lan_enabled which defaults off.
static constexpr char S_CLOUD_ENABLED[]  = "cloud_enabled";

// A packet held while the encoder has not yet told us its codec config.
//
// Kept as raw fields rather than as a CmafPacket because the muxer track a
// packet belongs to is decided by build_tracks(), which has not run yet — that
// is the whole reason these are being held.
struct HeldPacket {
    bool     is_video = false;
    size_t   track_idx = 0;          // OBS mixer index, for audio
    int64_t  pts_ns = 0, dts_ns = 0;
    bool     keyframe = false;
    std::vector<uint8_t> data;
};

// A finished fragment on its way from the muxer to the durable spool.
struct PendingFragment {
    std::vector<uint8_t> bytes;
    double duration_s = 0.0;
    double pts_offset_s = 0.0;
};

// Exposes marker drops to the hotkeys and Tools menu while broadcasting.
struct OutputCtx : EncoderControls {
    obs_output_t* output = nullptr;
    // S3Transport when cloud delivery is on, NullTransport when it's off (see
    // null_transport.h) — held through the abstract interface because Session
    // only ever needs a Transport&, and only self_test()/base_url() (below)
    // need the concrete type, guarded by cloud_enabled at their one call site
    // each.
    std::unique_ptr<Transport> transport;
    // The second bucket's transport, when redundancy is configured (Phase 9).
    // Kept alive here because Session borrows it; null means one target.
    std::unique_ptr<Transport> mirror_transport;
    std::unique_ptr<Session>     session;
    std::unique_ptr<CmafMuxer>   muxer;
    // LAN / direct delivery (PROJECT-SCOPE.md §8.7) — null unless the
    // operator turned it on. Fed by three Session callbacks wired right
    // after session is constructed; torn down before session in out_stop so
    // a confirm callback can never fire into a server that is already gone.
    std::unique_ptr<multisite::LanObjectServer> lan_server;
    std::string lan_error;   // set if lan_server failed to bind its port

    // OBS encoder index → muxer track index
    int  video_track = -1;
    int  audio_track_for[MAX_AUDIO_MIXES];
    std::mutex mtx;                  // guards start/stop transitions

    // Packets arrive on OBS's encoder threads while stop() runs on the UI
    // thread. `accepting` gates new packets and `mux_mtx` protects the muxer
    // itself, so the muxer can never be destroyed while it's being written to.
    std::atomic<bool> accepting{false};
    std::mutex        mux_mtx;
    bool              started = false;

    // Hashing a fragment and writing ~5 MB to disk must NOT happen on OBS's
    // encoder thread (it stalls audio). Fragments are handed to this writer
    // thread instead.
    std::deque<PendingFragment> wq;
    std::mutex                  wq_mtx;
    std::condition_variable     wq_cv;
    std::thread                 writer;
    std::atomic<bool>           writer_run{false};

    // ── Encoders that only reveal their codec config once they encode ────────
    // x264 computes SPS/PPS when it is initialised, so the config is there
    // before a single frame has been sent. Apple's VideoToolbox encoders do
    // not: OBS's mac-videotoolbox fills its extra_data inside handle_keyframe,
    // which is to say on the first keyframe it actually encodes. Reading it at
    // start therefore found nothing, and every hardware encoder on a Mac
    // failed to go live at all.
    //
    // So when the config is absent at start, the start is finished later —
    // once the first video packet proves it has arrived. Packets that turn up
    // meanwhile are held here rather than dropped, because the first of them
    // is the keyframe carrying that very config and the event would be
    // undecodable without it.
    //
    // Only when absent. An encoder that answers at start takes exactly the
    // path it always did, including reporting a bad bucket before OBS says
    // it went live.
    // What the start was given, kept so it can be finished later. Not re-read
    // from the output's settings at that point: an operator editing a field
    // between Go Live and the first keyframe would otherwise change the event
    // half way through starting it.
    S3Config      pending_s3;
    SessionConfig pending_sc;
    std::string   pending_labels, pending_chan_labels, pending_layout;
    // What this machine calls itself, stamped on every cue it drops.
    std::string   pending_site_name;
    // The operator's explicit choice from the dock, for this Go Live only —
    // never persisted (see S_FORCE_NEW).
    bool          pending_force_new_event = false;
    // LAN / direct delivery settings, read once at Go Live.
    bool          pending_lan_enabled = false;
    int           pending_lan_port = 9080;
    std::string   pending_lan_token;
    // Where retained LAN objects are kept. Resolved in out_start() from the
    // same cache setting as the spool, so the two live in one place.
    std::string   pending_lan_cache_dir;
    // Off means: skip S3Transport entirely and use a NullTransport instead
    // (see null_transport.h) — every segment still flows through the same
    // spool/retry/manifest pipeline, it just never leaves this machine.
    bool          pending_cloud_enabled = true;

    std::atomic<bool>       deferred{false};     // waiting on the first keyframe
    std::atomic<bool>       complete_requested{false};
    std::atomic<bool>       completing{false};   // hand-off happens once
    std::deque<HeldPacket>  held;
    std::mutex              held_mtx;
    size_t                  held_bytes = 0;
    uint64_t                deferred_since_ms = 0;

    // diagnostics
    bool     logged_first_packets = false;
    uint64_t video_packets_seen = 0;
    uint64_t segments_muxed = 0;
    std::string last_verify_note;

    // progress logging state
    uint64_t last_logged_seq = 0;
    int64_t  last_log_ms = 0;
    LinkHealth last_health = LinkHealth::Healthy;

    // EncoderControls — driven by hotkeys and the Tools menu.
    void drop_marker(const std::string& label) override;
    void cues(std::vector<CueEntry>& out) const override;
    void log_status() override;
    EncoderStats stats() const override;
};

static std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out; std::string cur;
    for (char c : s) { if (c == ',') { out.push_back(cur); cur.clear(); } else cur += c; }
    if (!cur.empty()) out.push_back(cur);
    return out;
}
static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t"); if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t");  return s.substr(a, b - a + 1);
}

// Build muxer track configs from the OBS encoders actually attached.
static bool build_tracks(OutputCtx* ctx, std::vector<CmafTrack>& tracks,
                         VideoInfo& vinfo, std::vector<AudioTrack>& ainfo,
                         const std::string& label_csv,
                         const std::string& channel_label_csv,
                         const std::string& layout_str) {
    for (int i = 0; i < MAX_AUDIO_MIXES; ++i) ctx->audio_track_for[i] = -1;

    obs_encoder_t* venc = obs_output_get_video_encoder(ctx->output);
    if (!venc) { mlog_error("no video encoder attached"); return false; }

    CmafTrack vt;
    vt.kind = CmafTrack::Video;
    // Whatever OBS's chosen encoder produces. The pipeline is codec-agnostic:
    // the muxer writes the matching sample entry (avc1/hvc1/av01) and the
    // manifest records the codec so a satellite knows what it is receiving.
    const char* vcodec = obs_encoder_get_codec(venc);
    const std::string vc = vcodec ? vcodec : "h264";
    if      (vc == "hevc") vt.codec_id = AV_CODEC_ID_HEVC;
    else if (vc == "av1")  vt.codec_id = AV_CODEC_ID_AV1;
    else                   vt.codec_id = AV_CODEC_ID_H264;
    vt.width  = (int)obs_encoder_get_width(venc);
    vt.height = (int)obs_encoder_get_height(venc);

    // SPS/PPS extradata. This is CRITICAL: with empty_moov the codec config is
    // baked into init.mp4 before any packet arrives, so if it's missing here
    // the whole event is undecodable even though uploads succeed.
    uint8_t* hdr = nullptr; size_t hdr_size = 0;
    if (obs_encoder_get_extra_data(venc, &hdr, &hdr_size) && hdr && hdr_size) {
        vt.extradata.assign(hdr, hdr + hdr_size);
        mlog_info("video extradata: %zu bytes, starts %02x %02x %02x %02x (%s)",
                  hdr_size, hdr[0],
                  hdr_size > 1 ? hdr[1] : 0,
                  hdr_size > 2 ? hdr[2] : 0,
                  hdr_size > 3 ? hdr[3] : 0,
                  hdr[0] == 1 ? "avcC" : "Annex B");
    } else {
        mlog_error("NO VIDEO EXTRADATA available from the encoder — init.mp4 "
                   "would carry no SPS/PPS and nothing could decode the stream");
        return false;
    }

    video_t* vid = obs_output_video(ctx->output);
    if (vid) {
        const struct video_output_info* voi = video_output_get_info(vid);
        if (voi && voi->fps_den) { vt.fps_num = (int)voi->fps_num; vt.fps_den = (int)voi->fps_den; }
    }
    ctx->video_track = 0;
    tracks.push_back(vt);

    vinfo.codec  = vc;
    vinfo.width  = vt.width; vinfo.height = vt.height;
    vinfo.fps    = vt.fps_den ? (double)vt.fps_num / vt.fps_den : 0.0;
    // Parsed rather than trusted: the setting is a fixed list today, but this
    // lands in event.json for every satellite and every future version to read,
    // and TileLayout::parse falls back to one whole picture for anything it
    // does not recognise.
    vinfo.layout = TileLayout::parse(layout_str);
    if (vinfo.layout.is_split()) {
        // Worth a line in the log: it changes what the other end does with the
        // picture, and a wrong setting here is otherwise invisible from the
        // encoder side — the operator sees their own composited scene either way.
        mlog_info("video: %dx%d declared as a %s layout — the satellite will "
                  "expose %d separate sources",
                  vinfo.width, vinfo.height, vinfo.layout.to_string().c_str(),
                  vinfo.layout.count());
    }

    auto labels = split_csv(label_csv);
    int audio_n = 0;
    for (int i = 0; i < MAX_AUDIO_MIXES; ++i) {
        obs_encoder_t* aenc = obs_output_get_audio_encoder(ctx->output, (size_t)i);
        if (!aenc) continue;

        CmafTrack at;
        at.kind = CmafTrack::Audio;
        at.codec_id = AV_CODEC_ID_AAC;
        audio_t* aud = obs_encoder_audio(aenc);
        if (aud) {
            at.sample_rate = (int)audio_output_get_sample_rate(aud);
            at.channels    = (int)audio_output_get_channels(aud);
        }
        uint8_t* ah = nullptr; size_t ah_size = 0;
        if (obs_encoder_get_extra_data(aenc, &ah, &ah_size) && ah && ah_size) {
            at.extradata.assign(ah, ah + ah_size);
            mlog_info("audio track %d extradata: %zu bytes (AudioSpecificConfig)",
                      i, ah_size);
        } else {
            mlog_warn("audio track %d has no extradata — that track may not "
                      "decode", i);
        }
        at.obs_track_idx = i;
        at.label = (audio_n < (int)labels.size() && !trim(labels[audio_n]).empty())
                     ? trim(labels[audio_n])
                     : ("Track " + std::to_string(i + 1));

        ctx->audio_track_for[i] = (int)tracks.size();

        AudioTrack meta;
        meta.idx = audio_n;
        meta.label = at.label;
        meta.codec = "aac";
        meta.channels = at.channels;
        meta.sample_rate = at.sample_rate;

        // Packed multi-channel mode: publish what each channel carries, so the
        // satellite (and its de-interleaver) can route by name instead of
        // guessing. Only meaningful beyond stereo.
        if (at.channels > 2) {
            auto cl = split_csv(channel_label_csv);
            for (int c = 0; c < at.channels; ++c) {
                std::string name = (c < (int)cl.size() && !trim(cl[c]).empty())
                                     ? trim(cl[c])
                                     : ("Channel " + std::to_string(c + 1));
                meta.channel_labels.push_back(name);
            }
            mlog_info("audio track %d is PACKED: %d channels", i, at.channels);
            for (int c = 0; c < at.channels; ++c)
                mlog_info("  channel %d -> %s", c + 1,
                          meta.channel_labels[(size_t)c].c_str());
        }
        ainfo.push_back(meta);

        tracks.push_back(at);
        ++audio_n;
    }
    mlog_info("configured %d video + %d audio track(s)", 1, audio_n);

    // The packed-mode warning, and ONLY for packed mode.
    //
    // It used to fire whenever more than two channel names were filled in,
    // regardless of what was actually being sent. A six-track multi-track
    // setup — the primary mode, and correct at a stereo global layout — was
    // therefore told to switch OBS to 7.1, which is the wrong advice: it would
    // have widened every track for no reason. Channel names left over in the
    // settings are not evidence of intent.
    //
    // Packed means a track that actually carries more than two channels. That
    // is the only case where the global layout can silently destroy content.
    {
        bool packed = false;
        for (const auto& a : ainfo) if (a.channels > 2) { packed = true; break; }
        struct obs_audio_info oai = {};
        if (packed && obs_get_audio_info(&oai)) {
            const int global_ch = (int)get_audio_channels(oai.speakers);
            int widest = 0;
            for (const auto& a : ainfo) widest = std::max(widest, a.channels);
            if (widest > global_ch)
                mlog_warn("a track carries %d channels but OBS is set to %d — "
                          "the extra channels are DOWNMIXED and lost. Set "
                          "Settings -> Audio -> Channels to 7.1, or send the "
                          "channels as separate tracks instead.",
                          widest, global_ch);
        }
    }
    return true;
}

// ── EncoderControls (hotkeys / Tools menu) ───────────────────────────────────
void OutputCtx::drop_marker(const std::string& label) {
    std::lock_guard<std::mutex> lk(mtx);
    if (!session) { mlog_warn("marker '%s' ignored — not live", label.c_str()); return; }
    session->add_marker(label);
    mlog_info("marker dropped: '%s' at segment %llu", label.c_str(),
              (unsigned long long)session->status().last_enqueued + 1);
}

void OutputCtx::cues(std::vector<CueEntry>& out) const {
    std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(mtx));
    if (!session) return;
    for (const auto& m : session->markers())
        out.push_back({ m.label, m.id, m.author, (long long)m.at_ms });
}

void OutputCtx::log_status() {
    std::lock_guard<std::mutex> lk(mtx);
    if (!session) { mlog_info("encoder: idle"); return; }
    auto st = session->status();
    mlog_info("encoder: event=%s confirmed=%llu pending=%zu retries=%llu "
              "%.1f MB uploaded",
              st.event_id.c_str(),
              (unsigned long long)st.confirmed_total, st.pending,
              (unsigned long long)st.retries,
              (double)st.bytes_uploaded / (1024.0 * 1024.0));
}

EncoderStats OutputCtx::stats() const {
    EncoderStats es;
    std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(mtx));
    if (!session) return es;
    auto st = session->status();
    es.event_id  = st.event_id;
    es.confirmed = st.confirmed_total;
    es.pending   = (unsigned long long)st.pending;
    es.retries   = st.retries;
    es.bytes     = st.bytes_uploaded;
    es.link_health = st.health == LinkHealth::Healthy  ? 0
                   : st.health == LinkHealth::Degraded ? 1 : 2;
    es.last_error = session->last_error();
    es.resumed_event_id          = st.resumed_event_id;
    es.resumed_event_started_ms  = (long long)st.resumed_event_started_ms;
    es.resumed_already_confirmed = st.resumed_already_confirmed;
    es.lan_running = lan_server != nullptr;
    if (lan_server) {
        es.lan_port            = lan_server->port();
        es.lan_cached_segments = (unsigned long long)lan_server->cached_count();
        es.mirror_configured    = st.mirror_configured;
        es.mirror_behind        = st.mirror_behind;
        es.mirror_waiting_on_primary = st.mirror_waiting_on_primary;
        es.mirror_unreachable   = st.mirror_unreachable;
        es.mirror_complete      = st.mirror_complete;
    }
    es.lan_error = lan_error;
    // Colo/host/rate are S3Transport-specific (not part of the abstract
    // Transport interface) and meaningless against a NullTransport — there is
    // no remote host, so nothing to report rather than a misleading blank.
    if (auto* s3 = dynamic_cast<S3Transport*>(transport.get())) {
        es.colo               = s3->last_colo();
        es.storage_host       = s3->host();
        es.upload_bytes_per_s = s3->observed_upload_bytes_per_s();
        es.upload_samples     = s3->upload_samples();
        es.clock_skew_ms      = (long long)s3->server_clock_skew_ms();
    }
    return es;
}

// ── OBS callbacks ─────────────────────────────────────────────────────────────
static const char* out_name(void*) { return obs_module_text("Multisite.Output"); }

static void* out_create(obs_data_t*, obs_output_t* output) {
    auto* ctx = new OutputCtx();
    ctx->output = output;
    return ctx;
}
static void out_destroy(void* data) { delete static_cast<OutputCtx*>(data); }

static void out_defaults(obs_data_t* s) {
    obs_data_set_default_string(s, S_ROOM, "main-auditorium");
    obs_data_set_default_string(s, S_REGION, "auto");
    obs_data_set_default_double(s, S_SEGDUR, 6.0);
    obs_data_set_default_string(s, S_TRACKLBL, "Main mix,Sermon ISO,Click");
    obs_data_set_default_string(s, S_CHANLBL,
        "Main L,Main R,Sermon ISO,Click,Spare 5,Spare 6,Spare 7,Spare 8");
    // One picture, which is what nearly every room sends and what every event
    // written before tiling existed carries implicitly.
    obs_data_set_default_string(s, S_LAYOUT, "1x1");
    obs_data_set_default_bool(s, S_TAGS, false);
    obs_data_set_default_bool(s, S_CLOUD_ENABLED, true);
}

static obs_properties_t* out_props(void*) {
    obs_properties_t* p = obs_properties_create();
    obs_properties_add_text(p, S_ENDPOINT, obs_module_text("EndpointHost"), OBS_TEXT_DEFAULT);
    obs_properties_add_text(p, S_ACCOUNT,  obs_module_text("R2AccountID"),  OBS_TEXT_DEFAULT);
    obs_properties_add_text(p, S_BUCKET,   obs_module_text("Bucket"),       OBS_TEXT_DEFAULT);
    obs_properties_add_text(p, S_KEYID,    obs_module_text("AccessKeyID"),  OBS_TEXT_DEFAULT);
    obs_properties_add_text(p, S_SECRET,   obs_module_text("SecretKey"),    OBS_TEXT_PASSWORD);
    obs_properties_add_text(p, S_REGION,   obs_module_text("Region"),       OBS_TEXT_DEFAULT);
    obs_properties_add_text(p, S_ROOM,     obs_module_text("RoomID"),       OBS_TEXT_DEFAULT);
    obs_properties_add_float_slider(p, S_SEGDUR, obs_module_text("SegmentDuration"), 2.0, 15.0, 0.5);
    obs_properties_add_text(p, S_TRACKLBL, obs_module_text("TrackLabels"), OBS_TEXT_DEFAULT);
    // Packed multi-channel mode: what each channel of the audio track carries.
    obs_properties_add_text(p, S_CHANLBL, obs_module_text("ChannelLabels"), OBS_TEXT_DEFAULT);
    // A list rather than free text: these are the only shapes the satellite can
    // pull apart, and a typo here would be discovered at the other end of the
    // country during a service.
    {
        obs_property_t* lay = obs_properties_add_list(
            p, S_LAYOUT, obs_module_text("TileLayout"),
            OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
        obs_property_list_add_string(lay, obs_module_text("TileLayout.1x1"), "1x1");
        obs_property_list_add_string(lay, obs_module_text("TileLayout.2x1"), "2x1");
        obs_property_list_add_string(lay, obs_module_text("TileLayout.1x2"), "1x2");
        obs_property_list_add_string(lay, obs_module_text("TileLayout.2x2"), "2x2");
        obs_property_set_long_description(lay, obs_module_text("TileLayout.Help"));
    }
    // R2 rejects x-amz-tagging; leave off unless the store supports tagging.
    {
        obs_property_t* tg =
            obs_properties_add_bool(p, S_TAGS, obs_module_text("SendExpiryTag"));
        obs_property_set_long_description(tg,
                                          obs_module_text("SendExpiryTagHint"));
    }
    return p;
}

// Drains finished fragments into the durable spool, off the encoder thread.
// Defined below, next to out_start, because that is the other place it runs
// from and reading the two together is the point.
static bool complete_start(OutputCtx* ctx);

// Finish a start that was waiting on the encoder's codec config, then let
// through everything held while it waited. Runs on the writer thread.
static void finish_deferred_start(OutputCtx* ctx) {
    // out_stop clears `accepting` before it does anything else, so this is how
    // a stop that arrives first is noticed. Without it, clicking Stop in the
    // moment before the first keyframe would still create an event in the
    // bucket — and then immediately end it.
    if (!ctx->accepting.load()) {
        mlog_info("stopped before the encoder produced a codec config — "
                  "no event was started");
        ctx->deferred = false;
        return;
    }

    if (!complete_start(ctx)) {
        mlog_error("could not start the event once the codec config arrived");
        if (ctx->accepting.exchange(false))
            obs_output_signal_stop(ctx->output, OBS_OUTPUT_ERROR);
        return;
    }

    // Order matters: the muxer must be reachable by out_packet only after
    // everything held ahead of those packets has gone through it, or the
    // event starts part way in — missing the keyframe that carries the very
    // config this was all waiting for.
    std::deque<HeldPacket> held;
    {
        std::lock_guard<std::mutex> lk(ctx->held_mtx);
        held.swap(ctx->held);
        ctx->held_bytes = 0;
    }
    // Checked again: start_new() talks to the bucket and can take a while, and
    // a stop may have begun during it. Pushing into a muxer out_stop has
    // already flushed and reset is the thing to avoid.
    if (!ctx->accepting.load()) { ctx->deferred = false; return; }

    size_t pushed = 0, dropped = 0;
    for (auto& h : held) {
        int track = h.is_video ? ctx->video_track
                  : (h.track_idx < MAX_AUDIO_MIXES
                         ? ctx->audio_track_for[h.track_idx] : -1);
        if (track < 0) { ++dropped; continue; }   // a track nobody asked for
        CmafPacket cp;
        cp.track    = track;
        cp.data     = std::move(h.data);
        cp.pts_ns   = h.pts_ns;
        cp.dts_ns   = h.dts_ns;
        cp.keyframe = h.keyframe;
        {
            std::lock_guard<std::mutex> mlk(ctx->mux_mtx);
            if (!ctx->muxer) break;
            ctx->muxer->push(cp);
        }
        ++pushed;
    }

    // Cleared last. Until it is, out_packet is still holding rather than
    // muxing, which is what keeps the drain above in front of live packets.
    ctx->deferred = false;
    mlog_info("multisite output started — room=%s event=%s "
              "(%zu held packet%s released%s)",
              ctx->pending_sc.room_id.c_str(),
              ctx->session ? ctx->session->event_id().c_str() : "?",
              pushed, pushed == 1 ? "" : "s",
              dropped ? " — some for tracks not being sent" : "");
}

static void writer_loop(OutputCtx* ctx) {
    for (;;) {
        PendingFragment f;
        bool complete_now = false;
        {
            std::unique_lock<std::mutex> lk(ctx->wq_mtx);
            ctx->wq_cv.wait(lk, [ctx] {
                return !ctx->wq.empty() || !ctx->writer_run.load() ||
                       ctx->complete_requested.load();
            });
            if (ctx->complete_requested.exchange(false)) {
                complete_now = true;
            } else if (ctx->wq.empty()) {
                if (!ctx->writer_run.load()) return;   // asked to stop, nothing left
                continue;
            } else {
                f = std::move(ctx->wq.front());
                ctx->wq.pop_front();
            }
        }
        if (complete_now) { finish_deferred_start(ctx); continue; }
        // Checksum + durable write happen here, safely away from OBS threads.
        uint64_t seq = ctx->session->publish_segment(std::move(f.bytes),
                                                     f.duration_s, f.pts_offset_s);
        ctx->segments_muxed++;
        mlog_debug("segment %llu spooled (%.1fs)",
                   (unsigned long long)seq, f.duration_s);
    }
}

// Wait for queued fragments to reach the spool (bounded), then stop the writer.
static void writer_shutdown(OutputCtx* ctx, int timeout_ms = 10000) {
    if (!ctx->writer_run.load()) return;
    for (int waited = 0; waited < timeout_ms; waited += 25) {
        {
            std::lock_guard<std::mutex> lk(ctx->wq_mtx);
            if (ctx->wq.empty()) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    ctx->writer_run = false;
    ctx->wq_cv.notify_all();
    if (ctx->writer.joinable()) ctx->writer.join();
}

// Everything the start can only do once the encoder has told us its codec
// config: build the tracks, build the muxer, and write event.json and init.mp4.
//
// Split out because it runs from one of two places. Normally straight from
// out_start, on the thread that called it, exactly as it always has. When the
// encoder has no config to give yet, from the writer thread instead, once the
// first video packet proves it does — which keeps the bucket writes off OBS's
// encode thread, the rule this file opens by stating.
static bool complete_start(OutputCtx* ctx) {
    std::vector<CmafTrack> tracks;
    VideoInfo vinfo;
    std::vector<AudioTrack> ainfo;
    if (!build_tracks(ctx, tracks, vinfo, ainfo, ctx->pending_labels,
                      ctx->pending_chan_labels, ctx->pending_layout))
        return false;

    // Under the lock, because this can now run on the writer thread while
    // out_stop resets the same pointer from OBS's UI thread. Two unassisted
    // unique_ptr writes to one pointer is a data race and, with the wrong
    // interleaving, a double free.
    {
        auto m = std::make_unique<CmafMuxer>(tracks,
                                             ctx->pending_sc.segment_duration_s);
        if (!m->ok()) {
            mlog_error("muxer init failed: %s", m->error().c_str());
            return false;
        }
        std::lock_guard<std::mutex> mlk(ctx->mux_mtx);
        ctx->muxer = std::move(m);
    }

    mlog_info("init segment: %zu bytes (carries the codec config for the event)",
              ctx->muxer->init_segment().size());
    if (ctx->muxer->init_segment().size() < 200)
        mlog_error("init segment looks too small — codec config is probably "
                   "missing, so decoders will reject the stream");

    if (ctx->pending_cloud_enabled) {
        // Two producers of the cloud leg, and nothing downstream learns which
        // (Phase 12). PAIRED: the collector minted credentials, so the bucket,
        // endpoint and keys all come from the shared identity — an encoder
        // writes, which the role says. DIRECT: the operator typed keys, exactly
        // as before. The operator's choice of provider decides, so a box with
        // typed keys that also heartbeats is NOT switched onto brokered storage.
        const BroadcastSettings bcfg =
            BroadcastController::instance().settings_copy();
        auto identity = reporter_cloud_identity();
        const bool use_paired =
            bcfg.storage_provider == "multisite_cloud" && identity &&
            identity->paired() && identity->credentials().present();

        if (use_paired) {
            multisite::CloudStorageConfig csc;
            csc.region = bcfg.region;
            auto ct = std::make_unique<multisite::CloudTransport>(
                *identity, multisite::CloudRole::Encoder, csc);
            mlog_info("storage: Multisite Cloud — bucket '%s'",
                      ct->bucket().c_str());
            ctx->transport = std::move(ct);
        } else if (bcfg.storage_provider == "multisite_cloud") {
            // Chosen, but not usable yet: no pairing, or no credentials fetched.
            // Refuse plainly rather than falling back to typed keys the operator
            // did not choose — silence here is how a box reads the wrong bucket.
            mlog_error("storage is set to Multisite Cloud but the machine is "
                       "not paired yet (or has no credentials) — pair it in "
                       "the Cloud section, or choose a different provider");
            return false;
        } else {
            auto s3 = std::make_unique<S3Transport>(ctx->pending_s3);
            // Report the URL actually in use: a mistyped endpoint is otherwise
            // only visible as curl's opaque "bad/illegal format" error.
            mlog_info("storage: %s", s3->base_url().c_str());
            ctx->transport = std::move(s3);
        }

        // The second bucket (PROJECT-SCOPE.md §10 Phase 9). Machine-wide
        // rather than part of this event's settings — see storage_secondary.h.
        // Only with cloud delivery: with it off nothing leaves the machine, so
        // there is nothing to mirror.
        const SecondaryTarget second = secondary_target();
        if (second.configured()) {
            S3Config sc2;
            secondary_s3_config(sc2);
            auto s3b = std::make_unique<S3Transport>(sc2);
            mlog_info("second bucket: %s", s3b->base_url().c_str());
            ctx->mirror_transport = std::move(s3b);
            ctx->pending_sc.mirror_transport = ctx->mirror_transport.get();
        } else if (second.enabled) {
            // Ticked but not finished. Say so plainly rather than mirroring to
            // a half-typed target or silently doing nothing.
            mlog_warn("second bucket is enabled but not complete — nothing is "
                      "being mirrored (fill in bucket, endpoint and keys)");
        }
    } else {
        mlog_info("cloud delivery is disabled for this event — publishing "
                  "to the LAN cache only, nothing leaves this machine");
        ctx->transport = std::make_unique<multisite::NullTransport>();
    }
    ctx->session   = std::make_unique<Session>(ctx->pending_sc, *ctx->transport);
    // Who this machine is, for the cues it drops — the same field a satellite
    // sets (see DecoderSettings), and empty reads as the main site.
    ctx->session->set_author_name(ctx->pending_site_name);

    // LAN / direct delivery (PROJECT-SCOPE.md §8.7) — wired before the
    // resume/start_new call below, because begin_common() fires the
    // event-started hook synchronously and a satellite must be able to
    // bootstrap from the very first event this Session ever publishes, not
    // just ones that start after this point.
    ctx->lan_error.clear();
    if (ctx->pending_lan_enabled) {
        multisite::LanServerConfig lan_cfg;
        lan_cfg.port = ctx->pending_lan_port;
        lan_cfg.auth_token = ctx->pending_lan_token;
        lan_cfg.room_id = ctx->pending_sc.room_id;
        // Resolved in out_start() from the same cache setting as the spool.
        std::string lan_cache_dir = ctx->pending_lan_cache_dir.empty()
                                        ? "./multisite_lan_cache"
                                        : ctx->pending_lan_cache_dir;
        ctx->lan_server = std::make_unique<multisite::LanObjectServer>(
            lan_cfg, lan_cache_dir);
        std::string lan_err;
        if (ctx->lan_server->start(lan_err)) {
            mlog_info("LAN delivery: listening on port %d%s",
                      ctx->lan_server->port(),
                      ctx->pending_lan_token.empty() ? " (no auth token set)" : "");
            // Raw pointers, not shared_ptr: ctx outlives Session (it is torn
            // down first in out_stop, below), and lan_server is torn down
            // after session in the same place — so by the time either
            // callback could fire, both are still valid, and once session is
            // gone, it can never call back at all.
            OutputCtx* raw = ctx;
            ctx->session->set_event_started_callback(
                [raw](const std::string& id, const std::string& json,
                      const std::vector<uint8_t>& init) {
                    if (raw->lan_server) raw->lan_server->on_event_started(id, json, init);
                });
            ctx->session->set_segment_confirmed_callback(
                [raw](uint64_t seq, const std::vector<uint8_t>& bytes) {
                    if (raw->lan_server) raw->lan_server->on_segment_confirmed(seq, bytes);
                });
            ctx->session->set_manifest_published_callback(
                [raw](const std::string& json) {
                    if (raw->lan_server) raw->lan_server->on_manifest_published(json);
                });
            // Without this, a LAN-only satellite (cloud_enabled off) has no
            // way to discover which event is live at all: live.json is the
            // one object none of the other three hooks cover.
            ctx->session->set_live_published_callback(
                [raw](const std::string& json) {
                    if (raw->lan_server) raw->lan_server->on_live_published(json);
                });
            // Without this, a marker dropped mid-event never reaches a
            // LAN-only satellite at all — there is no cloud copy of
            // markers.json to fall back to for one.
            ctx->session->set_markers_published_callback(
                [raw](const std::string& json) {
                    if (raw->lan_server) raw->lan_server->on_markers_published(json);
                });
            // The LAN cue hub: a satellite with no bucket hands its cue here,
            // and the encoder writes it under that site's own name (see
            // Session::add_cue_from) — so the satellite stays read-only and
            // every site still sees the cue. The Session pointer is taken
            // under the ctx lock and used without it: add_cue_from does
            // network I/O, and holding the lock across that would stall the UI
            // thread's stats() and drop_marker().
            ctx->lan_server->set_cue_callback(
                [raw](const std::string& author, const std::string& label,
                      std::string& error) -> bool {
                    Session* s = nullptr;
                    { std::lock_guard<std::mutex> lk(raw->mtx); s = raw->session.get(); }
                    if (!s) { error = "the event is not live"; return false; }
                    return s->add_cue_from(author, label, error);
                });
        } else {
            // Cloud upload is completely unaffected by this failing — LAN
            // delivery is a second path to the same objects, never a
            // precondition for the first. Reported to the dock, not fatal.
            mlog_error("LAN delivery failed to start on port %d: %s "
                       "(cloud upload continues normally)",
                       ctx->pending_lan_port, lan_err.c_str());
            ctx->lan_error = lan_err;
            ctx->lan_server.reset();
        }
    }

    // Resume an interrupted event unless the dock explicitly asked for a new
    // one. The dock is what decides THAT — checking staleness and asking the
    // operator when it matters (PROJECT-SCOPE.md §5.1) — before Go Live ever
    // gets this far; this only has to obey the one flag it was given.
    auto resume = ctx->session->check_resumable();
    bool ok;
    if (resume.resumable && !ctx->pending_force_new_event) {
        mlog_info("resuming interrupted event %s (%zu segments pending)",
                  resume.event_id.c_str(), resume.pending_count);
        ok = ctx->session->resume(ctx->muxer->init_segment(), vinfo, ainfo);
    } else {
        if (resume.resumable)
            mlog_info("operator chose to start a new event over the "
                       "interrupted one (%s, %zu segments were pending)",
                       resume.event_id.c_str(), resume.pending_count);
        ok = ctx->session->start_new(ctx->muxer->init_segment(), vinfo, ainfo);
    }
    if (!ok) {
        // Surface the actual HTTP failure rather than guessing.
        mlog_error("failed to start session: %s",
                   ctx->session->last_error().empty()
                       ? "no error recorded"
                       : ctx->session->last_error().c_str());
        // Probe the bucket so the operator learns whether it's credentials,
        // permissions, endpoint, or something request-specific. Only
        // meaningful against a real S3Transport — a NullTransport's put()
        // cannot fail, so start_new()/resume() failing here is never about
        // storage in the first place (cloud is disabled), and there is
        // nothing to probe.
        if (auto* s3 = dynamic_cast<S3Transport*>(ctx->transport.get())) {
            std::string probe = s3->self_test();
            if (probe.empty())
                mlog_error("connectivity probe SUCCEEDED — credentials and bucket are "
                           "fine, so the failure is request-specific (see above)");
            else
                mlog_error("connectivity probe also failed: %s", probe.c_str());
        }
        return false;
    }

    // Log upload progress so the operator can see it working without going
    // and inspecting the bucket. Chatty for the first few segments (so you
    // get quick confirmation), then once every ~30s.
    ctx->session->set_progress_callback([ctx](const Session::Status& st) {
        int64_t now = now_ms();
        bool early   = st.confirmed_total <= 3;
        bool overdue = (now - ctx->last_log_ms) > 30000;
        bool health_changed = st.health != ctx->last_health;

        if (early || overdue || health_changed) {
            const char* h = st.health == LinkHealth::Healthy  ? "healthy"
                          : st.health == LinkHealth::Degraded ? "DEGRADED"
                                                              : "OFFLINE";
            mlog_info("uploaded segment %llu — %llu confirmed, %.1f MB, "
                      "%zu queued, %llu retries, link %s",
                      (unsigned long long)st.last_confirmed,
                      (unsigned long long)st.confirmed_total,
                      (double)st.bytes_uploaded / (1024.0 * 1024.0),
                      st.pending,
                      (unsigned long long)st.retries, h);
            ctx->last_log_ms = now;
            ctx->last_health = st.health;
        }

        // Report the store-side verification of early uploads. A store that
        // returns success without persisting is otherwise invisible.
        //
        // "verified where it was sent" rather than "in bucket": with cloud
        // delivery off the store is the LAN cache, and a log line claiming a
        // bucket that never saw the bytes is how somebody concludes their event
        // was archived when it is not.
        if (!st.verify_note.empty() && st.verify_note != ctx->last_verify_note) {
            ctx->last_verify_note = st.verify_note;
            if (st.verify_failures > 0)
                mlog_error("UPLOAD VERIFICATION FAILED: %s", st.verify_note.c_str());
            else
                mlog_info("upload verified where it was sent: %s",
                          st.verify_note.c_str());
        }
        // Always warn when the link degrades — that's the thing an operator
        // must know about mid-event.
        if (health_changed && st.health != LinkHealth::Healthy) {
            mlog_warn("upload link %s — capture continues, segments are "
                      "queued to disk and will be sent when it recovers",
                      st.health == LinkHealth::Degraded ? "degraded" : "offline");
        }
    });

    // Hand finished fragments to the writer thread. This callback runs on an
    // OBS encoder thread, so it must stay cheap — no hashing, no disk I/O.
    ctx->muxer->on_segment([ctx](uint64_t, std::vector<uint8_t> bytes,
                                 double dur, double pts) {
        PendingFragment f;
        f.bytes = std::move(bytes);
        f.duration_s = dur;
        f.pts_offset_s = pts;
        {
            std::lock_guard<std::mutex> lk(ctx->wq_mtx);
            ctx->wq.push_back(std::move(f));
        }
        ctx->wq_cv.notify_one();
    });

    return true;
}

static bool out_start(void* data) {
    auto* ctx = static_cast<OutputCtx*>(data);
    // unique_lock, not lock_guard, because the registry has to be joined AFTER
    // this is released — see the lock-order note above out_stop.
    std::unique_lock<std::mutex> lk(ctx->mtx);

    obs_data_t* s = obs_output_get_settings(ctx->output);
    S3Config s3;
    s3.endpoint_host     = obs_data_get_string(s, S_ENDPOINT);
    s3.r2_account_id     = obs_data_get_string(s, S_ACCOUNT);
    s3.bucket            = obs_data_get_string(s, S_BUCKET);
    s3.access_key_id     = obs_data_get_string(s, S_KEYID);
    s3.secret_access_key = obs_data_get_string(s, S_SECRET);
    s3.region            = obs_data_get_string(s, S_REGION);

    SessionConfig sc;
    sc.room_id            = obs_data_get_string(s, S_ROOM);
    ctx->pending_site_name = obs_data_get_string(s, S_SITENAME);
    sc.event_name         = obs_data_get_string(s, S_EVENTNAME);
    sc.segment_duration_s = obs_data_get_double(s, S_SEGDUR);
    std::string labels     = obs_data_get_string(s, S_TRACKLBL);
    std::string chan_labels = obs_data_get_string(s, S_CHANLBL);
    sc.send_expiry_tag     = obs_data_get_bool(s, S_TAGS) ||
                             obs_data_get_bool(s, S_TAGS_OLD);
    // Read before the release below, not after it. This was being read from
    // `s` further down, past the point where our reference had been given up.
    const std::string layout_str = obs_data_get_string(s, S_LAYOUT);
    const bool force_new_event = obs_data_get_bool(s, S_FORCE_NEW);
    const bool lan_enabled = obs_data_get_bool(s, S_LAN_ENABLED);
    const int  lan_port    = (int)obs_data_get_int(s, S_LAN_PORT);
    const std::string lan_token = obs_data_get_string(s, S_LAN_TOKEN);
    const bool cloud_enabled = obs_data_get_bool(s, S_CLOUD_ENABLED);
    obs_data_release(s);

    if (cloud_enabled) {
        if (s3.bucket.empty() ||
            (s3.endpoint_host.empty() && s3.r2_account_id.empty())) {
            mlog_error("storage not configured (need bucket + endpoint or account id)");
            return false;
        }
    } else if (!lan_enabled) {
        mlog_error("cloud delivery is disabled and LAN delivery is off — "
                  "nothing would be delivered anywhere");
        return false;
    }

    // Durable spool: where an operator chose, else beside OBS's own config.
    // The controller resolves its resume peek from the same setting, so the two
    // cannot disagree about where the queue is.
    const std::string cache_override = obs_data_get_string(s, S_CACHE);
    if (!cache_override.empty()) {
        sc.spool_dir = cache_override;
    } else {
        char* cfgdir = obs_module_config_path("spool");
        sc.spool_dir = cfgdir ? cfgdir : "./multisite_spool";
        bfree(cfgdir);
    }

    // Retained LAN objects go UNDER the chosen cache folder rather than beside
    // OBS's config. An operator who has told us where video goes should not
    // then have to hunt for a second folder when the disk fills; the default
    // (nothing configured) stays where it has always been.
    if (!cache_override.empty()) {
        std::string root = cache_override;
        while (!root.empty() && (root.back() == '/' || root.back() == '\\'))
            root.pop_back();
        ctx->pending_lan_cache_dir = root + "/lan_cache";
    } else {
        char* lan_dir = obs_module_config_path("lan_cache");
        ctx->pending_lan_cache_dir = lan_dir ? lan_dir : "./multisite_lan_cache";
        bfree(lan_dir);
    }

    if (!obs_output_can_begin_data_capture(ctx->output, 0)) return false;
    if (!obs_output_initialize_encoders(ctx->output, 0))     return false;

    ctx->pending_s3          = s3;
    ctx->pending_sc          = sc;
    ctx->pending_labels      = labels;
    ctx->pending_chan_labels = chan_labels;
    ctx->pending_layout      = layout_str;
    ctx->pending_force_new_event = force_new_event;
    ctx->pending_lan_enabled = lan_enabled;
    ctx->pending_lan_port    = lan_port;
    ctx->pending_lan_token   = lan_token;
    ctx->pending_cloud_enabled = cloud_enabled;

    // The writer thread first: in the deferred case it is what finishes the
    // start, so it has to be running before any packet can ask it to.
    ctx->writer_run = true;
    ctx->writer = std::thread(writer_loop, ctx);

    // Has the encoder told us its codec config yet?
    //
    // x264 has: it computes SPS/PPS when initialised. Apple's VideoToolbox
    // encoders have not, and cannot — OBS's mac-videotoolbox fills its
    // extra_data on the first keyframe it encodes, which has not happened,
    // because capture has not begun. Asking again later is the only way.
    bool have_config = false;
    if (obs_encoder_t* venc = obs_output_get_video_encoder(ctx->output)) {
        uint8_t* hdr = nullptr; size_t hdr_size = 0;
        have_config = obs_encoder_get_extra_data(venc, &hdr, &hdr_size) &&
                      hdr && hdr_size;
    }

    if (have_config) {
        // The path every encoder that works today already takes, unchanged —
        // including refusing to start at all when the bucket is wrong, which
        // is worth keeping: an operator finds out before OBS says they are on.
        if (!complete_start(ctx)) { writer_shutdown(ctx); return false; }
    } else {
        const char* id = "?";
        if (obs_encoder_t* venc = obs_output_get_video_encoder(ctx->output))
            if (const char* n = obs_encoder_get_id(venc)) id = n;
        mlog_info("%s has no codec config yet — finishing the start on its "
                  "first keyframe, holding packets until then", id);
        ctx->deferred          = true;
        ctx->deferred_since_ms = (uint64_t)now_ms();
    }

    if (!obs_output_begin_data_capture(ctx->output, 0)) {
        writer_shutdown(ctx);
        return false;
    }
    ctx->started = true;
    ctx->accepting = true;      // packets may now enter the muxer, or be held

    const std::string room = sc.room_id;
    const std::string ev   = ctx->session ? ctx->session->event_id() : std::string();
    const bool deferred    = ctx->deferred.load();
    lk.unlock();                // ── ctx->mtx released ───────────────────────

    // Only now. Registering while holding ctx->mtx is the inverted order that
    // deadlocked the quit: a status poll holds the registry and wants ctx->mtx.
    register_encoder_controls(ctx);   // hotkeys can now drop markers
    if (!deferred)
        mlog_info("multisite output started — room=%s event=%s",
                  room.c_str(), ev.c_str());
    return true;
}

// ── Lock order: the controls registry BEFORE ctx->mtx, never the other way ───
//
// encoder_stats() holds the registry lock while calling through the pointer it
// protects — it has to, or the OutputCtx could be destroyed mid-call — and
// OutputCtx::stats() takes ctx->mtx. So that direction is fixed, and everything
// else has to agree with it.
//
// out_stop used to take ctx->mtx and then unregister, which is the opposite
// order, and the two deadlocked: OBS's UI thread sat in out_stop holding
// ctx->mtx waiting for the registry, while the web poll thread sat in stats()
// holding the registry waiting for ctx->mtx. OBS then never finished quitting,
// which reads as a crash once somebody force-quits it — and OBS reports it as
// one on the next launch.
static void out_stop(void* data, uint64_t) {
    auto* ctx = static_cast<OutputCtx*>(data);

    // Outside the lock, and first: nothing else may reach this output through
    // the registry once it is stopping, and taking it here is what keeps the
    // order above true.
    unregister_encoder_controls(ctx);

    std::lock_guard<std::mutex> lk(ctx->mtx);
    if (!ctx->started) return;

    // ORDER MATTERS. Packets arrive on OBS's encoder threads; stop runs on the
    // caller's thread. Close the gate first, let OBS stop delivering, and only
    // then touch the muxer — otherwise the muxer can be destroyed mid-write
    // (a use-after-free that crashes inside avformat).
    ctx->accepting = false;
    obs_output_end_data_capture(ctx->output);

    {
        std::lock_guard<std::mutex> mlk(ctx->mux_mtx);
        if (ctx->muxer) ctx->muxer->flush();   // emits the final fragment
        ctx->muxer.reset();                    // safe: no packet can be inside
    }

    // Make sure queued fragments reach the durable spool before draining.
    writer_shutdown(ctx);

    if (ctx->session) {
        auto before = ctx->session->status();
        if (before.pending)
            mlog_info("draining %zu queued segment(s) before stopping",
                      before.pending);

        const bool marked_ended = ctx->session->end();   // drains spool, marks ended

        // Reported AFTER the drain. The old order printed the queue depth as
        // it stood before end() ran, so a clean shutdown that uploaded its
        // last fragments still signed off with "2 pending" — which reads as
        // two segments lost when nothing had been.
        auto st = ctx->session->status();
        mlog_info("stopped: %llu confirmed, %llu retries, %llu segments muxed",
                  (unsigned long long)st.confirmed_total,
                  (unsigned long long)st.retries,
                  (unsigned long long)ctx->segments_muxed);
        if (st.pending)
            mlog_warn("%zu segment(s) were still unsent when the drain "
                      "deadline passed — they remain in the spool and will be "
                      "uploaded if this event is resumed", st.pending);
        // Worth its own line, and a loud one: the segments are recoverable,
        // this is not. A satellite learns an event is over by reading
        // live.json, so an event that stopped without being marked ended
        // leaves every campus polling a room nobody is broadcasting to until
        // it gives up and calls the encoder dead.
        if (!marked_ended)
            mlog_error("the event was NOT marked ended in storage (%s) — "
                       "campuses will keep polling this room and eventually "
                       "report it as interrupted rather than finished",
                       ctx->session->last_error().c_str());
        if (ctx->segments_muxed == 0)
            mlog_warn("no segments were produced — check that the video "
                      "encoder's keyframe interval is <= the segment duration");
    }
// controls were unregistered at the top, before ctx->mtx was taken
    ctx->started = false;
    // session first: its destructor stops the uploader thread and guarantees
    // no more confirm/manifest callbacks can fire, so lan_server (which those
    // callbacks reach through ctx) is safe to tear down right after it.
    ctx->session.reset();
    ctx->lan_server.reset();
    ctx->muxer.reset(); ctx->transport.reset();
}

// Lets OBS (and scripts via obs_output_get_total_bytes) show upload volume.
static uint64_t out_total_bytes(void* data) {
    auto* ctx = static_cast<OutputCtx*>(data);
    return (ctx && ctx->session) ? ctx->session->bytes_uploaded() : 0;
}

// OBS timestamps are in the ENCODER's timebase (e.g. 1/30 for 30fps video,
// 1/48000 for audio) — NOT nanoseconds. Convert explicitly; getting this wrong
// silently breaks segmentation, because the muxer's "have we reached the target
// duration?" test never becomes true.
static inline int64_t ts_to_ns(int64_t ts, int32_t tb_num, int32_t tb_den) {
    if (tb_den <= 0) tb_den = 1;
    if (tb_num <= 0) tb_num = 1;
    // ts * (tb_num / tb_den) seconds → nanoseconds. Split the multiply to
    // avoid overflow without needing 128-bit math (MSVC has no __int128).
    const int64_t ns_per_unit = 1000000000LL * (int64_t)tb_num / (int64_t)tb_den;
    const int64_t rem         = 1000000000LL * (int64_t)tb_num % (int64_t)tb_den;
    return ts * ns_per_unit + (ts * rem) / (int64_t)tb_den;
}

// Does this packet mean the encoder can now tell us its codec config?
static bool h_is_video_and_config_ready(OutputCtx* ctx,
                                        const struct encoder_packet* pkt) {
    if (pkt->type != OBS_ENCODER_VIDEO) return false;
    obs_encoder_t* venc = obs_output_get_video_encoder(ctx->output);
    if (!venc) return false;
    uint8_t* hdr = nullptr; size_t hdr_size = 0;
    return obs_encoder_get_extra_data(venc, &hdr, &hdr_size) && hdr && hdr_size;
}

// How much may be held while waiting for a codec config, before giving up.
//
// A keyframe is due immediately — encoders emit one first — so this should hold
// a handful of packets for a fraction of a second. These bounds exist for the
// case where it never arrives at all, so that a stuck encoder ends as a stopped
// output with a reason rather than as memory climbing until something dies.
static constexpr size_t kMaxHeldPackets = 900;      // ~30s of 30fps video
static constexpr size_t kMaxHeldBytes   = 64u << 20;
static constexpr int64_t kMaxHeldMs     = 15000;

static void out_packet(void* data, struct encoder_packet* pkt) {
    auto* ctx = static_cast<OutputCtx*>(data);
    // Cheap gate first: once stop() begins, packets are dropped immediately.
    if (!ctx || !pkt || !ctx->accepting.load()) return;

    // ── Still waiting for the encoder's codec config ─────────────────────────
    if (ctx->deferred.load()) {
        HeldPacket h;
        h.is_video  = (pkt->type == OBS_ENCODER_VIDEO);
        h.track_idx = pkt->track_idx;
        h.pts_ns    = ts_to_ns(pkt->pts, pkt->timebase_num, pkt->timebase_den);
        h.dts_ns    = ts_to_ns(pkt->dts, pkt->timebase_num, pkt->timebase_den);
        h.keyframe  = pkt->keyframe;
        h.data.assign(pkt->data, pkt->data + pkt->size);

        bool overflowed = false;
        {
            std::lock_guard<std::mutex> lk(ctx->held_mtx);
            ctx->held_bytes += h.data.size();
            ctx->held.push_back(std::move(h));
            overflowed = ctx->held.size() > kMaxHeldPackets ||
                         ctx->held_bytes > kMaxHeldBytes ||
                         (now_ms() - (int64_t)ctx->deferred_since_ms) > kMaxHeldMs;
        }

        if (overflowed) {
            // Only once: accepting is cleared first so no further packet can
            // take this branch and stop the output a second time.
            if (ctx->accepting.exchange(false)) {
                mlog_error("the video encoder never produced a codec config — "
                           "nothing can be published without it. Stopping. Try "
                           "the x264 encoder, which provides one immediately.");
                obs_output_signal_stop(ctx->output, OBS_OUTPUT_ENCODE_ERROR);
            }
            return;
        }

        // Ask the writer thread to finish the start, the moment a video packet
        // means the config exists. Done there rather than here because it
        // writes event.json and init.mp4 to the bucket, and this is OBS's
        // encode thread.
        if (h_is_video_and_config_ready(ctx, pkt) &&
            !ctx->completing.exchange(true)) {
            ctx->complete_requested = true;
            ctx->wq_cv.notify_one();
        }
        return;
    }

    int track = -1;
    if (pkt->type == OBS_ENCODER_VIDEO) track = ctx->video_track;
    else if (pkt->type == OBS_ENCODER_AUDIO &&
             pkt->track_idx < MAX_AUDIO_MIXES)
        track = ctx->audio_track_for[pkt->track_idx];
    if (track < 0) return;

    CmafPacket cp;
    cp.track = track;
    cp.data.assign(pkt->data, pkt->data + pkt->size);
    cp.pts_ns = ts_to_ns(pkt->pts, pkt->timebase_num, pkt->timebase_den);
    cp.dts_ns = ts_to_ns(pkt->dts, pkt->timebase_num, pkt->timebase_den);
    cp.keyframe = pkt->keyframe;

    // One-time sanity log: confirms the timebase conversion is sane and that
    // keyframes are arriving (both are prerequisites for segmentation).
    if (!ctx->logged_first_packets && pkt->type == OBS_ENCODER_VIDEO) {
        ctx->video_packets_seen++;
        if (ctx->video_packets_seen <= 2 || pkt->keyframe) {
            mlog_info("video pkt: pts=%lld tb=%d/%d -> %.3fs%s",
                      (long long)pkt->pts, (int)pkt->timebase_num,
                      (int)pkt->timebase_den, (double)cp.pts_ns / 1e9,
                      pkt->keyframe ? " [KEYFRAME]" : "");
            if (pkt->keyframe && ctx->video_packets_seen > 2)
                ctx->logged_first_packets = true;   // seen enough
        }
    }

    // Hold the muxer lock only for the push itself. stop() takes this same
    // lock before destroying the muxer, so this can never touch freed memory.
    {
        std::lock_guard<std::mutex> mlk(ctx->mux_mtx);
        if (!ctx->muxer) return;               // stop() got here first
        ctx->muxer->push(cp);
    }
}

void register_output() {
    struct obs_output_info info = {};
    info.id             = "multisite_output";
    info.flags          = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED | OBS_OUTPUT_MULTI_TRACK;
    info.get_name       = out_name;
    info.create         = out_create;
    info.destroy        = out_destroy;
    info.start          = out_start;
    info.stop           = out_stop;
    info.encoded_packet = out_packet;
    info.get_properties = out_props;
    info.get_defaults   = out_defaults;
    info.get_total_bytes = out_total_bytes;
    // AV1 is carried and muxed, but has had less real-world exercise than
    // H.264 and HEVC.
    info.encoded_video_codecs = "h264;hevc;av1";
    info.encoded_audio_codecs = "aac";
    obs_register_output(&info);
    mlog_info("registered output: multisite_output");
}

} // namespace multisite_obs
