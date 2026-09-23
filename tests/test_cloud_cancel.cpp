// SPDX-License-Identifier: GPL-3.0-or-later
// test_cloud_cancel.cpp — a stop reaches a request that began before a
// credential refresh.
//
// CloudTransport builds a new inner S3Transport whenever the identity's
// credential set changes, which on a paired box is every refresh (~7.5 min at a
// 900 s TTL). A request already in flight keeps running on the inner it began
// on. cancel_pending() used to cancel only the CURRENT inner, so stopping a
// source in the moment after a refresh left that older request running out its
// whole timeout — the teardown hang test_s3_cancel exists for, reintroduced one
// layer up.
//
// Same method as test_s3_cancel: a bare TCP listener that accepts and then
// never answers. POSIX sockets only; built beside test_s3_cancel.
#include "cloud_storage.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace multisite;
using Clock = std::chrono::steady_clock;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

static CredentialsReply creds(const std::string& endpoint, const char* token,
                              long long expires) {
    const std::string body =
        "{\"bucket\":\"b\",\"endpoint\":\"" + endpoint +
        "\",\"access_key_id\":\"k\",\"secret_access_key\":\"s\","
        "\"session_token\":\"" + token + "\",\"expires_at\":" +
        std::to_string(expires) + ",\"role\":\"decoder\"}";
    return cloud_parse_credentials(body, 200);
}

int main() {
    int srv = ::socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { std::printf("socket() failed\n"); return 1; }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(srv, (sockaddr*)&addr, sizeof(addr)) != 0) {
        std::printf("bind() failed\n"); return 1;
    }
    socklen_t alen = sizeof(addr);
    ::getsockname(srv, (sockaddr*)&addr, &alen);
    const int port = ntohs(addr.sin_port);
    ::listen(srv, 4);

    // Accept every connection and hold each one silent.
    std::atomic<int> accepted{0};
    std::atomic<bool> stop{false};
    std::vector<int> held;
    std::thread listener([&] {
        while (!stop.load()) {
            int c = ::accept(srv, nullptr, nullptr);
            if (c < 0) break;
            held.push_back(c);
            accepted++;
        }
    });

    const std::string endpoint = "127.0.0.1:" + std::to_string(port);
    CloudIdentity id;
    id.set_enrolment("https://collector", "app-1", "bearer");
    id.on_credentials(creds(endpoint, "token-1", 4102444800000LL), 1000);

    CloudStorageConfig cfg;
    cfg.use_https = false;
    // Generous, so a pass means cancellation did it and not a short timeout.
    cfg.connect_timeout_ms = 8000;
    cfg.request_timeout_ms = 8000;
    CloudTransport tx(id, CloudRole::Decoder, cfg);

    std::printf("== a stop after a refresh reaches the request begun before it ==\n");
    {
        std::atomic<long long> old_ms{-1}, new_ms{-1};
        const auto t0 = Clock::now();
        auto ms_since = [&] {
            return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - t0).count();
        };

        // Begun on the first credential set's inner.
        std::thread before([&] { tx.get("before/refresh"); old_ms = ms_since(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(400));

        // The refresh: a new token, so the next request builds a new inner.
        id.on_credentials(creds(endpoint, "token-2", 4102444900000LL), 2000);
        std::thread after([&] { tx.get("after/refresh"); new_ms = ms_since(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        CHECK(accepted.load() >= 2,
              "both requests are connected and stalled (the test is in the right state)");

        tx.cancel_pending();
        before.join();
        after.join();
        std::printf("      returned after: begun-before %lld ms, begun-after %lld ms "
                    "(timeout %d ms)\n", old_ms.load(), new_ms.load(),
                    cfg.request_timeout_ms);
        CHECK(new_ms.load() < 3000, "the request on the current inner is cancelled");
        CHECK(old_ms.load() < 3000,
              "and so is the one still running on the inner the refresh replaced");
    }

    stop = true;
    ::shutdown(srv, SHUT_RDWR);
    ::close(srv);
    listener.join();
    for (int c : held) ::close(c);

    std::printf("\n%s\n", g_fail == 0 ? "ALL CLOUD CANCEL TESTS PASSED"
                                      : "SOME TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
