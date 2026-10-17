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
                             const std::string& name) {
    const std::string want = lower(trim(name));
    long long best = -1;
    for (const auto& m : markers) {
        if (lower(trim(m.label)) != want) continue;
        if (m.at_media_ms < 0) continue;
        best = std::max(best, m.at_media_ms);
    }
    return best;
}

long long seek_ms_for(long long target_ms) { return target_ms == 0 ? 1 : target_ms; }

CueStep next_cue_step(long long target_ms, const Status& now, CueProgress& p,
                      long long now_wall_ms) {
    if (p.hold_sent) return CueStep::Done;
    if (target_ms < 0) {
        p.outcome = "no such marker"; p.pause_on_hold = now.playing;
        return CueStep::Hold;
    }
    if (now_wall_ms - p.started_wall_ms > kCueGiveUpMs) {
        p.outcome = "timed out"; p.pause_on_hold = now.playing;
        return CueStep::Hold;
    }
    const bool idle = !now.playing || now.paused;
    if (idle && !now.loading && !p.start_sent) { p.start_sent = true; return CueStep::Start; }
    if (now.loading || (idle && p.start_sent)) return CueStep::Wait;
    if (!p.seek_sent) { p.seek_sent = true; return CueStep::Seek; }
    if (now.seek_target_ms != 0) return CueStep::Wait;
    if (!p.target_cleared) {
        p.target_cleared = true;
        p.frames_at_clear = now.frames_out;
        return CueStep::Wait;
    }
    long long off = now.playhead_ms - seek_ms_for(target_ms);
    if (now.frames_out > p.frames_at_clear && off <= kCueLandToleranceMs
        && off >= -kCueLandToleranceMs) {
        p.outcome = "done"; p.pause_on_hold = now.playing;
        return CueStep::Hold;
    }
    return CueStep::Wait;
}

CueStep cue_after_seek(long long target_ms, const Status& after, CueProgress& p) {
    if (after.seek_target_ms == 0 && after.playhead_ms != seek_ms_for(target_ms)) {
        p.outcome = "cannot reach the marker"; p.pause_on_hold = after.playing;
        return CueStep::Hold;
    }
    return CueStep::Wait;
}

} // namespace multisite_player
