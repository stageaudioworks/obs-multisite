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
    std::printf(g_fail ? "FAILED\n" : "all passed\n");
    return g_fail ? 1 : 0;
}
