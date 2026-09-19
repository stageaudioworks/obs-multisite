// SPDX-License-Identifier: GPL-3.0-or-later
// The dock reads a snapshot struct, not the session. A field that disagrees
// with its source is invisible in testing and confusing in use — an earlier
// placeholder line reset `ended` to false AFTER the real assignment, so the
// state chip said "RECORDING" while the readout showed "behind live".
//
// This checks the derived values a snapshot is built from, against the session
// itself, in both live and finished states.
#include "../src/core/decoder_session.h"
#include "../src/core/checksum.h"
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
namespace fs = std::filesystem;
using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

class Store : public Transport {
public:
    std::map<std::string, std::vector<uint8_t>> objects; std::mutex mtx;
    PutResult put(const std::string& k, const std::vector<uint8_t>& b,
                  const std::string&, const std::map<std::string,std::string>&) override {
        std::lock_guard<std::mutex> lk(mtx); objects[k]=b; return {true,200,true,""}; }
    GetResult get(const std::string& k) override {
        std::lock_guard<std::mutex> lk(mtx); GetResult r; auto it=objects.find(k);
        if (it==objects.end()) { r.http_status=404; r.retryable=false; return r; }
        r.success=true; r.http_status=200; r.body=it->second; return r; }
};

int main(){
    fs::path base = fs::temp_directory_path()/"ms_snap"; fs::remove_all(base);
    fs::create_directories(base);
    Store st;
    const int64_t start = 1700000000000LL;
    Manifest m; m.event_id="01EVENTSNAPSNAPSNAPSNAPSN"; m.status="live";
    m.init="init.mp4"; m.video={"h264",1280,720,30.0};
    m.audio_tracks={{0,"Main","aac",2,48000}};
    m.first_available_seq=0; m.started_at_ms=start;
    st.put("events/"+m.event_id+"/init.mp4", std::vector<uint8_t>(1200,0x11), "", {});
    for (int i=0;i<30;++i) {
        std::vector<uint8_t> b(512,(uint8_t)i);
        char n[16]; std::snprintf(n,sizeof(n),"%08d",i);
        st.put("events/"+m.event_id+"/segments/"+n+".m4s", b, "", {});
        ManifestSegment ms; ms.seq=i; ms.duration_s=6.0; ms.checksum=sha256_hex(b);
        ms.at_ms=start+(int64_t)i*6000; m.push(ms,50);
    }
    auto publish = [&](const char* status){
        m.status = status; m.updated_at_ms = start + 30*6000;
        auto j=m.to_json();
        st.put("events/"+m.event_id+"/manifest.json",
               std::vector<uint8_t>(j.begin(),j.end()),"",{});
        LivePointer lp; lp.room_id="r"; lp.event_id=m.event_id; lp.status=status;
        lp.updated_at_ms=m.updated_at_ms; auto lj=lp.to_json();
        st.put("rooms/r/live.json", std::vector<uint8_t>(lj.begin(),lj.end()),"",{});
    };

    DecoderConfig cfg; cfg.room_id="r"; cfg.cache_dir=(base/"c").string();
    cfg.buffer_minutes=2;

    std::printf("== live event ==\n");
    publish("live");
    {
        DecoderSession d(cfg, st);
        d.poll(m.updated_at_ms);
        CHECK(d.room_state() == RoomState::Live, "room reports Live");
        CHECK(!d.event_ended(), "event_ended() agrees it is not ended");
        CHECK(d.end_media_ms() > 0, "an end position exists (the live edge)");
    }

    std::printf("== the same event, ended ==\n");
    publish("ended");
    {
        DecoderSession d(cfg, st);
        d.poll(m.updated_at_ms);
        // These three must agree. The chip reads room_state, the readout reads
        // event_ended(), and the length reads the start/end pair.
        CHECK(d.room_state() == RoomState::Ended, "room reports Ended");
        CHECK(d.event_ended(),
              "event_ended() AGREES with room_state (they must never differ)");
        // Media time begins at zero, so the end IS the length — no subtraction
        // of one clock from another.
        CHECK(d.end_media_ms() == 30 * 6000,
              "total length is the whole recording");
        CHECK(d.event_started_ms() == start, "start time is the event's start");
    }
    fs::remove_all(base);
    std::printf("\n%s\n", g_fail == 0 ? "SNAPSHOT SOURCES AGREE"
                                      : "SNAPSHOT SOURCES DISAGREE");
    return g_fail == 0 ? 0 : 1;
}
