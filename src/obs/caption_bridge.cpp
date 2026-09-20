// SPDX-License-Identifier: GPL-3.0-or-later
#include "caption_bridge.h"

#include "plugin_log.h"

#include <obs.h>

#include <chrono>

namespace multisite_obs {

// How long a caption stays up if nothing replaces it.
//
// Not a guess at how long the words take to read: it is the gate libobs uses to
// decide when the NEXT caption may go out (`caption_timestamp = frame_timestamp
// + display_duration` in obs-output.c). Too long and live speech is throttled —
// a sentence every four seconds when someone is talking continuously. Two
// seconds is what LocalVocal settled on for its own path, with a note that
// shorter values do not work, so it is a figure with some road behind it.
static constexpr double kDisplayDurationS = 2.0;

// How often the text source is read. Speech arrives in phrases, not frames;
// four times a second is comfortably inside human reaction time and costs a
// settings lookup. It is a poll rather than a signal because a text source's
// text changes through `obs_source_update`, which carries the whole settings
// object and no indication of WHICH field moved — so a signal would have to
// compare the text anyway, exactly as this does.
static constexpr int kPollMs = 250;

CaptionBridge::~CaptionBridge() { stop(); }

void CaptionBridge::start(obs_output_t* output, const std::string& source_name) {
    stop();
    if (!output || source_name.empty()) return;   // captions off: the default

    m_output      = output;
    m_source_name = source_name;
    {
        std::lock_guard<std::mutex> lk(m_last_mtx);
        m_last.clear();
    }
    m_sent    = 0;
    m_running = true;

    // A real CEA-708 producer, if this source happens to be one. Costs nothing
    // when it is not: the callback simply never fires.
    if (obs_source_t* src = obs_get_source_by_name(m_source_name.c_str())) {
        obs_source_add_caption_callback(src, &CaptionBridge::on_cea708, this);
        obs_source_release(src);
    } else {
        // Said once, plainly. A misspelt or deleted source is otherwise a
        // broadcast that quietly carries no captions at all, and the operator
        // has no way to tell that from "nobody is speaking".
        mlog_warn("captions: no source named \"%s\" — nothing will be sent. "
                  "Check the caption source in Settings.", m_source_name.c_str());
    }

    m_thread = std::thread([this] { watch_loop(); });
    mlog_info("captions: watching \"%s\", sending as CEA-708 in the video "
              "bitstream", m_source_name.c_str());
}

void CaptionBridge::stop() {
    if (!m_running.exchange(false)) {
        if (m_thread.joinable()) m_thread.join();
        return;
    }
    if (m_thread.joinable()) m_thread.join();

    if (!m_source_name.empty()) {
        if (obs_source_t* src = obs_get_source_by_name(m_source_name.c_str())) {
            obs_source_remove_caption_callback(src, &CaptionBridge::on_cea708, this);
            obs_source_release(src);
        }
    }
    mlog_info("captions: stopped after %llu caption(s)",
              (unsigned long long)m_sent.load());
    m_output = nullptr;
    m_source_name.clear();
}

void CaptionBridge::watch_loop() {
    while (m_running.load()) {
        // Looked up every pass rather than held. A text source can be deleted
        // and recreated mid-broadcast (an operator fixing a font), and a stale
        // pointer here would be a crash in the middle of an event; a fresh
        // lookup just starts working again when the source comes back.
        obs_source_t* src = obs_get_source_by_name(m_source_name.c_str());
        if (src) {
            if (obs_data_t* settings = obs_source_get_settings(src)) {
                const char* text = obs_data_get_string(settings, "text");
                if (text && *text) send(text);
                obs_data_release(settings);
            }
            obs_source_release(src);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
    }
}

void CaptionBridge::send(const std::string& text) {
    {
        std::lock_guard<std::mutex> lk(m_last_mtx);
        if (text == m_last) return;      // the source still holds the last phrase
        m_last = text;
    }
    if (!m_output) return;
    obs_output_output_caption_text2(m_output, text.c_str(), kDisplayDurationS);
    m_sent++;
}

void CaptionBridge::on_cea708(void* param, obs_source_t*,
                              const struct obs_source_cea_708* captions) {
    auto* self = static_cast<CaptionBridge*>(param);
    if (!self || !self->m_running.load() || !captions) return;
    // Already CEA-708, so it goes straight through as caption DATA rather than
    // being turned back into text: obs_output_caption takes the same struct
    // libobs would have built itself.
    obs_output_caption(self->m_output, captions);
    self->m_sent++;
}

} // namespace multisite_obs
