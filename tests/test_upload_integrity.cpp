// SPDX-License-Identifier: GPL-3.0-or-later
// test_upload_integrity.cpp — what a failed PUT means, and what the uploader
// does about it (docs/scope/upload-delivery-assurance.md, R4).
//
//   1. Content-MD5: RFC 1321 vectors and base64, since a wrong digest would
//      make every upload fail with BadDigest.
//   2. A store refusing damaged bytes (400 BadDigest /
//      XAmzContentSHA256Mismatch) or a stalled body (400 RequestTimeout) is
//      retryable. It used to be "permanent — check the storage credentials".
//   3. A permanent refusal backs off. It used to be retried every 200 ms for
//      as long as it lasted, while the log said uploading had stopped.
//   4. ...and still recovers on its own once the cause is fixed.
//
#include "../src/core/md5.h"
#include "../src/core/put_failure.h"
#include "../src/core/retry_uploader.h"
#include "../src/core/spool_queue.h"
#include "test_tmpdir.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

namespace fs = std::filesystem;
using namespace multisite;

static int g_failures = 0;
#define CHECK(cond, msg) do { if(!(cond)){ std::printf("  [FAIL] %s\n", msg); ++g_failures; } \
                              else { std::printf("  [ok]   %s\n", msg); } } while(0)

static std::string hex(const std::vector<uint8_t>& v) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (uint8_t b : v) { s += d[b >> 4]; s += d[b & 15]; }
    return s;
}
static std::string md5_hex(const std::string& s) {
    return hex(md5((const uint8_t*)s.data(), s.size()));
}

class RefusingTransport : public Transport {
public:
    std::atomic<int>  put_calls{0};
    std::atomic<bool> refuse{true};
    PutResult put(const std::string&, const std::vector<uint8_t>&,
                  const std::string&, const std::map<std::string, std::string>&) override {
        put_calls++;
        if (refuse) return {false, 403, false, "HTTP 403 AccessDenied"};
        return {true, 200, true, ""};
    }
};

static void enqueue_one(SpoolQueue& q, uint64_t seq) {
    SpooledSegment s;
    s.seq = seq;
    s.data.assign(1024, (uint8_t)seq);
    s.key = "events/E/segments/" + std::to_string(seq) + ".m4s";
    q.enqueue(std::move(s));
}

int main() {
    std::printf("== 1. Content-MD5 ==\n");
    CHECK(md5_hex("") == "d41d8cd98f00b204e9800998ecf8427e", "md5 of empty");
    CHECK(md5_hex("abc") == "900150983cd24fb0d6963f7d28e17f72", "md5 of abc");
    CHECK(md5_hex("message digest") == "f96b697d7cb7938d525a2f31aaf161d0",
          "md5 of 'message digest'");
    CHECK(md5_hex("12345678901234567890123456789012345678901234567890123456789012345678901234567890")
          == "57edf4a22be3c955ac49da2e2107b67a", "md5 across two blocks (80 bytes)");
    {
        // 56 bytes: the length that forces padding into a second block.
        std::string s(56, 'a');
        CHECK(md5_hex(s) == "3b0c8ac703f828b04c6c197006d17218", "md5 at the 56-byte padding edge");
    }
    CHECK(base64_encode({}) == "", "base64 empty");
    CHECK(base64_encode({'f'}) == "Zg==", "base64 one byte");
    CHECK(base64_encode({'f','o'}) == "Zm8=", "base64 two bytes");
    CHECK(base64_encode({'f','o','o'}) == "Zm9v", "base64 three bytes");
    {
        std::vector<uint8_t> abc = {'a','b','c'};
        CHECK(content_md5(abc) == "kAFQmDzST7DWlj99KOF/cg==", "Content-MD5 header value for abc");
    }

    std::printf("== 2. What a failed PUT means ==\n");
    const std::string sha_mismatch =
        "<?xml version=\"1.0\"?><Error><Code>XAmzContentSHA256Mismatch</Code>"
        "<Message>The provided 'x-amz-content-sha256' header does not match what was computed.</Message></Error>";
    {
        auto f = classify_put_failure(400, sha_mismatch);
        CHECK(f.retryable && f.kind == PutFailureKind::TransitDamage,
              "400 XAmzContentSHA256Mismatch is damage in transit, retried");
        CHECK(f.code == "XAmzContentSHA256Mismatch", "error code read from the document");
    }
    CHECK(classify_put_failure(400, "<Error><Code>BadDigest</Code></Error>").retryable,
          "400 BadDigest (Content-MD5 mismatch) is retried");
    CHECK(classify_put_failure(400, "<Error><Code>RequestTimeout</Code></Error>").kind
          == PutFailureKind::Stalled, "400 RequestTimeout is a stalled body, retried");
    CHECK(classify_put_failure(400, "<Error><Code>IncompleteBody</Code></Error>").retryable,
          "400 IncompleteBody is retried");
    {
        auto f = classify_put_failure(403, "<Error><Code>RequestTimeTooSkewed</Code></Error>");
        CHECK(!f.retryable && f.kind == PutFailureKind::ClockSkew,
              "403 RequestTimeTooSkewed is permanent, and named as the clock");
    }
    CHECK(!classify_put_failure(403, "<Error><Code>AccessDenied</Code></Error>").retryable,
          "403 AccessDenied stays permanent");
    CHECK(!classify_put_failure(404, "<Error><Code>NoSuchBucket</Code></Error>").retryable,
          "404 NoSuchBucket stays permanent");
    CHECK(!classify_put_failure(400, "").retryable, "a bare 400 stays permanent");
    CHECK(classify_put_failure(0, "").retryable, "no response at all is transient");
    CHECK(classify_put_failure(503, "<Error><Code>SlowDown</Code></Error>").retryable, "503 is transient");
    CHECK(classify_put_failure(429, "").retryable, "429 is transient");
    CHECK(classify_put_failure(408, "").retryable, "408 is transient");

    fs::path base = unique_temp_dir("multisite_upload_integrity");
    fs::remove_all(base);

    std::printf("== 3. A permanent refusal backs off ==\n");
    {
        SpoolQueue q((base / "s3").string());
        q.begin_event("E", 0);
        enqueue_one(q, 0);
        RefusingTransport tx;
        UploaderConfig cfg;
        cfg.verify_first_n = 0;
        cfg.permanent_backoff_ms = 5000;
        RetryUploader up(q, tx, cfg);
        up.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        up.stop();
        std::printf("  (%d PUTs in 1 s against a store refusing with 403)\n", tx.put_calls.load());
        CHECK(tx.put_calls.load() <= 2,
              "a 403 is not retried every 200 ms (at most 2 PUTs in 1 s)");
        CHECK(up.health() == LinkHealth::Offline, "health reads offline while refused");
        CHECK(q.pending_count() == 1, "the refused segment is kept, not dropped");
    }

    std::printf("== 4. ...and recovers once the cause is fixed ==\n");
    {
        SpoolQueue q((base / "s4").string());
        q.begin_event("E", 0);
        enqueue_one(q, 0);
        enqueue_one(q, 1);
        RefusingTransport tx;
        UploaderConfig cfg;
        cfg.verify_first_n = 0;
        cfg.permanent_backoff_ms = 300;
        RetryUploader up(q, tx, cfg);
        up.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        tx.refuse = false;   // the operator fixed the key
        bool drained = false;
        for (int i = 0; i < 100 && !drained; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            drained = q.pending_count() == 0;
        }
        up.stop();
        CHECK(drained, "both segments upload once the store accepts again");
        CHECK(up.health() == LinkHealth::Healthy, "health back to healthy");
    }

    fs::remove_all(base);
    std::printf("\n%s (%d failure(s))\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
