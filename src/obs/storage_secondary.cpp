// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage_secondary.h"

#include "plugin_log.h"

#include "../core/s3_transport.h"

#include <obs-module.h>
#include <util/platform.h>

#include <chrono>
#include <mutex>
#include <vector>

namespace multisite_obs {

namespace {

std::once_flag    g_once;
SecondaryTarget   g_target;

void take(obs_data_t* d, const char* key, std::string& out) {
    const char* v = obs_data_get_string(d, key);
    if (v) out = v;
}

void load_once() {
    char* path = obs_module_config_path("secondary.json");
    if (!path) return;
    obs_data_t* d = obs_data_create_from_json_file(path);
    bfree(path);
    if (!d) return;                        // never set: disabled, which is right
    // has_user_value rather than get_bool: a saved `false` must be honoured
    // rather than read as absent, or switching it off would last only until the
    // next start.
    if (obs_data_has_user_value(d, "enabled"))
        g_target.enabled = obs_data_get_bool(d, "enabled");
    take(d, "storage_provider", g_target.storage_provider);
    take(d, "endpoint_host",    g_target.endpoint_host);
    take(d, "r2_account_id",    g_target.r2_account_id);
    take(d, "bucket",           g_target.bucket);
    take(d, "access_key_id",    g_target.access_key_id);
    take(d, "secret_access_key", g_target.secret_access_key);
    take(d, "region",           g_target.region);
    if (g_target.region.empty()) g_target.region = "auto";
    obs_data_release(d);
}

} // namespace

SecondaryTarget secondary_target() {
    std::call_once(g_once, load_once);
    return g_target;
}

void set_secondary_target(const SecondaryTarget& t) {
    std::call_once(g_once, load_once);     // so a later read cannot undo this
    g_target = t;

    char* dir = obs_module_config_path("");
    if (dir) { os_mkdirs(dir); bfree(dir); }

    obs_data_t* d = obs_data_create();
    obs_data_set_bool(d, "enabled", t.enabled);
    obs_data_set_string(d, "storage_provider", t.storage_provider.c_str());
    obs_data_set_string(d, "endpoint_host", t.endpoint_host.c_str());
    obs_data_set_string(d, "r2_account_id", t.r2_account_id.c_str());
    obs_data_set_string(d, "bucket", t.bucket.c_str());
    obs_data_set_string(d, "access_key_id", t.access_key_id.c_str());
    obs_data_set_string(d, "secret_access_key", t.secret_access_key.c_str());
    obs_data_set_string(d, "region", t.region.c_str());

    char* path = obs_module_config_path("secondary.json");
    if (path) {
        if (!obs_data_save_json_safe(d, path, "tmp", "bak"))
            mlog_warn("could not save the second bucket to %s", path);
        bfree(path);
    }
    obs_data_release(d);

    if (t.enabled)
        mlog_info("second bucket set to '%s' — takes effect on the next Go Live",
                  t.bucket.c_str());
    else
        mlog_info("second bucket off — nothing is mirrored");
}

bool secondary_s3_config(multisite::S3Config& out) {
    const SecondaryTarget t = secondary_target();
    if (!t.configured()) return false;
    out.endpoint_host     = t.endpoint_host;
    out.r2_account_id     = t.r2_account_id;
    out.bucket            = t.bucket;
    out.access_key_id     = t.access_key_id;
    out.secret_access_key = t.secret_access_key;
    out.region            = t.region;
    return true;
}


} // namespace multisite_obs
