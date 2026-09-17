// SPDX-License-Identifier: GPL-3.0-or-later
// test_version.cpp — deciding whether the published tag is newer than this
// build. Pure arithmetic and one string scan; no network, no curl.
#include "../src/core/version.h"

#include <cstdio>
#include <string>

using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

int main() {
    std::printf("== 1. Parsing the tags this project actually cuts ==\n");
    {
        Version v;
        CHECK(parse_version("v0.1.21-alpha", v) && v.major == 0 && v.minor == 1 &&
                  v.patch == 21 && v.pre,
              "v0.1.21-alpha parses, and is a pre-release");
        CHECK(parse_version("0.1.21", v) && v.patch == 21 && !v.pre,
              "0.1.21 parses as a release");
        CHECK(parse_version("1.2.3-beta.2", v) && v.major == 1 && v.pre,
              "a dotted pre-release parses");
        CHECK(parse_version("v2", v) && v.major == 2 && v.minor == 0 && v.patch == 0,
              "a bare major parses, with minor and patch zero");
        CHECK(parse_version("  v0.2.0  ", v) && v.minor == 2,
              "surrounding whitespace is tolerated");
        CHECK(!parse_version("", v), "an empty tag does not parse");
        CHECK(!parse_version("latest", v), "\"latest\" does not parse");
        CHECK(!parse_version("v1.x.0", v), "a non-numeric component does not parse");
        CHECK(!parse_version("v1.2.3-", v), "a bare trailing dash does not parse");
    }

    std::printf("== 2. Ordering ==\n");
    {
        CHECK(compare_versions("v0.1.20-alpha", "v0.1.21-alpha") < 0,
              "0.1.20 is older than 0.1.21");
        CHECK(compare_versions("v0.1.21-alpha", "v0.1.21-alpha") == 0,
              "the same tag compares equal");
        CHECK(compare_versions("v0.2.0-alpha", "v0.1.99-alpha") > 0,
              "a minor bump beats any patch");
        CHECK(compare_versions("v1.0.0", "v0.9.9") > 0,
              "a major bump beats anything below it");
        // The rule that matters while every tag is a pre-release.
        CHECK(compare_versions("v0.2.0-alpha", "v0.2.0") < 0,
              "the alpha of a version is older than the release of it");
        CHECK(compare_versions("0.1.21", "v0.1.21-alpha") > 0,
              "and the release is newer than its own alpha");
        // No answer, rather than a wrong one.
        CHECK(compare_versions("nonsense", "v0.1.21-alpha") == 0,
              "an unparseable tag yields no answer");
        CHECK(compare_versions("v0.1.21-alpha", "") == 0,
              "an empty tag yields no answer");
    }

    std::printf("== 3. Reading the tag out of a releases/latest body ==\n");
    {
        const std::string body =
            "{\"url\":\"https://api.github.com/repos/x/y/releases/1\","
            "\"id\":123,\"tag_name\":\"v0.1.21-alpha\","
            "\"name\":\"v0.1.21-alpha\",\"draft\":false}";
        CHECK(latest_tag_from_release_json(body) == "v0.1.21-alpha",
              "the tag is found among the other fields");
        CHECK(latest_tag_from_release_json("{\"tag_name\": \"v0.2.0\"}") == "v0.2.0",
              "spacing after the colon is tolerated");
        CHECK(latest_tag_from_release_json("{\"message\":\"Not Found\"}") == "",
              "an error body yields nothing");
        CHECK(latest_tag_from_release_json("") == "", "an empty body yields nothing");
        CHECK(latest_tag_from_release_json("{\"tag_name\":\"v9.9.9") == "",
              "an unterminated string yields nothing");
        // A body that mentions the key only inside a string value must not be
        // mistaken for the real field — the scan is naive, so this documents
        // the limit rather than pretending there is none.
        CHECK(latest_tag_from_release_json("\"note\":\"see tag_name in docs\"") == "",
              "the key without a quoted value yields nothing");
    }

    std::printf("\n%s\n", g_fail == 0 ? "VERSION TESTS PASSED"
                                      : "VERSION TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
