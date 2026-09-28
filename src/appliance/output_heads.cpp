// SPDX-License-Identifier: GPL-3.0-or-later
#include "output_heads.h"

namespace multisite_player {

std::vector<int> assign_crtcs(const std::vector<uint32_t>& possible,
                              const std::vector<int>& current, int crtc_count) {
    const size_t n = possible.size();
    std::vector<int> out(n, -1);
    std::vector<bool> used((size_t)(crtc_count > 0 ? crtc_count : 0), false);
    // First, every head that already has a controller it may use keeps it.
    for (size_t i = 0; i < n; ++i) {
        const int c = i < current.size() ? current[i] : -1;
        if (c >= 0 && c < crtc_count && (possible[i] & (1u << c)) && !used[(size_t)c]) {
            out[i] = c;
            used[(size_t)c] = true;
        }
    }
    // Then the rest, in order, each the lowest free controller it can use.
    for (size_t i = 0; i < n; ++i) {
        if (out[i] >= 0) continue;
        for (int c = 0; c < crtc_count && c < 32; ++c) {
            if ((possible[i] & (1u << c)) && !used[(size_t)c]) {
                out[i] = c;
                used[(size_t)c] = true;
                break;
            }
        }
    }
    return out;
}

std::string output_label(int head, int heads, int tile, const multisite::TileLayout& layout) {
    if (heads < 2) return "";
    std::string s = "OUTPUT " + std::to_string(head + 1) + " OF " + std::to_string(heads);
    if (tile < 0 || !layout.is_split() || tile >= layout.count()) return s + " - WHOLE PICTURE";
    const int col = tile % layout.cols, row = tile / layout.cols;
    std::string where;
    if (layout.rows > 1) where = row == 0 ? "TOP" : row == layout.rows - 1 ? "BOTTOM" : "MIDDLE";
    if (layout.cols > 1) {
        const std::string h = col == 0 ? "LEFT" : col == layout.cols - 1 ? "RIGHT" : "CENTRE";
        where = where.empty() ? h : where + " " + h;
    }
    const bool halves = layout.count() == 2;
    return s + " - " + where + (halves ? " HALF" : "");
}

} // namespace multisite_player
