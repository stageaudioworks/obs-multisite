// SPDX-License-Identifier: GPL-3.0-or-later
// test_http_date.cpp — reading the store's own clock out of an HTTP Date
// header, which is how a box checks its time of day without an NTP client.
#include "../src/core/http_date.h"

#include <cstdio>
#include <string>

using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

static bool parsed(const std::string& s, int64_t want) {
    int64_t got = 0;
    return parse_http_date_ms(s, got) && got == want;
}

int main() {
    std::printf("http date:\n");

    // The canonical example from RFC 7231, and the epoch itself.
    CHECK(parsed("Sun, 06 Nov 1994 08:49:37 GMT", 784111777000LL),
          "a real Date header parses to the right instant");
    CHECK(parsed("Thu, 01 Jan 1970 00:00:00 GMT", 0LL),
          "the epoch parses to zero, not to a failure");

    // A leap day and a century boundary: the two places a hand-rolled
    // days-from-civil gets it wrong if it gets it wrong at all.
    CHECK(parsed("Sat, 29 Feb 2020 12:00:00 GMT", 1582977600000LL),
          "a leap day is right");
    CHECK(parsed("Fri, 01 Mar 2024 00:00:00 GMT", 1709251200000LL),
          "the day after a leap day is right");

    // Another published example (RFC 2616 uses this one for the old format),
    // so the expectation is a documented value rather than one computed here.
    CHECK(parsed("Tue, 15 Nov 1994 08:12:31 GMT", 784887151000LL),
          "the second published example parses");

    int64_t out = -1;
    CHECK(parse_http_date_ms("06 Nov 1994 08:49:37 GMT", out) && out == 784111777000LL,
          "a header with no weekday prefix still parses");

    // Anything malformed is refused rather than guessed at.
    int64_t junk = 0;
    CHECK(!parse_http_date_ms("", junk), "empty is refused");
    CHECK(!parse_http_date_ms("not a date", junk), "nonsense is refused");
    CHECK(!parse_http_date_ms("Sun, 06 Xxx 1994 08:49:37 GMT", junk),
          "an unknown month is refused");
    CHECK(!parse_http_date_ms("Sun, 06 Nov 1994 08:49 GMT", junk),
          "a truncated time is refused");
    CHECK(!parse_http_date_ms("Sun, 6 Nov 1994 08:49:37 GMT", junk),
          "an unpadded day is refused rather than misread");

    std::printf("\n%s\n", g_fail == 0 ? "HTTP DATE TESTS PASSED"
                                     : "HTTP DATE TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
