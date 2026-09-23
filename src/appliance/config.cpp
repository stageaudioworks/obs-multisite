// SPDX-License-Identifier: GPL-3.0-or-later
#include "config.h"

#include "../vendor/nlohmann/json.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

using json = nlohmann::json;

namespace multisite_player {

const char* to_string(IdleMode m) {
    switch (m) {
    case IdleMode::Black:     return "black";
    case IdleMode::HoldFrame: return "hold";
    case IdleMode::Splash:    return "splash";
    case IdleMode::Image:     return "image";
    }
    return "splash";
}

IdleMode idle_mode_from_string(const std::string& s, IdleMode fallback) {
    if (s == "black")  return IdleMode::Black;
    if (s == "hold")   return IdleMode::HoldFrame;
    if (s == "splash") return IdleMode::Splash;
    if (s == "image")  return IdleMode::Image;
    return fallback;
}

const char* default_config_path() {
    return "/etc/multisite-player/config.json";
}

namespace {

// Read a value only when the file actually carries it, so a config written by
// an older version keeps this version's defaults for anything it never knew
// about — rather than resetting those fields to zero.
template <typename T>
void take(const json& j, const char* key, T& out) {
    auto it = j.find(key);
    if (it != j.end() && !it->is_null()) {
        try { out = it->get<T>(); } catch (...) { /* keep the default */ }
    }
}

} // namespace

bool Config::load(const std::string& path, std::string& error) {
    error.clear();
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        // Not an error. A box with no config still boots and shows its splash.
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();

    json j;
    try {
        j = json::parse(ss.str());
    } catch (const std::exception& e) {
        error = std::string("config is not valid JSON: ") + e.what();
        return false;
    }
    if (!j.is_object()) {
        error = "config is not a JSON object";
        return false;
    }

    take(j, "storage_provider",     storage_provider);
    take(j, "endpoint_host",        endpoint_host);
    take(j, "r2_account_id",        r2_account_id);
    take(j, "bucket",               bucket);
    take(j, "access_key_id",        access_key_id);
    take(j, "secret_access_key",    secret_access_key);
    take(j, "region",               region);

    take(j, "lan_host",             lan_host);
    take(j, "lan_port",             lan_port);
    take(j, "lan_auth_token",       lan_auth_token);

    take(j, "reporter_enabled",     reporter_enabled);
    take(j, "reporter_url",         reporter_url);
    take(j, "reporter_appliance_id", reporter_appliance_id);
    take(j, "reporter_token",       reporter_token);
    take(j, "reporter_device_id",   reporter_device_id);
    take(j, "reporter_kind",        reporter_kind);

    take(j, "room_id",              room_id);
    take(j, "site_name",            site_name);
    take(j, "pinned_event_id",      pinned_event_id);
    take(j, "follow_next_event",    follow_next_event);
    take(j, "check_updates",        check_updates);

    take(j, "prebuffer_segments",   prebuffer_segments);
    take(j, "start_buffer_seconds", start_buffer_seconds);
    take(j, "poll_interval_ms",     poll_interval_ms);
    take(j, "keep_behind_segments", keep_behind_segments);
    take(j, "buffer_minutes",       buffer_minutes);
    take(j, "max_cached_segments",  max_cached_segments);
    take(j, "stale_after_ms",       stale_after_ms);
    take(j, "cache_dir",            cache_dir);
    take(j, "hardware_decode",      hardware_decode);

    take(j, "drm_card",             drm_card);
    take(j, "connector",            connector);
    take(j, "out_width",            out_width);
    take(j, "out_height",           out_height);
    take(j, "out_fps",              out_fps);
    take(j, "tile_index",           tile_index);

    std::string idle = to_string(idle_mode);
    take(j, "idle_mode", idle);
    idle_mode = idle_mode_from_string(idle, idle_mode);
    take(j, "idle_image_path",      idle_image_path);

    take(j, "audio_enabled",        audio_enabled);
    take(j, "alsa_device",          alsa_device);
    take(j, "audio_channels",       audio_channels);
    take(j, "audio_track",          audio_track);

    take(j, "web_port",             web_port);
    take(j, "web_bind",             web_bind);

    take(j, "auto_play",            auto_play);
    take(j, "delay_from_live_s",    delay_from_live_s);
    take(j, "locked",               locked);

    take(j, "zerotier_network_id",  zerotier_network_id);
    take(j, "cloudflared_token",    cloudflared_token);

    take(j, "aes67_manage",         aes67_manage);
    take(j, "aes67_address",        aes67_address);
    take(j, "aes67_channels",       aes67_channels);
    take(j, "aes67_previous_device", aes67_previous_device);
    return true;
}

bool Config::save(const std::string& path, std::string& error) const {
    error.clear();

    json j;
    j["storage_provider"]     = storage_provider;
    j["endpoint_host"]        = endpoint_host;
    j["r2_account_id"]        = r2_account_id;
    j["bucket"]               = bucket;
    j["access_key_id"]        = access_key_id;
    j["secret_access_key"]    = secret_access_key;
    j["region"]               = region;

    j["lan_host"]             = lan_host;
    j["lan_port"]             = lan_port;
    j["lan_auth_token"]       = lan_auth_token;

    j["reporter_enabled"]      = reporter_enabled;
    j["reporter_url"]          = reporter_url;
    j["reporter_appliance_id"] = reporter_appliance_id;
    j["reporter_token"]        = reporter_token;
    j["reporter_device_id"]    = reporter_device_id;
    j["reporter_kind"]         = reporter_kind;

    j["room_id"]              = room_id;
    j["site_name"]            = site_name;
    j["pinned_event_id"]      = pinned_event_id;
    j["follow_next_event"]    = follow_next_event;
    j["check_updates"]        = check_updates;

    j["prebuffer_segments"]   = prebuffer_segments;
    j["start_buffer_seconds"] = start_buffer_seconds;
    j["poll_interval_ms"]     = poll_interval_ms;
    j["keep_behind_segments"] = keep_behind_segments;
    j["buffer_minutes"]       = buffer_minutes;
    j["max_cached_segments"]  = max_cached_segments;
    j["stale_after_ms"]       = stale_after_ms;
    j["cache_dir"]            = cache_dir;
    j["hardware_decode"]      = hardware_decode;

    j["drm_card"]             = drm_card;
    j["connector"]            = connector;
    j["out_width"]            = out_width;
    j["out_height"]           = out_height;
    j["out_fps"]              = out_fps;
    j["tile_index"]           = tile_index;
    j["idle_mode"]            = to_string(idle_mode);
    j["idle_image_path"]      = idle_image_path;

    j["audio_enabled"]        = audio_enabled;
    j["alsa_device"]          = alsa_device;
    j["audio_channels"]       = audio_channels;
    j["audio_track"]          = audio_track;

    j["web_port"]             = web_port;
    j["web_bind"]             = web_bind;

    j["auto_play"]            = auto_play;
    j["delay_from_live_s"]    = delay_from_live_s;
    j["locked"]               = locked;

    j["zerotier_network_id"]  = zerotier_network_id;
    j["cloudflared_token"]    = cloudflared_token;

    j["aes67_manage"]         = aes67_manage;
    j["aes67_address"]        = aes67_address;
    j["aes67_channels"]       = aes67_channels;
    j["aes67_previous_device"] = aes67_previous_device;

    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "cannot write " + tmp;
            return false;
        }
        out << j.dump(2) << "\n";
        out.flush();
        if (!out) {
            error = "failed writing " + tmp;
            return false;
        }
    }
    // The secret key lives in here. Lock it down before it is in place under
    // its real name, so it is never briefly world-readable.
    ::chmod(tmp.c_str(), S_IRUSR | S_IWUSR);

    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        error = "cannot replace " + path;
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

} // namespace multisite_player
