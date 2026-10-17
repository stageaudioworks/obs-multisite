// SPDX-License-Identifier: GPL-3.0-or-later
// test_resume_driver.cpp — the restore order, pure.
#include "../src/appliance/resume_driver.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

using namespace multisite_player;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

static Status st(const char* ev, const char* pinned, bool loading, long long seek_target = 0) {
    Status s;
    s.event_id = ev; s.pinned_event_id = pinned; s.loading = loading;
    s.seek_target_ms = seek_target;
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
        // Waiting on the seek (target still set) must not send a second one.
        auto sv = saved(true, false);
        ResumeProgress p;
        int seeks = 0, waits = 0;
        for (int i = 0; i < 12; ++i) {
            auto r = next_resume_step(sv, st("evt", "evt", false, i ? 600000 : 0), p, T0 + i * 250);
            if (r == ResumeStep::Seek) ++seeks;
            if (r == ResumeStep::Wait) ++waits;
        }
        CHECK(seeks == 1 && waits == 11, "seek once, Wait while the target is set");
    }
    {
        // The hold goes out only once the seek has landed: while held nothing
        // is delivered, so the screen would never get to the target.
        auto sv = saved(true, true);
        ResumeProgress p;
        std::vector<ResumeStep> got = {
            next_resume_step(sv, st("evt", "evt", false), p, T0),
            next_resume_step(sv, st("evt", "evt", false, 600000), p, T0 + 250),
            next_resume_step(sv, st("evt", "evt", false, 600000), p, T0 + 500),
            next_resume_step(sv, st("evt", "evt", false), p, T0 + 750),
            next_resume_step(sv, st("evt", "evt", false), p, T0 + 1000)};
        std::vector<ResumeStep> want = {ResumeStep::Seek, ResumeStep::Wait,
            ResumeStep::Wait, ResumeStep::Pause, ResumeStep::Done};
        CHECK(got == want, "Seek, Wait while target set, Pause, Done");
    }
    {
        // Held at the very start: seeking to 0 is the failure sentinel in
        // Player::seek_to_media, and a fresh pin already starts there.
        auto sv = saved(true, true);
        sv.media_ms = 0;
        ResumeProgress p;
        auto a = next_resume_step(sv, st("evt", "evt", false), p, T0);
        auto b = next_resume_step(sv, st("evt", "evt", false), p, T0 + 250);
        CHECK(a == ResumeStep::Pause && b == ResumeStep::Done && p.seek_sent,
              "media_ms 0: no Seek, straight to Pause");
    }
    {
        // Config-pinned box: the pin was seen, never sent. The operator
        // unpinning within the window must not get the pin put back.
        auto sv = saved(true, true);
        ResumeProgress p;
        next_resume_step(sv, st("", "evt", true), p, T0);
        auto a = next_resume_step(sv, st("evt", "", false), p, T0 + 250);
        CHECK(a == ResumeStep::Done && !p.pinned_sent && !p.seek_sent,
              "pin seen (config): operator unpin is Done, no re-Pin, no Seek");
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
    {
        // pin_event() no-ops with no session yet: the pin must be re-sent, and
        // must not be mistaken for the operator choosing something else.
        auto sv = saved(true, true);
        ResumeProgress p;
        CHECK(next_resume_step(sv, st("", "", true), p, T0) == ResumeStep::Pin, "pin sent");
        resume_pin_result(sv, st("", "", true), p);   // no session: pin did not take
        CHECK(next_resume_step(sv, st("", "", true), p, T0 + 250) == ResumeStep::Pin,
              "pin that did not take is sent again, not Done");
        resume_pin_result(sv, st("", "evt", true), p);  // took
        CHECK(next_resume_step(sv, st("", "evt", true), p, T0 + 500) == ResumeStep::Wait,
              "pin that took is not re-sent");
    }
    {
        CHECK(resume_should_write(T0, T0, true), "a change is written at once");
        CHECK(!resume_should_write(T0 + 9999, T0, false), "no change, under 10 s: no write");
        CHECK(resume_should_write(T0 + 10000, T0, false),
              "no change at all still rewrites every 10 s (held box stays fresh)");
    }
    {
        // Clock trust: wait for it, bounded; a stop ends the wait.
        long long waited = 0;
        int polls = 0;
        auto wait = [&](long long ms) { waited += ms; return true; };
        auto synced_after3 = [&] { return ++polls > 3; };
        CHECK(resume_wait_for_clock(synced_after3, wait, 30000, 500) && waited == 1500,
              "clock syncs on the 4th look: true after 1500 ms of waiting");
        waited = 0;
        CHECK(!resume_wait_for_clock([] { return false; }, wait, 30000, 500) &&
              waited == 30000, "never syncs: false after exactly 30 s");
        CHECK(!resume_wait_for_clock([] { return false; },
                                     [](long long) { return false; }, 30000, 500),
              "asked to stop: false at once");
        CHECK(resume_wait_for_clock([] { return true; }, wait, 30000, 500),
              "already synchronised: true without waiting");
    }
    {
        // The thread, for real: an unconfigured Player, a fresh file, stop at
        // once. Covers stop-without-start, stop mid-restore (which must not
        // fall into save_loop and overwrite the file), and the join.
        const char* t = std::getenv("TMPDIR");
        std::string path = std::string(t && *t ? t : "/tmp") + "/test_resume_keeper.json";
        ResumeState r;
        r.event_id = "evt-keep"; r.pinned = true; r.paused = true;
        r.media_ms = 600000;
        r.saved_wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::string err;
        save_resume_state(path, r, err);
        auto slurp = [&] {
            std::ifstream in(path); std::stringstream ss; ss << in.rdbuf(); return ss.str();
        };
        const std::string before = slurp();
        {
            Config cfg;
            NullVideoOutput vout;
            NullAudioOutput aout;
            Player player(cfg, vout, aout);
            ResumeKeeper never_started(player, path);
            never_started.stop();
            ResumeKeeper keeper(player, path, [] { return true; });
            keeper.start();
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            keeper.stop();
        }
        CHECK(slurp() == before, "keeper stopped mid-restore leaves resume.json unchanged");
        std::remove(path.c_str());
    }
    std::printf(g_fail ? "FAILED\n" : "all ok\n");
    return g_fail ? 1 : 0;
}
