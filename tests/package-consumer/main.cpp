// One call into each of the three libraries, so the link is real.
#include "cmaf_muxer.h"
#include "collector_client.h"
#include "heartbeat_reporter.h"

#include <cstdio>

int main() {
    const std::string kind = multisite::heartbeat_player_kind("outpost-light");
    const std::string url = multisite::collector_url("https://collector.example", "/v1/x");
    multisite::CmafTrack t;
    t.kind = multisite::CmafTrack::Audio;
    multisite::CmafMuxer mux({t}, 6.0);
    std::printf("core: %s  s3: %s  cmaf: constructed (ok=%d)\n",
                kind.c_str(), url.c_str(), (int)mux.ok());
    return kind == "outpost-light" && !url.empty() ? 0 : 1;
}
