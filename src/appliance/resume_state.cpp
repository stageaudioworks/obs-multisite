// SPDX-License-Identifier: GPL-3.0-or-later
#include "resume_state.h"

#include "../vendor/nlohmann/json.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <unistd.h>

using json = nlohmann::json;

namespace multisite_player {

std::string resume_state_path(const std::string& config_path) {
    const auto slash = config_path.find_last_of('/');
    return (slash == std::string::npos ? std::string()
                                       : config_path.substr(0, slash + 1)) +
           "resume.json";
}

bool save_resume_state(const std::string& path, const ResumeState& s,
                       std::string& error) {
    json j;
    j["event_id"]      = s.event_id;
    j["pinned"]        = s.pinned;
    j["paused"]        = s.paused;
    j["media_ms"]      = s.media_ms;
    j["saved_wall_ms"] = s.saved_wall_ms;

    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) { error = "cannot write " + tmp; return false; }
        out << j.dump(2) << "\n";
        out.flush();
        if (!out) { error = "failed writing " + tmp; return false; }
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        error = "cannot replace " + path;
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

bool load_resume_state(const std::string& path, ResumeState& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    // Non-throwing parse: a truncated file is "nothing to restore", not a crash.
    const json j = json::parse(ss.str(), nullptr, false);
    if (!j.is_object()) return false;
    try {
        ResumeState r;
        r.event_id      = j.at("event_id").get<std::string>();
        r.pinned        = j.at("pinned").get<bool>();
        r.paused        = j.at("paused").get<bool>();
        r.media_ms      = j.at("media_ms").get<long long>();
        r.saved_wall_ms = j.at("saved_wall_ms").get<long long>();
        out = r;
        return true;
    } catch (const json::exception&) {
        return false;
    }
}

bool resume_is_fresh(const ResumeState& s, long long now_wall_ms,
                     long long max_age_ms) {
    const long long age = now_wall_ms - s.saved_wall_ms;
    return age >= 0 && age <= max_age_ms;
}

bool resume_worth_restoring(const ResumeState& s) {
    return s.pinned || s.paused;
}

} // namespace multisite_player
