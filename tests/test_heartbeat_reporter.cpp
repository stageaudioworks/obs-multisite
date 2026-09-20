// SPDX-License-Identifier: GPL-3.0-or-later
// test_heartbeat_reporter.cpp — the monitoring heartbeat's shape and schedule.
//
// Every case here is a clause of multisite-cloud's reporter-brief: the
// verbatim field selections, the 30 s / 5 min schedule, the 429/interval_s
// backoff, the appliance-only host block, and the screenshot test. The
// filter is the whole of the privacy guarantee, so it is pinned field by
// field rather than by example.
#include "../src/core/heartbeat_reporter.h"
#include "../src/vendor/nlohmann/json.hpp"

#include <cstdio>
#include <string>

using namespace multisite;
using json = nlohmann::json;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

static bool has(const json& j, const char* k) { return j.contains(k); }

int main() {
    std::printf("Kinds: the five wire names, nothing else\n");
    {
        CHECK(heartbeat_kind_known("obs-encoder"), "obs-encoder known");
        CHECK(heartbeat_kind_known("obs-decoder"), "obs-decoder known");
        CHECK(heartbeat_kind_known("pi-player"), "pi-player known");
        CHECK(heartbeat_kind_known("x86-player"), "x86-player known");
        CHECK(heartbeat_kind_known("relay"), "relay known");
        CHECK(!heartbeat_kind_known("obs"), "short names rejected");
        CHECK(!heartbeat_kind_known(""), "empty rejected");
    }

    std::printf("Encoder filter: listed fields survive, nothing else\n");
    {
        json in;
        for (const auto& f : heartbeat_encoder_fields()) in[f] = 1;
        in["link_health"] = "Degraded";
        in["role"] = "encoder";
        in["marker_labels"] = json::array({"Sermon Start"});
        // Everything the screenshot test forbids, though present in status JSON.
        in["cache_dir"] = "/var/lib/obs";
        in["site_name"] = "Main";
        in["secret_access_key"] = "hunter2";
        in["endpoint_host"] = "s3.example.com";
        in["clock_skew_ms"] = 12;
        in["other_page"] = "/decoder/";
        in["have_source"] = true;
        in["lan_configured"] = true;
        in["lan_active"] = true;
        in["default_event_name"] = "Sun 20 Sep";
        const json out = json::parse(heartbeat_filter_status("encoder", in.dump()));
        for (const auto& f : heartbeat_encoder_fields())
            CHECK(has(out, f.c_str()), "listed encoder field kept");
        CHECK(!has(out, "cache_dir"), "cache_dir (local path) dropped");
        CHECK(!has(out, "site_name"), "site_name dropped");
        CHECK(!has(out, "secret_access_key"), "secret dropped");
        CHECK(!has(out, "endpoint_host"), "endpoint detail dropped");
        CHECK(!has(out, "clock_skew_ms"), "unlisted field dropped");
        CHECK(!has(out, "other_page"), "UI chrome dropped");
        CHECK(!has(out, "have_source"), "UI chrome dropped");
        CHECK(!has(out, "lan_configured"), "LAN visibility dropped");
        CHECK(!has(out, "lan_active"), "LAN visibility dropped");
        CHECK(!has(out, "default_event_name"), "unlisted field dropped");
        CHECK(out["link_health"] == "Degraded", "link word verbatim, never reworded");
        CHECK(out["marker_labels"][0] == "Sermon Start", "marker labels carried");
        CHECK(out.size() == heartbeat_encoder_fields().size(), "nothing extra snuck in");
    }

    std::printf("Decoder filter: listed fields survive, nothing else\n");
    {
        json in;
        for (const auto& f : heartbeat_decoder_fields()) in[f] = 1;
        in["role"] = "decoder";
        in["link_health"] = "Healthy";
        in["markers"] = json::array({json{{"label", "Go to local"}}});
        in["cache_dir"] = "/tmp/x";
        in["site_name"] = "Campus B";
        in["lan_configured"] = true;
        in["lan_active"] = false;
        in["seek_target_ms"] = 5;
        in["clock_skew_ms"] = 3;
        in["have_source"] = true;
        in["other_page"] = "/encoder/";
        in["current_marker"] = "x";
        const json out = json::parse(heartbeat_filter_status("decoder", in.dump()));
        for (const auto& f : heartbeat_decoder_fields())
            CHECK(has(out, f.c_str()), "listed decoder field kept");
        CHECK(!has(out, "cache_dir"), "cache_dir dropped");
        CHECK(!has(out, "site_name"), "site_name dropped");
        CHECK(!has(out, "lan_configured"), "LAN visibility dropped even when true");
        CHECK(!has(out, "lan_active"), "LAN visibility dropped even when false");
        CHECK(!has(out, "seek_target_ms"), "unlisted decoder field dropped");
        CHECK(!has(out, "clock_skew_ms"), "unlisted decoder field dropped");
        CHECK(!has(out, "have_source"), "UI chrome dropped");
        CHECK(!has(out, "current_marker"), "unlisted decoder field dropped");
        CHECK(out.size() == heartbeat_decoder_fields().size(), "nothing extra snuck in");
    }

    std::printf("Filter refuses rather than guesses\n");
    {
        CHECK(heartbeat_filter_status("relay", "{}") == "{}", "unknown role yields {}");
        CHECK(heartbeat_filter_status("", "{}") == "{}", "empty role yields {}");
        CHECK(heartbeat_filter_status("encoder", "not json") == "{}",
              "malformed body yields {}, never throws");
        CHECK(heartbeat_filter_status("encoder", "[1,2]") == "{}",
              "non-object yields {}");
        CHECK(heartbeat_filter_status("encoder", "") == "{}", "empty yields {}");
    }

    std::printf("link_health: words verbatim, appliance ints mapped, junk omitted\n");
    {
        CHECK(heartbeat_link_word(0) == "Healthy", "0 is Healthy");
        CHECK(heartbeat_link_word(1) == "Degraded", "1 is Degraded");
        CHECK(heartbeat_link_word(2) == "Offline", "2 is Offline");
        CHECK(heartbeat_link_word(3).empty(), "3 is not a word — omitted");
        CHECK(heartbeat_link_word(-1).empty(), "absent (-1) omitted, never sent as Offline");
        CHECK(heartbeat_link_word_str("Healthy") == "Healthy", "word passthrough");
        CHECK(heartbeat_link_word_str("Degraded") == "Degraded", "word passthrough");
        CHECK(heartbeat_link_word_str("Offline") == "Offline", "word passthrough");
        CHECK(heartbeat_link_word_str("healthy").empty(), "case differs — omitted");
        CHECK(heartbeat_link_word_str("").empty(), "empty omitted");

        // Appliance shape through the decoder filter: 0/1/2 become words.
        for (int i = 0; i < 3; ++i) {
            json in = json{{"role", "decoder"}, {"link_health", i}};
            json out = json::parse(heartbeat_filter_status("decoder", in.dump()));
            CHECK(out["link_health"] == heartbeat_link_word(i), "appliance int mapped");
        }
        json bad = json{{"role", "decoder"}, {"link_health", 7}};
        CHECK(!json::parse(heartbeat_filter_status("decoder", bad.dump())).contains("link_health"),
              "unknown int omitted, not forwarded");
        json bads = json{{"role", "decoder"}, {"link_health", "fine"}};
        CHECK(!json::parse(heartbeat_filter_status("decoder", bads.dump())).contains("link_health"),
              "unknown word omitted, not forwarded");
    }

    std::printf("Schedule: 30 s active, 300 s idle, central wins, 429 lengthens\n");
    {
        CHECK(heartbeat_next_interval_s(true, 0, false, 0) == 30, "active base 30 s");
        CHECK(heartbeat_next_interval_s(false, 0, false, 0) == 300, "idle base 300 s");
        CHECK(heartbeat_next_interval_s(true, 60, false, 0) == 60, "central interval adopted");
        CHECK(heartbeat_next_interval_s(false, 60, false, 0) == 60, "central wins when idle too");
        CHECK(heartbeat_next_interval_s(true, 0, true, 0) == 60, "429 without Retry-After doubles");
        CHECK(heartbeat_next_interval_s(false, 0, true, 0) == 600, "idle 429 doubles too");
        CHECK(heartbeat_next_interval_s(true, 0, true, 120) == 120, "Retry-After honoured");
        CHECK(heartbeat_next_interval_s(true, 0, false, 120) == 30, "Retry-After without rate-limit is ignored");
        CHECK(heartbeat_next_interval_s(true, 5, false, 0) == 10, "central clamped up to 10 s");
        CHECK(heartbeat_next_interval_s(true, 99999, false, 0) == 3600, "central clamped down to 3600 s");
        CHECK(heartbeat_next_interval_s(true, -3, false, 0) == 30, "negative central ignored");
    }

    std::printf("Server interval parses, garbage does not\n");
    {
        CHECK(heartbeat_parse_server_interval("{\"interval_s\": 45}") == 45, "interval read");
        CHECK(heartbeat_parse_server_interval("{}") == 0, "absent is 0");
        CHECK(heartbeat_parse_server_interval("garbage") == 0, "garbage is 0");
        CHECK(heartbeat_parse_server_interval("{\"interval_s\": \"soon\"}") == 0, "string is 0");
        CHECK(heartbeat_parse_server_interval("{\"interval_s\": -5}") == 0, "negative is 0");
    }

    std::printf("Envelope: plugin omits host, appliance sends best-effort\n");
    {
        HeartbeatIdentity id{"apl_01J8", "obs-decoder", "0.1.23-alpha", 41203};
        const std::string filtered = heartbeat_filter_status(
            "decoder", "{\"role\":\"decoder\",\"link_health\":\"Healthy\"}");
        const json no_host =
            json::parse(heartbeat_build(id, nullptr, filtered, "2026-09-12T09:41:07Z"));
        CHECK(no_host["v"] == 1, "payload version 1");
        CHECK(no_host["sent_at"] == "2026-09-12T09:41:07Z", "sent_at carried");
        CHECK(no_host["appliance"]["id"] == "apl_01J8", "identity id");
        CHECK(no_host["appliance"]["kind"] == "obs-decoder", "identity kind");
        CHECK(no_host["appliance"]["product_version"] == "0.1.23-alpha", "version verbatim");
        CHECK(no_host["appliance"]["uptime_s"] == 41203, "uptime carried");
        CHECK(!no_host.contains("host"), "plugin omits host entirely");
        CHECK(no_host["status"]["link_health"] == "Healthy", "status embedded");

        HeartbeatHost h;
        h.cpu_pct = 34.2; h.mem_pct = 51.0; h.disk_free_bytes = 41203948032LL;
        h.temp_c = 58.1; h.has_temp = true; h.throttled = false;
        const json with_host =
            json::parse(heartbeat_build(id, &h, filtered, "2026-09-12T09:41:07Z"));
        CHECK(with_host["host"]["cpu_pct"] == 34.2, "cpu carried");
        CHECK(with_host["host"]["mem_pct"] == 51.0, "mem carried");
        CHECK(with_host["host"]["disk_free_bytes"] == 41203948032LL, "disk carried");
        CHECK(with_host["host"]["temp_c"] == 58.1, "temp carried");
        CHECK(with_host["host"]["throttled"] == false, "throttle flags carried");

        // Absent sensors read absent, never fabricated (brief).
        HeartbeatHost unknown;
        const json sparse =
            json::parse(heartbeat_build(id, &unknown, filtered, "2026-09-12T09:41:07Z"));
        CHECK(!sparse["host"].contains("cpu_pct"), "unknown cpu omitted, not 0");
        CHECK(!sparse["host"].contains("mem_pct"), "unknown mem omitted");
        CHECK(!sparse["host"].contains("disk_free_bytes"), "unknown disk omitted");
        CHECK(!sparse["host"].contains("temp_c"), "absent temp omitted");
        CHECK(sparse["host"].contains("throttled"), "throttled always present");

        const json broken =
            json::parse(heartbeat_build(id, nullptr, "garbage", "2026-09-12T09:41:07Z"));
        CHECK(broken["status"].is_object() && broken["status"].empty(),
              "malformed status embeds as {}, never throws");
    }

    std::printf("Pairing: start claims, poll waits then resolves\n");
    {
        const PairStartReply s = heartbeat_parse_pair_start(
            "{\"user_code\":\"JNB-4K7M\",\"verification_url\":\"https://x\","
            "\"poll_token\":\"p\",\"interval_s\":5,\"expires_in\":900}");
        CHECK(s.ok, "valid start claims");
        CHECK(s.user_code == "JNB-4K7M", "code carried");
        CHECK(s.poll_token == "p", "poll token carried");
        CHECK(!heartbeat_parse_pair_start("garbage").ok, "garbage start refused");
        CHECK(!heartbeat_parse_pair_start("{\"user_code\":\"x\"}").ok,
              "start without poll token refused");

        const PairPollReply w = heartbeat_parse_pair_poll("{\"status\":\"pending\"}", 428);
        CHECK(!w.ok && w.pending, "428 keeps waiting");
        CHECK(w.interval_s == 5, "pending defaults to pair cadence");
        const PairPollReply w2 =
            heartbeat_parse_pair_poll("{\"status\":\"pending\",\"interval_s\":9}", 428);
        CHECK(w2.pending && w2.interval_s == 9, "pending honours named interval");
        const PairPollReply done = heartbeat_parse_pair_poll(
            "{\"appliance_id\":\"apl_1\",\"appliance_token\":\"t\","
            "\"collector_url\":\"https://c\",\"interval_s\":30}",
            200);
        CHECK(done.ok && !done.pending, "200 with credentials resolves");
        CHECK(done.appliance_id == "apl_1", "id carried");
        CHECK(!heartbeat_parse_pair_poll("{}", 200).ok, "200 without credentials refused");
    }

    std::printf("Device id: minted locally, stable inputs, distinct roles\n");
    {
        const std::string a = heartbeat_mint_device_id("box", "pi-player", 7, 9);
        const std::string b = heartbeat_mint_device_id("box", "pi-player", 7, 9);
        CHECK(a == b, "same inputs mint the same id");
        CHECK(a.rfind("dev_", 0) == 0, "id carries the dev_ prefix");
        CHECK(heartbeat_mint_device_id("box", "obs-decoder", 7, 9) != a,
              "kind is part of the id");
        CHECK(heartbeat_mint_device_id("other", "pi-player", 7, 9) != a,
              "hostname is part of the id");

        const json start =
            json::parse(heartbeat_pair_start_body("dev_x", "pi-player", "box"));
        CHECK(start["device_id"] == "dev_x", "start names the device");
        CHECK(start["kind"] == "pi-player", "start names the kind");
        CHECK(start["hostname"] == "box", "start names the host");
        const json poll = json::parse(heartbeat_pair_poll_body("p"));
        CHECK(poll["poll_token"] == "p", "poll carries the token");
    }

    std::printf("Pairing state: waiting, polling, claimed, expired, failed\n");
    {
        Pairing p;
        CHECK(p.phase() == Pairing::Phase::Idle, "starts idle");
        CHECK(!p.poll_due(1000), "nothing due while idle");
        p.on_start_reply("garbage", 200, 1000);
        CHECK(p.phase() == Pairing::Phase::Failed, "useless start fails, never waits");
        p.cancel();
        CHECK(p.phase() == Pairing::Phase::Idle, "cancel returns to idle");

        p.on_start_reply(
            "{\"user_code\":\"JNB-4K7M\",\"verification_url\":\"https://x\","
            "\"poll_token\":\"p\",\"interval_s\":5,\"expires_in\":100}",
            200, 1000);
        CHECK(p.phase() == Pairing::Phase::Waiting, "valid start waits");
        CHECK(p.user_code() == "JNB-4K7M", "code kept for the screen");
        CHECK(!p.poll_due(1004), "poll not due before the interval");
        CHECK(p.poll_due(1005), "poll due at the interval");
        p.on_poll_reply("{\"status\":\"pending\"}", 428, 1005);
        CHECK(p.phase() == Pairing::Phase::Waiting, "pending stays waiting");
        CHECK(!p.poll_due(1009), "pending re-arms the interval");
        CHECK(p.poll_due(1010), "pending polls again after it");
        p.on_poll_reply(
            "{\"appliance_id\":\"apl_1\",\"appliance_token\":\"t\","
            "\"collector_url\":\"https://c\",\"interval_s\":30}",
            200, 1010);
        CHECK(p.phase() == Pairing::Phase::Done, "credentials resolve");
        CHECK(p.appliance_id() == "apl_1", "claimed id kept");
        CHECK(p.appliance_token() == "t", "claimed token kept");
        CHECK(!p.poll_due(9999), "nothing due once done");

        Pairing q;
        q.on_start_reply(
            "{\"user_code\":\"C\",\"verification_url\":\"https://x\","
            "\"poll_token\":\"p\",\"interval_s\":5,\"expires_in\":10}",
            200, 1000);
        q.tick(1011);
        CHECK(q.phase() == Pairing::Phase::Expired, "deadline expires the wait");
        CHECK(!q.poll_due(1011), "nothing due once expired");
        q.on_poll_reply("{}", 200, 1011);
        CHECK(q.phase() == Pairing::Phase::Expired, "a late reply cannot revive it");
    }

    if (g_fail == 0)
        std::printf("heartbeat_reporter: all passed\n");
    else
        std::printf("heartbeat_reporter: %d FAILURE(S)\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
