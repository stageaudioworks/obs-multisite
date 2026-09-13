// SPDX-License-Identifier: GPL-3.0-or-later
// test_retry_cancel.cpp — proves RetryUploader::stop() does not hang joining
// its upload thread when that thread is blocked inside a transport call.
//
// The decoder had exactly this hazard (see BUGS.md): tearing a source down
// while poll() was mid-request, with a 30-second timeout and nothing to
// cancel it, could block OBS's UI thread for however long that request had
// left — long enough that an operator force-quit it, which OBS then reported
// as a crash on the next launch. Fixed there via S3Transport::cancel_pending().
// RetryUploader::stop() has the same shape of join, through the same kind of
// blocking Transport::put() call, so it needed the same fix: cancel_pending()
// before m_thread.join().
//
// A real S3 endpoint can't be stalled on demand (see test_s3_cancel.cpp,
// which proves S3Transport's own cancel works, against a real stalled TCP
// connection). This is the layer above that: it proves RetryUploader actually
// CALLS cancel_pending() and that doing so is what unblocks stop() — using a
// plain mock transport that blocks until told to stop, so the test is
// portable and fast rather than POSIX-only.
#include "../src/core/retry_uploader.h"
#include "../src/core/spool_queue.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

namespace fs = std::filesystem;
using namespace multisite;
using Clock = std::chrono::steady_clock;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

// A transport whose put() never returns on its own — the same shape as a
// connection that went silent mid-request — until cancel_pending() is
// called, exactly like a real stalled libcurl request answering its progress
// callback. A 5-second safety net keeps a failing test from hanging the suite
// outright, but a passing test should never come close to it.
class StallingTransport : public Transport {
public:
    std::atomic<bool> cancelled{false};
    std::atomic<int>  put_calls{0};

    PutResult put(const std::string&, const std::vector<uint8_t>&,
                  const std::string&, const std::map<std::string,std::string>&) override {
        put_calls++;
        auto start = Clock::now();
        while (!cancelled.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (Clock::now() - start > std::chrono::seconds(5)) break;
        }
        return {false, 0, true, "cancelled"};
    }

    void cancel_pending() override { cancelled = true; }
};

int main() {
    fs::path dir = fs::temp_directory_path() / "multisite_retry_cancel_test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    std::printf("== stop() cancels an in-flight upload instead of hanging on it ==\n");
    {
        SpoolQueue q(dir.string());
        q.begin_event("01EVENT", 1);
        SpooledSegment seg;
        seg.seq = 1; seg.data = std::vector<uint8_t>(1024, 0x42); seg.key = "seg/1";
        q.enqueue(std::move(seg));

        StallingTransport tx;
        RetryUploader up(q, tx, {});
        up.start();

        // Wait for the upload thread to actually be inside put() — otherwise
        // stop() racing ahead of it would trivially "pass" for the wrong
        // reason (nothing to cancel yet).
        for (int i = 0; i < 200 && tx.put_calls.load() == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK(tx.put_calls.load() > 0, "the upload thread reached put() and is now stuck in it");

        auto t0 = Clock::now();
        up.stop();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0);

        CHECK(tx.cancelled.load(), "stop() called cancel_pending() on the transport");
        std::printf("     (stop() returned after %lld ms)\n", (long long)elapsed.count());
        CHECK(elapsed < std::chrono::milliseconds(1000),
              "stop() returned promptly rather than waiting out the stalled request");
    }

    fs::remove_all(dir);
    std::printf("\n%s\n", g_fail == 0
        ? "ALL RETRY-CANCEL TESTS PASSED"
        : "SOME TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
