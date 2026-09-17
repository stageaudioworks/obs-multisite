// SPDX-License-Identifier: GPL-3.0-or-later
#include "lan_transport.h"
#include "../vendor/nlohmann/json.hpp"

#include <curl/curl.h>

#include <mutex>

using json = nlohmann::json;

namespace multisite {

static std::once_flag g_curl_once;
static void ensure_curl() {
    std::call_once(g_curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

// Same technique as S3Transport's cancel_pending (see transport.h): libcurl
// calls this periodically DURING a transfer, on the thread that called
// curl_easy_perform, and a non-zero return aborts right there instead of
// waiting out the timeout.
static int curl_abort_cb(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* cancel = static_cast<std::atomic<bool>*>(clientp);
    return (cancel && cancel->load()) ? 1 : 0;
}

static size_t write_to_vec(void* ptr, size_t sz, size_t nm, void* ud) {
    auto* v = static_cast<std::vector<uint8_t>*>(ud);
    size_t n = sz * nm;
    const uint8_t* p = static_cast<const uint8_t*>(ptr);
    v->insert(v->end(), p, p + n);
    return n;
}

LanTransport::LanTransport(LanTransportConfig cfg) : m_cfg(std::move(cfg)) {}

std::string LanTransport::url_for(const std::string& key) const {
    // The LanObjectServer's routes are, by design, the exact object-key shape
    // a cloud decoder already asks for, just under a leading slash — see the
    // header comment. No translation table to keep in sync.
    return "http://" + m_cfg.host + ":" + std::to_string(m_cfg.port) + "/" + key;
}

GetResult LanTransport::get(const std::string& key) {
    ensure_curl();
    GetResult res;
    CURL* curl = curl_easy_init();
    if (!curl) { res.error = "curl init"; return res; }

    const std::string url = url_for(key);
    struct curl_slist* headers = nullptr;
    if (!m_cfg.auth_token.empty())
        headers = curl_slist_append(headers,
            ("Authorization: Bearer " + m_cfg.auth_token).c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_vec);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &res.body);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)m_cfg.connect_timeout_ms);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)m_cfg.request_timeout_ms);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_abort_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &m_cancel);

    CURLcode cc = curl_easy_perform(curl);
    if (cc == CURLE_OK) {
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        res.http_status = code;
        res.success = (code >= 200 && code < 300);
        m_last_reached = true;
        if (!res.success) {
            res.retryable = !(code >= 400 && code < 500) || code == 408 || code == 429;
            res.error = "HTTP " + std::to_string(code);
        }
    } else {
        // Connection refused, timed out, host unreachable — the LAN path
        // itself is the problem, not this one key.
        m_last_reached = false;
        res.retryable = true;
        res.error = curl_easy_strerror(cc);
    }

    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return res;
}

void LanTransport::cancel_pending() { m_cancel = true; }

bool LanTransport::publish_cue(const std::string& author, const std::string& label,
                               std::string& merged_json, std::string& error) {
    ensure_curl();
    CURL* curl = curl_easy_init();
    if (!curl) { error = "curl init"; return false; }

    const std::string url = "http://" + m_cfg.host + ":" +
                            std::to_string(m_cfg.port) + "/api/cue";
    // Built with the JSON writer, not by hand: the label is whatever the
    // operator typed, quotes and all.
    const std::string payload =
        json({ {"author", author}, {"label", label} }).dump();

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (!m_cfg.auth_token.empty())
        headers = curl_slist_append(headers,
            ("Authorization: Bearer " + m_cfg.auth_token).c_str());

    std::vector<uint8_t> resp;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)payload.size());
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_vec);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)m_cfg.connect_timeout_ms);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)m_cfg.request_timeout_ms);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_abort_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &m_cancel);

    bool ok = false;
    CURLcode cc = curl_easy_perform(curl);
    if (cc == CURLE_OK) {
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        m_last_reached = true;
        if (code >= 200 && code < 300) {
            merged_json.assign(resp.begin(), resp.end());
            ok = true;
        } else {
            // The hub answers a refusal with a plain-text reason, which is
            // exactly what an operator should be shown.
            error = "HTTP " + std::to_string(code);
            const std::string detail(resp.begin(), resp.end());
            if (!detail.empty()) error += ": " + detail;
        }
    } else {
        m_last_reached = false;
        error = curl_easy_strerror(cc);
    }

    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ok;
}

} // namespace multisite
