// SPDX-License-Identifier: GPL-3.0-or-later
#include <obs-module.h>
#include "plugin_log.h"
#include "../core/log.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-multisite", "en-US")

namespace multisite_obs {
void register_output();
void register_source();
void register_ui();
void unregister_ui();
void start_web_ui();
void stop_web_ui();
void shut_down_web_ui();
void register_vendor_api();
void unregister_vendor_api();
void reporter_start();
void reporter_stop();
#ifdef MULTISITE_HAVE_QT
void register_docks();
#endif
}

MODULE_EXPORT const char* obs_module_description(void) {
    return "Multisite: reliable store-and-forward video contribution over "
           "S3-compatible storage.";
}
MODULE_EXPORT const char* obs_module_name(void) { return "Multisite"; }

bool obs_module_load(void) {
    // The core can talk now. It does all of this project's uploading and had no
    // way to say anything at all — a failed PUT recorded an HTTP status and
    // threw it away. Installed first, before anything that might want to
    // complain, and it simply forwards into OBS's own log so an operator finds
    // it where they already look.
    multisite::set_log_sink([](multisite::LogLevel lvl, const std::string& msg) {
        switch (lvl) {
            case multisite::LogLevel::Error: mlog_error("%s", msg.c_str()); break;
            case multisite::LogLevel::Warn:  mlog_warn("%s", msg.c_str());  break;
            default:                         mlog_info("%s", msg.c_str());  break;
        }
    });

#ifdef MULTISITE_HAVE_QT
    mlog_info("loading obs-multisite %s (with operator docks)", PLUGIN_VERSION);
#else
    mlog_info("loading obs-multisite %s (hotkeys only — this build has no "
              "operator docks)", PLUGIN_VERSION);
#endif
    // Both types are registered whatever role this machine is set to. The
    // role decides which PANELS appear, never which sources exist: a scene
    // collection holding a Multisite Source has to keep resolving it, and a
    // preference is no reason to take a source type away from one.
    multisite_obs::register_output();   // main campus: sends
    multisite_obs::register_source();   // satellite: receives
    multisite_obs::register_ui();       // hotkeys + Tools menu (no Qt needed)
#ifdef MULTISITE_HAVE_QT
    multisite_obs::register_docks();    // encoder + decoder operator panels
#endif
    // The monitoring heartbeat. Started whether or not this build has docks:
    // a headless encoder is exactly the machine whose state wants reporting.
    // Off by default — until an operator configures it, this is one sleeping
    // thread and zero sockets.
    multisite_obs::reporter_start();
    // The phone-and-tablet interface, served from this process. No Qt either:
    // a build without docks still gets remote control, because the machine that
    // most needs it is the one nobody is sitting at.
    multisite_obs::start_web_ui();
    return true;
}
void obs_module_unload(void) {
    // Stopped first: while it is running, a request can arrive at any moment,
    // and it must not arrive after the things it controls have gone.
    multisite_obs::reporter_stop();
    multisite_obs::unregister_vendor_api();
    multisite_obs::shut_down_web_ui();
    multisite_obs::unregister_ui();
    mlog_info("obs-multisite unloaded");
}

// obs-websocket's own header is explicit that a vendor must be registered HERE
// and not in obs_module_load(): a vendor registered any earlier is never seen by
// a client. This runs after every module has loaded, so obs-websocket is ready.
// With obs-websocket absent there is nothing to register against and the call
// logs one line and returns — hotkeys, docks and pages are unaffected.
void obs_module_post_load(void) {
    multisite_obs::register_vendor_api();
}
