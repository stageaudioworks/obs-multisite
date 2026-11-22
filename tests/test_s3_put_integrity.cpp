// SPDX-License-Identifier: GPL-3.0-or-later
// test_s3_put_integrity.cpp — the real S3Transport against a loopback server
// that records what it was sent and answers what the test tells it to.
//
//   1. Every PUT carries Content-MD5, and it is the digest of the body sent —
//      a wrong one would make every upload fail with BadDigest.
//   2. With send_content_md5 off, the header is absent.
//   3. A 400 BadDigest answer comes back retryable, with advice about damage
//      in transit rather than credentials.
//   4. A 403 AccessDenied still comes back permanent.
//
// What this does NOT show: that a given store (R2, B2, Wasabi, MinIO) accepts
// the header. That needs a real bucket — docs/scope/upload-delivery-assurance.md.
// POSIX sockets only, like test_s3_cancel.
#include "s3_transport.h"
#include "md5.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <thread>

using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

struct Captured {
    std::string headers;   // lower-cased, for finding names
    std::string raw;       // as sent, for reading values
    std::string body;
};

// Accept one connection, read one request, answer `response`.
static Captured serve_one(int srv, const std::string& response) {
    Captured cap;
    int c = ::accept(srv, nullptr, nullptr);
    if (c < 0) return cap;
    std::string buf;
    char tmp[4096];
    size_t hdr_end = std::string::npos;
    while (hdr_end == std::string::npos) {
        ssize_t n = ::recv(c, tmp, sizeof(tmp), 0);
        if (n <= 0) { ::close(c); return cap; }
        buf.append(tmp, (size_t)n);
        hdr_end = buf.find("\r\n\r\n");
    }
    cap.raw = buf.substr(0, hdr_end);
    cap.headers = cap.raw;
    std::transform(cap.headers.begin(), cap.headers.end(), cap.headers.begin(),
                   [](unsigned char ch) { return (char)std::tolower(ch); });
    size_t len = 0;
    const size_t cl = cap.headers.find("content-length:");
    if (cl != std::string::npos) len = std::stoul(cap.headers.substr(cl + 15));
    if (cap.headers.find("expect: 100-continue") != std::string::npos) {
        const std::string cont = "HTTP/1.1 100 Continue\r\n\r\n";
        ::send(c, cont.data(), cont.size(), 0);
    }
    std::string body = buf.substr(hdr_end + 4);
    while (body.size() < len) {
        ssize_t n = ::recv(c, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        body.append(tmp, (size_t)n);
    }
    cap.body = body;
    ::send(c, response.data(), response.size(), 0);
    ::close(c);
    return cap;
}

static std::string http(int status, const std::string& reason, const std::string& body) {
    return "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n"
           "Content-Length: " + std::to_string(body.size()) + "\r\n"
           "Connection: close\r\n\r\n" + body;
}

// `name` lower-case; the value is returned as sent.
static std::string header_value(const Captured& cap, const std::string& name) {
    const std::string key = "\r\n" + name + ":";
    size_t a = ("\r\n" + cap.headers).find(key);
    if (a == std::string::npos) return {};
    std::string h = "\r\n" + cap.raw;
    size_t from = a + key.size();
    size_t to = h.find("\r\n", from);
    std::string v = h.substr(from, to - from);
    while (!v.empty() && v.front() == ' ') v.erase(v.begin());
    return v;
}

int main() {
    int srv = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(srv, (sockaddr*)&addr, sizeof(addr)) != 0) { std::printf("bind failed\n"); return 1; }
    socklen_t alen = sizeof(addr);
    ::getsockname(srv, (sockaddr*)&addr, &alen);
    const int port = ntohs(addr.sin_port);
    ::listen(srv, 4);

    S3Config cfg;
    cfg.endpoint_host = "127.0.0.1:" + std::to_string(port);
    cfg.use_https = false;
    cfg.bucket = "b";
    cfg.access_key_id = "k";
    cfg.secret_access_key = "s";
    cfg.request_timeout_ms = 5000;

    // Larger than curl's Expect: 100-continue threshold, like a real segment.
    std::vector<uint8_t> body(5000);
    for (size_t i = 0; i < body.size(); ++i) body[i] = (uint8_t)(i * 31 + 7);

    auto run = [&](S3Config c, const std::string& response, PutResult& out) {
        Captured cap;
        std::thread t([&] { cap = serve_one(srv, response); });
        S3Transport tx(c);
        out = tx.put("events/E/segments/00000001.m4s", body, "video/mp4", {});
        t.join();
        return cap;
    };

    std::printf("== 1. Content-MD5 is sent, and is the body's digest ==\n");
    {
        PutResult r;
        Captured cap = run(cfg, http(200, "OK", ""), r);
        const std::string got = header_value(cap, "content-md5");
        CHECK(r.success, "the PUT succeeded against the loopback store");
        CHECK(!got.empty(), "Content-MD5 header present");
        CHECK(got == content_md5(body), "Content-MD5 matches the bytes sent");
        CHECK(cap.body.size() == body.size(), "the whole body arrived");
        CHECK(cap.headers.find("content-md5") != std::string::npos &&
              header_value(cap, "authorization").find("content-md5") != std::string::npos,
              "Content-MD5 is among the SigV4 signed headers");
    }

    std::printf("== 2. ...and absent when switched off ==\n");
    {
        S3Config off = cfg;
        off.send_content_md5 = false;
        PutResult r;
        Captured cap = run(off, http(200, "OK", ""), r);
        CHECK(header_value(cap, "content-md5").empty(), "no Content-MD5 header");
    }

    std::printf("== 3. 400 BadDigest is retried, as damage in transit ==\n");
    {
        PutResult r;
        run(cfg, http(400, "Bad Request",
                      "<?xml version=\"1.0\"?><Error><Code>BadDigest</Code>"
                      "<Message>The Content-MD5 you specified did not match what we received.</Message></Error>"), r);
        CHECK(!r.success && r.http_status == 400, "reported as a failed PUT, HTTP 400");
        CHECK(r.retryable, "retryable");
        CHECK(r.advice.find("damaged in transit") != std::string::npos,
              "advice names damage in transit, not credentials");
    }

    std::printf("== 4. 403 AccessDenied stays permanent ==\n");
    {
        PutResult r;
        run(cfg, http(403, "Forbidden", "<Error><Code>AccessDenied</Code></Error>"), r);
        CHECK(!r.retryable, "not retryable");
        CHECK(r.advice.find("credentials") != std::string::npos, "advice names the credentials");
    }

    ::close(srv);
    std::printf("\n%s (%d failure(s))\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
