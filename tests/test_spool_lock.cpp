// SPDX-License-Identifier: GPL-3.0-or-later
// test_spool_lock.cpp — obs-multisite#36: the spool held its lock across each
// segment's SHA-256, both atomic writes and state.json's fsync. Everything that
// took that lock waited the whole time: pending_count() and state(), which is
// what Session::status() calls, so an encoder's dock waited on the disk. One
// enqueue measured 336 ms on a ROCK 5B encoder.
//
// The first case parks an enqueue inside its write (set_enqueue_pause_for_testing)
// and proves the readers answer anyway. Against the old code every read blocks
// until the pause is released, which is what makes this a regression test and
// not just a description of the new code.
//
// Offline and filesystem-only: no transport, no network.
#include "../src/core/spool_queue.h"
#include "test_tmpdir.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <future>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

static std::vector<uint8_t> fake_segment(uint64_t seq, size_t bytes = 2048) {
    std::vector<uint8_t> v(bytes);
    for (size_t i = 0; i < v.size(); ++i) v[i] = (uint8_t)((seq * 31 + i) & 0xFF);
    return v;
}

static SpooledSegment segment(uint64_t seq, size_t bytes = 2048) {
    SpooledSegment s;
    s.seq = seq;
    s.data = fake_segment(seq, bytes);
    s.key = "events/E/segments/" + std::to_string(seq) + ".m4s";
    return s;
}

// Polls `flag` for up to `ms`. Used instead of a joinable future so a blocked
// reader is reported as a failure rather than hanging the test (the reader is
// joined after the pause is released, when it can finish).
static bool wait_for_flag(const std::atomic<bool>& flag, int ms) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(ms);
    while (!flag && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return flag;
}

int main() {
    const fs::path base = unique_temp_dir("multisite_spool_lock");
    fs::remove_all(base);

    std::printf("== 1. status reads answer while an enqueue is inside its write ==\n");
    {
        SpoolQueue q((base / "one").string());
        q.begin_event("E", 1);

        std::promise<void> entered, release;
        std::atomic<bool> paused{false};
        q.set_enqueue_pause_for_testing([&] {
            entered.set_value();
            release.get_future().wait();
        });

        std::thread writer([&] {
            q.enqueue(segment(1));
        });
        const bool got_to_pause =
            entered.get_future().wait_for(std::chrono::seconds(5)) ==
            std::future_status::ready;
        CHECK(got_to_pause, "enqueue reached the middle of its write");

        // These are exactly what Session::status() calls, twice per dock tick.
        std::atomic<bool> state_done{false}, count_done{false}, peek_done{false};
        size_t counted = 0;
        bool peek_found = true;
        const auto t0 = std::chrono::steady_clock::now();
        auto ms_since = [&] {
            return std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - t0).count();
        };
        std::thread st([&] { (void)q.state();               state_done = true; });
        std::thread pc([&] { counted = q.pending_count();   count_done = true; });
        std::thread pk([&] { peek_found = q.peek_next(0).has_value(); peek_done = true; });

        CHECK(wait_for_flag(state_done, 500),
              "state() answers while an enqueue is writing (it used to wait the whole write)");
        CHECK(wait_for_flag(count_done, 500),
              "pending_count() answers while an enqueue is writing");
        CHECK(wait_for_flag(peek_done, 500),
              "peek_next() answers while an enqueue is writing");
        // The crash rule the ticket says must survive: bytes land before the
        // sidecar, and a drain in that window must not take the orphan .seg.
        CHECK(peek_done && !peek_found,
              "a .seg whose .meta is not written yet is not offered to the drain");
        CHECK(count_done && counted == 1,
              "the segment's bytes are already on disk, counted as pending");
        if (state_done)
            std::printf("  [meas] state() answered %.3f ms into the parked write "
                        "(the wait was unbounded before obs-multisite#36)\n",
                        ms_since());

        release.set_value();
        writer.join();
        st.join(); pc.join(); pk.join();

        CHECK(q.pending_count() == 1, "the enqueue completed with one segment pending");
        CHECK(q.state().last_enqueued == 1, "and state.json's view advanced to it");
        const auto next = q.peek_next(0);
        CHECK(next && next->seq == 1, "and the completed pair is now offered");
        CHECK(next && next->checksum.size() == 64,
              "with the checksum computed for it");
    }

    std::printf("== 2. Concurrent enqueue and confirm lose nothing ==\n");
    {
        // Both paths write state.json. They are serialised by the spool's own
        // write lock, so a later snapshot can never land before an earlier one
        // and leave last_confirmed behind on disk.
        SpoolQueue q((base / "two").string());
        q.begin_event("E", 1);

        constexpr uint64_t kLast = 20;
        std::thread writer([&] {
            for (uint64_t s = 1; s <= kLast; ++s) q.enqueue(segment(s));
        });

        uint64_t confirmed = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (confirmed < kLast && std::chrono::steady_clock::now() < deadline) {
            auto next = q.peek_next(0);
            if (!next) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
            CHECK(next->data.size() == 2048, "a segment read while another is written is whole");
            q.confirm(next->seq);
            confirmed = next->seq;
        }
        writer.join();

        CHECK(confirmed == kLast, "every segment was read and confirmed as it arrived");
        CHECK(q.pending_count() == 0, "nothing is left on disk");
        CHECK(q.state().last_confirmed == kLast, "the in-memory position is complete");
        CHECK(q.last_confirmed_for(0) == kLast, "and matches what was confirmed");
    }

    std::printf("== 3. A reopened event sees the state the writers left ==\n");
    {
        SpoolQueue q((base / "three").string());
        q.begin_event("E3", 1);
        q.enqueue(segment(1));
        q.enqueue(segment(2));
        q.confirm(1);
        q.mark_ended(true);

        SpoolQueue reopened((base / "three").string(), 0, "E3");
        const auto st = reopened.state();
        CHECK(st.valid && st.ended, "the event is there and ended");
        CHECK(st.last_enqueued == 2, "last_enqueued survived the restart");
        CHECK(st.last_confirmed == 1, "last_confirmed survived it too");
        CHECK(reopened.pending_count() == 1, "and one segment is still owed");
        const auto next = reopened.peek_next(0);
        CHECK(next && next->seq == 2, "which is the one it had not confirmed");
    }

    fs::remove_all(base);
    std::printf(g_fail ? "\nFAILED %d\n" : "\nALL PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
