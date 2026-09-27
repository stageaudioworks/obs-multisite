// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// fit_text.h — shortening one line of text to a width, keeping both ends.
//
// WHY. A value in a dock's Status grid was a plain QLabel, and an unwrapped
// QLabel will not shrink below its whole text. So the longest value set the
// grid column's minimum, and that set the dock's: an AES67 track named after
// its sending machine — "Programme (AES67: Daemon 0a0ac450 Multisite
// rpi5-nathan)" — held the decoder dock wider than the space it had, cutting
// the Storage value off at the edge (seen 2026-09-27). The one guard there was
// set_value(), which cut to a fixed 240 px: arbitrary, and cut even when the
// dock had room.
//
// FitLabel (fit_label.h) calls this with the width it actually has, each time
// it is resized. The middle goes, not the end, because both ends carry meaning
// here: a track's name and the machine it came from, a host and its speed.
//
// Pure C++ with the measurement passed in, so tests/test_fit_text.cpp pins it
// without Qt — and FitLabel passes Qt's real font measurement to this same
// function, so what is tested is what runs.

#include <functional>
#include <string>
#include <vector>

namespace multisite_ui {

// The marker put where the middle was: U+2026 HORIZONTAL ELLIPSIS, in UTF-8.
inline const std::string& fit_ellipsis() {
    static const std::string e = "\xE2\x80\xA6";
    return e;
}

// `text` (UTF-8) as it fits in `max_px`, measured by `width_of`:
//  - it fits: unchanged;
//  - it does not: the longest head + "…" + tail that fits, with the kept code
//    points split as evenly as they will go and the spaces either side of the
//    cut trimmed, so "a b … c" rather than "a b  …  c";
//  - not even "…" fits: empty.
// Never splits a UTF-8 sequence.
inline std::string fit_middle(const std::string& text, int max_px,
                              const std::function<int(const std::string&)>& width_of) {
    if (width_of(text) <= max_px) return text;
    const std::string& dots = fit_ellipsis();
    if (max_px <= 0 || width_of(dots) > max_px) return std::string();

    // Code point boundaries, so a cut can only land between characters.
    std::vector<size_t> starts;
    for (size_t i = 0; i < text.size(); ++i)
        if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) starts.push_back(i);
    const size_t n = starts.size();
    auto head = [&](size_t k) { return text.substr(0, k < n ? starts[k] : text.size()); };
    auto tail = [&](size_t k) { return k == 0 ? std::string() : text.substr(starts[n - k]); };
    auto trim_right = [](std::string s) {
        while (!s.empty() && s.back() == ' ') s.pop_back();
        return s;
    };
    auto trim_left = [](std::string s) {
        size_t i = 0;
        while (i < s.size() && s[i] == ' ') ++i;
        return s.substr(i);
    };
    auto candidate = [&](size_t keep) {
        const size_t h = (keep + 1) / 2;   // the head gets the odd one
        return trim_right(head(h)) + dots + trim_left(tail(keep - h));
    };

    // The most code points kept that still fit. Width grows with what is kept,
    // so this is a search over `keep`, not over every split.
    size_t lo = 0, hi = n > 0 ? n - 1 : 0;   // keep n is the whole text, which did not fit
    while (lo < hi) {
        const size_t mid = (lo + hi + 1) / 2;
        if (width_of(candidate(mid)) <= max_px) lo = mid;
        else hi = mid - 1;
    }
    return candidate(lo);
}

} // namespace multisite_ui
