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

UplinkTestResult secondary_uplink_test(size_t bytes) {
    UplinkTestResult out;
    const SecondaryTarget t = secondary_target();
    if (!t.configured()) {
        out.error = "no second bucket is configured";
        return out;
    }

    multisite::S3Config cfg;
    cfg.endpoint_host     = t.endpoint_host;
    cfg.r2_account_id     = t.r2_account_id;
    cfg.bucket            = t.bucket;
    cfg.access_key_id     = t.access_key_id;
    cfg.secret_access_key = t.secret_access_key;
    cfg.region            = t.region;
    multisite::S3Transport tx(cfg);

    // Incompressible-ish and cheap to build: the point is to move bytes, not to
    // be clever. A repeated pattern would be compressed by a proxy and measure
    // the proxy instead of the link.
    std::vector<uint8_t> payload(bytes);
    uint32_t x = 0x9e3779b9u;
    for (size_t i = 0; i < bytes; ++i) {
        x = x * 1664525u + 1013904223u;
        payload[i] = (uint8_t)(x >> 24);
    }

    const std::string key = "multisite-uplink-test.bin";
    const auto t0 = std::chrono::steady_clock::now();
    multisite::PutResult r = tx.put(key, payload, "application/octet-stream", {});
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();

    // Clean up whatever landed, even on a failure part-way through.
    tx.remove(key);

    if (!r.success) {
        out.error = "upload failed: HTTP " + std::to_string(r.http_status) +
                    (r.error.empty() ? "" : " " + r.error);
        return out;
    }
    if (ms <= 0) { out.ok = true; out.mbps = 0.0; return out; }  // too fast to time
    out.ok = true;
    out.mbps = ((double)bytes * 8.0 / 1e6) / ((double)ms / 1000.0);
    return out;
}

} // namespace multisite_obs
