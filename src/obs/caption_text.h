// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// caption_text.h — cutting a spoken sentence into captions that fit.
//
// libobs holds one caption in a fixed buffer of CAPTION_LINE_BYTES (128 bytes,
// being 4 * 32 characters) and fills it with a single snprintf:
//
//     snprintf(&next->text[0], CAPTION_LINE_BYTES + 1, "%.*s", (int)bytes, text);
//
// The comment above that call says "split text into 32 character strings". It
// does not split. It TRUNCATES, silently, and the comment has been describing an
// intention rather than the code for a long time. A caption of ordinary spoken
// length — a sermon sentence is easily 150 bytes — loses its back half with no
// warning anywhere.
//
// So the splitting happens here, before the text is handed over. On word
// boundaries, because a caption cut mid-word reads as a fault rather than as a
// line break; and on UTF-8 code point boundaries, because the buffer is a byte
// count and a caption cut through a multi-byte character is a mojibake glyph on
// a campus screen.
//
// Pure string handling with no libobs in it, so tests/test_caption_text.cpp can
// exercise it directly — which is the whole reason it is its own header.
#include <cstddef>
#include <string>
#include <vector>

namespace multisite_obs {

// libobs's own limit, mirrored. If it ever changes there this must follow; it
// is a byte count, not a character count.
static constexpr size_t kCaptionMaxBytes = 128;

// True when `c` is a UTF-8 continuation byte (10xxxxxx) — the middle of a
// character rather than the start of one.
inline bool is_utf8_continuation(unsigned char c) { return (c & 0xC0) == 0x80; }

// The largest cut of `s` starting at `from` that is at most `max_bytes` long,
// ends on a whole character, and prefers to end at a space.
inline size_t caption_cut_point(const std::string& s, size_t from, size_t max_bytes) {
    if (from >= s.size()) return s.size();
    if (s.size() - from <= max_bytes) return s.size();

    size_t end = from + max_bytes;
    // Back off to a character boundary first: a cut inside a multi-byte
    // character is worse than a short caption.
    while (end > from && is_utf8_continuation((unsigned char)s[end]))
        --end;

    // Then back off to the last space, if there is one worth using. "Worth" is
    // the guard against a single enormous word (a URL, a compound name) turning
    // every caption into two characters: if the space is very early, cut at the
    // character boundary instead and let the word break.
    const size_t space = s.find_last_of(' ', end > from ? end - 1 : from);
    if (space != std::string::npos && space > from &&
        (space - from) * 2 >= max_bytes)
        return space;
    return end;
}

// Split a caption into pieces libobs will carry whole. Collapses the run of
// spaces at each break so a caption never begins with one.
//
// An empty or all-space input yields nothing rather than one empty caption:
// there is no such thing as a caption with no words in it, and sending one
// would consume a display slot and blank the screen.
inline std::vector<std::string> split_caption(const std::string& text,
                                              size_t max_bytes = kCaptionMaxBytes) {
    std::vector<std::string> out;
    if (max_bytes == 0) return out;

    size_t i = text.find_first_not_of(' ');
    if (i == std::string::npos) return out;

    while (i < text.size()) {
        const size_t cut = caption_cut_point(text, i, max_bytes);
        size_t end = cut;
        while (end > i && text[end - 1] == ' ') --end;   // no trailing space
        if (end > i) out.push_back(text.substr(i, end - i));
        i = cut;
        while (i < text.size() && text[i] == ' ') ++i;   // no leading space
    }
    return out;
}

} // namespace multisite_obs
