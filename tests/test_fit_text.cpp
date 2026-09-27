// SPDX-License-Identifier: GPL-3.0-or-later
// test_fit_text.cpp — shortening a dock value to the width it has
// (src/obs/ui/fit_text.h). FitLabel passes Qt's font measurement to the same
// function; here a fake one makes every character 10 px, so each expected
// answer can be worked out by hand.
#include "../src/obs/ui/fit_text.h"

#include <cstdio>
#include <string>

using multisite_ui::fit_middle;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

// 10 px per code point, the ellipsis included.
static int width(const std::string& s) {
    int n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) ++n;
    return n * 10;
}
static bool starts_with(const std::string& s, const std::string& p) {
    return s.compare(0, p.size(), p) == 0;
}
static bool ends_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

int main() {
    const std::string dots = "\xE2\x80\xA6";
    // The value that held the decoder dock wide (2026-09-27).
    const std::string aes = "2 channels  Programme (AES67: Daemon 0a0ac450 Multisite rpi5-nathan)";

    std::printf("== a value that fits is left alone ==\n");
    CHECK(fit_middle("48.6 Mbps", 200, width) == "48.6 Mbps", "short text is unchanged");
    CHECK(fit_middle("abcdefghij", 100, width) == "abcdefghij", "exactly the width is unchanged");

    std::printf("== a value that does not is cut in the middle ==\n");
    {
        const std::string out = fit_middle(aes, 300, width);
        std::printf("       300 px: \"%s\"\n", out.c_str());
        CHECK(width(out) <= 300, "the result fits");
        CHECK(width(out) >= 290, "and uses the width: no more than one character short");
        CHECK(out.find(dots) != std::string::npos, "the cut is marked");
        CHECK(starts_with(out, "2 channels"), "the start is kept (what it is)");
        CHECK(ends_with(out, "nathan)"), "and so is the end (where it came from)");
    }
    {
        // 10 characters: 9 kept around the ellipsis, the odd one to the head.
        const std::string out = fit_middle("abcdefghijklmnop", 100, width);
        CHECK(out == "abcde" + dots + "mnop", "kept characters split evenly, the head taking the odd one");
    }
    {
        const std::string out = fit_middle("aaaa bbbb cccc dddd", 100, width);
        CHECK(out.find(" " + dots) == std::string::npos &&
                  out.find(dots + " ") == std::string::npos,
              "no space is left hanging either side of the cut");
    }
    {
        // Two-byte characters: a cut inside one would be mojibake.
        const std::string out = fit_middle("\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9", 50, width);
        CHECK(out == "\xC3\xA9\xC3\xA9" + dots + "\xC3\xA9\xC3\xA9",
              "a multi-byte character is never split");
    }

    std::printf("== too narrow for anything ==\n");
    CHECK(fit_middle(aes, 5, width).empty(), "narrower than the ellipsis: empty, not a stray part");
    CHECK(fit_middle(aes, 0, width).empty(), "a width of zero: empty");
    CHECK(fit_middle(aes, 10, width) == dots, "exactly the ellipsis: the ellipsis");
    CHECK(fit_middle("", 0, width).empty(), "and nothing stays nothing");

    std::printf("== more room never shows less ==\n");
    {
        bool ok = true;
        size_t last = 0;
        for (int px = 0; px <= width(aes); px += 5) {
            const std::string out = fit_middle(aes, px, width);
            if (width(out) > px || out.size() + 1 < last) ok = false;
            last = out.size();
        }
        CHECK(ok, "every width from 0 to the whole: it fits, and it only grows");
        CHECK(fit_middle(aes, width(aes), width) == aes, "and at full width, the whole of it");
    }

    std::printf("\n%s\n", g_fail == 0 ? "ALL FIT TEXT TESTS PASSED" : "SOME FIT TEXT TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
