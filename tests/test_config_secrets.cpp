// SPDX-License-Identifier: GPL-3.0-or-later
// test_config_secrets.cpp — a host can keep the config's secrets in its keychain.
#include "../src/appliance/config.h"
#include "test_tmpdir.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

using namespace multisite_player;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

namespace {
std::map<std::string, std::string> g_mem;   // reset by each test
bool g_get_fails = false;

SecretStore mem_store() {
    SecretStore s;
    s.get = [](const std::string& k, std::string& v) {
        if (g_get_fails) return false;
        auto it = g_mem.find(k);
        v = it == g_mem.end() ? "" : it->second;
        return true;
    };
    s.set = [](const std::string& k, const std::string& v) { g_mem[k] = v; return true; };
    return s;
}

std::string slurp(const std::string& p) {
    std::ifstream f(p); std::stringstream ss; ss << f.rdbuf(); return ss.str();
}
bool has(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

void reset() { set_secret_store({}); g_mem.clear(); g_get_fails = false; }
} // namespace

int main() {
    std::printf("config secrets:\n");
    const auto dir = unique_temp_dir("config_secrets");
    std::filesystem::create_directories(dir);
    const std::string path = (dir / "config.json").string();
    std::string err;

    // Round trip: no secret in the file, all four back on load.
    {
        reset();
        set_secret_store(mem_store());
        Config c;
        c.bucket = "b"; c.secret_access_key = "S3CRET"; c.lan_auth_token = "LAN";
        c.reporter_token = "REP"; c.cloudflared_token = "CF";
        CHECK(c.save(path, err), "save with a store");
        const std::string file = slurp(path);
        CHECK(!has(file, "S3CRET") && !has(file, "LAN\"") && !has(file, "REP")
              && !has(file, "\"CF\""), "no secret in the file");
        CHECK(g_mem["secret_access_key"] == "S3CRET" && g_mem["cloudflared_token"] == "CF",
              "secrets are in the store");
        Config d;
        CHECK(d.load(path, err) && err.empty(), "load with a store");
        CHECK(d.bucket == "b" && d.secret_access_key == "S3CRET" && d.lan_auth_token == "LAN"
              && d.reporter_token == "REP" && d.cloudflared_token == "CF",
              "secrets come back from the store");
    }

    // An older file with secrets in it: still read, moved on the next save.
    {
        reset();
        { std::ofstream(path) << R"({"bucket":"b","secret_access_key":"OLD","reporter_token":"R"})"; }
        set_secret_store(mem_store());
        Config c;
        CHECK(c.load(path, err) && c.secret_access_key == "OLD" && c.reporter_token == "R",
              "old file's secrets still load");
        CHECK(c.save(path, err), "save moves them");
        CHECK(!has(slurp(path), "OLD") && g_mem["secret_access_key"] == "OLD"
              && g_mem["reporter_token"] == "R", "moved into the store");
    }

    // A store that cannot be read: the rest loads, the error says so.
    {
        reset();
        set_secret_store(mem_store());
        Config c; c.bucket = "keep"; c.secret_access_key = "X";
        c.save(path, err);
        g_get_fails = true;
        Config d;
        CHECK(d.load(path, err), "load survives a failing store");
        CHECK(d.bucket == "keep" && d.secret_access_key.empty(), "other fields load, secret empty");
        CHECK(err == "secret store unavailable", "error says the store is unavailable");
    }

    // No store: as before, secrets stay in the file.
    {
        reset();
        Config c; c.secret_access_key = "PLAIN";
        c.save(path, err);
        CHECK(has(slurp(path), "PLAIN"), "without a store the file keeps the secret");
    }

    reset();
    std::filesystem::remove_all(dir);
    return g_fail ? 1 : 0;
}
