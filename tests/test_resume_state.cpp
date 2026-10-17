// SPDX-License-Identifier: GPL-3.0-or-later
// test_resume_state.cpp — the restart-restore decision, pure.
#include "../src/appliance/resume_state.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

using namespace multisite_player;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

static std::string tmpdir() {
    const char* t = std::getenv("TMPDIR");
    std::string d = t && *t ? t : "/tmp";
    if (d.back() != '/') d += '/';
    return d;
}

int main() {
    const std::string p = tmpdir() + "test_resume_state.json";
    std::remove(p.c_str());

    ResumeState s;
    s.event_id = "evt-1"; s.pinned = true; s.paused = true;
    s.media_ms = 123456789LL; s.saved_wall_ms = 1760000000000LL;
    std::string err;
    ResumeState got;
    CHECK(save_resume_state(p, s, err), "save succeeds");
    CHECK(load_resume_state(p, got), "load succeeds");
    CHECK(got.event_id == "evt-1" && got.pinned && got.paused &&
          got.media_ms == 123456789LL && got.saved_wall_ms == 1760000000000LL,
          "round trip keeps every field");

    std::remove(p.c_str());
    CHECK(!load_resume_state(p, got), "missing file is false");

    { std::ofstream(p) << "{\"event_id\":\"e1\",\"pin"; }
    CHECK(!load_resume_state(p, got), "truncated file is false");
    std::remove(p.c_str());

    // Invalid UTF-8 must not throw out of the keeper thread (terminate under
    // Restart=always).
    ResumeState bad = s;
    bad.event_id = std::string("evt-\xff\xfe");
    bool threw = false, ok = false;
    try { ok = save_resume_state(p, bad, err); } catch (...) { threw = true; }
    CHECK(!threw && ok, "invalid UTF-8 in event_id saves without throwing");
    std::remove(p.c_str());

    const long long now = 1760000000000LL;
    ResumeState f;
    f.saved_wall_ms = now - 29LL * 60 * 1000;
    CHECK(resume_is_fresh(f, now), "29 minutes old is fresh");
    f.saved_wall_ms = now - 31LL * 60 * 1000;
    CHECK(!resume_is_fresh(f, now), "31 minutes old is stale");
    f.saved_wall_ms = now + 60 * 1000;
    CHECK(!resume_is_fresh(f, now), "a future timestamp is not fresh");

    ResumeState w;
    CHECK(!resume_worth_restoring(w), "unpinned and playing: nothing to restore");
    w.pinned = true;
    CHECK(resume_worth_restoring(w), "pinned is worth restoring");
    w.pinned = false; w.paused = true;
    CHECK(resume_worth_restoring(w), "paused is worth restoring");

    CHECK(resume_state_path("/etc/multisite-player/config.json") ==
          "/etc/multisite-player/resume.json", "path is the config's sibling");

    if (g_fail) { std::printf("\n%d check(s) FAILED\n", g_fail); return 1; }
    std::printf("\nall checks passed\n");
    return 0;
}
