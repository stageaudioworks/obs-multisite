// SPDX-License-Identifier: GPL-3.0-or-later
// test_mirror_upload.cpp — two upload streams over one spool: the primary and
// the second bucket (PROJECT-SCOPE.md §10 Phase 9).
//
// The property under test is the yield rule, and the distinction it turns on:
// the mirror waits for a primary that is WORKING, never for one that is GONE.
// "Wait for the primary to be caught up" sounds right and starves the mirror
// exactly when the second copy is the only thing that matters, because a failed
// primary is never caught up. So the policy is a predicate the caller supplies
// and these tests supply both halves of it.
//
// Offline: the transports are in-process fakes.
#include "../src/core/retry_uploader.h"
#include "../src/core/spool_queue.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

static std::vector<uint8_t> fake_segment(uint64_t seq) {
    std::vector<uint8_t> v(2048);
    for (size_t i = 0; i < v.size(); ++i) v[i] = (uint8_t)((seq * 17 + i) & 0xFF);
    return v;
}

// A transport that can be shut off and turned back on, counting what it takes.
class GateTransport : public Transport {
public:
    std::atomic<bool> allow{true};
    std::atomic<int>  puts{0};

    PutResult put(const std::string&, const std::vector<uint8_t>&,
                  const std::string&,
                  const std::map<std::string, std::string>&) override {
        if (!allow.load()) return {false, 0, true, "blocked"};
        ++puts;
        return {true, 200, true, ""};
    }
};

static void fill(SpoolQueue& q, uint64_t from, uint64_t to) {
    for (uint64_t s = from; s <= to; ++s) {
        SpooledSegment seg;
        seg.seq = s;
        seg.data = fake_segment(s);
        seg.key = "events/E/segments/" + std::to_string(s) + ".m4s";
        q.enqueue(std::move(seg));
    }
}

template <typename F>
static bool wait_for(F pred, int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

int main() {
    const fs::path base = fs::temp_directory_path() / "multisite_mirror_upload";
    fs::remove_all(base);

    UploaderConfig fast;
    fast.base_backoff_ms = 1;
    fast.max_backoff_ms = 4;
    fast.jitter = 0.0;

    std::printf("== 1. Both targets drain when both are working ==\n");
    {
        SpoolQueue q((base / "both").string());
        q.set_targets(2);
        q.begin_event("E", 1);
        fill(q, 1, 5);

        GateTransport primary, second;
        // The real policy: wait while the primary is healthy and behind.
        std::atomic<bool> primary_healthy{true};
        UploaderConfig mc = fast;
        mc.target = 1;
        mc.may_upload = [&] { return !primary_healthy.load() || q.caught_up(0); };

        RetryUploader up(q, primary, fast);
        RetryUploader mir(q, second, mc);
        up.start(); mir.start();

        const bool drained = wait_for([&] { return q.pending_count() == 0; }, 4000);
        up.stop(); mir.stop();

        CHECK(drained, "the spool empties with two streams running");
        CHECK(q.last_confirmed_for(0) == 5, "the primary confirmed all five");
        CHECK(q.last_confirmed_for(1) == 5, "so did the second target");
        CHECK(primary.puts.load() == 5 && second.puts.load() == 5,
              "each segment was uploaded twice — once per target");
    }

    std::printf("== 2. The mirror waits for a BUSY primary… ==\n");
    {
        SpoolQueue q((base / "busy").string());
        q.set_targets(2);
        q.begin_event("E", 1);
        fill(q, 1, 5);

        GateTransport primary, second;
        primary.allow = false;                 // the primary is stuck
        std::atomic<bool> primary_healthy{true}; // …but still working at it

        UploaderConfig mc = fast;
        mc.target = 1;
        mc.may_upload = [&] { return !primary_healthy.load() || q.caught_up(0); };

        RetryUploader up(q, primary, fast);
        RetryUploader mir(q, second, mc);
        up.start(); mir.start();

        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        CHECK(second.puts.load() == 0,
              "the mirror uploaded nothing while the primary was still trying");
        CHECK(q.last_confirmed_for(1) == 0, "and its position has not moved");
        up.stop(); mir.stop();
    }

    std::printf("== 3. …but takes over once the primary is GONE ==\n");
    {
        SpoolQueue q((base / "gone").string());
        q.set_targets(2);
        q.begin_event("E", 1);
        fill(q, 1, 5);

        GateTransport primary, second;
        primary.allow = false;                  // the primary never comes back
        std::atomic<bool> primary_healthy{true};

        UploaderConfig mc = fast;
        mc.target = 1;
        mc.may_upload = [&] { return !primary_healthy.load() || q.caught_up(0); };

        RetryUploader up(q, primary, fast);
        RetryUploader mir(q, second, mc);
        up.start(); mir.start();

        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(second.puts.load() == 0, "still yielding while it looks healthy");

        // The sustained-window verdict lands, as Session would decide it.
        primary_healthy = false;

        const bool done = wait_for([&] { return q.last_confirmed_for(1) == 5; }, 4000);
        up.stop(); mir.stop();

        CHECK(done, "the survivor received the whole event once the primary was "
                    "declared gone — decision 4, which 'wait for the primary to "
                    "catch up' would have starved");
        CHECK(q.last_confirmed_for(0) == 0, "the failed target confirmed nothing");
        CHECK(q.pending_count() == 5,
              "and every file stays on disk, because only one target has it");
    }

    std::printf("== 4. One target configured: nothing to mirror ==\n");
    {
        SpoolQueue q((base / "single").string());
        q.begin_event("E", 1);
        fill(q, 1, 3);

        GateTransport primary, second;
        UploaderConfig mc = fast;
        mc.target = 1;
        mc.may_upload = [&] { return q.caught_up(0); };

        RetryUploader up(q, primary, fast);
        RetryUploader mir(q, second, mc);
        up.start(); mir.start();

        const bool drained = wait_for([&] { return q.pending_count() == 0; }, 3000);
        up.stop(); mir.stop();

        CHECK(drained, "the spool drains on the single configured target");
        CHECK(second.puts.load() == 0,
              "the mirror never uploaded — a one-target spool had nothing left");
    }

    std::printf("\n%s\n", g_fail == 0 ? "MIRROR UPLOAD TESTS PASSED"
                                      : "MIRROR UPLOAD TESTS FAILED");
    fs::remove_all(base);
    return g_fail == 0 ? 0 : 1;
}
