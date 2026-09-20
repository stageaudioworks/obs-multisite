// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// sysinfo.h — the box itself, as far as an operator needs to see it.
//
// A campus appliance has no keyboard and no screen worth reading, so the
// things somebody would normally check at a terminal — what my address is,
// whether the clock is right, how full the cache disk is — have to be
// answerable over the network and, for the address, on the splash screen
// before any network tool has been opened.
//
#include <string>
#include <vector>

namespace multisite_player {

struct NetInterface {
    std::string name;           // eth0, wlan0
    std::string ipv4;
    std::string mac;
    bool        up = false;
    bool        wireless = false;
};

// Every address the box can be reached on, loopback excluded. The first
// non-empty one is what the splash screen shows: it is the number somebody
// types into a phone.
std::vector<NetInterface> network_interfaces();

std::string hostname();

struct TimeInfo {
    long long   now_ms = 0;
    std::string timezone;
    std::string local_time;         // "2026-09-05 14:03:11"
    bool        ntp_synchronised = false;
    bool        ntp_enabled = false;
};
TimeInfo time_info();

// Time zones the box knows about, for the picker. Read from the system's own
// zone database rather than a list baked in here, which would go stale.
std::vector<std::string> available_timezones();

// Both go through timedatectl, so they behave exactly as they would if
// somebody had set them at a terminal. Returns an empty string on success or
// a human-readable reason on failure — usually "not running as root".
std::string set_timezone(const std::string& tz);
std::string set_ntp(bool enabled);
// Only meaningful with NTP off; a box in a building with no internet still
// needs a roughly correct clock for the times on its timeline to mean
// anything.
std::string set_time(long long epoch_ms);

struct DiskInfo {
    std::string path;
    long long   total_bytes = 0;
    long long   free_bytes = 0;
    // Whether this looks like removable/USB storage. The cache writes about
    // 3 GB an hour and will wear an SD card out, so it matters whether the
    // cache is on one.
    bool        is_sd_card = false;
    // "healthy" | "low" | "critical" — see multisite::classify_disk_free().
    // A box in the field has no screen worth reading, so this has to be
    // caught over the web UI rather than noticed as a stalled event.
    std::string health = "healthy";
};
DiskInfo disk_info(const std::string& path);

// One reading of a processor's jiffy counters, and the busy fraction between
// two of them.
//
// This is separated out and made pure because it is the only part of the
// measurement that can be wrong in a way nobody would notice: a misplaced field
// index counts iowait as work, and a box waiting on a slow SD card then reads
// as a box that is working hard — which is the opposite of the truth, and
// exactly the misreading that would send somebody looking for a faster Pi.
struct CpuTimes {
    unsigned long long idle = 0;     // idle + iowait
    unsigned long long total = 0;
};

// Parses one "cpu"/"cpuN" line of /proc/stat. False if it is not one.
bool parse_proc_stat_line(const std::string& line, CpuTimes& out);

// Busy percentage between two readings, or -1 when the pair says nothing:
// no time elapsed, or a counter that went backwards because the box was
// suspended or the counters wrapped.
double cpu_busy_percent(const CpuTimes& prev, const CpuTimes& now);

struct SystemInfo {
    std::string model;              // "Raspberry Pi 5 Model B Rev 1.0"
    std::string os_version;
    std::string kernel;
    double      uptime_s = 0;
    double      load_1min = 0;
    double      cpu_temp_c = 0;     // 0 when the box cannot report one
    // The Pi reports under-voltage and thermal throttling; both explain an
    // event that stutters, and neither is visible any other way.
    bool        throttled = false;
    bool        under_voltage = false;

    // ── How hard the box is working ──────────────────────────────────────────
    // Load average is what /proc offers first and it is the wrong number to put
    // in front of an operator: on four cores, "2.0" is half idle and looks
    // alarming. A percentage of the whole machine is the figure somebody can
    // act on, so it is measured properly — from the jiffies counters, across
    // the gap between two readings.
    double      cpu_percent = -1;   // the whole box, 0-100; -1 = not known yet
    // Per core, because the picture only stutters when ONE of them is pinned.
    // Decoding spreads across all of them; the thread that presents the picture
    // and writes the sound does not, so a single core at 100% while the others
    // idle is the shape of a box about to gap the audio, and an average hides
    // exactly that.
    std::vector<double> cpu_per_core;
    // Both stay at -1 until two readings exist: the first call after boot has
    // nothing to compare against, and reporting 0% then would read as an idle
    // box rather than an unmeasured one.

    long long   mem_total_bytes = 0;
    long long   mem_available_bytes = 0;
    long long   swap_total_bytes = 0;
    long long   swap_free_bytes = 0;
};
SystemInfo system_info();

// ── Remote access ────────────────────────────────────────────────────────────
//
// A campus box sits on a network nobody can dial into, which is what makes a
// setting wrong at the campus so expensive: the fix is a drive, not a click.
// Two optional tools remove that drive. ZeroTier is a private overlay network
// the box joins and keeps an address on wherever it is plugged in; cloudflared
// publishes the operator page on a public hostname with no port-forward.
//
// Neither is required. Every field below is false on a box that has neither
// installed, and the rest of the player does not care.
struct RemoteAccess {
    bool        zerotier_installed = false;
    bool        zerotier_running   = false;
    // The address on the overlay network, blank until the box has joined one
    // and been authorised at ZeroTier Central. This is the address the splash
    // screen prints as the remote access address.
    std::string zerotier_ip;
    bool        cloudflared_installed = false;
    bool        cloudflared_running   = false;
    // The public hostname the tunnel serves, when its own configuration names
    // one. A tunnel driven entirely from the dashboard keeps the name there,
    // so this is blank in that case rather than wrong.
    std::string cloudflared_hostname;
};
RemoteAccess remote_access();

// The ZeroTier address alone. The identity screen is redrawn about ten times a
// second and needs only this one string, so it must not pay for the systemctl
// calls the full probe below makes — forking twice every tenth of a second to
// draw a screen that has not changed is exactly the sort of cost an appliance
// must not carry. Just reading the interfaces is free.
std::string zerotier_ip();

// Bring a remote-access setting into force. Each returns an empty string on
// success, or a sentence saying what did not happen. A missing program is
// reported, not treated as fatal: a development machine has neither, and the
// rest of the player has to keep working regardless.
std::string apply_zerotier(const std::string& network_id);
std::string apply_cloudflared(const std::string& token);

// Restart the player, reboot, or shut down. Each returns an empty
// string once the request has been made.
std::string restart_service();
std::string reboot_box();
std::string shutdown_box();

// The version this build reports, in the UI and the log.
const char* player_version();

} // namespace multisite_player
