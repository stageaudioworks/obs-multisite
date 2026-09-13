// SPDX-License-Identifier: GPL-3.0-or-later
// test_storage_providers.cpp — the provider dropdown's derivation, the thing
// that lets an operator type an account id or a region instead of learning
// each provider's hostname convention by hand. See PROJECT-SCOPE.md §8.6.
#include "../src/core/storage_providers.h"

#include <cstdio>

using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

int main() {
    std::printf("== Cloudflare R2: an account id, not a hostname ==\n");
    {
        auto d = derive(StorageProvider::CloudflareR2, "abc123");
        CHECK(d.r2_account_id == "abc123", "account id passed straight through");
        CHECK(d.endpoint_host.empty(),
              "no endpoint host — S3Transport already derives it from the account id");
        CHECK(d.region == "auto", "region defaults to auto, same as today's default");
    }

    std::printf("== AWS / Backblaze / Wasabi: a region derives the hostname ==\n");
    {
        auto aws = derive(StorageProvider::AwsS3, "us-east-1");
        CHECK(aws.endpoint_host == "s3.us-east-1.amazonaws.com", "AWS hostname derived");
        CHECK(aws.region == "us-east-1", "region carried through for SigV4 signing");
        CHECK(aws.r2_account_id.empty(), "no account id for AWS");

        auto b2 = derive(StorageProvider::BackblazeB2, "us-west-004");
        CHECK(b2.endpoint_host == "s3.us-west-004.backblazeb2.com", "Backblaze hostname derived");

        auto wasabi = derive(StorageProvider::Wasabi, "us-east-1");
        CHECK(wasabi.endpoint_host == "s3.us-east-1.wasabisys.com", "Wasabi hostname derived");
    }

    std::printf("== Custom and Multisite Cloud derive nothing ==\n");
    {
        auto custom = derive(StorageProvider::Custom, "anything");
        CHECK(custom.endpoint_host.empty() && custom.r2_account_id.empty() &&
              custom.region.empty(),
              "Custom's own typed fields ARE the config — nothing derived here");
    }

    std::printf("== Loading a saved config guesses the right provider back ==\n");
    {
        CHECK(detect_provider("", "abc123") == StorageProvider::CloudflareR2,
              "an account id with no endpoint reads as R2");
        CHECK(detect_provider("abc123.r2.cloudflarestorage.com", "") == StorageProvider::CloudflareR2,
              "an R2 hostname pasted into the endpoint field still reads as R2");
        CHECK(detect_provider("s3.eu-west-1.amazonaws.com", "") == StorageProvider::AwsS3,
              "an AWS hostname reads as AWS");
        CHECK(detect_provider("s3.us-west-004.backblazeb2.com", "") == StorageProvider::BackblazeB2,
              "a Backblaze hostname reads as Backblaze");
        CHECK(detect_provider("s3.us-east-1.wasabisys.com", "") == StorageProvider::Wasabi,
              "a Wasabi hostname reads as Wasabi");
        CHECK(detect_provider("minio.internal.church:9000", "") == StorageProvider::Custom,
              "an unrecognised endpoint (MinIO, anything else) reads as Custom — "
              "never misrepresented as a provider it isn't");
        CHECK(detect_provider("", "") == StorageProvider::Custom,
              "nothing configured at all reads as Custom, the safe default");
    }

    std::printf("== Provider keys persist as strings, and round-trip ==\n");
    {
        for (const auto& info : all_providers()) {
            CHECK(provider_key(info.id) == info.key,
                  "provider_key() matches the table's own key");
            CHECK(provider_from_key(info.key) == info.id,
                  "provider_from_key() round-trips back to the same provider");
        }
        CHECK(provider_from_key("some_future_provider_this_build_does_not_know") ==
              StorageProvider::Custom,
              "an unrecognised key falls back to Custom rather than refusing to load");
    }

    std::printf("== Multisite Cloud is listed but not selectable yet ==\n");
    {
        const auto& info = provider_info(StorageProvider::MultisiteCloud);
        CHECK(!info.available, "greyed out until the brokered flow (§8.5) actually exists");
        bool found = false;
        for (const auto& p : all_providers())
            if (p.id == StorageProvider::MultisiteCloud) found = true;
        CHECK(found, "but still present in the list, so a dock can show it greyed out");
    }

    if (g_fail) {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("all storage-provider checks passed\n");
    return 0;
}
