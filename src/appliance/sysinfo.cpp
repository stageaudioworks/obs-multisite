// SPDX-License-Identifier: GPL-3.0-or-later
#include "sysinfo.h"
#include "log.h"
#include "../core/disk_health.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/statvfs.h>
#include <unistd.h>

#ifdef __linux__
#include <dirent.h>
#include <linux/if_packet.h>
#include <sys/sysinfo.h>
#else
#include <net/if_dl.h>
#endif

#ifndef MULTISITE_PLAYER_VERSION
#define MULTISITE_PLAYER_VERSION "dev"
#endif

namespace multisite_player {

namespace {

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                          s.back() == ' ' || s.back() == '\t' || s.back() == '\0'))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

std::string read_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return trim(ss.str());
}

// Run a command and collect its output. Used only for the handful of things
// that genuinely belong to systemd (the clock, restarting, rebooting): asking
// the tools that own them keeps this box behaving like any other Debian
// machine, rather than inventing a second way to set the time.
std::string run(const std::string& cmd, int* exit_code = nullptr) {
    std::string out;
    FILE* p = ::popen((cmd + " 2>&1").c_str(), "r");
    if (!p) {
        if (exit_code) *exit_code = -1;
        return "cannot run: " + cmd;
    }
    std::array<char, 512> buf{};
    while (::fgets(buf.data(), (int)buf.size(), p)) out += buf.data();
    const int rc = ::pclose(p);
    if (exit_code) *exit_code = rc;
    return trim(out);
}

bool have_command(const char* name) {
    int rc = 0;
    run(std::string("command -v ") + name, &rc);
    return rc == 0;
}

} // namespace

const char* player_version() { return MULTISITE_PLAYER_VERSION; }

// ── Processor time ───────────────────────────────────────────────────────────

bool parse_proc_stat_line(const std::string& line, CpuTimes& out) {
    std::istringstream is(line);
    std::string label;
    is >> label;
    if (label.rfind("cpu", 0) != 0) return false;

    unsigned long long v = 0, total = 0, idle = 0;
    for (int field = 0; is >> v; ++field) {
        total += v;
        // Fields 3 and 4 are idle and iowait. Both are the processor having
        // nothing to do, and counting iowait as work would show a box waiting
        // on a slow SD card as one that is working hard.
        if (field == 3 || field == 4) idle += v;
    }
    if (total == 0) return false;
    out.idle = idle;
    out.total = total;
    return true;
}

double cpu_busy_percent(const CpuTimes& prev, const CpuTimes& now) {
    if (now.total < prev.total || now.idle < prev.idle) return -1;
    const unsigned long long dt = now.total - prev.total;
    if (dt == 0) return -1;
    const unsigned long long di = now.idle - prev.idle;
    if (di > dt) return -1;                       // nonsense pair
    double pct = 100.0 * (double)(dt - di) / (double)dt;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

// ── Network ──────────────────────────────────────────────────────────────────

std::vector<NetInterface> network_interfaces() {
    std::vector<NetInterface> out;
    ifaddrs* ifa = nullptr;
    if (::getifaddrs(&ifa) != 0) return out;

    auto find_or_add = [&out](const std::string& name) -> NetInterface& {
        for (auto& i : out) if (i.name == name) return i;
        out.push_back(NetInterface{});
        out.back().name = name;
        return out.back();
    };

    for (ifaddrs* p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || !p->ifa_name) continue;
        const std::string name = p->ifa_name;
        if (name == "lo" || name == "lo0") continue;
        // Docker and similar bridges would only confuse somebody looking for
        // the address to type into a phone.
        if (name.rfind("docker", 0) == 0 || name.rfind("veth", 0) == 0 ||
            name.rfind("br-", 0) == 0)
            continue;

        if (p->ifa_addr->sa_family == AF_INET) {
            char ip[INET_ADDRSTRLEN] = {};
            auto* sin = (sockaddr_in*)p->ifa_addr;
            ::inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
            NetInterface& n = find_or_add(name);
            n.ipv4 = ip;
            n.up = (p->ifa_flags & IFF_UP) && (p->ifa_flags & IFF_RUNNING);
        }
#ifdef __linux__
        else if (p->ifa_addr->sa_family == AF_PACKET) {
            auto* ll = (sockaddr_ll*)p->ifa_addr;
            char mac[32] = {};
            if (ll->sll_halen == 6) {
                std::snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                              ll->sll_addr[0], ll->sll_addr[1], ll->sll_addr[2],
                              ll->sll_addr[3], ll->sll_addr[4], ll->sll_addr[5]);
            }
            NetInterface& n = find_or_add(name);
            n.mac = mac;
            if (!n.up) n.up = (p->ifa_flags & IFF_UP) && (p->ifa_flags & IFF_RUNNING);
        }
#else
        else if (p->ifa_addr->sa_family == AF_LINK) {
            auto* dl = (sockaddr_dl*)p->ifa_addr;
            const unsigned char* a = (const unsigned char*)LLADDR(dl);
            char mac[32] = {};
            if (dl->sdl_alen == 6)
                std::snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                              a[0], a[1], a[2], a[3], a[4], a[5]);
            NetInterface& n = find_or_add(name);
            n.mac = mac;
        }
#endif
    }
    ::freeifaddrs(ifa);

#ifdef __linux__
    for (auto& n : out)
        n.wireless = !read_file("/sys/class/net/" + n.name + "/phy80211/name").empty();
#endif

    // Wired first, then anything with an address: an appliance should be on
    // ethernet, and that is the address worth showing first.
    std::stable_sort(out.begin(), out.end(),
        [](const NetInterface& a, const NetInterface& b) {
            if (a.ipv4.empty() != b.ipv4.empty()) return !a.ipv4.empty();
            return a.wireless < b.wireless;
        });
    return out;
}

std::string hostname() {
    char buf[256] = {};
    if (::gethostname(buf, sizeof(buf) - 1) != 0) return "multisite-player";
    return buf;
}

// ── Time ─────────────────────────────────────────────────────────────────────

TimeInfo time_info() {
    TimeInfo t;
    using namespace std::chrono;
    t.now_ms = duration_cast<milliseconds>(
                   system_clock::now().time_since_epoch()).count();

    const std::time_t now = (std::time_t)(t.now_ms / 1000);
    std::tm tm{};
    ::localtime_r(&now, &tm);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    t.local_time = buf;

    t.timezone = read_file("/etc/timezone");
    if (t.timezone.empty()) {
        char zone[8] = {};
        std::strftime(zone, sizeof(zone), "%Z", &tm);
        t.timezone = zone;
    }

    if (have_command("timedatectl")) {
        const std::string s = run("timedatectl show -p NTPSynchronized "
                                  "-p NTP --value");
        // Two lines: NTP enabled, then synchronised.
        std::istringstream is(s);
        std::string line;
        int idx = 0;
        while (std::getline(is, line)) {
            const bool yes = (trim(line) == "yes");
            if (idx == 0) t.ntp_enabled = yes; else t.ntp_synchronised = yes;
            ++idx;
        }
    }
    return t;
}

std::vector<std::string> available_timezones() {
    std::vector<std::string> out;
    if (have_command("timedatectl")) {
        std::istringstream is(run("timedatectl list-timezones"));
        std::string line;
        while (std::getline(is, line)) {
            line = trim(line);
            if (!line.empty()) out.push_back(line);
        }
    }
    if (out.empty()) {
        // A box without systemd still deserves the common ones rather than an
        // empty picker.
        out = {"Etc/UTC", "Europe/London", "Europe/Dublin", "Europe/Paris",
               "America/New_York", "America/Chicago", "America/Denver",
               "America/Los_Angeles", "Australia/Sydney", "Pacific/Auckland"};
    }
    return out;
}

std::string set_timezone(const std::string& tz) {
    // A time zone name goes on a command line, so it must be a time zone name
    // and nothing else.
    for (char c : tz)
        if (!(std::isalnum((unsigned char)c) || c == '/' || c == '_' ||
              c == '-' || c == '+'))
            return "that is not a valid time zone name";
    if (!have_command("timedatectl")) return "this box has no timedatectl";
    int rc = 0;
    const std::string out = run("timedatectl set-timezone '" + tz + "'", &rc);
    if (rc != 0) return out.empty() ? "could not set the time zone" : out;
    plog_info("time zone set to %s", tz.c_str());
    return {};
}

std::string set_ntp(bool enabled) {
    if (!have_command("timedatectl")) return "this box has no timedatectl";
    int rc = 0;
    const std::string out =
        run(std::string("timedatectl set-ntp ") + (enabled ? "true" : "false"), &rc);
    if (rc != 0) return out.empty() ? "could not change network time" : out;
    plog_info("network time %s", enabled ? "enabled" : "disabled");
    return {};
}

std::string set_time(long long epoch_ms) {
    if (!have_command("timedatectl")) return "this box has no timedatectl";
    const std::time_t t = (std::time_t)(epoch_ms / 1000);
    std::tm tm{};
    ::localtime_r(&t, &tm);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    int rc = 0;
    const std::string out = run(std::string("timedatectl set-time '") + buf + "'", &rc);
    if (rc != 0)
        return out.empty() ? "could not set the clock"
                           : out + " (turn network time off first)";
    plog_info("clock set to %s", buf);
    return {};
}

// ── Disk ─────────────────────────────────────────────────────────────────────

DiskInfo disk_info(const std::string& path) {
    DiskInfo d;
    d.path = path;
    struct statvfs st{};
    if (::statvfs(path.c_str(), &st) == 0) {
        d.total_bytes = (long long)st.f_blocks * (long long)st.f_frsize;
        d.free_bytes  = (long long)st.f_bavail * (long long)st.f_frsize;
        d.health = multisite::disk_health_name(
            multisite::classify_disk_free((uint64_t)d.free_bytes));
    }
#ifdef __linux__
    // mmcblk0 is the SD card slot on a Pi. Writing the segment cache there
    // wears the card out, so it is worth saying plainly in the interface.
    int rc = 0;
    const std::string src = run("findmnt -n -o SOURCE --target '" + path + "'", &rc);
    d.is_sd_card = rc == 0 && src.find("mmcblk") != std::string::npos;
#endif
    return d;
}

// ── The machine ──────────────────────────────────────────────────────────────

// ── How busy the processor is ────────────────────────────────────────────────
//
// /proc/stat counts jiffies since boot, so a single reading says how busy the
// box has been since it was switched on — which is never the question. The
// figure an operator wants is "right now", and that only exists as the
// difference between two readings.
//
// The previous reading is kept here rather than taken fresh each time, so the
// answer covers the gap since the last time anybody asked. The web UI polls
// steadily, which makes that gap the few seconds it should be.
namespace {

std::mutex             g_cpu_mtx;
std::vector<CpuTimes>  g_cpu_prev;      // [0] is the whole box, then per core
bool                   g_cpu_have_prev = false;

// Fills cpu_percent and cpu_per_core, leaving them at -1 when there is nothing
// to compare against yet.
void read_cpu_usage(SystemInfo& s) {
    std::ifstream in("/proc/stat");
    if (!in) return;

    std::vector<CpuTimes> now;
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("cpu", 0) != 0) break;   // the cpu lines come first
        CpuTimes sample;
        if (parse_proc_stat_line(line, sample)) now.push_back(sample);
    }
    if (now.empty()) return;

    std::lock_guard<std::mutex> lk(g_cpu_mtx);
    if (g_cpu_have_prev && g_cpu_prev.size() == now.size()) {
        for (size_t i = 0; i < now.size(); ++i) {
            const double pct = cpu_busy_percent(g_cpu_prev[i], now[i]);
            if (i == 0) s.cpu_percent = pct;
            else        s.cpu_per_core.push_back(pct);
        }
    }
    g_cpu_prev = now;
    g_cpu_have_prev = true;
}

} // namespace

SystemInfo system_info() {
    SystemInfo s;
#ifdef __linux__
    s.model = read_file("/sys/firmware/devicetree/base/model");
    if (s.model.empty()) s.model = read_file("/proc/device-tree/model");
    s.kernel = run("uname -r");

    std::ifstream os("/etc/os-release");
    std::string line;
    while (std::getline(os, line)) {
        if (line.rfind("PRETTY_NAME=", 0) == 0) {
            s.os_version = line.substr(12);
            if (s.os_version.size() >= 2 && s.os_version.front() == '"')
                s.os_version = s.os_version.substr(1, s.os_version.size() - 2);
            break;
        }
    }

    struct sysinfo si{};
    if (::sysinfo(&si) == 0) {
        s.uptime_s  = (double)si.uptime;
        s.load_1min = (double)si.loads[0] / 65536.0;
    }

    read_cpu_usage(s);

    // Memory, from the same file the build sizing reads. MemAvailable is the
    // kernel's own estimate of what a new process could actually get, which is
    // the honest number — "free" on Linux looks alarmingly small on a healthy
    // box because the page cache is doing its job.
    {
        std::ifstream mi("/proc/meminfo");
        std::string key;
        long long value = 0;
        std::string unit;
        while (mi >> key >> value >> unit) {
            const long long bytes = value * 1024;
            if      (key == "MemTotal:")     s.mem_total_bytes = bytes;
            else if (key == "MemAvailable:") s.mem_available_bytes = bytes;
            else if (key == "SwapTotal:")    s.swap_total_bytes = bytes;
            else if (key == "SwapFree:")     s.swap_free_bytes = bytes;
        }
    }

    const std::string temp = read_file("/sys/class/thermal/thermal_zone0/temp");
    if (!temp.empty()) {
        try { s.cpu_temp_c = std::stod(temp) / 1000.0; } catch (...) {}
    }

    // Sustained decode on a passively cooled Pi throttles during a long
    // event, and an under-powered supply produces exactly the same symptom
    // as a bad network. Both are worth reporting rather than guessing at.
    if (have_command("vcgencmd")) {
        const std::string th = run("vcgencmd get_throttled");
        const size_t eq = th.find("0x");
        if (eq != std::string::npos) {
            try {
                const unsigned long bits = std::stoul(th.substr(eq), nullptr, 16);
                s.under_voltage = (bits & 0x1) || (bits & 0x10000);
                s.throttled     = (bits & 0x4) || (bits & 0x40000) ||
                                  (bits & 0x2) || (bits & 0x20000);
            } catch (...) {}
        }
    }
#else
    s.model = "development machine";
    s.kernel = run("uname -sr");
    s.os_version = run("uname -sr");
#endif
    return s;
}

// ── Remote access ────────────────────────────────────────────────────────────

namespace {

// Wrap a value for the shell, so a mistyped tunnel token cannot turn into a
// second command. Everything here is passed to popen, which is a shell.
std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else           out += c;
    }
    out += "'";
    return out;
}

bool service_active(const char* unit) {
    int rc = 0;
    run(std::string("systemctl is-active --quiet ") + unit, &rc);
    return rc == 0;
}

// The networks the box has been told to join are recorded one file per network
// in ZeroTier's state directory, each named for the network's sixteen hex
// digits. Reading them directly beats asking the daemon, which wants its
// authtoken to answer — and answering "nothing joined" would make a re-join
// leave the old network behind. The daemon also keeps "<id>.local.conf" here,
// which is why the name is checked rather than just the extension.
std::vector<std::string> zerotier_joined() {
    std::vector<std::string> out;
#ifdef __linux__
    DIR* d = ::opendir("/var/lib/zerotier-one/networks.d");
    if (!d) return out;
    while (dirent* e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (name.size() != 16 + 5 || name.compare(16, 5, ".conf") != 0)
            continue;
        bool hex = true;
        for (size_t i = 0; i < 16 && hex; ++i)
            hex = std::isxdigit((unsigned char)name[i]) != 0;
        if (hex) out.push_back(name.substr(0, 16));
    }
    ::closedir(d);
#endif
    return out;
}

// The hostname a locally-configured tunnel serves. A token-driven tunnel keeps
// its name in the dashboard and this stays blank, which is reported as unknown
// rather than guessed at.
std::string cloudflared_hostname_from_config() {
    std::istringstream is(read_file("/etc/cloudflared/config.yml"));
    std::string line;
    while (std::getline(is, line)) {
        line = trim(line);
        const size_t at = line.find("hostname:");
        if (at == std::string::npos) continue;
        const std::string v = trim(line.substr(at + 9));
        if (!v.empty()) return v;
    }
    return {};
}

} // namespace

// The address alone, with nothing forked to find it: ZeroTier names the
// interface it creates after itself, and reading the interfaces is all this
// takes. This is what the identity screen calls ten times a second.
std::string zerotier_ip() {
    for (const auto& n : network_interfaces())
        if (n.name.rfind("zt", 0) == 0 && !n.ipv4.empty()) return n.ipv4;
    return {};
}

RemoteAccess remote_access() {
    RemoteAccess r;
    r.zerotier_installed = have_command("zerotier-cli");
    r.zerotier_ip        = zerotier_ip();
#ifdef __linux__
    // The address appearing is proof enough on its own; the unit check covers
    // the moment before the interface has been given one.
    r.zerotier_running = service_active("zerotier-one.service") ||
                         !r.zerotier_ip.empty();
#else
    r.zerotier_running = !r.zerotier_ip.empty();
#endif
    r.cloudflared_installed = have_command("cloudflared");
#ifdef __linux__
    r.cloudflared_running = service_active("cloudflared.service");
#endif
    r.cloudflared_hostname = cloudflared_hostname_from_config();
    return r;
}

std::string apply_zerotier(const std::string& network_id) {
#ifdef __linux__
    if (!have_command("zerotier-cli"))
        return "ZeroTier is not installed on this box.";
    int rc = 0;
    // Joining is idempotent, and a box given a new network should leave the
    // old one: two overlays carrying the same routes is a support call waiting
    // to happen.
    const std::vector<std::string> joined = zerotier_joined();

    if (network_id.empty()) {
        for (const auto& id : joined)
            run("zerotier-cli leave " + shell_quote(id) + " >/dev/null 2>&1", &rc);
        return {};
    }

    rc = 0;
    const std::string out =
        run("zerotier-cli join " + shell_quote(network_id), &rc);
    if (rc != 0)
        return "ZeroTier would not join " + network_id + ": " +
               (out.empty() ? "no answer from the service" : out);

    for (const auto& id : joined)
        if (id != network_id)
            run("zerotier-cli leave " + shell_quote(id) + " >/dev/null 2>&1", &rc);
    return {};
#else
    (void)network_id;
    return "Remote access is set up by the installer on the box, not here.";
#endif
}

std::string apply_cloudflared(const std::string& token) {
#ifdef __linux__
    if (!have_command("cloudflared"))
        return "cloudflared is not installed on this box.";
    int rc = 0;
    if (token.empty()) {
        // Best effort: a box that never had a tunnel has nothing to remove.
        run("cloudflared service uninstall >/dev/null 2>&1", &rc);
        return {};
    }
    // `cloudflared service install <token>` writes the unit and the token and
    // brings the tunnel up; running it again replaces the token in place.
    const std::string out =
        run("cloudflared service install " + shell_quote(token), &rc);
    if (rc != 0)
        return "cloudflared would not install the tunnel: " +
               (out.empty() ? "no answer from the service" : out);
    run("systemctl restart cloudflared >/dev/null 2>&1", &rc);
    return {};
#else
    (void)token;
    return "Remote access is set up by the installer on the box, not here.";
#endif
}

std::string restart_service() {
    plog_info("restarting the player at the operator's request");
    // Detached, because systemd will stop this very process: replying first
    // and acting a moment later is what lets the browser see the answer.
    int rc = 0;
    run("(sleep 1; systemctl restart multisite-player) >/dev/null 2>&1 &", &rc);
    return {};
}

std::string reboot_box() {
    plog_warn("rebooting at the operator's request");
    int rc = 0;
    run("(sleep 1; systemctl reboot) >/dev/null 2>&1 &", &rc);
    return {};
}

std::string shutdown_box() {
    plog_warn("shutting down at the operator's request");
    int rc = 0;
    run("(sleep 1; systemctl poweroff) >/dev/null 2>&1 &", &rc);
    return {};
}

} // namespace multisite_player
