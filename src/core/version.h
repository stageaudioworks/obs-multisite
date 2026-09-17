// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// version.h — comparing the build you have against the one that is published.
//
// Nothing here touches the network. The check that fetches the published tag
// lives with each host — the plugin's own background thread, the appliance's
// libcurl helper — and the part that can actually be wrong, deciding whether
// one tag is newer than another, is here, where it is tested with no network at
// all.
//
#include <string>

namespace multisite {

// A version as these tags are written: `v0.1.21-alpha`, `0.1.21`, `1.2.3`.
// Anything else fails to parse, and a tag that fails to parse is ignored
// rather than treated as zero — "newer than everything" is the wrong way to be
// wrong about an update.
struct Version {
    int  major = 0, minor = 0, patch = 0;
    // A pre-release (`-alpha`, `-rc1`) sorts BELOW the release of the same
    // numbers: 0.2.0-alpha is older than 0.2.0. Every tag this project cuts is
    // a pre-release, so this one rule is what stops an alpha of the next
    // version from reading as newer than the release of it.
    bool pre = false;
};

bool parse_version(const std::string& text, Version& out);

// -1 if `a` is older than `b`, 0 if they are the same version, 1 if `a` is
// newer. Input that does not parse counts as no answer and returns 0, so a host
// handed a nonsense tag says nothing rather than announcing an update.
int compare_versions(const std::string& a, const std::string& b);

// The first `tag_name` in a GitHub releases body, or "" when it is not
// present. A scan for the one field, not a JSON parse: pulling a parser into
// the core for a single string is not a trade worth making, and every failure
// mode — a rate-limit body, an error object, HTML from a captive portal —
// simply yields "" rather than something that needs interpreting.
//
// "First" because the list endpoint is newest-first and the check asks for one
// release; see the note on the URL in src/obs/update_check.cpp for why the
// `releases/latest` endpoint cannot be used at all here.
std::string latest_tag_from_release_json(const std::string& body);

} // namespace multisite
