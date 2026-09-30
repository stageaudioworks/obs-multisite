// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// web_ui.h — remote control of this OBS machine, from a phone or a tablet.
//
// The docks are the interface for whoever is sitting at the desk. This is the
// interface for everybody else: the person in the foyer who has to press a
// marker from the back of the room, the volunteer who wants to know whether the
// campus is actually playing, the one who cannot see the desk screen from where
// they are standing. It is the same design as the campus player's own web UI —
// one page, polled twice a second, in the language of an event rather than of a
// video pipeline — because somebody who has learned one should not have to
// learn the other.
//
// Which half is served follows the machine's role, exactly as the docks do: a
// main site gets the encoder page and no decoder routes at all, a satellite the
// other way round, and a machine set to Both gets a page linking the two. That
// is deliberate. Hiding controls nobody on this machine can use is what keeps a
// volunteer one wrong click away from nothing.
//
// Trust model, deliberately the same as the player's: no login and no TLS, and
// the building's own network is the guard. It binds every interface by default
// because a page that only answers localhost cannot be reached from the tablet
// it exists for.
//
#include <string>

namespace multisite_obs {

struct WebUiSettings {
    bool        enabled = true;
    int         port    = 8080;   // the same port the campus player uses
    std::string bind    = "0.0.0.0";

    void load();
    void save() const;
};

WebUiSettings& web_ui_settings();
void set_web_ui_settings(const WebUiSettings& s);

// Called from obs_module_load and obs_module_unload. Never fatal to OBS: a port
// that is already taken means no remote control, which is worth saying loudly
// in the log and on the dock, and nothing more.
void start_web_ui();

// Stops the server so it can be started again — what the dock asks for when the
// port or the on/off switch changes. Deliberately NOT the same thing as shutting
// down: restarting must leave the controls working.
void stop_web_ui();

// Stops the server because this module is going away, and refuses anything that
// arrives afterwards. Only obs_module_unload calls this: a control accepted
// while OBS is tearing the module down would be applied to something already
// gone.
void shut_down_web_ui();

// True once a stop has begun. A handler checks this before touching anything
// OBS owns: while the module is unloading, a control that is accepted now would
// be a control applied to an object that is going away.
bool web_ui_stopping();

// "http://192.168.1.20:8080" — an address somebody can type from another
// device. Falls back to the port alone when no LAN address can be found, which
// is better than showing an address that does not work.
std::string web_ui_address();

// Why it is not serving, when it is not. Empty when it is.
std::string web_ui_problem();

// One switch for the whole remote surface, matching the player's Lock: with it
// on, anything that would change what is on air is refused. The docks and the
// hotkeys keep working — this guards the tablet left on a music stand, not the
// desk somebody is actually sitting at.
bool web_ui_locked();
void web_ui_set_locked(bool locked);

} // namespace multisite_obs