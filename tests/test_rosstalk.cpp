// SPDX-License-Identifier: GPL-3.0-or-later
// test_rosstalk.cpp — RossTalk line parsing and marker matching, pure.
#include "../src/appliance/rosstalk.h"

#include <cstdio>

using namespace multisite_player;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

static Status::MarkerEntry mk(const char* label, const char* author, long long at_ms,
                              long long at_media_ms) {
    Status::MarkerEntry m;
    m.label = label; m.author = author; m.at_ms = at_ms; m.at_media_ms = at_media_ms;
    return m;
}

static Status cs(bool playing, bool paused, bool loading, long long seek_target,
                 unsigned long long frames, long long playhead = 0) {
    Status s;
    s.playing = playing; s.paused = paused; s.loading = loading;
    s.seek_target_ms = seek_target; s.frames_out = frames; s.playhead_ms = playhead;
    return s;
}
static const long long T0 = 1000000, TARGET = 600000;

int main() {
    {
        CHECK(parse_rosstalk_line("PLAY").kind == RossCmd::Play, "PLAY");
        CHECK(parse_rosstalk_line("CC PLAY").kind == RossCmd::Play, "CC PLAY");
        CHECK(parse_rosstalk_line("cc play\r").kind == RossCmd::Play, "cc play\\r");
        CHECK(parse_rosstalk_line("HOLD").kind == RossCmd::Hold, "HOLD");
        CHECK(parse_rosstalk_line("LIVE").kind == RossCmd::Live, "LIVE");
        auto c = parse_rosstalk_line("CC CUE Sermon");
        CHECK(c.kind == RossCmd::Cue && c.arg == "Sermon", "CC CUE Sermon");
        CHECK(parse_rosstalk_line("CUE").kind == RossCmd::Unknown, "CUE without a name is Unknown");
        auto u = parse_rosstalk_line("JUMP 3\r");
        CHECK(u.kind == RossCmd::Unknown && u.text == "JUMP 3", "unknown keeps its text");
    }
    {
        auto c = parse_rosstalk_line("  CC   cue   sermon  ");
        CHECK(c.kind == RossCmd::Cue && c.arg == "sermon", "loose spacing and case");
        std::vector<Status::MarkerEntry> v = {mk(" Sermon", "a", 0, 600000)};
        CHECK(find_cue_marker_ms(v, "sermon", 0) == 600000, "label trimmed, case-insensitive");
    }
    {
        std::vector<Status::MarkerEntry> v = {mk("Sermon", "a", 0, 300000),
                                              mk("Sermon", "b", 0, 900000)};
        CHECK(find_cue_marker_ms(v, "Sermon", 0) == 900000, "latest wins");
    }
    {
        std::vector<Status::MarkerEntry> v = {mk("Sermon", "a", 5000000 + 120000, -1)};
        CHECK(find_cue_marker_ms(v, "Sermon", 5000000) == 120000, "legacy at_ms minus started");
        std::vector<Status::MarkerEntry> z = {mk("Sermon", "a", 0, -1)};
        CHECK(find_cue_marker_ms(z, "Sermon", 5000000) == -1, "no position is skipped");
    }
    {
        std::vector<Status::MarkerEntry> v = {mk("Intro", "a", 0, 1000)};
        CHECK(find_cue_marker_ms(v, "Sermon", 0) == -1, "missing");
    }
    {
        CueProgress p; p.started_wall_ms = T0;
        std::vector<CueStep> got = {
            next_cue_step(TARGET, cs(true, false, false, 0, 100), p, T0),
            next_cue_step(TARGET, cs(true, false, false, TARGET, 100), p, T0 + 250),
            next_cue_step(TARGET, cs(true, false, false, 0, 100, TARGET), p, T0 + 500),
            next_cue_step(TARGET, cs(true, false, false, 0, 100, TARGET), p, T0 + 750),
            next_cue_step(TARGET, cs(true, false, false, 0, 101, TARGET), p, T0 + 1000)};
        std::vector<CueStep> want = {CueStep::Seek, CueStep::Wait, CueStep::Wait, CueStep::Wait,
                                     CueStep::Hold};
        CHECK(got == want, "cue from playing: Seek, Wait x3, Hold");
        CHECK(p.outcome == "done", "cue from playing: outcome done");
        p.hold_sent = true;
        CHECK(next_cue_step(TARGET, cs(true, false, false, 0, 101, TARGET), p, T0 + 1250)
              == CueStep::Done, "cue from playing: Done after the hold");
    }
    {
        CueProgress p; p.started_wall_ms = T0;
        std::vector<CueStep> got = {
            next_cue_step(TARGET, cs(false, false, false, 0, 0), p, T0),
            next_cue_step(TARGET, cs(false, false, true, 0, 0), p, T0 + 250),
            next_cue_step(TARGET, cs(true, false, false, 0, 5), p, T0 + 500),
            next_cue_step(TARGET, cs(true, false, false, TARGET, 5), p, T0 + 750),
            next_cue_step(TARGET, cs(true, false, false, 0, 5, TARGET), p, T0 + 1000),
            next_cue_step(TARGET, cs(true, false, false, 0, 6, TARGET), p, T0 + 1250)};
        std::vector<CueStep> want = {CueStep::Start, CueStep::Wait, CueStep::Seek, CueStep::Wait,
                                     CueStep::Wait, CueStep::Hold};
        CHECK(got == want, "cue from stopped: Start, Wait, Seek, Wait, Wait, Hold");
    }
    {
        // Held: the Player is playing=true, paused=true. Start (a resume) comes
        // first; it is not repeated while still paused.
        CueProgress p; p.started_wall_ms = T0;
        std::vector<CueStep> got = {
            next_cue_step(TARGET, cs(true, true, false, 0, 100), p, T0),
            next_cue_step(TARGET, cs(true, true, false, 0, 100), p, T0 + 250),
            next_cue_step(TARGET, cs(true, false, false, 0, 100), p, T0 + 500),
            next_cue_step(TARGET, cs(true, false, false, TARGET, 100), p, T0 + 750),
            next_cue_step(TARGET, cs(true, false, false, 0, 100, TARGET), p, T0 + 1000),
            next_cue_step(TARGET, cs(true, false, false, 0, 101, TARGET), p, T0 + 1250)};
        std::vector<CueStep> want = {CueStep::Start, CueStep::Wait, CueStep::Seek, CueStep::Wait,
                                     CueStep::Wait, CueStep::Hold};
        CHECK(got == want, "cue from held: Start, Wait, Seek, Wait, Wait, Hold");
    }
    {
        CueProgress p; p.started_wall_ms = T0;
        auto r = next_cue_step(-1, cs(true, false, false, 0, 1), p, T0);
        CHECK(r == CueStep::Hold && p.outcome == "no such marker", "no marker: Hold at once");
    }
    {
        CueProgress p; p.started_wall_ms = T0;
        auto r = next_cue_step(TARGET, cs(false, false, true, 0, 0), p, T0 + 10001);
        CHECK(r == CueStep::Hold && p.outcome == "timed out", "gives up after 10 s");
    }
    {
        CueProgress p; p.started_wall_ms = T0;
        next_cue_step(TARGET, cs(true, false, false, 0, 100), p, T0);                 // Seek
        next_cue_step(TARGET, cs(true, false, false, 0, 100, TARGET), p, T0 + 250);   // snapshot
        bool held = false;
        for (int i = 0; i < 5; ++i)
            held |= next_cue_step(TARGET, cs(true, false, false, 0, 100, TARGET), p,
                                  T0 + 500 + i * 250) == CueStep::Hold;
        CHECK(!held, "audio first: no Hold while frames_out has not moved");
    }
    {
        // A failed seek leaves the target cleared; a frame from somewhere else
        // must not read as landed.
        CueProgress p; p.started_wall_ms = T0;
        next_cue_step(TARGET, cs(true, false, false, 0, 100), p, T0);
        next_cue_step(TARGET, cs(true, false, false, 0, 100, TARGET - 30000), p, T0 + 250);
        bool held = false;
        for (int i = 0; i < 5; ++i)
            held |= next_cue_step(TARGET, cs(true, false, false, 0, 101 + i, TARGET - 30000), p,
                                  T0 + 500 + i * 250) == CueStep::Hold;
        CHECK(!held, "wrong place: a frame 30 s away is not a landing");
        auto r = next_cue_step(TARGET, cs(true, false, false, 0, 110, TARGET - 30000), p,
                               T0 + 10001);
        CHECK(r == CueStep::Hold && p.outcome == "timed out", "wrong place: times out");
    }
    CHECK(seek_ms_for(0) == 1 && seek_ms_for(600000) == 600000, "seek_ms_for avoids 0");
    std::printf(g_fail ? "FAILED\n" : "all passed\n");
    return g_fail ? 1 : 0;
}
