// SPDX-License-Identifier: GPL-3.0-or-later
// test_spool_budget.cpp — the encoder's spool is sized from its free disk, not
// a fixed 4 GiB (docs/scope/upload-delivery-assurance.md, E3 and R2).
//
//   1. The arithmetic: a roomy disk gives far more than 4 GiB; a small or full
//      one keeps its reserve; pending bytes count inside the cap.
//   2. "Time left if the link stays down".
//   3. SpoolQueue asks its provider at every enqueue and evicts to that cap.
//   4. Session uses the free disk by default: with room it drops nothing,
//      with little it drops the oldest, and status() reports cap and time left.
//   5. A fixed max_spool_bytes still wins when given.
//
#include "../src/core/session.h"
#include "../src/core/spool_budget.h"
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

static constexpr uint64_t GiB = 1024ull * 1024 * 1024;

// Control files succeed; segment PUTs fail while `down` — a link that is out.
class OutageStore : public Transport {
public:
    std::atomic<bool> down{false};
    PutResult put(const std::string& key, const std::vector<uint8_t>&,
                  const std::string&, const std::map<std::string, std::string>&) override {
        if (down && key.find("/segments/") != std::string::npos)
            return {false, 0, true, "network down"};
        return {true, 200, true, ""};
    }
    GetResult get(const std::string&) override {
        GetResult r; r.http_status = 404; r.retryable = false; return r;
    }
};

static std::vector<uint8_t> blob(uint64_t n, size_t sz = 2048) {
    std::vector<uint8_t> v(sz);
    for (size_t i = 0; i < sz; ++i) v[i] = (uint8_t)((n * 31 + i) & 0xFF);
    return v;
}

int main() {
    std::printf("== 1. The cap from the disk ==\n");
    {
        // A 1 TB drive with 500 GB free: reserve is 10 % = 100 GB.
        const uint64_t cap = spool_cap_from_disk(0, 500 * GiB, 1000 * GiB);
        CHECK(cap == 400 * GiB, "500 GB free on 1 TB: 400 GB for the spool");
        CHECK(cap > 4 * GiB, "far more than the old fixed 4 GiB");
    }
    CHECK(spool_cap_from_disk(0, 50 * GiB, 120 * GiB) == 30 * GiB,
          "a small drive keeps the 20 GiB floor: 50 free gives 30");
    CHECK(spool_cap_from_disk(2 * GiB, 48 * GiB, 120 * GiB) == 2 * GiB + 28 * GiB,
          "pending bytes are already on the disk and count inside the cap");
    CHECK(spool_cap_from_disk(0, 10 * GiB, 120 * GiB) == 1 * GiB,
          "below the reserve: the 1 GiB minimum");
    CHECK(spool_cap_from_disk(0, 1 * GiB, 120 * GiB) == GiB / 2,
          "nearly full: never more than half of what is free");

    std::printf("== 2. Time left if the link stays down ==\n");
    {
        const double bps = 8e6 / 8.0;   // 8 Mbit/s
        const double s = seconds_until_spool_drops(4 * GiB, 0, bps);
        CHECK(s > 4290 && s < 4300, "4 GiB at 8 Mbit/s is about 72 minutes (4295 s)");
    }
    CHECK(seconds_until_spool_drops(4 * GiB, 0, 0.0) < 0, "unknown until a bitrate is measured");
    CHECK(seconds_until_spool_drops(GiB, 2 * GiB, 1e6) == 0.0, "over the cap: none left");

    fs::path base = unique_temp_dir("multisite_spool_budget");
    fs::remove_all(base);

    std::printf("== 3. SpoolQueue evicts to its provider's cap ==\n");
    {
        SpoolQueue q((base / "q").string());
        q.begin_event("E", 0);
        std::atomic<uint64_t> cap{3 * 1000};
        std::atomic<int> asked{0};
        q.set_cap_provider([&](uint64_t) { asked++; return cap.load(); });
        uint64_t dropped = 0;
        q.set_drop_callback([&](const SpoolDrop&) { dropped++; });
        for (uint64_t s = 0; s < 6; ++s) {
            SpooledSegment seg; seg.seq = s; seg.data.assign(1000, (uint8_t)s);
            seg.key = "k" + std::to_string(s);
            q.enqueue(std::move(seg));
        }
        CHECK(asked.load() == 6, "asked once per enqueue");
        CHECK(dropped == 3 && q.pending_count() == 3, "evicted down to three segments");
        CHECK(q.current_cap() == 3000, "reports the cap it worked to");
        cap = 0;   // "no cap"
        for (uint64_t s = 6; s < 10; ++s) {
            SpooledSegment seg; seg.seq = s; seg.data.assign(1000, (uint8_t)s);
            seg.key = "k" + std::to_string(s);
            q.enqueue(std::move(seg));
        }
        CHECK(dropped == 3, "a cap of 0 drops nothing");
    }

    VideoInfo video{ "h264", 1920, 1080, 30.0 };
    std::vector<AudioTrack> tracks = { { 0, "Main mix", "aac", 2, 48000 } };

    std::printf("== 4. Session sizes the spool from the disk by default ==\n");
    {
        OutageStore store;
        SessionConfig cfg;
        CHECK(cfg.max_spool_bytes == 0, "the default is sized from the disk, not fixed");
        cfg.spool_dir = (base / "roomy").string();
        cfg.base_backoff_ms = 1; cfg.max_backoff_ms = 2; cfg.backoff_jitter = 0.0;
        cfg.disk_space = [](uint64_t& f, uint64_t& c) { f = 500 * GiB; c = 1000 * GiB; return true; };
        Session ses(cfg, store);
        CHECK(ses.start_new(blob(0), video, tracks), "start_new");
        store.down = true;
        for (uint64_t i = 0; i < 8; ++i) ses.publish_segment(blob(i + 1), 6.0, i * 6.0);
        auto st = ses.status();
        CHECK(st.dropped_for_disk == 0, "a roomy disk drops nothing");
        CHECK(st.spool_cap_bytes > 4 * GiB, "and reports a cap far above 4 GiB");
        CHECK(st.spool_seconds_left > 0, "and a time left once a bitrate is known");
        std::printf("      cap %.0f GB, %.0f h left at this test's bitrate\n",
                    st.spool_cap_bytes / (double)GiB, st.spool_seconds_left / 3600.0);
        store.down = false;
        for (int i = 0; i < 200 && ses.status().pending > 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        ses.end();
    }
    {
        OutageStore store;
        SessionConfig cfg;
        cfg.spool_dir = (base / "tight").string();
        cfg.base_backoff_ms = 1; cfg.max_backoff_ms = 2; cfg.backoff_jitter = 0.0;
        // Nearly full: room for six segments in all, and like a real disk the
        // free space shrinks by whatever the spool writes.
        const std::string dir = cfg.spool_dir;
        cfg.disk_space = [dir](uint64_t& f, uint64_t& c) {
            uint64_t used = 0;
            std::error_code ec;
            for (auto& e : fs::recursive_directory_iterator(dir, ec))
                if (e.is_regular_file(ec) && e.path().extension() == ".seg")
                    used += e.file_size(ec);
            const uint64_t room = 6 * 2048;
            f = used < room ? room - used : 0;
            c = 100 * GiB;
            return true;
        };
        Session ses(cfg, store);
        CHECK(ses.start_new(blob(0), video, tracks), "start_new");
        store.down = true;
        for (uint64_t i = 0; i < 8; ++i) ses.publish_segment(blob(i + 1), 6.0, i * 6.0);
        auto st = ses.status();
        CHECK(st.dropped_for_disk > 0, "a full disk drops the oldest instead of filling up");
        CHECK(st.spool_seconds_left == 0.0 || st.spool_seconds_left < 30.0,
              "and says there is almost no time left");
        store.down = false;
        ses.end();
    }

    std::printf("== 5. A fixed max_spool_bytes still wins ==\n");
    {
        OutageStore store;
        SessionConfig cfg;
        cfg.spool_dir = (base / "fixed").string();
        cfg.max_spool_bytes = 3 * 2048;
        cfg.base_backoff_ms = 1; cfg.max_backoff_ms = 2; cfg.backoff_jitter = 0.0;
        bool measured = false;
        cfg.disk_space = [&](uint64_t& f, uint64_t& c) { measured = true; f = 500 * GiB; c = 1000 * GiB; return true; };
        Session ses(cfg, store);
        CHECK(ses.start_new(blob(0), video, tracks), "start_new");
        store.down = true;
        for (uint64_t i = 0; i < 8; ++i) ses.publish_segment(blob(i + 1), 6.0, i * 6.0);
        CHECK(ses.status().dropped_for_disk > 0, "drops at the fixed cap");
        CHECK(!measured, "and never measured the disk");
        CHECK(ses.status().spool_cap_bytes == 3 * 2048, "reports the fixed cap");
        store.down = false;
        ses.end();
    }

    fs::remove_all(base);
    std::printf("\n%s (%d failure(s))\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
