// SPDX-License-Identifier: GPL-3.0-or-later
// test_thread_hook.cpp — a host can name and prioritise the player's threads.
#include "../src/appliance/player.h"
#include "../src/core/cmaf_decoder.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace multisite_player;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

namespace {
std::mutex               g_mtx;
std::vector<std::string> g_roles;

void record(const char* role) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_roles.emplace_back(role);
}
bool saw(const char* role) {
    std::lock_guard<std::mutex> lk(g_mtx);
    return std::find(g_roles.begin(), g_roles.end(), role) != g_roles.end();
}
bool wait_for(const char* role) {
    for (int i = 0; i < 200 && !saw(role); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return saw(role);
}
} // namespace

int main() {
    std::printf("thread hook:\n");

    {
        NullVideoOutput video;
        NullAudioOutput audio;
        Player player(Config{}, video, audio);
        player.set_thread_hook(record);
        player.start();
        CHECK(wait_for("poll"),    "poll thread calls the hook");
        CHECK(wait_for("feed"),    "feed thread calls the hook");
        CHECK(wait_for("deliver"), "deliver thread calls the hook");
        CHECK(wait_for("aes67"),   "aes67 thread calls the hook");
        player.stop();
    }

    {
        g_roles.clear();
        multisite::CmafDecoder dec;
        dec.set_thread_hook(record);
        dec.start({0, 1, 2, 3});   // not a real stream; the thread still runs
        CHECK(wait_for("decode"), "decode thread calls the hook");
        dec.stop();
    }

    return g_fail ? 1 : 0;
}
