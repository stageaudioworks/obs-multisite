// SPDX-License-Identifier: GPL-3.0-or-later
#include "player.h"
#include "log.h"
#include "reporter.h"
#include "screen.h"
#include "sysinfo.h"
#include "audio_plan.h"   // how wide the card is opened — one rule, one place
#include "aes67.h"        // the daemon on this box, watched from aes67_loop()
#include "update_check.h"
#include "core/playout_clock.h"
#include "core/tile_crop.h"
#include "core/collector_client.h"   // the endpoint paths + bounded HTTP call
#include "core/feed_wait.h"          // what a hold does to the fragment in hand

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <unistd.h>   // getpid() — printed in the stall warning below

namespace multisite_player {

using namespace multisite;

namespace {

// The playout clock is monotonic, not wall time: a box that corrects its clock
// by NTP mid-event must not jump the picture.
uint64_t now_ns() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<nanoseconds>(
               steady_clock::now().time_since_epoch()).count();
}

long long now_ms() {
    return (long long)(now_ns() / 1000000ULL);
}

// How long to wait before trying a sound card again after it refused.
//
// A card that is not registered *yet* is the common case and not a fault: the
// AES67 kernel module registers its card a moment after the module loads, so a
// box that boots faster than the module used to open the card, fail, and then
// stay silent until somebody pressed Apply in the settings page. A couple of
// seconds is long enough for that race to resolve, and a card that is genuinely
// gone is retried every ten seconds forever — often enough that plugging it in
// fixes it without a reboot, rare enough that it cannot fill the journal.
uint64_t retry_backoff_seconds(int failures) {
    if (failures <= 1) return 2;
    if (failures == 2) return 4;
    if (failures == 3) return 6;
    return 10;
}

// The same frame, with the sound taken out of it.
//
// Muting no longer closes the card, so something still has to be handed over
// every frame's worth of time or the card under-runs — and a card that has
// under-run stops producing samples, which on this box is the same thing as the
// AES67 stream going off the network. Silence of the frame's own shape keeps
// the output running and everything downstream in step, and because the meter is
// tapped where this is handed over, the bars fall exactly as they should.
multisite::DecodedAudioFrame silence_like(
        const multisite::DecodedAudioFrame& f) {
    multisite::DecodedAudioFrame s;
    s.sample_rate = f.sample_rate;
    s.channels    = f.channels;
    s.frames      = f.frames;
    s.track_index = f.track_index;
    s.pts_ns      = f.pts_ns;
    const int ch = f.channels > 0 ? f.channels : 1;
    s.interleaved.assign((size_t)f.frames * (size_t)ch, 0.0f);
    return s;
}

} // namespace

const char* to_string(AudioState s) {
    switch (s) {
    case AudioState::Closed: return "closed";
    case AudioState::Open:   return "open";
    case AudioState::Failed: return "failed";
    }
    return "closed";
}

namespace {

// How far ahead of its due time a frame may be released. Small, because the
// output has nowhere to buffer it — unlike OBS, which had its own queue.
constexpr uint64_t kMaxDeliveryLeadNs = 20000000ULL;      // 20 ms
// Frames waiting to go out, bounded PER STREAM rather than as one total.
//
// A flat count sounds equivalent and is not, because the thread that drains
// this queue also presents the picture and writes to ALSA — and snd_pcm_writei
// blocks until the card takes the samples. While it is blocked the queue fills
// with whatever the decoder happens to be producing, and under a flat bound
// audio can take every slot: audio frames outnumber video ones, and there is
// nothing reserving space for a picture. Video frames then wait out the 250 ms
// in enqueue() and are dropped.
//
// This is not hypothetical on this box. A six-track event once put six tracks
// into a stereo device, ALSA applied the back-pressure it should, and video
// starved behind it — a few frames a second and a playout clock that could
// never catch up (see on_audio, which now drops the other tracks). That fixed
// the six-track case; it did not reserve video any room for the next thing
// that makes the card block, and an xrun is enough.
//
// Bounding each stream separately means video's allowance never depends on
// what audio is doing. 12 video frames is 400 ms at 30fps, far more than the
// 20 ms lead this player releases on, so it is headroom for a stall rather than
// pacing. Audio frames are small, so 48 costs almost nothing.
//
// Memory is also predictable now, which matters more here than on a desktop:
// the cap is 12 video frames whatever audio does, so at 1080p (~3 MB an I420
// frame) about 36 MB rather than up to 50.
constexpr size_t   kMaxQueuedVideo = 12;
constexpr size_t   kMaxQueuedAudio = 48;
// Past this much lateness the playout clock has drifted behind — normally a
// stall waiting for a segment. Re-anchor rather than dumping a backlog.
constexpr uint64_t kClockResyncThresholdNs = 2000000000ULL;   // 2 s
// Cushion when anchoring: covers the reordering window plus jitter.
constexpr uint64_t kPlayoutCushionNs = 500000000ULL;          // 500 ms
// Keep the decoder a couple of seconds ahead of the wall clock so the first
// frames of each fragment are never late, without running seconds ahead.
constexpr uint64_t kFeedLeadNs = 2500000000ULL;               // 2.5 s
// How long the identity screen stays up after power-on, whatever is about to
// air: the box proves it is alive before the picture takes over.
constexpr uint64_t kBootSplashNs = 5000000000ULL;             // 5 s

} // namespace

Player::Player(Config cfg, VideoOutput& video, AudioOutput& audio,
               std::string config_path)
    : m_cfg(std::move(cfg)), m_config_path(std::move(config_path)),
      m_video(video), m_audio(audio) {
    m_locked = m_cfg.locked;
    m_delay_from_live_s = m_cfg.delay_from_live_s;
    m_tile_sel = m_cfg.tile_index;
}

Player::~Player() { stop(); }

Config Player::config() const {
    std::lock_guard<std::mutex> lk(m_cfg_mtx);
    return m_cfg;
}

std::string Player::reporter_state() const {
    std::lock_guard<std::mutex> lk(m_obj_mtx);
    return m_reporter ? m_reporter->last_result() : std::string();
}

bool Player::store_config(const Config& cfg, std::string& error) {
    // Saving is what makes a setting survive the next power cut, so a
    // failure here is reported rather than silently applied. A player built
    // without a path (a test) keeps it in memory only.
    if (!m_config_path.empty() && !cfg.save(m_config_path, error))
        return false;
    reconfigure(cfg);
    return true;
}

bool Player::reporter_pair_begin() {
    std::lock_guard<std::mutex> lk(m_obj_mtx);
    return m_reporter ? m_reporter->pair_begin() : false;
}

void Player::reporter_pair_cancel() {
    std::lock_guard<std::mutex> lk(m_obj_mtx);
    if (m_reporter) m_reporter->pair_cancel();
}

PairView Player::reporter_pair_view() const {
    std::lock_guard<std::mutex> lk(m_obj_mtx);
    return m_reporter ? m_reporter->pair_view() : PairView{};
}

long long Player::clock_skew_ms() const {
    // Whichever cloud leg this box reads through. Asking the typed-key one
    // alone read 0 — "nothing observed" — on every paired box, so the clock
    // warning could never appear there.
    std::shared_ptr<S3Transport> tx;
    std::shared_ptr<CloudTransport> ctx;
    { std::lock_guard<std::mutex> lk(m_obj_mtx); tx = m_transport; ctx = m_cloud_transport; }
    if (ctx) return (long long)ctx->server_clock_skew_ms();
    return tx ? (long long)tx->server_clock_skew_ms() : 0;
}

// The cloud-leg half of storage_health(), for either producer of the leg. The
// two classes share these calls by name, not by base class — Transport has no
// notion of a PoP or a probe — so this is a template rather than a virtual.
template <class Tx>
static void fill_cloud_health(Player::StorageHealth& h, Tx& tx, bool probe,
                              const std::string& probe_key) {
    // Always cheap: these come from the segment traffic already flowing, so
    // they cost nothing and describe the link actually carrying the event.
    h.endpoint     = tx.host();
    h.colo         = tx.last_colo();
    h.server       = tx.last_server();
    h.bytes_per_s  = tx.observed_download_bytes_per_s();
    h.rate_samples = tx.download_samples();

    if (!probe) {
        // Traffic having been observed at all is itself evidence the bucket is
        // reachable, without spending a request to prove it again.
        h.reachable = h.rate_samples > 0 || !h.colo.empty() || !h.server.empty();
        h.readable  = h.reachable;
        return;
    }

    const StorageProbe p = tx.probe(probe_key);
    h.reachable     = p.reachable;
    h.readable      = p.readable;
    h.http_status   = p.http_status;
    h.error         = p.error;
    h.round_trip_ms = p.round_trip_ms;
    if (!p.colo.empty())   h.colo   = p.colo;
    if (!p.server.empty()) h.server = p.server;
}

Player::StorageHealth Player::storage_health(bool probe) {
    StorageHealth h;
    const Config cfg = config();
    h.endpoint = cfg.endpoint_host.empty() ? cfg.r2_account_id : cfg.endpoint_host;
    h.bucket   = cfg.bucket;
    h.room     = cfg.room_id;
    // ANY way to reach a room — cloud, LAN, or both. See Config::configured().
    h.configured = cfg.configured();

    std::shared_ptr<S3Transport> tx;
    std::shared_ptr<CloudTransport> ctx;
    std::shared_ptr<LanTransport> lan_tx;
    std::shared_ptr<FallbackTransport> fb;
    { std::lock_guard<std::mutex> lk(m_obj_mtx);
      tx = m_transport; ctx = m_cloud_transport;
      lan_tx = m_lan_transport; fb = m_fallback_transport; }

    h.lan_configured = (lan_tx != nullptr);
    // With both configured, FallbackTransport tracks which path the most
    // recent request actually took. LAN alone (no fallback object at all —
    // see rebuild_session) has nothing to ask that of, so ask the LAN
    // transport itself whether it's actually reachable — never just assume
    // "configured" means "working", or an unplugged cable reads as healthy.
    h.lan_active = fb ? fb->last_get_was_primary()
                       : (lan_tx && lan_tx->last_request_reached_server());

    // PAIRED: the leg is the CloudTransport, and the bucket is the broker's,
    // not the (empty) typed field. This answered "the player is not running"
    // on every paired box without LAN, because it only looked for typed keys.
    if (ctx) {
        h.bucket = ctx->bucket();
        fill_cloud_health(h, *ctx, probe, live_pointer_key(cfg.room_id));
        return h;
    }

    if (!tx) {
        // No cloud leg at all — either genuinely unconfigured, or a LAN-only
        // box working exactly as intended. The figures below (colo, server,
        // throughput, probe) are cloud-specific and simply don't apply.
        if (!h.configured) h.error = "storage has not been set up yet";
        else if (!lan_tx)  h.error = "the player is not running";
        return h;
    }

    fill_cloud_health(h, *tx, probe, live_pointer_key(cfg.room_id));
    return h;
}

std::shared_ptr<DecoderSession> Player::session_ref() const {
    std::lock_guard<std::mutex> lk(m_obj_mtx);
    return m_session;
}

std::shared_ptr<CmafDecoder> Player::decoder_ref() const {
    std::lock_guard<std::mutex> lk(m_obj_mtx);
    return m_decoder;
}

std::shared_ptr<CloudIdentity> Player::cloud_identity() const {
    std::lock_guard<std::mutex> lk(m_obj_mtx);
    return m_identity;
}

std::string Player::storage_mode() const {
    const Config cfg = config();
    std::lock_guard<std::mutex> lk(m_obj_mtx);
    if (m_cloud_transport) return "paired";
    if (m_transport) return "direct";
    if (m_lan_transport) return "LAN only";
    if (cfg.paired_configured()) return "waiting";
    return "none";
}

std::string Player::storage_bucket() const {
    std::lock_guard<std::mutex> lk(m_obj_mtx);
    if (m_cloud_transport) return m_cloud_transport->bucket();
    if (m_transport) return config().bucket;
    return std::string();
}

bool Player::storage_credentials_stale() const {
    std::lock_guard<std::mutex> lk(m_obj_mtx);
    return m_cloud_transport ? m_cloud_transport->credentials_are_stale() : false;
}

std::string Player::storage_credentials_note() const {
    // One sentence for the page, decided in one place so the status line and
    // the settings panel cannot describe the same state differently.
    if (storage_mode() != "paired") return std::string();
    if (storage_credentials_stale())
        return "last known credentials — the collector could not be reached";
    return std::string();
}

void Player::serve_cloud_credentials() {
    std::shared_ptr<CloudIdentity> id;
    Config cfg;
    {
        std::lock_guard<std::mutex> lk(m_obj_mtx);
        id = m_identity;
        if (!id) return;
    }
    cfg = config();

    const long long now_ms = (long long)std::chrono::duration_cast<
        std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

    if (id->tick(now_ms) != CloudAction::Fetch) return;

    // The fetch runs OUTSIDE m_obj_mtx — it is a network call, and holding the
    // object mutex across it would freeze the page and the poll loop for as
    // long as the request takes. The identity's own state is written by
    // on_credentials() after.
    // One snapshot: rebuild_session can rewrite the enrolment on another thread,
    // and a url from one pairing with the token from another is refused.
    const multisite::Enrolment en = id->enrolment();
    // What this box was reading from before the fetch, for the rebuild rule
    // below. Empty until the first set lands, and again after a pairing is
    // cleared (the identity is reset), so a re-pairing rebuilds as it must.
    const std::string bucket_before = id->credentials().bucket;
    const std::string url =
        multisite::collector_url(en.url, multisite::kCredentialsPath);
    const multisite::HttpResult r = multisite::http_get_json(url, en.token);
    multisite::CredentialsReply reply;   // stays !ok when not reached
    if (!r.reached) {
        // Unreachable: hand the identity an empty reply so it keeps last-good
        // and schedules a retry. The event goes on.
        id->on_credentials(multisite::CredentialsReply{}, now_ms);
        plog_info("cloud credentials: collector not reached — keeping the last "
                  "known set");
    } else {
        reply = multisite::cloud_parse_credentials(r.body, (int)r.code);
        if (reply.ok) {
            plog_info("cloud credentials: fetched %s (role %s, expires in %lld s)",
                      reply.creds.bucket.c_str(),
                      reply.creds.read_write ? "read-write" : "read-only",
                      (reply.creds.expires_at_ms - now_ms) / 1000);
        } else if (reply.unpaired) {
            plog_warn("cloud credentials: the collector says this device is "
                      "UNPAIRED — stopping fetches. A running event continues on "
                      "the last known credentials until they expire.");
        } else {
            plog_warn("cloud credentials: HTTP %ld, keeping the last known set",
                      r.code);
        }
        id->on_credentials(reply, now_ms);
    }

    // Ask for a rebuild ONLY when these credentials are the storage path. On a
    // box that pairs for MONITORING and reads with typed keys — the ordinary
    // case, and the one on the bench — a fetch must not tear a working session
    // down and build it again every refresh for nothing. `storage_provider` is
    // the operator's statement of which storage they use, so it decides.
    if (cfg.storage_provider != "multisite_cloud") return;

    // And only when the fetch MOVED storage: the first bucket, or a different
    // one. This asked on every fetch, so a paired box tore its decoder down and
    // restarted playback — from the start plan, not from where it was — every
    // refresh (~7.5 minutes at a 900 s TTL), and every 5 s retry while the
    // collector was unreachable. The CloudTransport signs with a refreshed
    // token by itself; a new token is not a new session.
    if (!multisite::credentials_move_storage(bucket_before, reply)) return;

    plog_info("cloud credentials: bucket %s '%s' — rebuilding the session to "
              "read through it", bucket_before.empty() ? "is" : "moved to",
              reply.creds.bucket.c_str());
    m_transport_wanted = true;
    m_poll_now = true;
}

void Player::serve_cue_credentials() {
    std::shared_ptr<CloudIdentity> id;
    {
        std::lock_guard<std::mutex> lk(m_obj_mtx);
        id = m_identity;
    }
    if (!id || !paired_for_storage(config())) return;

    const multisite::CueStep st =
        multisite::serve_cue_credentials(*m_cue_creds, id->enrolment());
    if (!st.fetched) return;

    // Once per change, not per retry: a collector that is down is asked every
    // fifteen seconds, and the log should say so once. The expiry is left out
    // of the comparison, so a routine refresh is not a change either.
    const long long now_ms = (long long)std::chrono::duration_cast<
        std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    const multisite::CueStepLine line = multisite::cue_step_line(st, now_ms);
    const std::string key = line.text.substr(0, line.text.find(" (expires in "));
    if (line.text.empty() || key == m_cue_last_said) return;
    m_cue_last_said = key;
    switch (line.level) {
    case multisite::CueStepLine::Level::Info:  plog_info("%s", line.text.c_str()); break;
    case multisite::CueStepLine::Level::Warn:  plog_warn("%s", line.text.c_str()); break;
    case multisite::CueStepLine::Level::Error: plog_error("%s", line.text.c_str()); break;
    }
}

void Player::note_error(const std::string& what) {
    std::lock_guard<std::mutex> lk(m_err_mtx);
    m_last_error = what;
}

// ── Lifecycle ────────────────────────────────────────────────────────────────

void Player::adopt_saved_pairing(const Config& cfg) {
    if (!m_identity) m_identity = std::make_shared<CloudIdentity>();
    // The identity FOLLOWS the saved pairing, clearing included.
    //
    // This comment used to promise a clearing branch — "a box whose pairing was
    // cleared must not keep a stale enrolment" — and the code under it returned
    // without clearing anything, on the grounds that the identity might hold a
    // claim newer than the settings. It cannot: set_enrolment() is called here
    // and nowhere else in the appliance, so the identity only ever holds what
    // the settings held. Keeping the old enrolment meant a cleared box went on
    // reading storage as its old appliance until it restarted — and now that
    // the heartbeat reads the identity too, it would have gone on reporting as
    // one.
    if (cfg.reporter_url.empty() || cfg.reporter_appliance_id.empty() ||
        cfg.reporter_token.empty()) {
        if (m_identity->paired()) {
            plog_info("cloud identity: pairing cleared — no longer reading or "
                      "reporting as %s", m_identity->appliance_id().c_str());
            m_identity->reset();
            m_cue_creds->reset();   // they were the old pairing's too
        }
        return;
    }
    // Idempotent: rebuilding the session is frequent, and re-enrolling would
    // throw away credentials the fetcher has already collected. Compared as one
    // snapshot — the enrolment's three strings only mean anything together.
    const multisite::Enrolment cur = m_identity->enrolment();
    if (cur.paired() && cur.url == cfg.reporter_url &&
        cur.id == cfg.reporter_appliance_id && cur.token == cfg.reporter_token)
        return;
    m_identity->set_enrolment(cfg.reporter_url, cfg.reporter_appliance_id,
                              cfg.reporter_token);
    m_cue_creds->reset();   // a new appliance holds none of the old one's
}

bool Player::paired_for_storage(const Config& cfg) const {
    // The operator has to have CHOSEN Multisite Cloud as this box's storage
    // provider. A pairing alone is not that choice: a box pairs so it can be
    // monitored, and it is entirely normal for it to pair while still reading
    // storage with typed keys (PROJECT-SCOPE §8.5: brokered credentials are
    // added BESIDE the typed keys, never instead of them).
    //
    // This is not a nicety. Without it, a box with typed keys that also
    // heartbeats was switched onto brokered storage the moment its first
    // credential fetch landed — mid-event — which is not what its operator
    // asked for and is what crashed a bench Pi on 2026-09-22.
    if (cfg.storage_provider != "multisite_cloud") return false;
    if (!cfg.paired_configured()) return false;
    if (!m_identity) return false;
    // Pairing alone is still not enough: the credential set is what a transport
    // needs, and until one has been fetched there is nothing to read with. A
    // box in that window falls through to the typed leg if it has one.
    return m_identity->credentials().present();
}

void Player::rebuild_session() {
    Config cfg = config();

    m_transport.reset();
    m_cloud_transport.reset();
    m_cue_writer.reset();
    m_lan_transport.reset();
    m_fallback_transport.reset();
    m_session.reset();
    m_catalog.reset();

    if (!cfg.configured()) {
        plog_warn("no storage configured — open the web interface and enter "
                  "the bucket details or a LAN host");
        note_error("no storage configured");
        return;
    }

    // Pick up a saved pairing before deciding which leg to build: an
    // already-paired box must work on upgrade with no operator action.
    adopt_saved_pairing(cfg);

    // Two producers of the cloud leg, and nothing downstream learns which it
    // got (docs/scope/phase12-capability-map.md):
    //
    //   PAIRED  — the collector minted credentials, so the bucket, endpoint and
    //             keys all come from the identity. No typed key is consulted,
    //             which is the whole point: a paired box cannot read storage
    //             with a pasted key because it has none.
    //   DIRECT  — the operator typed keys, unchanged from before.
    //
    // The identity is made once and kept, so the reporter and the transport
    // share it — one identity for storage and monitoring both.
    if (!m_identity) m_identity = std::make_shared<CloudIdentity>();
    const CloudRole role = CloudRole::Decoder;   // a campus reads
    if (paired_for_storage(cfg)) {
        CloudStorageConfig csc;
        csc.region = cfg.region;
        m_cloud_transport =
            std::make_shared<CloudTransport>(*m_identity, role, csc);
        // The cue credential's writer, beside the read-only leg. Shorter
        // timeouts than a segment read: a cue is dropped from a button, and the
        // person pressing it is waiting for the answer.
        CloudStorageConfig cue_csc = csc;
        cue_csc.request_timeout_ms = 10000;
        m_cue_writer = std::make_shared<CueWriter>(m_cue_creds, cue_csc);
    } else if (cfg.cloud_configured()) {
        S3Config s3;
        s3.endpoint_host     = cfg.endpoint_host;
        s3.r2_account_id     = cfg.r2_account_id;
        s3.bucket            = cfg.bucket;
        s3.access_key_id     = cfg.access_key_id;
        s3.secret_access_key = cfg.secret_access_key;
        s3.region            = cfg.region;
        m_transport = std::make_shared<S3Transport>(s3);
    }
    if (cfg.lan_configured()) {
        LanTransportConfig lcfg;
        lcfg.host       = cfg.lan_host;
        lcfg.port       = cfg.lan_port;
        lcfg.auth_token = cfg.lan_auth_token;
        m_lan_transport = std::make_shared<LanTransport>(lcfg);
    }

    // The cloud leg, whichever producer supplied it. A null here with LAN
    // configured is the LAN-only case; a null with neither is caught by
    // cfg.configured() above.
    Transport* cloud = m_cloud_transport
        ? static_cast<Transport*>(m_cloud_transport.get())
        : static_cast<Transport*>(m_transport.get());

    // Preference and fallback (§8.7): LAN answers when it can, cloud
    // otherwise, decided per request — see fallback_transport.h. Exactly one
    // of the three is real when only one leg is configured; cfg.configured()
    // above already guarantees at least one is.
    Transport* active = nullptr;
    if (m_lan_transport && cloud) {
        m_fallback_transport = std::make_shared<FallbackTransport>(*m_lan_transport, *cloud);
        active = m_fallback_transport.get();
    } else if (m_lan_transport) {
        active = m_lan_transport.get();
    } else {
        active = cloud;
    }

    DecoderConfig dc;
    dc.room_id              = cfg.room_id;
    dc.cache_dir            = cfg.cache_dir;
    dc.prebuffer_segments   = cfg.prebuffer_segments;
    dc.start_buffer_seconds = cfg.start_buffer_seconds;
    dc.buffer_minutes       = cfg.buffer_minutes;
    dc.max_cached_segments  = cfg.max_cached_segments;
    dc.keep_behind_segments = cfg.keep_behind_segments;
    dc.stale_after_ms       = cfg.stale_after_ms;
    dc.pinned_event_id      = cfg.pinned_event_id;
    // An appliance is usually unattended: when the event it is relaying ends it
    // should pick up the next one the room starts, not sit on the finished
    // recording waiting for someone to press Follow live. (There IS a Follow
    // live control on the web page — /api/follow-live — for the case where an
    // operator wants to hold a recording deliberately.) follow_next_event is the
    // operator-facing form of this; the config default is to follow.
    dc.hold_finished_event  = !cfg.follow_next_event;
    // Cues: who this box is, and whether it may drop one. See DecoderConfig.
    dc.author_name          = cfg.site_name;
    dc.can_author_cues      = !cfg.site_name.empty() && cfg.configured();
    // MULTISITE CLOUD: the cue goes to the collector's cue credential first;
    // the hub below, when there is LAN, is its fallback (see add_cue).
    if (m_cue_writer) {
        auto w = m_cue_writer;
        dc.cue_target = [w](const std::string& event_id) { return w->target(event_id); };
    }
    if (m_lan_transport && !m_transport) {
        // A cue goes to the encoder's hub only when there is no bucket to write
        // to. With cloud configured the cue is written directly, so a
        // configured-but-unreachable LAN host cannot break cue authoring.
        auto lan = m_lan_transport;
        dc.cue_hub = [lan](const std::string& author, const std::string& label,
                           std::string& merged, std::string& error) {
            return lan->publish_cue(author, label, merged, error);
        };
    }
    // A PAIRED box that has not fetched credentials yet reaches here with no
    // cloud leg and no LAN leg: it is configured (the pairing names a
    // collector) but has nothing to read through until the first fetch lands.
    // That is a waiting state, not an error — say so and let the poll loop
    // rebuild once credentials arrive, rather than falling through to a null
    // dereference.
    if (!active) {
        plog_info("paired but no cloud credentials yet — waiting for the "
                  "collector before reading storage");
        note_error("waiting for cloud credentials");
        return;
    }

    m_session = std::make_shared<DecoderSession>(dc, *active);

    // Event browsing (§7.5) is inherently cloud-only — there is no such
    // thing as "list every event a LAN endpoint has ever served"; it only
    // ever knows about whichever one is live right now. No catalog at all
    // when cloud isn't configured, rather than one that can only ever come
    // back empty and reads as a room with no history.
    //
    // Either producer of the cloud leg. This asked for the typed-key transport
    // alone, so a PAIRED box had no catalog and its page could not list a
    // single recording — the broker's bucket holds them just the same.
    Transport* catalog_tx = m_cloud_transport
        ? static_cast<Transport*>(m_cloud_transport.get())
        : static_cast<Transport*>(m_transport.get());
    if (catalog_tx) {
        CatalogConfig cc;
        cc.room_id        = cfg.room_id;
        cc.stale_after_ms = cfg.stale_after_ms;
        m_catalog = std::make_shared<EventCatalog>(cc, *catalog_tx);
    }

    if (m_transport && m_lan_transport)
        plog_info("receiving room '%s' — LAN preferred (%s:%d), cloud fallback at %s",
                  cfg.room_id.c_str(), cfg.lan_host.c_str(), cfg.lan_port,
                  m_transport->base_url().c_str());
    else if (m_cloud_transport && m_lan_transport)
        plog_info("receiving room '%s' — LAN preferred (%s:%d), Multisite Cloud "
                  "fallback, bucket '%s'", cfg.room_id.c_str(),
                  cfg.lan_host.c_str(), cfg.lan_port,
                  m_cloud_transport->bucket().c_str());
    else if (m_lan_transport)
        plog_info("receiving room '%s' — LAN only (%s:%d), no cloud storage configured",
                  cfg.room_id.c_str(), cfg.lan_host.c_str(), cfg.lan_port);
    else if (m_cloud_transport)
        // PAIRED: the endpoint and bucket came from the collector, so there is
        // no typed base URL to print — name the bucket the broker chose, which
        // is the useful fact for an operator and the one they cannot see
        // anywhere else.
        plog_info("receiving room '%s' from Multisite Cloud — bucket '%s'",
                  cfg.room_id.c_str(), m_cloud_transport->bucket().c_str());
    else if (m_transport)
        plog_info("receiving room '%s' from %s", cfg.room_id.c_str(),
                  m_transport->base_url().c_str());
    // No else: a paired box with no credentials yet already returned above with
    // its own "waiting" line. Reaching here with no leg at all is the
    // !configured() case, handled at the top.
    note_error("");
}

void Player::start() {
    if (m_running.exchange(true)) return;

    {
        std::lock_guard<std::mutex> lk(m_obj_mtx);
        rebuild_session();
    }

    // The identity screen comes up immediately and stays for a few seconds,
    // even though an event may be about to take the picture over.
    m_boot_splash_until_ns = now_ns() + kBootSplashNs;
    m_boot_splash_drawn = false;

    m_poll_thread    = std::thread([this] { poll_loop(); });
    m_feed_thread    = std::thread([this] { feed_loop(); });
    m_deliver_thread = std::thread([this] { deliver_loop(); });
    m_aes67_thread   = std::thread([this] { aes67_loop(); });

    // The monitoring heartbeat. Its own thread, started whether or not
    // anything is playing — an idle box still reports every five minutes —
    // and off by default, in which case it is one sleeping thread that sends
    // nothing. Reads the config every tick, so Save needs no restart here.
    {
        std::lock_guard<std::mutex> lk(m_obj_mtx);
        if (!m_reporter) m_reporter = std::make_unique<Reporter>();
        m_reporter->start(*this);
    }

    m_events_wanted = true;

    // One request to GitHub, in the background, so the page can say a newer
    // build exists. It is started here rather than in main() because this is
    // where the rest of the player's long-running work begins, and it is a
    // detached thread that fails quietly — an update check must never be the
    // reason a box does not come up.
    {
        const Config cfg = config();
        update_check_start(player_version(), cfg.check_updates);
    }

    // An appliance that needs somebody to press Play after a power cut is not
    // an appliance.
    if (config().auto_play) {
        m_playing = true;
        plog_info("auto-play is on — going to air as soon as there is a feed");
    }
}

void Player::stop() {
    m_flushing = true;                 // release a decoder blocked on the queue
    if (!m_running.exchange(false)) { m_flushing = false; return; }
    // The reporter only ever reads (status, config, observed health), but it
    // is stopped before the teardown below all the same: nothing should be
    // sampling state while the session it samples is being freed.
    {
        std::lock_guard<std::mutex> lk(m_obj_mtx);
        if (m_reporter) m_reporter->stop();
    }
    m_dq_cv.notify_all();
    if (m_poll_thread.joinable())    m_poll_thread.join();
    if (m_feed_thread.joinable())    m_feed_thread.join();
    if (m_deliver_thread.joinable()) m_deliver_thread.join();
    if (m_aes67_thread.joinable())   m_aes67_thread.join();
    m_flushing = false;

    teardown_decoder();
    {
        std::lock_guard<std::mutex> lk(m_dq_mtx);
        m_dq.clear();
    }
    {
        std::lock_guard<std::mutex> lk(m_obj_mtx);
        // session first: it holds a Transport& into whichever of these it
        // was actually built against (see rebuild_session), so nothing may
        // be freed before it is.
        m_session.reset();
        m_catalog.reset();
        m_fallback_transport.reset();
        m_lan_transport.reset();
        m_transport.reset();
        m_cloud_transport.reset();
    }
    // Never leave the last frame of an event on a screen in an empty room.
    m_video.blank();
    m_audio.close();
    m_audio_open_state = (int)AudioState::Closed;
    m_audio_opened_rate.store(0);
    m_audio_opened_channels.store(0);
    m_meter.forget();
}

void Player::teardown_decoder() {
    // The next decoder starts from nothing and cannot decode a bare fragment,
    // so the session must hand the init segment out again. Doing it here
    // rather than at each call site is the point: pinning an event tore the
    // decoder down without it, and every segment that arrived while init.mp4
    // downloaded was then rejected and dropped.
    if (auto sess = session_ref()) sess->request_init();

    std::shared_ptr<CmafDecoder> old;
    {
        std::lock_guard<std::mutex> lk(m_obj_mtx);
        old = m_decoder;
        m_decoder.reset();
    }
    // stop() joins the decode thread and can block; never under the lock.
    if (old) old->stop();
    m_decoder_started = false;
    m_first_pts_ns = -1;
    m_logged_av_offset = false;
    m_last_video_pts_ns = 0;
}

void Player::flush_delivery() {
    m_flushing = true;
    {
        std::lock_guard<std::mutex> lk(m_dq_mtx);
        m_dq.clear();
    }
    m_dq_cv.notify_all();
    m_flushing = false;
    m_first_pts_ns = -1;               // the next frame re-anchors the clock
    m_audio.flush();
    m_dq_cv.notify_all();
}

void Player::reconfigure(const Config& cfg) {
    Config before = config();
    {
        std::lock_guard<std::mutex> lk(m_cfg_mtx);
        m_cfg = cfg;
    }
    m_locked = cfg.locked;
    m_delay_from_live_s = cfg.delay_from_live_s;
    // Which tile is shown is applied by the present path on the next frame, so
    // it never interrupts an event the way a resolution change has to.
    m_tile_sel = cfg.tile_index;

    // Only a change to what is being received justifies taking the picture
    // away. Editing the idle colour must not interrupt an event.
    // storage_provider is here because it decides which leg is built at all
    // (typed keys or the paired CloudTransport). It was missing, and nothing
    // noticed: every credential fetch rebuilt the session anyway, so a switch
    // took effect within one refresh. With the fetch rebuilding only when the
    // bucket moves, a box already paired for monitoring would never switch.
    const bool receive_changed =
        before.storage_provider   != cfg.storage_provider ||
        before.endpoint_host      != cfg.endpoint_host ||
        before.r2_account_id      != cfg.r2_account_id ||
        before.bucket             != cfg.bucket ||
        before.access_key_id      != cfg.access_key_id ||
        before.secret_access_key  != cfg.secret_access_key ||
        before.region             != cfg.region ||
        before.lan_host           != cfg.lan_host ||
        before.lan_port           != cfg.lan_port ||
        before.lan_auth_token     != cfg.lan_auth_token ||
        before.room_id            != cfg.room_id ||
        before.cache_dir          != cfg.cache_dir ||
        before.prebuffer_segments != cfg.prebuffer_segments ||
        before.start_buffer_seconds != cfg.start_buffer_seconds ||
        before.buffer_minutes     != cfg.buffer_minutes ||
        before.keep_behind_segments != cfg.keep_behind_segments ||
        before.max_cached_segments  != cfg.max_cached_segments ||
        before.stale_after_ms     != cfg.stale_after_ms;

    // Settings that decide where picture and sound come OUT are applied by
    // reopening those outputs, not by restarting the receive path. Changing
    // the resolution must not tear down an event's buffer, and a setting
    // that only takes effect after a reboot is no use on a box with no
    // keyboard.
    const bool display_changed =
        before.drm_card   != cfg.drm_card   ||
        before.connector  != cfg.connector  ||
        before.out_width  != cfg.out_width  ||
        before.out_height != cfg.out_height ||
        before.out_fps    != cfg.out_fps;

    if (display_changed) {
        plog_info("display settings changed — resetting the output");
        m_video.close();
        std::string err;
        if (!m_video.open(cfg, err)) {
            plog_error("display: %s", err.c_str());
            note_error("display: " + err);
        } else {
            plog_info("display: %s", m_video.description().c_str());
        }
        // Whatever was on the screen belongs to the old mode.
        m_idle_showing = false;
        m_last_frame_ns = 0;
    }

    // The device, the width and the network switch all decide how and where the
    // card is opened, so all four are compared. `aes67_manage` and
    // `aes67_channels` were missing from this test, which meant switching the
    // network output on did not reopen the sound at the width it publishes: the
    // card stayed at whatever it was opened as — two channels, before any feed
    // had arrived — and the eight-channel stream it published was two channels
    // wide for ever after.
    //
    // Muting is deliberately NOT in this list. Muting used to close the card,
    // which on a box whose sound leaves over the network takes the stream off
    // air: receivers drop it, and un-muting does not get it back until they
    // re-subscribe. Mute is honoured where the samples are handed over instead
    // (the delivery loop writes silence of the frame's own shape while muted),
    // so the stream stays up and carries silence, which is what a mute should
    // sound like — and the meters, tapped at that same handover, fall with it.
    if (before.alsa_device    != cfg.alsa_device ||
        before.audio_channels != cfg.audio_channels ||
        before.aes67_manage   != cfg.aes67_manage ||
        before.aes67_channels != cfg.aes67_channels) {
        plog_info("sound settings changed — reopening the output");
        request_audio_reopen();
    }

    if (!receive_changed) {
        plog_info("settings saved");
        return;
    }

    plog_info("storage settings changed — reconnecting");
    teardown_decoder();
    flush_delivery();
    {
        std::lock_guard<std::mutex> lk(m_obj_mtx);
        rebuild_session();
    }
    m_events_wanted = true;
    m_poll_now = true;
}

// ── Frame arrival (decode thread) ────────────────────────────────────────────

int64_t Player::anchor_pts(int64_t pts_ns, bool is_video) {
    int64_t first = m_first_pts_ns.load();
    if (first < 0) {
        m_first_pts_ns = pts_ns;
        m_playout_base_ns = now_ns() + kPlayoutCushionNs;
        first = pts_ns;
        plog_debug("playout anchored on first %s frame (pts %.3fs)",
                   is_video ? "video" : "audio", (double)pts_ns / 1e9);
    }
    return first;
}

void Player::enqueue(PendingFrame&& f) {
    std::unique_lock<std::mutex> lk(m_dq_mtx);
    // Bounded wait. Waiting indefinitely for space would let the decoder's
    // thread block inside this callback whenever delivery stopped draining,
    // and CmafDecoder::stop() would then join a thread that could never
    // finish. Dropping a frame is far better than hanging the appliance.
    // Counted on demand rather than kept in parallel counters: the queue is at
    // most 60 items and counters would have to stay in step with every drain,
    // flush and clear, which is how they drift and stall the decoder against a
    // queue that is not actually full.
    const bool want_video = f.is_video;
    const bool space = m_dq_cv.wait_for(lk, std::chrono::milliseconds(250),
        [this, want_video] {
            if (!m_running.load() || m_flushing.load()) return true;
            // Stopped: nothing is going to be delivered, so nothing should be
            // queued either. Checked in the predicate as well as below so this
            // returns at once rather than sitting out the full wait — a decoder
            // callback that blocks here is a callback that cannot be joined, and
            // push_fragment would then back up behind it.
            if (!m_playing.load()) return true;
            const size_t n = (size_t)std::count_if(
                m_dq.begin(), m_dq.end(),
                [want_video](const PendingFrame& q) {
                    return q.is_video == want_video;
                });
            return n < (want_video ? kMaxQueuedVideo : kMaxQueuedAudio);
        });
    if (!m_running.load() || m_flushing.load()) return;
    // Stopped. Frames decoded before the operator pressed Stop must not be held
    // for the next play: they belong to where playback used to be, and the queue
    // has been flushed and the clock re-anchored for exactly that reason. Holding
    // them would also re-fill the queue for as long as the decoder runs, which is
    // the fault that made Stop look like it did nothing.
    if (!m_playing.load()) return;
    if (!space) {
        // Split by stream: a dropped picture repeats the last one, a dropped
        // audio frame is a hole you can hear, and the totals could not tell
        // them apart. Which one is climbing says where to look.
        m_frames_dropped++;
        if (want_video) m_dropped_video++; else m_dropped_audio++;
        return;
    }
    m_dq.push_back(std::move(f));
    lk.unlock();
    m_dq_cv.notify_all();
}

void Player::on_video(const DecodedVideoFrame& f) {
    if (!m_running.load()) return;
    // The decode thread has not opened a fragment when the decoder is created,
    // so none of this is known there — it printed 0x0 and no audio tracks
    // every time. By the first frame it is all real.
    if (!m_logged_stream.exchange(true)) {
        if (auto dec = decoder_ref()) {
            // A decoder that runs its own thread pool reports no count, and
            // "0 thread(s)" next to a picture holding full frame rate reads
            // like a fault rather than the arrangement it is.
            const int threads = dec->decode_threads();
            char how[48];
            if (threads > 0) std::snprintf(how, sizeof(how),
                                           "on %d thread(s)", threads);
            else std::snprintf(how, sizeof(how), "on its own threads");
            plog_info("stream: %s %dx%d, %d audio track(s), decoding %s",
                      dec->video_codec().c_str(), dec->video_width(),
                      dec->video_height(), dec->audio_track_count(), how);
        }
    }
    const int64_t first = anchor_pts(f.pts_ns, true);
    m_last_video_pts_ns = f.pts_ns;

    PendingFrame item;
    item.is_video = true;
    item.due_ns   = multisite::playout_due_ns(
        m_playout_base_ns.load(), f.pts_ns, first);
    item.video    = f;                 // deep copy; owns its planes
    fix_planes(item.video);
    enqueue(std::move(item));
}

void Player::on_audio(const DecodedAudioFrame& f) {
    if (!m_running.load()) return;

    // One track goes to air. OBS publishes up to six and an event carries
    // every one it was told to, so a six-track event used to put all six
    // into the same stereo device: six times real time into an output that
    // accepts one, on the thread that also presents video. ALSA applied the
    // back-pressure it should, and video starved behind it — a few frames a
    // second and a playout clock that could never catch up. Dropping the
    // other tracks here keeps them out of the queue as well as off the card.
    {
        const int wanted = config().audio_track;
        if (f.track_index != wanted) {
            if (!m_logged_audio_tracks.exchange(true))
                plog_info("this event carries more than one audio track — "
                          "playing track %d, ignoring the rest", wanted);
            return;
        }
    }

    const int64_t first = anchor_pts(f.pts_ns, false);

    if (!m_logged_av_offset && m_last_video_pts_ns != 0) {
        m_logged_av_offset = true;
        plog_debug("audio/video pts offset %.3fs (should be near zero)",
                   (double)(f.pts_ns - m_last_video_pts_ns) / 1e9);
    }

    PendingFrame item;
    item.is_video = false;
    item.due_ns   = multisite::playout_due_ns(
        m_playout_base_ns.load(), f.pts_ns, first);
    item.audio    = f;
    enqueue(std::move(item));
}

// ── Opening the sound card ───────────────────────────────────────────────────

// One place that opens, reopens and re-checks the sound card, because there are
// three moments that need it and they must not disagree: a frame arriving, the
// device being changed in the interface, and the box sitting idle.
//
// The idle case is not an optimisation. On a box whose sound is going onto the
// network, the AES67 daemon publishes what is written to the AES67 card, so an
// output that has never been opened is a stream that does not exist — and a
// satellite between services is idle for far more of the day than it is
// playing. Before this, the card was opened only when a frame arrived, so a box
// that had not played anything since it was switched on had nothing on the
// network at all, and changing the device on an idle box did nothing until
// something played.
//
// The rate is the feed's own when a frame has been seen, and 48 kHz before
// that, which is what every feed this project produces uses: CMAF at the
// encoder, and the AES67 stack on this box.
void Player::ensure_audio_open(const Config& cfg, int feed_channels,
                               int feed_rate) {
    if (feed_rate > 0) m_audio_rate.store(feed_rate);

    // What the rate and the width should be, from the one place that decides
    // both. 0 means "there is nothing to open for yet" and is a legitimate
    // answer — a box with no width set and no feed seen — not a reason to guess.
    int rate = m_audio_rate.load();
    if (rate <= 0) rate = 48000;
    const int want_channels = desired_audio_channels(cfg, feed_channels);

    const AudioState state = (AudioState)m_audio_open_state.load();
    const int  opened_rate = m_audio_opened_rate.load();
    const int  opened_ch   = m_audio_opened_channels.load();

    // Either the device was changed in the interface, or what the card was
    // opened as is no longer what it should be — a feed that turns out not to
    // run at the assumed 48 kHz, or (the fault this comparison was added for) a
    // card opened two channels wide against an eight-channel feed, which plays
    // the wrong channels to everybody listening and is reported by nothing.
    const bool reopen_asked = m_audio_reopen_requests.load() > 0;
    const bool rate_changed =
        state == AudioState::Open && feed_rate > 0 && feed_rate != opened_rate;
    const bool width_changed =
        state == AudioState::Open && want_channels > 0 &&
        want_channels != opened_ch;
    // A failed open is retried on its own schedule rather than every tick: a
    // card that is not registered yet may become registered, and a box that has
    // to be told to go and look at the settings to bring its own sound back is
    // not an appliance.
    const bool retry_due =
        state == AudioState::Failed && now_ns() >= m_audio_retry_at_ns.load();

    if (reopen_asked || rate_changed || width_changed || retry_due) {
        if (reopen_asked)
            m_audio_reopen_requests.fetch_sub(1);
        // Only a real change says so: an attempted retry that goes on failing
        // must not print two lines every few seconds for the rest of the year.
        if ((rate_changed || width_changed) && state == AudioState::Open)
            plog_info("audio out: reopening — %d Hz/%d ch, card was %d Hz/%d ch",
                      rate, want_channels, opened_rate, opened_ch);
        m_audio.close();
        m_audio_open_state = (int)AudioState::Closed;
        m_audio_opened_rate.store(0);
        m_audio_opened_channels.store(0);
        // The bars fall at the instant the card goes, rather than showing the
        // last thing heard before it went away for another 400 ms.
        m_meter.forget();
    }

    if (m_audio_open_state.load() == (int)AudioState::Open) return;

    // Nothing to open for yet. Not an error and not a failure: a box with no
    // width set and no frame decoded has simply nothing to say about how wide
    // the card should be, and the first frame settles it.
    if (want_channels <= 0) return;

    // A failure that is still in its backoff window is left alone.
    if (m_audio_open_state.load() == (int)AudioState::Failed &&
        now_ns() < m_audio_retry_at_ns.load())
        return;

    std::string err;
    if (m_audio.open(cfg, rate, want_channels, err)) {
        m_audio_open_state = (int)AudioState::Open;
        m_audio_opened_rate.store(rate);
        m_audio_opened_channels.store(want_channels);
        m_audio_fail_count.store(0);
        m_audio_retry_at_ns.store(0);
        {
            std::lock_guard<std::mutex> lk(m_audio_err_mtx);
            m_audio_last_error.clear();
        }
        plog_info("audio out: %s", m_audio.description().c_str());
    } else {
        // Marked failed, NOT open. This is the difference between a card that
        // recovers by itself and one that stays silent until somebody goes to
        // the settings page and presses Apply — which is exactly what a card
        // that loses a race with its own driver at boot used to do.
        m_audio_open_state = (int)AudioState::Failed;
        const int fails = m_audio_fail_count.fetch_add(1) + 1;
        const uint64_t backoff = retry_backoff_seconds(fails) * 1000000000ULL;
        m_audio_retry_at_ns.store(now_ns() + backoff);
        {
            std::lock_guard<std::mutex> lk(m_audio_err_mtx);
            m_audio_last_error = err;
        }
        plog_error("audio out failed: %s — will try again in %u s",
                   err.c_str(), (unsigned)retry_backoff_seconds(fails));
        note_error("audio output: " + err);
    }
}

void Player::request_audio_reopen() {
    m_audio_reopen_requests.fetch_add(1);
}

AudioMeterView Player::audio_meter() const {
    const Config cfg = config();
    const MeterReading reading = m_meter.read(now_ns());
    const bool card_open = m_audio_open_state.load() == (int)AudioState::Open;

    bool any_signal = false;
    for (float v : reading.peak) {
        if (v > 0.0f) { any_signal = true; break; }
    }

    AudioMeterView out;
    out.live = reading.live && card_open;
    out.reason = meter_reason(cfg.audio_enabled, card_open, reading.live,
                              any_signal);
    out.reason_text = meter_reason_text(out.reason);
    out.peak = reading.peak;
    out.db.reserve(reading.peak.size());
    for (float v : reading.peak) out.db.push_back(meter_dbfs(v));

    // A card that is closed has no width to draw, so the page is told the width
    // the settings ask for instead. Otherwise the bars would vanish every time
    // the box was quiet, and the panel would jump as they came back.
    if (out.peak.empty()) {
        const int want = desired_audio_channels(cfg, 0);
        if (want > 0) {
            out.peak.assign((size_t)want, 0.0f);
            out.db.assign((size_t)want, meter_dbfs(0.0f));
        }
    }
    return out;
}

// ── Keeping the network stream up ────────────────────────────────────────────

void Player::aes67_loop() {
    // Nothing to watch on a box that is not asked to manage the network sound,
    // which is the great majority of them. The thread is started anyway and
    // sleeps: starting and joining it from here keeps the box's thread lifetime
    // in one place, and a pass on an unmanaged box costs one config read.
    constexpr auto kTick = std::chrono::seconds(4);
    auto next = std::chrono::steady_clock::now();

    while (m_running.load()) {
        next += kTick;
        const Config cfg = config();
        if (cfg.aes67_manage && !cfg.alsa_device.empty()) {
            const char* action = aes67_reconcile_once();
            // One line per change of mind, never per tick. A box with no PTP
            // master sits in "waiting for the clock" indefinitely, and saying so
            // every four seconds for months is how a journal becomes unreadable
            // — which is exactly when somebody needs to read it.
            if (action != nullptr && *action != '\0') {
                if (m_aes67_last_action != action) {
                    plog_info("aes67: %s", action);
                    m_aes67_last_action = action;
                    m_aes67_last_log_ns = now_ns();
                } else if (now_ns() - m_aes67_last_log_ns > 300000000000ULL) {
                    // The same thing, still true, five minutes later: worth
                    // repeating once so a log gathered at the end of the day
                    // still shows it was happening.
                    plog_info("aes67: still %s", action);
                    m_aes67_last_log_ns = now_ns();
                }
            }
        }

        // Sleep in short steps so a stop is prompt rather than up to four
        // seconds of systemd waiting for the process to go.
        while (m_running.load() &&
               std::chrono::steady_clock::now() < next) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    plog_info("aes67 watch stopped");
}

const char* Player::aes67_reconcile_once() {
    // Serialised against the switch in the web interface, which takes the same
    // lock: an operator turning the stream off must not have a repair that was
    // already in flight turn it back on a moment later.
    std::lock_guard<std::mutex> lk(m_aes67_mtx);

    const Config cfg = config();

    // The daemon's own idea of the source is compared with the width and the
    // address this box publishes, so "is it set up right?" is a comparison and
    // not a second opinion.
    const int width = aes67_channels_to_map(cfg.aes67_channels);
    const std::string address =
        cfg.aes67_address.empty() ? aes67_default_address() : cfg.aes67_address;

    const Aes67State st = aes67_probe(cfg.alsa_device, width, address);
    // Cached for the status line (atomics, not under m_aes67_mtx — see the
    // header). Written here because this is the only place the probe runs.
    m_aes67_ptp_known  = st.ptp_known;
    m_aes67_ptp_locked = st.ptp_locked;
    m_aes67_ptp_jitter = st.ptp_jitter;
    const Aes67Action action = aes67_converge_action(st, cfg.aes67_manage,
                                                     st.source_correct);
    switch (action) {
    case Aes67Action::None:
    case Aes67Action::WaitForClock:
        // Waiting is not doing nothing quietly: the interface says which of the
        // two it is. Nothing is printed here beyond the transition line above.
        return action == Aes67Action::None ? "" : aes67_action_word(action);

    case Aes67Action::StartService: {
        // Started *and* enabled: enabling is what makes the stream come back
        // after a power cut, which is the promise the switch makes.
        const std::string problem = aes67_set_service(true, true);
        if (!problem.empty()) {
            note_error("aes67: " + problem);
            return "";
        }
        return aes67_action_word(action);
    }

    case Aes67Action::EnsureSource: {
        // The source is put back exactly as the operator asked for it, on/off
        // included: this path is only ever reached with it switched on, and
        // "on" is what a box that is managing the network sound should be.
        const std::string name = "Multisite " + hostname();
        const std::string problem =
            aes67_ensure_source(width, address, name, true);
        if (!problem.empty()) {
            note_error("aes67: " + problem);
            return "";
        }
        return aes67_action_word(action);
    }

    case Aes67Action::RepointCard: {
        // The one repair that changes this player rather than the daemon, and
        // the one the old one-shot switch could not do at all: a box that
        // booted before its kernel module registered the card had no card to
        // point at, and went on sending the sound to HDMI for ever.
        const std::string card = aes67_card_device(kAes67CardName);
        if (card.empty()) return "";

        Config updated = cfg;
        if (updated.alsa_device != card) {
            // A card comparison, not a substring search: "RAVENNA2" is a
            // different card, and treating it as this one would throw away the
            // device somebody deliberately chose.
            if (!aes67_device_is_card(updated.alsa_device, kAes67CardName))
                updated.aes67_previous_device = updated.alsa_device;
            updated.alsa_device = card;
            if (!m_config_path.empty()) {
                std::string err;
                if (!updated.save(m_config_path, err))
                    plog_warn("aes67: could not save the settings: %s",
                              err.c_str());
            }
        }
        // 8 channels at 48 kHz, the rate every feed this project produces runs
        // at. A feed that turns out to run at another rate is caught by the
        // delivery loop's own comparison and reopened, so nothing here has to
        // guess twice.
        m_audio_rate.store(48000);
        reconfigure(updated);
        return aes67_action_word(action);
    }
    }
    return "";
}

// ── Delivery ─────────────────────────────────────────────────────────────────

void Player::deliver_loop() {
    plog_info("delivery started");

    while (m_running.load()) {
        // Stopped. This is not the same state as held, and the difference is the
        // queue: Hold keeps it, so Continue resumes exactly where the operator
        // left off, and Stop discards it, because those frames belong to where
        // playback used to be. Delivering them anyway is the fault this branch
        // exists for — a stopped box that carried on playing out everything the
        // decoder had already produced, which is a stop that stops nothing.
        //
        // Checked here, before the queue is looked at, because the frames are
        // already in the queue by the time Stop is pressed: flushing them once is
        // not enough while dispatch does not care whether the box is playing.
        if (!m_playing.load()) {
            {
                std::lock_guard<std::mutex> lk(m_dq_mtx);
                m_dq.clear();
            }
            m_dq_cv.notify_all();
            // The card stays open and fed. Stopping the picture is not switching
            // the sound off, and on a box whose sound is on the network a stream
            // that stops when somebody presses Stop is a stream that receivers
            // drop — the same reasoning as the held case below.
            ensure_audio_open(config(), 0, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        // While held, deliver nothing: the picture stays on the last frame the
        // output received and the queue stays put, so Continue resumes exactly
        // where the operator stopped.
        if (m_paused.load()) {
            // Held: deliver nothing, but the card still has to be open and fed.
            // Holding the picture is not switching the sound off, and on a box
            // whose sound is on the network a stream that stops while somebody
            // holds a frame is a stream that receivers drop.
            ensure_audio_open(config(), 0, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        PendingFrame item;
        bool idle = false;
        {
            std::unique_lock<std::mutex> lk(m_dq_mtx);
            m_dq_cv.wait_for(lk, std::chrono::milliseconds(50), [this] {
                return !m_dq.empty() || !m_running.load();
            });
            if (!m_running.load()) break;
            if (m_dq.empty()) {
                idle = true;
            } else {
                // Release the earliest-timestamped frame in the window, not
                // simply the first enqueued: video and audio arrive in track
                // order.
                auto it = std::min_element(m_dq.begin(), m_dq.end(),
                    [](const PendingFrame& a, const PendingFrame& b) {
                        return a.due_ns < b.due_ns;
                    });
                item = std::move(*it);
                m_dq.erase(it);
            }
        }
        m_dq_cv.notify_all();

        // Nothing to deliver — but the card may still need opening or
        // reopening, and that is asked here rather than only when a frame
        // arrives. It is what lets a box that is idle put its sound on the
        // network at all, and what makes a device chosen in the interface take
        // effect straight away instead of at the next service.
        //
        // Done OUTSIDE the queue lock, deliberately. A card that will not open
        // is now retried on a timer, and an open attempt is milliseconds of
        // driver work; holding the lock a decoder writes through for that long,
        // every couple of seconds, is the sort of stall that shows up as
        // dropped video rather than as anything to do with sound.
        if (idle) {
            ensure_audio_open(config(), 0, 0);
            continue;
        }

        // Far past due means the clock drifted behind wall time, normally
        // because playback stalled waiting for a segment. Re-anchor by
        // ASSIGNING, never accumulating: a second resync for the same stall
        // must be a no-op, or the clock jumps twice and playback stalls for
        // good.
        {
            const uint64_t now = now_ns();
            if (item.due_ns + kClockResyncThresholdNs < now) {
                const int64_t first = m_first_pts_ns.load();
                const int64_t pts = item.is_video ? item.video.pts_ns
                                                  : item.audio.pts_ns;
                const uint64_t behind = now - item.due_ns;
                const uint64_t base = now - (uint64_t)(pts - first)
                                    + kMaxDeliveryLeadNs;
                m_playout_base_ns = base;
                item.due_ns = base + (uint64_t)(pts - first);
                {
                    std::lock_guard<std::mutex> lk(m_dq_mtx);
                    for (auto& q : m_dq) {
                        const int64_t qp = q.is_video ? q.video.pts_ns
                                                      : q.audio.pts_ns;
                        q.due_ns = base + (uint64_t)(qp - first);
                    }
                }
                const uint64_t last = m_last_resync_log_ns.load();
                if (now - last > 5000000000ULL) {
                    m_last_resync_log_ns = now;
                    plog_warn("playout fell %.1fs behind (waiting for the "
                              "feed?) — re-anchored", (double)behind / 1e9);
                }
            }
        }

        // Hold until due, in slices so shutdown stays responsive.
        while (m_running.load()) {
            const uint64_t now = now_ns();
            if (item.due_ns <= now + kMaxDeliveryLeadNs) break;
            uint64_t wait = item.due_ns - now - kMaxDeliveryLeadNs;
            if (wait > 20000000ULL) wait = 20000000ULL;
            std::this_thread::sleep_for(std::chrono::nanoseconds(wait));
        }
        if (!m_running.load()) break;

        // Sub-segment seek: drop frames before the requested moment. Segments
        // are the unit of transfer; they need not be the unit of seeking.
        {
            const int64_t skip = m_skip_until_pts_ns.load();
            if (skip >= 0) {
                int64_t base = m_seg_first_pts_ns.load();
                const int64_t pts = item.is_video ? item.video.pts_ns
                                                  : item.audio.pts_ns;
                if (base < 0) { m_seg_first_pts_ns = pts; base = pts; }
                if (pts - base < skip) continue;
                m_skip_until_pts_ns = -1;
            }
        }

        // Keep the reported time in step with the frame going to air, so it
        // advances continuously rather than once per segment.
        {
            int64_t base = m_seg_first_pts_ns.load();
            const int64_t pts = item.is_video ? item.video.pts_ns
                                              : item.audio.pts_ns;
            if (base < 0) { m_seg_first_pts_ns = pts; base = pts; }
            // The position IS the frame's pts: how far into the programme it
            // sits. This used to be the segment's recorded wall time plus the
            // offset within it, which needed the segment's at_ms to be right —
            // and for anything outside the manifest's rolling window that was
            // an estimate that drifted (BUGS #2b). A frame knows where it is.
            m_playing_at_ms = pts / 1000000LL;
            // The segment of the frame going to air, from the frame itself. A
            // cue is placed with this rather than a clock reading, because the
            // media clock is re-pinned on every seek.
            if (item.is_video) m_on_screen_seq = item.video.seq;
        }

        // Boot splash: while it is showing, drop video and audio alike so the
        // picture stays on the identity screen and the playout clock keeps
        // running — the box simply comes in a few seconds into the event.
        if (now_ns() < m_boot_splash_until_ns.load()) continue;

        // Something has reached the output, so whatever was asked for has
        // landed. Keyed on delivery rather than on the seek returning: the
        // seek is instant, arriving there is not.
        if (m_awaiting_frames.exchange(false)) {
            m_seek_target_ms = 0;
            m_loading_event  = false;
        }

        if (item.is_video) {
            const uint64_t t0 = now_ns();
            // One tile of a composited feed, if the operator asked for one.
            // The selection and the layout are read from atomics so no lock is
            // taken on a path that runs thirty times a second, and tile_view()
            // is a non-owning view, so nothing is copied or allocated.
            //
            // A 1x1 feed, or a layout with fewer tiles than the selection,
            // falls through to the whole picture — recoverable by hand, where
            // a wrongly cropped one is not obviously wrong at all.
            const int sel = m_tile_sel.load();
            if (sel >= 0) {
                TileLayout lay;
                lay.cols = m_tile_cols.load();
                lay.rows = m_tile_rows.load();
                if (lay.is_split() && sel < lay.count())
                    m_video.present(tile_view(item.video, lay, sel));
                else
                    m_video.present(item.video);
            } else {
                m_video.present(item.video);
            }
            const uint64_t took = now_ns() - t0;
            m_present_ns += took;
            m_presents++;
            { // plain compare-exchange loop: a running maximum, no lock
              uint64_t prev = m_present_max_ns.load();
              while (took > prev &&
                     !m_present_max_ns.compare_exchange_weak(prev, took)) {}
            }
            m_frames_out++;
            m_last_frame_ns = now_ns();
            m_idle_showing = false;
            // Keep the newest picture for the preview. Copied under its own
            // lock so a browser reading it can never stall the output.
            //
            // Deliberately the WHOLE frame, not the tile that went to the
            // screen: fix_planes() below re-derives the plane pointers from
            // this frame's own buffer, and tile_view() is a non-owning view
            // with no buffer of its own. The preview therefore shows what the
            // box received, which is also what an operator wants to see when
            // they are checking that a split feed is arriving at all.
            {
                std::lock_guard<std::mutex> lk(m_frame_mtx);
                m_last_frame = item.video;
                fix_planes(m_last_frame);
            }
            m_frame_version++;
        } else {
            const Config cfg = config();
            ensure_audio_open(cfg, item.audio.channels, item.audio.sample_rate);
            // Muted means silence is written to a card that stays open, rather
            // than the card being closed. On a box whose sound leaves over the
            // network, closing the card takes the stream off air and receivers
            // drop it; writing silence leaves the stream up and silent, which is
            // what a mute is supposed to sound like.
            if (cfg.audio_enabled) {
                m_audio.write(item.audio);
                m_meter.observe(item.audio.interleaved.data(), item.audio.frames,
                                item.audio.channels, m_audio_opened_channels.load(),
                                now_ns());
            } else {
                const multisite::DecodedAudioFrame quiet = silence_like(item.audio);
                m_audio.write(quiet);
                m_meter.observe(quiet.interleaved.data(), quiet.frames,
                                quiet.channels, m_audio_opened_channels.load(),
                                now_ns());
            }
        }
    }
    plog_info("delivery stopped");
}

// ── The idle screen ──────────────────────────────────────────────────────────

SplashInfo Player::splash_info() const {
    const Config cfg = config();
    SplashInfo info;
    info.hostname   = hostname();
    info.room       = cfg.room_id;
    info.version    = player_version();
    info.configured = cfg.configured();
    for (const auto& n : network_interfaces()) {
        if (n.ipv4.empty()) continue;
        info.addresses.push_back("http://" + n.ipv4 + ":" +
                                 std::to_string(cfg.web_port));
    }

    // The address a remote operator reaches this box on. It is a different
    // kind of thing from the addresses above — those only work from inside
    // the building — so the splash labels it separately, and says nothing at
    // all when the box has not been put on a remote network. The lookup is the
    // cheap one on purpose: this runs on every idle redraw.
    info.remote_ip = zerotier_ip();

    // The state line, in the words an operator would use standing in front of
    // the screen.
    if (!cfg.configured()) {
        // The splash already says the box has no storage details; repeating
        // it as a state line would just be the same sentence twice.
        info.state.clear();
    } else if (auto sess = session_ref()) {
        const double ahead = sess->buffered_ahead_s();
        // A dead link while content is still buffered is the one moment the
        // screen must say something specific: the venue's internet is gone but
        // the event can keep playing for a while. Saying "waiting for the
        // main site" here would read as a fault at the main site, which it is
        // not.
        if (sess->link_health() == LinkHealth::Offline && ahead > 1.0) {
            info.state = "NO CONNECTION - PLAYING BUFFER";
            info.detail = std::to_string((int)(ahead / 60)) +
                          " MINUTES LEFT";
        } else {
            switch (sess->room_state()) {
            case RoomState::Live:
                info.state = m_playing.load() ? "STARTING" : "READY - NOT ON AIR";
                break;
            case RoomState::Ended:
                info.state = "RECORDING READY";
                break;
            case RoomState::Interrupted:
                info.state = "LAST EVENT WAS CUT SHORT";
                break;
            case RoomState::Offline:
                info.state = "WAITING FOR THE MAIN SITE";
                break;
            default:
                info.state = "LOOKING FOR THE MAIN SITE";
                break;
            }
            if (ahead > 1) {
                info.detail = std::to_string((int)(ahead / 60)) +
                              " MINUTES READY TO PLAY";
            }
        }
    } else {
        info.state = "WAITING FOR THE MAIN SITE";
    }
    return info;
}

void Player::update_screen() {
    // Boot splash: the identity screen owns the display for the first few
    // seconds after power-on, whatever idle mode is set and whether an event
    // is already arriving (delivery holds its frames back meanwhile).
    if (now_ns() < m_boot_splash_until_ns.load()) {
        if (!m_boot_splash_drawn.exchange(true)) {
            int width = 1920, height = 1080;
            m_video.size(width, height);
            Canvas canvas(width, height);
            render_splash(canvas, splash_info());
            m_video.present_bgrx(canvas.width(), canvas.height(),
                                 canvas.stride(), canvas.pixels());
            m_idle_showing = true;
        }
        return;
    }
    if (m_boot_splash_drawn.exchange(false)) {
        // Hand the screen over to the ordinary idle behaviour.
        m_idle_showing = false;
    }

    // What the screen should be doing is decided in screen_action(), where the
    // rule lives and is tested without a display, a decoder or a network — see
    // screen.h. The one thing worth knowing here is that a picture being held
    // outranks the idle screen: the operator asked for it to stay.
    const uint64_t last = m_last_frame_ns.load();
    const bool frames_arriving = last != 0 && now_ns() - last < 2000000000ULL;

    const Config cfg = config();
    const ScreenAction wanted = screen_action(cfg.idle_mode, frames_arriving,
                                              m_playing.load() && m_paused.load(),
                                              m_frames_out.load() > 0);

    if (wanted == ScreenAction::Leave) {
        // Either a picture is arriving, in which case any idle screen it
        // interrupted is over and done with, or one is being held — in which
        // case the screen is deliberately left exactly as it is, because that
        // is what holding means.
        if (frames_arriving) m_idle_showing = false;
        return;
    }

    if (wanted == ScreenAction::Blank) {
        if (!m_idle_showing) { m_video.blank(); m_idle_showing = true; }
        return;
    }

    int width = 1920, height = 1080;
    m_video.size(width, height);

    if (wanted == ScreenAction::Still) {
        if (m_idle_showing) return;      // a still does not change
        Canvas canvas(width, height);
        std::string err;
        if (load_still(cfg.idle_image_path, width, height, canvas, err)) {
            m_video.present_bgrx(canvas.width(), canvas.height(),
                                 canvas.stride(), canvas.pixels());
            m_idle_showing = true;
            return;
        }
        // A holding slide that cannot be read must not leave a black screen
        // with no explanation — fall through to the splash, which at least
        // says where the box is.
        plog_warn("holding slide: %s", err.c_str());
    }

    SplashInfo info = splash_info();

    // Nothing has changed, so nothing needs redrawing.
    std::string signature = info.state + "|" + info.detail + "|" + info.room;
    for (const auto& a : info.addresses) signature += "|" + a;
    signature += "|" + info.remote_ip;
    if (m_idle_showing && signature == m_idle_signature) return;
    m_idle_signature = signature;

    Canvas canvas(width, height);
    render_splash(canvas, info);
    m_video.present_bgrx(canvas.width(), canvas.height(), canvas.stride(),
                         canvas.pixels());
    m_idle_showing = true;
}

// ── Poll loop ────────────────────────────────────────────────────────────────

void Player::poll_loop() {
    plog_info("receive loop started");
    // Put the identity screen up straight away, before the first network poll
    // can take seconds.
    update_screen();
    long long next_poll = 0;
    long long last_status_log = now_ms();

    while (m_running.load()) {
        const long long now = now_ms();
        const int interval = config().poll_interval_ms;

        if (now >= next_poll || m_poll_now.exchange(false)) {
            next_poll = now + interval;

            // A new credential set arrived (the reporter fetched it): rebuild
            // so the session reads through it. This is the paired box's first
            // transport, or a swap from stale keys to fresh ones. Done here
            // rather than on the reporter's thread because the poll loop owns
            // session lifetime.
            if (m_transport_wanted.exchange(false)) {
                plog_info("cloud credentials available — rebuilding the session");
                teardown_decoder();
                flush_delivery();
                {
                    std::lock_guard<std::mutex> lk(m_obj_mtx);
                    rebuild_session();
                }
                m_events_wanted = true;
            }

            auto sess = session_ref();
            if (sess) {
                // poll() does network I/O and can take seconds; never under a
                // lock.
                const RoomState st = sess->poll();

                // Which event this box is in, for its cue credential: fetched
                // on joining, so a cue dropped later needs nothing from the
                // collector. Only a box whose storage is Multisite Cloud holds
                // one; every other box wants none, and so fetches none.
                m_cue_creds->want_event(paired_for_storage(config())
                                            ? sess->current_event_id()
                                            : std::string());

                // The declared layout, for the crop the delivery thread
                // applies. It cannot change mid-event (event.json is written
                // once at Go Live), but a different event can declare a
                // different one, so it is re-read every poll rather than once
                // at startup.
                {
                    const TileLayout lay = sess->video_layout();
                    m_tile_cols = lay.cols;
                    m_tile_rows = lay.rows;
                }

                // Load does not go to air, so no frame will arrive to clear
                // this — the poll that performed the switch has to.
                if (m_loading_event.load() && st != RoomState::Unknown)
                    m_loading_event = false;

                if ((int)st != m_last_room) {
                    m_last_room = (int)st;
                    const char* name =
                        room_state_words(st, sess->was_live_this_session());
                    plog_info("room is %s", name);
                    if (!sess->last_error().empty())
                        note_error(sess->last_error());
                }
            }
        }

        // Download-ahead runs continuously — this is what keeps filling the
        // cache while the picture is held or sitting behind live.
        int fetched = 0;
        if (auto sess = session_ref()) fetched = sess->pump_downloads(8);

        // The event list, on this thread rather than a browser's. A listing
        // plus one manifest per event is seconds of network work.
        if (m_events_wanted.exchange(false)) {
            std::shared_ptr<EventCatalog> cat;
            { std::lock_guard<std::mutex> lk(m_obj_mtx); cat = m_catalog; }
            if (cat) {
                m_events_refreshing = true;
                cat->refresh();
                EventListing listing;
                listing.listed_once   = true;
                listing.fallback_scan = cat->used_fallback_scan();
                listing.skipped       = cat->skipped();
                listing.error         = cat->last_error();
                for (const auto& e : cat->events()) {
                    EventEntry row;
                    row.event_id   = e.event_id;
                    row.name       = e.name;
                    row.started_ms = (long long)e.started_at_ms;
                    row.duration_s = e.duration_s;
                    row.state      = (int)e.state;
                    listing.events.push_back(std::move(row));
                }
                const size_t count = listing.events.size();
                {
                    std::lock_guard<std::mutex> lk(m_events_mtx);
                    m_events = std::move(listing);
                }
                m_events_refreshing = false;
                plog_info("event list refreshed — %zu event(s)%s", count,
                          cat->used_fallback_scan()
                              ? " (scanned events/: these predate the room index)"
                              : "");
            }
        }

        update_screen();

        if (now - last_status_log > 60000) {
            const long long span_ms = now - last_status_log;
            last_status_log = now;
            if (auto sess = session_ref()) {
                const auto& s = sess->stats();
                // Frames per second over the interval rather than a running
                // total: a total that keeps climbing tells you playback is
                // alive, not whether it is keeping up.
                const uint64_t out = m_frames_out.load();
                const double fps = span_ms > 0
                    ? (double)(out - m_last_frames_out) * 1000.0 / (double)span_ms
                    : 0.0;
                m_last_frames_out = out;
                // What the player is actually doing. Without this the line
                // read as perfect health while nothing was happening: two
                // hours of "behind=0s buffered=0s fps=0.0" turned out to be
                // playback having reached the end of a recording and stopped,
                // which in that line looked exactly like keeping up. The
                // head sitting one past the live edge was the only evidence,
                // and it takes someone who knows the code to read it.
                //
                // The operator's own state is asked FIRST, and that is the
                // correction here. This line used to read from the session
                // alone, and the session has no idea the operator pressed Stop:
                // stopping holds the head where it is (the feed loop stops
                // pulling) without moving the session out of Playing, so the
                // line went on saying "playing" for as long as the box was
                // stopped. A box that reports the wrong state is worse than one
                // that reports nothing, because the wrong state is believed —
                // it is what sent somebody looking at the decoder for a fault
                // that was in this function.
                const char* state =
                      !m_playing.load()                      ? "stopped"
                    : m_paused.load()                        ? "held"
                    : sess->at_end()                         ? "at-end"
                    : sess->play_state() == PlayState::Stopped ? "starting"
                                                               : "playing";
                // PTP lock accuracy, when this box manages the network sound.
                // Appended to the one line somebody already reads rather than
                // logged separately: the accuracy question (BUGS.md entry 1) is
                // only answerable over a long run, so it has to be in the
                // periodic record and not only when the state changes.
                char ptp[64] = "";
                if (m_aes67_ptp_known.load())
                    std::snprintf(ptp, sizeof(ptp), " ptp=%s%.1fns",
                                  m_aes67_ptp_locked.load() ? "locked "
                                                            : "UNLOCKED ",
                                  m_aes67_ptp_jitter.load());
                plog_info("%s head=%llu live=%llu behind=%.0fs buffered=%.0fs "
                          "cached=%zu downloaded=%llu frames_out=%llu "
                          "fps=%.1f dropped=%llu (%llu v / %llu a)%s",
                          state,
                          (unsigned long long)sess->playback_head(),
                          (unsigned long long)sess->live_edge(),
                          sess->behind_live_s(), sess->buffered_ahead_s(),
                          sess->cache().count(),
                          (unsigned long long)s.downloaded,
                          (unsigned long long)out, fps,
                          (unsigned long long)m_frames_dropped.load(),
                          (unsigned long long)m_dropped_video.load(),
                          (unsigned long long)m_dropped_audio.load(),
                          ptp);

                // A stall that looks nothing like a download problem: the
                // exact shape BUGS.md entry 0 describes — head and frames_out
                // frozen while downloaded keeps climbing, for several minutes,
                // with playback nominally still "playing". If feed_loop is
                // parked inside push_fragment() (the leading hypothesis; not
                // yet confirmed), nothing else here would ever say so — this
                // makes it a single unmistakable log line instead of a
                // pattern someone has to notice across several status lines.
                const bool downloading_ok = s.downloaded > m_last_downloaded_stat;
                m_last_downloaded_stat = s.downloaded;
                if (std::strcmp(state, "playing") == 0 && fps < 0.05 && downloading_ok) {
                    if (++m_stall_intervals >= 2) {
                        plog_warn("STALL SUSPECTED: playing but frames_out has not "
                                  "advanced for %d update(s) while downloads keep "
                                  "succeeding (head=%llu unchanged, downloaded still "
                                  "climbing). If this is still true, capture a thread "
                                  "dump now: gdb -p %d -batch -ex \"thread apply all bt\" "
                                  "— see BUGS.md entry 0.",
                                  m_stall_intervals,
                                  (unsigned long long)sess->playback_head(),
                                  (int)getpid());
                    }
                } else {
                    m_stall_intervals = 0;
                }

                // Where the delivery thread's time actually goes. A mean that
                // approaches the frame interval means the display path, not
                // the network, is setting the pace.
                const uint64_t n = m_presents.exchange(0);
                const uint64_t total = m_present_ns.exchange(0);
                const uint64_t worst = m_present_max_ns.exchange(0);
                if (n > 0)
                    plog_debug("present: %.1f ms mean, %.1f ms worst, over %llu "
                               "frame(s)", (double)total / (double)n / 1e6,
                               (double)worst / 1e6, (unsigned long long)n);
            }
        }

        // Always yield, even when there is more to fetch: a tight download
        // loop starves everything else, and a few milliseconds costs nothing
        // against a segment download.
        std::this_thread::sleep_for(
            std::chrono::milliseconds(fetched == 0 ? 100 : 5));
    }
    plog_info("receive loop stopped");
}

// ── Feed loop ────────────────────────────────────────────────────────────────

void Player::feed_loop() {
    plog_info("feed loop started");
    while (m_running.load()) {
        // While held, stop pulling and feeding entirely: otherwise the queues
        // fill, push_fragment blocks, and Continue cannot get in.
        if (m_paused.load() || !m_playing.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        auto sess = session_ref();
        if (!sess) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        if (sess->play_state() == PlayState::Stopped) sess->start();
        std::optional<PlayableSegment> seg = sess->next_segment();
        if (!seg) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        // A jump means the next fragment belongs to a different timeline.
        // Feeding across one produces out-of-order timestamps and a glitched
        // picture, so the decoder is torn down and restarts from the re-sent
        // init segment.
        {
            const uint64_t d = sess->discontinuity_id();
            if (d != m_seen_discontinuity) {
                m_seen_discontinuity = d;
                if (m_decoder_started.load()) {
                    plog_info("playback jumped — restarting the decoder");
                    teardown_decoder();
                    {
                        std::lock_guard<std::mutex> lk(m_dq_mtx);
                        m_dq.clear();
                    }
                    m_dq_cv.notify_all();
                }
            }
        }

        if (!m_decoder_started.load()) {
            if (seg->init.empty()) {
                plog_error("first segment arrived with no init segment — "
                           "cannot start decoding");
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                continue;
            }
            auto dec = std::make_shared<CmafDecoder>();
            // Pi 4: prefer the hardware H.264 decoder, falling back to
            // software. The name is ignored on any box that has no such
            // decoder — every non-Pi, and the Pi 5 — so asking for it
            // unconditionally is safe; where it does open, it is the
            // difference between a picture that keeps up and three cores
            // spent keeping up. See CmafDecoder::set_preferred_video_decoders.
            if (config().hardware_decode)
                dec->set_preferred_video_decoders({ "h264_v4l2m2m" });
            dec->on_video([this](const DecodedVideoFrame& f) { on_video(f); });
            dec->on_audio([this](const DecodedAudioFrame& f) { on_audio(f); });
            if (!dec->start(seg->init)) {
                plog_error("decoder failed to start: %s", dec->error().c_str());
                note_error("decoder: " + dec->error());
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            { std::lock_guard<std::mutex> lk(m_obj_mtx); m_decoder = dec; }
            m_decoder_started = true;
            m_feed_start_ns = now_ns();
            m_pushed_media_ns = 0;
            m_logged_stream = false;    // report it once it is actually known
            plog_info("decoder started");
        }

        // Feed at playout rate with a small lead, so the decoder always has
        // work but never runs seconds ahead of the clock.
        //
        // A hold KEEPS the fragment in hand (#10, the same rule as the OBS
        // source). This loop used to exit on `m_paused` and fall straight
        // through to the push, so every hold began by sending the decoder one
        // more fragment than the picture was showing. A jump still drops it:
        // next_segment() has already moved past it, and the teardown above
        // runs on the next pass.
        const uint64_t feeding_disc = sess->discontinuity_id();
        bool jumped = false;
        while (m_running.load()) {
            const uint64_t elapsed = now_ns() - m_feed_start_ns.load();
            const multisite::FeedWait step = multisite::feed_wait_step(
                /*jumped=*/!m_decoder_started.load() ||
                    sess->discontinuity_id() != feeding_disc,
                /*paused=*/m_paused.load(),
                /*lead_allows=*/m_pushed_media_ns <= elapsed + kFeedLeadNs);
            if (step == multisite::FeedWait::Drop) { jumped = true; break; }
            if (step == multisite::FeedWait::Push) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!m_running.load()) break;
        if (jumped) continue;

        m_seg_starts_at_ms = (long long)seg->starts_at_ms;
        m_seg_first_pts_ns = -1;                 // set by the first frame
        if (seg->skip_to_ms > 0)
            m_skip_until_pts_ns = seg->skip_to_ms * 1000000LL;

        // push_fragment blocks when the decoder is full — never under a lock.
        // It returns false only when the decoder has stopped consuming (BUGS.md
        // entry 0): rebuilding it beats freezing for ever behind this call, and
        // teardown_decoder() re-requests the init segment so the next segment
        // that arrives is not rejected for want of it.
        if (auto dec = decoder_ref()) {
            if (!dec->push_fragment(seg->media, seg->seq)) {
                plog_warn("decoder stopped consuming fragments — rebuilding it "
                          "(%s)", dec->error().c_str());
                note_error("decoder: " + dec->error());
                teardown_decoder();
                continue;   // the loop rebuilds it on the next pass
            }
        }
        m_pushed_media_ns += (uint64_t)(seg->duration_s * 1e9);
    }
    plog_info("feed loop stopped");
}

// ── Operator controls ────────────────────────────────────────────────────────

void Player::play() {
    auto sess = session_ref();
    if (!sess) { plog_warn("play ignored — no storage configured"); return; }
    m_paused = false;
    m_playing = true;
    sess->resume();
    plog_info("PLAY — %.0fs behind live, %.0fs buffered",
              sess->behind_live_s(), sess->buffered_ahead_s());
}

void Player::stop_playback() {
    m_playing = false;
    m_paused = false;
    flush_delivery();
    // Stopping is a deliberate act, so the screen should reflect it rather
    // than keeping the last frame of an event up. Downloading continues.
    Config cfg = config();
    if (cfg.idle_mode != IdleMode::HoldFrame) m_video.blank();
    plog_info("STOPPED (still downloading, ready to play again)");
}

void Player::pause() {
    auto sess = session_ref();
    if (!sess) return;
    // Order matters: stop delivery first so the picture holds immediately,
    // then stop pulling new segments.
    m_paused = true;
    m_pause_started_ns = now_ns();
    sess->pause();
    plog_info("HOLDING the picture — the cache keeps filling");
}

void Player::resume() {
    auto sess = session_ref();
    if (!sess) { plog_warn("continue ignored — no session"); return; }
    // Re-anchor rather than shift. Advancing the clock by the paused duration
    // has several ways to fail quietly, and every one of them leaves frames
    // that never become due — a picture frozen for good. Treating it as a
    // discontinuity costs the handful of frames still queued and always works.
    flush_delivery();
    // Held time is not time fallen behind; without this the lead gate let the
    // feed run until the decoder's queue refused it (see feed_start_after_hold).
    // Only a hold that is still on counts: a jump or a stop ends one without
    // coming through here, and its start must not be charged to a later decoder.
    const uint64_t held_since = m_pause_started_ns.exchange(0);
    if (m_paused.load())
        m_feed_start_ns = multisite::feed_start_after_hold(
            m_feed_start_ns.load(), held_since, now_ns());
    m_paused = false;
    sess->resume();
    plog_info("CONTINUING at %.0fs behind live", sess->behind_live_s());
}

void Player::toggle_pause() {
    if (m_paused.load()) resume(); else pause();
}

void Player::jump_to_live() {
    auto sess = session_ref();
    if (!sess) return;
    sess->jump_to_live();
    m_paused = false;
    m_delay_from_live_s = 0.0;
    plog_info("CAUGHT UP TO NOW");
}

void Player::seek_to_media(long long media_ms) {
    auto sess = session_ref();
    if (!sess) return;
    const int64_t got = sess->seek_to_media_ms((int64_t)media_ms);
    if (got == 0) {
        // The session knows which bound was hit; repeating a guess here is how
        // "past the end of the recording" came to be reported as storage
        // having lost it.
        const std::string why = sess->last_error();
        const std::string msg = why.empty()
            ? std::string("that moment cannot be played")
            : why;
        plog_warn("%s", msg.c_str());
        note_error(msg);
        return;
    }
    flush_delivery();
    m_paused = false;

    // Report where playback is GOING immediately. Waiting until a frame has
    // been fetched and decoded there made a jog look like a dropped click that
    // then snapped into place; the UI marks this as provisional until frames
    // arrive.
    m_seek_target_ms  = (long long)got;
    m_playing_at_ms   = (long long)got;
    m_awaiting_frames = true;
    m_poll_now        = true;
    plog_info("went to %lld (%.0fs behind live)", (long long)got,
              sess->behind_live_s());
}

void Player::jog(double seconds) {
    long long from = m_playing_at_ms.load();
    if (from <= 0) {
        // Nothing delivered yet — a recording loaded but not played, which is
        // exactly when an operator wants to move to their intended start
        // point. Jog from where the playhead SITS rather than refusing.
        if (auto sess = session_ref()) from = (long long)sess->playhead_media_ms();
    }
    if (from <= 0) {
        plog_warn("cannot jog until the recording has loaded");
        return;
    }
    seek_to_media(from + (long long)(seconds * 1000.0));
}

void Player::set_delay_from_live(double seconds) {
    auto sess = session_ref();
    if (!sess) return;
    const int64_t live = sess->live_media_ms();
    if (live <= 0) {
        plog_warn("the live edge is not known yet");
        return;
    }
    m_delay_from_live_s = seconds;
    seek_to_media((long long)live - (long long)(seconds * 1000.0));
    plog_info("holding %.0f minute(s) behind live", seconds / 60.0);
}

bool Player::add_cue(const std::string& label, std::string& error) {
    if (m_locked.load()) { error = "this box is locked"; return false; }
    auto sess = session_ref();
    if (!sess) { error = "no event is loaded"; return false; }
    // The on-screen SEGMENT goes with it: the media clock is re-pinned on every
    // seek, so a clock reading names a place inconsistently, while the segment
    // number is where the operator is actually looking.
    return sess->add_cue(label, error, m_on_screen_seq.load());
}

void Player::jump_to_marker(const std::string& id) {
    auto sess = session_ref();
    if (!sess) return;
    if (!sess->jump_to_marker(id)) {
        plog_warn("could not jump to that cue — it may no longer be retained");
        note_error("that cue is no longer available");
        return;
    }
    flush_delivery();
    m_paused = false;
    m_awaiting_frames = true;
    m_poll_now = true;
    plog_info("jumped to a cue");
}

void Player::pin_event(const std::string& event_id) {
    auto sess = session_ref();
    if (!sess) return;
    sess->pin_event(event_id);
    {
        std::lock_guard<std::mutex> lk(m_cfg_mtx);
        m_cfg.pinned_event_id = event_id;
    }
    teardown_decoder();
    flush_delivery();
    m_loading_event = true;
    m_poll_now = true;
    plog_info("loading event %s", event_id.c_str());
}

void Player::unpin_event() {
    auto sess = session_ref();
    if (!sess) return;
    sess->unpin();
    {
        std::lock_guard<std::mutex> lk(m_cfg_mtx);
        m_cfg.pinned_event_id.clear();
    }
    teardown_decoder();
    flush_delivery();
    m_loading_event = true;
    m_poll_now = true;
    m_events_wanted = true;
    plog_info("following whatever is live in the room");
}

void Player::set_locked(bool locked) {
    m_locked = locked;
    {
        std::lock_guard<std::mutex> lk(m_cfg_mtx);
        m_cfg.locked = locked;
    }
    plog_info(locked ? "controls locked" : "controls unlocked");
}

void Player::refresh_events() { m_events_wanted = true; }

void Player::event_listing(EventListing& out) const {
    {
        std::lock_guard<std::mutex> lk(m_events_mtx);
        out = m_events;
    }
    out.loading = m_events_refreshing.load();
    // No catalogue means no cloud storage configured, which means this list can
    // never fill: recordings are enumerated out of a bucket, and the LAN side
    // only ever knows the event that is on air. Recording an event with cloud
    // delivery off is a legitimate thing to do — it is how a church keeps an
    // event on its own network — so the page has to say that this is why the
    // list is empty rather than letting it read as "nothing was recorded".
    if (!m_catalog) {
        out.listed_once = true;
        out.no_catalog  = true;
    }
}

// `show_tile` returns the region that is going to the output rather than the
// whole frame that arrived, so the browser can be shown either. The layout and
// the selection are read here, under the same lock as the frame, so the two are
// always the same instant rather than a frame and a tile from different ones.
bool Player::latest_frame(DecodedVideoFrame& out, uint64_t& version,
                          bool show_tile) const {
    std::lock_guard<std::mutex> lk(m_frame_mtx);
    if (m_last_frame.data.empty()) return false;
    const int sel = m_tile_sel.load();
    TileLayout lay;
    lay.cols = m_tile_cols.load();
    lay.rows = m_tile_rows.load();
    if (show_tile && sel >= 0 && lay.is_split() && sel < lay.count())
        // Copied, not viewed: the JPEG encoder walks `data`, and a view has
        // none. Preview requests only, so this cost is an operator's, not the
        // thirty-times-a-second path's.
        out = tile_copy(m_last_frame, lay, sel);
    else
        out = m_last_frame;
    fix_planes(out);
    version = m_frame_version.load();
    return true;
}

// ── Status ───────────────────────────────────────────────────────────────────

void Player::status(Status& out) const {
    out = Status{};
    Config cfg = config();

    out.room_id           = cfg.room_id;
    out.configured        = cfg.configured();
    out.locked            = m_locked.load();
    out.reporter_state    = reporter_state();
    out.playing           = m_playing.load();
    out.paused            = m_paused.load();
    out.loading           = m_loading_event.load();
    out.seek_target_ms    = m_seek_target_ms.load();
    out.delay_from_live_s = m_delay_from_live_s.load();
    out.frames_out        = m_frames_out.load();
    out.frames_dropped    = m_frames_dropped.load();

    out.output_description = m_video.description();
    out.video_output_ok    = m_video.ok();
    // Whether the sound is leaving is no longer the same question as whether
    // the operator left the sound switched on. It is whether the card is open:
    // muted writes silence to a live card, which a receiver hears as silence
    // rather than as a stream that has gone away.
    const AudioState astate = (AudioState)m_audio_open_state.load();
    // The words are the meters' own, from the one helper that decides them: the
    // readout beside the meters and the meters themselves must never say two
    // different things about the same state. `output_reason` rather than
    // `meter_reason` because this readout is about where the sound is going,
    // not about what the event happens to be carrying — a silent event is still
    // playing. The card's own description is used only when there is a card
    // actually open and being fed.
    out.audio_description =
        (astate == AudioState::Open)
            ? m_audio.description()
            : meter_reason_text(output_reason(cfg.audio_enabled,
                                              astate == AudioState::Open));
    out.audio_output_ok = astate == AudioState::Open;
    out.audio_state     = to_string(astate);
    out.audio_error.clear();
    if (astate == AudioState::Failed) {
        std::lock_guard<std::mutex> lk(m_audio_err_mtx);
        out.audio_error = m_audio_last_error;
    }

    {
        std::lock_guard<std::mutex> lk(m_err_mtx);
        out.last_error = m_last_error;
    }

    auto sess = session_ref();
    if (!sess) return;

    out.room_state     = (int)sess->room_state();
    out.event_id       = sess->event_id();
    out.pinned_event_id = sess->pinned_event();
    out.live_elsewhere = sess->live_elsewhere();
    out.live_event_id  = sess->live_event_id();

    out.ended       = sess->event_ended();
    out.at_end      = sess->at_end();
    out.was_live    = sess->was_live_this_session();
    out.interrupted = sess->was_interrupted();
    out.plays_as_recording = sess->plays_as_recording();

    // Positions in MEDIA time — how far into the programme. started_ms stays a
    // time of day: it is when the event was recorded, which is its identity,
    // not a position within it. See BUGS #2b.
    out.live_ms     = sess->live_media_ms();
    out.earliest_ms = sess->earliest_media_ms();
    out.started_ms  = sess->event_started_ms();
    out.end_ms      = sess->end_media_ms();
    // Media time begins at zero, so the end IS the length.
    if (out.ended && out.end_ms > 0) out.total_ms = out.end_ms;

    // The delivered time is the honest one — it is what is actually on the
    // screen. Fall back to the playhead before anything has gone out.
    const long long playing_at = m_playing_at_ms.load();
    out.playhead_ms = playing_at > 0 ? playing_at
                                     : (long long)sess->playhead_media_ms();
    // Never report past the end of a recording: once playback runs past the
    // last segment the playhead points at a position that does not exist.
    if (out.ended && out.end_ms > 0 && out.playhead_ms > out.end_ms)
        out.playhead_ms = out.end_ms;

    out.behind_live_s    = sess->behind_live_s();
    out.buffered_ahead_s = sess->buffered_ahead_s();
    out.cached_segments  = sess->cache().count();
    out.buffering        = m_playing.load() && !m_paused.load() &&
                           m_frames_out.load() == 0;

    out.link_health      = (int)sess->link_health();
    out.link_known       = sess->link_known();

    const auto& st = sess->stats();
    out.downloaded        = st.downloaded;
    out.download_failures = st.download_failures;
    out.checksum_failures = st.checksum_failures;
    out.gaps_waited       = st.gaps_waited;

    if (out.last_error.empty()) out.last_error = sess->last_error();

    for (const auto& span : sess->cached_ranges()) {
        // [start, END boundary] and >= 0, for the reasons given in the OBS
        // source: the far edge is the boundary AFTER the last segment, and
        // segment 0 sits at media time 0 so a `> 0` guard would drop it.
        const int64_t a = sess->media_ms_for_seq(span.first);
        const int64_t b = sess->media_ms_for_seq(span.second + 1);
        if (a >= 0 && b >= a) out.cached_spans.emplace_back((long long)a,
                                                           (long long)b);
    }

    for (const auto& m : sess->markers()) {
        Status::MarkerEntry e;
        e.label = m.label;
        e.id    = m.id;
        e.author = m.author;
        // The cue's OWN anchor. This used to throw it away and recompute a
        // time from the segment NUMBER, which placed every cue on a segment
        // boundary — up to six seconds out, the exact coarseness BUGS #3 had
        // just removed from timeline clicks — and then drifted 1.11% on top.
        e.at_media_ms = (long long)multisite::marker_media_ms(
            m, sess->event_started_ms());
        e.at_ms = (long long)sess->wall_clock_ms(m.seq);
        out.markers.push_back(std::move(e));
    }
    if (auto cur = sess->current_marker()) out.current_marker = cur->label;

    for (const auto& t : sess->audio_layout()) {
        out.audio_channels = std::max(out.audio_channels, t.channels);
        for (const auto& lbl : t.channel_labels)
            out.channel_labels.push_back(lbl);
        if (t.channel_labels.empty() && !t.label.empty())
            out.channel_labels.push_back(t.label);
    }

    if (auto dec = decoder_ref()) {
        out.video_width  = dec->video_width();
        out.video_height = dec->video_height();
    }

    out.video_layout = sess->video_layout().to_string();
}

} // namespace multisite_player
