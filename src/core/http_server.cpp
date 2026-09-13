// SPDX-License-Identifier: GPL-3.0-or-later
#include "http_server.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>

#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

namespace multisite {

namespace {

// ── The platform's sockets, behind one small set of names ────────────────────
//
// The appliance this server came from only ever ran on a Raspberry Pi, so it
// used POSIX sockets directly. An OBS plugin does not have that luxury: the
// same file has to compile on Windows too, where a socket handle is a UINT_PTR
// rather than a file descriptor, `close` is `closesocket`, there is no
// MSG_NOSIGNAL, and SO_REUSEADDR means something closer to the opposite of what
// it means here.

#ifdef _WIN32
using sock_t = SOCKET;
constexpr sock_t kBadSocket    = INVALID_SOCKET;
constexpr int    kNoSignalFlag = 0;            // Winsock has no MSG_NOSIGNAL
constexpr int    kShutBoth     = SD_BOTH;
#elif defined(__APPLE__)
using sock_t = int;
constexpr sock_t kBadSocket    = -1;
constexpr int    kNoSignalFlag = 0;            // macOS: SO_NOSIGPIPE instead
constexpr int    kShutBoth     = SHUT_RDWR;
#else
using sock_t = int;
constexpr sock_t kBadSocket    = -1;
constexpr int    kNoSignalFlag = MSG_NOSIGNAL;
constexpr int    kShutBoth     = SHUT_RDWR;
#endif

// A socket handle crosses the public header as a plain number, which is what
// keeps platform socket types out of it. SOCKET is a UINT_PTR on 64-bit
// Windows and INVALID_SOCKET is all-ones — exactly the -1 that means "none".
sock_t    to_sock(long long fd) { return (sock_t)fd; }
long long from_sock(sock_t fd)  { return (long long)fd; }

void close_socket(sock_t fd) {
    if (fd == kBadSocket) return;
#ifdef _WIN32
    ::closesocket(fd);
#else
    ::close(fd);
#endif
}

// Writing to a socket whose peer has gone raises SIGPIPE on POSIX, which kills
// the process. The appliance ignores that signal for the whole process; a
// plugin loaded into OBS cannot do that to somebody else's application, so the
// socket is told not to raise it instead.
void suppress_sigpipe(sock_t fd) {
#ifdef __APPLE__
    int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, (const char*)&on, sizeof(on));
#else
    (void)fd;
#endif
}

int last_socket_error() {
#ifdef _WIN32
    return ::WSAGetLastError();
#else
    return errno;
#endif
}

bool interrupted(int e) {
#ifdef _WIN32
    return e == WSAEINTR;
#else
    return e == EINTR;
#endif
}

// A read that simply ran out of time, rather than one that failed.
bool would_block(int e) {
#ifdef _WIN32
    return e == WSAETIMEDOUT || e == WSAEWOULDBLOCK;
#else
    return e == EAGAIN || e == EWOULDBLOCK || e == ETIMEDOUT;
#endif
}

// Plain language rather than strerror(): these strings end up in an operator's
// log or on a web page, where "error 10048" means nothing and "the port is
// already in use" tells somebody what to do about it.
std::string socket_error_text(int e) {
#ifdef _WIN32
    switch (e) {
    case WSAEADDRINUSE:     return "the port is already in use";
    case WSAEACCES:         return "permission denied";
    case WSAENETDOWN:       return "the network is down";
    case WSAEHOSTUNREACH:   return "the host cannot be reached";
    case WSAECONNRESET:     return "the connection was reset";
    case WSAETIMEDOUT:      return "the connection timed out";
    case WSANOTINITIALISED: return "winsock is not ready yet";
    default: return "socket error " + std::to_string(e);
    }
#else
    switch (e) {
    case EADDRINUSE: return "the port is already in use";
    case EACCES:     return "permission denied";
    default: {
        const char* s = strerror(e);
        return s ? std::string(s) : ("socket error " + std::to_string(e));
    }
    }
#endif
}

#ifdef _WIN32
// WSAStartup is per-process and Windows refcounts it, but OBS loads and unloads
// modules, so this keeps its own count and lets the last server to stop be the
// one that cleans up. Deliberately leaked: a detached connection thread must
// never touch a destroyed mutex.
struct WsaState { std::mutex mtx; int refs = 0; };
WsaState& wsa_state() { static WsaState* s = new WsaState(); return *s; }

bool sockets_init(std::string& error) {
    std::lock_guard<std::mutex> lk(wsa_state().mtx);
    WSADATA data{};
    const int rc = ::WSAStartup(MAKEWORD(2, 2), &data);
    if (rc != 0) {
        error = "winsock could not start (code " + std::to_string(rc) + ")";
        return false;
    }
    ++wsa_state().refs;
    return true;
}

void sockets_done() {
    std::lock_guard<std::mutex> lk(wsa_state().mtx);
    if (--wsa_state().refs <= 0) { wsa_state().refs = 0; ::WSACleanup(); }
}
#else
bool sockets_init(std::string&) { return true; }
void sockets_done() {}
#endif

// ── Logging ──────────────────────────────────────────────────────────────────
//
// The core has no log of its own: an appliance writes to the journal, the relay
// to `docker logs`, a plugin to OBS's log. Each installs its own sink, and with
// none installed the server stays quiet rather than writing to stderr behind
// somebody's back. Leaked for the same reason as above.
struct LogState { std::mutex mtx; HttpLogSink sink; };
LogState& log_state() { static LogState* s = new LogState(); return *s; }

void http_log(HttpLogLevel level, const std::string& text) {
    std::lock_guard<std::mutex> lk(log_state().mtx);
    if (log_state().sink) log_state().sink(level, text);
}

// ── Small shared helpers ─────────────────────────────────────────────────────

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

const char* status_text(int code) {
    switch (code) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    }
    return "OK";
}

const char* content_type_for(const std::string& path) {
    auto ends = [&](const char* ext) {
        const size_t n = std::strlen(ext);
        return path.size() >= n && path.compare(path.size() - n, n, ext) == 0;
    };
    if (ends(".html")) return "text/html; charset=utf-8";
    if (ends(".js"))   return "application/javascript; charset=utf-8";
    if (ends(".css"))  return "text/css; charset=utf-8";
    if (ends(".json")) return "application/json; charset=utf-8";
    if (ends(".svg"))  return "image/svg+xml";
    if (ends(".png"))  return "image/png";
    if (ends(".jpg") || ends(".jpeg")) return "image/jpeg";
    if (ends(".ico"))  return "image/x-icon";
    if (ends(".woff2")) return "font/woff2";
    return "application/octet-stream";
}

// A request body is a settings form, never an upload. Anything larger is a
// mistake or an attack, and reading it would only waste the machine's memory.
constexpr size_t kMaxBody = 256 * 1024;
constexpr size_t kMaxHeaderBytes = 32 * 1024;

bool write_all(sock_t fd, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    while (len > 0) {
        const int chunk = (int)std::min<size_t>(len, 0x7fffffff);
        const long long n = ::send(fd, p, chunk, kNoSignalFlag);
        if (n > 0) { p += n; len -= (size_t)n; continue; }
        if (n < 0 && interrupted(last_socket_error())) continue;
        return false;
    }
    return true;
}

// How long a read waits before giving the thread a chance to look around.
constexpr int kRecvTimeoutMs = 500;
// And how long a connection may sit with nobody saying anything before it is
// closed, which is what stops a client that connects and goes quiet from
// holding a slot for ever.
constexpr int kIdleLimitMs = 30000;

// Reads once into `buf`, reporting whether the connection is still worth
// keeping.
//
// A read timeout is not a closed connection: it is the only moment this thread
// gets to notice that the server is being stopped. Closing or shutting down a
// socket does NOT reliably wake a recv that is already blocked on it — the
// first version of this relied on that and left a thread running inside a
// plugin being unloaded, which is the crash this exists to prevent.
bool read_more(sock_t fd, std::string& buf, int& idle_ms, bool stopping) {
    char chunk[4096];
    const long long n = ::recv(fd, chunk, sizeof(chunk), 0);
    if (n > 0) {
        buf.append(chunk, (size_t)n);
        idle_ms = 0;
        return true;
    }
    if (n < 0 && would_block(last_socket_error())) {
        idle_ms += kRecvTimeoutMs;
        if (stopping || idle_ms >= kIdleLimitMs) return false;
        return true;                       // nothing yet; wait again
    }
    return false;                          // the client has gone
}

} // namespace

// ── Public entry points ──────────────────────────────────────────────────────

void http_server_set_log_sink(HttpLogSink sink) {
    std::lock_guard<std::mutex> lk(log_state().mtx);
    log_state().sink = std::move(sink);
}

// ── URL helpers ──────────────────────────────────────────────────────────────

std::string url_decode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '+') { out.push_back(' '); continue; }
        if (in[i] == '%' && i + 2 < in.size() &&
            std::isxdigit((unsigned char)in[i + 1]) &&
            std::isxdigit((unsigned char)in[i + 2])) {
            out.push_back((char)std::stoi(in.substr(i + 1, 2), nullptr, 16));
            i += 2;
            continue;
        }
        out.push_back(in[i]);
    }
    return out;
}

std::map<std::string, std::string> parse_query(const std::string& in) {
    std::map<std::string, std::string> out;
    size_t pos = 0;
    while (pos < in.size()) {
        size_t amp = in.find('&', pos);
        if (amp == std::string::npos) amp = in.size();
        const std::string pair = in.substr(pos, amp - pos);
        const size_t eq = pair.find('=');
        if (eq == std::string::npos) {
            if (!pair.empty()) out[url_decode(pair)] = "";
        } else {
            out[url_decode(pair.substr(0, eq))] = url_decode(pair.substr(eq + 1));
        }
        pos = amp + 1;
    }
    return out;
}

// ── Stream ───────────────────────────────────────────────────────────────────

bool HttpStream::write(const void* data, size_t len) {
    if (!m_alive) return false;
    if (!write_all(to_sock(m_fd), data, len)) m_alive = false;
    return m_alive;
}

// ── Server ───────────────────────────────────────────────────────────────────

HttpServer::HttpServer(std::string bind_address, int port)
    : m_bind(std::move(bind_address)), m_port(port) {}

HttpServer::~HttpServer() { stop(); }

void HttpServer::route(const std::string& method, const std::string& path,
                       HttpHandler handler) {
    m_routes[method + " " + path] = std::move(handler);
}

void HttpServer::route_prefix(const std::string& method, const std::string& prefix,
                              HttpHandler handler) {
    m_prefix_routes.emplace_back(method + " " + prefix, std::move(handler));
}

bool HttpServer::start(std::string& error) {
    error.clear();
    if (!sockets_init(error)) return false;

    const sock_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == kBadSocket) {
        error = "socket: " + socket_error_text(last_socket_error());
        sockets_done();
        return false;
    }
    suppress_sigpipe(fd);

    int on = 1;
#ifdef _WIN32
    // SO_REUSEADDR on Windows does the opposite of what it does on POSIX: it
    // lets a second process take a port that is already bound. This is the
    // option that means "the port is mine, and nobody else may have it".
    ::setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&on, sizeof(on));
#else
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof(on));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)m_port);
    if (m_bind.empty() || m_bind == "0.0.0.0") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else if (::inet_pton(AF_INET, m_bind.c_str(), &addr.sin_addr) != 1) {
        error = "not a valid bind address: " + m_bind;
        close_socket(fd);
        sockets_done();
        return false;
    }

    if (::bind(fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
        const int e = last_socket_error();
        error = "cannot listen on port " + std::to_string(m_port) + ": " +
                socket_error_text(e) +
#ifdef _WIN32
                (e == WSAEADDRINUSE ? " (is something already listening on it?)" : "");
#else
                (e == EADDRINUSE ? " (is something already listening on it?)" : "");
#endif
        close_socket(fd);
        sockets_done();
        return false;
    }
    if (::listen(fd, 16) != 0) {
        error = "listen: " + socket_error_text(last_socket_error());
        close_socket(fd);
        sockets_done();
        return false;
    }

    m_listen_fd = from_sock(fd);
    m_running = true;
    m_accept_thread = std::thread([this] { accept_loop(); });
    return true;
}

void HttpServer::stop() {
    if (!m_running.exchange(false)) return;
    // Shutting the listening socket down releases accept() immediately, which
    // is what lets a stop finish promptly rather than after a timeout.
    if (m_listen_fd != -1) {
        const sock_t fd = to_sock(m_listen_fd);
        ::shutdown(fd, kShutBoth);
        close_socket(fd);
        m_listen_fd = -1;
    }
    if (m_accept_thread.joinable()) m_accept_thread.join();
    // Then the connections that are still open, and not merely asked to go
    // away: a phone left polling this page must not be holding a thread inside
    // a module that is being unloaded around it.
    drop_connections();
    sockets_done();
}

void HttpServer::drop_connections() {
    {
        std::lock_guard<std::mutex> lk(m_conn_mutex);
        for (long long handle : m_conn_fds)
            ::shutdown(to_sock(handle), kShutBoth);
        if (m_conn_fds.empty()) return;
    }

    // A fresh lock for the wait, deliberately: a connection thread has to be
    // able to take this mutex to report that it has finished, so holding it
    // across the wait would be the very deadlock this exists to avoid.
    std::unique_lock<std::mutex> lk(m_conn_mutex);
    m_conn_done.wait_for(lk, std::chrono::seconds(5),
                         [this] { return m_conn_fds.empty(); });
}

void HttpServer::accept_loop() {
    while (m_running.load()) {
        sockaddr_in peer{};
        socklen_t len = sizeof(peer);
        const sock_t fd = ::accept(to_sock(m_listen_fd), (sockaddr*)&peer, &len);
        if (fd == kBadSocket) {
            if (!m_running.load()) break;
            if (interrupted(last_socket_error())) continue;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        if (m_connections.load() >= kMaxConnections) {
            static const char busy[] =
                "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n"
                "Content-Length: 0\r\n\r\n";
            write_all(fd, busy, sizeof(busy) - 1);
            close_socket(fd);
            continue;
        }

        suppress_sigpipe(fd);
        m_connections++;
        const long long handle = from_sock(fd);
        {
            std::lock_guard<std::mutex> lk(m_conn_mutex);
            m_conn_fds.insert(handle);
        }
        std::thread([this, handle] {
            serve_connection(handle);
            {
                // Erased BEFORE the socket is closed, and both under the mutex
                // stop() takes: a handle stop() can still see is therefore one
                // that is still open, so it can never shut down a handle the
                // operating system has already handed to somebody else.
                std::lock_guard<std::mutex> lk(m_conn_mutex);
                m_conn_fds.erase(handle);
            }
            close_socket(to_sock(handle));
            m_conn_done.notify_all();
            m_connections--;
        }).detach();
    }
}

void HttpServer::serve_connection(long long handle) {
    const sock_t fd = to_sock(handle);

    int on = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&on, sizeof(on));
    // Reads time out quickly on purpose: that is how this thread notices that
    // the server is being stopped (see read_more). Writes keep a generous
    // timeout, because a slow phone must not have its status document cut off
    // mid-sentence. Windows wants milliseconds, POSIX a timeval.
#ifdef _WIN32
    const DWORD recv_ms = kRecvTimeoutMs;
    const DWORD send_ms = 30000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&recv_ms, sizeof(recv_ms));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&send_ms, sizeof(send_ms));
#else
    timeval rtv{};
    rtv.tv_sec  = kRecvTimeoutMs / 1000;
    rtv.tv_usec = (kRecvTimeoutMs % 1000) * 1000;
    timeval stv{};
    stv.tv_sec = 30;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rtv, sizeof(rtv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&stv, sizeof(stv));
#endif

    int idle_ms = 0;
    std::string buf;
    for (;;) {
        // Read up to the end of the headers.
        size_t header_end = buf.find("\r\n\r\n");
        while (header_end == std::string::npos) {
            if (!read_more(fd, buf, idle_ms, !m_running.load())) return;
            if (buf.size() > kMaxHeaderBytes) return;
            header_end = buf.find("\r\n\r\n");
        }

        HttpRequest req;
        {
            std::istringstream hs(buf.substr(0, header_end));
            std::string line;
            if (!std::getline(hs, line)) return;
            if (!line.empty() && line.back() == '\r') line.pop_back();

            std::istringstream rl(line);
            std::string target, version;
            rl >> req.method >> target >> version;
            if (req.method.empty() || target.empty()) return;

            const size_t q = target.find('?');
            if (q == std::string::npos) {
                req.path = url_decode(target);
            } else {
                req.path  = url_decode(target.substr(0, q));
                req.query = parse_query(target.substr(q + 1));
            }

            while (std::getline(hs, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;
                const size_t colon = line.find(':');
                if (colon == std::string::npos) continue;
                std::string key = lower(line.substr(0, colon));
                size_t v = colon + 1;
                while (v < line.size() && line[v] == ' ') ++v;
                req.headers[key] = line.substr(v);
            }
        }

        size_t body_len = 0;
        {
            auto it = req.headers.find("content-length");
            if (it != req.headers.end()) {
                try { body_len = (size_t)std::stoul(it->second); } catch (...) {}
            }
        }
        if (body_len > kMaxBody) {
            static const char big[] =
                "HTTP/1.1 413 Payload Too Large\r\nConnection: close\r\n"
                "Content-Length: 0\r\n\r\n";
            write_all(fd, big, sizeof(big) - 1);
            return;
        }

        const size_t body_start = header_end + 4;
        while (buf.size() < body_start + body_len) {
            if (!read_more(fd, buf, idle_ms, !m_running.load())) return;
        }
        req.body = buf.substr(body_start, body_len);
        buf.erase(0, body_start + body_len);

        if (!handle_request(handle, req)) return;

        auto conn = req.headers.find("connection");
        if (conn != req.headers.end() && lower(conn->second) == "close") return;
    }
}

bool HttpServer::handle_request(long long handle, const HttpRequest& req) {
    const sock_t fd = to_sock(handle);
    HttpResponse res;

    auto it = m_routes.find(req.method + " " + req.path);
    const HttpHandler* handler = it != m_routes.end() ? &it->second : nullptr;

    // No exact match: the longest registered prefix for this method that the
    // path actually starts with, if any. Longest rather than first-registered
    // so a more specific prefix (e.g. "/events/<id>/segments/") can be
    // registered alongside a more general one ("/events/") without
    // registration order deciding which one answers.
    const std::string key = req.method + " ";
    size_t best_len = 0;
    const HttpHandler* prefix_handler = nullptr;
    if (!handler) {
        for (const auto& pr : m_prefix_routes) {
            if (pr.first.compare(0, key.size(), key) != 0) continue;
            const std::string& prefix = pr.first; // "METHOD prefix"
            const size_t path_len = prefix.size() - key.size();
            if (path_len <= best_len) continue;
            if (req.path.compare(0, path_len, prefix, key.size(), path_len) == 0) {
                best_len = path_len;
                prefix_handler = &pr.second;
            }
        }
    }
    if (!handler) handler = prefix_handler;

    if (handler) {
        try {
            (*handler)(req, res);
        } catch (const std::exception& e) {
            // A handler throwing must produce an error page, not kill the
            // control surface on a machine nobody can reach.
            http_log(HttpLogLevel::Error, "request " + req.method + " " +
                     req.path + " failed: " + e.what());
            res = HttpResponse{};
            res.text(500, std::string("internal error: ") + e.what());
        }
    } else if (req.method == "GET" || req.method == "HEAD") {
        if (!serve_static(req, res)) res.text(404, "not found");
    } else {
        res.text(405, "method not allowed");
    }

    std::string head;
    head.reserve(256);
    head += "HTTP/1.1 " + std::to_string(res.status) + " " +
            status_text(res.status) + "\r\n";
    head += "Content-Type: " + res.content_type + "\r\n";
    // The UI is a single page polled from a phone; caching any of it would
    // only ever show an operator a stale reading.
    if (res.headers.find("Cache-Control") == res.headers.end())
        head += "Cache-Control: no-store\r\n";
    for (const auto& h : res.headers) head += h.first + ": " + h.second + "\r\n";

    if (res.stream) {
        head += "Connection: close\r\n\r\n";
        if (!write_all(fd, head.data(), head.size())) return false;
        HttpStream stream(handle);
        try {
            res.stream(stream);
        } catch (const std::exception& e) {
            http_log(HttpLogLevel::Warn, "stream " + req.path + " ended: " + e.what());
        }
        return false;               // streamed responses always close
    }

    head += "Content-Length: " + std::to_string(res.body.size()) + "\r\n";
    head += "Connection: keep-alive\r\n\r\n";
    if (!write_all(fd, head.data(), head.size())) return false;
    if (req.method == "HEAD") return true;
    return write_all(fd, res.body.data(), res.body.size());
}

bool HttpServer::serve_static(const HttpRequest& req, HttpResponse& res) {
    if (m_static_root.empty()) return false;

    std::string rel = req.path == "/" ? "/index.html" : req.path;
    // No traversal out of the web root. A church network is not behind a
    // hardened proxy, so this check is the only thing standing between a stray
    // request and the storage credentials sitting beside the module.
    if (rel.find("..") != std::string::npos) return false;
    if (rel.empty() || rel[0] != '/') return false;

    const std::string path = m_static_root + rel;
    struct stat st{};
    // S_ISREG is not in the C runtime on Windows, so the mode bits are tested
    // the long way round, which means the same thing everywhere.
    if (::stat(path.c_str(), &st) != 0 || (st.st_mode & S_IFMT) != S_IFREG)
        return false;

    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();

    res.status = 200;
    res.content_type = content_type_for(path);
    res.body = ss.str();
    return true;
}

} // namespace multisite
