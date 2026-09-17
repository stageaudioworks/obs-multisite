// SPDX-License-Identifier: GPL-3.0-or-later
// multisite_ui.cpp — operator controls via OBS's frontend API and hotkeys.
//
// Deliberately Qt-free. `obs_frontend_add_tools_menu_item` takes a plain C
// callback, and hotkeys need no UI toolkit at all — only docks require a
// QWidget. For a live event, hotkeys are also the better interface: an
// operator holding for their own welcome wants a keypress, not a properties
// dialog. A Qt dock (scrub bar, behind-live readout) can be layered on later
// without changing any of this.
//
// Registered controls:
//   Encoder  — Drop marker (x4 configurable labels)
//   Decoder  — Pause / Resume / Toggle, Jump to live, Log status
//
#include <obs-module.h>

// Tools-menu items come from obs-frontend-api, which is part of OBS's UI
// subproject and is not built when libobs is compiled with ENABLE_UI=OFF (as
// our CI does, to avoid pulling in Qt). Hotkeys, by contrast, live in libobs
// itself — so they are always available and are the primary interface for a
// live operator anyway. The menu items are therefore optional.
#ifdef MULTISITE_HAVE_FRONTEND_API
#include <obs-frontend-api.h>
#endif

#include "plugin_log.h"
#include "multisite_ui.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

namespace multisite_obs {

// ── Registry of live components ──────────────────────────────────────────────
// Hotkeys and menu items are global, but the things they act on come and go as
// sources are created and outputs start. Rather than holding raw pointers, the
// encoder and decoder register themselves here and unregister on teardown.
namespace {

std::mutex g_mtx;
EncoderControls* g_encoder = nullptr;
std::vector<DecoderControls*> g_decoders;

// Hotkey ids, so they can be unregistered on unload.
std::vector<obs_hotkey_id> g_hotkeys;

} // namespace

void register_encoder_controls(EncoderControls* e) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_encoder = e;
}
void unregister_encoder_controls(EncoderControls* e) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_encoder == e) g_encoder = nullptr;
}
void register_decoder_controls(DecoderControls* d) {
    std::lock_guard<std::mutex> lk(g_mtx);
    // Idempotent. OBS can call a source's update more than once, and a
    // duplicate entry made every control fire twice and every reconfigure
    // restart the worker threads twice.
    for (auto* e : g_decoders) if (e == d) return;
    g_decoders.push_back(d);
}
void unregister_decoder_controls(DecoderControls* d) {
    std::lock_guard<std::mutex> lk(g_mtx);
    // Remove ALL matches, not just the first: a stale duplicate would
    // otherwise survive and keep acting on a source that had gone.
    g_decoders.erase(std::remove(g_decoders.begin(), g_decoders.end(), d),
                     g_decoders.end());
}

bool encoder_stats(EncoderStats& out) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_encoder) return false;
    out = g_encoder->stats();
    return true;
}

void forward_marker_to_encoder(const std::string& label) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_encoder) {
        mlog_warn("marker '%s' ignored — not broadcasting", label.c_str());
        return;
    }
    g_encoder->drop_marker(label);
}

bool decoder_snapshot(DecoderSnapshot& out) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_decoders.empty()) return false;
    g_decoders.front()->snapshot(out);     // the dock follows the first source
    return true;
}

void decoder_pause_all() {
    std::lock_guard<std::mutex> lk(g_mtx);
    for (auto* d : g_decoders) d->pause();
}
void decoder_resume_all() {
    std::lock_guard<std::mutex> lk(g_mtx);
    for (auto* d : g_decoders) d->resume();
}
void decoder_jump_live_all() {
    std::lock_guard<std::mutex> lk(g_mtx);
    for (auto* d : g_decoders) d->jump_to_live();
}

void decoder_reconfigure_all() {
    std::lock_guard<std::mutex> lk(g_mtx);
    for (auto* d : g_decoders) d->reconfigure();
}

void decoder_play_all() {
    std::lock_guard<std::mutex> lk(g_mtx);
    for (auto* d : g_decoders) d->play();
}
void decoder_stop_all() {
    std::lock_guard<std::mutex> lk(g_mtx);
    for (auto* d : g_decoders) d->stop_playback();
}
void decoder_seek_time(long long wall_ms) {
    std::lock_guard<std::mutex> lk(g_mtx);
    for (auto* d : g_decoders) d->seek_to_time(wall_ms);
}
void decoder_jog(double seconds) {
    std::lock_guard<std::mutex> lk(g_mtx);
    for (auto* d : g_decoders) d->jog(seconds);
}
void decoder_set_delay(double seconds) {
    std::lock_guard<std::mutex> lk(g_mtx);
    for (auto* d : g_decoders) d->set_delay_from_live(seconds);
}
void decoder_set_locked(bool locked) {
    std::lock_guard<std::mutex> lk(g_mtx);
    for (auto* d : g_decoders) d->set_locked(locked);
}

void decoder_jump_to_marker(const std::string& id) {
    std::lock_guard<std::mutex> lk(g_mtx);
    for (auto* d : g_decoders) d->jump_to_marker(id);
}

void decoder_seek(unsigned long long seq) {
    std::lock_guard<std::mutex> lk(g_mtx);
    for (auto* d : g_decoders) d->seek(seq);
}

// ── Cues ─────────────────────────────────────────────────────────────────────
bool encoder_cues(std::vector<CueEntry>& out) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_encoder) return false;
    g_encoder->cues(out);
    return true;
}

bool decoder_cues(std::vector<CueEntry>& out) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_decoders.empty()) return false;
    DecoderSnapshot s;
    g_decoders.front()->snapshot(s);      // the dock follows the first source
    out = std::move(s.markers);
    return true;
}

bool decoder_add_cue(const std::string& label, std::string& error) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_decoders.empty()) {
        error = "no decoder source is present";
        return false;
    }
    g_decoders.front()->add_cue(label, error);
    return error.empty();
}

// The event list belongs to one source, not all of them: it is a list of what
// THIS room recorded, and the dock follows the first source exactly as the
// snapshot does.
void decoder_refresh_events() {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_decoders.empty()) return;
    g_decoders.front()->refresh_events();
}
bool decoder_event_listing(EventListing& out) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_decoders.empty()) return false;
    g_decoders.front()->event_listing(out);
    return true;
}
void decoder_pin_event(const std::string& event_id) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_decoders.empty()) return;
    g_decoders.front()->pin_event(event_id);
}
void decoder_unpin_event() {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_decoders.empty()) return;
    g_decoders.front()->unpin_event();
}

// ── Encoder: drop a marker ───────────────────────────────────────────────────
static void drop_marker(const std::string& label) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_encoder) {
        mlog_warn("marker '%s' ignored — not broadcasting", label.c_str());
        return;
    }
    g_encoder->drop_marker(label);
}

struct MarkerSlot { int index; };
static MarkerSlot g_slots[4] = { {0}, {1}, {2}, {3} };

static std::string marker_label_for(int index) {
    // The names come from the event's own cues — the same names the Cues dock
    // shows as one-press buttons — so the hotkeys and the dock agree with no
    // separate list to configure. Deliberately NOT under g_mtx: encoder_cues()
    // takes that lock itself.
    std::vector<CueEntry> cues;
    if (encoder_cues(cues)) {
        std::vector<std::string> names;
        for (const auto& c : cues) {
            if (c.label.empty()) continue;
            bool seen = false;
            for (const auto& n : names) if (n == c.label) { seen = true; break; }
            if (!seen) names.push_back(c.label);
        }
        if (index < (int)names.size()) return names[index];
    }
    return "Marker " + std::to_string(index + 1);
}

static void hotkey_marker(void* data, obs_hotkey_id, obs_hotkey_t*, bool pressed) {
    if (!pressed) return;
    auto* slot = static_cast<MarkerSlot*>(data);
    drop_marker(marker_label_for(slot->index));
}

static void menu_marker(void* data) {
    auto* slot = static_cast<MarkerSlot*>(data);
    drop_marker(marker_label_for(slot->index));
}

// ── Decoder: timeslipping controls ───────────────────────────────────────────
// These act on every multisite source present, which is what an operator
// expects: one keypress holds the feed, however many sources are showing it.
static void for_each_decoder(void (*fn)(DecoderControls*)) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_decoders.empty()) {
        mlog_warn("no multisite source is active");
        return;
    }
    for (auto* d : g_decoders) fn(d);
}

static void hotkey_pause(void*, obs_hotkey_id, obs_hotkey_t*, bool pressed) {
    if (pressed) for_each_decoder([](DecoderControls* d) { d->pause(); });
}
static void hotkey_resume(void*, obs_hotkey_id, obs_hotkey_t*, bool pressed) {
    if (pressed) for_each_decoder([](DecoderControls* d) { d->resume(); });
}
static void hotkey_toggle(void*, obs_hotkey_id, obs_hotkey_t*, bool pressed) {
    if (pressed) for_each_decoder([](DecoderControls* d) { d->toggle_pause(); });
}
static void hotkey_live(void*, obs_hotkey_id, obs_hotkey_t*, bool pressed) {
    if (pressed) for_each_decoder([](DecoderControls* d) { d->jump_to_live(); });
}
// Play / Stop / Lock and jog, so an event can be run from the keyboard
// without touching the mouse — the same reason Resi ships hotkeys.
static void hotkey_play(void*, obs_hotkey_id, obs_hotkey_t*, bool pressed) {
    if (pressed) decoder_play_all();
}
static void hotkey_stop(void*, obs_hotkey_id, obs_hotkey_t*, bool pressed) {
    if (pressed) decoder_stop_all();
}
static void hotkey_jog_back(void*, obs_hotkey_id, obs_hotkey_t*, bool pressed) {
    if (pressed) decoder_jog(-10.0);
}
static void hotkey_jog_fwd(void*, obs_hotkey_id, obs_hotkey_t*, bool pressed) {
    if (pressed) decoder_jog(10.0);
}

static void hotkey_status(void*, obs_hotkey_id, obs_hotkey_t*, bool pressed) {
    if (pressed) {
        for_each_decoder([](DecoderControls* d) { d->log_status(); });
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_encoder) g_encoder->log_status();
    }
}

static void menu_pause(void*)  { for_each_decoder([](DecoderControls* d) { d->toggle_pause(); }); }
static void menu_live(void*)   { for_each_decoder([](DecoderControls* d) { d->jump_to_live(); }); }
static void menu_status(void*) { hotkey_status(nullptr, 0, nullptr, true); }

// ── Registration ─────────────────────────────────────────────────────────────
void register_ui() {
    // Hotkeys: the primary live interface. Unbound by default — the operator
    // assigns keys in Settings -> Hotkeys.
    g_hotkeys.push_back(obs_hotkey_register_frontend(
        "multisite.pause", obs_module_text("Hotkey.Pause"),
        hotkey_pause, nullptr));
    g_hotkeys.push_back(obs_hotkey_register_frontend(
        "multisite.resume", obs_module_text("Hotkey.Resume"),
        hotkey_resume, nullptr));
    g_hotkeys.push_back(obs_hotkey_register_frontend(
        "multisite.toggle", obs_module_text("Hotkey.Toggle"),
        hotkey_toggle, nullptr));
    g_hotkeys.push_back(obs_hotkey_register_frontend(
        "multisite.jump_live", obs_module_text("Hotkey.JumpLive"),
        hotkey_live, nullptr));
    g_hotkeys.push_back(obs_hotkey_register_frontend(
        "multisite.status", obs_module_text("Hotkey.Status"),
        hotkey_status, nullptr));
    g_hotkeys.push_back(obs_hotkey_register_frontend(
        "multisite.play", obs_module_text("Hotkey.Play"),
        hotkey_play, nullptr));
    g_hotkeys.push_back(obs_hotkey_register_frontend(
        "multisite.stop", obs_module_text("Hotkey.Stop"),
        hotkey_stop, nullptr));
    g_hotkeys.push_back(obs_hotkey_register_frontend(
        "multisite.jog_back", obs_module_text("Hotkey.JogBack"),
        hotkey_jog_back, nullptr));
    g_hotkeys.push_back(obs_hotkey_register_frontend(
        "multisite.jog_fwd", obs_module_text("Hotkey.JogForward"),
        hotkey_jog_fwd, nullptr));

    for (int i = 0; i < 4; ++i) {
        std::string name = "multisite.marker" + std::to_string(i + 1);
        std::string desc = std::string(obs_module_text("Hotkey.Marker")) + " " +
                           std::to_string(i + 1);
        g_hotkeys.push_back(obs_hotkey_register_frontend(
            name.c_str(), desc.c_str(), hotkey_marker, &g_slots[i]));
    }

#ifdef MULTISITE_HAVE_FRONTEND_API
    // Tools menu: discoverable equivalents for anyone who hasn't set hotkeys.
    obs_frontend_add_tools_menu_item(obs_module_text("Menu.Marker1"),
                                     menu_marker, &g_slots[0]);
    obs_frontend_add_tools_menu_item(obs_module_text("Menu.Marker2"),
                                     menu_marker, &g_slots[1]);
    obs_frontend_add_tools_menu_item(obs_module_text("Menu.TogglePause"),
                                     menu_pause, nullptr);
    obs_frontend_add_tools_menu_item(obs_module_text("Menu.JumpLive"),
                                     menu_live, nullptr);
    obs_frontend_add_tools_menu_item(obs_module_text("Menu.Status"),
                                     menu_status, nullptr);
    mlog_info("registered %zu hotkeys and 5 Tools menu items",
              g_hotkeys.size());
#else
    (void)&menu_marker; (void)&menu_pause; (void)&menu_live; (void)&menu_status;
    mlog_info("registered %zu hotkeys (assign keys in Settings -> Hotkeys); "
              "Tools-menu items need a build with the OBS frontend API",
              g_hotkeys.size());
#endif
}

void unregister_ui() {
    for (obs_hotkey_id id : g_hotkeys) obs_hotkey_unregister(id);
    g_hotkeys.clear();
}

} // namespace multisite_obs
