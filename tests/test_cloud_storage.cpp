// SPDX-License-Identifier: GPL-3.0-or-later
// test_cloud_storage.cpp — the paired device's Transport.
//
// The parts testable without a bucket are exactly the parts that encode the
// rules: the role is honoured (a decoder cannot write), there is no fallback to
// a configured key, and a device with no credentials says so rather than
// silently succeeding. The network itself is S3Transport's, already covered.
#include "../src/core/cloud_storage.h"

#include <cstdio>
#include <string>

using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

static CredentialsReply good(const char* bucket, const char* role,
                             long long expires) {
    std::string body = std::string("{\"bucket\":\"") + bucket +
                       "\",\"endpoint\":\"s3.example.com\",\"expires_at\":" +
                       std::to_string(expires) +
                       ",\"session_token\":\"sess\",\"role\":\"" + role + "\"}";
    return cloud_parse_credentials(body, 200);
}

int main() {
    std::printf("A decoder refuses to write — the role is honoured here\n");
    {
        CloudIdentity id;
        id.set_enrolment("https://collector", "app-1", "bearer");
        id.on_credentials(good("b", "decoder", 100000), 1000);

        CloudStorageConfig cfg;
        CloudTransport tx(id, CloudRole::Decoder, cfg);

        const auto p = tx.put("events/x/segments/1.m4s", {1, 2, 3}, "video/mp4", {});
        CHECK(!p.success, "a decoder's put fails");
        CHECK(!p.retryable, "and is PERMANENT — the role will not change");
        CHECK(p.error.find("reads only") != std::string::npos,
              "and says why, not just a status");

        const auto d = tx.remove("events/x/segments/1.m4s");
        CHECK(!d.success, "and a decoder cannot delete the archive either");
        CHECK(!d.retryable, "permanently");
    }

    std::printf("An encoder is allowed to write\n");
    {
        CloudIdentity id;
        id.set_enrolment("https://collector", "app-1", "bearer");
        id.on_credentials(good("b", "encoder", 100000), 1000);
        CloudStorageConfig cfg;
        CloudTransport tx(id, CloudRole::Encoder, cfg);
        // No bucket reachable here, so this fails at the network — but NOT on
        // the role. The distinction is the testable part.
        const auto p = tx.put("k", {}, "video/mp4", {});
        CHECK(p.error.find("reads only") == std::string::npos,
              "an encoder's put is not refused by the role");
        CHECK(p.retryable, "and a transport failure is retryable");
    }

    std::printf("No credentials: says so, does not silently succeed\n");
    {
        CloudIdentity id;
        CloudStorageConfig cfg;
        CloudTransport tx(id, CloudRole::Encoder, cfg);
        CHECK(!tx.has_credentials(), "an unpaired device holds none");
        const auto g = tx.get("k");
        CHECK(!g.success, "a get fails rather than returning empty data");
        CHECK(g.retryable, "and is retryable — a pairing may fix it");
        CHECK(g.error.find("no cloud credentials") != std::string::npos,
              "with a reason that names the state");
        CHECK(tx.bucket().empty(), "and no bucket is reported");
    }

    std::printf("Never falls back — there is no key to fall back to\n");
    {
        // The spec's rule made structural: this transport has no access_key /
        // secret field at all, so a failed fetch cannot resolve to a pasted key.
        CloudIdentity id;
        id.set_enrolment("https://collector", "app-1", "bearer");
        id.on_credentials(CredentialsReply{}, 1000);   // first fetch fails
        CloudStorageConfig cfg;
        CloudTransport tx(id, CloudRole::Encoder, cfg);
        CHECK(!tx.has_credentials(), "no credentials means no credentials");
        const auto p = tx.put("k", {}, "video/mp4", {});
        CHECK(!p.success, "and a write does not proceed on a pasted key");
        CHECK(p.error.find("secret") == std::string::npos &&
              p.error.find("access_key") == std::string::npos,
              "the message never mentions a fallback key because none exists");
    }

    std::printf("Staleness is visible, not hidden\n");
    {
        CloudIdentity id;
        id.set_enrolment("https://collector", "app-1", "bearer");
        id.on_credentials(good("keep-me", "encoder", 100000), 1000);
        CloudStorageConfig cfg;
        CloudTransport tx(id, CloudRole::Encoder, cfg);
        CHECK(tx.bucket() == "keep-me", "the bucket is the identity's");
        CHECK(!tx.credentials_are_stale(), "fresh to start with");

        // The collector goes away; the identity keeps last-good and marks it.
        id.on_credentials(CredentialsReply{}, 20000);
        CHECK(tx.bucket() == "keep-me", "the bucket survives an outage");
        CHECK(tx.credentials_are_stale(),
              "and the transport says the credentials are stale, so a dock can");
    }

    std::printf("Re-pairing swaps the bucket\n");
    {
        CloudIdentity id;
        id.set_enrolment("https://collector", "app-1", "bearer");
        id.on_credentials(good("old-bucket", "encoder", 100000), 1000);
        CloudStorageConfig cfg;
        CloudTransport tx(id, CloudRole::Encoder, cfg);
        CHECK(tx.bucket() == "old-bucket", "the first bucket");
        id.on_credentials(good("new-bucket", "encoder", 200000), 2000);
        CHECK(tx.bucket() == "new-bucket", "a refresh takes the new one");
    }

    std::printf("\n%s\n", g_fail == 0 ? "ALL CLOUD STORAGE TESTS PASSED"
                                      : "SOME CLOUD STORAGE TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
