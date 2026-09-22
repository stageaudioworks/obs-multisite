// SPDX-License-Identifier: GPL-3.0-or-later
//
// probe_credentials — a one-shot diagnostic that drives the Phase 12 pairing
// and credential flow against a REAL collector, then prints exactly what came
// back.
//
// WHY IT EXISTS. The core modules (cloud_identity, collector_client,
// cloud_storage) are tested against mocks, and the spec
// (docs/scope/spec-cloud-identity.md) states the /v1/credentials reply shape
// from the design rather than from observation. This tool turns that guess into
// a measurement: it pairs a throwaway device, fetches credentials with the
// bearer it is given, and prints the bucket, endpoint, role, session token and
// expiry the collector actually returns. Run it before wiring a host, so the
// host is built against the real contract.
//
// It is a diagnostic, not a product: no thread, no retries, one device id per
// run, and it says plainly when it cannot reach anything.
//
// Build (from the repo root, after configuring the tree):
//   cmake --build build --target probe_credentials
//   ./build/probe_credentials https://api.multisite-cloud.streamworks.video
//
// Not part of ctest: it needs a live collector and an operator to approve the
// code, so it cannot run unattended.

#include "../src/core/collector_client.h"
#include "../src/core/cloud_identity.h"
#include "../src/core/heartbeat_reporter.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <unistd.h>

using namespace multisite;

namespace {

const char* kKind = "obs-encoder";

std::string hostname() {
    char buf[256];
    buf[sizeof(buf) - 1] = 0;
    if (gethostname(buf, sizeof(buf) - 1) != 0) return "probe";
    return buf;
}

long long wall_now_s() {
    return (long long)std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

long long steady_ns() {
    return (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void line(const char* tag, const std::string& v) {
    std::printf("  %-12s %s\n", tag, v.empty() ? "(empty)" : v.c_str());
}

} // namespace

int main(int argc, char** argv) {
    // Line-buffered: the pairing code has to be visible while the tool is
    // still waiting for approval, not flushed when it exits.
    setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc < 2) {
        std::fprintf(stderr,
            "usage: probe_credentials <collector-url>\n"
            "  e.g. probe_credentials https://api.multisite-cloud.example\n");
        return 2;
    }
    const std::string base = argv[1];

    collector_http_init();
    std::printf("Collector: %s\n\n", base.c_str());

    // ── 1. Pair ─────────────────────────────────────────────────────────────
    const std::string dev = heartbeat_mint_device_id(
        hostname(), kKind, steady_ns(), (long long)getpid());
    std::printf("1. PAIR — device id %s\n", dev.c_str());

    const HttpResult start = http_post_json(
        collector_url(base, kPairStartPath), std::string(),
        heartbeat_pair_start_body(dev, kKind, hostname()));
    if (!start.reached) {
        std::fprintf(stderr,
            "   the collector was not reached. Is the URL right, and is this "
            "machine online?\n");
        return 1;
    }
    std::printf("   HTTP %ld\n", start.code);

    Pairing pairing;
    pairing.on_start_reply(start.body, (int)start.code, wall_now_s());
    if (pairing.phase() != Pairing::Phase::Waiting) {
        std::fprintf(stderr, "   pairing did not start: %s\n",
                     pairing.error().c_str());
        std::fprintf(stderr, "   body: %s\n", start.body.c_str());
        return 1;
    }

    std::printf("\n");
    std::printf("   ┌──────────────────────────────────────────────┐\n");
    std::printf("   │   Enter this code at the collector's page:   │\n");
    std::printf("   │                                              │\n");
    std::printf("   │              %-10s                    │\n",
                pairing.user_code().c_str());
    std::printf("   │                                              │\n");
    std::printf("   │   %-42s │\n", pairing.verification_url().c_str());
    std::printf("   └──────────────────────────────────────────────┘\n");
    std::printf("\n   Waiting for approval (polling every %d s, times out in "
                "15 min)...\n\n", pairing.poll_interval_s());

    // ── 2. Poll until approved ──────────────────────────────────────────────
    std::string appliance_id, appliance_token, collector_from_reply;
    bool approved = false;
    const long long deadline = wall_now_s() + 15 * 60;
    while (wall_now_s() < deadline) {
        std::this_thread::sleep_for(
            std::chrono::seconds(pairing.poll_interval_s()));
        const HttpResult poll = http_post_json(
            collector_url(base, kPairPollPath), std::string(),
            heartbeat_pair_poll_body(pairing.poll_token()));
        if (!poll.reached) {
            std::printf("   poll: not reached (will keep trying)\n");
            continue;
        }
        pairing.on_poll_reply(poll.body, (int)poll.code, wall_now_s());
        if (pairing.phase() == Pairing::Phase::Done) {
            appliance_id = pairing.appliance_id();
            appliance_token = pairing.appliance_token();
            collector_from_reply = pairing.collector_url();
            approved = true;
            break;
        }
        if (pairing.phase() == Pairing::Phase::Failed ||
            pairing.phase() == Pairing::Phase::Expired) {
            std::fprintf(stderr, "   pairing %s: %s\n",
                pairing.phase() == Pairing::Phase::Expired ? "expired" : "failed",
                pairing.error().c_str());
            return 1;
        }
        std::printf("   poll: HTTP %ld — still waiting\n", poll.code);
    }
    if (!approved) {
        std::fprintf(stderr, "   gave up after 15 minutes.\n");
        return 1;
    }

    std::printf("\n2. APPROVED\n");
    line("appliance_id", appliance_id);
    line("bearer", std::string(appliance_token.size(), '*') + "  (" +
                   std::to_string(appliance_token.size()) + " chars)");
    line("collector", collector_from_reply);
    // The collector may hand back a different base URL than the one typed. If it
    // does, THAT is the address every later call must use, and a host that kept
    // the typed one would be talking to the wrong place.
    const std::string use_base =
        collector_from_reply.empty() ? base : collector_from_reply;
    if (!collector_from_reply.empty() && collector_from_reply != base)
        std::printf("   NOTE: the collector named a different base (%s) than "
                    "the one typed. This is why the reply's URL is used "
                    "onward.\n", collector_from_reply.c_str());

    // ── 3. Fetch credentials ────────────────────────────────────────────────
    std::printf("\n3. CREDENTIALS — GET %s\n",
                collector_url(use_base, kCredentialsPath).c_str());
    const HttpResult cr = http_get_json(
        collector_url(use_base, kCredentialsPath), appliance_token);
    if (!cr.reached) {
        std::fprintf(stderr, "   not reached.\n");
        return 1;
    }
    std::printf("   HTTP %ld\n", cr.code);
    std::printf("   raw body: %s\n\n", cr.body.c_str());

    const CredentialsReply reply =
        cloud_parse_credentials(cr.body, (int)cr.code);
    if (reply.unpaired) {
        std::printf("   → the collector says this device is UNPAIRED (403).\n"
                    "     A host must stop fetching, not retry.\n");
        return 0;
    }
    if (!reply.ok) {
        std::printf("   → this body did not parse as usable credentials.\n"
                    "     That is a contract mismatch: the spec expects "
                    "{bucket, endpoint, session_token, expires_at, role}.\n");
        return 1;
    }

    const Credentials& c = reply.creds;
    const long long now = wall_now_s() * 1000;
    std::printf("   → parsed as:\n");
    line("bucket", c.bucket);
    line("endpoint", c.endpoint);
    line("role", c.read_write ? "read-write (encoder)" : "read-only (decoder)");
    line("session_token", std::string(c.session_token.size(), '*') + "  (" +
                          std::to_string(c.session_token.size()) + " chars)");
    std::printf("  %-12s %lld ms  (%s)\n", "expires_at", c.expires_at_ms,
                c.expires_at_ms > now ? "in the future" : "ALREADY PAST");
    if (c.expires_at_ms > now) {
        std::printf("  %-12s %lld s from now\n", "ttl",
                    (c.expires_at_ms - now) / 1000);
        std::printf("  %-12s %lld ms (half the TTL)\n", "next refresh",
                    cloud_next_refresh_ms(c, now));
    }

    // ── 4. What a host would do with it ─────────────────────────────────────
    std::printf("\n4. THE IDENTITY, as a host sees it\n");
    CloudIdentity id;
    id.set_enrolment(use_base, appliance_id, appliance_token);
    id.on_credentials(reply, now);
    std::printf("  %-14s %s\n", "paired", id.paired() ? "yes" : "no");
    std::printf("  %-14s %s\n", "bucket", id.credentials().bucket.c_str());
    std::printf("  %-14s %s\n", "live",
                id.credentials().live(now) ? "yes" : "no (last-good or expired)");
    std::printf("  %-14s %s\n", "next fetch",
                id.tick(now) == CloudAction::Fetch ? "due now" : "scheduled");

    std::printf("\nDone. If the fields above are what the spec expects, the "
                "contract holds and a host can be wired against it.\n");
    return 0;
}
