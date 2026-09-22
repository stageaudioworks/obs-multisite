// SPDX-License-Identifier: GPL-3.0-or-later
#include "commands.h"
#include "api_common.h"
#include "web_ui.h"

#include "../broadcast_controller.h"
#include "../decoder_settings.h"
#include "../multisite_ui.h"
#include "../plugin_log.h"
#include "../plugin_role.h"
#include "../reporter.h"   // the plugin's shared CloudIdentity (Phase 12)
#include "core/storage_providers.h"

#include <obs-module.h>

#include <ctime>
#include <string>

namespace multisite_obs {

using multisite::StorageProvider;
using multisite::provider_key;
using multisite::provider_from_key;
using multisite::detect_provider;
using multisite::derive;

namespace {

// The body of a request, as an object. Anything else — including an empty body,
// which is what a button with nothing to say sends — is an empty object rather
// than an error, so a command that needs no JSON needs none.
json parse_object(const std::string& body) {
    if (body.empty()) return json::object();
    try {
        auto j = json::parse(body);
        return j.is_object() ? j : json::object();
    } catch (...) {
        return json::object();
    }
}

// The name an event-name field starts with, so a field nobody touched still
// names "now". The same rule the dock follows, expressed without Qt: this
// reaches a network thread and has no business holding a widget toolkit.
std::string default_event_name() {
    const std::time_t t = std::time(nullptr);
    char buf[64];
    if (!std::strftime(buf, sizeof(buf), "%a %d %b %Y, %H:%M",
                       std::localtime(&t)))
        return "";
    return buf;
}

// Applies a posted document to a copy of the stored settings. The guard rails
// live here rather than in the caller: a mistyped figure must not be able to
// make broadcasting impossible from the very interface being used to fix it.
void apply_encoder_settings(const json& j, BroadcastSettings& s) {
    // The provider decides how the one field the operator actually typed
    // becomes endpoint_host/r2_account_id/region — see storage_providers.h.
    // Custom's fields already ARE that shape, unchanged. Applied only when
    // the page sent a provider; an older or hand-built client that only
    // sends the raw fields leaves the stored provider untouched and edits
    // them directly, as before this dropdown existed.
    std::string provider_str;
    if (json_str(j, "storage_provider", provider_str)) {
        const StorageProvider provider = provider_from_key(provider_str);
        s.storage_provider = provider_key(provider);
        if (provider == StorageProvider::Custom) {
            json_str(j, "endpoint_host", s.endpoint_host);
            json_str(j, "region",        s.region);
            s.r2_account_id.clear();
        } else {
            std::string input;
            if (provider == StorageProvider::CloudflareR2)
                json_str(j, "r2_account_id", input);
            else
                json_str(j, "region", input);
            const auto derived = derive(provider, input);
            s.endpoint_host = derived.endpoint_host;
            s.r2_account_id = derived.r2_account_id;
            s.region        = derived.region;
        }
    } else {
        json_str(j, "endpoint_host", s.endpoint_host);
        json_str(j, "r2_account_id", s.r2_account_id);
        json_str(j, "region",        s.region);
    }
    json_str(j, "bucket",           s.bucket);
    json_str(j, "access_key_id",    s.access_key_id);
    json_str(j, "room_id",          s.room_id);
    json_str(j, "site_name",        s.site_name);
    json_str(j, "cache_dir",        s.cache_dir);
    json_str(j, "video_encoder_id", s.video_encoder_id);
    json_str(j, "track_labels",     s.track_labels);
    json_str(j, "channel_labels",  s.channel_labels);
    json_bool(j, "send_expiry_tag", s.send_expiry_tag);

    std::string secret;
    if (json_str(j, "secret_access_key", secret) && secret != kSecretPlaceholder)
        s.secret_access_key = secret;

    // LAN / direct delivery (PROJECT-SCOPE.md §8.7) — the same fields the
    // dock's LAN group exposes.
    json_bool(j, "lan_enabled", s.lan_enabled);
    int lan_port;
    if (json_int(j, "lan_port", lan_port)) s.lan_port = lan_port;
    if (s.lan_port < 1 || s.lan_port > 65535) s.lan_port = 9080;
    std::string lan_token;
    if (json_str(j, "lan_auth_token", lan_token) && lan_token != kSecretPlaceholder)
        s.lan_auth_token = lan_token;
    json_bool(j, "cloud_enabled", s.cloud_enabled);
    // Cloud off with LAN also off would deliver this event nowhere at all —
    // the dock's own safety net (encoder_dock.cpp's updateLanFields())
    // applies the identical rule when LAN is switched off with cloud
    // already disabled.
    if (!s.lan_enabled && !s.cloud_enabled) s.cloud_enabled = true;

    double d;
    if (json_num(j, "segment_duration_s", d)) s.segment_duration_s = d;

    int n;
    if (json_int(j, "video_bitrate_kbps", n)) s.video_bitrate_kbps = n;
    if (json_int(j, "audio_bitrate_kbps", n)) s.audio_bitrate_kbps = n;
    if (json_int(j, "audio_tracks", n))       s.audio_tracks = n;

    if (s.segment_duration_s < 1.0)   s.segment_duration_s  = 1.0;
    if (s.segment_duration_s > 30.0)  s.segment_duration_s  = 30.0;
    if (s.video_bitrate_kbps < 200)   s.video_bitrate_kbps  = 200;
    if (s.video_bitrate_kbps > 100000) s.video_bitrate_kbps = 100000;
    if (s.audio_bitrate_kbps < 32)    s.audio_bitrate_kbps  = 32;
    if (s.audio_bitrate_kbps > 320)   s.audio_bitrate_kbps  = 320;
    if (s.audio_tracks < 1)           s.audio_tracks        = 1;
    if (s.audio_tracks > 6)           s.audio_tracks        = 6;
    if (s.room_id.empty())            s.room_id = "main-auditorium";
}

// Guard rails rather than trust: a figure typed on a phone in a dark room must
// not be able to stop this campus playing, from the page being used to fix it.
void apply_decoder_settings(const json& j, DecoderSettings& s) {
    // See apply_encoder_settings() above — identical provider-aware handling
    // against the identical table (storage_providers.h).
    std::string provider_str;
    if (json_str(j, "storage_provider", provider_str)) {
        const StorageProvider provider = provider_from_key(provider_str);
        s.storage_provider = provider_key(provider);
        if (provider == StorageProvider::Custom) {
            json_str(j, "endpoint_host", s.endpoint_host);
            json_str(j, "region",        s.region);
            s.r2_account_id.clear();
        } else {
            std::string input;
            if (provider == StorageProvider::CloudflareR2)
                json_str(j, "r2_account_id", input);
            else
                json_str(j, "region", input);
            const auto derived = derive(provider, input);
            s.endpoint_host = derived.endpoint_host;
            s.r2_account_id = derived.r2_account_id;
            s.region        = derived.region;
        }
    } else {
        json_str(j, "endpoint_host", s.endpoint_host);
        json_str(j, "r2_account_id", s.r2_account_id);
        json_str(j, "region",        s.region);
    }
    json_str(j, "bucket",        s.bucket);
    json_str(j, "access_key_id", s.access_key_id);
    json_str(j, "room_id",       s.room_id);

    std::string secret;
    if (json_str(j, "secret_access_key", secret) && secret != kSecretPlaceholder)
        s.secret_access_key = secret;

    // LAN / direct delivery — read straight from the encoder's LanObjectServer
    // instead of the bucket, LAN-preferred with cloud fallback, or LAN alone.
    json_str(j, "lan_host", s.lan_host);
    int lan_port;
    if (json_int(j, "lan_port", lan_port)) s.lan_port = lan_port;
    if (s.lan_port < 1 || s.lan_port > 65535) s.lan_port = 9080;
    std::string lan_token;
    if (json_str(j, "lan_auth_token", lan_token) && lan_token != kSecretPlaceholder)
        s.lan_auth_token = lan_token;

    int n;
    if (json_int(j, "prebuffer_segments", n))   s.prebuffer_segments = n;
    if (json_int(j, "start_buffer_seconds", n)) s.start_buffer_seconds = n;
    if (json_int(j, "poll_interval_ms", n))     s.poll_interval_ms = n;
    if (json_int(j, "keep_behind_segments", n)) s.keep_behind_segments = n;
    if (json_int(j, "buffer_minutes", n))       s.buffer_minutes = n;
    json_str(j, "cache_dir", s.cache_dir);
    json_str(j, "site_name", s.site_name);

    if (s.prebuffer_segments   < 0)    s.prebuffer_segments = 0;
    if (s.start_buffer_seconds < 0)    s.start_buffer_seconds = 0;
    if (s.poll_interval_ms     < 500)  s.poll_interval_ms = 500;
    if (s.keep_behind_segments < 10)   s.keep_behind_segments = 10;
    if (s.buffer_minutes       < 1)    s.buffer_minutes = 1;
    if (s.buffer_minutes       > 240)  s.buffer_minutes = 240;
    if (s.room_id.empty())             s.room_id = "main-auditorium";
}

} // namespace

// ── Status documents ─────────────────────────────────────────────────────────

// One document, polled twice a second, and the page draws everything from it —
// so no two parts of the interface can disagree about what is happening.
std::string encoder_status_json() {
    BroadcastController& c = BroadcastController::instance();
    const BroadcastSettings cfg = c.settings_copy();
    const BroadcastStatus   st  = c.status();

    json j;
    j["role"]    = "encoder";
    j["now_ms"]  = now_ms();
    j["version"] = PLUGIN_VERSION;
    j["locked"]  = web_ui_locked();

    j["live"]       = st.live;
    j["event_id"]   = st.event_id;
    j["event_name"] = cfg.event_name;
    j["uptime_s"]   = st.uptime_s;
    j["confirmed"]  = (unsigned long long)st.confirmed;
    j["pending"]    = (unsigned long long)st.pending;
    j["retries"]    = (unsigned long long)st.retries;
    j["bytes"]      = (unsigned long long)st.bytes;
    j["last_error"] = st.last_error;

    // The internet reading, and whether it is a measurement or a default. A
    // page that said "healthy" before anything had been tried would be telling
    // an operator the thing they most need to know, wrongly.
    j["link_health"]        = st.link_health;
    j["link_known"]         = st.link_known;
    j["colo"]               = st.colo;
    j["clock_skew_ms"]      = st.clock_skew_ms;
    j["storage_host"]       = st.storage_host;
    j["upload_bytes_per_s"] = st.upload_bytes_per_s;
    j["upload_samples"]     = (unsigned long long)st.upload_samples;

    j["room_id"]    = cfg.room_id;
    j["site_name"]  = cfg.site_name;
    j["cache_dir"]  = cfg.cache_dir;
    j["bucket"]     = cfg.bucket;
    j["configured"] = !cfg.bucket.empty() &&
                      (!cfg.endpoint_host.empty() || !cfg.r2_account_id.empty());

    // Paired storage (Phase 12), so the page can say what this machine is
    // really storing through — and so a Multisite Cloud selection shows the
    // broker's bucket rather than the (absent) typed one. Read from the shared
    // identity, the same seam the transport uses, so the two cannot disagree.
    j["storage_provider"] = cfg.storage_provider;
    {
        auto id = reporter_cloud_identity();
        const bool paired = id && id->paired();
        j["paired"] = paired;
        if (paired && id->credentials().present()) {
            j["paired_bucket"] = id->credentials().bucket;
            j["paired_stale"]  = id->credentials().from_last_good;
        }
    }
    j["video_encoder_id"]   = cfg.video_encoder_id;
    j["default_event_name"] = default_event_name();
    // The other half of the plugin, when this machine has one. A page linking to
    // a route that was never registered is a page that looks broken.
    j["other_page"] = plugin_role() == Role::EncoderOnly ? "" : "/decoder/";

    json markers = json::array();
    // The event's own cue names, one per distinct name, in the order they were
    // set — the same names the Cues dock shows. There is no configured list any
    // more, so the buttons on this page and the dock cannot disagree.
    {
        std::vector<CueEntry> cues;
        std::vector<std::string> names;
        if (encoder_cues(cues))
            for (const auto& c : cues) {
                if (c.label.empty()) continue;
                bool seen = false;
                for (const auto& n : names) if (n == c.label) { seen = true; break; }
                if (!seen) names.push_back(c.label);
            }
        for (const auto& n : names) markers.push_back(n);
    }
    j["marker_labels"] = std::move(markers);
    return j.dump();
}

std::string encoder_settings_json() {
    const BroadcastSettings s =
        BroadcastController::instance().settings_copy();

    json j;
    // Empty means this was saved before the dropdown existed (or hand-edited):
    // fall back to guessing from the raw fields, the same rule the dock's own
    // loadIntoFields() applies, so an upgrade never misrepresents an existing
    // AWS/Backblaze/Wasabi/Custom setup as something it isn't.
    const StorageProvider provider = s.storage_provider.empty()
        ? detect_provider(s.endpoint_host, s.r2_account_id)
        : provider_from_key(s.storage_provider);
    j["storage_provider"] = provider_key(provider);
    j["endpoint_host"] = s.endpoint_host;
    j["r2_account_id"] = s.r2_account_id;
    j["bucket"]        = s.bucket;
    j["access_key_id"] = s.access_key_id;
    // Never the secret itself. Sending the placeholder back means "unchanged",
    // so nobody has to retype a bucket key in order to change a room name — and
    // the key never has to travel to a phone in order to come back again.
    j["secret_access_key"] = s.secret_access_key.empty()
                                 ? std::string()
                                 : std::string(kSecretPlaceholder);
    j["region"]             = s.region;
    j["room_id"]            = s.room_id;
    j["site_name"]          = s.site_name;
    j["send_expiry_tag"]    = s.send_expiry_tag;
    j["video_encoder_id"]   = s.video_encoder_id;
    j["segment_duration_s"] = s.segment_duration_s;
    j["video_bitrate_kbps"] = s.video_bitrate_kbps;
    j["audio_bitrate_kbps"] = s.audio_bitrate_kbps;
    j["audio_tracks"]       = s.audio_tracks;
    j["track_labels"]       = s.track_labels;
    j["channel_labels"]     = s.channel_labels;

    j["lan_enabled"] = s.lan_enabled;
    j["lan_port"]    = s.lan_port;
    // Same placeholder convention as the bucket secret above.
    j["lan_auth_token"] = s.lan_auth_token.empty()
                              ? std::string()
                              : std::string(kSecretPlaceholder);
    j["cloud_enabled"] = s.cloud_enabled;

    // The encoders this machine actually has, so a page offers the same list
    // the dock does rather than a text field to mistype an id into.
    json encoders = json::array();
    for (const auto& e : available_video_encoders())
        encoders.push_back(json{{"id", e.id}, {"name", e.name},
                                {"codec", e.codec}, {"hardware", e.hardware}});
    j["encoders"] = std::move(encoders);
    return j.dump();
}



// The satellite's document. The field names follow the campus player's own
// status document, so an operator who has used that page reads this one without
// learning it again.
std::string decoder_status_json() {
    DecoderSnapshot s;
    const bool have_source = decoder_snapshot(s);

    json j;
    j["role"]    = "decoder";
    j["now_ms"]  = now_ms();
    j["version"] = PLUGIN_VERSION;
    // The remote surface's own lock, and the one the desk set. Both are shown,
    // because "why will this not respond" has two different answers.
    j["locked"]        = web_ui_locked();
    j["source_locked"] = s.locked;
    // The other half of the plugin, when this machine has one.
    j["other_page"] = plugin_role() == Role::DecoderOnly ? "" : "/encoder/";

    // No source means no picture and nothing to control, and it must not look
    // like a player that has merely stopped: the fix is in the scene collection,
    // not on this page.
    j["have_source"] = have_source;
    if (!have_source) return j.dump();

    j["room_id"]   = s.room_id;
    j["room_state"] = s.room_state;
    j["event_id"]  = s.event_id;
    j["pinned_event_id"] = s.pinned_event_id;
    j["live_elsewhere"]  = s.live_elsewhere;
    j["live_event_id"]   = s.live_event_id;

    j["playing"]    = s.playing;
    j["stopped"]    = s.stopped;
    j["paused"]     = s.paused;
    j["buffering"]  = s.buffering;
    j["loading"]    = s.loading;
    j["ended"]      = s.ended;
    j["at_end"]     = s.at_end;
    j["was_live"]   = s.was_live;
    j["interrupted"] = s.interrupted;

    j["playhead_ms"]    = s.playhead_ms;
    j["live_ms"]        = s.live_ms;
    j["earliest_ms"]    = s.earliest_ms;
    j["started_ms"]     = s.started_ms;
    j["end_ms"]         = s.end_ms;
    j["total_ms"]       = s.total_ms;
    j["seek_target_ms"] = s.seek_target_ms;
    j["behind_live_s"]  = s.behind_live_s;
    j["buffered_ahead_s"] = s.buffered_ahead_s;
    j["cached_segments"]  = (unsigned long long)s.cached;

    j["link_health"] = s.link_health;
    j["link_known"]  = s.link_known;
    j["last_error"]  = s.last_error;
    j["colo"]        = s.colo;
    j["clock_skew_ms"] = s.clock_skew_ms;
    j["storage_host"] = s.storage_host;
    j["download_bytes_per_s"] = s.download_bytes_per_s;
    j["download_samples"]     = (unsigned long long)s.download_samples;
    // LAN / direct delivery (PROJECT-SCOPE.md §8.7, "Visibility") — absent
    // the concept entirely on a machine that has never touched a LAN host,
    // rather than a lan_active that would always read false and look like a
    // real, checked answer.
    j["lan_configured"] = s.lan_configured;
    if (s.lan_configured) j["lan_active"] = s.lan_active;

    j["audio_channels"]   = s.audio_channels;
    j["audio_track_label"] = s.audio_track_label;
    j["current_marker"]   = s.current_marker;

    json channels = json::array();
    for (const auto& c : s.channel_labels) channels.push_back(c);
    j["channel_labels"] = std::move(channels);

    json spans = json::array();
    for (const auto& sp : s.cached_spans)
        spans.push_back(json{{"from_ms", sp.first}, {"to_ms", sp.second}});
    j["cached_spans"] = std::move(spans);

    json markers = json::array();
    for (const auto& m : s.markers)
        markers.push_back(json{{"label", m.label}, {"id", m.id},
                               {"author", m.author},
                               {"at_ms", m.at_ms},
                               {"at_media_ms", m.at_media_ms}});
    j["markers"] = std::move(markers);

    const DecoderSettings& cfg = decoder_settings();
    j["configured"] = cfg.configured();
    return j.dump();
}

std::string decoder_events_json() {
    EventListing l;
    decoder_event_listing(l);

    json j;
    j["loading"]       = l.loading;
    j["listed_once"]   = l.listed_once;
    j["fallback_scan"] = l.fallback_scan;
    j["skipped"]       = l.skipped;
    j["error"]         = l.error;

    json rows = json::array();
    for (const auto& e : l.events)
        rows.push_back(json{{"event_id", e.event_id}, {"name", e.name},
                            {"started_ms", e.started_ms},
                            {"duration_s", e.duration_s}, {"state", e.state}});
    j["events"] = std::move(rows);
    return j.dump();
}

std::string decoder_settings_json() {
    const DecoderSettings& s = decoder_settings();

    json j;
    const StorageProvider provider = s.storage_provider.empty()
        ? detect_provider(s.endpoint_host, s.r2_account_id)
        : provider_from_key(s.storage_provider);
    j["storage_provider"] = provider_key(provider);
    j["endpoint_host"] = s.endpoint_host;
    j["r2_account_id"] = s.r2_account_id;
    j["bucket"]        = s.bucket;
    j["access_key_id"] = s.access_key_id;
    // Never the secret itself: the placeholder coming back means "unchanged".
    j["secret_access_key"] = s.secret_access_key.empty()
                                 ? std::string()
                                 : std::string(kSecretPlaceholder);
    j["region"]               = s.region;
    j["room_id"]              = s.room_id;
    j["prebuffer_segments"]   = s.prebuffer_segments;
    j["start_buffer_seconds"] = s.start_buffer_seconds;
    j["poll_interval_ms"]     = s.poll_interval_ms;
    j["keep_behind_segments"] = s.keep_behind_segments;
    j["buffer_minutes"]       = s.buffer_minutes;
    j["cache_dir"]            = s.cache_dir;
    j["site_name"]            = s.site_name;

    j["lan_host"] = s.lan_host;
    j["lan_port"] = s.lan_port;
    j["lan_auth_token"] = s.lan_auth_token.empty()
                              ? std::string()
                              : std::string(kSecretPlaceholder);
    return j.dump();
}

// ── Encoder commands ─────────────────────────────────────────────────────────

bool encoder_go_live(const std::string& event_name, std::string& error) {
    auto& c = BroadcastController::instance();
    BroadcastSettings s = c.settings_copy();
    // Blank means "name it now", which is what the dock does with a field
    // nobody touched.
    s.event_name = event_name.empty() ? default_event_name() : event_name;
    c.set_settings(s);

    if (!c.go_live(error)) {
        mlog_warn("go live failed: %s", error.c_str());
        return false;
    }
    return true;
}

bool encoder_end(std::string& error) {
    auto& c = BroadcastController::instance();
    if (!c.is_live()) { error = "not broadcasting"; return false; }
    c.end_broadcast();
    return true;
}

bool encoder_marker(const std::string& label, std::string& error) {
    if (label.empty()) { error = "which marker?"; return false; }
    BroadcastController::instance().drop_marker(label);
    return true;
}

// ── Settings ─────────────────────────────────────────────────────────────────

bool encoder_apply_settings(const std::string& json_body, std::string& error) {
    (void)error;
    auto& c = BroadcastController::instance();
    BroadcastSettings s = c.settings_copy();
    // The event name is per-event and belongs to Go Live: a settings save must
    // not quietly rename the broadcast that is running.
    const std::string keep = s.event_name;
    apply_encoder_settings(parse_object(json_body), s);
    s.event_name = keep;
    c.set_settings(s);
    return true;
}

bool decoder_apply_settings(const std::string& json_body, std::string& error) {
    (void)error;
    DecoderSettings s = decoder_settings_copy();
    apply_decoder_settings(parse_object(json_body), s);
    set_decoder_settings(s);
    // The sources are already running with the old figures: they re-read them,
    // exactly as saving in the dock does.
    decoder_reconfigure_all();
    return true;
}

} // namespace multisite_obs

