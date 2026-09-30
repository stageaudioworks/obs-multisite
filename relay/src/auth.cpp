// SPDX-License-Identifier: GPL-3.0-or-later
#include "auth.h"
#include "text.h"
#include "log.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <chrono>
#include <cstdint>
#include <vector>

namespace multisite_relay {
using multisite::trim;

namespace {

// Enough that guessing a weak password offline is slow, few enough that a
// login on a $5 VPS still feels instant. Stored alongside the hash so it can
// be raised later without invalidating what is already saved.
constexpr int kIterations = 210000;
constexpr size_t kSaltBytes = 16;
constexpr size_t kHashBytes = 32;
constexpr int64_t kSessionLifetimeMs = 12LL * 60 * 60 * 1000;   // a long event day

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string to_hex(const unsigned char* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        out.push_back(d[p[i] >> 4]);
        out.push_back(d[p[i] & 0x0f]);
    }
    return out;
}

std::vector<unsigned char> from_hex(const std::string& s) {
    std::vector<unsigned char> out;
    if (s.size() % 2) return out;
    out.reserve(s.size() / 2);
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        const int hi = nib(s[i]), lo = nib(s[i + 1]);
        if (hi < 0 || lo < 0) return {};
        out.push_back((unsigned char)((hi << 4) | lo));
    }
    return out;
}

std::string random_hex(size_t bytes) {
    std::vector<unsigned char> buf(bytes);
    if (RAND_bytes(buf.data(), (int)bytes) != 1) return {};
    return to_hex(buf.data(), buf.size());
}

std::vector<unsigned char> derive(const std::string& password,
                                  const std::vector<unsigned char>& salt,
                                  int iterations) {
    std::vector<unsigned char> out(kHashBytes);
    if (PKCS5_PBKDF2_HMAC(password.data(), (int)password.size(),
                          salt.data(), (int)salt.size(), iterations,
                          EVP_sha256(), (int)out.size(), out.data()) != 1)
        return {};
    return out;
}


} // namespace

bool Auth::configured() const {
    return !m_cfg.get_secret("auth_user").empty() &&
           !m_cfg.get_secret("auth_hash").empty();
}

std::string Auth::set_credentials(const std::string& user_in,
                                  const std::string& password) {
    const std::string user = trim(user_in);
    if (user.empty()) return "Choose a username.";
    // Not a policy, a floor. The only thing standing between this and a
    // stranger redirecting a church's event is what gets typed here.
    if (password.size() < 10)
        return "Use a password of at least 10 characters. This is the only "
               "thing stopping a stranger from changing where your event "
               "is sent.";

    const std::string salt_hex = random_hex(kSaltBytes);
    if (salt_hex.empty()) return "Could not generate a secure password salt.";
    const auto salt = from_hex(salt_hex);
    const auto hash = derive(password, salt, kIterations);
    if (hash.empty()) return "Could not secure that password.";

    m_cfg.set_secret("auth_user", user);
    m_cfg.set_secret("auth_salt", salt_hex);
    m_cfg.set_secret("auth_iterations", std::to_string(kIterations));
    m_cfg.set_secret("auth_hash", to_hex(hash.data(), hash.size()));

    // Everything signed in under the old password stops being valid.
    std::lock_guard<std::mutex> lk(m_mtx);
    m_sessions.clear();
    return {};
}

bool Auth::verify(const std::string& user, const std::string& password) const {
    const std::string want_user = m_cfg.get_secret("auth_user");
    const std::string want_hex  = m_cfg.get_secret("auth_hash");
    const std::string salt_hex  = m_cfg.get_secret("auth_salt");
    if (want_user.empty() || want_hex.empty() || salt_hex.empty()) return false;

    int iterations = kIterations;
    try { iterations = std::stoi(m_cfg.get_secret("auth_iterations")); }
    catch (...) {}
    if (iterations < 1000) iterations = kIterations;

    // Derive regardless of whether the username matched, so a wrong username
    // and a wrong password take the same time to reject.
    const auto salt = from_hex(salt_hex);
    const auto got  = derive(password, salt, iterations);
    const auto want = from_hex(want_hex);
    if (got.empty() || want.size() != got.size()) return false;

    const bool hash_ok =
        CRYPTO_memcmp(got.data(), want.data(), got.size()) == 0;
    const bool user_ok =
        want_user.size() == trim(user).size() &&
        CRYPTO_memcmp(want_user.data(), trim(user).data(), want_user.size()) == 0;
    return hash_ok && user_ok;
}

std::string Auth::create_session() {
    const std::string token = random_hex(32);
    if (token.empty()) return {};
    std::lock_guard<std::mutex> lk(m_mtx);
    m_sessions[token] = now_ms() + kSessionLifetimeMs;
    return token;
}

bool Auth::valid_session(const std::string& token) {
    if (token.empty()) return false;
    std::lock_guard<std::mutex> lk(m_mtx);
    const int64_t t = now_ms();
    // Clearing expired entries here keeps the map from growing across a long
    // deployment without needing anything to sweep it.
    for (auto it = m_sessions.begin(); it != m_sessions.end(); )
        it = (it->second <= t) ? m_sessions.erase(it) : std::next(it);
    auto it = m_sessions.find(token);
    return it != m_sessions.end();
}

void Auth::destroy_session(const std::string& token) {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_sessions.erase(token);
}

std::string Auth::cookie_for(const std::string& token, bool secure) {
    std::string c = "relay_session=" + token +
                    "; HttpOnly; SameSite=Strict; Path=/; Max-Age=43200";
    if (secure) c += "; Secure";
    return c;
}

std::string Auth::clear_cookie() {
    return "relay_session=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0";
}

std::string Auth::session_from_cookies(const std::string& header) {
    const std::string key = "relay_session=";
    size_t p = 0;
    while ((p = header.find(key, p)) != std::string::npos) {
        // Must be at the start or just after a "; " separator, or
        // "other_relay_session=" would match.
        if (p == 0 || header[p - 1] == ' ' || header[p - 1] == ';') {
            const size_t start = p + key.size();
            const size_t end = header.find(';', start);
            return header.substr(start, end == std::string::npos
                                            ? std::string::npos
                                            : end - start);
        }
        p += key.size();
    }
    return {};
}

bool connection_is_private(const std::map<std::string, std::string>& headers) {
    auto proto = headers.find("x-forwarded-proto");
    if (proto != headers.end() && proto->second.find("https") != std::string::npos)
        return true;

    auto host = headers.find("host");
    if (host != headers.end()) {
        const std::string& h = host->second;
        if (h.rfind("localhost", 0) == 0 || h.rfind("127.0.0.1", 0) == 0 ||
            h.rfind("[::1]", 0) == 0)
            return true;
    }
    return false;
}

} // namespace multisite_relay
