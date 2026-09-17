// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// lan_object_server.h — serves the encoder's own event objects directly to
// satellites on the same network or an existing VPN, instead of only
// through the bucket. See PROJECT-SCOPE.md §8.7.
//
// Serves the IDENTICAL object shape a cloud decoder already reads —
// manifest.json, event.json, init.mp4, segments/{seq}.m4s — so a decoder's
// LAN transport is just a plain HTTP GET against this instead of against
// S3; nothing downstream has to know which one it got. Cloud upload is
// never affected by this: it is a second way to reach the same objects, not
// a replacement for the first, and this class has no way to touch it even
// by accident — it never sees Transport, SpoolQueue or the bucket.
//
// A segment already confirmed to the bucket is gone from the encoder's spool
// (see spool_queue.h) — that is what makes the spool durable and bounded.
// LAN serving therefore keeps its OWN bounded retention window, reusing the
// SegmentCache a decoder already uses for exactly this job, fed as each
// segment confirms (Session::set_segment_confirmed_callback) rather than
// read back from the spool.
//
#include "segment_cache.h"
#include "http_server.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace multisite {

struct LanServerConfig {
    std::string bind_address = "0.0.0.0";
    int         port = 9080;
    // Must match SessionConfig::room_id: this is what a satellite's
    // live.json GET actually asks for (see live_pointer_key()), and there is
    // no other way this class could learn it — it never sees SessionConfig.
    std::string room_id = "main-auditorium";
    // A pre-shared token satellites present as `Authorization: Bearer
    // <token>`. Empty disables the check — appropriate on a plain building
    // LAN, where the remote-control pages already go unauthenticated for the
    // same reason (the network itself is the trust boundary); set it once
    // the path crosses a VPN. See §8.7.
    std::string auth_token;
    // How many confirmed segments to keep available to LAN satellites — a
    // hard ceiling, the same shape as the decoder's own max_cached_segments,
    // so a long event cannot fill the encoder's disk with a second copy of
    // everything it has ever sent.
    int max_cached_segments = 2000;
};

class LanObjectServer {
public:
    LanObjectServer(LanServerConfig cfg, std::string cache_dir);
    ~LanObjectServer();

    LanObjectServer(const LanObjectServer&) = delete;
    LanObjectServer& operator=(const LanObjectServer&) = delete;

    bool start(std::string& error);
    void stop();
    int  port() const;

    // A cue handed in by a satellite that has no bucket of its own, over
    // POST /api/cue. The server does not write anything: it parses and
    // authorises the request and hands it to this, which is the encoder's
    // Session (see Session::add_cue_from) — so one writer owns each cue
    // object and a satellite stays read-only where it has to be. Returns
    // false with `error` set when the cue is refused.
    using CueHandler = std::function<bool(const std::string& author,
                                          const std::string& label,
                                          std::string& error)>;
    void set_cue_callback(CueHandler cb) { m_on_cue = std::move(cb); }

    // Wire these three straight to the matching Session callbacks
    // (set_event_started_callback / set_segment_confirmed_callback /
    // set_manifest_published_callback) and this class needs nothing else.

    // A new event started (or resumed): what a satellite bootstraps from.
    // Also resets the retention window — an old event's segments are not
    // meaningful under a new one, the same reasoning SegmentCache::set_event
    // already applies on the decoder side.
    void on_event_started(const std::string& event_id,
                          const std::string& event_json,
                          const std::vector<uint8_t>& init_bytes);
    // A segment was confirmed durable — the moment its bytes are still
    // available (see the file header), whether or not the spool keeps them
    // any longer.
    void on_segment_confirmed(uint64_t seq, const std::vector<uint8_t>& bytes);
    // The manifest was (re)published — kept verbatim, not recomputed, so a
    // LAN-connected decoder never sees a manifest a cloud decoder couldn't
    // also have seen.
    void on_manifest_published(std::string json);
    // live.json was (re)published — the ONLY way a LAN satellite following
    // the room (rather than a pinned event id) can discover which event is
    // live at all, needed in full for a cloud-disabled encoder (see
    // null_transport.h) where there is no bucket live.json to fall back to.
    void on_live_published(std::string json);
    // markers.json was (re)published. Without this a LAN-only satellite
    // (cloud disabled) can never receive a marker at all, since there is no
    // cloud copy to fall back to for it — unlike segments and the manifest,
    // which a cloud-enabled event still publishes even with LAN also on.
    void on_markers_published(std::string json);

    // How many segments are currently retained, and how many have had to be
    // evicted for the cap — surfaced for the encoder dock, the same shape as
    // Session::Status::dropped_for_disk.
    size_t   cached_count() const;

private:
    LanServerConfig m_cfg;
    std::unique_ptr<SegmentCache> m_cache;
    std::unique_ptr<HttpServer>   m_http;

    mutable std::mutex m_mtx;
    std::string m_event_id;         // guarded by m_mtx
    std::string m_event_json;
    std::string m_manifest_json;
    std::string m_live_json;        // guarded by m_mtx
    std::string m_markers_json;     // guarded by m_mtx

    bool check_auth(const HttpRequest& req) const;
    void handle_events(const HttpRequest& req, HttpResponse& res);
    void handle_live(const HttpRequest& req, HttpResponse& res);
    void handle_cue(const HttpRequest& req, HttpResponse& res);

    CueHandler m_on_cue;            // set by the encoder; empty means no cue authoring
};

} // namespace multisite
