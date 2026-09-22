// SPDX-License-Identifier: GPL-3.0-or-later
//
// main.cpp — the relay, as a container.
//
// Deployment simplicity is a feature, so everything here is either an
// environment variable with a sensible default or something the operator sets
// in the browser. A church's integrator should get this running with one
// `docker run` and a port.
//
#include "api.h"
#include "relay_send.h"
#include "log.h"
#include "service.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <thread>

using namespace multisite_relay;

#ifndef MULTISITE_RELAY_VERSION
#define MULTISITE_RELAY_VERSION "dev"
#endif

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }

std::string env(const char* name, const std::string& fallback) {
    const char* v = ::getenv(name);
    return (v && *v) ? std::string(v) : fallback;
}

int env_int(const char* name, int fallback) {
    const char* v = ::getenv(name);
    if (!v || !*v) return fallback;
    try { return std::stoi(v); } catch (...) { return fallback; }
}

// Seeding from the environment exists so a church's integrator can bring a
// container up already pointing at the right bucket, rather than having to
// open the UI to type credentials before anything works. Anything already in
// the database wins: a value typed in the browser must not be silently
// overwritten by a stale variable on the next restart.
void seed_from_environment(Service& service) {
    auto c = service.config().storage();
    bool changed = false;
    auto take = [&](const char* name, std::string& field) {
        const std::string v = env(name, "");
        if (!v.empty() && field.empty()) { field = v; changed = true; }
    };
    take("RELAY_BUCKET", c.bucket);
    take("RELAY_ENDPOINT_HOST", c.endpoint_host);
    take("RELAY_R2_ACCOUNT_ID", c.r2_account_id);
    take("RELAY_ACCESS_KEY_ID", c.access_key_id);
    take("RELAY_SECRET_ACCESS_KEY", c.secret_access_key);
    take("RELAY_REGION", c.region);
    if (const char* v = ::getenv("RELAY_USE_HTTPS")) {
        const bool want = !(v[0] == '0' || v[0] == 'f' || v[0] == 'F');
        if (want != c.use_https) { c.use_https = want; changed = true; }
    }
    if (changed) service.config().set_storage(c);

    auto lan = service.config().lan();
    bool lan_changed = false;
    const std::string lan_host = env("RELAY_LAN_HOST", "");
    if (!lan_host.empty() && lan.host.empty()) { lan.host = lan_host; lan_changed = true; }
    const std::string lan_port = env("RELAY_LAN_PORT", "");
    if (!lan_port.empty() && lan.port == 9080) {
        try { lan.port = std::stoi(lan_port); lan_changed = true; } catch (...) {}
    }
    const std::string lan_token = env("RELAY_LAN_AUTH_TOKEN", "");
    if (!lan_token.empty() && lan.auth_token.empty()) {
        lan.auth_token = lan_token; lan_changed = true;
    }
    if (lan_changed) { service.config().set_lan(lan); changed = true; }

    auto r = service.config().room();
    const std::string room = env("RELAY_ROOM", "");
    if (!room.empty() && r.room_id == "main-auditorium" && room != r.room_id) {
        r.room_id = room;
        service.config().set_room(r);
        changed = true;
    }
    if (changed) service.reload();
}

} // namespace

int main(int argc, char** argv) {
    // Answering --version matters more here than for a desktop app: this ships
    // as a container, and `docker run --rm <image> --version` is how somebody
    // finds out what they actually deployed without starting a relay.
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--version" || a == "-V") {
            std::printf("%s\n", MULTISITE_RELAY_VERSION);
            return 0;
        }
    }
    ::signal(SIGINT, on_signal);
    ::signal(SIGTERM, on_signal);

    if (env("RELAY_DEBUG", "").size()) set_debug_logging(true);

    const std::string data_dir = env("RELAY_DATA_DIR", "/data");
    const std::string db_path  = data_dir + "/relay.db";
    const std::string web_root = env("RELAY_WEB_ROOT", "/app/web");
    const int port = env_int("RELAY_PORT", 8080);

    ::mkdir(data_dir.c_str(), 0700);
    ::mkdir((data_dir + "/cache").c_str(), 0700);
    if (!::getenv("RELAY_CACHE_DIR"))
        ::setenv("RELAY_CACHE_DIR", (data_dir + "/cache").c_str(), 0);

    Service service;
    const std::string e = service.start(db_path);
    if (!e.empty()) {
        rlog_error("could not start: %s", e.c_str());
        return 1;
    }
    seed_from_environment(service);

    Auth auth(service.config());
    // Seeding a login from the environment lets an integrator hand over a box
    // that is already claimed, rather than leaving the first person to find
    // the port to claim it. Only applied when there is no login yet, so it
    // cannot silently reset one that has been changed in the browser.
    if (!auth.configured()) {
        const std::string u = env("RELAY_USER", "");
        const std::string p = env("RELAY_PASSWORD", "");
        if (!u.empty() && !p.empty()) {
            const std::string e = auth.set_credentials(u, p);
            if (e.empty()) rlog_info("operator login set from the environment");
            else rlog_error("could not set the login: %s", e.c_str());
        }
    }

    // Binds to localhost unless told otherwise. The relay can change where an
    // event is sent, so publishing it to the internet has to be a
    // deliberate act — put a TLS-terminating proxy in front of it and point
    // that at this, rather than opening the port.
    const std::string bind = env("RELAY_BIND", "127.0.0.1");
    // The HTTP server lives in the shared core now, and the core has no log of
    // its own: point it at this container's.
    multisite::http_server_set_log_sink(
        [](multisite::HttpLogLevel level, const std::string& text) {
            if (level == multisite::HttpLogLevel::Error)
                rlog_error("%s", text.c_str());
            else
                rlog_warn("%s", text.c_str());
        });
    multisite::HttpServer server(bind, port);
    server.set_static_root(web_root);
    register_routes(server, service, auth);

    std::string err;
    if (!server.start(err)) {
        // Almost always a second copy already running, which is worth saying
        // rather than leaving as "bind failed".
        rlog_error("could not listen on port %d: %s", port, err.c_str());
        return 1;
    }
    rlog_info("multisite relay %s ready on %s:%d",
              MULTISITE_RELAY_VERSION, bind.c_str(), port);
    if (!auth.configured())
        rlog_warn("no login is set yet — open the page and set one before "
                  "anyone else can reach this");
    // In a container, binding every interface is correct and is not what
    // decides exposure — the host side of the port mapping does. Warning about
    // it there would be noise on every start, and would train an operator to
    // ignore the line that matters.
    const bool in_container = ::access("/.dockerenv", F_OK) == 0;
    if (bind != "127.0.0.1" && bind != "localhost" && !in_container)
        rlog_warn("listening on %s, not just localhost — make sure something "
                  "in front of this is terminating TLS", bind.c_str());
    if (!service.config().configured())
        rlog_info("storage is not set up yet — open the page and fill in the "
                  "bucket, a LAN host, or both");
    if (!ffmpeg_supports_srt())
        rlog_warn("this ffmpeg was built without SRT, so only rtmp:// and "
                  "rtmps:// destinations will work here");

    while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(200));

    rlog_info("stopping");
    server.stop();
    service.stop();
    return 0;
}
