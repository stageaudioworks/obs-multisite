// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// api.h — the web UI's half of the appliance, expressed as JSON over HTTP.
//
// Every control the Qt dock offers appears here, plus the system settings a
// box with no keyboard cannot otherwise be given: which display mode to use,
// which sound card, the clock, and the storage credentials themselves.
//
// Two rules run through the whole surface:
//
//   • The answer is immediate. An operator pressing Hold during an event must
//     see the interface acknowledge it at once, not after the next network
//     poll. Every control acts on live state and returns the new status in the
//     same response.
//   • Lock means lock. With the controls locked, anything that would change
//     what is on air is refused — the tablet left on a music stand cannot
//     stop the player by being leant on.
//
#include "player.h"
#include "core/http_server.h"

#include <string>

namespace multisite_player {

// Registers every route on `server`. `config_path` is where edited settings
// are written back to.
void register_api(multisite::HttpServer& server, Player& player,
                  std::string config_path);

// The status document as text — the same object the page polls, for the
// monitoring heartbeat to filter and send.
std::string player_status_json(const Player& player);

} // namespace multisite_player
