// SPDX-License-Identifier: GPL-3.0-or-later
//
// main.cpp — the appliance.
//
// Boots into its job, restarts itself on failure (systemd does that part),
// keeps its cache across reboots, and presents one simple screen. There is no
// desktop, no window manager and nobody sitting at it: the whole operator
// interface is the web UI, and the only thing on the display is the programme.
//
#include "api.h"
#include "config.h"
#include "core/http_server.h"
#include "log.h"
#include "../core/log.h"
#include "player.h"
#include "sysinfo.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include <sys/stat.h>
#include <sys/types.h>

namespace {

std::atomic<bool> g_stop{false};

// The HTTP server is shared with the relay and the OBS plugin, so it lives in
// the core and its names arrive qualified.
using multisite::HttpServer;

void on_signal(int sig) {
    // Async-signal-safe: set a flag, nothing more. The main loop does the work.
    (void)sig;
    g_stop = true;
}

void usage() {
    std::printf(
        "multisite-player — satellite campus receive appliance\n"
        "\n"
        "  --config PATH    settings file (default %s)\n"
        "  --web-root PATH  directory holding the operator interface\n"
        "  --port N         override the web interface port\n"
        "  --no-display     receive and serve the interface, drive no display\n"
        "  --verbose        include debug lines in the log\n"
        "  --version        print the version and exit\n"
        "  --help           this\n",
        multisite_player::default_config_path());
}

// mkdir -p, because the cache directory is normally on a USB SSD that may have
// been plugged in after the last boot.
bool make_directories(const std::string& path) {
    if (path.empty()) return false;
    std::string acc;
    size_t i = 0;
    if (path[0] == '/') { acc = "/"; i = 1; }
    while (i <= path.size()) {
        const size_t slash = path.find('/', i);
        const std::string part =
            path.substr(i, slash == std::string::npos ? std::string::npos
                                                      : slash - i);
        if (!part.empty()) {
            acc += part;
            if (::mkdir(acc.c_str(), 0755) != 0 && errno != EEXIST) return false;
            acc += "/";
        }
        if (slash == std::string::npos) break;
        i = slash + 1;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    using namespace multisite_player;

    // Same seam as the plugin installs: the core does the uploading and the
    // downloading, and until now had no way to report a failure. Forwarded into
    // the player's own log so it lands in the file an operator already reads.
    multisite::set_log_sink([](multisite::LogLevel lvl, const std::string& msg) {
        switch (lvl) {
            case multisite::LogLevel::Error: plog_error("%s", msg.c_str()); break;
            case multisite::LogLevel::Warn:  plog_warn("%s", msg.c_str());  break;
            default:                         plog_info("%s", msg.c_str());  break;
        }
    });

    std::string config_path = default_config_path();
    std::string web_root    = MULTISITE_PLAYER_WEB_ROOT;
    int  port_override = 0;
    bool no_display = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--config")        config_path = next("--config");
        else if (a == "--web-root") web_root    = next("--web-root");
        else if (a == "--port")     port_override = std::atoi(next("--port").c_str());
        else if (a == "--no-display") no_display = true;
        else if (a == "--verbose")  set_debug_logging(true);
        else if (a == "--version")  { std::printf("%s\n", player_version()); return 0; }
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else {
            std::fprintf(stderr, "unknown option: %s\n", a.c_str());
            usage();
            return 2;
        }
    }

    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);
    // A browser closing a preview mid-write would otherwise take the whole
    // appliance down with it.
    std::signal(SIGPIPE, SIG_IGN);

    plog_info("multisite player %s starting on %s", player_version(),
              hostname().c_str());

    // The server has no log of its own — the core does not decide where
    // anybody's messages go — so point it at this process's.
    multisite::http_server_set_log_sink(
        [](multisite::HttpLogLevel level, const std::string& text) {
            if (level == multisite::HttpLogLevel::Error)
                plog_error("%s", text.c_str());
            else
                plog_warn("%s", text.c_str());
        });

    Config cfg;
    {
        std::string err;
        if (!cfg.load(config_path, err)) {
            if (!err.empty()) {
                plog_error("%s: %s", config_path.c_str(), err.c_str());
                // Refusing to start would leave a box that cannot be reached
                // to be fixed. Carry on with defaults and say so loudly.
                plog_warn("carrying on with default settings — open the web "
                          "interface and check them");
            } else {
                plog_info("no settings yet at %s — this box needs its storage "
                          "details entering", config_path.c_str());
            }
        }
    }
    if (port_override > 0) cfg.web_port = port_override;

    if (!make_directories(cfg.cache_dir))
        plog_warn("cannot create the cache directory %s — downloads will fail",
                  cfg.cache_dir.c_str());
    {
        const DiskInfo disk = disk_info(cfg.cache_dir);
        if (disk.is_sd_card)
            plog_warn("the cache is on the SD card. It writes about 3 GB an "
                      "hour and will wear the card out — move it to a USB SSD.");
        if (disk.total_bytes > 0)
            plog_info("cache disk %s: %.1f GB free of %.1f GB",
                      disk.path.c_str(), disk.free_bytes / 1e9,
                      disk.total_bytes / 1e9);
    }

    std::unique_ptr<VideoOutput> video(
        no_display ? new NullVideoOutput() : make_video_output());
    std::unique_ptr<AudioOutput> audio(make_audio_output());
    {
        std::string err;
        if (!video->open(cfg, err)) {
            // Not fatal. The feed still arrives and the interface still
            // answers, so an operator can see what is wrong.
            plog_error("display: %s", err.c_str());
        } else {
            plog_info("display: %s", video->description().c_str());
        }
    }

    Player player(cfg, *video, *audio, config_path);
    player.start();

    HttpServer server(cfg.web_bind, cfg.web_port);
    server.set_static_root(web_root);
    register_api(server, player, config_path);
    {
        std::string err;
        if (!server.start(err)) {
            plog_error("%s", err.c_str());
            // With no control surface this box cannot be operated at all, so
            // this one IS fatal — systemd will restart it, which is the right
            // response to a port that was briefly still held by a dying copy.
            player.stop();
            return 1;
        }
    }

    for (const auto& n : network_interfaces()) {
        if (n.ipv4.empty()) continue;
        plog_info("operator interface: http://%s:%d  (%s)", n.ipv4.c_str(),
                  cfg.web_port, n.name.c_str());
    }

    while (!g_stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(200));

    plog_info("shutting down");
    server.stop();
    player.stop();
    video->close();
    audio->close();
    plog_info("stopped");
    return 0;
}
