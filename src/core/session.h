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
    // The second bucket, when redundancy is configured (PROJECT-SCOPE.md §10
    // Phase 9). Borrowed, not owned — the caller keeps it alive for the
    // Session's lifetime, exactly as it does the primary. Null means one target,
    // and everything behaves as it always has.
    Transport*  mirror_transport = nullptr;
    // How long a resumable event may sit with no activity before it is too
    // old to resume silently. Below this, a crash-and-restart just continues
    // the same recording, as it always has. At or above it, resuming would
    // risk silently swallowing a genuinely new broadcast into a leftover from
    // last week — so the caller (the OBS layer) is expected to ask the
    // operator instead of calling resume() itself. Same shape as the
    // decoder's own stale_after_ms, which makes the same kind of judgment
    // about a quiet room. See PROJECT-SCOPE.md §5.1.
    int64_t     resume_stale_after_ms = 30LL * 60 * 1000; // 30 minutes
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

    // Who this encoder is — "Main site", a room name, whatever the operator
    // called it. Stamped on every cue it drops so a shared cue list can say
    // which site set one. Empty (the default) reads as the main site.
    void set_author_name(const std::string& name);

    // The cues this event has, in the order they were published — the
    // encoder's own, plus any a LAN satellite handed in (see add_cue_from).
    // A guarded copy, safe to call from the UI thread.
    std::vector<Marker> markers() const;

    // A cue handed in by another site over the LAN, where this encoder is
    // acting as the hub because the satellite has no bucket of its own. Writes
    // the author's own cue object (so cloud readers see it too), folds it into
    // the list served over the LAN, and republishes. Returns false with
    // `error` when the cue could not be stored.
    bool add_cue_from(const std::string& author, const std::string& label,
                      std::string& error);

    // Refresh live.json's heartbeat (drives decoder stale-detection).
    void heartbeat();

    // Flush remaining spool, mark the event ended, publish final state. Runs
    // on whatever thread calls it — OBS's own UI thread, in practice — so the
    // deadline is a direct trade: an operator watching Stop feels every
    // second of it. 8s gives a healthy link (segments upload in low single
    // digits of seconds per the encoder's own logs) a real chance to clear
    // the queue, without turning a routine Stop into a UI freeze indistinct
    // from a hang — see BUGS.md for the bug that made this bound matter.
    //
    // Returns false when the event could not actually be marked ended in the
    // store — manifest.json or live.json did not land. That distinction is
    // the difference between a satellite finishing cleanly and one polling a
    // room nobody is broadcasting to until it declares the encoder dead, so
    // the caller is expected to say so rather than sign off with the tidy
    // "stopped" line it prints either way. last_error() has the detail.
    bool end(std::chrono::milliseconds drain_deadline = std::chrono::milliseconds(8000));

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

    // ── LAN / direct delivery hooks (PROJECT-SCOPE.md §8.7) ──────────────────
    // Optional, and cost nothing when unset: a Session behaves exactly as it
    // always has if a host never calls these setters. They exist so a
    // LanObjectServer — a completely separate, optional class — can serve
    // satellites on the same network the identical objects a cloud decoder
    // eventually reads, without Session knowing anything about HTTP servers,
    // sockets, or that LAN delivery exists at all.
    //
    // Fired once per event, with exactly what begin_common() already has in
    // hand: the freshly-assigned event id, event.json's own JSON (the same
    // bytes just PUT to the bucket), and the init segment. A LanObjectServer
    // uses this to bootstrap a satellite that has never seen this event
    // before.
    using EventStartedCallback = std::function<void(
        const std::string& event_id, const std::string& event_json,
        const std::vector<uint8_t>& init)>;
    void set_event_started_callback(EventStartedCallback cb) {
        m_on_event_started = std::move(cb);
    }
    // Fired every time a segment is confirmed durable — the exact moment
    // on_confirmed() already has the raw bytes in hand, before the spool file
    // that held them is removed. A LanObjectServer retains a bounded window
    // of these (see SegmentCache) so a segment already gone from the spool —
    // which is most of them, by design — is still there to serve directly.
    using SegmentConfirmedCallback = std::function<void(
        uint64_t seq, const std::vector<uint8_t>& bytes)>;
    void set_segment_confirmed_callback(SegmentConfirmedCallback cb) {
        m_on_segment_confirmed = std::move(cb);
    }
    // Fired every time the manifest is (re)published to the bucket, with the
    // exact JSON just sent — so a LAN-connected decoder's manifest.json is
    // never more than one confirm behind what a cloud decoder would
    // eventually see, and is never a separate, independently-derived copy
    // that could drift from it.
    using ManifestPublishedCallback = std::function<void(const std::string& json)>;
    void set_manifest_published_callback(ManifestPublishedCallback cb) {
        m_on_manifest_published = std::move(cb);
    }
    // Fired every time live.json is (re)published — at start, on every
    // heartbeat, and at end() — with the exact JSON just sent. Without this,
    // a LAN-only satellite (cloud delivery disabled — see NullTransport) has
    // no way to discover WHICH event is live at all: live.json is the only
    // thing that names it, and it is the one object the other two hooks
    // don't already cover.
    using LivePublishedCallback = std::function<void(const std::string& json)>;
    void set_live_published_callback(LivePublishedCallback cb) {
        m_on_live_published = std::move(cb);
    }
    // Fired every time add_marker() publishes markers.json. Without this, a
    // LAN-only satellite (cloud delivery disabled) can never receive a
    // marker at all — there is no cloud copy to fall back on for it, unlike
    // every other object, which is why this is its own hook rather than
    // being folded into manifest-published.
    using MarkersPublishedCallback = std::function<void(const std::string& json)>;
    void set_markers_published_callback(MarkersPublishedCallback cb) {
        m_on_markers_published = std::move(cb);
    }

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
    // The second target's stream. Null unless a mirror transport was supplied.
    std::unique_ptr<RetryUploader> m_mirror;

    std::string m_event_id;
    uint64_t    m_next_seq = 0;
    int64_t     m_last_heartbeat_ms = 0;
    std::string m_author_name;      // guarded by m_mtx; stamped on cues

    Manifest    m_manifest;
    MarkerList  m_markers;
    // Cues handed in over the LAN by other sites, kept apart from the
    // encoder's own so markers.json stays exactly what it always was; the two
    // are merged only for what the LAN serves.
    MarkerList  m_guest_markers;
    uint64_t    m_dropped_total = 0;   // guarded by m_mtx
    // Set once in resume(), read by status(). Empty resumed_event_id means
    // this run started fresh. Guarded by m_mtx like the rest of Status's
    // sources.
    std::string m_resumed_event_id;
    int64_t     m_resumed_event_started_ms = 0;
    uint64_t    m_resumed_already_confirmed = 0;
    std::string m_last_error;
    ProgressCallback m_on_progress;
    EventStartedCallback      m_on_event_started;
    SegmentConfirmedCallback  m_on_segment_confirmed;
    ManifestPublishedCallback m_on_manifest_published;
    LivePublishedCallback     m_on_live_published;
    MarkersPublishedCallback  m_on_markers_published;
    mutable std::mutex m_mtx;

    std::string segment_key(uint64_t seq) const;
    std::string event_prefix() const;
    bool  put_json(const std::string& key, const std::string& body);
    // Fires the markers-published hook with every cue this event has — the
    // encoder's own plus any a LAN satellite handed in — so a LAN decoder sees
    // cues from every site, including ones set at another campus.
    void  publish_lan_markers();
    bool  put_bytes(const std::string& key, const std::vector<uint8_t>& b,
                    const std::string& content_type);
    std::string publish_manifest_locked();
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
