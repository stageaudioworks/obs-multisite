// SPDX-License-Identifier: GPL-3.0-or-later
#include "session.h"

#include <chrono>
#include <random>
#include <cstdio>
#include <algorithm>

namespace multisite {

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// Crockford base32 alphabet (ULID-style)
static const char* B32 = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

std::string make_event_id(int64_t t) {
    if (t == 0) t = now_ms();
    std::string out(26, '0');
    // 10 chars of timestamp (48 bits)
    uint64_t ts = (uint64_t)t;
    for (int i = 9; i >= 0; --i) { out[i] = B32[ts & 31]; ts >>= 5; }
    // 16 chars of randomness
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<int> d(0, 31);
    for (int i = 10; i < 26; ++i) out[i] = B32[d(rng)];
    return out;
}

static std::string seq_name(uint64_t seq) {
    char b[16];
    std::snprintf(b, sizeof(b), "%08llu", (unsigned long long)seq);
    return b;
}

Session::Session(SessionConfig cfg, Transport& transport)
    : m_cfg(std::move(cfg)), m_tx(transport) {
    m_spool = std::make_unique<SpoolQueue>(m_cfg.spool_dir, m_cfg.max_spool_bytes);
    m_spool->set_drop_callback([this](const SpoolDrop& d) { on_dropped(d); });

    // How many targets must hold a segment comes from the CONFIGURATION IN
    // FORCE, not from what was persisted. A spool left at two targets after the
    // operator turns the second bucket off would wait for a confirmation that is
    // never coming, and grow until the disk cap dropped the lot — so this is
    // stated once, here, and set_targets(1) also sweeps whatever the remaining
    // target already has.
    m_spool->set_targets(m_cfg.mirror_transport ? 2 : 1);

    UploaderConfig ucfg;
    ucfg.content_type = "video/mp4";
    if (m_cfg.send_expiry_tag)
        ucfg.tags = { { m_cfg.expiry_tag_key, m_cfg.expiry_tag_val } };
    else
        ucfg.tags.clear();
    ucfg.base_backoff_ms = m_cfg.base_backoff_ms;
    ucfg.max_backoff_ms  = m_cfg.max_backoff_ms;
    ucfg.jitter          = m_cfg.backoff_jitter;
    m_uploader = std::make_unique<RetryUploader>(*m_spool, m_tx, ucfg);
    m_uploader->set_confirm_callback(
        [this](const SpooledSegment& s) { on_confirmed(s); });
    // Progress is reported after the spool entry clears, so the counters the
    // host logs (confirmed / queued) are accurate rather than off-by-one.
    m_uploader->set_post_confirm_callback([this](const SpooledSegment&) {
        if (m_on_progress) m_on_progress(status());
    });

    if (m_cfg.mirror_transport) {
        // Two targets are already in force (see set_targets above): the spool
        // keeps a segment until BOTH have it, so the mirror uploads it from the
        // same file whenever it gets a window. No second copy on disk and no
        // backlog to keep in step.
        UploaderConfig mcfg = ucfg;
        mcfg.target = 1;
        // The yield rule (PROJECT-SCOPE.md §10 Phase 9), and it is phrased as
        // "is the primary WORKING" rather than "is the primary caught up" on
        // purpose: a failed primary is never caught up, so the latter starves
        // the mirror exactly when the second copy is the only thing that
        // matters. Degraded still yields — it is trying — so this waits while
        // the primary is making progress and goes once it has stopped. The link
        // tracker supplies the hysteresis, so it is not reacting to one failed
        // request.
        mcfg.may_upload = [this] { return mirror_may_go(); };
        m_mirror = std::make_unique<RetryUploader>(*m_spool,
                                                   *m_cfg.mirror_transport,
                                                   mcfg);
        // Deliberately NO confirm callback on the mirror: the manifest still
        // advances on the primary's confirmations.
        //
        // Publication follows "the preferred target that is currently working",
        // but moving it to the mirror's confirmations before the read side can
        // fall back would publish a manifest naming segments the primary does
        // not have, and a decoder reading the primary would then be handed a
        // manifest it cannot fetch a segment of. That moves together with read
        // fallback, not before it.
    }
}

Session::~Session() {
    if (m_mirror) m_mirror->stop();
    m_obj_run = false;
    if (m_obj_thread.joinable()) m_obj_thread.join();
    if (m_uploader) m_uploader->stop();
}

std::string Session::event_prefix() const {
    return event_prefix_for(m_event_id);
}
std::string Session::segment_key(uint64_t seq) const {
    return event_prefix() + "segments/" + seq_name(seq) + ".m4s";
}

bool Session::put_bytes(const std::string& key, const std::vector<uint8_t>& b,
                        const std::string& content_type) {
    std::map<std::string, std::string> tags;
    if (m_cfg.send_expiry_tag)
        tags[m_cfg.expiry_tag_key] = m_cfg.expiry_tag_val;
    PutResult r = m_tx.put(key, b, content_type, tags);
    if (!r.success) {
        m_last_error = "PUT " + key + " -> HTTP " +
                       std::to_string(r.http_status) + " " + r.error;
    }
    // Queue it for the second bucket, rather than putting it there now: this
    // runs on the encode thread, and a second put that waits on a request
    // timeout would stall the live feed on the insurance policy. Latest wins
    // per key — a manifest is rewritten every segment and the mirror only needs
    // the current one.
    if (m_cfg.mirror_transport) {
        std::lock_guard<std::mutex> lk(m_obj_mtx);
        m_obj_pending[key] = { b, content_type };
    }
    return r.success;
}

bool Session::mirror_may_go() const {
    // One predicate for both halves of the mirror — the media stream and the
    // small objects — so they can never disagree about whether the primary is
    // working. See the note on the uploader's may_upload in the constructor.
    return m_uploader->health() != LinkHealth::Healthy || m_spool->caught_up(0);
}

void Session::mirror_objects_loop() {
    while (m_obj_run.load()) {
        if (mirror_may_go()) {
            std::map<std::string, std::pair<std::vector<uint8_t>, std::string>> batch;
            {
                std::lock_guard<std::mutex> lk(m_obj_mtx);
                batch.swap(m_obj_pending);
            }
            std::map<std::string, std::string> tags;
            if (m_cfg.send_expiry_tag)
                tags[m_cfg.expiry_tag_key] = m_cfg.expiry_tag_val;
            for (auto& kv : batch) {
                PutResult r = m_cfg.mirror_transport->put(kv.first, kv.second.first,
                                                          kv.second.second, tags);
                if (!r.success) {
                    // Put it back for a later pass — unless the key has been
                    // rewritten meanwhile, in which case the newer copy is the
                    // one that belongs there.
                    std::lock_guard<std::mutex> lk(m_obj_mtx);
                    if (!m_obj_pending.count(kv.first))
                        m_obj_pending[kv.first] = kv.second;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}
bool Session::put_json(const std::string& key, const std::string& body) {
    std::vector<uint8_t> b(body.begin(), body.end());
    return put_bytes(key, b, "application/json");
}

// Shared by Session::check_resumable() and the standalone peek_resumable()
// below, so the two can never compute staleness differently.
static void apply_staleness(ResumeInfo& info, int64_t resume_stale_after_ms) {
    if (info.resumable)
        info.stale = (now_ms() - info.last_activity_ms) > resume_stale_after_ms;
}

ResumeInfo Session::check_resumable() const {
    ResumeInfo info = m_spool->inspect();
    apply_staleness(info, m_cfg.resume_stale_after_ms);
    return info;
}

ResumeInfo peek_resumable(const std::string& spool_dir,
                          int64_t resume_stale_after_ms) {
    SpoolQueue q(spool_dir);
    ResumeInfo info = q.inspect();
    apply_staleness(info, resume_stale_after_ms);
    return info;
}

bool Session::begin_common(const std::vector<uint8_t>& init,
                           const VideoInfo& video,
                           const std::vector<AudioTrack>& tracks) {
    m_last_error.clear();
    // A Session cancels its transport when it stops using one (~Session and
    // end(), both through RetryUploader::stop()), and that cancel is sticky.
    // So a transport handed to a second Session — resuming a crashed event is
    // exactly this — arrives switched off, and every publish below would fail
    // for a reason no error message would ever explain. Re-arm it here, at
    // the one point where a Session starts issuing requests, rather than
    // relying on every caller to hand over a factory-fresh transport.
    m_tx.resume_pending();
    // event.json — static descriptor
    EventInfo ev;
    ev.event_id           = m_event_id;
    ev.room_id            = m_cfg.room_id;
    ev.name               = m_cfg.event_name;
    ev.started_at_ms      = now_ms();
    ev.first_seq          = m_next_seq;
    ev.segment_duration_s = m_cfg.segment_duration_s;
    ev.init               = "init.mp4";
    ev.video              = video;
    ev.audio_tracks       = tracks;
    if (!put_json(event_prefix() + "event.json", ev.to_json())) return false;

    // rooms/{room}/events/{id}.json — the per-room index the event list reads.
    // Deliberately NOT fatal: this only makes a past event easier to find, and
    // refusing to go live because an index entry failed to write would be a
    // event off the air for the sake of a listing convenience. A satellite
    // falls back to scanning events/ when the entry is missing.
    RoomEventEntry idx;
    idx.event_id      = m_event_id;
    idx.room_id       = m_cfg.room_id;
    idx.name          = m_cfg.event_name;
    idx.started_at_ms = ev.started_at_ms;
    if (!put_json(room_event_key(m_cfg.room_id, m_event_id), idx.to_json())) {
        m_last_error.clear();   // reported above; not a go-live failure
    }

    // init.mp4 — must exist before any segment is referenced
    if (!put_bytes(event_prefix() + "init.mp4", init, "video/mp4")) return false;

    // A LAN satellite bootstraps from exactly these two things — the same
    // event.json just published and the same init bytes — fired unlocked, on
    // principle (see publish_manifest_locked()).
    if (m_on_event_started) m_on_event_started(m_event_id, ev.to_json(), init);

    // seed manifest state
    std::string manifest_json;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_manifest = Manifest{};
        m_manifest.event_id            = m_event_id;
        m_manifest.status              = "live";
        m_manifest.name                = m_cfg.event_name;
        m_manifest.init                = "init.mp4";
        m_manifest.video               = video;
        m_manifest.audio_tracks        = tracks;
        m_manifest.first_available_seq = m_next_seq;
        m_manifest.started_at_ms       = ev.started_at_ms;
        m_manifest.updated_at_ms       = now_ms();
        manifest_json = publish_manifest_locked();
    }
    if (m_on_manifest_published) m_on_manifest_published(manifest_json);

    publish_live("live");
    m_uploader->start();
    // The mirror starts with the event. It will mostly sit yielded; that is the
    // point, and it costs one sleeping thread.
    if (m_mirror) {
        m_mirror->start();
        // …and the small objects get their own thread, for the reason in the
        // header: they are queued by put_bytes rather than put there.
        m_obj_run = true;
        m_obj_thread = std::thread([this] { mirror_objects_loop(); });
    }
    return true;
}

bool Session::start_new(const std::vector<uint8_t>& init,
                        const VideoInfo& video,
                        const std::vector<AudioTrack>& tracks) {
    m_event_id = make_event_id();
    m_next_seq = 0;
    m_spool->begin_event(m_event_id, 0);
    m_markers = MarkerList{};
    m_guest_markers = MarkerList{};
    return begin_common(init, video, tracks);
}

bool Session::resume(const std::vector<uint8_t>& init,
                     const VideoInfo& video,
                     const std::vector<AudioTrack>& tracks) {
    auto info = m_spool->inspect();
    if (!info.resumable) return false;
    m_event_id = info.event_id;
    m_next_seq = info.last_enqueued + 1;   // continue the sequence

    // Read the original event.json back for two things the spool alone
    // doesn't record: the operator's original name (kept unless they typed a
    // new one) and the wall-clock time the event actually started, which is
    // what the dock's "Resumed event from ..." line names (see
    // PROJECT-SCOPE.md §5.1). event_prefix() already uses m_event_id, set
    // above, so this reads the very event being resumed. Best-effort: a
    // failed read still resumes the event, it just can't name a time for it.
    int64_t started_at_ms = 0;
    {
        auto r = m_tx.get(event_prefix() + "event.json");
        if (r.success) {
            try {
                EventInfo old = EventInfo::from_json(
                    std::string(r.body.begin(), r.body.end()));
                if (m_cfg.event_name.empty() && !old.name.empty())
                    m_cfg.event_name = old.name;
                started_at_ms = old.started_at_ms;
            } catch (...) {}
        }
    }

    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_resumed_event_id          = m_event_id;
        m_resumed_event_started_ms  = started_at_ms;
        // Segments already confirmed before this run — what "start new"
        // instead would abandon. Sequences are zero-based, so this is exact
        // in the common case (no evictions yet); it is a display figure, not
        // a protocol guarantee.
        m_resumed_already_confirmed = info.last_confirmed + 1;
    }

    m_spool->resume_event();
    return begin_common(init, video, tracks);
}

uint64_t Session::publish_segment(std::vector<uint8_t> fragment,
                                  double duration_s, double pts_offset_s) {
    uint64_t seq = m_next_seq++;
    SpooledSegment s;
    s.seq          = seq;
    s.data         = std::move(fragment);
    s.duration_s   = duration_s;
    s.pts_offset_s = pts_offset_s;
    s.key          = segment_key(seq);
    m_spool->enqueue(std::move(s));   // durable BEFORE any upload attempt
    return seq;
}

// Called by the uploader once the store has confirmed a segment durable.
// This is the only place the manifest gains an entry — the write-ordering rule.
void Session::on_confirmed(const SpooledSegment& seg) {
  std::string manifest_json;
  {
    std::lock_guard<std::mutex> lk(m_mtx);
    ManifestSegment ms;
    ms.seq        = seg.seq;
    ms.duration_s = seg.duration_s;
    ms.checksum   = seg.checksum;
    // Content time, not upload time: event start plus this segment's offset in
    // the programme. Upload time would drift with network delays.
    ms.at_ms      = m_manifest.started_at_ms +
                    (int64_t)(seg.pts_offset_s * 1000.0);
    m_manifest.push(ms, m_cfg.manifest_window);
    m_manifest.updated_at_ms = now_ms();
    manifest_json = publish_manifest_locked();

    // Periodic heartbeat so decoders can distinguish "quiet" from "dead".
    int64_t t = now_ms();
    if (t - m_last_heartbeat_ms > (int64_t)m_cfg.heartbeat_interval_s * 1000) {
        m_last_heartbeat_ms = t;
        // publish outside the manifest lock is not required; live.json is small
        LivePointer lp;
        lp.room_id = m_cfg.room_id;
        lp.event_id = m_event_id;
        lp.status = "live";
        lp.updated_at_ms = t;
        put_json(live_pointer_key(m_cfg.room_id), lp.to_json());
    }
  }
  // This segment's bytes are exactly what a LAN satellite would otherwise
  // wait for the bucket to hand back — and, once the spool file behind it is
  // gone (which the uploader does right after this callback returns), the
  // ONLY place they still exist locally, unless the caller retains them.
  if (m_on_segment_confirmed) m_on_segment_confirmed(seg.seq, seg.data);
  if (m_on_manifest_published) m_on_manifest_published(manifest_json);
}

// A segment was dropped from the local spool for disk space, before it could
// ever be uploaded (see SessionConfig::max_spool_bytes). Nothing here may
// touch the network: this runs on the encode thread via SpoolQueue's drop
// callback, and publish_segment() is documented to never block on it. The
// advanced floor is folded into whichever manifest publish happens next
// (on_confirmed, or end()) rather than sent immediately.
void Session::on_dropped(const SpoolDrop& d) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (d.new_floor > m_manifest.first_available_seq)
        m_manifest.first_available_seq = d.new_floor;
    ++m_dropped_total;
    m_last_error = "local spool cap reached: dropped queued segment " +
                   std::to_string(d.seq) + " before it could be uploaded "
                   "(upload link has been down or overwhelmed for too long)";
}

uint64_t Session::bytes_uploaded() const {
    return m_uploader ? m_uploader->stats().bytes.load() : 0;
}

// Returns the JSON just published, so a caller can hand it to the LAN hook
// (set_manifest_published_callback) AFTER releasing m_mtx — callbacks are
// never fired locked in this file, on principle: SpoolQueue's own drop
// callback earned that rule the hard way (see spool_queue.cpp), and nothing
// here needs the exception.
std::string Session::publish_manifest_locked() {
    std::string json = m_manifest.to_json();
    put_json(event_prefix() + "manifest.json", json);
    return json;
}

void Session::publish_live(const std::string& status) {
    LivePointer lp;
    lp.room_id       = m_cfg.room_id;
    lp.event_id      = m_event_id;
    lp.status        = status;
    lp.updated_at_ms = now_ms();
    m_last_heartbeat_ms = lp.updated_at_ms;
    std::string json = lp.to_json();
    put_json(live_pointer_key(m_cfg.room_id), json);
    // Not called under m_mtx (publish_live never is — see its three callers),
    // so no unlock dance is needed here unlike publish_manifest_locked().
    if (m_on_live_published) m_on_live_published(json);
}

void Session::heartbeat() { publish_live("live"); }

void Session::set_author_name(const std::string& name) {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_author_name = name;
}

std::vector<Marker> Session::markers() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_markers.markers;
}

void Session::add_marker(const std::string& label, const std::string& type) {
    std::string json;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        Marker mk;
        mk.seq   = m_next_seq;          // marker applies at the current live edge
        mk.at_ms = now_ms();
        mk.type  = type;
        mk.label = label;
        mk.author = m_author_name;      // empty reads as the main site
        mk.id    = make_event_id(mk.at_ms);
        m_markers.markers.push_back(mk);
        json = m_markers.to_json();
        put_json(event_prefix() + "markers.json", json);
    }
    // Unlocked, on the same principle as every other hook in this file: a
    // LAN-only satellite (cloud delivery disabled — see null_transport.h)
    // has no cloud copy of markers.json to fall back on, so without this a
    // marker jump simply never works for one at all, not just serves late.
    //
    // Fired with the MERGED list — the encoder's own cues plus any a LAN
    // satellite has handed in — so a campus sees cues set at another campus.
    publish_lan_markers();
}

void Session::publish_lan_markers() {
    std::string json;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        json = merge_markers({ m_markers, m_guest_markers }).to_json();
    }
    if (m_on_markers_published) m_on_markers_published(json);
}

bool Session::add_cue_from(const std::string& author, const std::string& label,
                           std::string& error) {
    if (label.empty()) { error = "a cue needs a name"; return false; }

    Marker mk;
    std::string event_id;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        event_id  = m_event_id;
        mk.seq    = m_next_seq;          // the live edge, same as an encoder cue
        mk.at_ms  = now_ms();
        mk.type   = "cue";
        mk.label  = label;
        mk.id     = make_event_id(mk.at_ms);
        mk.author = author;
    }

    // Persist the author's OWN cue object, so a cloud reader merges it exactly
    // as it would one the site wrote itself. Read-modify-write of that one
    // object only — the same single-writer property the whole cue layout rests
    // on. A LAN-only encoder's NullTransport put() simply succeeds and nothing
    // leaves the machine, which is the intended behaviour there.
    const std::string key = cue_object_key(event_id, author);
    MarkerList mine;
    auto g = m_tx.get(key);
    if (g.success) {
        try {
            mine = MarkerList::from_json(std::string(g.body.begin(), g.body.end()));
        } catch (...) {
            // An unreadable file of this author's is replaced, not appended to.
        }
    }
    mine.markers.push_back(mk);
    if (!put_json(key, mine.to_json())) {
        error = m_last_error.empty() ? "the cue could not be stored" : m_last_error;
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_guest_markers.markers.push_back(mk);
    }
    publish_lan_markers();
    return true;
}

bool Session::end(std::chrono::milliseconds drain_deadline) {
    // stop() cancels the transport before joining its upload thread, so that
    // a join does not wait out a request's full timeout. That cancel is
    // sticky (see Transport::resume_pending()), and everything below this
    // line — the drain, the final manifest, live.json — goes through that
    // same transport. Without re-arming it here, ending a broadcast aborted
    // every one of them: the last segment never uploaded, and the event was
    // never marked ended, so satellites kept polling a room whose encoder
    // had already gone and eventually called it interrupted instead.
    //
    // Safe at this point and only at this point: stop() has joined the
    // upload thread, so nothing is in flight to lose its cancellation.
    // Stop the mirror first: it must not be uploading while the primary is
    // being drained to a deadline, and whatever it has not finished is left on
    // disk for the next run — the store-and-forward bargain the yield rule
    // makes (the segment stays until BOTH targets have it, so nothing is lost
    // by stopping here).
    if (m_mirror) m_mirror->stop();
    m_obj_run = false;
    if (m_obj_thread.joinable()) m_obj_thread.join();
    m_uploader->stop();
    m_tx.resume_pending();

    // Drain whatever is still spooled so nothing is lost on a clean stop.
    m_uploader->drain_blocking(drain_deadline);

    // These two writes are the whole of "this event is over" as far as any
    // satellite is concerned, and until now they could both fail without
    // anyone hearing about it: put_bytes() records the reason and returns,
    // end() discarded it, and the operator's log signed off with a tidy
    // "stopped: N confirmed". Clearing the error first makes the answer
    // specific to this ending rather than to anything earlier in the event.
    m_last_error.clear();

    std::string manifest_json;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_manifest.status = "ended";
        m_manifest.updated_at_ms = now_ms();
        manifest_json = publish_manifest_locked();
    }
    if (m_on_manifest_published) m_on_manifest_published(manifest_json);
    publish_live("ended");
    m_spool->mark_ended();
    return m_last_error.empty();
}

Session::Status Session::status() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    Status s;
    auto st = m_spool->state();
    s.event_id        = m_event_id;
    s.last_confirmed  = st.last_confirmed;
    s.last_enqueued   = st.last_enqueued;
    s.pending         = m_spool->pending_count();
    s.confirmed_total = m_uploader->stats().confirmed.load();
    s.bytes_uploaded  = m_uploader->stats().bytes.load();
    s.retries         = m_uploader->stats().retries.load();
    s.verify_failures = m_uploader->stats().verify_failures.load();
    s.verify_note     = m_uploader->last_verify_note();
    s.health          = m_uploader->health();
    s.dropped_for_disk = m_dropped_total;
    s.resumed_event_id           = m_resumed_event_id;
    s.resumed_event_started_ms   = m_resumed_event_started_ms;
    s.resumed_already_confirmed  = m_resumed_already_confirmed;
    return s;
}

} // namespace multisite
