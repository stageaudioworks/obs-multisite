// SPDX-License-Identifier: GPL-3.0-or-later
#include "splash.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cctype>
#include <cstring>

#ifdef MULTISITE_HAVE_QRCODE
// The QR tile on the identity screen. libqrencode is a system package, found
// by the build rather than shipped; without it the splash compiles and simply
// has no code on it. The installer and CI install the library, which is where
// a Pi's build gets it.
extern "C" {
#include <qrencode.h>
}
#endif

#ifdef MULTISITE_HAVE_FREETYPE
// Smooth text on the identity screen. Like libqrencode, found by the build
// rather than shipped; without it the splash falls back to its bitmap font.
#include <ft2build.h>
#include FT_FREETYPE_H
#endif

namespace multisite_player {

namespace {

// A 5x7 font, written out so it can be read and checked rather than trusted.
// Capitals, digits and the punctuation an address needs — nothing else, which
// is why the splash speaks in capitals.
struct Glyph { char c; const char* rows[7]; };

const Glyph kFont[] = {
{' ', {".....", ".....", ".....", ".....", ".....", ".....", "....."}},
{'A', {".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
{'B', {"####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."}},
{'C', {".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."}},
{'D', {"####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."}},
{'E', {"#####", "#....", "#....", "####.", "#....", "#....", "#####"}},
{'F', {"#####", "#....", "#....", "####.", "#....", "#....", "#...."}},
{'G', {".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".###."}},
{'H', {"#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
{'I', {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "#####"}},
{'J', {"....#", "....#", "....#", "....#", "#...#", "#...#", ".###."}},
{'K', {"#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"}},
{'L', {"#....", "#....", "#....", "#....", "#....", "#....", "#####"}},
{'M', {"#...#", "##.##", "#.#.#", "#...#", "#...#", "#...#", "#...#"}},
{'N', {"#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "#...#"}},
{'O', {".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
{'P', {"####.", "#...#", "#...#", "####.", "#....", "#....", "#...."}},
{'Q', {".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"}},
{'R', {"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"}},
{'S', {".####", "#....", "#....", ".###.", "....#", "....#", "####."}},
{'T', {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."}},
{'U', {"#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
{'V', {"#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."}},
{'W', {"#...#", "#...#", "#...#", "#...#", "#.#.#", "##.##", "#...#"}},
{'X', {"#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"}},
{'Y', {"#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."}},
{'Z', {"#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"}},
{'0', {".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."}},
{'1', {"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."}},
{'2', {".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"}},
{'3', {"#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###."}},
{'4', {"...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."}},
{'5', {"#####", "#....", "####.", "....#", "....#", "#...#", ".###."}},
{'6', {"..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###."}},
{'7', {"#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."}},
{'8', {".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."}},
{'9', {".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.."}},
{'.', {".....", ".....", ".....", ".....", ".....", ".##..", ".##.."}},
{',', {".....", ".....", ".....", ".....", ".##..", ".##..", ".#..."}},
{':', {".....", ".##..", ".##..", ".....", ".##..", ".##..", "....."}},
{'-', {".....", ".....", ".....", "#####", ".....", ".....", "....."}},
{'/', {"....#", "....#", "...#.", "..#..", ".#...", "#....", "#...."}},
{'(', {"..##.", ".#...", "#....", "#....", "#....", ".#...", "..##."}},
{')', {".##..", "...#.", "....#", "....#", "....#", "...#.", ".##.."}},
{'\'', {".##..", ".##..", ".#...", ".....", ".....", ".....", "....."}},
{'?', {".###.", "#...#", "....#", "...#.", "..#..", ".....", "..#.."}},
{'!', {"..#..", "..#..", "..#..", "..#..", "..#..", ".....", "..#.."}},
{'+', {".....", "..#..", "..#..", "#####", "..#..", "..#..", "....."}},
{'=', {".....", ".....", "#####", ".....", "#####", ".....", "....."}},
{'_', {".....", ".....", ".....", ".....", ".....", ".....", "#####"}},
{'@', {".###.", "#...#", "#.###", "#.#.#", "#.###", "#....", ".###."}},
{'%', {"#...#", "#..#.", "...#.", "..#..", ".#...", ".#..#", "#...#"}},
{'#', {".#.#.", ".#.#.", "#####", ".#.#.", "#####", ".#.#.", ".#.#."}},
};

const Glyph* glyph_for(char c) {
    const char up = (char)std::toupper((unsigned char)c);
    for (const auto& g : kFont) if (g.c == up) return &g;
    return nullptr;
}

// One column of spacing between characters, scaled with the text.
constexpr int kAdvance = 6;      // 5 wide + 1 gap

// A dark ground rather than black: a screen showing pure black in a lit room
// looks broken, and a campus wants to be able to tell "waiting" from "off".
constexpr uint32_t kBg      = 0x00121519;
constexpr uint32_t kBgTop   = 0x001a2732;  // identity screen gradient top
constexpr uint32_t kText    = 0x00e8edf4;
constexpr uint32_t kDim     = 0x0093a1b5;
constexpr uint32_t kAccent  = 0x004a9de0;
constexpr uint32_t kWarn    = 0x00c8871d;
constexpr uint32_t kRule    = 0x002e3644;
#ifdef MULTISITE_HAVE_QRCODE
constexpr uint32_t kCard    = 0x00ffffff;  // ground the QR tile is scanned against
constexpr uint32_t kShadow  = 0x001d242e;  // flat offset "shadow" under the tile
constexpr uint32_t kQrDark  = 0x00000000;  // the modules themselves
#endif

} // namespace

Canvas::Canvas(int width, int height)
    : m_width(std::max(1, width)), m_height(std::max(1, height)),
      m_px((size_t)m_width * (size_t)m_height, 0) {}

void Canvas::fill(uint32_t bgrx) {
    std::fill(m_px.begin(), m_px.end(), bgrx);
}

void Canvas::rect(int x, int y, int w, int h, uint32_t bgrx) {
    const int x0 = std::max(0, x), y0 = std::max(0, y);
    const int x1 = std::min(m_width, x + w), y1 = std::min(m_height, y + h);
    // Wholly off the canvas: nothing to fill. Without this, a rectangle past
    // the left edge gave x1 < x0, and std::fill a backwards range.
    if (x1 <= x0 || y1 <= y0) return;
    for (int row = y0; row < y1; ++row)
        std::fill(m_px.begin() + (size_t)row * m_width + x0,
                  m_px.begin() + (size_t)row * m_width + x1, bgrx);
}

uint32_t Canvas::pixel(int x, int y) const {
    if (x < 0 || y < 0 || x >= m_width || y >= m_height) return 0;
    return m_px[(size_t)y * m_width + x];
}

void Canvas::blend(int x, int y, uint32_t rgb, uint8_t alpha) {
    if (x < 0 || y < 0 || x >= m_width || y >= m_height || alpha == 0) return;
    if (alpha == 255) { rect(x, y, 1, 1, rgb); return; }
    const uint32_t back = m_px[(size_t)y * m_width + x];
    const uint8_t fr = (rgb >> 16) & 0xff, fg = (rgb >> 8) & 0xff, fb = rgb & 0xff;
    const uint8_t br = (back >> 16) & 0xff, bg = (back >> 8) & 0xff, bb = back & 0xff;
    const uint32_t r = ((uint32_t)fr * alpha + (uint32_t)br * (255 - alpha)) / 255;
    const uint32_t g = ((uint32_t)fg * alpha + (uint32_t)bg * (255 - alpha)) / 255;
    const uint32_t b = ((uint32_t)fb * alpha + (uint32_t)bb * (255 - alpha)) / 255;
    m_px[(size_t)y * m_width + x] = (r << 16) | (g << 8) | b;
}


int Canvas::text_width(const std::string& s, int scale) {
    if (s.empty()) return 0;
    return (int)s.size() * kAdvance * scale - scale;
}

void Canvas::text(int x, int y, const std::string& s, int scale,
                  uint32_t bgrx) {
    if (scale < 1) scale = 1;
    int pen = x;
    for (char c : s) {
        const Glyph* g = glyph_for(c);
        if (g) {
            for (int row = 0; row < 7; ++row) {
                const char* bits = g->rows[row];
                for (int col = 0; col < 5; ++col) {
                    if (bits[col] != '#') continue;
                    rect(pen + col * scale, y + row * scale, scale, scale, bgrx);
                }
            }
        }
        pen += kAdvance * scale;
    }
}

void Canvas::text_centred(int y, const std::string& s, int scale,
                          uint32_t bgrx) {
    text((m_width - text_width(s, scale)) / 2, y, s, scale, bgrx);
}

#ifdef MULTISITE_HAVE_QRCODE

namespace {

// A QR tile for the primary address, built once per redraw. libqrencode keeps
// one byte per module; bit 0 set means a dark module.
struct QrBlock {
    QRcode* code = nullptr;
    int modules  = 0;
    int scale    = 2;
    // The tile includes a 4-module quiet zone on every side, which scanners
    // need and which would otherwise have to be drawn as extra white.
    int side() const { return (modules + 8) * scale; }
};

bool build_qr(const std::string& url, int max_side, QrBlock& out) {
    QRcode* code = QRcode_encodeString(url.c_str(), 0, QR_ECLEVEL_M,
                                       QR_MODE_8, 1);
    if (!code) return false;
    out.code = code;
    out.modules = code->width;
    out.scale = std::max(2, max_side / (out.modules + 8));
    return true;
}

void draw_qr(Canvas& canvas, int x, int y, const QrBlock& b) {
    const int side = b.side();
    // A flat offset block behind the tile reads as a shadow using the pixel
    // renderer's whole vocabulary: rectangles, no blending.
    const int off = std::max(2, b.scale);
    canvas.rect(x + off, y + off, side, side, kShadow);
    canvas.rect(x, y, side, side, kCard);
    const int q = 4 * b.scale;
    for (int row = 0; row < b.modules; ++row) {
        const unsigned char* line = b.code->data + (size_t)row * b.modules;
        for (int col = 0; col < b.modules; ++col) {
            if (line[col] & 1)
                canvas.rect(x + q + col * b.scale, y + q + row * b.scale,
                            b.scale, b.scale, kQrDark);
        }
    }
}

} // namespace

#endif // MULTISITE_HAVE_QRCODE

namespace {

// A vertical gradient under the identity screen: flat navy reads as a dead
// monitor, a gentle lift reads as deliberate.
// SMPTE-style colour bars at 75%, their castellations and the bottom row:
// the screen says at a glance that the output works and its colours are right.
void fill_bars(Canvas& canvas) {
    static const uint32_t top[7]    = {0xbfbfbf, 0xbfbf00, 0x00bfbf, 0x00bf00, 0xbf00bf, 0xbf0000, 0x0000bf};
    static const uint32_t middle[7] = {0x0000bf, 0x131313, 0xbf00bf, 0x131313, 0x00bfbf, 0x131313, 0xbfbfbf};
    static const uint32_t bottom[7] = {0x00214c, 0xebebeb, 0x32006a, 0x131313, 0x090909, 0x131313, 0x1d1d1d};
    const int w = canvas.width(), h = canvas.height();
    const int rows[4] = {0, h * 2 / 3, h * 3 / 4, h};
    const uint32_t* bands[3] = {top, middle, bottom};
    for (int b = 0; b < 3; ++b)
        for (int i = 0; i < 7; ++i)
            canvas.rect(w * i / 7, rows[b], w * (i + 1) / 7 - w * i / 7, rows[b + 1] - rows[b], bands[b][i]);
}

void fill_gradient(Canvas& canvas, uint32_t top, uint32_t bottom) {
    const int w = canvas.width(), h = canvas.height();
    const int tr = (top >> 16) & 0xff, tg = (top >> 8) & 0xff, tb = top & 0xff;
    const int br = (bottom >> 16) & 0xff, bg = (bottom >> 8) & 0xff, bb = bottom & 0xff;
    for (int y = 0; y < h; ++y) {
        const int t = h > 1 ? (y * 256) / (h - 1) : 0;
        const uint32_t c = 0x00000000u
            | ((uint32_t)((tr * (256 - t) + br * t) / 256) << 16)
            | ((uint32_t)((tg * (256 - t) + bg * t) / 256) << 8)
            |  (uint32_t)((tb * (256 - t) + bb * t) / 256);
        canvas.rect(0, y, w, 1, c);
    }
}

#ifdef MULTISITE_HAVE_FREETYPE
// Text drawn with the system font through FreeType: genuinely smooth letters
// rather than the 5x7 grid the bitmap fallback uses.
class Font {
public:
    Font() {
        if (FT_Init_FreeType(&m_lib)) return;
        for (const char* path : kCandidates) {
            if (FT_New_Face(m_lib, path, 0, &m_face) == 0) { m_ok = true; return; }
        }
    }
    bool ok() const { return m_ok && m_face; }

    // Set the em size for `scale` and return the ascent in pixels (the baseline
    // sits this far below the top of the text box).
    int set_size(int scale) {
        FT_Set_Pixel_Sizes(m_face, 0, std::max(7, scale * 7));
        return (int)(m_face->size->metrics.ascender >> 6);
    }
    int width(const std::string& s) {
        int pen = 0;
        for (unsigned char c : s) {
            FT_UInt gi = FT_Get_Char_Index(m_face, c);
            if (gi == 0) { pen += space(); continue; }
            if (FT_Load_Glyph(m_face, gi, FT_LOAD_DEFAULT) == 0)
                pen += (int)(m_face->glyph->advance.x >> 6);
        }
        return pen;
    }
    void draw(Canvas& canvas, int x, int baseline, const std::string& s,
              uint32_t colour) {
        int pen = x;
        for (unsigned char c : s) {
            FT_UInt gi = FT_Get_Char_Index(m_face, c);
            if (gi == 0) { pen += space(); continue; }
            if (FT_Load_Glyph(m_face, gi, FT_LOAD_RENDER)) continue;
            FT_GlyphSlot g = m_face->glyph;
            const FT_Bitmap& bmp = g->bitmap;
            for (unsigned row = 0; row < bmp.rows; ++row) {
                const unsigned char* line = bmp.buffer + (size_t)row * bmp.pitch;
                for (unsigned col = 0; col < bmp.width; ++col) {
                    const uint8_t a = line[col];
                    if (a) canvas.blend(pen + g->bitmap_left + (int)col,
                                        baseline - g->bitmap_top + (int)row,
                                        colour, a);
                }
            }
            pen += (int)(g->advance.x >> 6);
        }
    }

private:
    int space() {
        FT_UInt gi = FT_Get_Char_Index(m_face, ' ');
        if (gi && FT_Load_Glyph(m_face, gi, FT_LOAD_DEFAULT) == 0)
            return (int)(m_face->glyph->advance.x >> 6);
        return (int)(m_face->size->metrics.x_ppem) / 2;
    }
    static constexpr const char* kCandidates[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    };
    FT_Library m_lib = nullptr;
    FT_Face    m_face = nullptr;
    bool       m_ok = false;

public:
    ~Font() {
        if (m_face) FT_Done_Face(m_face);
        if (m_lib)  FT_Done_FreeType(m_lib);
    }
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;
};
#endif

// The single seam all splash text flows through: smooth when freetype and a
// font are available, the bitmap grid otherwise. Line spacing still comes from
// Canvas::text_height, so the two renderings lay out the same.
class Text {
public:
    int width(const std::string& s, int scale) {
#ifdef MULTISITE_HAVE_FREETYPE
        if (m_font.ok()) { m_font.set_size(scale); return m_font.width(s); }
#endif
        return Canvas::text_width(s, scale);
    }
    int ascent(int scale) {
#ifdef MULTISITE_HAVE_FREETYPE
        if (m_font.ok()) return m_font.set_size(scale);
#endif
        (void)scale;
        return 0;
    }
    void draw(Canvas& c, int x, int top, const std::string& s, int scale,
              uint32_t colour) {
#ifdef MULTISITE_HAVE_FREETYPE
        if (m_font.ok()) {
            const int a = m_font.set_size(scale);
            m_font.draw(c, x, top + a, s, colour);
            return;
        }
#endif
        c.text(x, top, s, scale, colour);
    }

private:
#ifdef MULTISITE_HAVE_FREETYPE
    Font m_font;
#endif
};

} // namespace

void render_splash(Canvas& canvas, const SplashInfo& info) {
    const int W = canvas.width(), H = canvas.height();
    // Scale everything off the display height, so the same layout works on a
    // 720p monitor in an office and a 4K screen in an auditorium.
    const int unit  = std::max(1, H / 180);         // 6 at 1080p

    // Where the layout goes: the whole screen, or with the test pattern a
    // dark panel across the colour bars, which stay in sight above and below.
    int X0 = 0, Y0 = 0, X1 = W, Y1 = H;
    if (info.test_pattern) {
        fill_bars(canvas);
        X0 = W * 3 / 100; X1 = W - X0; Y0 = H * 12 / 100; Y1 = H - Y0;
        const int border = std::max(2, unit / 2);
        canvas.rect(X0 - border, Y0 - border, X1 - X0 + 2 * border, Y1 - Y0 + 2 * border, 0x00c8ccd4);
        canvas.rect(X0, Y0, X1 - X0, Y1 - Y0, kBg);
    } else {
        fill_gradient(canvas, kBgTop, kBg);
    }
    const int big   = unit * 2;
    const int mid   = unit;
    const int small = std::max(1, unit * 2 / 3);
    // Built once, not once per redraw. render_splash is only ever called from
    // Player::update_screen() on the poll thread, so this is safe; a fresh Text
    // here re-ran FT_Init_FreeType and FT_New_Face — opening and parsing a font
    // file off disk — on every idle-screen redraw for as long as the box runs.
    static Text tx;

    // Header: what the box is, up top, with a hairline under it. Everything a
    // person standing in the room needs is below that, in one glance.
    const std::string brand = "MULTISITE PLAYER";
    tx.draw(canvas, X0 + unit * 6, Y0 + unit * 4, brand, small, kDim);
    if (!info.version.empty()) {
        const std::string v = "V" + info.version;
        tx.draw(canvas, X1 - unit * 6 - tx.width(v, small), Y0 + unit * 4, v, small,
                kDim);
    }
    canvas.rect(X0 + unit * 6, Y0 + unit * 11, X1 - X0 - unit * 12, std::max(1, unit / 3),
                kRule);

    const int top   = Y0 + unit * 16;  // below the header
    const int floor = Y1 - unit * 6;   // bottom margin

    // The QR tile. One box is enough: whatever the room needs to reach, the
    // web page on this box is where it starts.
    bool have_qr = false;
    int  qr_x = 0, qr_y = 0, tile = 0;
#ifdef MULTISITE_HAVE_QRCODE
    QrBlock block;
    if (!info.addresses.empty()) {
        const int avail = std::min(((X1 - X0) * 3) / 10, floor - top);
        if (build_qr(info.addresses.front(), std::max(unit * 12, avail),
                     block)) {
            have_qr = true;
            tile = block.side();
            qr_x = X1 - unit * 6 - tile;
            const int caption = Canvas::text_height(small) + unit * 2;
            qr_y = top + std::max(0, (floor - top - tile - caption) / 2);
        }
    }
#else
    (void)qr_x;
    (void)qr_y;
    (void)tile;
#endif

    // The words occupy the space left of the tile, centred in it — or the whole
    // width, when there is nothing to scan.
    const int text_right = have_qr ? qr_x - unit * 4 : X1 - unit * 6;
    const int cx         = (X0 + unit * 6 + text_right) / 2;
    auto centred = [&](int y, const std::string& s, int scale, uint32_t c) {
        tx.draw(canvas, cx - tx.width(s, scale) / 2, y, s, scale, c);
    };

    // The message block is built before it is drawn so its true height is
    // known and it can sit centred in the space it has been given.
    struct Line { std::string text; int scale; uint32_t colour; int gap_below; };
    std::vector<Line> lines;

    // A long hostname must not push the title across the room; trim it to the
    // column rather than letting it collide with the tile.
    std::string title = info.hostname.empty() ? "CAMPUS PLAYER"
                                              : info.hostname;
    const int title_max = std::max(1, text_right - X0 - unit * 12);
    while (!title.empty() && tx.width(title, big) > title_max)
        title.pop_back();
    lines.push_back({title, big, kText, info.label.empty() ? unit * 4 : unit * 2});
    // What the box is, in its host's words, right under its name.
    if (!info.label.empty()) lines.push_back({info.label, small, kDim, unit * 4});
    // And which screen this is, when there are several.
    if (!info.output_label.empty()) lines.push_back({info.output_label, mid, kAccent, unit * 4});
    lines.push_back({"", 0, kRule, unit * 4});       // the rule

    if (!info.configured)
        lines.push_back({"THIS BOX HAS NO STORAGE DETAILS YET", mid, kWarn,
                         unit * 4});

    if (info.addresses.empty()) {
        lines.push_back({"NO NETWORK CONNECTION", mid, kWarn, unit * 3});
        lines.push_back({"PLUG IN AN ETHERNET CABLE", small, kDim, unit * 4});
    } else {
        if (have_qr) {
            lines.push_back({"SCAN THE CODE ON THE RIGHT", small, kDim,
                             unit * 2});
            lines.push_back({"OR TYPE ONE OF THESE", small, kDim, unit * 3});
        } else {
            lines.push_back({"OPEN ONE OF THESE ON A PHONE OR TABLET", small,
                             kDim, unit * 3});
        }
        for (const auto& a : info.addresses)
            lines.push_back({a, mid, kAccent, unit * 2});
        lines.back().gap_below = unit * 5;
    }

    // Remote access, when the box has been put on a network it can be reached
    // on from outside the building. It is labelled in full and kept apart from
    // the addresses above: those are typed into a phone standing in the room,
    // this one is not, and confusing the two is the mistake worth designing
    // out. With no remote access there is simply nothing here.
    if (!info.remote_ip.empty()) {
        lines.push_back({"REMOTE ACCESS IP", small, kDim, unit * 1});
        lines.push_back({info.remote_ip, mid, kAccent, unit * 5});
    }

    if (info.configured && !info.room.empty())
        lines.push_back({"ROOM  " + info.room, small, kDim, unit * 3});

    if (!info.state.empty())
        lines.push_back({info.state, small, kText, unit * 2});
    if (!info.detail.empty())
        lines.push_back({info.detail, small, kDim, unit * 2});

    int total = 0;
    for (const auto& l : lines)
        total += (l.scale ? Canvas::text_height(l.scale)
                          : std::max(1, unit / 3)) + l.gap_below;
    total -= lines.empty() ? 0 : lines.back().gap_below;

    const int y0 = top + std::max(0, (floor - top - total) / 2);
    int y = y0;
    for (const auto& l : lines) {
        if (l.scale == 0) {
            const int thickness = std::max(1, unit / 3);
            canvas.rect(X0 + unit * 6, y, std::max(1, text_right - X0 - unit * 6),
                        thickness, l.colour);
            y += thickness + l.gap_below;
            continue;
        }
        centred(y, l.text, l.scale, l.colour);
        y += Canvas::text_height(l.scale) + l.gap_below;
    }

#ifdef MULTISITE_HAVE_QRCODE
    if (have_qr) {
        draw_qr(canvas, qr_x, qr_y, block);
        QRcode_free(block.code);
        const std::string cap = "SCAN WITH YOUR PHONE";
        tx.draw(canvas, qr_x + tile / 2 - tx.width(cap, small) / 2,
                qr_y + tile + unit * 2, cap, small, kDim);
    }
#endif
}

bool load_still(const std::string& path, int width, int height, Canvas& out,
                std::string& error) {
    error.clear();
    if (path.empty()) { error = "no holding slide has been set"; return false; }

    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
        error = "cannot open " + path;
        return false;
    }
    struct Closer {
        AVFormatContext** f;
        ~Closer() { if (*f) avformat_close_input(f); }
    } closer{&fmt};

    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        error = path + " is not a picture this box can read";
        return false;
    }
    int stream = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            stream = (int)i;
            break;
        }
    }
    if (stream < 0) { error = path + " holds no picture"; return false; }

    AVCodecParameters* par = fmt->streams[stream]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    if (!codec) { error = "this build cannot read that picture format"; return false; }

    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx) { error = "out of memory"; return false; }
    struct CtxCloser {
        AVCodecContext** c;
        ~CtxCloser() { if (*c) avcodec_free_context(c); }
    } ctx_closer{&ctx};

    if (avcodec_parameters_to_context(ctx, par) < 0 ||
        avcodec_open2(ctx, codec, nullptr) < 0) {
        error = "could not decode " + path;
        return false;
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame*  frame = av_frame_alloc();
    struct FrameCloser {
        AVPacket** p; AVFrame** f;
        ~FrameCloser() { if (*p) av_packet_free(p); if (*f) av_frame_free(f); }
    } frame_closer{&pkt, &frame};
    if (!pkt || !frame) { error = "out of memory"; return false; }

    bool got = false;
    while (!got && av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == stream &&
            avcodec_send_packet(ctx, pkt) >= 0 &&
            avcodec_receive_frame(ctx, frame) >= 0)
            got = true;
        av_packet_unref(pkt);
    }
    if (!got) { error = "nothing could be decoded from " + path; return false; }

    // Fit inside the screen without distorting it: a slide stretched to a
    // shape it was not made in looks worse than one with a border.
    const double sx = (double)width / frame->width;
    const double sy = (double)height / frame->height;
    const double scale = std::min(sx, sy);
    int dw = std::max(2, (int)(frame->width * scale)) & ~1;
    int dh = std::max(2, (int)(frame->height * scale)) & ~1;

    std::vector<uint8_t> rgb((size_t)dw * dh * 4);
    SwsContext* sws = sws_getContext(frame->width, frame->height,
                                     (AVPixelFormat)frame->format, dw, dh,
                                     AV_PIX_FMT_BGRA, SWS_BILINEAR,
                                     nullptr, nullptr, nullptr);
    if (!sws) { error = "could not scale the holding slide"; return false; }
    uint8_t* dst[4] = { rgb.data(), nullptr, nullptr, nullptr };
    int dst_stride[4] = { dw * 4, 0, 0, 0 };
    sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dst,
              dst_stride);
    sws_freeContext(sws);

    out = Canvas(width, height);
    out.fill(0x00000000);
    const int ox = (width - dw) / 2, oy = (height - dh) / 2;
    for (int row = 0; row < dh; ++row) {
        const uint8_t* src = rgb.data() + (size_t)row * dw * 4;
        for (int col = 0; col < dw; ++col) {
            const uint32_t px = (uint32_t)src[col * 4 + 0]
                              | ((uint32_t)src[col * 4 + 1] << 8)
                              | ((uint32_t)src[col * 4 + 2] << 16);
            out.rect(ox + col, oy + row, 1, 1, px);
        }
    }
    return true;
}

} // namespace multisite_player
