// SPDX-License-Identifier: GPL-3.0-or-later
#include "decoder_settings.h"
#include "plugin_log.h"

#include <obs-module.h>
#include <util/platform.h>

#include <mutex>

namespace multisite_obs {

static std::mutex g_mtx;

static DecoderSettings& storage() {
    static DecoderSettings s;
    return s;
}

DecoderSettings& decoder_settings() {
    static std::once_flag once;
    // Load exactly once, on first use. std::call_once avoids the re-entrancy
    // trap of taking g_mtx here and again in the setter.
    std::call_once(once, [] { storage().load(); });
    return storage();
}

DecoderSettings decoder_settings_copy() {
    (void)decoder_settings();            // ensure the initial load happened
    std::lock_guard<std::mutex> lk(g_mtx);
    return storage();
}

void set_decoder_settings(const DecoderSettings& in) {
    (void)decoder_settings();            // ensure the initial load happened
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        storage() = in;
    }
    in.save();
}

void DecoderSettings::load() {
    char* path = obs_module_config_path("decoder.json");
    if (!path) return;
    obs_data_t* d = obs_data_create_from_json_file(path);
    bfree(path);
    if (!d) return;

    storage_provider  = obs_data_get_string(d, "storage_provider");
    endpoint_host     = obs_data_get_string(d, "endpoint_host");
    r2_account_id     = obs_data_get_string(d, "r2_account_id");
    bucket            = obs_data_get_string(d, "bucket");
    access_key_id     = obs_data_get_string(d, "access_key_id");
    secret_access_key = obs_data_get_string(d, "secret_access_key");
    if (obs_data_has_user_value(d, "region"))
        region = obs_data_get_string(d, "region");
    if (obs_data_has_user_value(d, "room_id"))
        room_id = obs_data_get_string(d, "room_id");
    if (obs_data_has_user_value(d, "prebuffer_segments"))
        prebuffer_segments = (int)obs_data_get_int(d, "prebuffer_segments");
    if (obs_data_has_user_value(d, "start_buffer_seconds"))
        start_buffer_seconds = (int)obs_data_get_int(d, "start_buffer_seconds");
    if (obs_data_has_user_value(d, "poll_interval_ms"))
        poll_interval_ms = (int)obs_data_get_int(d, "poll_interval_ms");
    if (obs_data_has_user_value(d, "keep_behind_segments"))
        keep_behind_segments = (int)obs_data_get_int(d, "keep_behind_segments");
    if (obs_data_has_user_value(d, "buffer_minutes"))
        buffer_minutes = (int)obs_data_get_int(d, "buffer_minutes");
    obs_data_release(d);
}

void DecoderSettings::save() const {
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
    obs_data_set_int(d, "prebuffer_segments", prebuffer_segments);
    obs_data_set_int(d, "start_buffer_seconds", start_buffer_seconds);
    obs_data_set_int(d, "poll_interval_ms", poll_interval_ms);
    obs_data_set_int(d, "keep_behind_segments", keep_behind_segments);
    obs_data_set_int(d, "buffer_minutes", buffer_minutes);

    char* path = obs_module_config_path("decoder.json");
    if (path) {
        if (!obs_data_save_json_safe(d, path, "tmp", "bak"))
            mlog_warn("could not save decoder settings to %s", path);
        bfree(path);
    }
    obs_data_release(d);
}

} // namespace multisite_obs
