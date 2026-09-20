// SPDX-License-Identifier: GPL-3.0-or-later
// test_cpu_usage.cpp — the processor-busy figure the appliance's web UI shows.
//
// /proc/stat counts jiffies since boot, so "how busy is this box right now"
// only exists as the difference between two readings. The parsing and the
// subtraction are kept out of the file-reading so they can be tested with no
// /proc at all — which also means they are tested on a Mac, where the box this
// runs on has no /proc to disagree with.
//
// The failure worth guarding against is silent: get the field index wrong and
// iowait counts as work, so a box stalled on a slow SD card reads as a box
// working hard. That is the opposite of the truth, and it is the reading that
// would send somebody out to buy a faster Pi.
#include "../src/appliance/sysinfo.h"

#include <cstdio>
#include <string>

using namespace multisite_player;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

static bool near(double a, double b) { return a > b - 0.01 && a < b + 0.01; }

int main() {
    std::printf("Parsing /proc/stat lines\n");
    {
        CpuTimes t;
        //                 user nice system  idle iowait irq softirq steal
        CHECK(parse_proc_stat_line("cpu  100 0 100 800 0 0 0 0", t),
              "the whole-box line parses");
        CHECK(t.total == 1000, "every field counts towards the total");
        CHECK(t.idle == 800, "idle is field 3");

        CpuTimes c;
        CHECK(parse_proc_stat_line("cpu3 10 0 10 70 10 0 0 0", c),
              "a per-core line parses");
        CHECK(c.idle == 80, "iowait is idle too, not work");
        CHECK(c.total == 100, "and it still counts towards the total");

        CpuTimes junk;
        CHECK(!parse_proc_stat_line("intr 12345 0 0", junk),
              "a line that is not a cpu line is refused");
        CHECK(!parse_proc_stat_line("", junk), "an empty line is refused");
        CHECK(!parse_proc_stat_line("cpu 0 0 0 0 0 0 0 0", junk),
              "a line whose counters are all zero says nothing");
    }

    std::printf("\nBusy percentage between two readings\n");
    {
        // 1000 jiffies passed, 500 of them idle.
        CHECK(near(cpu_busy_percent({0, 0}, {500, 1000}), 50.0),
              "half the time idle is 50%");
        CHECK(near(cpu_busy_percent({800, 1000}, {1600, 2000}), 20.0),
              "measured across the gap, not since boot");
        CHECK(near(cpu_busy_percent({0, 0}, {0, 1000}), 100.0),
              "no idle time at all is 100%");
        CHECK(near(cpu_busy_percent({0, 0}, {1000, 1000}), 0.0),
              "entirely idle is 0%");
    }

    std::printf("\nPairs that say nothing are not reported as zero\n");
    {
        // A box that has just started has one reading and nothing to compare
        // it against. Reporting 0% would read as an idle box rather than an
        // unmeasured one.
        CHECK(cpu_busy_percent({500, 1000}, {500, 1000}) < 0,
              "no time elapsed is not a reading");
        CHECK(cpu_busy_percent({500, 2000}, {500, 1000}) < 0,
              "a total that went backwards is not a reading");
        CHECK(cpu_busy_percent({900, 1000}, {400, 2000}) < 0,
              "an idle count that went backwards is not a reading");
        CHECK(cpu_busy_percent({0, 0}, {2000, 1000}) < 0,
              "more idle than elapsed is nonsense, not 0%");
    }

    std::printf("\nRealistic readings\n");
    {
        // A Pi 5 decoding AV1: busy, but not pinned.
        const double busy = cpu_busy_percent({100000, 400000}, {100600, 402000});
        CHECK(busy > 65.0 && busy < 75.0, "a working decoder reads around 70%");
        // The same box idling between events.
        const double idle = cpu_busy_percent({100000, 400000}, {101900, 402000});
        CHECK(idle < 10.0, "an idle box reads near zero");
    }

    std::printf(g_fail ? "\n%d check(s) failed\n" : "\nall checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
