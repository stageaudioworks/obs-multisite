// SPDX-License-Identifier: GPL-3.0-or-later
// test_segment_cache.cpp — the satellite's local DVR store, and the orphan
// sweep that keeps a crash from leaking disk forever.
//
// A clean event switch has always removed the directory being switched away
// from. What it never covered: a crash, force-kill or power loss mid-event
// never gets to switch away from anything, so that event's whole directory
// used to sit there forever — invisible to prune_below() and the
// segment-count ceiling, which only ever look inside the CURRENT event's own
// folder (see BUGS.md). These tests prove the sweep that closes that gap,
// and that it does not touch the directory actually being switched to.
#include "../src/core/segment_cache.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

static void touch(const fs::path& p) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << "x";
}

int main() {
    fs::path base = fs::temp_directory_path() / "multisite_segment_cache_test";
    fs::remove_all(base);
    fs::create_directories(base);

    std::printf("== 1. A clean switch still removes the directory it left ==\n");
    {
        fs::path dir = base / "t1";
        SegmentCache c(dir.string(), "EVENT_A");
        touch(dir / "EVENT_A" / "00000000.m4s");
        c.set_event("EVENT_B");
        CHECK(!fs::exists(dir / "EVENT_A"), "the old event's directory is gone");
        CHECK(fs::exists(dir / "EVENT_B"), "the new event has its own directory");
    }

    std::printf("== 2. Orphans left by a crash (not just the most recent) are swept ==\n");
    {
        fs::path dir = base / "t2";
        // Simulate what a few crashes leave behind: directories nobody ever
        // switched away from, sitting beside the placeholder a fresh
        // DecoderSession always starts with.
        touch(dir / "CRASHED_ONE" / "00000000.m4s");
        touch(dir / "CRASHED_TWO" / "00000003.m4s");
        touch(dir / "pending" / "00000001.m4s");

        // A fresh process, discovering the room's real live event for the
        // first time — the same "pending" -> real id transition every
        // DecoderSession makes at startup.
        SegmentCache c(dir.string(), "pending");
        c.set_event("EVENT_LIVE");

        CHECK(!fs::exists(dir / "CRASHED_ONE"), "an orphan from two crashes ago is gone");
        CHECK(!fs::exists(dir / "CRASHED_TWO"), "an orphan from the last crash is gone");
        CHECK(!fs::exists(dir / "pending"), "the placeholder directory is gone too");
        CHECK(fs::exists(dir / "EVENT_LIVE"), "the event actually being followed exists");
    }

    std::printf("== 3. Resuming into an event already on disk keeps its content ==\n");
    {
        fs::path dir = base / "t3";
        // The box crashed WHILE this exact event was live, and on restart the
        // room is still following the same one — the segments already banked
        // before the crash are exactly what timeslipping depends on; losing
        // them on every restart would defeat the point of having them.
        touch(dir / "SAME_EVENT" / "00000000.m4s");
        touch(dir / "SAME_EVENT" / "00000001.m4s");
        touch(dir / "OTHER_ORPHAN" / "00000000.m4s");

        SegmentCache c(dir.string(), "pending");
        c.set_event("SAME_EVENT");

        CHECK(fs::exists(dir / "SAME_EVENT" / "00000000.m4s"),
              "segment 0 survives switching into the same event");
        CHECK(fs::exists(dir / "SAME_EVENT" / "00000001.m4s"),
              "segment 1 survives too");
        CHECK(!fs::exists(dir / "OTHER_ORPHAN"), "an unrelated orphan is still cleaned up");
        CHECK(c.has(0) && c.has(1),
              "the rebuilt index sees the segments that were already on disk");
    }

    std::printf("== 4. Switching to the event already current is a no-op ==\n");
    {
        fs::path dir = base / "t4";
        touch(dir / "EVENT_X" / "00000000.m4s");   // on disk before the index is built
        SegmentCache c(dir.string(), "EVENT_X");
        CHECK(c.has(0), "constructor's index picks up what was already on disk");
        c.set_event("EVENT_X");   // same id — must not clear anything
        CHECK(c.has(0), "calling set_event with the current id does not wipe it");
    }

    fs::remove_all(base);
    std::printf("\n%s\n", g_fail == 0
        ? "ALL SEGMENT-CACHE TESTS PASSED"
        : "SOME TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
