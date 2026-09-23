// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// broadcast_controller.h — owns the encoder-side broadcast.
//
// OBS does not expose custom outputs in its UI, so something has to create the
// output and its encoders and start them. That was a Lua script; this brings it
// into the plugin so the dock can drive it directly.
//
// Deliberately free of Qt: the dock is a thin view over this, and the same
// controller is reachable from hotkeys or (later) obs-websocket.
//
#include <obs.h>

#include "../core/spool_queue.h"   // ResumeInfo

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace multisite_obs {

// Opaque idle-connection monitor. Owns its own transport and thread so the
// encoder can show a live internet reading even when nothing is being sent.
struct IdleMonitor;

struct BroadcastSettings {
    // storage
    // Which provider the dock's dropdown is showing, as a provider_key()
    // string (see storage_providers.h) — "r2", "aws", "backblaze", "wasabi"
    // or "custom". Empty on a config saved before this existed; the dock
    // then falls back to detect_provider() against whatever endpoint_host /
    // r2_account_id is already there, so an upgrade never loses or
    // misrepresents a working setup.
    std::string storage_provider;
    std::string endpoint_host;      // blank when using an R2 account id
    std::string r2_account_id;
    std::string bucket;
    std::string access_key_id;
    std::string secret_access_key;
    std::string region = "auto";
    std::string room_id = "main-auditorium";
    // What this machine calls itself, stamped on every cue it drops. Empty
    // reads as "main site" — which the encoder is unless told otherwise. The
    // Cues dock is the same on either end, so both have this (§7).
    std::string site_name;
    // Where this machine keeps the durable store-and-forward queue — the
    // encoder's equivalent of a satellite's cache folder. Empty keeps the
    // built-in location beside OBS's own plugin config, as the decoder does.
    std::string cache_dir;
    // Operator-facing title for the next event, taken from the dock at Go
    // Live. Not persisted: it is per-event and defaults to the current
    // date/time. Empty means "no custom name".
    std::string event_name;
    bool        send_expiry_tag = false;   // R2 rejects tagging; a tag deletes nothing

    // media
    // Which OBS video encoder to use. Defaults to x264 because it exists on
    // every machine; hardware encoders are offered when present.
    std::string video_encoder_id = "obs_x264";
    double segment_duration_s = 6.0;
    int    video_bitrate_kbps = 6000;
    int    audio_bitrate_kbps = 160;
    int    audio_tracks = 1;               // OBS mixer tracks to send
    std::string track_labels =
        "Main mix,Sermon ISO,Click";
    std::string channel_labels =
        "Main L,Main R,Sermon ISO,Click,Spare 5,Spare 6,Spare 7,Spare 8";
    // How this feed is composited, if it carries more than one picture:
    // "1x1", "2x1", "1x2" or "2x2". The satellite splits it accordingly and
    // exposes each region as its own source. Declared rather than detected —
    // a 3840x1080 frame is a legitimate ultrawide picture as well as a
    // plausible pair, and nothing in the video distinguishes them.
    std::string tile_layout = "1x1";

    // LAN / direct delivery (PROJECT-SCOPE.md §8.7) — off by default: this
    // opens a port, and doing that without being asked is exactly the kind
    // of thing an operator should turn on, not discover. Cloud upload is
    // completely unaffected either way.
    bool        lan_enabled = false;
    int         lan_port = 9080;
    // A pre-shared token satellites present as a bearer token. Empty means
    // no check at all — appropriate on a plain building LAN, where the
    // remote-control pages already go unauthenticated for the same reason.
    std::string lan_auth_token;
    // Only meaningful, and only ever shown, while lan_enabled is true —
    // disabling cloud with LAN off would mean nothing is delivered anywhere
    // at all. Uses NullTransport in place of S3Transport (see
    // null_transport.h): every segment still flows through the exact same
    // spool → retry-uploader → manifest pipeline, and the LAN hooks fire
    // exactly as they would with cloud on, since NullTransport confirms a
    // segment the instant it's asked to store one.
    bool        cloud_enabled = true;

    // Monitoring heartbeat (reporter-brief) — off by default. When disabled
    // or unconfigured the reporter sends nothing: zero sockets, zero bytes.
    // The collector URL is a plain string with no default pointing at our
    // infrastructure. The id must match the id the token was minted for.
    bool        reporter_enabled = false;
    std::string reporter_url;
    std::string reporter_appliance_id;
    std::string reporter_token;
    // Local device id for pairing (PROJECT-SCOPE §8.5: minted locally, no
    // network). Minted once on first Connect and persisted — an identifier,
    // not a secret, so uniqueness is what matters.
    std::string reporter_device_id;

    // Persisted alongside OBS's own plugin config.
    void load();
    void save() const;
};

// Live status, polled by the dock for display.
struct BroadcastStatus {
    bool        live = false;
    std::string event_id;
    uint64_t    confirmed = 0;
    size_t      pending = 0;
    uint64_t    retries = 0;
    uint64_t    bytes = 0;
    int         link_health = 0;      // 0 healthy, 1 degraded, 2 offline
    // True once the health above is a real measurement rather than a default.
    // While live it is always true (the uploader reports); while idle it
    // becomes true only after the first idle probe has completed.
    bool        link_known = false;
    std::string last_error;
    double      uptime_s = 0.0;
    // The link, as measured from this broadcast's own uploads.
    std::string colo;
    std::string storage_host;
    // This machine's clock against the store's, from the HTTP Date header on
    // traffic already being sent. A large value means THIS box is the one that
    // is out; 0 means nothing has been observed yet.
    long long   clock_skew_ms = 0;
    double      upload_bytes_per_s = 0.0;
    unsigned long long upload_samples = 0;
    // Free space where the durable spool lives. Checked whether live or
    // idle, so a low disk shows up before it starts costing segments (see
    // SessionConfig::max_spool_bytes). 0 healthy, 1 low, 2 critical.
    int         disk_health = 0;
    bool        disk_known = false;
    unsigned long long disk_free_bytes = 0;
    // Set for the life of the broadcast when it began by resuming an
    // interrupted event rather than starting fresh — empty means this run
    // started with start_new(). The dock shows this persistently, not just
    // once, so "what actually happened" stays visible (PROJECT-SCOPE.md §5.1).
    std::string resumed_event_id;
    long long   resumed_event_started_ms = 0;   // 0 if unknown
    unsigned long long resumed_already_confirmed = 0;

    // LAN / direct delivery (PROJECT-SCOPE.md §8.7). lan_running is only
    // meaningful while live — the server starts at Go Live and stops at End,
    // there is nothing to serve otherwise. lan_error is set when the
    // operator asked for it but the port could not be bound (e.g. already in
    // use); cloud upload is unaffected either way.
    bool        lan_enabled = false;
    bool        lan_running = false;
    int         lan_port = 0;
    size_t      lan_cached_segments = 0;
    std::string lan_error;
    // The second bucket (PROJECT-SCOPE.md §10 Phase 9). Same meaning as the
    // Session's fields of the same names; reported whether idle or live.
    bool        mirror_configured = false;
    uint64_t    mirror_behind = 0;
    bool        mirror_waiting_on_primary = false;
    bool        mirror_unreachable = false;
    bool        mirror_complete = false;
    // What Go Live will do next time — true whether idle or live, same as
    // lan_enabled above.
    bool        cloud_enabled = true;
};

// A video encoder OBS actually has on this machine.
struct EncoderChoice {
    std::string id;        // e.g. "obs_nvenc_hevc_tex"
    std::string name;      // e.g. "NVIDIA NVENC HEVC"
    std::string codec;     // "h264" | "hevc" | "av1"
    bool        hardware = false;
};

// Video encoders present on this machine that this plugin can carry. Ordered
// hardware-first, since a hardware encoder frees the CPU for everything else
// the main campus is doing.
std::vector<EncoderChoice> available_video_encoders();

class BroadcastController {
public:
    static BroadcastController& instance();

    const BroadcastSettings& settings() const { return m_cfg; }
    // A snapshot, for a reader that is not on the UI thread: the remote-control
    // pages poll from a network thread while the dock edits here on OBS's. The
    // reference above stays for callers that are already on that thread.
    BroadcastSettings settings_copy() const;
    void set_settings(const BroadcastSettings& s);

    // Creates the output plus a video encoder and one audio encoder per
    // requested track, then starts it. Returns false and fills `error` on
    // failure — the dock shows that rather than the operator hunting the log.
    // `force_new_event` mirrors S_FORCE_NEW in multisite_output.cpp: set it
    // when the operator has explicitly chosen to abandon a resumable event
    // rather than continue it (see check_resumable_before_go_live() and
    // PROJECT-SCOPE.md §5.1). Left false, an interrupted event resumes as it
    // always has.
    bool go_live(std::string& error, bool force_new_event = false);
    void end_broadcast();

    // Releases anything this controller holds from libobs, called from
    // obs_module_unload — which runs BEFORE libobs tears its own statics down.
    //
    // Without it the captions bridge is stopped by its destructor instead,
    // during __cxa_finalize at process exit, when this controller's
    // function-local static is destroyed. By then obs_get_signal_handler()
    // refers to a torn-down libobs, and disconnecting from it is a segfault on
    // the way out of a clean shutdown — which is what a real OBS produced
    // (2026-09-22).
    void unload();

    // Whether there is an interrupted event to resume, checked from disk
    // alone — no output, no Session, no network — so the dock can decide
    // whether to ask the operator BEFORE Go Live creates anything. A
    // deferred-start encoder may not construct its Session until well after
    // the operator has already clicked the button, which is too late to ask.
    multisite::ResumeInfo check_resumable_before_go_live() const;

    bool is_live() const;
    BroadcastStatus status() const;

    // Drop a marker on the running broadcast.
    void drop_marker(const std::string& label);

private:
    BroadcastController() = default;
    ~BroadcastController();   // stops the idle monitor; defined where IdleMonitor is complete
    void release_all();

    // The idle connection monitor, run only when not broadcasting. Started when
    // settings exist and stopped the moment Go Live is pressed (the uploader's
    // own traffic is the health signal while live).
    void start_idle_monitor();
    void stop_idle_monitor();
    void idle_probe_loop();

    BroadcastSettings m_cfg;
    // Guards m_cfg for readers on other threads. Writes happen on OBS's UI
    // thread — the dock directly, the remote-control pages through that same
    // queue — so this is what makes a snapshot consistent rather than a
    // half-copied string.
    mutable std::mutex m_cfg_mtx;
    obs_output_t*  m_output = nullptr;
    obs_encoder_t* m_venc = nullptr;
    std::vector<obs_encoder_t*> m_aencs;
    uint64_t m_started_ns = 0;

    std::unique_ptr<IdleMonitor> m_idle;
};

} // namespace multisite_obs
