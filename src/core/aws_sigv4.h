// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// aws_sigv4.h — minimal AWS Signature Version 4 signer for S3-compatible APIs.
//
// Depends only on OpenSSL (libcrypto) for SHA-256 / HMAC-SHA256.
// No aws-sdk-cpp. Designed for signing single S3 requests (PUT/GET/DELETE)
// against Cloudflare R2, which speaks the S3 REST API.
//
// Usage:
//   SigV4Signer signer{access_key, secret_key, "auto", "s3"};
//   auto headers = signer.sign("PUT", url, payload_bytes, extra_headers);
//   // -> feed headers into libcurl
//
#include <string>
#include <vector>
#include <map>
#include <cstdint>

namespace multisite {

struct SignedRequest {
    // Fully-formed HTTP headers (name: value) ready to hand to libcurl.
    std::map<std::string, std::string> headers;
    // Convenience: the same headers as "Name: Value" strings.
    std::vector<std::string> header_lines() const;
};

class SigV4Signer {
public:
    SigV4Signer(std::string access_key,
                std::string secret_key,
                std::string region = "auto",
                std::string service = "s3");

    // Sign a request. `url` must be a full https URL. `payload` is the raw
    // request body (may be empty for GET/DELETE). `extra_headers` are any
    // additional headers you want signed (e.g. Content-Type).
    //
    // `amz_date_override` / `datestamp_override` exist only for testing so a
    // fixed timestamp can be injected; leave empty in production.
    SignedRequest sign(const std::string& method,
                       const std::string& url,
                       const std::vector<uint8_t>& payload,
                       const std::map<std::string, std::string>& extra_headers = {},
                       const std::string& amz_date_override = "",
                       const std::string& datestamp_override = "") const;

    // Exposed for unit testing — normally internal.
    static std::string uri_encode(const std::string& s, bool encode_slash);
    // Canonicalise a query string the way S3 does: decode what the URL carries,
    // re-encode it to the AWS rules, sort by encoded key. Listing requests are
    // the only signed requests here that have a query at all.
    static std::string canonical_query(const std::string& query);

private:
    std::string m_access_key;
    std::string m_secret_key;
    std::string m_region;
    std::string m_service;

    static std::vector<uint8_t> hmac_sha256(const std::vector<uint8_t>& key,
                                            const std::string& msg);
    std::vector<uint8_t> signing_key(const std::string& datestamp) const;
};

} // namespace multisite
