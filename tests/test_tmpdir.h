// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// test_tmpdir.h — a scratch directory no other test run is using.
//
// Every test that touches disk used a FIXED name under the temp directory and
// began with remove_all() on it. Two runs at once on one machine — two agents,
// or a build and a TSan build side by side — then deleted each other's spools
// and caches mid-test, which reads as a flaky failure in whichever test lost.
// A per-run suffix makes them independent; the fixed stem keeps the directory
// recognisable when a crashed run leaves one behind.
#include <chrono>
#include <filesystem>
#include <random>
#include <string>

inline std::filesystem::path unique_temp_dir(const std::string& stem) {
    std::random_device rd;
    const auto t = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string tag = std::to_string((unsigned long long)t ^
                                           ((unsigned long long)rd() << 20));
    return std::filesystem::temp_directory_path() / (stem + "_" + tag);
}
