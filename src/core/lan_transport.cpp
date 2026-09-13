// SPDX-License-Identifier: GPL-3.0-or-later
#include "lan_transport.h"

#include <curl/curl.h>

#include <mutex>

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

} // namespace multisite
