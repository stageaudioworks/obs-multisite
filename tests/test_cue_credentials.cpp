// SPDX-License-Identifier: GPL-3.0-or-later
// test_cue_credentials.cpp — a paired decoder's second credential, for its own
// cue file per event (cue_credentials.h; multisite-cloud TELEMETRY.md §4).
//
// Pinned here: the reply is parsed by the main credential's parser plus
// object_key, and a key outside this event's cues/ is refused; the lifecycle
// fetches on JOINING an event (not on a cue), refreshes at half-life, keeps the
// last good set through an outage, stops for good on 403, and treats 409 and
// 400 as answers rather than retries.
#include "../src/core/cue_credentials.h"

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

using namespace multisite;
using O = CueCredentialsReply::Outcome;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

static const char* kEvent = "01M371AFJJDMER8XYGSC4JJAT6";

static std::string body(const std::string& key, long long expires_ms) {
    return "{\"endpoint\":\"https://acct.r2.example\",\"bucket\":\"org\","
           "\"region\":\"auto\",\"access_key_id\":\"AK\",\"secret_access_key\":\"SK\","
           "\"session_token\":\"ST\",\"role\":\"decoder\",\"expires_at\":" +
           std::to_string(expires_ms) + ",\"object_key\":\"" + key + "\"}";
}
static std::string good_key() {
    return std::string("events/") + kEvent + "/cues/north-campus-2062aa.json";
}

int main() {
    const long long t0 = 1'800'000'000'000LL;
    const long long hour = 3'600'000LL;

    std::printf("== the reply ==\n");
    {
        const auto r = cloud_parse_cue_credentials(body(good_key(), t0 + 12 * hour), 200, kEvent);
        CHECK(r.outcome == O::Ok, "a 200 with a key in this event's cues/ is accepted");
        CHECK(r.object_key == good_key(), "and the key is taken exactly as given");
        CHECK(r.creds.session_token == "ST" && r.creds.access_key_id == "AK",
              "with the credential fields the main parser reads");

        CHECK(cloud_parse_cue_credentials("", 403, kEvent).outcome == O::Unpaired,
              "403 is unpaired");
        CHECK(cloud_parse_cue_credentials("", 409, kEvent).outcome == O::NotADecoder,
              "409 is 'an encoder writes with its own'");
        CHECK(cloud_parse_cue_credentials("", 400, kEvent).outcome == O::BadEvent,
              "400 is a bad event id");
        CHECK(cloud_parse_cue_credentials("", 401, kEvent).outcome == O::Failed,
              "401 is an ordinary failure, retried, as for the main credential");
        CHECK(cloud_parse_cue_credentials("not json", 200, kEvent).outcome == O::Failed,
              "a body that does not parse is a failure, not a throw");

        const std::string other = "events/01M371AFJJDMER8XYGSC4JJZZZ/cues/north.json";
        CHECK(cloud_parse_cue_credentials(body(other, t0 + hour), 200, kEvent).outcome == O::Failed,
              "a key in ANOTHER event is refused");
        const std::string manifest = std::string("events/") + kEvent + "/manifest.json";
        CHECK(cloud_parse_cue_credentials(body(manifest, t0 + hour), 200, kEvent).outcome == O::Failed,
              "a key outside cues/ is refused");
        const std::string deeper = std::string("events/") + kEvent + "/cues/a/b.json";
        CHECK(cloud_parse_cue_credentials(body(deeper, t0 + hour), 200, kEvent).outcome == O::Failed,
              "a key deeper than cues/ is refused");
        CHECK(cloud_parse_cue_credentials(body("", t0 + hour), 200, kEvent).outcome == O::Failed,
              "a reply with no key is refused");

        CHECK(cue_credentials_request(kEvent) ==
                  std::string("{\"event_id\":\"") + kEvent + "\"}",
              "the request body is {\"event_id\": …}");
        CHECK(cue_credentials_request("a\"b").find("a\\\"b") != std::string::npos,
              "and an id cannot break out of its string");
    }

    std::printf("== fetched on joining an event, not on a cue ==\n");
    {
        CueCredentials c;
        CHECK(!c.tick(t0).fetch, "nothing is fetched before an event is joined");
        CHECK(!c.held(kEvent, t0).present, "and nothing is held");

        c.want_event(kEvent);
        const auto due = c.tick(t0);
        CHECK(due.fetch && due.event_id == kEvent, "joining one makes it due at once");

        c.on_reply(kEvent, cloud_parse_cue_credentials(
                               body(good_key(), t0 + 12 * hour), 200, kEvent), t0);
        CHECK(!c.tick(t0 + hour).fetch, "an hour in, nothing is due");
        CHECK(c.tick(t0 + 6 * hour).fetch, "at half the twelve hours, it is refreshed");
        const auto h = c.held(kEvent, t0 + hour);
        CHECK(h.present && h.object_key == good_key(), "it is held for a cue to use");
        CHECK(!c.held("01M371AFJJDMER8XYGSC4JJZZZ", t0 + hour).present,
              "and only for its own event");
    }

    std::printf("== an outage keeps the last good set ==\n");
    {
        CueCredentials c;
        c.want_event(kEvent);
        c.on_reply(kEvent, cloud_parse_cue_credentials(
                               body(good_key(), t0 + 12 * hour), 200, kEvent), t0);
        CueCredentialsReply down; down.outcome = O::Unreachable;
        c.on_reply(kEvent, down, t0 + 6 * hour);
        CHECK(c.held(kEvent, t0 + 6 * hour).present,
              "a collector outage mid-service does not stop cues");
        CHECK(!c.tick(t0 + 6 * hour + 10'000).fetch &&
                  c.tick(t0 + 6 * hour + 5 * 60 * 1000).fetch,
              "and it is asked again within five minutes, not hours later");
        CHECK(!c.held(kEvent, t0 + 12 * hour).present,
              "until the set actually expires");
        CHECK(c.held(kEvent, t0 + 12 * hour).why.find("expired") != std::string::npos,
              "and then it says so");

        CueCredentials never;
        never.want_event(kEvent);
        never.on_reply(kEvent, down, t0);
        const auto h = never.held(kEvent, t0);
        CHECK(!h.present && h.why.find("could not be reached") != std::string::npos,
              "never fetched: nothing held, and the reason is the outage");
        CHECK(!never.tick(t0 + 5'000).fetch && never.tick(t0 + 15'000).fetch,
              "retried every fifteen seconds while there is nothing");
    }

    std::printf("== 403 stops, 409 and 400 are answers ==\n");
    {
        CueCredentials c;
        c.want_event(kEvent);
        c.on_reply(kEvent, cloud_parse_cue_credentials(
                               body(good_key(), t0 + 12 * hour), 200, kEvent), t0);
        c.on_reply(kEvent, cloud_parse_cue_credentials("", 403, kEvent), t0 + hour);
        CHECK(!c.tick(t0 + 7 * hour).fetch, "403: no more fetches, ever");
        CHECK(c.held(kEvent, t0 + hour).present,
              "but the running event's set still covers cues until it expires");

        CueCredentials enc;
        enc.want_event(kEvent);
        enc.on_reply(kEvent, cloud_parse_cue_credentials("", 409, kEvent), t0);
        CHECK(!enc.tick(t0 + hour).fetch, "409 is not retried");
        CHECK(enc.held(kEvent, t0).why.find("encoder") != std::string::npos,
              "and held() says why");

        CueCredentials bad;
        bad.want_event(kEvent);
        bad.on_reply(kEvent, cloud_parse_cue_credentials("", 400, kEvent), t0);
        CHECK(!bad.tick(t0 + hour).fetch, "400 is not retried either");
    }

    std::printf("== another event, and a new pairing ==\n");
    {
        CueCredentials c;
        c.want_event(kEvent);
        c.on_reply(kEvent, cloud_parse_cue_credentials(
                               body(good_key(), t0 + 12 * hour), 200, kEvent), t0);
        const std::string next = "01M371AFJJDMER8XYGSC4JJB00";
        c.want_event(next);
        const auto due = c.tick(t0 + hour);
        CHECK(due.fetch && due.event_id == next, "switching events fetches the new one");
        CHECK(c.held(kEvent, t0 + hour).present,
              "and the recent one is kept, so switching back does not wait");

        for (int i = 0; i < 8; ++i) c.want_event("01M371AFJJDMER8XYGSC4JJC0" + std::to_string(i));
        CHECK(!c.held(kEvent, t0 + hour).present,
              "but not a hundred of them: old events are dropped past a small cap");

        c.reset();
        CHECK(!c.held(kEvent, t0 + hour).present, "a new pairing holds nothing of the old one");
        CHECK(c.tick(t0 + hour).fetch, "and fetches for the event playing now at once");
    }

    std::printf("== three threads at once, as the hosts use it ==\n");
    {
        // The poll loop says which event (want_event), the reporter fetches
        // (tick / on_reply), and a cue asks what to write with (held) — on
        // three threads. Run under TSan in CI; here it must at least never
        // hand out a key without its credential.
        CueCredentials c;
        std::atomic<bool> stop{false};
        std::atomic<int> torn{0};
        const std::string ev2 = "01M371AFJJDMER8XYGSC4JJB00";
        std::thread poller([&] {
            for (int i = 0; i < 20000; ++i) c.want_event(i % 2 ? kEvent : ev2);
            stop = true;
        });
        std::thread reporter([&] {
            long long t = t0;
            while (!stop.load()) {
                const auto d = c.tick(t);
                if (d.fetch) {
                    const std::string key = "events/" + d.event_id + "/cues/n-1.json";
                    c.on_reply(d.event_id, cloud_parse_cue_credentials(
                                   body(key, t + 12 * hour), 200, d.event_id), t);
                }
                t += 1000;
            }
        });
        std::thread dropper([&] {
            while (!stop.load()) {
                const auto h = c.held(kEvent, t0);
                if (h.present && (h.creds.session_token != "ST" ||
                                  h.object_key.find(kEvent) == std::string::npos))
                    ++torn;
            }
        });
        poller.join(); reporter.join(); dropper.join();
        CHECK(torn.load() == 0, "a held key always comes with its own event's credential");
    }

    std::printf("\n%s\n", g_fail == 0 ? "ALL CUE CREDENTIAL TESTS PASSED"
                                      : "SOME CUE CREDENTIAL TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
