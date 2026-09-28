// SPDX-License-Identifier: GPL-3.0-or-later
// test_aes67.cpp — the shapes the AES67 daemon's REST interface is made of.
//
// The player can configure the daemon, watch it and switch it, and every one of
// those is text: a JSON body it sends, JSON it reads back, and the SDP the
// daemon publishes. Text is what goes quietly wrong — a channel map that names
// the wrong eight channels, an address read out of the wrong line, a body
// missing the one key the daemon's parser insists on — and none of it needs a
// Pi, a kernel module or a sound card to check.
//
// So this runs anywhere. `aes67.cpp`, which does talk to the daemon, is
// deliberately not needed here: it is compiled only for the player, whereas
// this test is one of the ones CI runs on three operating systems.
#include "aes67.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace multisite_player;

static int g_fail = 0;
#define CHECK(c, m) do { if (!(c)) { std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while (0)

// A real SDP, taken verbatim from aes67-linux-daemon's daemon/README.md, where
// it is given as an example of what a discovered source announces. Keeping the
// fixture real rather than invented is the point: it is the shapes that are
// being tested, and a made-up one would only test the made-up one.
static const char* kRealSdp =
    "v=0\n"
    "o=- 2 0 IN IP4 10.0.0.13\n"
    "s=ALSA Source 2\n"
    "c=IN IP4 239.1.0.3/15\n"
    "t=0 0\n"
    "a=clock-domain:PTPv2 0\n"
    "m=audio 5004 RTP/AVP 98\n"
    "c=IN IP4 239.1.0.3/15\n"
    "a=rtpmap:98 L16/48000/2\n"
    "a=sync-time:0\n"
    "a=framecount:48\n"
    "a=ptime:1\n"
    "a=mediaclk:direct=0\n"
    "a=ts-refclk:ptp=IEEE1588-2008:00-10-4B-FF-FE-7A-87-FC:0\n"
    "a=recvonly\n";

int main() {
    // ── The channel map ──────────────────────────────────────────────────────
    // The width of the stream. Eight is what the appliance publishes; the
    // important properties are that an unset count means eight, that the
    // daemon's own limit is never exceeded, and that the answer is never zero —
    // a zero-channel source is a stream that exists and carries nothing.
    std::printf("== the channel count ==\n");
    CHECK(aes67_channels_to_map(8) == 8,          "eight stays eight");
    CHECK(aes67_channels_to_map(0) == 8,          "unset means eight");
    CHECK(aes67_channels_to_map(2) == 2,          "a narrower ask is honoured");
    CHECK(aes67_channels_to_map(64) == 64,        "the daemon's limit is exact");
    CHECK(aes67_channels_to_map(99) == 64,        "more than the limit is clamped");
    CHECK(aes67_channels_to_map(-4) == 8,         "a negative is treated as unset");
    CHECK(aes67_channels_to_map(1) >= 1,          "never zero channels");

    // ── The body the source is created with ──────────────────────────────────
    // The daemon reads these with get<>() and no default, so a body missing one
    // is rejected outright rather than defaulted. Asserting every key is here is
    // the difference between "it was accepted" and "it was accepted for the
    // reason we think".
    std::printf("== the source body ==\n");
    {
        const std::string body =
            aes67_source_body(0, 8, "239.1.0.1", "Multisite player", true);

        bool parsed = true;
        nlohmann::json j;
        try { j = nlohmann::json::parse(body); } catch (...) { parsed = false; }
        CHECK(parsed, "the body is valid JSON");

        if (parsed) {
            for (const char* key : {"id", "enabled", "name", "io",
                                    "max_samples_per_packet", "codec", "address",
                                    "ttl", "payload_type", "dscp",
                                    "refclk_ptp_traceable", "map"}) {
                CHECK(j.contains(key),
                      (std::string("the body carries ") + key).c_str());
            }
            CHECK(j.value("codec", std::string()) == "L24",
                  "it asks for L24, which is what the card is handed");
            CHECK(j.value("io", std::string()) == "Audio Device",
                  "it reads the ALSA card rather than a file");
            CHECK(j.value("address", std::string()) == "239.1.0.1",
                  "the multicast address is the one asked for");
            CHECK(j.value("payload_type", 0) == 98,
                  "the payload type is the AES67 one for L24");
            CHECK(j.value("max_samples_per_packet", 0) == 48,
                  "one millisecond of audio per packet");
            CHECK(j.value("enabled", false), "it is enabled when asked for");

            // The map is the whole point: eight channels means the first eight,
            // in order, and nothing else.
            const auto map = j["map"];
            CHECK(map.is_array() && map.size() == 8, "eight channels are mapped");
            bool in_order = map.is_array() && map.size() == 8;
            for (size_t i = 0; in_order && i < map.size(); ++i)
                in_order = map[i].is_number() && map[i].get<int>() == (int)i;
            CHECK(in_order, "the map is 0..7 in order");
        }

        // A two-channel body must map exactly two, or a stereo source would
        // claim eight channels and send six of nothing.
        const auto two = nlohmann::json::parse(
            aes67_source_body(0, 2, "239.1.0.1", "n", false));
        CHECK(two["map"].size() == 2, "a two-channel source maps two");
        CHECK(two["enabled"] == false, "an off source is created off");
    }

    // ── Reading the daemon's answer ──────────────────────────────────────────
    // GET /api/streams is {"sources":[...],"sinks":[...]}. What matters is that
    // one source named with one address is recognised, that a different width
    // is seen as different, and that anything unexpected is reported as "not
    // understood" rather than thrown at whoever is reading the interface.
    std::printf("== reading the source list ==\n");
    {
        const std::string streams = R"({
          "sources": [
            { "id": 0, "enabled": true, "name": "Multisite player",
              "io": "Audio Device", "codec": "L24", "address": "239.1.0.1",
              "payload_type": 98, "map": [0,1,2,3,4,5,6,7] },
            { "id": 1, "enabled": false, "name": "Stereo",
              "io": "Audio Device", "codec": "L24", "address": "239.1.0.9",
              "payload_type": 98, "map": [0,1] }
          ],
          "sinks": []
        })";

        std::vector<Aes67Source> srcs;
        CHECK(aes67_parse_sources(streams, srcs), "the streams answer parses");
        CHECK(srcs.size() == 2, "both sources are read");
        if (srcs.size() == 2) {
            CHECK(srcs[0].id == 0 && srcs[1].id == 1, "the ids come through");
            CHECK(srcs[0].address == "239.1.0.1", "the address comes through");
            CHECK(srcs[0].channels == 8,
                  "eight map entries is an eight-channel stream");
            CHECK(srcs[1].channels == 2, "two map entries is a stereo stream");
            CHECK(srcs[0].codec == "L24" && srcs[0].payload_type == 98,
                  "codec and payload type come through");
            CHECK(srcs[0].enabled && !srcs[1].enabled,
                  "the on/off state comes through, per source");

            // The question the player actually asks: is OUR source set up the
            // way we mean it to be? Another source must not answer yes.
            CHECK(srcs[0].matches_shape(8, "239.1.0.1"),
                  "our source matches the shape we want");
            CHECK(!srcs[0].matches_shape(2, "239.1.0.1"),
                  "a different width does not match");
            CHECK(!srcs[0].matches_shape(8, "239.1.0.9"),
                  "a different address does not match");
            CHECK(!srcs[1].matches_shape(8, "239.1.0.1"),
                  "another source does not stand in for ours");
            // A source that is switched off must still "match the shape", so
            // that ensuring the shape never switches back on what an operator
            // turned off on purpose. That has to be our own source, off:
            std::vector<Aes67Source> off;
            aes67_parse_sources(
                R"({"sources":[{"id":0,"enabled":false,"codec":"L24",)"
                R"("address":"239.1.0.1","payload_type":98,)"
                R"("map":[0,1,2,3,4,5,6,7]}]})", off);
            CHECK(off.size() == 1 && !off[0].enabled &&
                  off[0].matches_shape(8, "239.1.0.1"),
                  "a switched-off source still matches the shape");
        }

        // GET /api/sources is the same payload without the sinks.
        std::vector<Aes67Source> only;
        CHECK(aes67_parse_sources(R"({"sources":[{"id":0,"map":[0,1,2]}]})", only),
              "a sources-only answer parses");
        CHECK(only.size() == 1 && only[0].channels == 3,
              "a three-entry map is a three-channel stream");
        // Absent keys read as unset rather than throwing: a daemon that omits
        // one must not take the interface down with it.
        CHECK(only[0].address.empty() && !only[0].enabled,
              "keys that are absent read as unset rather than throwing");

        CHECK(!aes67_parse_sources("", only) && only.empty(),
              "an empty body is not an answer");
        CHECK(!aes67_parse_sources("<html>error</html>", only) && only.empty(),
              "an error page is not an answer");
        CHECK(!aes67_parse_sources(R"({"sinks":[]})", only) && only.empty(),
              "an answer with no sources is not a list of sources");
    }

    // ── What is actually on the wire ─────────────────────────────────────────
    std::printf("== the published SDP ==\n");
    {
        const Aes67Sdp s = aes67_parse_sdp(kRealSdp);
        CHECK(s.valid, "a real SDP is understood");
        CHECK(s.address == "239.1.0.3",
              "the address is read, and the /15 dropped");
        CHECK(s.port == 5004, "the port is read");
        CHECK(s.codec == "L16", "the codec is read");
        CHECK(s.sample_rate == 48000, "the rate is read");
        CHECK(s.channels == 2, "the channel count is read");
        CHECK(s.ptp_referenced, "it is referenced to a PTP clock");

        // The shape this appliance publishes. Constructed rather than captured,
        // because the point is that an L24 eight-channel source parses the same
        // way an L16 stereo one does.
        const char* ours =
            "v=0\n"
            "o=- 0 0 IN IP4 192.168.1.50\n"
            "s=Multisite player\n"
            "c=IN IP4 239.1.0.1/15\n"
            "m=audio 5004 RTP/AVP 98\n"
            "a=rtpmap:98 L24/48000/8\n"
            "a=ts-refclk:ptp=IEEE1588-2008:00-10-4B-FF-FE-7A-87-FC:0\n";
        const Aes67Sdp o = aes67_parse_sdp(ours);
        CHECK(o.valid && o.address == "239.1.0.1" && o.port == 5004,
              "our own source's address and port are read");
        CHECK(o.codec == "L24" && o.channels == 8,
              "our own source is L24, eight channels");
        CHECK(o.ptp_referenced, "our own source is referenced to PTP");

        // Lines arriving with carriage returns, as some senders write them.
        const Aes67Sdp cr = aes67_parse_sdp(
            "c=IN IP4 239.1.0.5/15\r\nm=audio 5004 RTP/AVP 98\r\n");
        CHECK(cr.valid && cr.address == "239.1.0.5" && cr.port == 5004,
              "a CRLF SDP parses too");

        // Truncated or nonsense input must not read as a working stream.
        CHECK(!aes67_parse_sdp("").valid, "an empty SDP is not valid");
        CHECK(!aes67_parse_sdp("v=0\ns=nothing here\n").valid,
              "an SDP with no address or port is not valid");
        const Aes67Sdp half = aes67_parse_sdp("c=IN IP4 239.1.0.7/15\n");
        CHECK(!half.valid && half.address == "239.1.0.7",
              "an address with no port is read but not valid");
    }

    // ── Where the daemon is ──────────────────────────────────────────────────
    std::printf("== the daemon's port ==\n");
    {
        // The installer's own file, as it writes it.
        const std::string conf =
            "{\n  \"http_port\": 8081,\n  \"rtsp_port\": 8854,\n"
            "  \"rtp_mcast_base\": \"239.1.0.1\",\n  \"rtp_port\": 5004\n}\n";
        CHECK(aes67_daemon_port_from_conf(conf) == 8081,
              "the daemon's port is read out of its configuration");
        CHECK(aes67_daemon_port_from_conf("{\"http_port\":9000}") == 9000,
              "another port is read correctly");
        CHECK(aes67_daemon_port_from_conf("{}") == 0,
              "a file that says nothing yields nothing");
        CHECK(aes67_daemon_port_from_conf("") == 0,
              "a missing file yields nothing");
        // rtsp_port must not be mistaken for http_port.
        CHECK(aes67_daemon_port_from_conf("{\"rtsp_port\":8854}") == 0,
              "the RTSP port is not mistaken for the HTTP one");
    }

    // ── The address ──────────────────────────────────────────────────────────
    std::printf("== the multicast address ==\n");
    CHECK(aes67_address_or_default("239.1.0.9") == "239.1.0.9",
          "an address the box was given is used");
    CHECK(!aes67_address_or_default("").empty(),
          "an unset address falls back rather than being blank");
    std::printf("== each box its own group (two boxes on 239.1.0.1 doubled every packet) ==\n");
    CHECK(aes67_address_for_mac("02:00:00:00:00:e0") == "239.1.0.224", "from the last two bytes");
    CHECK(aes67_address_for_mac("d8:3a:dd:12:ab:cd") == "239.1.171.205", "hex, either case");
    CHECK(aes67_address_for_mac("D8:3A:DD:12:AB:CD") == "239.1.171.205", "upper case too");
    CHECK(aes67_address_for_mac("02:00:00:00:00:e0") != aes67_address_for_mac("d8:3a:dd:12:ab:cd"),
          "two boxes, two groups");
    CHECK(aes67_address_for_mac("00:11:22:33:00:01") == "239.1.0.2", "never the old shared group");
    CHECK(aes67_address_for_mac("").empty() && aes67_address_for_mac("02:00:00:00:00").empty() &&
              aes67_address_for_mac("02:00:00:00:00:e0:ff").empty() &&
              aes67_address_for_mac("zz:00:00:00:00:e0").empty(),
          "not a MAC: nothing");
    const std::string conf = R"({"http_port": 8081, "interface_name": "eth0"})";
    std::string asked;
    CHECK(aes67_box_address_from(conf, [&](const std::string& i) { asked = i; return std::string("02:00:00:00:00:e0"); })
              == "239.1.0.224" && asked == "eth0",
          "the MAC of the card the daemon's configuration names");
    CHECK(aes67_box_address_from("{}", [](const std::string&) { return std::string("02:00:00:00:00:e0"); })
              == aes67_default_address(),
          "no interface named: the old default, as a last resort");
    CHECK(aes67_box_address_from(conf, [](const std::string&) { return std::string(); })
              == aes67_default_address(),
          "an interface with no MAC to read: likewise");
    CHECK(aes67_box_address_from("not json", [](const std::string&) { return std::string("02:00:00:00:00:e0"); })
              == aes67_default_address(),
          "a configuration that does not parse: likewise");

    // ── Finding the card to point the player at ──────────────────────────────
    // The device string is built from this, and getting it wrong means pointing
    // the player at a card that is not there — which silences the room. The
    // bracketed id is space-padded by ALSA, and the id is only one of the places
    // a card's name appears on the line.
    std::printf("== the card's own id ==\n");
    {
        // The shape /proc/asound/cards really has, with the Pi's two HDMI
        // outputs above the card we are looking for.
        const std::string cards =
            " 0 [vc4hdmi0       ]: vc4-hdmi - vc4-hdmi\n"
            "                      vc4-hdmi\n"
            " 1 [vc4hdmi1       ]: vc4-hdmi - vc4-hdmi\n"
            "                      vc4-hdmi\n"
            " 3 [RAVENNA        ]: MergingRavennaALSA - Merging RAVENNA\n"
            "                      Merging RAVENNA\n";

        CHECK(aes67_card_id_from_cards(cards, "RAVENNA") == "RAVENNA",
              "the card's id is read, with the padding stripped");
        CHECK(aes67_card_id_from_cards(cards, "NOTHING") .empty(),
              "a card that is not there yields nothing");
        CHECK(aes67_card_id_from_cards("", "RAVENNA").empty(),
              "an empty listing yields nothing");
        CHECK(aes67_card_id_from_cards(cards, "").empty(),
              "being asked for no name yields nothing");

        // The first card on the box must not be mistaken for the one asked for,
        // and an id that is not the first field must still be found — the id is
        // not the only place the name appears.
        CHECK(aes67_card_id_from_cards(cards, "RAVENNA") != "vc4hdmi0",
              "another card is not returned in its place");

        // A fifteen-character id, which is where ALSA truncates, and the
        // driver's own name rather than the id.
        const std::string truncated =
            " 2 [SomeLongCardId ]: SomeDriver - A Long Name\n";
        CHECK(aes67_card_id_from_cards(truncated, "SomeLongCardId") ==
                  "SomeLongCardId",
              "an id that fills the field comes back whole");
        CHECK(aes67_card_id_from_cards(truncated, "SomeDriver") ==
                  "SomeLongCardId",
              "the driver's name finds the card too");
        CHECK(aes67_card_id_from_cards(truncated, "A Long Name") ==
                  "SomeLongCardId",
              "so does the card's long name");

        // A line that is not a card at all — the indented continuation lines
        // under each card are the ones that catch a careless parser.
        CHECK(aes67_card_id_from_cards("      vc4-hdmi\n", "vc4-hdmi").empty(),
              "a continuation line is not a card");
    }

    // ── Telling one card from another by name ────────────────────────────────
    // Two questions hang off this comparison and both were answered by looking
    // for the name anywhere in a string, which is wrong on a box that has a
    // second card whose name merely contains it: whether the card the daemon
    // reads is registered at all, and whether the player is sending the sound to
    // it. "RAVENNA2" is a different card; treating it as this one would report a
    // stream that cannot work as healthy, and would throw away the device
    // somebody deliberately chose.
    std::printf("== one card is not another ==\n");
    {
        const std::string cards =
            " 0 [vc4hdmi0       ]: vc4-hdmi - vc4-hdmi\n"
            " 1 [RAVENNA2       ]: MergingRavennaALSA - Merging RAVENNA\n"
            " 2 [NOTRAVENNA     ]: SomeOther - Another Card\n";
        CHECK(!aes67_card_present(cards, "RAVENNA"),
              "a card whose id merely contains the name is not the card");
        CHECK(aes67_card_present(cards, "RAVENNA2"),
              "the card that really is there is found");
        CHECK(!aes67_card_present("", "RAVENNA"),
              "an empty listing registers nothing");
        CHECK(!aes67_card_present(cards, ""),
              "being asked for no name finds nothing");
        CHECK(aes67_card_ids(cards).size() == 3,
              "every card on the listing is named");

        // The device strings ALSA really uses. The CARD= field is compared
        // whole — "RAVENNA2" must not match "RAVENNA" — and the bare form is
        // the card's name on its own.
        CHECK(aes67_device_is_card("plughw:CARD=RAVENNA,DEV=0", "RAVENNA"),
              "the plug-layer device for the card matches");
        CHECK(aes67_device_is_card("hw:CARD=RAVENNA", "RAVENNA"),
              "so does the direct one, with no DEV");
        CHECK(aes67_device_is_card("RAVENNA", "RAVENNA"),
              "and the card's name on its own");
        CHECK(!aes67_device_is_card("plughw:CARD=RAVENNA2,DEV=0", "RAVENNA"),
              "a longer name is a different card");
        CHECK(!aes67_device_is_card("hw:CARD=vc4hdmi0,DEV=0", "RAVENNA"),
              "another card is not it");
        CHECK(!aes67_device_is_card("default", "RAVENNA"),
              "the system default is not a card");
        CHECK(!aes67_device_is_card("hw:0", "RAVENNA"),
              "a device named by number cannot be matched to a name");
        CHECK(!aes67_device_is_card("", "RAVENNA"), "nothing is not the card");
        CHECK(!aes67_device_is_card("hw:CARD=RAVENNA", ""),
              "and no name matches nothing");
    }

    // ── Which device the sound should be on ──────────────────────────────────
    // One rule, two callers: the reconciler and the settings page. They used to
    // answer it separately, which is exactly how the setting and the sound came
    // to disagree — the page offered a device while the reconciler was about to
    // move the sound onto the AES67 card.
    std::printf("== where the sound should be ==\n");
    {
        const std::vector<std::string> ids = {
            "hw:CARD=vc4hdmi0,DEV=0", "plughw:CARD=RAVENNA,DEV=0",
        };
        bool found = false;
        CHECK(aes67_pick_alsa_device("hw:CARD=vc4hdmi0,DEV=0", "RAVENNA", ids,
                                     &found) == "plughw:CARD=RAVENNA,DEV=0",
              "managing the network sound puts the sound on the card");
        CHECK(found, "and the device it chose is one the box has");

        // The id is taken from the box's own list rather than written down: the
        // driver's id is settable at module load, so the id is not ours to
        // guess.
        const std::vector<std::string> renamed = {
            "hw:CARD=OurOwnName,DEV=0",
        };
        CHECK(aes67_pick_alsa_device("default", "OurOwnName", renamed) ==
                  "hw:CARD=OurOwnName,DEV=0",
              "a card registered under another id is still found");

        // Nothing enumerated — a test, or a box where ALSA refused. The card is
        // named directly rather than left unset, because a device that cannot be
        // found now can be found on the next pass.
        CHECK(aes67_pick_alsa_device("default", "RAVENNA",
                                     std::vector<std::string>{}) ==
                  "hw:CARD=RAVENNA,DEV=0",
              "an unenumerated box is still pointed at the card");

        // No card at all, which is what a box without the module installed says.
        // The configured device stands, untouched.
        CHECK(aes67_pick_alsa_device("hw:CARD=vc4hdmi0,DEV=0", "", ids,
                                     &found) == "hw:CARD=vc4hdmi0,DEV=0",
              "with no card there is nothing to move the sound to");
        CHECK(!found, "and the answer says so");

        // A card that is registered by number, not by name, cannot be found in
        // the list — the direct form is used, which the plug layer resolves.
        const std::vector<std::string> numbered = {"hw:0", "hw:1"};
        CHECK(aes67_pick_alsa_device("hw:0", "RAVENNA", numbered) ==
                  "hw:CARD=RAVENNA,DEV=0",
              "a listing with no card in it falls back to naming the card");
    }

    std::printf("\n%s\n", g_fail == 0 ? "ALL AES67 TESTS PASSED"
                                      : "SOME AES67 TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
