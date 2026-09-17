// SPDX-License-Identifier: GPL-3.0-or-later
#include "http_date.h"

#include <cctype>

namespace multisite {

namespace {

int month_from(const std::string& m) {
    static const char* k[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                               "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    for (int i = 0; i < 12; ++i)
        if (m == k[i]) return i + 1;
    return 0;
}

// Days since 1970-01-01 for a proleptic Gregorian date. Howard Hinnant's
// days_from_civil, which is exact for every date the era can hold and needs no
// <ctime> and no timezone database.
int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= (m <= 2);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

} // namespace

bool parse_http_date_ms(const std::string& s, int64_t& out_ms) {
    // Skip an optional "Www, " weekday prefix — the shape is fixed without it
    // as well, and a store that omits the day name should still be readable.
    size_t i = 0;
    const size_t comma = s.find(',');
    if (comma != std::string::npos) i = comma + 1;
    while (i < s.size() && s[i] == ' ') ++i;

    // "DD Mon YYYY HH:MM:SS GMT" is 20 characters or more.
    if (s.size() < i + 19) return false;

    auto num = [&](size_t at, int len) -> int {
        int v = 0;
        for (int k = 0; k < len; ++k) {
            const char c = s[at + (size_t)k];
            if (c < '0' || c > '9') return -1;
            v = v * 10 + (c - '0');
        }
        return v;
    };

    const int day = num(i, 2);
    if (day < 1 || s[i + 2] != ' ') return false;
    const int month = month_from(s.substr(i + 3, 3));
    if (!month || s[i + 6] != ' ') return false;
    const int year = num(i + 7, 4);
    if (year < 0 || s[i + 11] != ' ') return false;
    const int hh = num(i + 12, 2);
    if (hh < 0 || s[i + 14] != ':') return false;
    const int mm = num(i + 15, 2);
    if (mm < 0 || s[i + 17] != ':') return false;
    const int ss = num(i + 18, 2);
    if (ss < 0) return false;

    const int64_t days = days_from_civil(year, (unsigned)month, (unsigned)day);
    out_ms = ((days * 24 + hh) * 60 + mm) * 60 * 1000 + (int64_t)ss * 1000;
    return true;
}

} // namespace multisite
