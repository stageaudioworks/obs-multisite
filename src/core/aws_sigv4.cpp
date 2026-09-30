// SPDX-License-Identifier: GPL-3.0-or-later
#include "aws_sigv4.h"
#include "text.h"
#include "crypto.h"

#include <ctime>
#include <cstdio>
#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace multisite {

// ── Low-level crypto helpers (delegate to the platform backend) ──────────────


std::vector<uint8_t> SigV4Signer::hmac_sha256(const std::vector<uint8_t>& key,
                                              const std::string& msg) {
    return crypto::hmac_sha256(key, msg);
}

// ── Percent-encoding per RFC 3986 (AWS flavour) ──────────────────────────────
// AWS requires uppercase hex, and does NOT encode: A-Z a-z 0-9 - _ . ~
// The path segment keeps '/' unencoded (encode_slash=false); query values
// encode everything (encode_slash=true).
std::string SigV4Signer::uri_encode(const std::string& s, bool encode_slash) {
    static const char* hx = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        bool unreserved = (c >= 'A' && c <= 'Z') ||
                          (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') ||
                          c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) {
            out.push_back((char)c);
        } else if (c == '/' && !encode_slash) {
            out.push_back('/');
        } else {
            out.push_back('%');
            out.push_back(hx[c >> 4]);
            out.push_back(hx[c & 0xF]);
        }
    }
    return out;
}

// ── URL parsing (host + canonical path + query) ──────────────────────────────
namespace {
struct ParsedUrl {
    std::string host;
    std::string path;   // begins with '/'
    std::string query;  // without leading '?'
};

ParsedUrl parse_url(const std::string& url) {
    ParsedUrl p;
    auto scheme_pos = url.find("://");
    size_t host_start = (scheme_pos == std::string::npos) ? 0 : scheme_pos + 3;
    auto path_start = url.find('/', host_start);
    auto query_start = url.find('?', host_start);

    if (path_start == std::string::npos) {
        p.host = url.substr(host_start);
        p.path = "/";
        return p;
    }
    p.host = url.substr(host_start, path_start - host_start);

    if (query_start == std::string::npos) {
        p.path = url.substr(path_start);
    } else {
        p.path  = url.substr(path_start, query_start - path_start);
        p.query = url.substr(query_start + 1);
    }
    return p;
}



// Percent-decode, so canonicalisation is independent of how the caller chose to
// write the URL.
std::string percent_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    auto hexval = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const int hi = hexval(s[i + 1]), lo = hexval(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back((char)((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}
} // namespace

// Canonicalise a query string: split on &, DECODE then re-encode each key and
// value, then sort by encoded key (then value).
//
// The decode step matters. S3 canonicalises the query it receives after
// decoding it, so a request whose URL carries an already-encoded value (a
// listing prefix such as "rooms/main/events/", where the slashes must be
// %2F for the URL to be well-formed for arbitrary room names) would otherwise
// be signed over a double-encoded string and rejected as SignatureDoesNotMatch.
// Encoding without decoding first only happens to work while every value is
// already free of reserved characters.
std::string SigV4Signer::canonical_query(const std::string& query) {
    if (query.empty()) return "";
    std::vector<std::pair<std::string,std::string>> params;
    std::stringstream ss(query);
    std::string item;
    while (std::getline(ss, item, '&')) {
        if (item.empty()) continue;
        auto eq = item.find('=');
        std::string k = (eq == std::string::npos) ? item : item.substr(0, eq);
        std::string v = (eq == std::string::npos) ? "" : item.substr(eq + 1);
        params.emplace_back(uri_encode(percent_decode(k), true),
                            uri_encode(percent_decode(v), true));
    }
    std::sort(params.begin(), params.end());
    std::string out;
    for (size_t i = 0; i < params.size(); ++i) {
        if (i) out.push_back('&');
        out += params[i].first + "=" + params[i].second;
    }
    return out;
}

// ── Signing key derivation ───────────────────────────────────────────────────
std::vector<uint8_t> SigV4Signer::signing_key(const std::string& datestamp) const {
    std::string k0 = "AWS4" + m_secret_key;
    std::vector<uint8_t> kSecret(k0.begin(), k0.end());
    auto kDate    = hmac_sha256(kSecret, datestamp);
    auto kRegion  = hmac_sha256(kDate, m_region);
    auto kService = hmac_sha256(kRegion, m_service);
    auto kSigning = hmac_sha256(kService, "aws4_request");
    return kSigning;
}

// ── Constructor ──────────────────────────────────────────────────────────────
SigV4Signer::SigV4Signer(std::string access_key, std::string secret_key,
                         std::string region, std::string service)
    : m_access_key(std::move(access_key)),
      m_secret_key(std::move(secret_key)),
      m_region(std::move(region)),
      m_service(std::move(service)) {}

// ── The main event ───────────────────────────────────────────────────────────
SignedRequest SigV4Signer::sign(const std::string& method,
                                const std::string& url,
                                const std::vector<uint8_t>& payload,
                                const std::map<std::string,std::string>& extra_headers,
                                const std::string& amz_date_override,
                                const std::string& datestamp_override) const {
    ParsedUrl u = parse_url(url);

    // Timestamps
    std::string amz_date, datestamp;
    if (!amz_date_override.empty()) {
        amz_date  = amz_date_override;
        datestamp = datestamp_override;
    } else {
        std::time_t now = std::time(nullptr);
        std::tm gmt{};
#if defined(_WIN32)
        gmtime_s(&gmt, &now);
#else
        gmtime_r(&now, &gmt);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &gmt);
        amz_date = buf;
        datestamp = amz_date.substr(0, 8);
    }

    // Payload hash
    std::string payload_hash = payload.empty()
        ? crypto::sha256_hex(reinterpret_cast<const uint8_t*>(""), 0)
        : crypto::sha256_hex(payload.data(), payload.size());

    // Build the set of headers to sign. Always include host + the x-amz ones.
    std::map<std::string, std::string> signed_headers;
    signed_headers["host"] = u.host;
    signed_headers["x-amz-content-sha256"] = payload_hash;
    signed_headers["x-amz-date"] = amz_date;
    for (const auto& [k, v] : extra_headers) {
        signed_headers[lower(k)] = trim(v);
    }

    // Canonical headers block + signed header list
    std::string canonical_headers;
    std::string signed_header_list;
    bool first = true;
    for (const auto& [k, v] : signed_headers) { // std::map => sorted by key
        canonical_headers += k + ":" + trim(v) + "\n";
        if (!first) signed_header_list += ";";
        signed_header_list += k;
        first = false;
    }

    // Canonical request
    std::string canonical_uri = uri_encode(u.path, /*encode_slash=*/false);
    std::string canonical_qs  = canonical_query(u.query);

    std::string canonical_request =
        method + "\n" +
        canonical_uri + "\n" +
        canonical_qs + "\n" +
        canonical_headers + "\n" +
        signed_header_list + "\n" +
        payload_hash;

    // String to sign
    std::string scope = datestamp + "/" + m_region + "/" + m_service + "/aws4_request";
    std::string cr_hash = crypto::sha256_hex(
        reinterpret_cast<const uint8_t*>(canonical_request.data()),
        canonical_request.size());

    std::string string_to_sign =
        "AWS4-HMAC-SHA256\n" +
        amz_date + "\n" +
        scope + "\n" +
        cr_hash;

    // Signature
    auto kSigning = signing_key(datestamp);
    auto sig_bytes = hmac_sha256(kSigning, string_to_sign);
    static const char* hx = "0123456789abcdef";
    std::string signature;
    for (uint8_t b : sig_bytes) {
        signature.push_back(hx[b >> 4]);
        signature.push_back(hx[b & 0xF]);
    }

    // Authorization header
    std::string authorization =
        "AWS4-HMAC-SHA256 "
        "Credential=" + m_access_key + "/" + scope + ", "
        "SignedHeaders=" + signed_header_list + ", "
        "Signature=" + signature;

    SignedRequest result;
    result.headers = signed_headers;
    result.headers["Authorization"] = authorization;
    // Normalise the casing of the x-amz headers for the wire
    result.headers.erase("host"); // libcurl sets Host itself
    return result;
}

std::vector<std::string> SignedRequest::header_lines() const {
    std::vector<std::string> lines;
    for (const auto& [k, v] : headers) {
        lines.push_back(k + ": " + v);
    }
    return lines;
}

} // namespace multisite
