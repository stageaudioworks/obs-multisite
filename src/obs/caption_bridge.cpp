// SPDX-License-Identifier: GPL-3.0-or-later
#include "caption_bridge.h"
#include "caption_text.h"
#include "plugin_log.h"

#include <obs.h>
#include <callback/calldata.h>
#include <callback/signal.h>

#include <chrono>

namespace multisite_obs {

// How long a caption stays up if nothing replaces it. Only the TEXT path uses
// it: embedded captions carry their own timing in the cc_data.
//
// It is not a guess at reading speed. It is the gate libobs uses to decide when
// the NEXT caption may go out (`caption_timestamp = frame_timestamp +
// display_duration`), so too long throttles live speech. Two seconds is what
// LocalVocal settled on for its own path, with a note that shorter does not
// work, so it is a figure with some road behind it.
static constexpr double kDisplayDurationS = 2.0;

// Only the text path polls. Speech arrives in phrases, not frames.
static constexpr int kPollMs = 250;

CaptionBridge::~CaptionBridge() { stop(); }

void CaptionBridge::subscribe(obs_source_t* src) {
    if (!src) return;
    obs_source_add_caption_callback(src, &CaptionBridge::on_cea708, this);
    obs_weak_source_t* weak = obs_source_get_weak_source(src);
    if (!weak) return;
    std::lock_guard<std::mutex> lk(m_subs_mtx);
    m_subs.push_back(weak);
}

void CaptionBridge::on_source_created(void* param, calldata_t* cd) {
    auto* self = static_cast<CaptionBridge*>(param);
    if (!self || !self->m_running.load() || !self->m_automatic) return;
    auto* src = (obs_source_t*)calldata_ptr(cd, "source");
    self->subscribe(src);
}

void CaptionBridge::start(obs_output_t* output, const std::string& setting) {
    stop();
    if (!output || setting.empty()) return;      // no captions

    m_output    = output;
    m_automatic = (setting == kCaptionAuto());
    m_text_source = m_automatic ? std::string() : setting;
    {
        std::lock_guard<std::mutex> lk(m_last_mtx);
        m_last.clear();
    }
    m_sent = 0;
    m_said_embedded = false;
    m_said_split = false;
    m_running = true;

    if (m_automatic) {
        // Every source, so it does not matter which plugin carries the
        // captions. DeckLink emits them from SDI VANC today; this needs to know
        // nothing about that, or about whatever does it next.
        obs_enum_sources(
            [](void* param, obs_source_t* src) -> bool {
                static_cast<CaptionBridge*>(param)->subscribe(src);
                return true;
            },
            this);
        signal_handler_connect(obs_get_signal_handler(), "source_create",
                               &CaptionBridge::on_source_created, this);
        mlog_info("captions: listening to every source for embedded CEA-708 "
                  "(%zu source(s) now)", m_subs.size());
    } else {
        obs_source_t* src = obs_get_source_by_name(m_text_source.c_str());
        if (src) {
            // Subscribed as well as polled: a named source might carry real
            // captions rather than text, and then the better path is used
            // without the operator having to know the difference.
            subscribe(src);
            obs_source_release(src);
        } else {
            // Said plainly. A misspelt or deleted source is otherwise a
            // broadcast with no captions and no way to tell that from silence.
            mlog_warn("captions: no source named \"%s\" — nothing will be sent",
                      m_text_source.c_str());
        }
        m_thread = std::thread([this] { watch_text_loop(); });
        mlog_info("captions: taking text from \"%s\"", m_text_source.c_str());
    }
}

void CaptionBridge::unsubscribe_all() {
    std::lock_guard<std::mutex> lk(m_subs_mtx);
    for (obs_weak_source_t* weak : m_subs) {
        // The source may already be gone; the weak ref is what makes asking
        // safe rather than a use-after-free.
        if (obs_source_t* src = obs_weak_source_get_source(weak)) {
            obs_source_remove_caption_callback(src, &CaptionBridge::on_cea708, this);
            obs_source_release(src);
        }
        obs_weak_source_release(weak);
    }
    m_subs.clear();
}

void CaptionBridge::stop() {
    const bool was = m_running.exchange(false);
    if (m_thread.joinable()) m_thread.join();
    if (!was) return;

    if (m_automatic)
        signal_handler_disconnect(obs_get_signal_handler(), "source_create",
                                  &CaptionBridge::on_source_created, this);
    unsubscribe_all();

    mlog_info("captions: stopped after %llu caption(s)",
              (unsigned long long)m_sent.load());
    m_output = nullptr;
    m_automatic = false;
    m_text_source.clear();
}

void CaptionBridge::watch_text_loop() {
    while (m_running.load()) {
        // Looked up each pass rather than held: a text source can be deleted
        // and recreated mid-broadcast, and a stale pointer would be a crash in
        // the middle of an event. A fresh lookup just starts working again.
        if (obs_source_t* src = obs_get_source_by_name(m_text_source.c_str())) {
            if (obs_data_t* settings = obs_source_get_settings(src)) {
                const char* text = obs_data_get_string(settings, "text");
                if (text && *text) send_text(text);
                obs_data_release(settings);
            }
            obs_source_release(src);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
    }
}

void CaptionBridge::send_text(const std::string& text) {
    {
        std::lock_guard<std::mutex> lk(m_last_mtx);
        if (text == m_last) return;   // the source still holds the last phrase
        m_last = text;
    }
    if (!m_output) return;

    // SPLIT, because libobs will not. It holds a caption in a 128-byte buffer
    // and fills it with one snprintf — truncating, beneath a comment claiming
    // it splits. A spoken sentence passes 128 bytes regularly. See
    // caption_text.h. Embedded captions never come through here: they are
    // bytes, not text, and go straight out untouched.
    const std::vector<std::string> parts = split_caption(text);
    for (const std::string& part : parts) {
        obs_output_output_caption_text2(m_output, part.c_str(), kDisplayDurationS);
        m_sent++;
    }
    if (parts.size() > 1 && !m_said_split.exchange(true))
        mlog_info("captions: this source sends more than %zu bytes at a time, so "
                  "a sentence is split across screens and held %.1fs each",
                  kCaptionMaxBytes, kDisplayDurationS);
}

void CaptionBridge::on_cea708(void* param, obs_source_t* source,
                              const struct obs_source_cea_708* captions) {
    auto* self = static_cast<CaptionBridge*>(param);
    if (!self || !self->m_running.load() || !captions || !self->m_output) return;

    // Straight through, byte for byte. These are already CEA-708 cc_data, so
    // turning them into text and re-encoding would only lose whatever the
    // upstream encoder decided — timing, roll-up, positioning.
    obs_output_caption(self->m_output, captions);
    self->m_sent++;

    // Worth one line the first time, because "the feed has captions and we are
    // carrying them" is otherwise invisible until somebody checks the far end.
    if (!self->m_said_embedded.exchange(true))
        mlog_info("captions: carrying embedded captions from \"%s\"",
                  source ? obs_source_get_name(source) : "a source");
}

} // namespace multisite_obs
