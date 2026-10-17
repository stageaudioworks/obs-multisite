// SPDX-License-Identifier: GPL-3.0-or-later
//
// output_factory.cpp — choosing where picture and sound go.
//
// A box with no display attached, or a development machine with no KMS at all,
// still has to receive the feed and answer its web UI: a campus should be able
// to prepare an event before the screen is plugged in, and a failure to open
// an output must never take the receiver down with it. So the factories fall
// back to a null output and say so, rather than refusing to start.
//
// Compiled into multisite-player only, not the engine library: the
// MULTISITE_HAVE_DRM / _ALSA switches belong to the executable that links
// drm_output.cpp and alsa_output.cpp, and an installed engine must not
// reference make_drm_output, which no external consumer provides.
//
#include "audio_output.h"
#include "video_output.h"
#include "log.h"

namespace multisite_player {

VideoOutput* make_video_output() {
#ifdef MULTISITE_HAVE_DRM
    if (VideoOutput* drm = make_drm_output()) return drm;
    plog_warn("no usable KMS display — the feed will be received but not "
              "shown. Check that a screen is connected.");
#endif
    return new NullVideoOutput();
}

AudioOutput* make_audio_output() {
#ifdef MULTISITE_HAVE_ALSA
    if (AudioOutput* alsa = make_alsa_output()) return alsa;
    plog_warn("no usable ALSA device — the feed will be received but silent.");
#endif
    return new NullAudioOutput();
}

} // namespace multisite_player
