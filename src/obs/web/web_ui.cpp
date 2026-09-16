// SPDX-License-Identifier: GPL-3.0-or-later
#include "web_ui.h"
#include "encoder_api.h"
#include "decoder_api.h"
#include "net_address.h"

#include "../plugin_log.h"
#include "../plugin_role.h"

#include "core/http_server.h"
#include "core/storage_providers.h"
#include "vendor/nlohmann/json.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>

using json = nlohmann::json;
using multisite::HttpRequest;
using multisite::HttpResponse;
using multisite::HttpServer;

namespace multisite_obs {

namespace {

std::mutex g_mutex;
std::unique_ptr<HttpServer> g_server;
std::string g_problem;

// Deliberately not persisted. Locking is for the duration of an event, and a
// lock that survived a restart would leave a campus unable to play with no
// obvious reason why — the tablet that set it is long since charged and put
// away.
std::atomic<bool> g_locked{false};

// Set the moment a stop begins, and cleared again when a start follows: a
// restart is not a shutdown. The dock applies its remote-control settings by
// stopping and starting the server, and leaving this set would refuse every
// control on both pages afterwards with "OBS is closing" — a real bug, and the
// reason this is cleared in start_web_ui() rather than only set here.
std::atomic<bool> g_stopping{false};

WebUiSettings& settings_storage() {
    static WebUiSettings s;
    return s;
}

// ── Serving the pages ────────────────────────────────────────────────────────

// Read from the module's data directory on every request rather than compiled
// in, for the same reason the appliance does it: somebody can fix a label on a
// machine that is mid-event without a rebuild, and the page on disk is the page
// that ships.
bool read_module_file(const std::string& rel, std::string& out) {
    char* path = obs_module_file(rel.c_str());
    if (!path) return false;
    std::ifstream in(path, std::ios::binary);
    bfree(path);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return !out.empty();
}

// The content type now comes from the core's single table. The local copy kept
// here had three entries and forgot `.svg`, so the mark in these pages' footer
// went out as application/octet-stream — which a browser is entitled to refuse
// to draw, and which nothing would have reported.

// One exact path, one file. The shared server routes by exact path and has no
// prefix matching, so every asset a page asks for is registered here — which is
// also what makes the role rule enforceable rather than advisory: a page whose
// routes were never registered does not exist, and its API does not either.
void route_file(HttpServer& server, const std::string& path,
                const std::string& file) {
    server.route("GET", path, [file](const HttpRequest&, HttpResponse& res) {
        std::string body;
        if (!read_module_file(file, body)) {
            res.text(500, "the remote-control files are not installed");
            return;
        }
        res.status = 200;
        res.content_type = multisite::content_type_for(file);
        res.body = std::move(body);
    });
}

} // namespace

// ── Settings ─────────────────────────────────────────────────────────────────

WebUiSettings& web_ui_settings() {
    static std::once_flag once;
    std::call_once(once, [] { settings_storage().load(); });
    return settings_storage();
}

void set_web_ui_settings(const WebUiSettings& in) {
    (void)web_ui_settings();
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        settings_storage() = in;
    }
    in.save();
}

void WebUiSettings::load() {
    char* path = obs_module_config_path("web.json");
    if (!path) return;
    obs_data_t* d = obs_data_create_from_json_file(path);
    bfree(path);
    if (!d) return;

    if (obs_data_has_user_value(d, "enabled"))
        enabled = obs_data_get_bool(d, "enabled");
    if (obs_data_has_user_value(d, "port"))
        port = (int)obs_data_get_int(d, "port");
    if (obs_data_has_user_value(d, "bind"))
        bind = obs_data_get_string(d, "bind");
    obs_data_release(d);

    // Guard rails, so a figure typed into the very page being used to fix it
    // cannot make the page unreachable.
    if (port < 1 || port > 65535) port = 8080;
    if (bind.empty()) bind = "0.0.0.0";
}

void WebUiSettings::save() const {
    char* dir = obs_module_config_path("");
    if (dir) { os_mkdirs(dir); bfree(dir); }

    obs_data_t* d = obs_data_create();
    obs_data_set_bool(d, "enabled", enabled);
    obs_data_set_int(d, "port", port);
    obs_data_set_string(d, "bind", bind.c_str());

    char* path = obs_module_config_path("web.json");
    if (path) {
        if (!obs_data_save_json_safe(d, path, "tmp", "bak"))
            mlog_warn("could not save the remote-control settings to %s", path);
        bfree(path);
    }
    obs_data_release(d);
}

// ── State ────────────────────────────────────────────────────────────────────

bool web_ui_running() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_server != nullptr;
}

bool web_ui_stopping() { return g_stopping.load(); }

std::string web_ui_problem() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_problem;
}

bool web_ui_locked() { return g_locked.load(); }

void web_ui_set_locked(bool locked) {
    if (g_locked.exchange(locked) != locked)
        mlog_info("remote control %s", locked ? "locked" : "unlocked");
}

std::string web_ui_address() {
    const WebUiSettings& cfg = web_ui_settings();
    const std::string ip = local_ipv4();
    if (ip.empty()) return "http://localhost:" + std::to_string(cfg.port);
    return "http://" + ip + ":" + std::to_string(cfg.port);
}

// ── Starting and stopping ────────────────────────────────────────────────────

void start_web_ui() {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_problem.clear();

    // Starting means not stopping. Without this, changing the port (or anything
    // else in the dock's Remote control group) stopped the server, started it
    // again, and left every control answering "OBS is closing" for the rest of
    // the session.
    g_stopping = false;

    if (g_server) return;

    const WebUiSettings cfg = web_ui_settings();
    if (!cfg.enabled) {
        mlog_info("remote control is switched off");
        return;
    }

    auto server = std::make_unique<HttpServer>(cfg.bind, cfg.port);

    // The server lives in the shared core, which has no log of its own.
    multisite::http_server_set_log_sink(
        [](multisite::HttpLogLevel level, const std::string& text) {
            if (level == multisite::HttpLogLevel::Error)
                mlog_error("%s", text.c_str());
            else
                mlog_warn("%s", text.c_str());
        });

    const Role role = plugin_role();

    // The role decides what EXISTS here, not what is merely hidden: a satellite
    // has no encoder routes to reach, so there is nothing for a stray request
    // to find. Both pages still ship in one module, because both types stay
    // registered for the same reason — a preference is no reason to take away
    // something a scene collection might depend on.
    if (role != Role::DecoderOnly) {
        route_file(*server, "/encoder/",           "web/encoder/index.html");
        route_file(*server, "/encoder/app.js",     "web/encoder/app.js");
        route_file(*server, "/encoder/style.css",  "web/shared/style.css");
        // The mark in the page footer. Served from the plugin's own data
        // directory rather than linked from the website, because these pages
        // are reached over a church's network and often have no way out to the
        // internet at all — a logo that only loads online is missing exactly
        // where it is most useful.
        route_file(*server, "/encoder/saw-logo.svg", "web/shared/saw-logo.svg");
        register_encoder_api(*server);
    }
    if (role != Role::EncoderOnly) {
        route_file(*server, "/decoder/",           "web/decoder/index.html");
        route_file(*server, "/decoder/app.js",     "web/decoder/app.js");
        route_file(*server, "/decoder/style.css",  "web/shared/style.css");
        route_file(*server, "/decoder/saw-logo.svg", "web/shared/saw-logo.svg");
        register_decoder_api(*server);
    }

    // "/" is this machine's page. A machine set to Both gets the small chooser
    // instead, because guessing which half somebody wants is how a volunteer
    // ends up on a page with controls that do nothing.
    if (role == Role::EncoderOnly)
        route_file(*server, "/", "web/encoder/index.html");
    else if (role == Role::DecoderOnly)
        route_file(*server, "/", "web/decoder/index.html");
    else
        route_file(*server, "/", "web/index.html");

    // ── Shared by both pages ────────────────────────────────────────────────
    // Static and role-agnostic, so it is registered exactly once regardless
    // of which page(s) this machine serves — the encoder and decoder
    // settings pages both read the identical list for the identical
    // dropdown (see storage_providers.h).
    server->route("GET", "/api/storage/providers",
                 [](const HttpRequest&, HttpResponse& res) {
        json list = json::array();
        for (const auto& info : multisite::all_providers()) {
            list.push_back(json{{"key", info.key},
                                {"display_name", info.display_name},
                                {"needs_account_id", info.needs_account_id},
                                {"needs_region", info.needs_region},
                                {"needs_endpoint", info.needs_endpoint},
                                {"available", info.available}});
        }
        res.json(json{{"providers", list}}.dump());
    });

    // An operator with a phone and no access to the desk should still be able
    // to see why nothing is happening.
    server->route("GET", "/api/log", [](const HttpRequest& req,
                                        HttpResponse& res) {
        size_t lines = 150;
        try {
            const std::string v = req.param("lines");
            if (!v.empty()) lines = (size_t)std::stoul(v);
        } catch (...) {}
        json rows = json::array();
        for (const auto& e : plugin_log_recent(lines))
            rows.push_back(json{{"at_ms", e.at_ms},
                                {"level", plugin_log_level_name(e.level)},
                                {"text", e.text}});
        res.json(json{{"lines", std::move(rows)}}.dump());
    });

    // Registered outside the lock check, because unlocking has to work while
    // locked — that is the entire point of it.
    server->route("POST", "/api/lock", [](const HttpRequest& req,
                                          HttpResponse& res) {
        const std::string v = req.param("on");
        const bool on = v.empty() ? !web_ui_locked()
                                  : (v == "1" || v == "true" || v == "yes");
        web_ui_set_locked(on);
        res.json(json{{"locked", on}}.dump());
    });

    std::string err;
    if (!server->start(err)) {
        g_problem = err;
        // Not fatal to OBS. Worth saying plainly, because the usual cause is
        // something else already on the port — and a church that cannot reach
        // the page will otherwise assume the plugin is broken.
        mlog_error("remote control could not start: %s", err.c_str());
        return;
    }

    g_server = std::move(server);
    mlog_info("remote control: %s  (%s)", web_ui_address().c_str(),
              role == Role::EncoderOnly ? "encoder page"
              : role == Role::DecoderOnly ? "decoder page"
                                          : "encoder and decoder pages");
}

void stop_web_ui() {
    std::unique_ptr<HttpServer> server;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        server = std::move(g_server);
        g_problem.clear();
    }
    // Stopped outside the lock: it joins its accept thread, and holding the
    // mutex while a connection finishes would only risk a deadlock with a
    // handler that wants the same mutex.
    if (server) {
        server->stop();
        mlog_info("remote control stopped");
    }
    multisite::http_server_set_log_sink(nullptr);
}

void shut_down_web_ui() {
    // Set before the server goes, and never cleared while this process lives:
    // a request already inside a handler must not start touching OBS while the
    // module is being unloaded around it. A later start_web_ui() (OBS reloading
    // the module) clears it again, because serving is exactly what it means.
    g_stopping = true;
    stop_web_ui();
}

} // namespace multisite_obs
