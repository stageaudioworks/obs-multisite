// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// http_server.h — a control surface, in about as little code as an HTTP server
// can be written in.
//
// An operator's interface is a phone or a tablet on the church network, so
// something has to serve a web page and answer requests. It deliberately does
// NOT pull in a web framework: this has to build with one command on a stock
// Raspberry Pi OS, inside the relay's container, and inside an OBS plugin on
// Windows, and keep working for years without anyone updating a dependency
// tree — so it speaks the small part of HTTP/1.1 it actually needs and nothing
// else.
//
// It lives in the core because three consumers need exactly this and none of
// them wants a third copy: the campus player appliance, the relay, and the OBS
// plugin's remote-control pages. That is also why it is portable — POSIX
// sockets everywhere except Windows, where it is Winsock — while the appliance
// it came from never had to be.
//
// What it supports: GET/POST/PUT, query strings, a request body, keep-alive,
// static files, and streamed responses.
// What it does not: TLS, chunked request bodies, compression, or anything
// facing the public internet. This is a LAN control surface, not a web server.
//
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace multisite {

struct HttpRequest {
    std::string method;
    std::string path;                            // no query string
    std::map<std::string, std::string> query;
    std::map<std::string, std::string> headers;  // keys lowercased
    std::string body;

    std::string param(const std::string& name,
                      const std::string& fallback = "") const {
        auto it = query.find(name);
        return it == query.end() ? fallback : it->second;
    }
};

// A live connection, for responses that are produced over time rather than
// all at once. The player's preview does not use it — it returns one JPEG per
// request — but any streamed response can.
class HttpStream {
public:
    // The socket handle, kept as a number so this header stays free of
    // platform socket types: SOCKET on Windows, int everywhere else.
    explicit HttpStream(long long fd) : m_fd(fd) {}
    // Returns false once the client has gone away, which is the signal to stop
    // producing. A browser closing a preview tab must not leave a thread
    // encoding frames forever.
    bool write(const void* data, size_t len);
    bool write(const std::string& s) { return write(s.data(), s.size()); }
    bool alive() const { return m_alive; }

private:
    long long m_fd;
    bool m_alive = true;
};

struct HttpResponse {
    int         status = 200;
    std::string content_type = "application/json; charset=utf-8";
    std::string body;
    std::map<std::string, std::string> headers;

    // When set, the body is ignored and this is called with the connection
    // after the headers have gone out. The connection closes when it returns.
    std::function<void(HttpStream&)> stream;

    void json(const std::string& text) {
        content_type = "application/json; charset=utf-8";
        body = text;
    }
    void text(int code, const std::string& message) {
        status = code;
        content_type = "text/plain; charset=utf-8";
        body = message;
    }
};

using HttpHandler = std::function<void(const HttpRequest&, HttpResponse&)>;

// The core has no logging facility of its own — an appliance logs to the
// journal, the relay to `docker logs`, the plugin to OBS's log — so each
// consumer installs its own sink once at startup. With none installed, the
// server stays quiet rather than writing to stderr behind somebody's back.
enum class HttpLogLevel { Warn, Error };
using HttpLogSink = std::function<void(HttpLogLevel, const std::string&)>;
void http_server_set_log_sink(HttpLogSink sink);

class HttpServer {
public:
    HttpServer(std::string bind_address, int port);
    ~HttpServer();

    // Exact-path routes. Registered before start().
    void route(const std::string& method, const std::string& path,
               HttpHandler handler);

    // A route matched by "starts with `prefix`" rather than exact equality —
    // for a path that names something dynamic, like a segment number, where
    // registering one exact route per possible value makes no sense. Checked
    // after exact routes and before static files; the longest registered
    // prefix wins, so a more specific prefix can sit inside a more general
    // one without the general one shadowing it. Registered before start().
    void route_prefix(const std::string& method, const std::string& prefix,
                      HttpHandler handler);

    // Files served for any GET that matches no route. "/" serves index.html.
    // Served from disk rather than compiled in, so the interface can be edited
    // on a running box without a rebuild.
    void set_static_root(std::string dir) { m_static_root = std::move(dir); }

    // Binds and starts accepting. Returns false (with `error` set) if the port
    // is taken — worth reporting plainly, because the usual cause is a second
    // copy already running.
    bool start(std::string& error);
    void stop();

    int port() const { return m_port; }

private:
    void accept_loop();
    void serve_connection(long long fd);
    bool handle_request(long long fd, const HttpRequest& req);
    bool serve_static(const HttpRequest& req, HttpResponse& res);
    void drop_connections();

    std::string m_bind;
    int         m_port;
    long long   m_listen_fd = -1;
    std::string m_static_root;

    std::map<std::string, HttpHandler> m_routes;   // "GET /api/status"
    // method -> (prefix, handler), checked longest-prefix-first.
    std::vector<std::pair<std::string, HttpHandler>> m_prefix_routes;
    std::thread m_accept_thread;
    std::atomic<bool> m_running{false};

    // Connections are handled on their own detached threads. A long-lived
    // streamed response occupies one for as long as it is open, so the cap is
    // what stops a browser that keeps reconnecting from exhausting the box.
    std::atomic<int> m_connections{0};
    static constexpr int kMaxConnections = 24;

    // The live ones, by handle, so stop() can close them and wait. That is what
    // makes it safe to stop a server out from under a browser that is still
    // polling — inside a plugin being unloaded, a thread left running in this
    // code is a crash waiting for the next request.
    //
    // Both the close and the shutdown happen under this mutex: otherwise stop()
    // could shut down a handle that a finishing thread had just closed and the
    // operating system had already handed to somebody else.
    std::mutex              m_conn_mutex;
    std::condition_variable m_conn_done;
    std::set<long long>     m_conn_fds;
};

// Percent-decoding and query parsing, exposed because an API layer needs the
// same rules for form bodies.
std::string url_decode(const std::string& in);
std::map<std::string, std::string> parse_query(const std::string& in);

} // namespace multisite