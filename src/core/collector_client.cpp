// SPDX-License-Identifier: GPL-3.0-or-later
#include "collector_client.h"

#include <curl/curl.h>

#include <mutex>

namespace multisite {
namespace {

// A monitor must not hold anything hostage: short connect, bounded total. Same
// bounds the two hosts already chose independently, now in one place.
constexpr long kConnectTimeoutMs = 5000;
constexpr long kRequestTimeoutMs = 15000;
// The only things read from a response are a small interval and a small
// credentials object, so a body bigger than this is a server misbehaving and is
// cut off rather than buffered.
constexpr size_t kMaxBodyBytes = 65536;

size_t append_capped(char* data, size_t size, size_t nmemb, void* user) {
    auto* body = static_cast<std::string*>(user);
    const size_t n = size * nmemb;
    const size_t room = n > kMaxBodyBytes ? 0
        : kMaxBodyBytes > body->size() ? kMaxBodyBytes - body->size() : 0;
    body->append(data, room < n ? room : n);
    return n;
}

// Header lines arrive one call each. Only the delay-seconds form of Retry-After
// is read; an HTTP-date form yields 0 and a 429 is then handled by backing off,
// which is the honest "told to slow down" without parsing dates.
size_t capture_retry_after(char* data, size_t size, size_t nmemb, void* user) {
    const size_t n = size * nmemb;
    static const char kPrefix[] = "retry-after:";
    if (n > sizeof(kPrefix) - 1) {
        bool match = true;
        for (size_t i = 0; i < sizeof(kPrefix) - 1; ++i) {
            const char a = data[i] >= 'A' && data[i] <= 'Z'
                ? static_cast<char>(data[i] + ('a' - 'A')) : data[i];
            if (a != kPrefix[i]) { match = false; break; }
        }
        if (match)
            *static_cast<int*>(user) = atoi(data + sizeof(kPrefix) - 1);
    }
    return n;
}

HttpResult perform(const std::string& url, const std::string& token,
                   const std::string& payload, bool is_post) {
    HttpResult r;
    CURL* curl = curl_easy_init();
    if (!curl) return r;

    struct curl_slist* headers = nullptr;
    if (is_post)
        headers = curl_slist_append(headers, "Content-Type: application/json");
    // No token, no header — see the header's note on pairing's first request.
    const std::string auth = "Authorization: Bearer " + token;
    if (!token.empty())
        headers = curl_slist_append(headers, auth.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    if (is_post) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)payload.size());
    }
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_capped);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, capture_retry_after);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &r.retry_after_s);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, kConnectTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, kRequestTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    const CURLcode cc = curl_easy_perform(curl);
    if (cc == CURLE_OK &&
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r.code) == CURLE_OK)
        r.reached = true;
    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return r;
}

} // namespace

std::string collector_url(const std::string& base, const char* path) {
    std::string u = base;
    while (!u.empty() && u.back() == '/') u.pop_back();
    return u + (path ? path : "");
}

HttpResult http_post_json(const std::string& url, const std::string& token,
                          const std::string& payload) {
    return perform(url, token, payload, true);
}

HttpResult http_get_json(const std::string& url, const std::string& token) {
    return perform(url, token, std::string(), false);
}

void collector_http_init() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

} // namespace multisite
