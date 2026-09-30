// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// text.h — the two string helpers that had been written out in eight files.
#include <algorithm>
#include <cctype>
#include <string>

namespace multisite {

// Leading and trailing whitespace (std::isspace: space, tab, CR, LF, VT, FF).
inline std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

// ASCII lower case.
inline std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

} // namespace multisite
