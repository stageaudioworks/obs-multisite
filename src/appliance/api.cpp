// SPDX-License-Identifier: GPL-3.0-or-later
#include "api.h"
#include "log.h"
#include "sysinfo.h"
#include "video_output.h"
#include "audio_output.h"
#include "aes67.h"   // the daemon on this box: its shapes, and talking to it
#include "update_check.h"
#include "preview.h"
#include "../core/storage_providers.h"

#include "../vendor/nlohmann/json.hpp"

#include <cmath>
#include <array>

using json = nlohmann::json;

// The HTTP server lives in the shared core now — the relay and the OBS plugin's
// remote-control pages use the same one — so its names arrive qualified.
using multisite::HttpHandler;
using multisite::HttpRequest;
using multisite::HttpResponse;
using multisite::HttpServer;
using multisite::StorageProvider;
using multisite::all_providers;
using multisite::provider_key;
using multisite::provider_from_key;
using multisite::detect_provider;
using multisite::derive;

namespace multisite_player {

namespace {

// What the UI shows in place of a stored secret. Sending it back unchanged
// means "leave it alone", so an operator editing the room name does not have
// to retype the bucket's secret key — and the key never has to leave the box
// to come back again.
constexpr char kSecretPlaceholder[] = "••••••••";

json status_json(const Player& player) {
    Status s;
    player.status(s);

    json j;
    j["room_id"]        = s.room_id;
    j["room_state"]     = s.room_state;
    j["event_id"]       = s.event_id;
    j["pinned_event_id"] = s.pinned_event_id;
    j["live_elsewhere"] = s.live_elsewhere;
    j["live_event_id"]  = s.live_event_id;

    j["playing"]    = s.playing;
    j["paused"]     = s.paused;
    j["buffering"]  = s.buffering;
    j["loading"]    = s.loading;
    j["locked"]     = s.locked;
    j["configured"] = s.configured;

    j["playhead_ms"]    = s.playhead_ms;
    j["live_ms"]        = s.live_ms;
    j["earliest_ms"]    = s.earliest_ms;
    j["started_ms"]     = s.started_ms;
    j["end_ms"]         = s.end_ms;
    j["total_ms"]       = s.total_ms;
    j["seek_target_ms"] = s.seek_target_ms;
    j["behind_live_s"]  = s.behind_live_s;
    j["delay_from_live_s"] = s.delay_from_live_s;
    j["ended"]       = s.ended;
    j["at_end"]      = s.at_end;
    j["was_live"]    = s.was_live;
    j["interrupted"] = s.interrupted;
    // The authority's own answer, so the page never has to assemble it from the
    // two fields above — that expression omits the pinned case.
    j["plays_as_recording"] = s.plays_as_recording;

    j["buffered_ahead_s"]  = s.buffered_ahead_s;
    j["cached_segments"]   = (unsigned long long)s.cached_segments;
    j["downloaded"]        = s.downloaded;
    j["download_failures"] = s.download_failures;
    j["checksum_failures"] = s.checksum_failures;
    j["gaps_waited"]       = s.gaps_waited;
    j["frames_out"]        = s.frames_out;
    j["frames_dropped"]    = s.frames_dropped;
    j["link_health"]       = s.link_health;   // 0 healthy, 1 degraded, 2 offline
    j["link_known"]        = s.link_known;
    j["last_error"]        = s.last_error;

    j["video_width"]    = s.video_width;
    j["video_height"]   = s.video_height;
    j["video_layout"]   = s.video_layout;   // e.g. "2x1"; "1x1" is one picture
    j["audio_channels"] = s.audio_channels;
    j["channel_labels"] = s.channel_labels;

    j["output_description"] = s.output_description;
    j["audio_description"]  = s.audio_description;
    j["video_output_ok"]    = s.video_output_ok;
    j["audio_output_ok"]    = s.audio_output_ok;
    // "closed" / "open" / "failed", plus the card's own message when it is the
    // last of those. Sent as a word rather than three booleans because the three
    // mean different things to somebody looking at the page: for Closed the
    // answer is "switch it on", for Failed it is "go and look at the card".
    j["audio_state"]        = s.audio_state;
    j["audio_error"]        = s.audio_error;

    json spans = json::array();
    for (const auto& sp : s.cached_spans)
        spans.push_back(json{{"from_ms", sp.first}, {"to_ms", sp.second}});
    j["cached_spans"] = std::move(spans);

    json markers = json::array();
    for (const auto& m : s.markers)
        markers.push_back(json{{"label", m.label}, {"id", m.id},
                               {"author", m.author}, {"at_ms", m.at_ms},
                               {"at_media_ms", m.at_media_ms}});
    j["markers"] = std::move(markers);
    j["current_marker"] = s.current_marker;

    j["now_ms"]  = time_info().now_ms;
    j["version"] = player_version();
    return j;
}

json config_json(const Config& c) {
    json j;
    // Empty storage_provider means this was saved before the provider
    // dropdown existed (or hand-edited): fall back to guessing from the raw
    // fields rather than defaulting blindly to R2, so an upgrade never
    // misrepresents a working AWS/Backblaze/Wasabi/Custom setup as something
    // it isn't. Resolved here, once, rather than in the page's JS, on the
    // same "decide in one place" principle as the rest of storage_providers.h.
    const StorageProvider provider = c.storage_provider.empty()
        ? detect_provider(c.endpoint_host, c.r2_account_id)
        : provider_from_key(c.storage_provider);
    j["storage_provider"] = provider_key(provider);
    j["endpoint_host"]  = c.endpoint_host;
    j["r2_account_id"]  = c.r2_account_id;
    j["bucket"]         = c.bucket;
    j["access_key_id"]  = c.access_key_id;
    // Never sent out. The UI shows placeholder dots and sends them back
    // untouched unless somebody types a new key.
    j["secret_access_key"] = c.secret_access_key.empty()
                                 ? std::string()
                                 : std::string(kSecretPlaceholder);
    j["secret_set"]     = !c.secret_access_key.empty();
    j["region"]         = c.region;

    j["lan_host"] = c.lan_host;
    j["lan_port"] = c.lan_port;
    // Same placeholder convention as the bucket secret above.
    j["lan_auth_token"] = c.lan_auth_token.empty()
                              ? std::string()
                              : std::string(kSecretPlaceholder);
    j["lan_auth_token_set"] = !c.lan_auth_token.empty();

    j["room_id"]            = c.room_id;
    j["site_name"]          = c.site_name;
    j["pinned_event_id"]    = c.pinned_event_id;
    j["follow_next_event"]  = c.follow_next_event;
    j["check_updates"]      = c.check_updates;
    j["prebuffer_segments"] = c.prebuffer_segments;
    j["start_buffer_seconds"] = c.start_buffer_seconds;
    j["poll_interval_ms"]   = c.poll_interval_ms;
    j["keep_behind_segments"] = c.keep_behind_segments;
    j["buffer_minutes"]     = c.buffer_minutes;
    j["max_cached_segments"] = c.max_cached_segments;
    j["stale_after_ms"]     = c.stale_after_ms;
    j["cache_dir"]          = c.cache_dir;
    j["hardware_decode"]    = c.hardware_decode;

    j["drm_card"]        = c.drm_card;
    j["connector"]       = c.connector;
    j["out_width"]       = c.out_width;
    j["out_height"]      = c.out_height;
    j["out_fps"]         = c.out_fps;
    j["tile_index"]      = c.tile_index;
    j["idle_mode"]       = to_string(c.idle_mode);
    j["idle_image_path"] = c.idle_image_path;

    j["audio_enabled"]  = c.audio_enabled;
    j["alsa_device"]    = c.alsa_device;
    j["audio_channels"] = c.audio_channels;
    j["audio_track"] = c.audio_track;

    j["web_port"] = c.web_port;
    j["web_bind"] = c.web_bind;

    j["auto_play"]         = c.auto_play;
    j["delay_from_live_s"] = c.delay_from_live_s;
    j["locked"]            = c.locked;

    j["zerotier_network_id"] = c.zerotier_network_id;
    // The tunnel token is a credential: shown as dots and sent back unchanged
    // unless somebody types a new one, exactly like the bucket secret.
    j["cloudflared_token"] = c.cloudflared_token.empty()
                                 ? std::string()
                                 : std::string(kSecretPlaceholder);
    j["cloudflared_set"]   = !c.cloudflared_token.empty();

    j["aes67_manage"]   = c.aes67_manage;
    j["aes67_address"]  = c.aes67_address;
    j["aes67_channels"] = c.aes67_channels;
    return j;
}

// What the remote-access panel reports: the settings the operator has entered,
// and what the box has actually managed to do with them. Both halves are
// needed — "set up but not running" is the state that sends somebody looking.
json remote_json(const Config& cfg) {
    const RemoteAccess r = remote_access();
    return json{
        {"zerotier", json{
            {"installed",  r.zerotier_installed},
            {"running",    r.zerotier_running},
            {"network_id", cfg.zerotier_network_id},
            {"joined",     !r.zerotier_ip.empty()},
            {"ip",         r.zerotier_ip},
        }},
        {"cloudflared", json{
            {"installed",  r.cloudflared_installed},
            {"running",    r.cloudflared_running},
            {"configured", !cfg.cloudflared_token.empty()},
            {"hostname",   r.cloudflared_hostname},
        }},
        // The one value the splash prints, at the top level so the interface
        // does not have to know how it was obtained.
        {"remote_ip", r.zerotier_ip},
    };
}

template <typename T>
void take(const json& j, const char* key, T& out) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return;
    try { out = it->get<T>(); } catch (...) {}
}

// Merge an edit into the config in force. Anything the browser did not send
// keeps its current value, so a partial form cannot wipe settings it never
// showed.
Config apply_edit(Config c, const json& j) {
    // The provider decides how the ONE field the operator actually typed
    // becomes endpoint_host/r2_account_id/region — see storage_providers.h.
    // Custom's fields already ARE that shape, unchanged. Applied only when
    // the browser actually sent a provider (every page built against this
    // API does); an older or hand-built client that only ever sends the raw
    // fields leaves storage_provider untouched and simply edits them as
    // before Phase 13 existed here.
    auto provider_it = j.find("storage_provider");
    if (provider_it != j.end() && provider_it->is_string()) {
        const StorageProvider provider = provider_from_key(provider_it->get<std::string>());
        c.storage_provider = provider_key(provider);
        if (provider == StorageProvider::Custom) {
            take(j, "endpoint_host", c.endpoint_host);
            take(j, "region",        c.region);
            c.r2_account_id.clear();
        } else {
            std::string input;
            if (provider == StorageProvider::CloudflareR2) take(j, "r2_account_id", input);
            else                                            take(j, "region", input);
            const auto derived = derive(provider, input);
            c.endpoint_host = derived.endpoint_host;
            c.r2_account_id = derived.r2_account_id;
            c.region        = derived.region;
        }
    } else {
        take(j, "endpoint_host", c.endpoint_host);
        take(j, "r2_account_id", c.r2_account_id);
        take(j, "region",        c.region);
    }
    take(j, "bucket",        c.bucket);
    take(j, "access_key_id", c.access_key_id);
    {
        std::string secret;
        take(j, "secret_access_key", secret);
        // The placeholder means "unchanged". An empty string means the
        // operator cleared it deliberately.
        if (secret != kSecretPlaceholder) {
            auto it = j.find("secret_access_key");
            if (it != j.end() && !it->is_null()) c.secret_access_key = secret;
        }
    }
    take(j, "lan_host", c.lan_host);
    take(j, "lan_port", c.lan_port);
    {
        std::string token;
        take(j, "lan_auth_token", token);
        // Same rule as the bucket secret: the placeholder means "unchanged",
        // an empty string means the operator cleared it on purpose.
        if (token != kSecretPlaceholder) {
            auto it = j.find("lan_auth_token");
            if (it != j.end() && !it->is_null()) c.lan_auth_token = token;
        }
    }
    take(j, "room_id", c.room_id);
    take(j, "site_name", c.site_name);

    take(j, "prebuffer_segments",   c.prebuffer_segments);
    take(j, "start_buffer_seconds", c.start_buffer_seconds);
    take(j, "poll_interval_ms",     c.poll_interval_ms);
    take(j, "keep_behind_segments", c.keep_behind_segments);
    take(j, "buffer_minutes",       c.buffer_minutes);
    take(j, "max_cached_segments",  c.max_cached_segments);
    take(j, "stale_after_ms",       c.stale_after_ms);
    take(j, "cache_dir",            c.cache_dir);
    take(j, "hardware_decode",      c.hardware_decode);
    take(j, "follow_next_event",    c.follow_next_event);
    take(j, "check_updates",        c.check_updates);

    take(j, "drm_card",   c.drm_card);
    take(j, "connector",  c.connector);
    take(j, "out_width",  c.out_width);
    take(j, "out_height", c.out_height);
    take(j, "out_fps",    c.out_fps);
    take(j, "tile_index", c.tile_index);
    {
        std::string idle;
        take(j, "idle_mode", idle);
        if (!idle.empty()) c.idle_mode = idle_mode_from_string(idle, c.idle_mode);
    }
    take(j, "idle_image_path", c.idle_image_path);

    take(j, "audio_enabled",  c.audio_enabled);
    take(j, "alsa_device",    c.alsa_device);
    take(j, "audio_channels", c.audio_channels);
    take(j, "audio_track", c.audio_track);

    take(j, "web_port", c.web_port);
    take(j, "web_bind", c.web_bind);

    take(j, "auto_play",         c.auto_play);
    take(j, "delay_from_live_s", c.delay_from_live_s);
    take(j, "locked",            c.locked);

    take(j, "zerotier_network_id", c.zerotier_network_id);
    {
        std::string token;
        take(j, "cloudflared_token", token);
        // Same rule as the bucket secret: dots mean "unchanged", an empty
        // string means the operator cleared it on purpose.
        if (token != kSecretPlaceholder) {
            auto it = j.find("cloudflared_token");
            if (it != j.end() && !it->is_null()) c.cloudflared_token = token;
        }
    }

    take(j, "aes67_manage",   c.aes67_manage);
    take(j, "aes67_address",  c.aes67_address);
    take(j, "aes67_channels", c.aes67_channels);

    // Guard rails, so a mistyped figure cannot make the box unusable from the
    // very interface being used to fix it.
    if (c.poll_interval_ms   < 500)  c.poll_interval_ms = 500;
    if (c.prebuffer_segments < 0)    c.prebuffer_segments = 0;
    if (c.start_buffer_seconds < 0)  c.start_buffer_seconds = 0;
    if (c.buffer_minutes     < 1)    c.buffer_minutes = 1;
    if (c.max_cached_segments < 10)  c.max_cached_segments = 10;
    if (c.stale_after_ms     < 30000) c.stale_after_ms = 30000;
    if (c.web_port < 1 || c.web_port > 65535) c.web_port = 8080;
    if (c.lan_port < 1 || c.lan_port > 65535) c.lan_port = 9080;
    return c;
}

double num_param(const HttpRequest& req, const char* name, double fallback) {
    const std::string v = req.param(name);
    if (v.empty()) return fallback;
    try { return std::stod(v); } catch (...) { return fallback; }
}

bool bool_param(const HttpRequest& req, const char* name, bool fallback) {
    const std::string v = req.param(name);
    if (v.empty()) return fallback;
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

// ── Reading a POST that the interface sends as JSON ──────────────────────────
// `param()` reads the query string only, and the interface sends a JSON body —
// so both are honoured, the body first. The query string is not redundancy for
// its own sake: it is what somebody at a terminal reaches for when they are
// testing a box with curl, and there is no reason for the two to differ.
json body_json(const HttpRequest& req) {
    if (req.body.empty()) return json::object();
    try {
        return json::parse(req.body);
    } catch (...) {
        // A body that will not parse is treated as absent, so the query string
        // still gets its chance rather than the request failing over a
        // malformed one.
        return json::object();
    }
}

std::string text_param(const HttpRequest& req, const json& body,
                       const char* name) {
    const auto it = body.find(name);
    if (it != body.end() && it->is_string()) return it->get<std::string>();
    return req.param(name);
}

bool flag_param(const HttpRequest& req, const json& body, const char* name,
                bool fallback) {
    const auto it = body.find(name);
    if (it != body.end()) {
        if (it->is_boolean()) return it->get<bool>();
        if (it->is_number())  return it->get<int>() != 0;
    }
    return bool_param(req, name, fallback);
}

double number_param(const HttpRequest& req, const json& body, const char* name,
                    double fallback) {
    const auto it = body.find(name);
    if (it != body.end()) {
        if (it->is_number()) return it->get<double>();
        if (it->is_string()) {
            try { return std::stod(it->get<std::string>()); } catch (...) {}
        }
    }
    return num_param(req, name, fallback);
}

} // namespace

void register_api(HttpServer& server, Player& player, std::string config_path) {

    // ── Status ───────────────────────────────────────────────────────────────
    server.route("GET", "/api/status", [&player](const HttpRequest&,
                                                 HttpResponse& res) {
        res.json(status_json(player).dump());
    });

    // Every control answers with the new status, so the interface never has to
    // guess what its own click did.
    auto control = [&player, config_path](const char* name,
                                          std::function<void(const HttpRequest&)> act) {
        return [&player, name, act](const HttpRequest& req, HttpResponse& res) {
            if (player.locked()) {
                res.status = 409;
                res.json(json{{"error", "the controls are locked"},
                              {"locked", true}}.dump());
                return;
            }
            (void)name;
            act(req);
            res.json(status_json(player).dump());
        };
    };

    server.route("POST", "/api/play",   control("play",
        [&player](const HttpRequest&) { player.play(); }));
    server.route("POST", "/api/stop",   control("stop",
        [&player](const HttpRequest&) { player.stop_playback(); }));
    server.route("POST", "/api/hold",   control("hold",
        [&player](const HttpRequest&) { player.pause(); }));
    server.route("POST", "/api/continue", control("continue",
        [&player](const HttpRequest&) { player.resume(); }));
    server.route("POST", "/api/toggle", control("toggle",
        [&player](const HttpRequest&) { player.toggle_pause(); }));
    server.route("POST", "/api/catch-up", control("catch up",
        [&player](const HttpRequest&) { player.jump_to_live(); }));
    server.route("POST", "/api/seek", control("go to",
        [&player](const HttpRequest& req) {
            player.seek_to_media((long long)num_param(req, "ms", 0));
        }));
    server.route("POST", "/api/jog", control("jog",
        [&player](const HttpRequest& req) {
            player.jog(num_param(req, "seconds", 0));
        }));
    server.route("POST", "/api/delay", control("stay behind live",
        [&player](const HttpRequest& req) {
            player.set_delay_from_live(num_param(req, "seconds", 0));
        }));
    server.route("POST", "/api/marker", control("go to cue",
        [&player](const HttpRequest& req) {
            player.jump_to_marker(req.param("id"));
        }));
    // Drop a cue with an operator-typed name. The one control here that
    // WRITES, so a refusal is reported to the page rather than swallowed —
    // "no site name is set" is something an operator has to fix.
    server.route("POST", "/api/cue", [&player](const HttpRequest& req,
                                               HttpResponse& res) {
        std::string err;
        if (!player.add_cue(req.param("label"), err)) {
            res.status = 409;
            res.json(json{ {"ok", false}, {"error", err} }.dump());
            return;
        }
        res.json(status_json(player).dump());
    });
    server.route("POST", "/api/load", control("load",
        [&player](const HttpRequest& req) {
            const std::string id = req.param("event");
            if (id.empty()) player.unpin_event(); else player.pin_event(id);
        }));
    server.route("POST", "/api/follow-live", control("follow live",
        [&player](const HttpRequest&) { player.unpin_event(); }));

    // Deliberately NOT behind the lock: locking is a control, and a lock you
    // cannot undo from the interface is a fault, not a safeguard.
    server.route("POST", "/api/lock", [&player, config_path](
                                          const HttpRequest& req,
                                          HttpResponse& res) {
        player.set_locked(bool_param(req, "on", true));
        Config c = player.config();
        std::string err;
        c.save(config_path, err);
        res.json(status_json(player).dump());
    });

    // ── Event list ───────────────────────────────────────────────────────────
    server.route("GET", "/api/events", [&player](const HttpRequest&,
                                                 HttpResponse& res) {
        EventListing listing;
        player.event_listing(listing);

        json rows = json::array();
        for (const auto& e : listing.events)
            rows.push_back(json{{"event_id", e.event_id},
                                {"name", e.name},
                                {"started_ms", e.started_ms},
                                {"duration_s", e.duration_s},
                                {"state", e.state}});
        res.json(json{{"events", std::move(rows)},
                      {"loading", listing.loading},
                      {"listed_once", listing.listed_once},
                      {"fallback_scan", listing.fallback_scan},
                      {"skipped", listing.skipped},
                      {"no_catalog", listing.no_catalog},
                      {"error", listing.error}}.dump());
    });

    server.route("POST", "/api/events/refresh", [&player](const HttpRequest&,
                                                          HttpResponse& res) {
        player.refresh_events();
        res.json(json{{"ok", true}}.dump());
    });

    // ── Settings ─────────────────────────────────────────────────────────────
    // What the storage-provider dropdown offers (PROJECT-SCOPE.md §8.6) —
    // static, so fetched once rather than folded into every /api/config
    // response.
    server.route("GET", "/api/storage/providers", [](const HttpRequest&,
                                                     HttpResponse& res) {
        json list = json::array();
        for (const auto& info : all_providers()) {
            list.push_back(json{
                {"key", info.key},
                {"display_name", info.display_name},
                {"needs_account_id", info.needs_account_id},
                {"needs_region", info.needs_region},
                {"needs_endpoint", info.needs_endpoint},
                {"available", info.available},
            });
        }
        res.json(json{{"providers", list}}.dump());
    });

    server.route("GET", "/api/config", [&player](const HttpRequest&,
                                                 HttpResponse& res) {
        res.json(config_json(player.config()).dump());
    });

    server.route("PUT", "/api/config", [&player, config_path](
                                           const HttpRequest& req,
                                           HttpResponse& res) {
        json body;
        try {
            body = json::parse(req.body);
        } catch (const std::exception& e) {
            res.status = 400;
            res.json(json{{"error", std::string("bad request: ") + e.what()}}.dump());
            return;
        }
        Config updated = apply_edit(player.config(), body);

        std::string err;
        if (!updated.save(config_path, err)) {
            // Saving is what makes a setting survive the next power cut, so a
            // failure here must be reported rather than silently applied.
            res.status = 500;
            res.json(json{{"error", "could not save settings: " + err}}.dump());
            return;
        }
        player.reconfigure(updated);
        res.json(config_json(player.config()).dump());
    });

    // What this box can actually be set to. Listed from the hardware rather
    // than typed into a form, so a setting the display would refuse cannot be
    // chosen in the first place.
    server.route("GET", "/api/outputs", [&player](const HttpRequest&,
                                                  HttpResponse& res) {
        json displays = json::array();
        for (const auto& d : player.video().displays()) {
            json modes = json::array();
            for (const auto& m : d.modes)
                modes.push_back(json{{"width", m.width}, {"height", m.height},
                                     {"refresh_mhz", m.refresh_mhz},
                                     {"preferred", m.preferred}});
            displays.push_back(json{{"connector", d.connector},
                                    {"connected", d.connected},
                                    {"monitor_name", d.monitor_name},
                                    {"modes", std::move(modes)}});
        }
        json devices = json::array();
        const std::string net_card = aes67_card_device(kAes67CardName);
        const Config cfg = player.config();
        for (const auto& a : player.audio().devices()) {
            // Marked so the page can say why one of them cannot be chosen: this
            // is the card the AES67 daemon reads, so it is the one device whose
            // selection is owned by the network output rather than by whoever
            // is looking at the settings page.
            const bool net = !net_card.empty() && a.id == net_card;
            devices.push_back(json{{"id", a.id},
                                   {"description", a.description},
                                   {"max_channels", a.max_channels},
                                   {"locks_audio_device", net}});
        }
        // While the network output is on, the device is not the operator's to
        // choose: the sound has to be on the AES67 card, because that is what
        // the stream publishes. The page disables the picker and needs to say
        // *why*, and it must not send a device either — a disabled picker still
        // has a value, and saving that value is what silently moved the sound
        // off the card while the stream stayed switched on.
        const bool locked = cfg.aes67_manage;
        res.json(json{{"displays", std::move(displays)},
                      {"audio_devices", std::move(devices)},
                      {"display_in_use", player.video().description()},
                      {"audio_in_use", player.audio().description()},
                      {"audio_device_locked", locked},
                      {"audio_device_lock_reason", locked
                           ? std::string("the network audio output is on: the "
                                         "sound goes to the AES67 card because "
                                         "that is the card the stream publishes")
                           : std::string()}}.dump());
    });

    // ── The box ──────────────────────────────────────────────────────────────
    server.route("GET", "/api/system", [&player](const HttpRequest&,
                                                 HttpResponse& res) {
        const SystemInfo sys = system_info();
        const TimeInfo   t   = time_info();
        const Config     cfg = player.config();
        const DiskInfo   disk = disk_info(cfg.cache_dir);

        json nets = json::array();
        for (const auto& n : network_interfaces())
            nets.push_back(json{{"name", n.name}, {"ipv4", n.ipv4},
                                {"mac", n.mac}, {"up", n.up},
                                {"wireless", n.wireless}});

        const UpdateInfo upd = update_check_info();

        res.json(json{
            {"hostname", hostname()},
            {"version", player_version()},
            // Whether a newer release exists, when the once-per-run check got
            // an answer. `update_checked` is false when it could not ask, which
            // the page shows as nothing at all.
            {"update_checked", upd.checked},
            {"update_newer", upd.newer},
            {"update_latest", upd.latest},
            {"model", sys.model},
            {"os_version", sys.os_version},
            {"kernel", sys.kernel},
            {"uptime_s", sys.uptime_s},
            {"load_1min", sys.load_1min},
            {"cpu_temp_c", sys.cpu_temp_c},
            {"throttled", sys.throttled},
            {"under_voltage", sys.under_voltage},
            {"interfaces", std::move(nets)},
            {"time", json{{"now_ms", t.now_ms},
                          {"local_time", t.local_time},
                          {"timezone", t.timezone},
                          {"ntp_enabled", t.ntp_enabled},
                          {"ntp_synchronised", t.ntp_synchronised},
                          {"clock_skew_ms", player.clock_skew_ms()}}},
            {"disk", json{{"path", disk.path},
                          {"total_bytes", disk.total_bytes},
                          {"free_bytes", disk.free_bytes},
                          {"is_sd_card", disk.is_sd_card},
                          {"health", disk.health}}},
        }.dump());
    });

    // ── Storage ──────────────────────────────────────────────────────────────
    // Is the bucket reachable, which Cloudflare edge is serving it, and what
    // is the link managing? Asked on demand rather than on the status poll,
    // because ?probe=1 spends a request; without it the figures come from the
    // segment traffic already flowing and cost nothing.
    server.route("GET", "/api/storage", [&player](const HttpRequest& req,
                                                  HttpResponse& res) {
        const bool probe = bool_param(req, "probe", false);
        const Player::StorageHealth h = player.storage_health(probe);
        json j{
            {"configured", h.configured},
            {"endpoint", h.endpoint},
            {"bucket", h.bucket},
            {"room", h.room},
            {"reachable", h.reachable},
            {"readable", h.readable},
            {"http_status", h.http_status},
            {"error", h.error},
            {"probed", probe},
            {"colo", h.colo},
            {"server", h.server},
            {"rate_samples", h.rate_samples},
            {"lan_configured", h.lan_configured},
        };
        // Only send figures that mean something. A zero rate and a zero round
        // trip read as "the link is dead" when they actually mean "nothing has
        // been measured", which is the sort of display that sends somebody to
        // fix a link that is fine.
        if (h.rate_samples > 0) j["bytes_per_s"] = h.bytes_per_s;
        if (probe && h.round_trip_ms > 0) j["round_trip_ms"] = h.round_trip_ms;
        // Absent entirely (not just false) on a box that has never touched a
        // LAN host — the "via LAN"/"via cloud" distinction is meaningless if
        // there is no LAN leg to distinguish it from at all.
        if (h.lan_configured) j["lan_active"] = h.lan_active;
        res.json(j.dump());
    });

    server.route("GET", "/api/system/timezones", [](const HttpRequest&,
                                                    HttpResponse& res) {
        res.json(json{{"timezones", available_timezones()}}.dump());
    });

    server.route("POST", "/api/system/time", [](const HttpRequest& req,
                                                HttpResponse& res) {
        std::string err;
        if (!req.param("timezone").empty())
            err = set_timezone(req.param("timezone"));
        if (err.empty() && !req.param("ntp").empty())
            err = set_ntp(bool_param(req, "ntp", true));
        if (err.empty() && !req.param("epoch_ms").empty())
            err = set_time((long long)num_param(req, "epoch_ms", 0));

        if (!err.empty()) {
            res.status = 500;
            res.json(json{{"error", err}}.dump());
            return;
        }
        const TimeInfo t = time_info();
        res.json(json{{"now_ms", t.now_ms}, {"local_time", t.local_time},
                      {"timezone", t.timezone},
                      {"ntp_enabled", t.ntp_enabled},
                      {"ntp_synchronised", t.ntp_synchronised}}.dump());
    });

    server.route("POST", "/api/system/restart", [](const HttpRequest&,
                                                   HttpResponse& res) {
        restart_service();
        res.json(json{{"ok", true}, {"message", "restarting"}}.dump());
    });
    server.route("POST", "/api/system/reboot", [](const HttpRequest&,
                                                  HttpResponse& res) {
        reboot_box();
        res.json(json{{"ok", true}, {"message", "rebooting"}}.dump());
    });
    server.route("POST", "/api/system/shutdown", [](const HttpRequest&,
                                                    HttpResponse& res) {
        shutdown_box();
        res.json(json{{"ok", true}, {"message", "shutting down"}}.dump());
    });

    // ── Remote access ────────────────────────────────────────────────────────
    // The whole reason these exist is that nobody drives to the campus to
    // touch the box, so they are deliberately not behind the lock: a lock you
    // cannot reach past when the box is off site is a fault, not a safeguard.
    server.route("GET", "/api/remote", [&player](const HttpRequest&,
                                                 HttpResponse& res) {
        res.json(remote_json(player.config()).dump());
    });

    server.route("POST", "/api/remote", [&player, config_path](
                                            const HttpRequest& req,
                                            HttpResponse& res) {
        json body;
        try {
            body = json::parse(req.body);
        } catch (const std::exception& e) {
            res.status = 400;
            res.json(json{{"error", std::string("bad request: ") + e.what()}}
                         .dump());
            return;
        }

        Config updated = player.config();
        take(body, "zerotier_network_id", updated.zerotier_network_id);
        {
            std::string token;
            take(body, "cloudflared_token", token);
            if (token != kSecretPlaceholder) {
                auto it = body.find("cloudflared_token");
                if (it != body.end() && !it->is_null())
                    updated.cloudflared_token = token;
            }
        }

        std::string err;
        if (!updated.save(config_path, err)) {
            res.status = 500;
            res.json(json{{"error", "could not save settings: " + err}}.dump());
            return;
        }

        // Bring the settings into force. A failure here is reported alongside
        // the state, not thrown away: the likely cause is a package that the
        // installer will add on its next run, and the setting must survive to
        // be picked up then.
        const std::string zt_err = apply_zerotier(updated.zerotier_network_id);
        const std::string cf_err = apply_cloudflared(updated.cloudflared_token);

        player.reconfigure(updated);
        json out = remote_json(updated);
        if (!zt_err.empty()) out["zerotier_error"] = zt_err;
        if (!cf_err.empty()) out["cloudflared_error"] = cf_err;
        res.json(out.dump());
    });

    // ── Preview ──────────────────────────────────────────────────────────────
    // One JPEG per request rather than a stream: a dropped connection costs a
    // single frame instead of the whole preview, and a phone that goes to
    // sleep stops asking without leaving anything encoding on the box.
    //
    // The encoder is shared and serialised: several browsers looking at once
    // must not each start their own. The last frame that encoded successfully
    // is kept so a transient failure — nothing decoded yet, or an encode that
    // hiccuped — serves the last good picture instead of blanking the preview.
    // There is one such cache per view, because the two views are different
    // pictures: handing one back where the other was asked for would be a
    // quiet lie about what is on the screen in the room.
    {
        // `view=out` is the region being sent to the output — which is the
        // whole picture when no tile is selected — and `view=feed` is
        // everything the box received, tiles and all. Absent means "out", so a
        // bare /preview.jpg shows what the room is showing.
        struct PreviewCache {
            std::vector<uint8_t> jpeg;
            uint64_t             version = 0;
            bool                 have = false;
        };
        auto encoder = std::make_shared<JpegEncoder>();
        auto encoder_mtx = std::make_shared<std::mutex>();
        auto cache = std::make_shared<std::array<PreviewCache, 2>>();
        auto logged_fail = std::make_shared<bool>(false);
        server.route("GET", "/preview.jpg",
                     [&player, encoder, encoder_mtx, cache, logged_fail](
                         const HttpRequest& req, HttpResponse& res) {
            const std::string view = req.param("view");
            const bool whole_feed = (view == "feed" || view == "whole");
            const int slot = whole_feed ? 1 : 0;

            multisite::DecodedVideoFrame frame;
            uint64_t version = 0;
            const int width = std::max(
                160, std::min(1920, (int)num_param(req, "width", 640)));
            const int quality = std::max(
                10, std::min(95, (int)num_param(req, "quality", 70)));

            bool ok = false;
            std::string err;
            if (player.latest_frame(frame, version, !whole_feed)) {
                std::vector<uint8_t> jpeg;
                std::lock_guard<std::mutex> lk(*encoder_mtx);
                ok = encoder->encode(frame, width, quality, jpeg, err);
                if (ok) {
                    (*cache)[slot].jpeg = std::move(jpeg);
                    (*cache)[slot].version = version;
                    (*cache)[slot].have = true;
                } else if (!*logged_fail) {
                    *logged_fail = true;
                    plog_warn("preview: %s", err.c_str());
                }
            }

            if (ok || (*cache)[slot].have) {
                // A failed encode serves the last good frame rather than a
                // bare error page, so the preview never blanks.
                const PreviewCache& c = (*cache)[slot];
                res.content_type = "image/jpeg";
                res.headers["X-Frame-Version"] = std::to_string(c.version);
                res.body.assign(c.jpeg.begin(), c.jpeg.end());
            } else {
                res.status = 503;
                res.content_type = "text/plain; charset=utf-8";
                res.body = err.empty() ? "nothing decoded yet" : err;
            }
        });
    }

    // ── Log ──────────────────────────────────────────────────────────────────
    // An operator with a phone and no SSH should still be able to see why
    // nothing is playing.
    server.route("GET", "/api/log", [](const HttpRequest& req,
                                       HttpResponse& res) {
        const size_t lines = (size_t)num_param(req, "lines", 120);
        json rows = json::array();
        for (const auto& e : recent_log(lines))
            rows.push_back(json{{"at_ms", e.at_ms},
                                {"level", to_string(e.level)},
                                {"text", e.text}});
        res.json(json{{"lines", std::move(rows)}}.dump());
    });

    // ── The sound on the network (AES67) ─────────────────────────────────────
    // The daemon on this box owns the stream; these routes are how an operator
    // sees it and switches it. The state is gathered fresh on every request
    // rather than cached: it is a handful of loopback calls, and a stale answer
    // to "is the sound leaving the building?" is worse than a slow one.
    //
    // Reading is never gated by the lock — somebody should be able to find out
    // that the network feed is down without unlocking anything. Changing it is
    // gated, because it changes what is on air, and this is exactly the sort of
    // setting a tablet left on a music stand must not be able to alter.
    auto aes67_state = [&player]() {
        const Config c = player.config();
        return aes67_probe(c.alsa_device, c.aes67_channels, c.aes67_address);
    };

    auto aes67_json = [](const Aes67State& s) {
        return json{
            {"installed",       s.installed},
            {"service_active",  s.service_active},
            {"rest_reachable",  s.rest_reachable},
            {"port",            s.port},
            {"ptp_known",       s.ptp_known},
            {"ptp_locked",      s.ptp_locked},
            {"ptp_status",      s.ptp_status_word},
            {"ptp_gmid",        s.ptp_gmid},
            {"ptp_jitter",      s.ptp_jitter},
            {"sources_known",   s.sources_known},
            {"source_present",  s.source_present},
            {"source_enabled",  s.source_enabled},
            {"source_correct",  s.source_correct},
            {"source_channels", s.source_channels},
            {"source_address",  s.source_address},
            {"source_name",     s.source_name},
            {"sdp_valid",       s.sdp_valid},
            {"sdp_port",        s.sdp_port},
            {"sdp_codec",       s.sdp_codec},
            {"sdp_channels",    s.sdp_channels},
            {"sdp_ptp",         s.sdp_ptp},
            // The whole SDP, because it is the one thing a console's engineer
            // will ask for and it is only a couple of hundred bytes.
            {"sdp",             s.sdp_text},
            {"card_present",    s.card_present},
            {"player_on_card",  s.player_on_card},
            // Everything that has to be true for the sound to actually leave,
            // concluded once here rather than re-derived in the interface.
            {"carrying_audio",  s.carrying_audio()},
            {"error",           s.error},
        };
    };

    // The refusal the locked controls share. Not the `control` helper above:
    // that one wraps the player's own controls and answers with its status,
    // whereas these act on another process entirely.
    auto aes67_locked = [&player](HttpResponse& res) {
        if (!player.locked()) return false;
        res.status = 409;
        res.json(json{{"error", "the controls are locked"},
                      {"locked", true}}.dump());
        return true;
    };

    server.route("GET", "/api/aes67", [aes67_state, aes67_json](
                                         const HttpRequest&, HttpResponse& res) {
        res.json(aes67_json(aes67_state()).dump());
    });

    // What the sound card is being given, channel by channel.
    //
    // Taken where the samples leave the player rather than where they arrive
    // from the network, so a muted box and a card that will not open both read
    // as silence — and `reason` says which of the several causes it is. Polled
    // rather than streamed, and polled by the page only while its meters are on
    // screen: a bar meter at two readings a second is a bar meter, and a
    // websocket for it would be a second protocol to keep working for no
    // visible gain.
    server.route("GET", "/api/audio/levels", [&player](const HttpRequest&,
                                                      HttpResponse& res) {
        const AudioMeterView m = player.audio_meter();
        json peak = json::array();
        for (float v : m.peak) peak.push_back(v);
        json db = json::array();
        for (float v : m.db) db.push_back(v);
        res.json(json{{"channels", (int)m.peak.size()},
                      {"peak", std::move(peak)},
                      {"db", std::move(db)},
                      {"live", m.live},
                      {"reason", to_string(m.reason)},
                      {"reason_text", m.reason_text}}.dump());
    });

    // The operator's switch, and the address and width it publishes. One route
    // because they are one decision: asking for the sound on the network means
    // asking for a stream of a given width at a given address, and doing it in a
    // single call is what makes it one action rather than three that can
    // disagree with each other if the second one fails.
    server.route("POST", "/api/aes67/source",
                 [&player, config_path, aes67_state, aes67_json, aes67_locked](
                     const HttpRequest& req, HttpResponse& res) {
        if (aes67_locked(res)) return;

        // Held while the source is rewritten, against the reconciler's own
        // pass, which takes the same lock. This is an operator saying what they
        // want, and a repair that was already in flight must not land a moment
        // later and undo it — switching the stream off and having it come back
        // on by itself is the worst possible answer to a deliberate act.
        std::lock_guard<std::mutex> lk(player.aes67_mutex());

        const json body = body_json(req);
        Config updated = player.config();
        const bool enabled =
            flag_param(req, body, "enabled", updated.aes67_manage);
        updated.aes67_manage = enabled;

        const std::string addr = text_param(req, body, "address");
        if (!addr.empty()) updated.aes67_address = addr;

        updated.aes67_channels = (int)number_param(
            req, body, "channels", updated.aes67_channels);

        // ── The sound has to go to the AES67 card, or there is no stream ─────
        // There is one output device here, not two. The daemon publishes what is
        // written to the AES67 card, so if the player is sending the sound
        // anywhere else, the stream is silent however healthy it looks — and
        // that is the state this moves rather than warns about.
        //
        // The card is not named here: it is asked for by name and the box's own
        // device list is asked which ALSA id that is, through the same helper
        // the reconciler uses. Guessing an id is how the setting came to name a
        // device the box does not have.
        const std::string net_card = aes67_card_device(kAes67CardName);
        std::vector<std::string> alsa_ids;
        for (const auto& d : player.audio().devices()) alsa_ids.push_back(d.id);
        const std::string want =
            aes67_pick_alsa_device(updated.alsa_device, net_card, alsa_ids);
        if (enabled) {
            if (!net_card.empty() && updated.alsa_device != want) {
                // Remember where the room's sound was, so that switching this
                // off puts it back there rather than guessing.
                updated.aes67_previous_device = updated.alsa_device;
                updated.alsa_device = want;
            }
        } else if (aes67_device_is_card(updated.alsa_device, kAes67CardName)) {
            // Only if the sound is actually on that card: a box somebody has
            // already pointed at HDMI deliberately must be left alone. The
            // comparison is a card comparison, not a substring search —
            // "RAVENNA2" is a different card and must not be moved.
            updated.alsa_device = updated.aes67_previous_device.empty()
                                      ? std::string("default")
                                      : updated.aes67_previous_device;
            updated.aes67_previous_device.clear();
        }

        // Saved before the daemon is asked, so that a daemon which refuses still
        // leaves the box remembering what it was asked to do — and so the
        // setting survives the power cut that might have caused the refusal.
        std::string err;
        if (!updated.save(config_path, err)) {
            res.status = 500;
            res.json(json{{"error", "could not save settings: " + err}}.dump());
            return;
        }
        player.reconfigure(updated);

        const int width = aes67_channels_to_map(updated.aes67_channels);
        const std::string address =
            aes67_address_or_default(updated.aes67_address);
        // The name is what appears in a console's source list, so it says which
        // room this is rather than what it is running.
        const std::string name = "Multisite " + hostname();

        std::string problem;
        if (enabled && net_card.empty()) {
            // Said rather than done. Pointing the player at a card that is not
            // registered would silence the room for a stream that could not have
            // worked anyway, so the sound is left where it was and the reason is
            // given. The setting itself is kept: a module that was not loaded yet
            // being loaded is then all that is missing.
            problem =
                "the AES67 card is not registered with ALSA, so there is "
                "nothing for the sound to go to — the kernel module is probably "
                "not loaded. The room's sound has been left where it was.";
        } else if (enabled) {
            // Started *and* enabled, together, every time: enabling is what
            // makes it come back after a power cut, which is the promise the
            // interface makes, and a daemon that happens to be running but is
            // not enabled would break it silently. Both calls are idempotent,
            // so repeating them costs nothing.
            problem = aes67_set_service(true, true);
            if (problem.empty())
                problem = aes67_ensure_source(width, address, name, true);
        } else if (aes67_state().source_present) {
            // Switching off stops the stream rather than deleting it: the
            // address and width are kept, so switching it back on is one click
            // and not a re-entry of everything.
            problem = aes67_set_source_enabled(false);
        }

        json result = aes67_json(aes67_state());
        if (!problem.empty()) result["problem"] = problem;
        res.json(result.dump());
    });

    // Starting and stopping the daemon itself. An engineer's control rather than
    // an operator's: stopping it takes the whole stack down, including anything
    // else that has been aimed at this box's streams.
    server.route("POST", "/api/aes67/service",
                 [&player, aes67_state, aes67_json, aes67_locked](
                     const HttpRequest& req, HttpResponse& res) {
        if (aes67_locked(res)) return;

        // The same lock the reconciler takes. Stopping the daemon is a
        // deliberate act and must not be undone four seconds later by a watch
        // that noticed it was down.
        std::lock_guard<std::mutex> lk(player.aes67_mutex());

        const json body = body_json(req);
        const bool start = flag_param(req, body, "start", true);
        // Enabling is what survives a power cut, and stopping is what somebody
        // does for an afternoon, so they are asked for separately. Defaulting
        // `enable` to the same as `start` is right for both: starting a daemon
        // that will not come back is rarely what was meant.
        const bool enable = flag_param(req, body, "enable", start);
        const std::string problem = aes67_set_service(start, enable);

        json result = aes67_json(aes67_state());
        if (!problem.empty()) result["problem"] = problem;
        res.json(result.dump());
    });
}

} // namespace multisite_player
