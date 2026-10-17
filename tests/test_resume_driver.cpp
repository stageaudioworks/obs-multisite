// SPDX-License-Identifier: GPL-3.0-or-later
// test_resume_driver.cpp — the restore order, pure.
#include "../src/appliance/resume_driver.h"

#include <cstdio>
#include <vector>

using namespace multisite_player;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

static Status st(const char* ev, const char* pinned, bool loading, long long playhead = 0) {
    Status s;
    s.event_id = ev; s.pinned_event_id = pinned; s.loading = loading;
    s.playhead_media_ms = playhead;
    return s;
}
static ResumeState saved(bool pinned, bool paused) {
    ResumeState r;
    r.event_id = "evt"; r.pinned = pinned; r.paused = paused; r.media_ms = 600000;
    return r;
}
static const long long T0 = 1000000;

int main() {
    {
        auto sv = saved(true, true);
        ResumeProgress p;
        std::vector<ResumeStep> got = {
            next_resume_step(sv, st("", "", true), p, T0),
            next_resume_step(sv, st("", "evt", true), p, T0 + 250),
            next_resume_step(sv, st("evt", "evt", false), p, T0 + 500),
            next_resume_step(sv, st("evt", "evt", false), p, T0 + 750),
            next_resume_step(sv, st("evt", "evt", false), p, T0 + 1000)};
        std::vector<ResumeStep> want = {ResumeStep::Pin, ResumeStep::Wait,
            ResumeStep::Seek, ResumeStep::Pause, ResumeStep::Done};
        CHECK(got == want, "pin then seek then pause: Pin, Wait, Seek, Pause, Done");
    }
    {
        auto sv = saved(true, false);
        ResumeProgress p;
        int seeks = 0;
        for (int i = 0; i < 12; ++i)
            if (next_resume_step(sv, st("evt", "evt", false, 5000), p, T0 + i * 250)
                == ResumeStep::Seek) ++seeks;
        CHECK(seeks == 1, "seek once, however far the playhead still is");
    }
    {
        auto sv = saved(false, true);
        ResumeProgress p;
        auto a = next_resume_step(sv, st("evt", "", false), p, T0);
        auto b = next_resume_step(sv, st("evt", "", false), p, T0 + 250);
        auto c = next_resume_step(sv, st("evt", "", false), p, T0 + 500);
        CHECK(a == ResumeStep::Seek && b == ResumeStep::Pause && c == ResumeStep::Done,
              "live held: Seek, Pause, Done, never Pin");
    }
    {
        auto sv = saved(false, true);
        ResumeProgress p;
        auto a = next_resume_step(sv, st("other", "", false), p, T0);
        CHECK(a == ResumeStep::Done && !p.seek_sent, "room moved on: Done, no Seek");
    }
    {
        auto sv = saved(true, true);
        ResumeProgress p;
        next_resume_step(sv, st("", "", true), p, T0);
        auto a = next_resume_step(sv, st("", "other", true), p, T0 + 250);
        CHECK(a == ResumeStep::Done, "operator wins: another pin after ours is Done");
    }
    {
        auto sv = saved(true, true);
        ResumeProgress p;
        next_resume_step(sv, st("", "", true), p, T0);
        auto a = next_resume_step(sv, st("", "evt", true), p, T0 + 60001);
        CHECK(a == ResumeStep::Done && resume_gave_up(p, T0 + 60001) && p.pinned_sent,
              "gives up: still loading at started + 60001");
        ResumeProgress q;
        next_resume_step(sv, st("", "", true), q, T0);
        CHECK(next_resume_step(sv, st("", "evt", true), q, T0 + 60000) == ResumeStep::Wait,
              "not yet at exactly 60000");
    }
    {
        // Already pinned by the saved config: no second pin.
        auto sv = saved(true, false);
        ResumeProgress p;
        auto a = next_resume_step(sv, st("", "evt", true), p, T0);
        CHECK(a == ResumeStep::Wait && !p.pinned_sent, "already pinned: waits, does not re-pin");
    }
    // ResumeKeeper::stop() is safe when start() never ran.
    // (A Player is heavy to build here, so this is covered by the smoke run.)
    std::printf(g_fail ? "FAILED\n" : "all ok\n");
    return g_fail ? 1 : 0;
}
