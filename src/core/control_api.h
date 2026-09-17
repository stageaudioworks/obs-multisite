// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// control_api.h — the one list of command names every control surface speaks.
//
// §8.3 of the scope promises "one surface, two transports": the plugin's HTTP
// pages and its obs-websocket vendor requests are the same commands with the
// same payloads, so a control written against one works against the other. That
// promise is only true while the names live in exactly one place.
//
// They live here, in the portable core, because this is the only layer both
// transports — and a core test, which has no libobs — can reach. A command is
// named as the tail of its HTTP path: "decoder/play" is served at
// /api/decoder/play and is the obs-websocket vendor request type
// "decoder/play". One string, two ways in.
//
#include <string>

namespace multisite {

// The encoder's commands: what a main campus can be told to do.
inline constexpr const char* kEncoderCommands[] = {
    "encoder/status",
    "encoder/go-live",
    "encoder/end",
    "encoder/marker",
    "encoder/settings",
};

// The decoder's commands: what a satellite can be told to do. These are the
// same actions the campus player exposes on its own routes (/api/play and the
// rest), carrying the plugin's namespace so both halves can sit under one
// vendor without colliding on "marker".
inline constexpr const char* kDecoderCommands[] = {
    "decoder/status",
    "decoder/play",
    "decoder/stop",
    "decoder/hold",
    "decoder/continue",
    "decoder/catch-up",
    "decoder/jog",
    "decoder/seek",
    "decoder/delay",
    "decoder/marker",
    "decoder/cue",
    "decoder/events",
    "decoder/events/refresh",
    "decoder/load-event",
    "decoder/return-to-live",
    "decoder/settings",
};

// The HTTP path for a command: one leading "/api/" and no doubling, whatever
// the caller passes.
inline std::string api_path(const std::string& command) {
    return command.empty() ? std::string("/api/")
                           : std::string("/api/") + command;
}

} // namespace multisite
