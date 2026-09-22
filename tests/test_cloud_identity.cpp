// SPDX-License-Identifier: GPL-3.0-or-later
// test_cloud_identity.cpp — the paired appliance's credential lifecycle.
//
// Every case here is a clause of docs/scope/spec-cloud-identity.md: the
// refresh-at-half-TTL schedule, last-good kept on a collector outage, 403
// stopping the device without tearing down a running event, and the one value
// a consumer must never see without its liveness. The point of the module is
// that a paired device's storage and its heartbeat share an identity, so the
// distinctions the consumers depend on are pinned here rather than by example.
#include "../src/core/cloud_identity.h"
#include "../src/vendor/nlohmann/json.hpp"

#include <cstdio>
#include <string>

using namespace multisite;
using json = nlohmann::json;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

static std::string creds_body(const char* bucket, const char* endpoint,
                              long long expires_at, const char* token,
                              const char* role) {
    json j;
    j["bucket"] = bucket;
    j["endpoint"] = endpoint;
    j["expires_at"] = expires_at;
    j["session_token"] = token;
    j["role"] = role;
    return j.dump();
}

int main() {
    std::printf("Parsing: a usable reply, and the reasons one is not\n");
    {
        const auto ok = cloud_parse_credentials(
            creds_body("church-media", "s3.example", 5000, "tok", "encoder"),
            200);
        CHECK(ok.ok, "a well-formed 200 parses");
        CHECK(ok.creds.bucket == "church-media", "the bucket is taken as sent");
        CHECK(ok.creds.session_token == "tok", "the session token is kept");
        CHECK(ok.creds.read_write, "role encoder means read-write");
        CHECK(!ok.creds.live(1000) == false, "and it is live before expiry");
        CHECK(!ok.creds.live(6000), "and not live after it");

        const auto ro = cloud_parse_credentials(
            creds_body("b", "e", 5000, "t", "decoder"), 200);
        CHECK(ro.ok && !ro.creds.read_write, "role decoder is read-only");

        // A missing bucket is useless however well-formed, and must NOT read as
        // success — that is how a working last-good set gets cleared.
        json no_bucket;
        no_bucket["endpoint"] = "e";
        no_bucket["expires_at"] = 5000;
        CHECK(!cloud_parse_credentials(no_bucket.dump(), 200).ok,
              "a reply naming no bucket is not success");

        CHECK(!cloud_parse_credentials("{not json", 200).ok,
              "malformed JSON is a failed fetch, not a throw");
        CHECK(!cloud_parse_credentials("", 200).ok, "empty body fails");
    }

    std::printf("403 is its own outcome: unpaired, not a generic failure\n");
    {
        const auto r403 = cloud_parse_credentials("", 403);
        CHECK(r403.unpaired, "403 means the device was unpaired");
        CHECK(!r403.ok, "and is not a success");
        const auto r500 = cloud_parse_credentials("", 500);
        CHECK(!r500.unpaired, "500 is not the unpaired signal");
        CHECK(!r500.ok, "and is a failure");
    }

    std::printf("Refresh at half the TTL, and never past the expiry\n");
    {
        Credentials c;
        c.bucket = "b";
        c.expires_at_ms = 100000;
        // Half of 90 s remaining is 45 s.
        CHECK(cloud_next_refresh_ms(c, 10000) == 45000,
              "half the remaining TTL");
        // Near expiry the floor applies rather than a vanishing interval.
        CHECK(cloud_next_refresh_ms(c, 99000) == 5000,
              "a very short remainder floors at 5 s");
        CHECK(cloud_next_refresh_ms(c, 100000) == 0, "expired schedules nothing");
        Credentials empty;
        CHECK(cloud_next_refresh_ms(empty, 0) == 0,
              "nothing to refresh schedules nothing");
    }

    std::printf("Unpaired: tick does nothing at all\n");
    {
        CloudIdentity id;
        CHECK(!id.paired(), "a fresh identity is unpaired");
        CHECK(id.tick(0) == CloudAction::Idle, "and ticks idle");
        CHECK(id.tick(999999) == CloudAction::Idle, "however long it waits");
    }

    std::printf("Boot: fetch at once, then on the half-TTL schedule\n");
    {
        CloudIdentity id;
        id.set_enrolment("https://collector", "app-1", "bearer-1");
        CHECK(id.paired(), "an enrolment pairs the device");
        CHECK(id.tick(0) == CloudAction::Fetch, "boot fetches immediately");
        CHECK(id.tick(1000) == CloudAction::Fetch,
              "and keeps asking until it is answered");

        id.on_credentials(cloud_parse_credentials(
            creds_body("b", "e", 100000, "s", "encoder"), 200), 1000);
        CHECK(!id.credentials().from_last_good, "a good reply is not stale");
        CHECK(id.credentials().bucket == "b", "and replaces the set");
        CHECK(id.tick(1000) == CloudAction::Idle, "nothing due right after");
        // 100000 - 1000 = 99000 remaining, half is 49500.
        CHECK(id.tick(50499) == CloudAction::Idle, "not yet at the half-TTL");
        CHECK(id.tick(50501) == CloudAction::Fetch, "fetch at half the TTL");
    }

    std::printf("A collector outage keeps last-good and never clears it\n");
    {
        CloudIdentity id;
        id.set_enrolment("https://collector", "app-1", "bearer-1");
        id.on_credentials(cloud_parse_credentials(
            creds_body("keep-me", "e", 100000, "s", "encoder"), 200), 1000);
        CHECK(id.credentials().bucket == "keep-me", "a good set is present");

        // An unreachable collector: no reply at all.
        id.on_credentials(CredentialsReply{}, 20000);
        CHECK(id.credentials().bucket == "keep-me",
              "the bucket is KEPT across a failed fetch");
        CHECK(id.credentials().from_last_good,
              "and is marked last-good, so consumers can see it is stale");
        CHECK(!id.credentials().live(20000),
              "stale credentials do not read as live");
        CHECK(id.tick(20000) == CloudAction::Idle,
              "with time left, it waits rather than hammering");
        CHECK(id.tick(200000) == CloudAction::Fetch,
              "but a transient outage is recovered from, not permanent");
    }

    std::printf("First fetch fails with nothing to fall back on\n");
    {
        CloudIdentity id;
        id.set_enrolment("https://collector", "app-1", "bearer-1");
        id.on_credentials(CredentialsReply{}, 1000);
        CHECK(!id.credentials().present(), "there is nothing to keep");
        // kMinRefreshMs is 5 s, so the retry is scheduled for t=6000, not now.
        CHECK(id.tick(5999) == CloudAction::Idle, "not before the retry");
        CHECK(id.tick(6000) == CloudAction::Fetch,
              "so it tries again soon rather than never");
    }

    std::printf("403 STOPS the device, and does not tear an event down\n");
    {
        CloudIdentity id;
        id.set_enrolment("https://collector", "app-1", "bearer-1");
        id.on_credentials(cloud_parse_credentials(
            creds_body("live-event-bucket", "e", 1000000, "s", "encoder"),
            200), 1000);

        // Revoked mid-event.
        id.on_credentials(cloud_parse_credentials("", 403), 2000);
        CHECK(id.unpaired(), "403 marks the device unpaired");
        CHECK(!id.error().empty(), "and says so");
        CHECK(id.credentials().bucket == "live-event-bucket",
              "the running event's credentials are NOT cleared");
        CHECK(id.credentials().from_last_good,
              "they are marked last-good, and cover the event to expiry");
        // The whole point: no retry. Fetching again would be retrying a
        // revocation, which is how a device becomes a stuck one.
        CHECK(id.tick(2000) == CloudAction::Idle, "no fetch is scheduled");
        CHECK(id.tick(99999999) == CloudAction::Idle, "now or ever");
    }

    std::printf("Re-pairing recovers a stopped device\n");
    {
        CloudIdentity id;
        id.set_enrolment("https://collector", "old", "oldtok");
        id.on_credentials(cloud_parse_credentials("", 403), 1000);
        CHECK(id.unpaired(), "revoked");
        id.set_enrolment("https://collector", "new", "newtok");
        CHECK(!id.unpaired(), "a new enrolment clears the stopped state");
        CHECK(!id.credentials().present(), "and the old device's keys with it");
        CHECK(id.tick(1000) == CloudAction::Fetch, "and it fetches at once");
    }

    std::printf("Disconnect forgets everything\n");
    {
        CloudIdentity id;
        id.set_enrolment("https://collector", "app-1", "bearer-1");
        id.on_credentials(cloud_parse_credentials(
            creds_body("b", "e", 100000, "s", "encoder"), 200), 1000);
        id.reset();
        CHECK(!id.paired(), "disconnect leaves the device unpaired");
        CHECK(!id.credentials().present(), "with no credentials held");
        CHECK(id.tick(0) == CloudAction::Idle, "and nothing scheduled");
    }

    std::printf("One authority: the set carries its own liveness\n");
    {
        // The distinction cloud-storage depends on: a bucket is never visible
        // without the liveness of the credentials behind it.
        Credentials fresh;
        fresh.bucket = "b"; fresh.expires_at_ms = 1000; fresh.from_last_good = false;
        CHECK(fresh.live(500), "a fresh, unexpired set is live");
        Credentials stale = fresh;
        stale.from_last_good = true;
        CHECK(!stale.live(500),
              "the SAME values marked stale are not live — the flag is what "
              "tells a consumer to say so");
    }

    std::printf("\n%s\n", g_fail == 0 ? "ALL CLOUD IDENTITY TESTS PASSED"
                                      : "SOME CLOUD IDENTITY TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
