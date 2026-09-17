// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage_health.h"

#include <cctype>
#include <cstring>

namespace multisite {

namespace {

std::string trim(const std::string& v) {
    const char* ws = " \t\r\n";
    const size_t a = v.find_first_not_of(ws);
    if (a == std::string::npos) return "";
    const size_t b = v.find_last_not_of(ws);
    return v.substr(a, b - a + 1);
}

char upper(char c) {
    return (char)std::toupper((unsigned char)c);
}

} // namespace

bool header_is(const std::string& line, const char* name, std::string& value) {
    const size_t n = std::strlen(name);
    if (line.size() <= n) return false;
    for (size_t i = 0; i < n; ++i)
        if (upper(line[i]) != upper(name[i])) return false;
    // The name must be followed by the colon, or "cf-ray-thing:" would match
    // a request for "cf-ray".
    size_t i = n;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i >= line.size() || line[i] != ':') return false;
    value = trim(line.substr(i + 1));
    return true;
}

std::string cloudflare_colo(const std::string& cf_ray) {
    const std::string v = trim(cf_ray);
    const size_t dash = v.rfind('-');
    if (dash == std::string::npos || dash + 1 >= v.size()) return "";
    std::string tail = v.substr(dash + 1);
    // Colo codes are three letters, occasionally four. Anything else is not a
    // colo — including the plain request id a non-Cloudflare store might send,
    // and Cloudflare's own "-FRA-METAL" style suffixes we should not guess at.
    if (tail.size() < 3 || tail.size() > 4) return "";
    for (char& c : tail) {
        if (!std::isalpha((unsigned char)c)) return "";
        c = upper(c);
    }
    return tail;
}

void RateMeter::add(uint64_t bytes, double seconds) {
    // 64 kB and 50 ms: below either, the measurement is dominated by latency
    // and connection setup rather than the link's actual capacity.
    if (bytes < uint64_t{64} * 1024 || seconds < 0.05) return;
    const double rate = (double)bytes / seconds;
    if (m_samples == 0) m_rate = rate;
    else                m_rate = m_alpha * rate + (1.0 - m_alpha) * m_rate;
    ++m_samples;
}

} // namespace multisite
