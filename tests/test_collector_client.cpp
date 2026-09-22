// SPDX-License-Identifier: GPL-3.0-or-later
// test_collector_client.cpp — the collector URL authority.
//
// The paths live in one place now (collector_client.h) because two hosts each
// built their own and the four endpoints could drift apart. The URL builder is
// the part that can be wrong without a socket, so it is pinned here.
#include "../src/core/collector_client.h"

#include <cstdio>
#include <string>

using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

int main() {
    std::printf("A trailing slash never doubles\n");
    {
        CHECK(collector_url("https://c.example", "/v1/heartbeat") ==
                  "https://c.example/v1/heartbeat", "no slash, plain join");
        // The operator's field may carry a slash; both must give the same URL.
        CHECK(collector_url("https://c.example/", "/v1/heartbeat") ==
                  "https://c.example/v1/heartbeat", "one trailing slash trimmed");
        CHECK(collector_url("https://c.example///", "/v1/heartbeat") ==
                  "https://c.example/v1/heartbeat", "several trimmed");
        CHECK(collector_url("https://c.example/", "/v1/heartbeat") ==
                  collector_url("https://c.example", "/v1/heartbeat"),
              "the two forms agree — the whole point");
    }

    std::printf("Every endpoint shares the same base handling\n");
    {
        const char* base = "https://api.multisite-cloud.example/";
        CHECK(collector_url(base, kHeartbeatPath) ==
                  "https://api.multisite-cloud.example/v1/heartbeat",
              "heartbeat");
        CHECK(collector_url(base, kPairStartPath) ==
                  "https://api.multisite-cloud.example/v1/pair/start",
              "pair/start");
        CHECK(collector_url(base, kPairPollPath) ==
                  "https://api.multisite-cloud.example/v1/pair/poll",
              "pair/poll");
        CHECK(collector_url(base, kCredentialsPath) ==
                  "https://api.multisite-cloud.example/v1/credentials",
              "credentials");
    }

    std::printf("Degenerate bases do not crash\n");
    {
        CHECK(collector_url("", "/v1/heartbeat") == "/v1/heartbeat",
              "empty base yields the path");
        CHECK(collector_url("/", "/v1/heartbeat") == "/v1/heartbeat",
              "a lone slash yields the path");
        CHECK(collector_url("https://c.example", "") == "https://c.example",
              "empty path yields the base, slashes trimmed");
    }

    std::printf("\n%s\n", g_fail == 0 ? "ALL COLLECTOR CLIENT TESTS PASSED"
                                      : "SOME COLLECTOR CLIENT TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
