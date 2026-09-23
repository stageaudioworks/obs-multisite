// SPDX-License-Identifier: GPL-3.0-or-later
// test_spool_targets.cpp — per-target confirmation in the spool, which is what
// lets the encoder write to two buckets independently (PROJECT-SCOPE.md §10
// Phase 9, "everything to both").
//
// Offline and filesystem-only: no transport, no network.
#include "../src/core/spool_queue.h"
#include "test_tmpdir.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

static std::vector<uint8_t> fake_segment(uint64_t seq) {
    std::vector<uint8_t> v(2048);
    for (size_t i = 0; i < v.size(); ++i) v[i] = (uint8_t)((seq * 31 + i) & 0xFF);
    return v;
}

static void fill(SpoolQueue& q, uint64_t from, uint64_t to) {
    for (uint64_t s = from; s <= to; ++s) {
        SpooledSegment seg;
        seg.seq = s;
        seg.data = fake_segment(s);
        seg.key = "events/E/segments/" + std::to_string(s) + ".m4s";
        q.enqueue(std::move(seg));
    }
}

int main() {
    const fs::path base = unique_temp_dir("multisite_spool_targets");
    fs::remove_all(base);

    std::printf("== 1. One target is exactly the old behaviour ==\n");
    {
        SpoolQueue q((base / "one").string());
        q.begin_event("E", 1);
        fill(q, 1, 3);
        CHECK(q.targets() == 1, "a spool starts with one target");
        CHECK(q.pending_count() == 3, "3 segments on disk");

        q.confirm(1);
        CHECK(q.pending_count() == 2, "confirming the only target removes the file");
        CHECK(q.last_confirmed_for(0) == 1, "and advances its position");
    }

    std::printf("== 2. Two targets: files stay until BOTH have the segment ==\n");
    {
        SpoolQueue q((base / "two").string());
        q.set_targets(2);
        q.begin_event("E", 1);
        fill(q, 1, 3);

        q.confirm(1, 0);
        CHECK(q.pending_count() == 3,
              "the primary confirming is not enough to remove the files");
        CHECK(q.last_confirmed_for(0) == 1, "the primary's position moved");
        CHECK(q.last_confirmed_for(1) == 0,
              "and the second target's did not — this is the lag");

        q.confirm(1, 1);
        CHECK(q.pending_count() == 2, "the second confirming removes them");
        CHECK(q.last_confirmed_for(1) == 1, "the second's position catches up");
    }

    std::printf("== 3. The two positions are independent ==\n");
    {
        SpoolQueue q((base / "indep").string());
        q.set_targets(2);
        q.begin_event("E", 1);
        fill(q, 1, 4);

        // The shape this exists for: the primary is working, the second is not.
        q.confirm(1, 0); q.confirm(2, 0); q.confirm(3, 0); q.confirm(4, 0);
        CHECK(q.last_confirmed_for(0) == 4, "the primary is fully caught up");
        CHECK(q.last_confirmed_for(1) == 0, "the second has confirmed nothing");
        CHECK(q.pending_count() == 4,
              "so every segment is still on disk, waiting for it");

        // …and now the second catches up, which is the outage ending.
        for (uint64_t s = 1; s <= 4; ++s) q.confirm(s, 1);
        CHECK(q.pending_count() == 0, "when it catches up the spool empties");
    }

    std::printf("== 4. A later segment does not confirm an earlier one ==\n");
    {
        SpoolQueue q((base / "gap").string());
        q.set_targets(2);
        q.begin_event("E", 1);
        fill(q, 1, 3);
        // Out of order, which is how a retrying uploader actually confirms.
        q.confirm(3, 0);
        q.confirm(3, 1);
        CHECK(q.pending_count() == 2,
              "confirming 3 leaves 1 and 2 — a position is not a sweep");
    }

    std::printf("== 5. Dropping back to one target clears what it already has ==\n");
    {
        SpoolQueue q((base / "downgrade").string());
        q.set_targets(2);
        q.begin_event("E", 1);
        fill(q, 1, 3);
        q.confirm(1, 0);
        q.confirm(2, 0);
        CHECK(q.pending_count() == 3, "2 segments are waiting on the second target");

        q.set_targets(1);   // the second bucket switched off mid-event
        CHECK(q.targets() == 1, "back to one target");
        CHECK(q.pending_count() == 1,
              "and the two the remaining target confirmed are removed at once");
    }

    std::printf("== 6. Both survive a restart ==\n");
    {
        {
            SpoolQueue q((base / "persist").string());
            q.set_targets(2);
            q.begin_event("E", 1);
            fill(q, 1, 3);
            q.confirm(2, 0);   // primary ahead, second behind
        }
        SpoolQueue q2((base / "persist").string());
        CHECK(q2.targets() == 2, "the second target is still configured");
        CHECK(q2.last_confirmed_for(0) == 2, "the primary's position survived");
        CHECK(q2.last_confirmed_for(1) == 0, "so did the second's, still behind");
        CHECK(q2.pending_count() == 3,
              "and nothing was removed that only one target had");
    }

    std::printf("\n%s\n", g_fail == 0 ? "SPOOL TARGET TESTS PASSED"
                                      : "SPOOL TARGET TESTS FAILED");
    fs::remove_all(base);
    return g_fail == 0 ? 0 : 1;
}
