// SPDX-License-Identifier: GPL-3.0-or-later
#include "vendor_api.h"

#include "../multisite_ui.h"
#include "../plugin_log.h"
#include "../plugin_role.h"
#include "../web/commands.h"
#include "../web/ui_thread.h"

#include "core/control_api.h"

// Header-only and resolved through OBS's proc handler, so the plugin gains no
// link dependency on obs-websocket: if it is absent, every call returns false
// after one log line and control simply disappears. Vendored so every build —
// OBS source tree, distro libobs-dev, macOS headers-only — compiles the same.
#include "vendor/obs-websocket/obs-websocket-api.h"

#include "vendor/nlohmann/json.hpp"

#include <obs-module.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace multisite_obs {

namespace {

// The name a client addresses these under: obs-multisite.<requestType>, e.g.
// obs-multisite.decoder/hold. One name for the whole surface, because a church
// runs one plugin.
constexpr const char* kVendorName = "obs-multisite";

obs_websocket_vendor g_vendor = nullptr;

// The event poller. Events are what let a Companion module light a button while
// an event is live; obs-websocket has no way to be told from here that state
// changed, so this notices it and emits.
std::thread g_poll_thread;
std::mutex g_poll_mtx;
std::condition_variable g_poll_cv;
std::atomic<bool> g_poll_stop{false};

// ── Reading a request ────────────────────────────────────────────────────────

std::string req_str(obs_data_t* req, const char* key) {
    if (!req || !obs_data_has_user_value(req, key)) return "";
    return obs_data_get_string(req, key);
}

bool req_has(obs_data_t* req, const char* key) {
    return req && obs_data_has_user_value(req, key);
}

double req_num(obs_data_t* req, const char* key, double fallback) {
    return req_has(req, key) ? obs_data_get_double(req, key) : fallback;
}

// obs-websocket takes a whole request as one obs_data document, so a settings
// save arrives as fields rather than as JSON text. Re-serialising it means the
// same applier the page uses sees the same shape, rather than a second parse.
std::string obs_to_json(obs_data_t* d) {
    if (!d) return "{}";
    const char* s = obs_data_get_json(d);
    return (s && *s) ? std::string(s) : std::string("{}");
}

// ── Writing a response ───────────────────────────────────────────────────────
//
// The same document the HTTP pages are served, put into an obs_data response so
// a client reads typed fields rather than a string to parse. Scalars keep their
// type; an array or object — the marker list, the list of encoders — is carried
// as its JSON text under the same key, because obs_data has no general nesting
// and inventing one here would be a second definition of the payload to keep in
// step with the first.
void set_json(obs_data_t* out, const std::string& text) {
    if (!out) return;
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(text);
    } catch (...) {
        return;
    }
    if (!j.is_object()) return;

    for (auto it = j.begin(); it != j.end(); ++it) {
        const std::string key = it.key();
        const nlohmann::json& v = it.value();
        if (v.is_boolean())
            obs_data_set_bool(out, key.c_str(), v.get<bool>());
        else if (v.is_number_integer() || v.is_number_unsigned())
            obs_data_set_int(out, key.c_str(), (long long)v.get<long long>());
        else if (v.is_number())
            obs_data_set_double(out, key.c_str(), v.get<double>());
        else if (v.is_string())
            obs_data_set_string(out, key.c_str(),
                                v.get<std::string>().c_str());
        else if (!v.is_null())
            obs_data_set_string(out, key.c_str(), v.dump().c_str());
    }
}

// A refusal says why, in words, exactly as the HTTP surface does — obs-websocket
// gives a vendor request no status code of its own, so the reason travels in an
// "error" field and a client that cares reads it.
void reply(obs_data_t* res, const std::string& status_json,
           const std::string& error) {
    if (!error.empty()) obs_data_set_string(res, "error", error.c_str());
    set_json(res, status_json);
}

} // namespace
} // namespace multisite_obs
// ── Encoder handlers ─────────────────────────────────────────────────────────
//
// Each runs the command on OBS's UI thread, exactly as the HTTP wrapper does,
// and answers with the new status so a client never has to guess what its own
// request did.

namespace multisite_obs {
namespace {

void h_encoder_status(obs_data_t*, obs_data_t* res, void*) {
    set_json(res, encoder_status_json());
}

void h_encoder_go_live(obs_data_t* req, obs_data_t* res, void*) {
    // The title arrives as a field. Blank means "name it now", which the
    // command resolves to the current time.
    std::string name = req_str(req, "event_name");
    if (name.empty()) name = req_str(req, "name");

    auto error = std::make_shared<std::string>();
    if (!run_on_ui_thread([name, error] { encoder_go_live(name, *error); }))
        *error = "OBS is busy - try again in a moment";
    reply(res, encoder_status_json(), *error);
}

void h_encoder_end(obs_data_t*, obs_data_t* res, void*) {
    auto error = std::make_shared<std::string>();
    if (!run_on_ui_thread([error] { encoder_end(*error); }))
        *error = "OBS is busy - try again in a moment";
    reply(res, encoder_status_json(), *error);
}

void h_encoder_marker(obs_data_t* req, obs_data_t* res, void*) {
    const std::string label = req_str(req, "label");
    auto error = std::make_shared<std::string>();
    if (!run_on_ui_thread([label, error] { encoder_marker(label, *error); }))
        *error = "OBS is busy - try again in a moment";
    reply(res, encoder_status_json(), *error);
}

void h_encoder_settings_get(obs_data_t*, obs_data_t* res, void*) {
    set_json(res, encoder_settings_json());
}

void h_encoder_settings_set(obs_data_t* req, obs_data_t* res, void*) {
    const std::string body = obs_to_json(req);
    auto error = std::make_shared<std::string>();
    if (!run_on_ui_thread([body, error] { encoder_apply_settings(body, *error); },
                          10000))
        *error = "OBS is busy - try again in a moment";
    reply(res, encoder_settings_json(), *error);
}

} // namespace
} // namespace multisite_obs

// ── Decoder handlers ─────────────────────────────────────────────────────────
//
// These are the same functions the hotkeys call, so no thread hand-off is
// needed — they were written to be reached from a thread nobody owns.

namespace multisite_obs {
namespace {

void h_decoder_status(obs_data_t*, obs_data_t* res, void*) {
    set_json(res, decoder_status_json());
}

void h_decoder_play(obs_data_t*, obs_data_t* res, void*) {
    decoder_play_all();
    set_json(res, decoder_status_json());
}

void h_decoder_stop(obs_data_t*, obs_data_t* res, void*) {
    decoder_stop_all();
    set_json(res, decoder_status_json());
}

void h_decoder_hold(obs_data_t*, obs_data_t* res, void*) {
    decoder_pause_all();
    set_json(res, decoder_status_json());
}

void h_decoder_continue(obs_data_t*, obs_data_t* res, void*) {
    decoder_resume_all();
    set_json(res, decoder_status_json());
}

void h_decoder_catch_up(obs_data_t*, obs_data_t* res, void*) {
    decoder_jump_live_all();
    set_json(res, decoder_status_json());
}

void h_decoder_jog(obs_data_t* req, obs_data_t* res, void*) {
    decoder_jog(req_num(req, "seconds", 0.0));
    set_json(res, decoder_status_json());
}

void h_decoder_seek(obs_data_t* req, obs_data_t* res, void*) {
    decoder_seek_time((long long)req_num(req, "ms", 0.0));
    set_json(res, decoder_status_json());
}

void h_decoder_delay(obs_data_t* req, obs_data_t* res, void*) {
    decoder_set_delay(req_num(req, "seconds", 0.0));
    set_json(res, decoder_status_json());
}

void h_decoder_marker(obs_data_t* req, obs_data_t* res, void*) {
    const std::string id = req_str(req, "id");
    if (!id.empty()) decoder_jump_to_marker(id);
    set_json(res, decoder_status_json());
}

void h_decoder_cue(obs_data_t* req, obs_data_t* res, void*) {
    const std::string label = req_str(req, "label");
    std::string error;
    // A refusal is reported in words — "no site name is set" is something the
    // operator has to fix, and a button that quietly does nothing is worse.
    if (!decoder_add_cue(label, error))
        reply(res, decoder_status_json(), error);
    else
        set_json(res, decoder_status_json());
}

void h_decoder_events(obs_data_t*, obs_data_t* res, void*) {
    set_json(res, decoder_events_json());
}

void h_decoder_events_refresh(obs_data_t*, obs_data_t* res, void*) {
    decoder_refresh_events();
    set_json(res, decoder_events_json());
}

void h_decoder_load_event(obs_data_t* req, obs_data_t* res, void*) {
    const std::string id = req_str(req, "event_id");
    if (!id.empty()) decoder_pin_event(id);
    set_json(res, decoder_status_json());
}

void h_decoder_return_to_live(obs_data_t*, obs_data_t* res, void*) {
    decoder_unpin_event();
    set_json(res, decoder_status_json());
}

void h_decoder_settings_get(obs_data_t*, obs_data_t* res, void*) {
    set_json(res, decoder_settings_json());
}

void h_decoder_settings_set(obs_data_t* req, obs_data_t* res, void*) {
    std::string error;
    decoder_apply_settings(obs_to_json(req), error);
    reply(res, decoder_settings_json(), error);
}

} // namespace
} // namespace multisite_obs

// ── The surface, name by name ────────────────────────────────────────────────
//
// Every name in the core's list has exactly one handler here. Keeping the
// mapping explicit rather than generated means a name with no handler is a
// visible gap, and register_requests() says so rather than shipping a request
// that silently does nothing.

namespace multisite_obs {
namespace {

obs_websocket_request_callback_function handler_for(const char* name) {
    const std::string n = name;
    if (n == "encoder/status")         return h_encoder_status;
    if (n == "encoder/go-live")        return h_encoder_go_live;
    if (n == "encoder/end")            return h_encoder_end;
    if (n == "encoder/marker")         return h_encoder_marker;
    if (n == "encoder/settings")       return h_encoder_settings_get;
    if (n == "decoder/status")         return h_decoder_status;
    if (n == "decoder/play")           return h_decoder_play;
    if (n == "decoder/stop")           return h_decoder_stop;
    if (n == "decoder/hold")           return h_decoder_hold;
    if (n == "decoder/continue")       return h_decoder_continue;
    if (n == "decoder/catch-up")       return h_decoder_catch_up;
    if (n == "decoder/jog")            return h_decoder_jog;
    if (n == "decoder/seek")           return h_decoder_seek;
    if (n == "decoder/delay")          return h_decoder_delay;
    if (n == "decoder/marker")         return h_decoder_marker;
    if (n == "decoder/cue")            return h_decoder_cue;
    if (n == "decoder/events")         return h_decoder_events;
    if (n == "decoder/events/refresh") return h_decoder_events_refresh;
    if (n == "decoder/load-event")     return h_decoder_load_event;
    if (n == "decoder/return-to-live") return h_decoder_return_to_live;
    if (n == "decoder/settings")       return h_decoder_settings_get;
    return nullptr;
}

void register_requests(const char* const* names, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        auto fn = handler_for(names[i]);
        if (!fn) {
            mlog_warn("obs-websocket: no handler for '%s' — not registered",
                      names[i]);
            continue;
        }
        if (!obs_websocket_vendor_register_request(g_vendor, names[i], fn,
                                                   nullptr))
            mlog_warn("obs-websocket: could not register '%s'", names[i]);
    }
}

} // namespace
} // namespace multisite_obs

// ── Events ───────────────────────────────────────────────────────────────────
//
// A handful of fields that mean "something changed for an operator", so a
// client is not woken every second by a byte counter moving.

namespace multisite_obs {
namespace {

std::string signature_of(const std::string& json_text,
                         const char* const* keys, std::size_t count) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(json_text);
    } catch (...) {
        return "";
    }
    std::string sig;
    for (std::size_t i = 0; i < count; ++i) {
        auto it = j.find(keys[i]);
        sig += keys[i];
        sig += '=';
        sig += (it == j.end()) ? "?" : it->dump();
        sig += ';';
    }
    return sig;
}

void emit_state(const char* event_name, const std::string& json_text) {
    obs_data_t* d = obs_data_create();
    set_json(d, json_text);
    obs_websocket_vendor_emit_event(g_vendor, event_name, d);
    obs_data_release(d);
}

void poll_once(std::string& last_encoder, std::string& last_decoder) {
    // A marker appearing is something an operator acts on, so it belongs here
    // even though it is not "state" in the transport sense: without it, a cue
    // the main site has just dropped reaches a client only on the next poll,
    // up to five seconds later — which for a button is the difference between
    // "it works" and "it didn't work".
    static const char* enc_keys[] = {"live", "event_id", "link_health",
                                     "marker_labels"};
    static const char* dec_keys[] = {"room_state", "playing", "paused",
                                     "event_id", "live_event_id",
                                     "configured", "markers"};
    const Role role = plugin_role();

    if (role != Role::DecoderOnly) {
        const std::string s = encoder_status_json();
        const std::string sig = signature_of(s, enc_keys, std::size(enc_keys));
        if (sig != last_encoder) {
            last_encoder = sig;
            emit_state("encoder/state", s);
        }
    }
    if (role != Role::EncoderOnly) {
        const std::string s = decoder_status_json();
        const std::string sig = signature_of(s, dec_keys, std::size(dec_keys));
        if (sig != last_decoder) {
            last_decoder = sig;
            emit_state("decoder/state", s);
        }
    }
}

void poll_loop() {
    std::string last_encoder, last_decoder;
    std::unique_lock<std::mutex> lk(g_poll_mtx);
    while (!g_poll_stop.load()) {
        if (g_poll_cv.wait_for(lk, std::chrono::seconds(1),
                               [] { return g_poll_stop.load(); }))
            break;
        lk.unlock();
        poll_once(last_encoder, last_decoder);
        lk.lock();
    }
}

} // namespace
} // namespace multisite_obs

namespace multisite_obs {

void register_vendor_api() {
    if (g_vendor) return;

    g_vendor = obs_websocket_register_vendor(kVendorName);
    if (!g_vendor) {
        mlog_info("obs-websocket is not present: the vendor API is off; "
                  "docks, hotkeys and the remote-control pages are unaffected");
        return;
    }

    // The role decides what EXISTS, exactly as it does for the HTTP pages: a
    // satellite has no encoder request to reach, so nothing routes to one.
    const Role role = plugin_role();
    if (role != Role::DecoderOnly)
        register_requests(multisite::kEncoderCommands,
                          std::size(multisite::kEncoderCommands));
    if (role != Role::EncoderOnly)
        register_requests(multisite::kDecoderCommands,
                          std::size(multisite::kDecoderCommands));

    // Emit the state once at the start, then whenever it changes, so a client
    // that connects mid-event is not left guessing until something moves.
    g_poll_stop = false;
    g_poll_thread = std::thread(poll_loop);

    mlog_info("obs-websocket vendor '%s' registered (%s)", kVendorName,
              role == Role::EncoderOnly   ? "encoder commands"
              : role == Role::DecoderOnly ? "decoder commands"
                                          : "encoder and decoder commands");
}

void unregister_vendor_api() {
    if (!g_vendor) return;

    // The poller first: while it runs it reads the status of things this module
    // owns, and it must not be doing that as they go away.
    {
        std::lock_guard<std::mutex> lk(g_poll_mtx);
        g_poll_stop = true;
    }
    g_poll_cv.notify_all();
    if (g_poll_thread.joinable()) g_poll_thread.join();

    // The requests are NOT unregistered, and that is the fix for a crash rather
    // than an oversight.
    //
    // obs_websocket_vendor_unregister_request() goes through
    // obs_websocket_vendor_run_simple_proc(), which calls
    // proc_handler_call(_ph, ...) on a static pointer the vendored header
    // caches the first time it is used and never invalidates. This function
    // runs only from obs_module_unload, i.e. from inside obs_shutdown's
    // free_module loop — and module unload order is not specified. When
    // obs-websocket is freed first, `_ph` points at a destroyed proc handler
    // and the call dereferences its mutex:
    //
    //   w32-pthreads.dll!pthread_mutex_unlock
    //   obs.dll!proc_handler_call
    //   obs-multisite.dll!...              <- here
    //   obs.dll!free_module
    //   obs.dll!obs_shutdown
    //
    // Seen on Windows on 2026-09-12, as c0000005 while quitting after a
    // broadcast. There is nothing to gain by unregistering at this point:
    // obs-websocket is going away too and frees its own vendor registry, so
    // the calls were pure risk. Dropping the pointer is all that is needed.
    //
    // If this is ever called while OBS keeps running, unregistering would
    // matter again — and would be safe, because obs-websocket would still be
    // loaded. It has only ever been called from obs_module_unload.
    g_vendor = nullptr;
    mlog_info("obs-websocket vendor '%s' released", kVendorName);
}

bool vendor_api_active() { return g_vendor != nullptr; }

} // namespace multisite_obs

