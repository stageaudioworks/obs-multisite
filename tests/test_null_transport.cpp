// SPDX-License-Identifier: GPL-3.0-or-later
// test_null_transport.cpp — the encoder's "cloud delivery disabled" Transport
// (PROJECT-SCOPE.md §8.7): every PUT must succeed instantly, and the
// post-success size check RetryUploader runs on the first few segments of
// every event (UploaderConfig::verify_first_n) must see a match rather than
// a false "object not found after a successful PUT".
#include "../src/core/null_transport.h"

#include <cstdio>

using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

int main() {
    NullTransport tx;

    std::printf("Every PUT succeeds instantly\n");
    {
        std::vector<uint8_t> body = { 1, 2, 3, 4, 5 };
        auto r = tx.put("events/E1/segments/00000000.m4s", body, "video/mp4", {});
        CHECK(r.success, "put() reports success");
        CHECK(r.http_status == 200, "with a real 2xx status");
    }

    std::printf("object_size() echoes back what was just put, not -1\n");
    {
        std::vector<uint8_t> body(4096, 0x42);
        tx.put("events/E1/segments/00000001.m4s", body, "video/mp4", {});
        CHECK(tx.object_size("events/E1/segments/00000001.m4s") == 4096,
              "the exact size just written, so RetryUploader's verify step "
              "never reports a false failure against this transport");
    }

    std::printf("An unwritten key reports -1, same as a real miss\n");
    {
        CHECK(tx.object_size("events/E1/segments/never_put.m4s") == -1,
              "nothing was ever put under this key");
    }

    std::printf("get()/list()/remove() are the base class's honest "
                "\"not implemented\" — an encoder never calls them\n");
    {
        auto g = tx.get("events/E1/manifest.json");
        CHECK(!g.success, "get() is not implemented by a write-only transport");
    }

    std::printf("\n%s\n", g_fail == 0
        ? "ALL NULL-TRANSPORT TESTS PASSED"
        : "SOME TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
