// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// session.h — the publishing layer.
//
// Ties the CMAF muxer → durable spool → retrying uploader → manifest/live
// pointer together, and enforces the protocol's write-ordering invariant:
// a segment is only ever listed in manifest.json AFTER the object store has
// confirmed it durable. If a decoder can see a manifest entry, the segment
// exists.
//
// Object layout written by a session:
//   rooms/{room_id}/live.json
//   events/{event_id}/event.json
//   events/{event_id}/init.mp4
//   events/{event_id}/segments/{seq:08d}.m4s
//   events/{event_id}/manifest.json
//   events/{event_id}/markers.json
//
#include "model.h"
#include "spool_queue.h"
#include "retry_uploader.h"
#include "transport.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <cstdint>

namespace multisite {

struct SessionConfig {
    std::string room_id = "main-auditorium";
    // Operator-facing title for the next event (editable in the encoder dock).
    // Empty means "no custom name" — the satellite falls back to the time.
    std::string event_name;
    std::string spool_dir;                 // local durable queue location
    // Local disk cap for unconfirmed segments (0 = unlimited, retry forever).
    // Protects the encoder machine's disk when the upload link is down or too
    // slow to keep up for a long stretch, at the cost of the oldest queued
    // segments — see spool_queue.h. 4 GiB is generous headroom for normal
    // operation (the spool should sit near-empty) while still bounding a
    // multi-hour outage on a small drive.
    uint64_t    max_spool_bytes = 4ull * 1024 * 1024 * 1024;
    // How long a resumable event may sit with no activity before it is too
    // old to resume silently. Below this, a crash-and-restart just continues
    // the same recording, as it always has. At or above it, resuming would
    // risk silently swallowing a genuinely new broadcast into a leftover from
    // last week — so the caller (the OBS layer) is expected to ask the
    // operator instead of calling resume() itself. Same shape as the
    // decoder's own stale_after_ms, which makes the same kind of judgment
    // about a quiet room. See PROJECT-SCOPE.md §5.1.
    int64_t     resume_stale_after_ms = 30 * 60 * 1000; // 30 minutes
    double      segment_duration_s = 6.0;
    size_t      manifest_window = 50;      // rolling window size
    // Object tagging: S3 supports it, but Cloudflare R2 does NOT and rejects
    // requests carrying x-amz-tagging. R2 users should instead configure a
    // bucket lifecycle rule by prefix/age. Default off for broad compatibility.
    // Whether to attach the expiry tag to each PUT. Named for what it does,
    // not for what it might appear to achieve: it does NOT expire anything.
    // A tag only gives a bucket lifecycle rule something to match, and the
    // rule is what deletes. Off by default because Cloudflare R2 rejects
    // x-amz-tagging outright, and because prefix-and-age rules are the
    // portable mechanism (see scope §4.6).
    bool        send_expiry_tag = false;
    std::string expiry_tag_key = "MultisiteExpiry";
    std::string expiry_tag_val = "7d";
    int         heartbeat_interval_s = 10; // live.json refresh cadence
    // Upload retry tuning (production defaults; tests shorten these).
    int         base_backoff_ms = 250;
    int         max_backoff_ms  = 15000;
    double      backoff_jitter  = 0.30;
};

// Generates a lexicographically-sortable, time-prefixed unique event id
// (ULID-style: 10 chars of time + 16 of randomness, Crockford base32).
std::string make_event_id(int64_t now_ms = 0);

int64_t now_ms();

// Whether the spool at `spool_dir` has an interrupted event, and whether it's
// stale — without constructing a Session or a Transport. For a caller that
// has to decide, before doing anything else, whether to ask the operator
// (see PROJECT-SCOPE.md §5.1): the OBS dock calls this synchronously, on the
// UI thread, before Go Live even creates the output — Session itself may not
// be constructed until well after that, once a deferred-start encoder has
// said what it encoded. Applies the same staleness policy
// Session::check_resumable() does; the two share one implementation so they
// can't drift apart.
ResumeInfo peek_resumable(const std::string& spool_dir,
                          int64_t resume_stale_after_ms);

class Session {
public:
    Session(SessionConfig cfg, Transport& transport);
    ~Session();

    // Is there an unfinished event on disk that could be resumed?
    ResumeInfo check_resumable() const;

    // Start a new event. Publishes event.json + init.mp4, then live.json.
    // `init` is the CMAF init segment; tracks describe what's inside segments.
    bool start_new(const std::vector<uint8_t>& init,
                   const VideoInfo& video,
                   const std::vector<AudioTrack>& audio_tracks);

    // Resume the on-disk event, continuing its sequence numbering.
    bool resume(const std::vector<uint8_t>& init,
                const VideoInfo& video,
                const std::vector<AudioTrack>& audio_tracks);

    // Durably enqueue a finished media fragment. Returns its sequence number.
    // Safe to call from the encode thread; never blocks on the network.
    uint64_t publish_segment(std::vector<uint8_t> fragment,
                             double duration_s,
                             double pts_offset_s);

    // Append a marker and publish markers.json.
    void add_marker(const std::string& label, const std::string& type = "cue");

    // Refresh live.json's heartbeat (drives decoder stale-detection).
    void heartbeat();

    // Flush remaining spool, mark the event ended, publish final state.
    void end(std::chrono::milliseconds drain_deadline = std::chrono::milliseconds(30000));

    // Status for the encoder UI.
    struct Status {
        std::string event_id;
        uint64_t    last_confirmed = 0;
        uint64_t    last_enqueued  = 0;
        size_t      pending        = 0;
        uint64_t    confirmed_total = 0;
        uint64_t    bytes_uploaded = 0;
        uint64_t    retries        = 0;
        uint64_t    verify_failures = 0;
        std::string verify_note;      // result of the last upload verification
        LinkHealth  health = LinkHealth::Healthy;
        // How many segments this event has had to drop from the local spool
        // for disk space (see SessionConfig::max_spool_bytes). Zero in normal
        // operation; the encoder dock surfaces this as an operator warning.
        uint64_t    dropped_for_disk = 0;

        // Set once, the moment this event was resumed rather than started
        // fresh — empty event_id means this run started with start_new().
        // Kept for the life of the Session (not just logged once) so the dock
        // can show a persistent line for as long as it's actually true. See
        // PROJECT-SCOPE.md §5.1.
        std::string resumed_event_id;
        // Wall-clock start of the ORIGINAL event, read back from event.json —
        // 0 if that read failed, in which case the dock says "an interrupted
        // event" rather than naming a time it does not actually know.
        int64_t     resumed_event_started_ms = 0;
        // Segments already confirmed durable before this run resumed —
        // i.e. work that would be abandoned by choosing "start new" instead.
        uint64_t    resumed_already_confirmed = 0;
    };
    Status status() const;

    // Called after every segment is confirmed durable, so the host can report
    // progress (segment count, bytes, pending depth, link health).
    using ProgressCallback = std::function<void(const Status&)>;
    void set_progress_callback(ProgressCallback cb) { m_on_progress = std::move(cb); }

    // Bytes confirmed uploaded so far (feeds OBS's own output stats).
    uint64_t bytes_uploaded() const;

    const std::string& event_id() const { return m_event_id; }

    // Why the last operation failed (HTTP status + body). Empty if none.
    const std::string& last_error() const { return m_last_error; }

private:
    SessionConfig m_cfg;
    Transport&    m_tx;
    std::unique_ptr<SpoolQueue>    m_spool;
    std::unique_ptr<RetryUploader> m_uploader;

    std::string m_event_id;
    uint64_t    m_next_seq = 0;
    int64_t     m_last_heartbeat_ms = 0;

    Manifest    m_manifest;
    MarkerList  m_markers;
    uint64_t    m_dropped_total = 0;   // guarded by m_mtx
    // Set once in resume(), read by status(). Empty resumed_event_id means
    // this run started fresh. Guarded by m_mtx like the rest of Status's
    // sources.
    std::string m_resumed_event_id;
    int64_t     m_resumed_event_started_ms = 0;
    uint64_t    m_resumed_already_confirmed = 0;
    std::string m_last_error;
    ProgressCallback m_on_progress;
    mutable std::mutex m_mtx;

    std::string segment_key(uint64_t seq) const;
    std::string event_prefix() const;
    bool  put_json(const std::string& key, const std::string& body);
    bool  put_bytes(const std::string& key, const std::vector<uint8_t>& b,
                    const std::string& content_type);
    void  publish_manifest_locked();
    void  publish_live(const std::string& status);
    bool  begin_common(const std::vector<uint8_t>& init,
                       const VideoInfo& video,
                       const std::vector<AudioTrack>& tracks);
    void  on_confirmed(const SpooledSegment& seg);
    // Called (on the encode thread, via SpoolQueue's drop callback) when a
    // segment had to be evicted for disk space. Must never touch the network:
    // publish_segment()'s "never blocks on the network" guarantee for the
    // encode thread depends on it. The advanced floor rides along on the next
    // manifest publish, whenever that naturally happens.
    void  on_dropped(const SpoolDrop& d);
};

} // namespace multisite
