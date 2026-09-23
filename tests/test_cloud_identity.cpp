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
#include "../src/core/s3_transport.h"   // the S3Config the converter returns
#include "../src/vendor/nlohmann/json.hpp"

#include <atomic>
#include <thread>
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

    std::printf("expires_at is an ISO-8601 STRING, not an integer\n");
    {
        // The spec guessed an integer; the live collector sends a string
        // (measured 2026-09-22). Reading it as an integer yielded 0, which
        // marked every credential set already-expired.
        CHECK(cloud_parse_iso8601_ms("1970-01-01T00:00:00Z") == 0,
              "the epoch");
        CHECK(cloud_parse_iso8601_ms("1970-01-01T00:00:01Z") == 1000,
              "one second");
        CHECK(cloud_parse_iso8601_ms("2026-09-22T08:18:03Z") == 1790065083000LL,
              "a real instant, to the second");
        CHECK(cloud_parse_iso8601_ms("2026-09-22T08:18:03.586Z") ==
                  1790065083586LL,
              "and with milliseconds, which is the form the collector sends");
        // Fractional digits are scaled by what was actually read, so .5 is
        // 500 ms and not 5.
        CHECK(cloud_parse_iso8601_ms("2026-09-22T08:18:03.5Z") ==
                  1790065083500LL, ".5 means 500 ms");
        CHECK(cloud_parse_iso8601_ms("2026-09-22T08:18:03.123456Z") ==
                  1790065083123LL, "longer fractions are truncated, not scaled");
        // Fails safe: 0 is "no expiry known", which live() never accepts.
        CHECK(cloud_parse_iso8601_ms("") == 0, "empty is 0");
        CHECK(cloud_parse_iso8601_ms("not a date") == 0, "garbage is 0");
        CHECK(cloud_parse_iso8601_ms("2026-13-01T00:00:00Z") == 0,
              "an impossible month is 0");
        CHECK(cloud_parse_iso8601_ms("2026-09-22") == 0,
              "a date with no time is 0");
    }

    std::printf("The reply carries the key PAIR as well as the token\n");
    {
        // Measured field names, from the live collector 2026-09-22.
        json j;
        j["endpoint"] = "https://acct.r2.cloudflarestorage.com";
        j["bucket"] = "multisite-demo-org";
        j["region"] = "auto";
        j["access_key_id"] = "cdc6289bd3612745784cbd28204aead0";
        j["secret_access_key"] = "df88a47a...";
        j["session_token"] = "and0L2V5...";
        j["role"] = "encoder";
        j["expires_at"] = "2026-09-22T08:18:03.586Z";
        const auto r = cloud_parse_credentials(j.dump(), 200);
        CHECK(r.ok, "the real shape parses");
        CHECK(r.creds.access_key_id == "cdc6289bd3612745784cbd28204aead0",
              "the key id is kept — a token alone signs with nothing");
        CHECK(r.creds.secret_access_key == "df88a47a...", "the secret is kept");
        CHECK(r.creds.expires_at_ms == 1790065083586LL,
              "and the string expiry became a real instant");
        CHECK(r.creds.live(1790065083000LL),
              "so the set is live before it expires");
        CHECK(!r.creds.live(1790065084000LL), "and not after");
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

    std::printf("Credentials become a transport config in exactly one place\n");
    {
        // Every field a transport needs comes from the credentials. This is
        // the conversion that had been hand-written at four surfaces and got
        // the SAME field wrong at three of them: the endpoint, left empty
        // because it was filled from the dock's typed host instead. That is
        // what made "Test connection" say "could not resolve host name" on a
        // working box, and the storage window list a bucket nobody was
        // recording to.
        Credentials c;
        c.bucket = "multisite-demo-org";
        c.endpoint = "https://s3.example.test";
        c.access_key_id = "AKIA";
        c.secret_access_key = "secret";
        c.session_token = "tok";
        c.expires_at_ms = 1000;

        const S3Config s3 = s3_config_from_credentials(c);
        CHECK(s3.bucket == "multisite-demo-org",
              "the bucket is the credentials' bucket");
        CHECK(s3.endpoint_host == "https://s3.example.test",
              "the endpoint is the credentials' own host, scheme and all — "
              "never re-derived from a region or read from a typed field");
        CHECK(s3.access_key_id == "AKIA" && s3.secret_access_key == "secret",
              "the key PAIR travels, not the token alone");
        CHECK(s3.session_token == "tok",
              "with the session token that proves the pair is temporary");
        CHECK(s3.endpoint_host.rfind("http", 0) == 0,
              "the scheme is preserved for S3Transport to strip");
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

    std::printf("== the identity is read and written from different threads ==\n");
    {
        // #11. A host's worker writes the credentials and the enrolment while
        // the storage path reads them from its own threads — and on the Pi the
        // enrolment is written from rebuild_session, which runs on several.
        // There was no lock. This is the shape of that use, compressed: one
        // thread rewriting, others reading, for long enough that the sanitizer
        // job sees any unsynchronised access. It fails under TSan against the
        // lock-free version and passes against the locked one; in an ordinary
        // build it simply has to finish with every read coherent.
        CloudIdentity id;
        id.set_enrolment("https://a.example", "apl_a", "tok_a");
        std::atomic<bool> stop{false};
        std::atomic<int>  torn_creds{0};
        std::atomic<int>  torn_enrol{0};

        std::thread writer([&] {
            for (int i = 0; i < 4000; ++i) {
                const bool even = (i % 2) == 0;
                id.set_enrolment(even ? "https://a.example" : "https://b.example",
                                 even ? "apl_a" : "apl_b",
                                 even ? "tok_a" : "tok_b");
                CredentialsReply r;
                r.ok = true;
                r.creds.bucket        = even ? "bucket-a" : "bucket-b";
                r.creds.endpoint      = even ? "https://a.r2" : "https://b.r2";
                r.creds.session_token = std::string(64, even ? 'a' : 'b');
                r.creds.expires_at_ms = 1000000 + i;
                id.on_credentials(r, 1000 + i);
            }
            stop = true;
        });
        auto reader = [&] {
            while (!stop.load()) {
                (void)id.paired();
                (void)id.tick(2000);
                const Enrolment e = id.enrolment();   // ONE snapshot
                const std::string& u = e.url;
                const std::string& a = e.id;
                const Credentials c = id.credentials();
                // A copy taken mid-write would mix the two sets. Each field is
                // written as a pair, so any mixture is a tear.
                if (!c.bucket.empty() && !c.session_token.empty() &&
                    (c.bucket == "bucket-a") != (c.session_token[0] == 'a'))
                    ++torn_creds;
                if (!u.empty() && !a.empty() &&
                    (u == "https://a.example") != (a == "apl_a"))
                    ++torn_enrol;
            }
        };
        std::thread r1(reader), r2(reader);
        writer.join(); r1.join(); r2.join();
        std::printf("         tears: credentials %d, enrolment %d\n",
                    torn_creds.load(), torn_enrol.load());
        CHECK(torn_creds.load() == 0,
              "one credentials() call is never half of one set and half of another");
        CHECK(torn_enrol.load() == 0,
              "the enrolment read as one snapshot is never two pairings mixed");
        CHECK(id.paired(), "and the identity is still coherent afterwards");
    }

    std::printf("== a refresh does not move storage; a new bucket does ==\n");
    {
        // The Pi asked for a full session rebuild on EVERY fetch, so a paired
        // box lost its picture and its position every ~7.5 minutes, and every
        // 5 s while the collector was unreachable. The transport signs with
        // refreshed tokens by itself; only a new bucket needs a new session.
        CredentialsReply fresh;
        fresh.ok = true;
        fresh.creds.bucket = "org-bucket";
        fresh.creds.session_token = "token-2";
        CHECK(credentials_move_storage("", fresh),
              "the first bucket moves storage: the session has nothing to read yet");
        CHECK(!credentials_move_storage("org-bucket", fresh),
              "a refresh of the same bucket does not — a new token is not a new session");
        CHECK(credentials_move_storage("old-bucket", fresh),
              "a different bucket does");
        CredentialsReply unreachable;          // the collector was not reached
        CHECK(!credentials_move_storage("org-bucket", unreachable),
              "an unreachable collector does not: last-good carries on");
        CHECK(!credentials_move_storage("", unreachable),
              "not even before the first bucket — there is nothing new to read");
        CredentialsReply refused;
        refused.unpaired = true;
        CHECK(!credentials_move_storage("org-bucket", refused),
              "an unpairing does not rebuild; the event runs out on last-good");
        CredentialsReply empty_ok;
        empty_ok.ok = true;
        CHECK(!credentials_move_storage("org-bucket", empty_ok),
              "an accepted reply naming no bucket moves nothing");
    }

    std::printf("A read-only device refuses credentials that can write\n");
    {
        // ADR-0002. The relay only ever reads, and is the one component
        // deliberately exposed to the internet, so a read-write set from the
        // collector is an error rather than a convenience.
        CloudIdentity id;
        id.set_require_read_only(true);
        id.set_enrolment("https://collector", "apl_relay", "tok");

        auto rw = cloud_parse_credentials(
            creds_body("org-bucket", "https://s3.example", 0, "sess", "encoder"),
            200);
        CHECK(rw.ok && rw.creds.read_write, "the reply itself is read-write");

        id.on_credentials(rw, 1000);
        CHECK(!id.credentials().present(),
              "nothing is adopted: a refused set must not become the one in use");
        CHECK(id.error().find("read-write") != std::string::npos,
              "and the reason says what was wrong");
        CHECK(!id.unpaired(),
              "this is not an unpairing — the device is still enrolled");

        // Not terminal: a collector corrected to issue read-only is picked up
        // without anyone restarting the relay.
        CHECK(id.tick(1000 + 60000) == CloudAction::Fetch,
              "it keeps asking, so a corrected collector recovers by itself");

        auto ro = cloud_parse_credentials(
            creds_body("org-bucket", "https://s3.example",
                       0, "sess", "decoder"),
            200);
        id.on_credentials(ro, 2000);
        CHECK(id.credentials().present() && !id.credentials().read_write,
              "and a read-only set is accepted normally");
        CHECK(id.error().empty(), "with the refusal cleared");
    }

    std::printf("A read-only refusal leaves last-good exactly as it was\n");
    {
        // The trap this avoids: refusing by clearing would leave a working
        // device with nothing, so a collector that starts issuing read-write
        // mid-event would end the event rather than decline the new set.
        CloudIdentity id;
        id.set_require_read_only(true);
        id.set_enrolment("https://collector", "apl_relay", "tok");

        auto ro = cloud_parse_credentials(
            creds_body("org-bucket", "https://s3.example", 0, "sess", "decoder"),
            200);
        id.on_credentials(ro, 1000);
        CHECK(id.credentials().present(), "a good set is in use");

        auto rw = cloud_parse_credentials(
            creds_body("other-bucket", "https://s3.example", 0, "s2", "encoder"),
            200);
        id.on_credentials(rw, 2000);
        CHECK(id.credentials().bucket == "org-bucket",
              "the refused set does not replace the working one");
        CHECK(!id.credentials().from_last_good,
              "and the working one is not marked stale by a refusal it survived");
    }

    std::printf("Requiring read-only is off unless a host asks for it\n");
    {
        // Every existing host keeps today's behaviour: the decoder logs the
        // role and carries on (ADR-0001), and nothing changes for it.
        CloudIdentity id;
        CHECK(!id.requires_read_only(), "off by default");
        id.set_enrolment("https://collector", "apl_dec", "tok");
        auto rw = cloud_parse_credentials(
            creds_body("org-bucket", "https://s3.example", 0, "sess", "encoder"),
            200);
        id.on_credentials(rw, 1000);
        CHECK(id.credentials().present() && id.credentials().read_write,
              "a read-write set is still accepted where it is allowed");
    }

    std::printf("\n%s\n", g_fail == 0 ? "ALL CLOUD IDENTITY TESTS PASSED"
                                      : "SOME CLOUD IDENTITY TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
