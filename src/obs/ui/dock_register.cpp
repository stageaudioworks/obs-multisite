// SPDX-License-Identifier: GPL-3.0-or-later
// dock_register.cpp — installs the two docks in the OBS window.
//
// Compiled only when the plugin is built with Qt and obs-frontend-api
// (ENABLE_QT). Everything else in the plugin works without them.
#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QScrollArea>
#include <QWidget>

#include "encoder_dock.h"
#include "decoder_dock.h"
#include "cues_dock.h"
#include "settings_tabs.h"
#include "../plugin_log.h"
#include "../plugin_role.h"
#include "../update_check.h"

namespace multisite_obs {

// OBS docks share vertical space with the mixer, transitions and controls, so
// a dock can end up much shorter than its content. Wrapping in a scroll area
// means nothing becomes unreachable on a small screen — and it has to be a
// ShrinkableScrollArea, because a plain QScrollArea reports its content's
// minimum as its own and the dock then cannot be shrunk at all.
static QWidget* scrollable(QWidget* inner) {
    auto* area = new ShrinkableScrollArea();
    area->setWidget(inner);
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    // Let the dock get narrow without forcing a horizontal scrollbar.
    area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    return area;
}

void register_docks() {
    // A machine set to one role gets one dock. A campus that only receives has
    // no business being one click from going live, and an operator should not
    // have to learn to ignore half of what is on screen.
    //
    // The output and source types are registered regardless — see
    // plugin_role.h. This hides panels; it does not take a source type away
    // from a scene collection that is using it.
    const Role role = plugin_role();

    // add_dock_by_id takes ownership of the widget and remembers its geometry
    // and visibility between sessions, so an operator's layout persists.
    if (role != Role::DecoderOnly)
        obs_frontend_add_dock_by_id("multisite_encoder",
                                    obs_module_text("Dock.Encoder"),
                                    scrollable(new EncoderDock()));
    if (role != Role::EncoderOnly)
        obs_frontend_add_dock_by_id("multisite_decoder",
                                    obs_module_text("Dock.Decoder"),
                                    scrollable(new DecoderDock()));

    // The Cues dock is in BOTH roles on purpose. Cues are the same thing on
    // either end — the same list, the same names, whoever set them — so an
    // operator should find them in the same place whichever this machine is.
    obs_frontend_add_dock_by_id("multisite_cues",
                                obs_module_text("Dock.Cues"),
                                scrollable(new CuesDock()));

    // One update check per OBS run, started here because the docks are what
    // show the answer and this is the one place both of them are known to
    // exist. It runs in the background and fails quietly; nothing waits on it.
    update_check_start(PLUGIN_VERSION);

    switch (role) {
        case Role::EncoderOnly:
            mlog_info("registered the encoder dock only (role 'encoder')");
            break;
        case Role::DecoderOnly:
            mlog_info("registered the decoder dock only (role 'decoder')");
            break;
        default:
            mlog_info("registered encoder and decoder docks");
            break;
    }
}

} // namespace multisite_obs
