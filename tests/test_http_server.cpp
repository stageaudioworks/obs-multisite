// SPDX-License-Identifier: GPL-3.0-or-later
// test_http_server.cpp — the shared HTTP server, exercised over a real socket.
//
// Three things depend on this file's subject: the appliance's operator
// interface, the relay's browser, and the OBS plugin's remote-control pages. A
// routing mistake is not cosmetic — it is an operator's click doing nothing, or
// somebody reaching a page they should not. So this test speaks HTTP for real
// over loopback rather than calling the handlers directly, and it runs on
// Windows as well as POSIX, because a plugin has to work on both.
#include "../src/core/http_server.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

namespace {

#ifdef _WIN32
using sock_t = SOCKET;
constexpr sock_t kBadSocket = INVALID_SOCKET;
void close_socket(sock_t s) { ::closesocket(s); }
#else
using sock_t = int;
constexpr sock_t kBadSocket = -1;
void close_socket(sock_t s) { ::close(s); }
#endif

sock_t connect_local(int port) {
    const sock_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == kBadSocket) return kBadSocket;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port   = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(s, (sockaddr*)&a, sizeof(a)) != 0) {
        close_socket(s);
        return kBadSocket;
    }
    return s;
}

bool send_all(sock_t s, const std::string& text) {
    size_t sent = 0;
    while (sent < text.size()) {
        const int n = ::send(s, text.data() + sent,
                             (int)(text.size() - sent), 0);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

// A client connection that remembers what it has read past the end of the last
// response.
//
// Without this, a server that answers two pipelined requests in one TCP segment
// loses the second answer: the reading side consumed both and threw one away.
// That is exactly the case the keep-alive check exists to cover, and it failed
// on macOS for this reason rather than because the server was wrong.
struct Conn {
    sock_t      fd = kBadSocket;
    std::string buf;

    Conn() = default;
    Conn(const Conn&) = delete;
    Conn& operator=(const Conn&) = delete;
    ~Conn() { if (fd != kBadSocket) close_socket(fd); }
};

bool open_conn(Conn& c, int port) {
    c.fd = connect_local(port);
    return c.fd != kBadSocket;
}

// Reads exactly one response: the headers, then the body the headers promised.
// Reading "until the socket closes" would not do for the keep-alive case, which
// is the one that matters most — a phone polls this server twice a second over
// one connection.
std::string read_response(Conn& c) {
    std::string& buf = c.buf;
    char chunk[4096];

    size_t head_end = std::string::npos;
    while ((head_end = buf.find("\r\n\r\n")) == std::string::npos) {
        const long long n = ::recv(c.fd, chunk, sizeof(chunk), 0);
        if (n <= 0) return "";
        buf.append(chunk, (size_t)n);
        if (buf.size() > 1u << 20) return "";
    }

    size_t want = 0;
    bool have_length = false;
    const std::string heads = buf.substr(0, head_end);
    size_t pos = 0;
    while (pos < heads.size()) {
        const size_t nl = heads.find("\r\n", pos);
        const std::string line =
            heads.substr(pos, nl == std::string::npos ? std::string::npos
                                                      : nl - pos);
        const char* key = "Content-Length:";
        const size_t key_len = std::strlen(key);
        if (line.size() > key_len && line.compare(0, key_len, key) == 0) {
            have_length = true;
            try { want = (size_t)std::stoul(line.substr(key_len)); } catch (...) {}
        }
        if (nl == std::string::npos) break;
        pos = nl + 2;
    }

    // A streamed response promises no length, so the only end it has is the
    // connection closing — which is what the server does when the stream
    // returns.
    if (!have_length) {
        for (;;) {
            const long long n = ::recv(c.fd, chunk, sizeof(chunk), 0);
            if (n <= 0) break;
            buf.append(chunk, (size_t)n);
        }
        std::string out = std::move(buf);
        buf.clear();
        return out;
    }

    const size_t total = head_end + 4 + want;
    while (buf.size() < total) {
        const long long n = ::recv(c.fd, chunk, sizeof(chunk), 0);
        if (n <= 0) break;
        buf.append(chunk, (size_t)n);
    }

    std::string out = buf.substr(0, total);
    buf.erase(0, total);
    return out;
}

std::string round_trip(int port, const std::string& request_text) {
    Conn c;
    if (!open_conn(c, port)) return "";
    send_all(c.fd, request_text);
    return read_response(c);
}

int status_of(const std::string& r) {
    const size_t sp = r.find(' ');
    if (sp == std::string::npos || r.compare(0, 5, "HTTP/") != 0) return 0;
    try { return std::stoi(r.substr(sp + 1, 3)); } catch (...) { return 0; }
}

std::string body_of(const std::string& r) {
    const size_t e = r.find("\r\n\r\n");
    return e == std::string::npos ? std::string() : r.substr(e + 4);
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// A web root on disk, because that is what the server serves from — and
// "served from disk rather than compiled in" is the property that lets an
// operator edit the page on a running box without a rebuild.
std::filesystem::path make_web_root() {
    const auto dir = std::filesystem::temp_directory_path() /
                     "multisite-http-test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    { std::ofstream f(dir / "index.html", std::ios::binary);
      f << "<!doctype html><title>test page</title>"; }
    { std::ofstream f(dir / "style.css", std::ios::binary);
      f << "body { background: #12151a; }"; }
    // One level up, where a traversal would land if the guard ever failed.
    { std::ofstream f(dir.parent_path() / "multisite-http-secret.txt",
                      std::ios::binary);
      f << "somebody's bucket key"; }
    return dir;
}

} // namespace

int main() {
    std::printf("Percent-decoding and query strings\n");
    {
        CHECK(url_decode("a+b") == "a b", "a plus is a space");
        CHECK(url_decode("Sermon%20Start") == "Sermon Start", "%20 is a space");
        CHECK(url_decode("100%25") == "100%", "%25 is a percent sign");
        CHECK(url_decode("broken%zz") == "broken%zz",
              "a malformed escape is passed through, not mangled");

        const auto q = parse_query("label=Sermon%20Start&n=3&flag");
        CHECK(q.size() == 3, "three pairs come out of one query string");
        CHECK(q.count("label") == 1 && q.at("label") == "Sermon Start",
              "names and values are both decoded");
        CHECK(q.count("flag") == 1 && q.at("flag").empty(),
              "a name with no value is an empty value, not a missing one");
    }

    std::printf("Serving a control surface over loopback\n");
    const auto web = make_web_root();

    // Whatever the server says about itself goes to the installed sink, so the
    // test can see it — the core deliberately has no log of its own.
    std::string logged;
    http_server_set_log_sink(
        [&logged](HttpLogLevel level, const std::string& text) {
            if (level == HttpLogLevel::Error) logged = text;
        });

    // A port is picked rather than fixed: a CI runner may already have
    // something on any given one, and a test that fails for that reason trains
    // everybody to ignore it.
    std::unique_ptr<HttpServer> server;
    int port = 0;
    for (int candidate = 18741; candidate < 18761; ++candidate) {
        auto s = std::make_unique<HttpServer>("127.0.0.1", candidate);
        s->set_static_root(web.string());
        s->route("GET", "/api/ping", [](const HttpRequest&, HttpResponse& res) {
            res.json("{\"pong\":true}");
        });
        s->route("GET", "/api/echo", [](const HttpRequest& req, HttpResponse& res) {
            res.json("{\"label\":\"" + req.param("label") + "\"}");
        });
        s->route("POST", "/api/body", [](const HttpRequest& req, HttpResponse& res) {
            res.json("{\"got\":\"" + req.body + "\"}");
        });
        s->route("GET", "/api/boom", [](const HttpRequest&, HttpResponse&) {
            throw std::runtime_error("the handler fell over");
        });
        // A response produced over time, which is how a long download or a
        // preview that never ends has to be written.
        s->route("GET", "/api/stream", [](const HttpRequest&, HttpResponse& res) {
            res.content_type = "text/plain; charset=utf-8";
            res.stream = [](HttpStream& out) {
                out.write("first ");
                out.write("second");
            };
        });
        // A general prefix and a more specific one nested inside it, the
        // shape LAN delivery needs (PROJECT-SCOPE.md §8.7): one prefix for
        // "everything under this event", another for "segments specifically".
        s->route_prefix("GET", "/events/", [](const HttpRequest& req, HttpResponse& res) {
            res.json("{\"matched\":\"general\",\"path\":\"" + req.path + "\"}");
        });
        s->route_prefix("GET", "/events/E1/segments/",
                        [](const HttpRequest& req, HttpResponse& res) {
            res.json("{\"matched\":\"segments\",\"path\":\"" + req.path + "\"}");
        });

        std::string err;
        if (s->start(err)) { server = std::move(s); port = candidate; break; }
    }
    CHECK(server != nullptr, "the server bound a loopback port");
    if (!server) {
        std::printf("cannot run the socket checks without a port\n");
        return 1;
    }
    std::printf("  (listening on 127.0.0.1:%d)\n", port);

    std::printf("Routing, verbs and headers\n");
    {
        const std::string r = round_trip(port,
            "GET /api/ping HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
        CHECK(status_of(r) == 200, "a registered route answers 200");
        CHECK(contains(body_of(r), "pong"), "with what its handler produced");
        CHECK(contains(r, "application/json"), "as JSON, which is the default");
        CHECK(contains(r, "Cache-Control: no-store"),
              "and never cached — a polled page must not show a stale reading");
    }
    {
        const std::string r = round_trip(port,
            "GET /api/echo?label=Sermon%20Start&x=1 HTTP/1.1\r\n"
            "Connection: close\r\n\r\n");
        CHECK(status_of(r) == 200, "a query string reaches the handler");
        CHECK(contains(body_of(r), "Sermon Start"), "decoded, not still escaped");
    }
    {
        const std::string body = "{\"event\":\"Sunday\"}";
        const std::string r = round_trip(port,
            "POST /api/body HTTP/1.1\r\nContent-Type: application/json\r\n"
            "Content-Length: " + std::to_string(body.size()) +
            "\r\nConnection: close\r\n\r\n" + body);
        CHECK(status_of(r) == 200, "a POST carrying a body is answered");
        CHECK(contains(body_of(r), "Sunday"), "and the handler saw that body");
    }
    {
        const std::string r = round_trip(port,
            "GET /api/nothing-here HTTP/1.1\r\nConnection: close\r\n\r\n");
        CHECK(status_of(r) == 404, "a path nobody registered is a 404");
    }
    {
        const std::string r = round_trip(port,
            "POST /api/ping HTTP/1.1\r\nContent-Length: 0\r\n"
            "Connection: close\r\n\r\n");
        CHECK(status_of(r) == 405, "the wrong verb on a known path is a 405");
    }

    std::printf("Prefix routes\n");
    {
        const std::string r = round_trip(port,
            "GET /events/E2/manifest.json HTTP/1.1\r\nConnection: close\r\n\r\n");
        CHECK(status_of(r) == 200, "a path under the general prefix is matched");
        CHECK(contains(body_of(r), "\"general\""),
              "by the general handler — no more specific prefix applies to E2");
    }
    {
        const std::string r = round_trip(port,
            "GET /events/E1/segments/00000042.m4s HTTP/1.1\r\nConnection: close\r\n\r\n");
        CHECK(status_of(r) == 200, "a path under the nested prefix is matched");
        CHECK(contains(body_of(r), "\"segments\""),
              "by the MORE SPECIFIC handler, not the general one it also starts with");
    }
    {
        const std::string r = round_trip(port,
            "GET /api/ping HTTP/1.1\r\nConnection: close\r\n\r\n");
        CHECK(status_of(r) == 200 && contains(body_of(r), "pong"),
              "an exact route still wins over any prefix that would also match");
    }
    {
        const std::string r = round_trip(port,
            "GET /eventsomething HTTP/1.1\r\nConnection: close\r\n\r\n");
        CHECK(status_of(r) == 404,
              "a path that merely starts the same way, without the prefix's own "
              "trailing slash, does not match");
    }

    std::printf("The web root\n");
    {
        const std::string r = round_trip(port,
            "GET / HTTP/1.1\r\nConnection: close\r\n\r\n");
        CHECK(status_of(r) == 200 && contains(body_of(r), "test page"),
              "index.html is what / serves");
        const std::string css = round_trip(port,
            "GET /style.css HTTP/1.1\r\nConnection: close\r\n\r\n");
        CHECK(status_of(css) == 200 && contains(css, "text/css"),
              "and a stylesheet arrives as CSS, not as an octet stream");
        const std::string out = round_trip(port,
            "GET /../multisite-http-secret.txt HTTP/1.1\r\n"
            "Connection: close\r\n\r\n");
        CHECK(status_of(out) == 404 && !contains(body_of(out), "bucket key"),
              "a path climbing out of the web root is refused");
    }

    std::printf("One connection, several requests\n");
    {
        // What a phone polling this twice a second actually does. A server that
        // answered only the first request per connection would still pass every
        // check above and would feel broken to an operator.
        Conn c;
        std::string first, second;
        if (open_conn(c, port)) {
            send_all(c.fd, "GET /api/ping HTTP/1.1\r\n\r\n"
                           "GET /api/echo?label=two HTTP/1.1\r\n"
                           "Connection: close\r\n\r\n");
            first  = read_response(c);
            second = read_response(c);
        }
        CHECK(status_of(first) == 200 && contains(first, "Connection: keep-alive"),
              "the first response keeps the connection open");
        CHECK(status_of(second) == 200 && contains(body_of(second), "two"),
              "and the second request on that connection is answered too");
    }
    {
        const std::string r = round_trip(port,
            "GET /api/stream HTTP/1.1\r\nConnection: close\r\n\r\n");
        CHECK(status_of(r) == 200 && contains(body_of(r), "first second"),
              "a response written over time delivers all of it");
        CHECK(contains(r, "Connection: close"),
              "and closes once the stream has finished");
    }

    std::printf("A handler that falls over\n");
    {
        const std::string r = round_trip(port,
            "GET /api/boom HTTP/1.1\r\nConnection: close\r\n\r\n");
        CHECK(status_of(r) == 500,
              "a throwing handler becomes a 500, not a dead control surface");
        CHECK(contains(logged, "the handler fell over"),
              "and is reported through the log sink the consumer installed");
        const std::string after = round_trip(port,
            "GET /api/ping HTTP/1.1\r\nConnection: close\r\n\r\n");
        CHECK(status_of(after) == 200, "and the server carries on serving");
    }

    std::printf("A port that is already taken\n");
    {
        HttpServer other("127.0.0.1", port);
        std::string err;
        CHECK(!other.start(err), "a second server cannot take a port in use");
        CHECK(contains(err, std::to_string(port)),
              "and the reason names the port, not just \"bind failed\"");
    }

    std::printf("Stopping while a phone is still connected\n");
    {
        // What happens when OBS is closed with the operator page still open in
        // somebody's browser: the server has to take the connection down with
        // it rather than leave a thread running in a module that is being
        // unloaded around it.
        Conn c;
        CHECK(open_conn(c, port), "a connection is open");
        if (c.fd != kBadSocket) {
            send_all(c.fd, "GET /api/ping HTTP/1.1\r\n\r\n");   // kept alive
            const std::string first = read_response(c);
            CHECK(contains(first, "Connection: keep-alive"),
                  "and the browser is holding it open for its next poll");

            const auto started = std::chrono::steady_clock::now();
            server->stop();
            const double waited_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
            std::printf("  (waiting took %.0f ms)\n", waited_ms);
            CHECK(waited_ms < 1000.0,
                  "stop() waits for it and returns, rather than hanging");

            char buf[64];
            const long long n = ::recv(c.fd, buf, sizeof(buf), 0);
            CHECK(n <= 0, "and the connection is closed, not left dangling");
        }
    }

    server.reset();
    // The sink captures a local, so it must not outlive it: a detached
    // connection thread logging after main() returns would be writing into
    // nothing.
    http_server_set_log_sink(nullptr);

    std::error_code ec;
    std::filesystem::remove_all(web, ec);
    std::filesystem::remove(web.parent_path() / "multisite-http-secret.txt", ec);

    if (g_fail) {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("all http-server checks passed\n");
    return 0;
}
