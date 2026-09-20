// SPDX-License-Identifier: GPL-3.0-or-later
// test_caption_text.cpp — a spoken sentence cut into captions that survive.
//
// libobs truncates a caption at CAPTION_LINE_BYTES with a single snprintf,
// under a comment claiming it splits. A sermon sentence is routinely longer
// than that, so without this the back half of most captions never leaves the
// building. Nothing here needs libobs: that is the point of the split.
#include "../src/obs/caption_text.h"

#include <cstdio>
#include <numeric>

using namespace multisite_obs;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

static bool all_within(const std::vector<std::string>& v, size_t n) {
    for (const auto& s : v) if (s.size() > n) return false;
    return true;
}
// Every word that went in comes out, in order, exactly once.
static std::string joined(const std::vector<std::string>& v) {
    std::string out;
    for (const auto& s : v) { if (!out.empty()) out += ' '; out += s; }
    return out;
}

int main() {
    std::printf("== short text is one caption, unchanged ==\n");
    {
        auto v = split_caption("Welcome to the service.");
        CHECK(v.size() == 1, "one caption");
        CHECK(v[0] == "Welcome to the service.", "and it is the text as given");
    }

    std::printf("== nothing in, nothing out ==\n");
    {
        CHECK(split_caption("").empty(), "empty text yields no caption");
        CHECK(split_caption("     ").empty(), "spaces alone yield no caption");
        CHECK(split_caption("hi", 0).empty(), "a zero limit yields no caption");
    }

    std::printf("== a real sentence, longer than the 128-byte buffer ==\n");
    {
        // 180 bytes: the length libobs would silently cut to 128.
        const std::string s =
            "And he said unto them, go ye into all the world and preach the "
            "gospel to every creature, for the harvest truly is plenteous but "
            "the labourers are few indeed.";
        CHECK(s.size() > kCaptionMaxBytes, "the fixture is longer than one caption");
        auto v = split_caption(s);
        CHECK(v.size() > 1, "it is split, not truncated");
        CHECK(all_within(v, kCaptionMaxBytes), "every caption fits the buffer");
        CHECK(joined(v) == s, "and every word survives, in order");
    }

    std::printf("== cuts land on word boundaries ==\n");
    {
        std::string s;
        for (int i = 0; i < 40; ++i) s += "alpha ";
        s.pop_back();
        auto v = split_caption(s);
        CHECK(all_within(v, kCaptionMaxBytes), "every caption fits");
        bool whole_words = true;
        for (const auto& c : v) {
            if (c.front() == ' ' || c.back() == ' ') whole_words = false;
            // "alpha" is 5 letters; any fragment proves a mid-word cut.
            for (size_t i = 0; i + 5 <= c.size(); i += 6)
                if (c.compare(i, 5, "alpha") != 0) whole_words = false;
        }
        CHECK(whole_words, "no caption starts, ends or cuts mid-word");
        CHECK(joined(v) == s, "every word survives");
    }

    std::printf("== a word longer than a caption still gets through ==\n");
    {
        // The guard against one enormous token (a URL) producing a caption of
        // two characters at a time, or worse, looping for ever.
        const std::string huge(300, 'x');
        auto v = split_caption(huge);
        CHECK(v.size() == 3, "broken into whole-buffer pieces");
        CHECK(all_within(v, kCaptionMaxBytes), "each fits");
        CHECK(v[0].size() == kCaptionMaxBytes, "and fills the buffer rather than dribbling");
        std::string back; for (auto& s : v) back += s;
        CHECK(back == huge, "nothing is lost");
    }

    std::printf("== UTF-8 is never cut through a character ==\n");
    {
        // Three-byte characters, so a byte-count cut lands mid-character unless
        // it is backed off. A caption cut here is a mojibake glyph on screen.
        std::string s;
        for (int i = 0; i < 100; ++i) s += "\xE2\x80\x94";   // em dash
        auto v = split_caption(s);
        CHECK(all_within(v, kCaptionMaxBytes), "each fits the buffer");
        bool clean = true;
        for (const auto& c : v) {
            if (c.size() % 3 != 0) clean = false;            // whole characters only
            if (!c.empty() && is_utf8_continuation((unsigned char)c[0])) clean = false;
        }
        CHECK(clean, "no caption begins or ends inside a character");
        std::string back; for (auto& c : v) back += c;
        CHECK(back == s, "nothing is lost");
    }

    std::printf("== accented words survive a cut ==\n");
    {
        std::string s;
        for (int i = 0; i < 30; ++i) s += "café ";          // é is two bytes
        s.pop_back();
        auto v = split_caption(s);
        CHECK(all_within(v, kCaptionMaxBytes), "each fits");
        CHECK(joined(v) == s, "every word survives intact");
    }

    std::printf("%s\n", g_fail ? "FAILED" : "all passed");
    return g_fail ? 1 : 0;
}
