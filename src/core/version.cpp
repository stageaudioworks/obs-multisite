// SPDX-License-Identifier: GPL-3.0-or-later
#include "version.h"

#include <cctype>

namespace multisite {

namespace {

bool digits(const std::string& s, size_t& i, int& out) {
    const size_t start = i;
    long v = 0;
    while (i < s.size() && std::isdigit((unsigned char)s[i])) {
        v = v * 10 + (s[i] - '0');
        if (v > 1000000) v = 1000000;      // a junk tag must not overflow
        ++i;
    }
    if (i == start) return false;
    out = (int)v;
    return true;
}

} // namespace

bool parse_version(const std::string& text, Version& out) {
    size_t i = 0;
    while (i < text.size() && std::isspace((unsigned char)text[i])) ++i;
    if (i < text.size() && (text[i] == 'v' || text[i] == 'V')) ++i;

    Version v;
    if (!digits(text, i, v.major)) return false;
    // Minor and patch are optional, so `v2` and `0.1` both parse rather than
    // being discarded for not being written out in full.
    if (i < text.size() && text[i] == '.') {
        ++i;
        if (!digits(text, i, v.minor)) return false;
        if (i < text.size() && text[i] == '.') {
            ++i;
            if (!digits(text, i, v.patch)) return false;
        }
    }

    // Whatever is left must be a pre-release or build marker. `-alpha`,
    // `-rc1`, `+build` — the project uses `-alpha`, and anything after a dash
    // is a pre-release for our purposes. Trailing junk with no separator at
    // all is not a version this can reason about. Trailing whitespace is not
    // junk: a tag pasted out of a release page arrives with it.
    while (i < text.size() && std::isspace((unsigned char)text[i])) ++i;
    if (i < text.size()) {
        if (text[i] != '-' && text[i] != '+') return false;
        v.pre = (text[i] == '-');
        ++i;
        if (i >= text.size()) return false;    // a bare trailing dash
    }

    out = v;
    return true;
}

int compare_versions(const std::string& a, const std::string& b) {
    Version va, vb;
    if (!parse_version(a, va) || !parse_version(b, vb)) return 0;   // no answer

    if (va.major != vb.major) return va.major < vb.major ? -1 : 1;
    if (va.minor != vb.minor) return va.minor < vb.minor ? -1 : 1;
    if (va.patch != vb.patch) return va.patch < vb.patch ? -1 : 1;
    // Same numbers: the release is newer than its own pre-release, and two
    // pre-releases of the same numbers are treated as the same version. The
    // ordering of `-alpha` against `-beta` is not a distinction this project
    // makes, since it never publishes two pre-releases of one version.
    if (va.pre != vb.pre) return va.pre ? -1 : 1;
    return 0;
}

std::string latest_tag_from_release_json(const std::string& body) {
    const std::string key = "\"tag_name\"";
    const size_t k = body.find(key);
    if (k == std::string::npos) return "";
    size_t i = k + key.size();
    while (i < body.size() && std::isspace((unsigned char)body[i])) ++i;
    if (i >= body.size() || body[i] != ':') return "";
    ++i;
    while (i < body.size() && std::isspace((unsigned char)body[i])) ++i;
    if (i >= body.size() || body[i] != '"') return "";
    ++i;
    std::string out;
    while (i < body.size() && body[i] != '"') {
        if (body[i] == '\\' && i + 1 < body.size()) ++i;   // escaped char
        out.push_back(body[i]);
        ++i;
    }
    if (i >= body.size()) return "";    // unterminated
    return out;
}

} // namespace multisite
