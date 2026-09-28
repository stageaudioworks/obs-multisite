// SPDX-License-Identifier: GPL-3.0-or-later
// test_splash.cpp — the identity screen on colour bars, until the first
// picture (Config::identity_until_video), and the host's own line under the
// name (Config::splash_label).
//
// Drawn into a Canvas and read back pixel by pixel: no display is needed to
// know whether the bars, the panel and the words are where they should be.
#include "splash.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace multisite_player;

static int g_fail = 0;
#define CHECK(c, m) do { if (!(c)) { std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while (0)

static SplashInfo info() {
    SplashInfo i;
    i.hostname = "rock-5b";
    i.addresses = {"http://192.0.2.24:8080"};
    i.room = "main-auditorium";
    i.state = "WAITING FOR THE MAIN SITE";
    i.version = "0.1.30";
    return i;
}

// SPLASH_DUMP=dir writes each screen drawn here as a PPM, to look at.
static void dump(const Canvas& c, const char* name) {
    const char* dir = std::getenv("SPLASH_DUMP");
    if (!dir) return;
    const std::string path = std::string(dir) + "/" + name + ".ppm";
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", c.width(), c.height());
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x) {
            const uint32_t p = c.pixel(x, y);
            const unsigned char rgb[3] = {(unsigned char)(p >> 16), (unsigned char)(p >> 8), (unsigned char)p};
            std::fwrite(rgb, 1, 3, f);
        }
    std::fclose(f);
}

static int differing(const Canvas& a, const Canvas& b, int x0, int y0, int x1, int y1) {
    int n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            if (a.pixel(x, y) != b.pixel(x, y)) ++n;
    return n;
}

int main() {
    const int W = 1920, H = 1080;
    std::printf("== Before the first picture: the identity screen on colour bars ==\n");
    SplashInfo bars = info();
    bars.test_pattern = true;
    Canvas c(W, H);
    render_splash(c, bars);
    CHECK((c.pixel(5, 5) & 0xffffff) == 0xbfbfbf, "75% white bar at the top left");
    CHECK((c.pixel(W - 5, 5) & 0xffffff) == 0x0000bf, "75% blue bar at the top right");
    CHECK((c.pixel(W / 2 + 10, H - 5) & 0xffffff) == 0x131313 || (c.pixel(W / 2 + 10, H - 5) & 0xffffff) == 0x090909,
          "the bottom row below the panel");
    const int X0 = W * 3 / 100, Y1 = H - H * 12 / 100;
    CHECK((c.pixel(X0 + 2, Y1 - 2) & 0xffffff) == 0x121519, "a dark panel across the middle");
    CHECK((c.pixel(X0 - 1, H / 2) & 0xffffff) == 0xc8ccd4, "with a light border");

    std::printf("\n== After it: the plain identity screen, as before ==\n");
    Canvas plain(W, H);
    render_splash(plain, info());
    dump(plain, "splash-plain");
    CHECK((plain.pixel(5, 5) & 0xffffff) != 0xbfbfbf, "no bars");
    CHECK(differing(c, plain, 0, 0, W, H / 10) > W * H / 20, "the two screens differ where the bars are");

    std::printf("\n== The host's line under the name ==\n");
    SplashInfo labelled = bars;
    labelled.label = "DECODER - MULTISITEOS 0.2.36";
    Canvas l(W, H);
    render_splash(l, labelled);
    dump(l, "splash-bars-labelled");
    CHECK(differing(l, c, X0, H * 12 / 100, W - X0, H - H * 12 / 100) > 200, "it is drawn on the panel");
    CHECK((l.pixel(5, 5) & 0xffffff) == 0xbfbfbf, "and the bars are still there");
    SplashInfo long_label = labelled;
    long_label.label = std::string(400, 'W');
    Canvas ll(W, H);
    render_splash(ll, long_label);   // must not write outside the canvas or crash
    CHECK((ll.pixel(5, 5) & 0xffffff) == 0xbfbfbf, "a label too long for the screen does not break it");

    std::printf("\n%s\n", g_fail == 0 ? "ALL SPLASH TESTS PASSED" : "SOME SPLASH TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
