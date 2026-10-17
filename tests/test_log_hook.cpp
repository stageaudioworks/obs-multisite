// SPDX-License-Identifier: GPL-3.0-or-later
// test_log_hook.cpp — a host can receive the player's log lines.
#include "../src/appliance/log.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace multisite_player;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

int main() {
    std::printf("log hook:\n");
    std::vector<std::pair<int, std::string>> got;
    set_log_hook([&](int level, const char* msg) { got.emplace_back(level, msg); });

    plog_warn("x %d", 1);
    CHECK(got.size() == 1, "one line reaches the hook");
    CHECK(!got.empty() && got[0].first == 4, "warn is level 4");
    CHECK(!got.empty() && got[0].second == "x 1", "message has no prefix");

    plog_error("e");
    plog_info("i");
    CHECK(got.size() == 3 && got[1].first == 3 && got[2].first == 6,
          "error 3, info 6");

    plog_debug("hidden");
    CHECK(got.size() == 3, "debug is dropped while debug logging is off");

    CHECK(!recent_log(1).empty() && recent_log(1)[0].text == "i",
          "the ring still gets the line");

    set_log_hook(nullptr);
    plog_warn("after");
    CHECK(got.size() == 3, "cleared hook gets nothing");

    return g_fail ? 1 : 0;
}
