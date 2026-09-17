// SPDX-License-Identifier: GPL-3.0-or-later
#include "update_check.h"

#include "../core/version.h"
#include "plugin_log.h"

#include <obs-module.h>
#include <util/platform.h>

#include <curl/curl.h>

#include <atomic>
#include <mutex>
#include <thread>

namespace multisite_obs {

namespace {

// The one place this project asks the internet a question it does not need the
// answer to in order to work. github.com rather than our own site so the answer
// comes from where the releases actually are, and the API rather than a page
// because a page is HTML that would have to be parsed and will change.
//
// The LIST endpoint, not `/releases/latest`, and the difference is not
// cosmetic: `latest` deliberately skips pre-releases, and every tag this project
// cuts is one, so it answers 404 for every release there has ever been. The list
// is newest-first and `per_page=1` keeps the body to a single object.
constexpr const char* kLatestReleaseUrl =
    "https://api.github.com/repos/stageaudioworks/obs-multisite/releases?per_page=1";

// GitHub requires a User-Agent and rejects a request without one. Naming the
// plugin and its version is the honest minimum, and it is what someone reading
// their own firewall log would want to see.
constexpr const char* kUserAgent = "obs-multisite-update-check";

constexpr long kConnectTimeoutMs = 3000;
constexpr long kRequestTimeoutMs = 6000;

std::atomic<UpdateState> g_state{UpdateState::Checking};
std::mutex               g_latest_mtx;
std::string              g_latest;
std::once_flag           g_started;

std::once_flag g_enabled_once;
bool           g_enabled = true;      // on unless the operator turned it off

void load_enabled_once() {
    char* path = obs_module_config_path("update.json");
    if (!path) return;
    obs_data_t* d = obs_data_create_from_json_file(path);
    bfree(path);
    if (!d) return;                    // never set: check, which is the default
    // obs_data_has_user_value, not the value: a saved `false` must be honoured
    // rather than read as "absent", which is the bug that would make switching
    // this off last only until the next start.
    if (obs_data_has_user_value(d, "check"))
        g_enabled = obs_data_get_bool(d, "check");
    obs_data_release(d);
}

size_t append_to_string(char* data, size_t size, size_t nmemb, void* user) {
    static_cast<std::string*>(user)->append(data, size * nmemb);
    return size * nmemb;
}

bool http_get(const std::string& url, std::string& body, long& status) {
    CURL* curl = curl_easy_init();
    if (curl == nullptr) return false;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
    std::string ua = std::string("User-Agent: ") + kUserAgent;
    headers = curl_slist_append(headers, ua.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, kConnectTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, kRequestTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    const CURLcode rc = curl_easy_perform(curl);
    const bool ok = (rc == CURLE_OK) &&
                    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status) == CURLE_OK;
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ok;
}

void run(const std::string& current_version) {
    // curl_global_init is not safe to race; the S3 transport initialises it too,
    // so this is the shared one-shot rather than a second initialisation.
    static std::once_flag curl_once;
    std::call_once(curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

    std::string body;
    long status = 0;
    if (!http_get(kLatestReleaseUrl, body, status) || status != 200) {
        // Offline, behind a captive portal, rate-limited (403), or a 404 if the
        // repository ever moves. None of these is the operator's problem and
        // none of them should look like one, so the dock says nothing.
        g_state = UpdateState::Failed;
        return;
    }

    const std::string tag = multisite::latest_tag_from_release_json(body);
    if (tag.empty()) {
        g_state = UpdateState::Failed;
        return;
    }
    if (multisite::compare_versions(tag, current_version) > 0) {
        {
            std::lock_guard<std::mutex> lk(g_latest_mtx);
            g_latest = tag;
        }
        g_state = UpdateState::Newer;
        mlog_info("update check: %s is published; this build is %s",
                  tag.c_str(), current_version.c_str());
    } else {
        g_state = UpdateState::UpToDate;
        mlog_info("update check: %s is the latest release (this build is %s)",
                  tag.c_str(), current_version.c_str());
    }
}

} // namespace

bool update_check_enabled() {
    std::call_once(g_enabled_once, load_enabled_once);
    return g_enabled;
}

void update_check_set_enabled(bool on) {
    std::call_once(g_enabled_once, load_enabled_once);
    g_enabled = on;

    char* dir = obs_module_config_path("");
    if (dir) { os_mkdirs(dir); bfree(dir); }

    obs_data_t* d = obs_data_create();
    obs_data_set_bool(d, "check", on);
    char* path = obs_module_config_path("update.json");
    if (path) {
        if (!obs_data_save_json_safe(d, path, "tmp", "bak"))
            mlog_warn("could not save the update-check setting to %s", path);
        bfree(path);
    }
    obs_data_release(d);
    mlog_info("update check %s", on ? "on" : "off — nothing is fetched");
}

void update_check_start(const std::string& current_version) {
    if (!update_check_enabled()) {
        g_state = UpdateState::Disabled;
        return;
    }
    std::call_once(g_started, [current_version] {
        g_state = UpdateState::Checking;
        std::thread(run, current_version).detach();
    });
}

UpdateState update_check_state() { return g_state.load(); }

std::string update_check_latest() {
    std::lock_guard<std::mutex> lk(g_latest_mtx);
    return g_latest;
}

} // namespace multisite_obs
