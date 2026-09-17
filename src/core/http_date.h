// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// http_date.h — reading an HTTP Date header, so a box can check its own clock
// against the store's.
//
// The bucket answers every request with a Date header set by the storage
// provider's own NTP-disciplined servers (Cloudflare, AWS, Backblaze). That is
// a second opinion on the time of day with no NTP client, no extra socket and
// no privilege — the connection is one the transport is already making. A
// seconds-level difference is all a shared cue list needs to know about.
//
#include <cstdint>
#include <string>

namespace multisite {

// Parse an RFC 7231 IMF-fixdate — "Sun, 06 Nov 1994 08:49:37 GMT" — into Unix
// milliseconds. Returns false when the string is not that shape.
//
// Hand-rolled and portable on purpose: the value is only ever used for a
// seconds-level clock check, and timegm()/strptime() are not portable enough
// (nor strand-initialisation-safe enough) to lean on for it.
bool parse_http_date_ms(const std::string& s, int64_t& out_ms);

} // namespace multisite
