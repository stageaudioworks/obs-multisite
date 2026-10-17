// SPDX-License-Identifier: GPL-3.0-or-later
#include "rosstalk.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace multisite_player {

namespace {
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}
std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
} // namespace

RossCommand parse_rosstalk_line(const std::string& line) {
    RossCommand c;
    std::string t = line;
    t.erase(std::remove(t.begin(), t.end(), '\r'), t.end());
    c.text = t;
    t = trim(t);
    std::istringstream in(t);
    std::string word;
    in >> word;
    if (lower(word) == "cc") in >> word; else if (word.empty()) return c;
    std::string verb = lower(word);
    std::string rest;
    std::getline(in, rest);
    rest = trim(rest);
    if (verb == "play") c.kind = RossCmd::Play;
    else if (verb == "hold") c.kind = RossCmd::Hold;
    else if (verb == "live") c.kind = RossCmd::Live;
    else if (verb == "cue" && !rest.empty()) { c.kind = RossCmd::Cue; c.arg = rest; }
    return c;
}

long long find_cue_marker_ms(const std::vector<Status::MarkerEntry>& markers,
                             const std::string& name, long long started_ms) {
    const std::string want = lower(trim(name));
    long long best = -1;
    for (const auto& m : markers) {
        if (lower(trim(m.label)) != want) continue;
        long long pos;
        if (m.at_media_ms >= 0) pos = m.at_media_ms;
        else if (m.at_ms > 0) pos = m.at_ms - started_ms;
        else continue;
        best = std::max(best, pos);
    }
    return best;
}

} // namespace multisite_player
