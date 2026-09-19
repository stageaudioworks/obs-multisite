// SPDX-License-Identifier: GPL-3.0-or-later
#include "decoder_api.h"
#include "api_common.h"
#include "commands.h"
#include "web_ui.h"

#include "../multisite_ui.h"

#include "core/control_api.h"

#include <obs-module.h>

#include <functional>
#include <string>

namespace multisite_obs {

namespace {

using ControlAction = std::function<void(const multisite::HttpRequest&)>;

// A control: refused while locked, and answered with the new status.
//
// Unlike the encoder's, this needs no thread hand-off: these are the same
// functions the hotkeys call, which were written to be reached from a thread
// nobody owns. obs-websocket reaches exactly the same ones through the same
// wrapper, so a button and a keypress still cannot disagree.
multisite::HttpHandler decoder_control(ControlAction act) {
    return [act](const multisite::HttpRequest& req,
                 multisite::HttpResponse& res) {
        // Checked before anything else: while OBS is closing, a control
        // accepted now would be applied to something already going away.
        if (web_ui_stopping()) {
            fail(res, 503, "OBS is closing");
            return;
        }
        if (web_ui_locked()) {
            res.status = 409;
            res.json(json{{"error", "the controls are locked"},
                          {"locked", true}}.dump());
            return;
        }
        act(req);
        res.json(decoder_status_json());
    };
}

// The one place a route path is written: the name list in the core, prefixed
// with /api/. What a page calls and what a vendor request is named therefore
// cannot drift.
std::string path(const char* name) { return multisite::api_path(name); }

} // namespace

void register_decoder_api(multisite::HttpServer& server) {
    using multisite::HttpRequest;
    using multisite::HttpResponse;

    server.route("GET", path("decoder/status"),
                 [](const HttpRequest&, HttpResponse& res) {
        res.json(decoder_status_json());
    });

    // ── Transport ───────────────────────────────────────────────────────────
    // Named for what an operator says, not for what the pipeline does: loading
    // buffers, playing goes to air, holding keeps the picture on screen.
    server.route("POST", path("decoder/play"), decoder_control(
        [](const HttpRequest&) { decoder_play_all(); }));
    server.route("POST", path("decoder/stop"), decoder_control(
        [](const HttpRequest&) { decoder_stop_all(); }));
    server.route("POST", path("decoder/hold"), decoder_control(
        [](const HttpRequest&) { decoder_pause_all(); }));
    server.route("POST", path("decoder/continue"), decoder_control(
        [](const HttpRequest&) { decoder_resume_all(); }));
    server.route("POST", path("decoder/catch-up"), decoder_control(
        [](const HttpRequest&) { decoder_jump_live_all(); }));

    server.route("POST", path("decoder/jog"), decoder_control(
        [](const HttpRequest& req) {
            decoder_jog(num_param(req, "seconds", 0.0));
        }));

    // `ms` is a POSITION — milliseconds into the programme — because that is
    // what this plugin's own page draws its timeline in now. It was a time of
    // day, which the page then had to convert back (BUGS #2b).
    server.route("POST", path("decoder/seek"), decoder_control(
        [](const HttpRequest& req) {
            decoder_seek_media((long long)num_param(req, "ms", 0.0));
        }));

    server.route("POST", path("decoder/delay"), decoder_control(
        [](const HttpRequest& req) {
            decoder_set_delay(num_param(req, "seconds", 0.0));
        }));

    server.route("POST", path("decoder/marker"), decoder_control(
        [](const HttpRequest& req) {
            const std::string id = str_param(req, "id");
            if (!id.empty()) decoder_jump_to_marker(id);
        }));

    // Drop a cue under this machine's site name. Not wrapped in decoder_control
    // because its refusal must reach the caller in words — "no site name is
    // set" is something an operator has to fix, not a silent no-op.
    server.route("POST", path("decoder/cue"),
                 [](const HttpRequest& req, HttpResponse& res) {
        if (web_ui_stopping()) { fail(res, 503, "OBS is closing"); return; }
        if (web_ui_locked()) {
            res.status = 409;
            res.json(json{{"error", "the controls are locked"},
                          {"locked", true}}.dump());
            return;
        }
        const json j = body_object(req);
        std::string label;
        if (!json_str(j, "label", label)) label = str_param(req, "label");
        std::string error;
        if (!decoder_add_cue(label, error)) {
            res.status = 409;
            res.json(json{{"error", error}}.dump());
            return;
        }
        res.json(decoder_status_json());
    });

    // ── Recordings ──────────────────────────────────────────────────────────
    server.route("GET", path("decoder/events"),
                 [](const HttpRequest&, HttpResponse& res) {
        res.json(decoder_events_json());
    });

    // Refreshing is a request, not a wait: listing a bucket plus one manifest
    // per event is far too slow to make a phone stand still for, so the answer
    // is the listing as it stands and the next poll shows it filling in.
    server.route("POST", path("decoder/events/refresh"), decoder_control(
        [](const HttpRequest&) { decoder_refresh_events(); }));

    server.route("POST", path("decoder/load-event"), decoder_control(
        [](const HttpRequest& req) {
            const json j = body_object(req);
            std::string id;
            if (!json_str(j, "event_id", id)) id = str_param(req, "event_id");
            if (!id.empty()) decoder_pin_event(id);
        }));

    server.route("POST", path("decoder/return-to-live"), decoder_control(
        [](const HttpRequest&) { decoder_unpin_event(); }));

    // ── Settings ────────────────────────────────────────────────────────────
    server.route("GET", path("decoder/settings"),
                 [](const HttpRequest&, HttpResponse& res) {
        res.json(decoder_settings_json());
    });

    server.route("POST", path("decoder/settings"),
                 [](const HttpRequest& req, HttpResponse& res) {
        if (web_ui_locked()) { fail(res, 409, "the controls are locked"); return; }

        std::string error;
        decoder_apply_settings(req.body, error);
        res.json(decoder_settings_json());
    });
}

} // namespace multisite_obs
