// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// put_failure.h — what a failed PUT means, from its status and the store's
// error document.
//
// The status alone was the old rule: every 4xx but 408 and 429 was permanent.
// That put two quite different failures under "check the storage credentials":
//
//   • The store refusing the bytes as damaged in transit. S3 answers 400
//     XAmzContentSHA256Mismatch when the body does not match the signed hash,
//     and 400 BadDigest when it does not match Content-MD5. That is the
//     integrity check doing its job inside the request (docs/adr/0003 §1), and
//     the right answer is to send the segment again, as for any failed upload.
//   • 400 RequestTimeout / IncompleteBody: the connection stalled mid-body.
//     A slow link, not a misconfiguration.
//
// Both are retryable. RequestTimeTooSkewed (403) is not — it will fail until
// the encoder's clock is fixed — but it gets its own words, because "check the
// credentials" sends the operator to the wrong screen.
//
// Kept apart from s3_transport.cpp so it is tested with no network and no curl.
//
#include <string>

namespace multisite {

enum class PutFailureKind {
    Transient,      // network, 5xx, 408, 429: the outage case
    TransitDamage,  // the store found the body did not match its hash
    Stalled,        // the request body stalled before the store had it all
    ClockSkew,      // the encoder's clock is too far out for SigV4
    Refused,        // any other 4xx: credentials, bucket, policy
};

struct PutFailure {
    PutFailureKind kind = PutFailureKind::Transient;
    bool           retryable = true;
    std::string    code;     // the store's <Code>, when it sent one
};

// The <Code> element of an S3 error document, or "" when there is none.
inline std::string s3_error_code(const std::string& body) {
    const std::string open = "<Code>", close = "</Code>";
    const size_t a = body.find(open);
    if (a == std::string::npos) return {};
    const size_t from = a + open.size();
    const size_t b = body.find(close, from);
    if (b == std::string::npos) return {};
    return body.substr(from, b - from);
}

// `http_status` 0 means no response at all (DNS, refused, reset): transient.
inline PutFailure classify_put_failure(long http_status, const std::string& body) {
    PutFailure f;
    f.code = s3_error_code(body);
    if (http_status == 0 || http_status >= 500 ||
        http_status == 408 || http_status == 429) {
        return f;  // Transient, retryable
    }
    if (f.code == "XAmzContentSHA256Mismatch" || f.code == "BadDigest") {
        f.kind = PutFailureKind::TransitDamage;
        return f;
    }
    if (f.code == "RequestTimeout" || f.code == "IncompleteBody") {
        f.kind = PutFailureKind::Stalled;
        return f;
    }
    if (http_status >= 400 && http_status < 500) {
        f.retryable = false;
        f.kind = f.code == "RequestTimeTooSkewed" ? PutFailureKind::ClockSkew
                                                  : PutFailureKind::Refused;
        return f;
    }
    return f;  // 1xx/3xx where a 2xx was wanted: odd, but not permanent
}

// One plain sentence for the log, in the operator's terms.
inline const char* put_failure_advice(PutFailureKind k) {
    switch (k) {
    case PutFailureKind::TransitDamage:
        return "the store refused the bytes as damaged in transit; sending again";
    case PutFailureKind::Stalled:
        return "the upload stalled before the store had all of it; sending again";
    case PutFailureKind::ClockSkew:
        return "this computer's clock is too far out for the store to accept "
               "the request; set the clock automatically";
    case PutFailureKind::Refused:
        return "check the storage credentials and bucket name";
    case PutFailureKind::Transient:
    default:
        return "the link failed; retrying";
    }
}

} // namespace multisite
