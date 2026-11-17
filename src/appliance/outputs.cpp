// SPDX-License-Identifier: GPL-3.0-or-later
//
// outputs.cpp — the null outputs.
//
// A box with no display attached, or a development machine with no KMS at all,
// still has to receive the feed and answer its web UI. The null outputs live
// here, in the engine library; the factories that fall back to them are in
// output_factory.cpp, which only the Linux player compiles.
//
#include "audio_output.h"
#include "video_output.h"
#include "log.h"

namespace multisite_player {

bool NullVideoOutput::open(const Config& cfg, std::string& error) {
    error.clear();
    if (cfg.out_width > 0 && cfg.out_height > 0) {
        m_width  = cfg.out_width;
        m_height = cfg.out_height;
    }
    if (cfg.out_fps > 0) m_fps = cfg.out_fps;
    m_desc = "no display output (this build has no KMS support)";
    return true;
}

bool NullAudioOutput::open(const Config&, int sample_rate, int channels,
                           std::string& error) {
    error.clear();
    m_desc = "no audio output (this build has no ALSA support)";
    (void)sample_rate; (void)channels;
    return true;
}

} // namespace multisite_player
