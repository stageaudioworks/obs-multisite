// SPDX-License-Identifier: GPL-3.0-or-later
#include "broadcast_controller.h"
#include "plugin_log.h"
#include "multisite_ui.h"

#include "../core/link_health.h"
#include "../core/disk_health.h"
#include "../core/model.h"
#include "../core/s3_transport.h"
#include "../core/session.h"   // peek_resumable(), SessionConfig defaults

#include <util/platform.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace multisite_obs {

// The idle connection monitor. Deliberately opaque in the header: it owns a
// libcurl-backed transport and a thread, neither of which belongs in the Qt-free
// controller interface.
struct IdleMonitor {
    multisite::S3Config cfg;
    std::unique_ptr<multisite::S3Transport> tx;
    std::atomic<bool> running{false};
    std::thread thread;

    multisite::LinkTracker link;
    std::mutex mtx;
    std::string colo;
    std::string host;
    std::string error;
};

// ── Encoder discovery ────────────────────────────────────────────────────────
// Asks OBS what it has rather than assuming: the same plugin runs on machines
// with NVENC, QuickSync, AMF or nothing but x264.
std::vector<EncoderChoice> available_video_encoders() {
    std::vector<EncoderChoice> out;
    // Every video encoder rejected, and why. Reported once per session,
    // because "the dock is missing my encoder" is otherwise unanswerable
    // without a build: OBS's own log lists what exists, and nothing said why
    // this plugin declined any of it.
    static bool reported = false;
    std::string rejected;

    const char* id = nullptr;
    for (size_t i = 0; obs_enum_encoder_types(i, &id); ++i) {
        if (!id) continue;
        if (obs_get_encoder_type(id) != OBS_ENCODER_VIDEO) continue;

        // Skip what OBS itself hides. Enumeration returns legacy aliases and
        // internal entries too, which is why an AMD machine showed the AMF
        // encoder twice: the real one plus a deprecated alias for the same
        // hardware. OBS's own encoder list filters on these flags.
        const uint32_t caps = obs_get_encoder_caps(id);
        if (caps & (OBS_ENCODER_CAP_DEPRECATED | OBS_ENCODER_CAP_INTERNAL)) {
            rejected += std::string(" ") + id +
                        ((caps & OBS_ENCODER_CAP_DEPRECATED) ? "(deprecated)"
                                                             : "(internal)");
            continue;
        }

        const char* codec = obs_get_encoder_codec(id);
        if (!codec) {
            rejected += std::string(" ") + id + "(no codec reported)";
            continue;
        }
        const std::string c = codec;
        // Only codecs the CMAF muxer and the satellite decoder handle.
        if (c != "h264" && c != "hevc" && c != "av1") {
            rejected += std::string(" ") + id + "(codec " + c + ")";
            continue;
        }

        EncoderChoice e;
        e.id = id;
        const char* disp = obs_encoder_get_display_name(id);
        e.name  = disp ? disp : id;
        e.codec = c;
        // Hardware encoders are identified by name rather than by a flag,
        // because OBS does not expose one.
        e.hardware = (e.id.find("nvenc") != std::string::npos) ||
                     (e.id.find("qsv")   != std::string::npos) ||
                     (e.id.find("amf")   != std::string::npos) ||
                     (e.id.find("_tex")  != std::string::npos) ||
                     (e.id.find("vaapi") != std::string::npos) ||
                     (e.id.find("videotoolbox") != std::string::npos);
        // Belt and braces against two entries that present identically.
        bool dup = false;
        for (const auto& x : out)
            if (x.name == e.name && x.codec == e.codec) { dup = true; break; }
        if (dup) {
            rejected += std::string(" ") + id + "(duplicate name)";
            continue;
        }

        out.push_back(e);
    }

    // x264 exists on every OBS install and is the fallback the controller uses,
    // so make sure it is always offered even if enumeration missed it.
    {
        bool have_x264 = false;
        for (const auto& e : out) if (e.id == "obs_x264") have_x264 = true;
        if (!have_x264 && obs_get_encoder_codec("obs_x264")) {
            EncoderChoice e;
            e.id = "obs_x264";
            const char* disp = obs_encoder_get_display_name("obs_x264");
            e.name = disp ? disp : "x264";
            e.codec = "h264";
            out.push_back(e);
        }
    }

    if (!reported) {
        reported = true;
        std::string offered;
        for (const auto& e : out) offered += " " + e.id;
        mlog_info("video encoders offered:%s",
                  offered.empty() ? " none" : offered.c_str());
        if (!rejected.empty())
            mlog_info("video encoders not offered:%s", rejected.c_str());
    }

    // Hardware first, then by codec, so the best option is the obvious one.
    std::stable_sort(out.begin(), out.end(),
        [](const EncoderChoice& a, const EncoderChoice& b) {
            if (a.hardware != b.hardware) return a.hardware;
            // hevc, then h264, then av1 — av1 last because it has had the
            // least real-world exercise here.
            auto rank = [](const std::string& c) {
                return c == "hevc" ? 0 : (c == "h264" ? 1 : 2);
            };
            return rank(a.codec) < rank(b.codec);
        });
    return out;
}

BroadcastController& BroadcastController::instance() {
    static BroadcastController c;
    return c;
}

// ── Settings persistence ─────────────────────────────────────────────────────
// Stored with OBS's own plugin config so it survives restarts and is per-profile
// in the same way OBS's settings are.
void BroadcastSettings::load() {
    char* path = obs_module_config_path("encoder.json");
    if (!path) return;
    obs_data_t* d = obs_data_create_from_json_file(path);
    bfree(path);
    if (!d) return;

    storage_provider   = obs_data_get_string(d, "storage_provider");
    endpoint_host      = obs_data_get_string(d, "endpoint_host");
    r2_account_id      = obs_data_get_string(d, "r2_account_id");
    bucket             = obs_data_get_string(d, "bucket");
    access_key_id      = obs_data_get_string(d, "access_key_id");
    secret_access_key  = obs_data_get_string(d, "secret_access_key");
    if (obs_data_has_user_value(d, "region"))
        region = obs_data_get_string(d, "region");
    if (obs_data_has_user_value(d, "room_id"))
        room_id = obs_data_get_string(d, "room_id");
    // Renamed from use_object_tags; the old key is still read so an encoder
    // configured before the rename keeps its setting.
    send_expiry_tag    = obs_data_get_bool(d, "send_expiry_tag") ||
                         obs_data_get_bool(d, "use_object_tags");
    if (obs_data_has_user_value(d, "segment_duration_s"))
        segment_duration_s = obs_data_get_double(d, "segment_duration_s");
    if (obs_data_has_user_value(d, "video_bitrate_kbps"))
        video_bitrate_kbps = (int)obs_data_get_int(d, "video_bitrate_kbps");
    if (obs_data_has_user_value(d, "audio_bitrate_kbps"))
        audio_bitrate_kbps = (int)obs_data_get_int(d, "audio_bitrate_kbps");
    if (obs_data_has_user_value(d, "audio_tracks"))
        audio_tracks = (int)obs_data_get_int(d, "audio_tracks");
    if (obs_data_has_user_value(d, "track_labels"))
        track_labels = obs_data_get_string(d, "track_labels");
    if (obs_data_has_user_value(d, "channel_labels"))
        channel_labels = obs_data_get_string(d, "channel_labels");
    if (obs_data_has_user_value(d, "marker_labels"))
        marker_labels = obs_data_get_string(d, "marker_labels");
    if (obs_data_has_user_value(d, "video_encoder_id"))
        video_encoder_id = obs_data_get_string(d, "video_encoder_id");
    if (obs_data_has_user_value(d, "tile_layout"))
        tile_layout = obs_data_get_string(d, "tile_layout");
    if (obs_data_has_user_value(d, "lan_enabled"))
        lan_enabled = obs_data_get_bool(d, "lan_enabled");
    if (obs_data_has_user_value(d, "lan_port"))
        lan_port = (int)obs_data_get_int(d, "lan_port");
    if (obs_data_has_user_value(d, "lan_auth_token"))
        lan_auth_token = obs_data_get_string(d, "lan_auth_token");
    obs_data_release(d);
}

void BroadcastSettings::save() const {
    char* dir = obs_module_config_path("");
    if (dir) { os_mkdirs(dir); bfree(dir); }

    obs_data_t* d = obs_data_create();
    obs_data_set_string(d, "storage_provider", storage_provider.c_str());
    obs_data_set_string(d, "endpoint_host", endpoint_host.c_str());
    obs_data_set_string(d, "r2_account_id", r2_account_id.c_str());
    obs_data_set_string(d, "bucket", bucket.c_str());
    obs_data_set_string(d, "access_key_id", access_key_id.c_str());
    obs_data_set_string(d, "secret_access_key", secret_access_key.c_str());
    obs_data_set_string(d, "region", region.c_str());
    obs_data_set_string(d, "room_id", room_id.c_str());
    obs_data_set_bool(d, "send_expiry_tag", send_expiry_tag);
    obs_data_set_double(d, "segment_duration_s", segment_duration_s);
    obs_data_set_int(d, "video_bitrate_kbps", video_bitrate_kbps);
    obs_data_set_int(d, "audio_bitrate_kbps", audio_bitrate_kbps);
    obs_data_set_int(d, "audio_tracks", audio_tracks);
    obs_data_set_string(d, "track_labels", track_labels.c_str());
    obs_data_set_string(d, "channel_labels", channel_labels.c_str());
    obs_data_set_string(d, "marker_labels", marker_labels.c_str());
    obs_data_set_string(d, "video_encoder_id", video_encoder_id.c_str());
    obs_data_set_string(d, "tile_layout", tile_layout.c_str());
    obs_data_set_bool(d, "lan_enabled", lan_enabled);
    obs_data_set_int(d, "lan_port", lan_port);
    obs_data_set_string(d, "lan_auth_token", lan_auth_token.c_str());

    char* path = obs_module_config_path("encoder.json");
    if (path) {
        if (!obs_data_save_json_safe(d, path, "tmp", "bak"))
            mlog_warn("could not save encoder settings to %s", path);
        bfree(path);
    }
    obs_data_release(d);
}

BroadcastSettings BroadcastController::settings_copy() const {
    std::lock_guard<std::mutex> lk(m_cfg_mtx);
    return m_cfg;
}

void BroadcastController::set_settings(const BroadcastSettings& s) {
    {
        std::lock_guard<std::mutex> lk(m_cfg_mtx);
        m_cfg = s;
        m_cfg.save();
    }
    // Idle monitor follows the settings: any change to credentials or room
    // rebuilds the transport and re-probes.
    start_idle_monitor();
}

bool BroadcastController::is_live() const { return m_output != nullptr; }

// ── Going live ───────────────────────────────────────────────────────────────
bool BroadcastController::go_live(std::string& error, bool force_new_event) {
    if (m_output) { error = "already broadcasting"; return false; }

    if (m_cfg.bucket.empty()) {
        error = "Bucket is required";
        return false;
    }
    if (m_cfg.endpoint_host.empty() && m_cfg.r2_account_id.empty()) {
        error = "Set either an R2 Account ID or an endpoint host";
        return false;
    }
    if (m_cfg.access_key_id.empty() || m_cfg.secret_access_key.empty()) {
        error = "Access Key ID and Secret Access Key are required";
        return false;
    }

    // The uploader's own traffic becomes the health signal once the output is
    // actually running (the monitor is stopped just after obs_output_start).
    obs_data_t* s = obs_data_create();
    obs_data_set_string(s, "endpoint_host", m_cfg.endpoint_host.c_str());
    obs_data_set_string(s, "r2_account_id", m_cfg.r2_account_id.c_str());
    obs_data_set_string(s, "bucket", m_cfg.bucket.c_str());
    obs_data_set_string(s, "access_key_id", m_cfg.access_key_id.c_str());
    obs_data_set_string(s, "secret_access_key", m_cfg.secret_access_key.c_str());
    obs_data_set_string(s, "region", m_cfg.region.c_str());
    obs_data_set_string(s, "room_id", m_cfg.room_id.c_str());
    obs_data_set_string(s, "event_name", m_cfg.event_name.c_str());
    obs_data_set_double(s, "segment_duration_s", m_cfg.segment_duration_s);
    obs_data_set_string(s, "track_labels", m_cfg.track_labels.c_str());
    obs_data_set_string(s, "channel_labels", m_cfg.channel_labels.c_str());
    obs_data_set_string(s, "marker_labels", m_cfg.marker_labels.c_str());
    obs_data_set_bool(s, "send_expiry_tag", m_cfg.send_expiry_tag);
    // Read once, at Go Live, and written into event.json — which is why
    // changing it mid-broadcast does nothing until the next event.
    obs_data_set_string(s, "tile_layout", m_cfg.tile_layout.c_str());
    // Transient — never read back from settings, never persisted. The
    // operator's explicit choice for THIS Go Live only (PROJECT-SCOPE.md §5.1).
    obs_data_set_bool(s, "force_new_event", force_new_event);
    // LAN / direct delivery (PROJECT-SCOPE.md §8.7).
    obs_data_set_bool(s, "lan_enabled", m_cfg.lan_enabled);
    obs_data_set_int(s, "lan_port", m_cfg.lan_port);
    obs_data_set_string(s, "lan_auth_token", m_cfg.lan_auth_token.c_str());

    m_output = obs_output_create("multisite_output", "multisite_out", s, nullptr);
    obs_data_release(s);
    if (!m_output) {
        error = "could not create the multisite output (is the plugin loaded?)";
        return false;
    }

    // Video encoder. Two settings are not optional whichever encoder is used:
    //   * the keyframe interval must equal the segment duration, or the muxer
    //     can never cut a segment;
    //   * anything that inserts extra keyframes must be off, or segment
    //     lengths vary (which is what produced 5.6-7.1 MB segments earlier).
    std::string enc_id = m_cfg.video_encoder_id.empty() ? "obs_x264"
                                                        : m_cfg.video_encoder_id;
    // Fall back rather than fail if the chosen encoder has gone (different
    // machine, driver removed, GPU changed).
    {
        bool found = false;
        for (const auto& e : available_video_encoders())
            if (e.id == enc_id) { found = true; break; }
        if (!found) {
            mlog_warn("video encoder '%s' is not available on this machine — "
                      "falling back to x264", enc_id.c_str());
            enc_id = "obs_x264";
        }
    }

    obs_data_t* vs = obs_data_create();
    obs_data_set_int(vs, "bitrate", m_cfg.video_bitrate_kbps);
    obs_data_set_int(vs, "keyint_sec",
                     (int)std::max(1.0, m_cfg.segment_duration_s + 0.5));
    obs_data_set_string(vs, "rate_control", "CBR");

    if (enc_id == "obs_x264") {
        // scenecut inserts IDRs at scene changes, which breaks even spacing.
        obs_data_set_string(vs, "x264opts", "scenecut=0");
    } else if (enc_id.find("nvenc") != std::string::npos) {
        // Look-ahead can move I-frames off the interval, which is the one
        // thing that must not happen: keyframes have to land on the segment
        // boundary or segment lengths vary.
        obs_data_set_bool(vs, "lookahead", false);
    }
    // Nothing else is set. Quality presets differ in key and value between
    // encoder families, and a mistyped key is silently ignored — an earlier
    // attempt set the AMF preset to "quality" and the encoder logged
    // "preset: speed", i.e. it did nothing. OBS's own defaults are sensible,
    // and leaving them alone is more honest than pretending to tune something.
    // Only the settings that the segmenting REQUIRES are forced.

    m_venc = obs_video_encoder_create(enc_id.c_str(), "multisite_v", vs, nullptr);
    obs_data_release(vs);
    if (!m_venc) {
        error = "could not create the video encoder '" + enc_id + "'";
        release_all();
        return false;
    }
    {
        const char* codec = obs_encoder_get_codec(m_venc);
        mlog_info("video encoder: %s (%s), keyframes every %.0fs",
                  enc_id.c_str(), codec ? codec : "?",
                  m_cfg.segment_duration_s);
    }
    obs_encoder_set_video(m_venc, obs_get_video());
    obs_output_set_video_encoder(m_output, m_venc);

    // One AAC encoder per requested OBS mixer track.
    const int tracks = std::max(1, std::min(6, m_cfg.audio_tracks));
    for (int i = 0; i < tracks; ++i) {
        obs_data_t* as = obs_data_create();
        obs_data_set_int(as, "bitrate", m_cfg.audio_bitrate_kbps);
        std::string name = "multisite_a" + std::to_string(i + 1);
        obs_encoder_t* enc = obs_audio_encoder_create(
            "ffmpeg_aac", name.c_str(), as, (size_t)i, nullptr);
        obs_data_release(as);
        if (!enc) {
            error = "could not create audio encoder for track " +
                    std::to_string(i + 1);
            release_all();
            return false;
        }
        obs_encoder_set_audio(enc, obs_get_audio());
        obs_output_set_audio_encoder(m_output, enc, (size_t)i);
        m_aencs.push_back(enc);
    }

    if (!obs_output_start(m_output)) {
        const char* le = obs_output_get_last_error(m_output);
        error = le && *le ? le
                          : "the output refused to start — see the OBS log";
        release_all();
        return false;
    }

    // Live now: the uploader's own traffic is the health signal, so retire the
    // idle probe.
    stop_idle_monitor();

    m_started_ns = os_gettime_ns();
    mlog_info("broadcast started: room=%s, %d audio track(s), %.1fs segments",
              m_cfg.room_id.c_str(), tracks, m_cfg.segment_duration_s);
    return true;
}

void BroadcastController::end_broadcast() {
    if (!m_output) return;
    mlog_info("ending broadcast (draining the upload queue)");
    obs_output_stop(m_output);
    release_all();
    // Back to idle: resume the background probe so the operator still sees a
    // live reading before the next Go Live.
    start_idle_monitor();
}

void BroadcastController::release_all() {
    for (obs_encoder_t* e : m_aencs) if (e) obs_encoder_release(e);
    m_aencs.clear();
    if (m_venc)   { obs_encoder_release(m_venc); m_venc = nullptr; }
    if (m_output) { obs_output_release(m_output); m_output = nullptr; }
    m_started_ns = 0;
}

// Same computation as multisite_output.cpp's out_start(): the durable spool
// lives beside OBS's own plugin config. Kept in sync deliberately rather than
// shared, since the two call sites otherwise have nothing else in common.
static std::string spool_dir_path() {
    char* cfgdir = obs_module_config_path("spool");
    std::string dir = cfgdir ? cfgdir : "./multisite_spool";
    bfree(cfgdir);
    return dir;
}

multisite::ResumeInfo BroadcastController::check_resumable_before_go_live() const {
    // The default straight from SessionConfig, rather than a second literal
    // that could drift from it — there is no operator-facing setting for
    // this yet (see PROJECT-SCOPE.md §5.1's open question).
    return multisite::peek_resumable(spool_dir_path(),
                                      multisite::SessionConfig{}.resume_stale_after_ms);
}

BroadcastStatus BroadcastController::status() const {
    BroadcastStatus st;
    st.live = m_output != nullptr;

    // Checked regardless of live/idle: a full disk is worth knowing about
    // before Go Live, not just discovered mid-event. The directory itself is
    // only created when a Session first starts, so on a machine that has
    // never gone live yet, create it here too rather than showing "—" until
    // the first broadcast.
    {
        std::string dir = spool_dir_path();
        std::error_code ec;
        auto sp = std::filesystem::space(dir, ec);
        if (ec) {
            std::filesystem::create_directories(dir, ec);
            sp = std::filesystem::space(dir, ec);
        }
        if (!ec) {
            st.disk_known      = true;
            st.disk_free_bytes = (unsigned long long)sp.available;
            st.disk_health     = (int)multisite::classify_disk_free(sp.available);
        }
    }
    st.lan_enabled = m_cfg.lan_enabled;   // true whether idle or live: what
                                           // Go Live will do next time
    if (st.live) {
        st.bytes = obs_output_get_total_bytes(m_output);
        st.uptime_s = m_started_ns
            ? (double)(os_gettime_ns() - m_started_ns) / 1e9 : 0.0;
        // The richer figures (queue depth, retries, link health) live in the
        // output's session; it publishes them through the controls registry.
        EncoderStats es;
        if (encoder_stats(es)) {
            st.event_id    = es.event_id;
            st.confirmed   = es.confirmed;
            st.pending     = (size_t)es.pending;
            st.retries     = es.retries;
            st.link_health = es.link_health;
            st.last_error  = es.last_error;
            if (es.bytes) st.bytes = es.bytes;
            st.colo               = es.colo;
            st.storage_host       = es.storage_host;
            st.upload_bytes_per_s = es.upload_bytes_per_s;
            st.upload_samples     = es.upload_samples;
            st.resumed_event_id           = es.resumed_event_id;
            st.resumed_event_started_ms   = es.resumed_event_started_ms;
            st.resumed_already_confirmed  = es.resumed_already_confirmed;
            st.lan_running          = es.lan_running;
            st.lan_port             = es.lan_port;
            st.lan_cached_segments  = (size_t)es.lan_cached_segments;
            st.lan_error            = es.lan_error;
        }
        st.link_known = true;   // the uploader always reports once live
        return st;
    }

    // Idle: the background probe is the only traffic, so it owns the reading.
    if (m_idle) {
        st.link_health = (int)m_idle->link.health();
        st.link_known  = m_idle->link.known();
        std::lock_guard<std::mutex> lk(m_idle->mtx);
        st.colo         = m_idle->colo;
        st.storage_host = m_idle->host;
        st.last_error   = m_idle->error;
    }
    return st;
}

// ── Idle connection monitor ──────────────────────────────────────────────────
// One tiny signed GET of the room's live.json every ten seconds, so the dock
// can show a live internet reading even when nothing is being sent. The probe
// is read-only and the object exists the moment the room has ever been used;
// a 404 still means the endpoint answered, which is all this measures.

BroadcastController::~BroadcastController() {
    stop_idle_monitor();
}

void BroadcastController::stop_idle_monitor() {
    if (!m_idle) return;
    m_idle->running = false;
    // A probe in flight may be blocking inside libcurl for up to the request
    // timeout; cancel it rather than making Go Live wait on it.
    if (m_idle->tx) m_idle->tx->cancel_pending();
    if (m_idle->thread.joinable()) m_idle->thread.join();
    m_idle.reset();
}

void BroadcastController::start_idle_monitor() {
    stop_idle_monitor();
    if (is_live()) return;
    if (m_cfg.bucket.empty() ||
        (m_cfg.endpoint_host.empty() && m_cfg.r2_account_id.empty()) ||
        m_cfg.access_key_id.empty() || m_cfg.secret_access_key.empty())
        return;

    auto m = std::make_unique<IdleMonitor>();
    m->cfg.endpoint_host     = m_cfg.endpoint_host;
    m->cfg.r2_account_id     = m_cfg.r2_account_id;
    m->cfg.bucket            = m_cfg.bucket;
    m->cfg.access_key_id     = m_cfg.access_key_id;
    m->cfg.secret_access_key = m_cfg.secret_access_key;
    m->cfg.region            = m_cfg.region;
    m->tx = std::make_unique<multisite::S3Transport>(m->cfg);
    m->host = m->tx->host();
    // The thread reads m_idle, so publish it before the thread can run.
    m_idle = std::move(m);
    m_idle->running = true;
    m_idle->thread = std::thread([this] { idle_probe_loop(); });
}

void BroadcastController::idle_probe_loop() {
    if (!m_idle) return;
    const std::string key = multisite::live_pointer_key(m_cfg.room_id);
    auto next = std::chrono::steady_clock::now();   // first probe immediately
    while (m_idle->running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (!m_idle->running.load()) break;
        if (std::chrono::steady_clock::now() < next) continue;
        next = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        if (is_live()) break;   // the uploader is the signal once broadcasting

        const multisite::StorageProbe p = m_idle->tx->probe(key);
        m_idle->link.observe(p.reachable);
        std::lock_guard<std::mutex> lk(m_idle->mtx);
        m_idle->colo  = p.colo;
        m_idle->error = p.reachable ? std::string() : p.error;
    }
}

void BroadcastController::drop_marker(const std::string& label) {
    forward_marker_to_encoder(label);
}

} // namespace multisite_obs
