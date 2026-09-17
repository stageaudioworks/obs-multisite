// SPDX-License-Identifier: GPL-3.0-or-later
#include "update_check.h"

#include "../core/version.h"
#include "log.h"

#include <curl/curl.h>

#include <atomic>
#include <mutex>
#include <thread>

namespace multisite_player {

namespace {

// The one place this box asks the internet a question it does not need the
// answer to in order to work. The LIST endpoint rather than `/releases/latest`,
// and that is not cosmetic: `latest` skips pre-releases, every tag this project
// cuts is one, and it therefore answers 404 for every release there has ever
// been. Newest-first, one entry.
constexpr const char* kLatestReleaseUrl =
    "https://api.github.com/repos/stageaudioworks/obs-multisite/releases?per_page=1";

// GitHub requires a User-Agent. Naming the box's own client is the honest
// minimum, and is what somebody reading their own firewall log wants to see.
constexpr const char* kUserAgent = "multisite-player-update-check";

// Public-internet timeouts, unlike the AES67 probe's loopback ones: a campus
// link can be slow, but an unattended box must not sit on this. A failure is
// silent, so being generous costs nothing anyone can see.
constexpr long kConnectTimeoutMs = 5000;
constexpr long kRequestTimeoutMs = 8000;

std::atomic<bool> g_checked{false};
std::atomic<bool> g_newer{false};
std::mutex        g_latest_mtx;
std::string       g_latest;
std::once_flag    g_started;

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

void run(const std::string& current) {
    static std::once_flag curl_once;
    std::call_once(curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

    std::string body;
    long status = 0;
    if (!http_get(kLatestReleaseUrl, body, status) || status != 200) {
        // Offline, rate-limited, or the repository moved. None of it is the
        // operator's problem, so nothing is said and `checked` stays false.
        return;
    }
    const std::string tag = multisite::latest_tag_from_release_json(body);
    if (tag.empty()) return;

    if (multisite::compare_versions(tag, current) > 0) {
        {
            std::lock_guard<std::mutex> lk(g_latest_mtx);
            g_latest = tag;
        }
        g_newer = true;
        plog_info("update check: %s is published; this build is %s",
                  tag.c_str(), current.c_str());
    } else {
        plog_info("update check: %s is the latest release (this build is %s)",
                  tag.c_str(), current.c_str());
    }
    g_checked = true;
}

} // namespace

void update_check_start(const std::string& current, bool enabled) {
    if (!enabled) return;
    std::call_once(g_started, [current] {
        std::thread(run, current).detach();
    });
}

UpdateInfo update_check_info() {
    UpdateInfo out;
    out.checked = g_checked.load();
    out.newer   = g_newer.load();
    if (out.newer) {
        std::lock_guard<std::mutex> lk(g_latest_mtx);
        out.latest = g_latest;
    }
    return out;
}

} // namespace multisite_player
