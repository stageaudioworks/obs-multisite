// SPDX-License-Identifier: GPL-3.0-or-later
// test_reliability.cpp — exercises the Phase 1 reliability core end to end.
//
// Covers:
//   1. Durable spool survives a simulated crash (new instance sees pending work)
//   2. Ordered draining through a network OUTAGE with retries — zero loss/reorder
//   3. Checksum verification detects corruption
//   4. Resume-after-crash detection + clean-end suppression
//   5. Manifest rolling window + round-trip with audio tracks
//
#include "../src/core/spool_queue.h"
#include "../src/core/retry_uploader.h"
#include "../src/core/model.h"
#include "../src/core/checksum.h"
#include "../src/core/session.h"   // for now_ms()

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <vector>
#include <map>

namespace fs = std::filesystem;
using namespace multisite;

static int g_failures = 0;
#define CHECK(cond, msg) do { if(!(cond)){ std::printf("  [FAIL] %s\n", msg); ++g_failures; } \
                              else { std::printf("  [ok]   %s\n", msg); } } while(0)

static std::vector<uint8_t> fake_segment(uint64_t seq, size_t n = 4096) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = (uint8_t)((seq * 131 + i * 7) & 0xFF);
    return v;
}

// A transport that fails the first `outage` PUTs (simulating a network drop),
// then succeeds. Records the order in which keys are confirmed.
class FlakyTransport : public Transport {
public:
    std::atomic<int> fail_budget{0};   // number of upcoming PUTs that will fail
    std::atomic<int> put_calls{0};
    std::vector<std::string> confirmed_keys;
    std::mutex mtx;
    bool permanent = false;

    PutResult put(const std::string& key, const std::vector<uint8_t>&,
                  const std::string&, const std::map<std::string,std::string>&) override {
        put_calls++;
        if (permanent) return {false, 403, false, "forbidden"};
        if (fail_budget.load() > 0) {
            fail_budget--;
            return {false, 0, true, "network down"};
        }
        std::lock_guard<std::mutex> lk(mtx);
        confirmed_keys.push_back(key);
        return {true, 200, true, ""};
    }
};

int main() {
    fs::path base = fs::temp_directory_path() / "multisite_test";
    fs::remove_all(base);
    fs::create_directories(base);

    std::printf("== 1. Durability across a simulated crash ==\n");
    {
        std::string dir = (base / "spool1").string();
        {
            SpoolQueue q(dir);
            q.begin_event("01EVENT", 1);
            for (uint64_t s = 1; s <= 5; ++s) {
                SpooledSegment seg;
                seg.seq = s; seg.data = fake_segment(s);
                seg.key = "events/01EVENT/segments/0000000" + std::to_string(s) + ".m4s";
                q.enqueue(std::move(seg));
            }
            CHECK(q.pending_count() == 5, "5 segments spooled to disk");
        } // <- q destroyed: simulates process exit / crash (nothing confirmed)

        // "Restart": brand-new instance reading the same directory.
        SpoolQueue q2(dir);
        CHECK(q2.pending_count() == 5, "after crash, all 5 segments still on disk");
        auto info = q2.inspect();
        CHECK(info.resumable && info.event_id == "01EVENT", "prior event detected as resumable");
        CHECK(info.pending_count == 5, "resume reports 5 pending");
    }

    std::printf("== 2. Ordered draining through a network outage ==\n");
    {
        std::string dir = (base / "spool2").string();
        SpoolQueue q(dir);
        q.begin_event("01EVENT", 1);
        for (uint64_t s = 1; s <= 8; ++s) {
            SpooledSegment seg; seg.seq = s; seg.data = fake_segment(s);
            seg.key = "seg/" + std::to_string(s);
            q.enqueue(std::move(seg));
        }
        FlakyTransport tx;
        tx.fail_budget = 12; // simulate an outage: first 12 PUT attempts fail

        UploaderConfig cfg;
        cfg.base_backoff_ms = 1; cfg.max_backoff_ms = 4; cfg.jitter = 0.0;
        RetryUploader up(q, tx, cfg);

        bool drained = up.drain_blocking(std::chrono::milliseconds(5000));
        CHECK(drained, "spool fully drained after the outage cleared");
        CHECK(q.pending_count() == 0, "no segments left pending (zero loss)");
        CHECK(up.stats().confirmed.load() == 8, "all 8 segments confirmed");
        CHECK(up.stats().retries.load() >= 12, "retries occurred during outage");

        // Confirmed strictly in ascending seq order (no reordering).
        bool ordered = true;
        for (size_t i = 0; i < tx.confirmed_keys.size(); ++i)
            if (tx.confirmed_keys[i] != "seg/" + std::to_string(i + 1)) ordered = false;
        CHECK(ordered && tx.confirmed_keys.size() == 8, "segments confirmed in strict order");
    }

    std::printf("== 3. Checksum verification ==\n");
    {
        auto data = fake_segment(42, 8192);
        std::string good = sha256_hex(data);
        CHECK(verify_sha256(data, good), "valid segment verifies");
        auto corrupt = data; corrupt[100] ^= 0xFF;
        CHECK(!verify_sha256(corrupt, good), "corrupted segment fails verification");
    }

    std::printf("== 4. Resume vs clean-end ==\n");
    {
        std::string dir = (base / "spool4").string();
        {
            SpoolQueue q(dir);
            q.begin_event("01EVENT", 1);
            SpooledSegment seg; seg.seq = 1; seg.data = fake_segment(1);
            seg.key = "seg/1"; q.enqueue(std::move(seg));
            q.mark_ended(); // clean shutdown
        }
        SpoolQueue q2(dir);
        CHECK(!q2.inspect().resumable, "cleanly-ended event is NOT offered for resume");
    }

    std::printf("== 5. Manifest rolling window + round-trip ==\n");
    {
        Manifest m;
        m.event_id = "01EVENT"; m.first_available_seq = 1;
        m.video = { "h264", 1920, 1080, 30.0 };
        m.audio_tracks = {
            { 0, "Main mix",  "aac", 2, 48000 },
            { 1, "Sermon ISO","aac", 1, 48000 },
            { 2, "Click",     "aac", 1, 48000 },
        };
        for (uint64_t s = 1; s <= 60; ++s) {
            ManifestSegment ms; ms.seq = s; ms.duration_s = 6.0;
            ms.checksum = sha256_hex(fake_segment(s));
            m.push(ms, 50); // window of 50
        }
        CHECK(m.segments.size() == 50, "window trimmed to 50 segments");
        CHECK(m.latest_seq == 60, "latest_seq tracks the live edge");
        CHECK(m.window_start_seq == 11, "window_start_seq advances (11..60)");

        Manifest r = Manifest::from_json(m.to_json());
        CHECK(r.audio_tracks.size() == 3 && r.audio_tracks[2].label == "Click",
              "audio tracks survive round-trip");
        CHECK(r.segments.size() == 50 && r.segments.back().seq == 60,
              "segments survive round-trip");
        CHECK(!r.segments.back().checksum.empty(), "per-segment checksums preserved");
    }

    std::printf("== 6. Permanent failure stops the drain (no infinite spin) ==\n");
    {
        std::string dir = (base / "spool6").string();
        SpoolQueue q(dir);
        q.begin_event("01EVENT", 1);
        SpooledSegment seg; seg.seq = 1; seg.data = fake_segment(1); seg.key = "seg/1";
        q.enqueue(std::move(seg));
        FlakyTransport tx; tx.permanent = true;
        RetryUploader up(q, tx, {});
        bool drained = up.drain_blocking(std::chrono::milliseconds(500));
        CHECK(!drained, "permanent (403) error does not silently drop the segment");
        CHECK(q.pending_count() == 1, "segment stays spooled for operator to fix creds");
        CHECK(up.stats().permanent_failures.load() == 1, "permanent failure surfaced");
    }

    std::printf("== 7. Disk cap drops the OLDEST unconfirmed segment ==\n");
    {
        std::string dir = (base / "spool7").string();
        // Cap small enough that a handful of 4 KiB segments overflow it.
        SpoolQueue q(dir, /*max_bytes=*/10 * 1024);
        q.begin_event("01EVENT", 1);

        std::vector<SpoolDrop> drops;
        q.set_drop_callback([&](const SpoolDrop& d) { drops.push_back(d); });

        for (uint64_t s = 1; s <= 6; ++s) {
            SpooledSegment seg;
            seg.seq = s; seg.data = fake_segment(s, 4096);
            seg.key = "seg/" + std::to_string(s);
            q.enqueue(std::move(seg));
        }
        CHECK(q.bytes_pending() <= 10 * 1024, "pending bytes stay under the cap");
        CHECK(!drops.empty(), "the cap actually evicted something");
        CHECK(drops.front().seq == 1, "the OLDEST segment (seq 1) was dropped first");
        // 10 KiB / 4 KiB rounds to 2 segments kept; the newest write is never
        // the one sacrificed.
        auto pend = q.pending_count();
        CHECK(pend >= 1 && pend < 6, "some segments evicted, not all of them");
        CHECK(q.state().first_seq == drops.back().seq + 1,
              "the floor advances to just past the last thing actually dropped");
        CHECK(q.dropped_count() == drops.size(), "drop counter matches callback firings");

        // The newest segment written is never the one evicted, even if it
        // alone would exceed the cap — dropping it would defeat the point
        // (losing exactly the segment that just arrived) and stop progress.
        SpooledSegment huge;
        huge.seq = 7; huge.data = fake_segment(7, 64 * 1024);
        huge.key = "seg/7";
        q.enqueue(std::move(huge));
        CHECK(q.pending_count() >= 1 && q.peek_next().has_value(),
              "a single oversized newest segment is kept, not dropped");
    }

    std::printf("== 8. Confirming a segment frees its bytes from the cap ==\n");
    {
        std::string dir = (base / "spool8").string();
        SpoolQueue q(dir, /*max_bytes=*/100 * 1024 * 1024); // generous
        q.begin_event("01EVENT", 1);
        SpooledSegment seg; seg.seq = 1; seg.data = fake_segment(1, 4096); seg.key = "seg/1";
        q.enqueue(std::move(seg));
        CHECK(q.bytes_pending() == 4096, "bytes_pending reflects the write");
        q.confirm(1);
        CHECK(q.bytes_pending() == 0, "confirming frees the byte count, not just the file");
    }

    std::printf("== 9. last_activity_ms tracks real activity, and survives a crash ==\n");
    {
        std::string dir = (base / "spool9").string();
        int64_t before = now_ms();
        {
            SpoolQueue q(dir);
            q.begin_event("01EVENT", 1);

            SpooledSegment seg; seg.seq = 1; seg.data = fake_segment(1); seg.key = "seg/1";
            q.enqueue(std::move(seg));
            int64_t after_enqueue = q.state().last_activity_ms;
            CHECK(after_enqueue >= before, "enqueue() stamps last_activity_ms");

            q.confirm(1);
            int64_t after_confirm = q.state().last_activity_ms;
            CHECK(after_confirm >= after_enqueue, "confirm() stamps it again");
        } // <- crash: never marked ended

        // "Restart": a brand-new instance must read the same timestamp back,
        // not reset it to 0 — that timestamp is the ONLY signal that tells a
        // genuine crash-and-restart apart from a stale leftover event (see
        // PROJECT-SCOPE.md §5.1). Losing it on every restart would make
        // every resumable event look infinitely stale.
        SpoolQueue q2(dir);
        auto info = q2.inspect();
        CHECK(info.resumable, "unfinished event still offered for resume");
        CHECK(info.last_activity_ms >= before,
              "last_activity_ms survived the crash instead of resetting to 0");
    }

    fs::remove_all(base);
    std::printf("\n%s\n", g_failures == 0
        ? "ALL RELIABILITY TESTS PASSED"
        : "SOME TESTS FAILED");
    return g_failures == 0 ? 0 : 1;
}
