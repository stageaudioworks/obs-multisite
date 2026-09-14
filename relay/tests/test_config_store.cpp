// SPDX-License-Identifier: GPL-3.0-or-later
// test_config_store.cpp — what the relay remembers between restarts.
//
// The things worth pinning down here are the ones that lose a church its
// setup or leak a stream key: that a saved destination comes back exactly as
// it was, that saving the storage form without retyping the secret does not
// wipe it, and that validation happens before anything reaches the database
// rather than after.
#include "../src/config_store.h"

#include <cstdio>
#include <string>
#include <unistd.h>

using namespace multisite_relay;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

static Destination sample() {
    Destination d;
    d.name = "YouTube";
    d.room_id = "main-auditorium";
    d.url = "rtmp://a.rtmp.youtube.com/live2";
    d.stream_key = "abcd-efgh-ijkl";
    d.audio.label = "Sermon ISO";
    d.delay_s = 240;
    return d;
}

int main() {
    std::printf("config store\n");
    const std::string path = "/tmp/relay_test_config.db";
    ::unlink(path.c_str());
    ::unlink((path + "-wal").c_str());
    ::unlink((path + "-shm").c_str());

    {
        ConfigStore cs;
        CHECK(cs.open(path).empty(), "a new database opens");
        CHECK(!cs.storage_configured(),
              "and reports that storage is not set up yet");

        std::string err;
        const int64_t id = cs.add(sample(), err);
        CHECK(id > 0 && err.empty(), "a valid destination saves");

        Destination bad = sample();
        bad.url = "https://youtube.com/live";
        CHECK(cs.add(bad, err) == 0 && !err.empty(),
              "an invalid one is refused before it reaches the database");
        CHECK(cs.destinations().size() == 1,
              "and leaves nothing behind when it is");

        multisite::S3Config s;
        s.bucket = "church-media";
        s.r2_account_id = "acct123";
        s.access_key_id = "AKIA";
        s.secret_access_key = "very-secret";
        s.use_https = false;
        cs.set_storage(s);
        CHECK(cs.storage_configured(), "storage is configured once it is set");

        CHECK(!cs.lan_configured(), "and LAN is not configured yet");
        CHECK(cs.configured(), "but the combined gate is already true from "
                                "cloud alone");

        cs.set_storage_provider("aws");
        CHECK(cs.storage_provider() == "aws",
              "the storage provider dropdown's choice is remembered");

        ConfigStore::LanConfig lan;
        lan.host = "192.168.1.50";
        lan.port = 9081;
        lan.auth_token = "shared-secret";
        cs.set_lan(lan);
        CHECK(cs.lan_configured(), "LAN is configured once a host is set");
    }

    // Everything must survive the process going away — that is the whole point
    // of the file.
    {
        ConfigStore cs;
        CHECK(cs.open(path).empty(), "the database reopens");

        auto all = cs.destinations();
        CHECK(all.size() == 1, "the destination is still there after a restart");
        const auto& d = all[0];
        CHECK(d.name == "YouTube" && d.url == "rtmp://a.rtmp.youtube.com/live2",
              "with its name and address intact");
        CHECK(d.stream_key == "abcd-efgh-ijkl", "and its stream key");
        CHECK(d.audio.label == "Sermon ISO",
              "and the sound feed the operator chose");
        CHECK(d.delay_s == 240, "and its delay");
        CHECK(!d.enabled, "a saved destination comes back stopped");

        const auto s = cs.storage();
        CHECK(s.bucket == "church-media" && s.access_key_id == "AKIA",
              "the storage settings survive too");
        CHECK(s.secret_access_key == "very-secret", "including the secret");
        CHECK(s.use_https == false,
              "and the choice to use plain HTTP, which must not silently "
              "flip back on");
        CHECK(cs.storage_provider() == "aws",
              "the storage provider dropdown's choice survives a restart too");

        const auto lan = cs.lan();
        CHECK(lan.host == "192.168.1.50" && lan.port == 9081,
              "and the LAN settings come back the same way");
        CHECK(lan.auth_token == "shared-secret", "including the shared token");
        CHECK(cs.lan_configured() && cs.configured(),
              "so a restarted relay still knows both paths are set up");

        // Enabling is what Start does, and it has to outlive a restart or a
        // container replacement mid-event would come back doing nothing.
        cs.set_enabled(d.id, true);
        CHECK(cs.destination(d.id)->enabled, "starting a destination persists");

        Destination up = *cs.destination(d.id);
        up.name = "YouTube (main)";
        std::string err;
        CHECK(cs.update(up, err), "a destination can be renamed");
        CHECK(cs.destination(d.id)->name == "YouTube (main)", "and it sticks");
        CHECK(cs.destination(d.id)->stream_key == "abcd-efgh-ijkl",
              "without disturbing the stream key");

        CHECK(cs.remove(d.id), "and removed");
        CHECK(cs.destinations().empty(), "leaving none");
    }

    // An SRT destination has parts an RTMP one does not, and they have to
    // survive the trip through the database intact — a passphrase that comes
    // back empty is a stream that will not connect, on a Sunday, with nothing
    // saying why.
    {
        ConfigStore cs;
        CHECK(cs.open(path).empty(), "the database reopens");

        Destination srt;
        srt.name = "Partner feed";
        srt.room_id = "main-auditorium";
        // Pasted whole, the way a partner usually hands one over.
        srt.url = "srt://ingest.example.com:9000?streamid=abc&"
                  "passphrase=hunter2hunter2&latency=1500000";

        std::string err;
        const int64_t id = cs.add(srt, err);
        CHECK(id > 0 && err.empty(), "a pasted SRT address saves");

        auto back = cs.destination(id);
        CHECK(back.has_value(), "and reads back");
        CHECK(back->url == "srt://ingest.example.com:9000",
              "with the secrets taken out of the address, as stored");
        CHECK(back->stream_key == "abc", "the stream id kept separately");
        CHECK(back->srt_passphrase == "hunter2hunter2",
              "the passphrase with it");
        CHECK(back->srt_latency_ms == 1500,
              "and the latency in the units the operator is shown");

        Destination listener;
        listener.name = "Hardware decoder";
        listener.room_id = "main-auditorium";
        listener.url = "srt://:9000";
        CHECK(cs.add(listener, err) > 0 && err.empty(),
              "so does a listener with no host at all");
        for (const auto& x : cs.destinations())
            if (x.name == "Hardware decoder")
                CHECK(x.srt_mode == SrtMode::Listener,
                      "and it is still a listener when it comes back");

        for (const auto& x : cs.destinations()) cs.remove(x.id);
    }

    ::unlink(path.c_str());
    ::unlink((path + "-wal").c_str());
    ::unlink((path + "-shm").c_str());

    std::printf("\n%s\n", g_fail == 0 ? "ALL CONFIG STORE TESTS PASSED"
                                      : "SOME TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
