// SPDX-License-Identifier: GPL-3.0-or-later
// test_relay_state.cpp — the awkward cases, without a destination or a wait.
//
// Everything here is a situation that would otherwise need a real stream key,
// a real outage and two hours of patience to see once. The grace period in
// particular exists because of a measured fact — ffmpeg blocks silently on a
// stalled pipe and will never tell us — so the machine has to notice on its
// own, and that noticing is what these tests pin down.
#include "relay_send.h"
#include "../src/relay_state.h"

#include <cstdio>
#include <string>

using namespace multisite_relay;
using multisite::RoomState;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

static Destination dest(const std::string& label) {
    Destination d;
    d.name = "YouTube";
    d.room_id = "main-auditorium";
    d.url = "rtmp://a.rtmp.youtube.com/live2";
    d.stream_key = "key";
    d.audio.label = label;
    d.delay_s = 180;
    return d;
}

static RelayInput live_at(int64_t now, uint64_t latest) {
    RelayInput in;
    in.now_ms = now;
    in.operator_wants_running = true;
    in.room = RoomState::Live;
    in.latest_seq = latest;
    in.first_available_seq = 0;
    in.segment_duration_s = 6.0;
    in.plan_ok = true;
    in.child_alive = false;
    in.next_segment_ready = true;
    in.init_ready = true;
    in.delay_s = 180;
    in.grace_s = 45;
    return in;
}

int main() {
    std::printf("relay state machine\n");

    // ── Taking up position ───────────────────────────────────────────────────
    {
        // 100 segments of 6s exist; a 180s delay is 30 segments back.
        CHECK(seq_behind_live(100, 0, 6.0, 180) == 70,
              "a three-minute delay starts thirty segments behind live");
        CHECK(seq_behind_live(5, 0, 6.0, 180) == 0,
              "an event that just started begins at the beginning");
        CHECK(seq_behind_live(100, 90, 6.0, 180) == 90,
              "never earlier than what storage still holds");
        CHECK(seq_behind_live(100, 0, 6.0, 0) == 100,
              "no delay means the live edge");
    }

    // ── Ordinary running ─────────────────────────────────────────────────────
    {
        RelayMachine m;
        auto in = live_at(1000, 100);
        auto d = m.step(in);
        CHECK(d.action == RelayAction::Spawn, "it starts ffmpeg when live");
        CHECK(m.head() == 70, "positioned three minutes behind live");
        CHECK(m.state() == RelayState::Streaming, "and reports it is sending");

        in.child_alive = true;
        d = m.step(in);
        CHECK(d.action == RelayAction::FeedSegment && d.seq == 70,
              "the first segment goes out immediately");

        d = m.step(in);
        CHECK(d.action == RelayAction::None,
              "the next one is not due yet — this is what holds 1x pacing");

        in.now_ms = 1000 + 6000;
        d = m.step(in);
        CHECK(d.action == RelayAction::FeedSegment && d.seq == 71,
              "it goes out one segment-duration later");
    }

    // ── A stall shorter than the grace period ────────────────────────────────
    {
        RelayMachine m;
        auto in = live_at(0, 100);
        m.step(in);                     // spawn
        in.child_alive = true;
        m.step(in);                     // feed 70

        // The main site stops. Segment 71 comes due and is not there — but a
        // segment a moment late is normal near the live edge and must not be
        // reported as a problem.
        in.now_ms = 6000;
        in.next_segment_ready = false;
        auto d = m.step(in);
        CHECK(m.state() == RelayState::Streaming,
              "a segment that is a moment late is not called a stall");

        in.now_ms = 6000 + 3000;
        d = m.step(in);
        CHECK(d.action == RelayAction::None && m.state() == RelayState::Stalled,
              "three seconds of silence is");

        // Thirty seconds of silence: still riding it out.
        in.now_ms = 36000;
        d = m.step(in);
        CHECK(d.action == RelayAction::None,
              "thirty seconds in, the connection is left alone");
        CHECK(m.state() == RelayState::Stalled, "still stalled");

        // It comes back at forty seconds, inside the grace period.
        in.now_ms = 46000;
        in.next_segment_ready = true;
    in.init_ready = true;
        d = m.step(in);
        CHECK(d.action == RelayAction::FeedSegment && d.seq == 71,
              "when content returns it resumes at the very next segment");
        CHECK(m.state() == RelayState::Streaming, "and is sending again");
        CHECK(m.restarts() == 0,
              "a short stall costs no reconnection at all");
    }

    // ── A stall longer than the grace period ─────────────────────────────────
    {
        RelayMachine m;
        auto in = live_at(0, 100);
        m.step(in);
        in.child_alive = true;
        m.step(in);

        in.now_ms = 6000;
        in.next_segment_ready = false;
        m.step(in);                     // content stops at t=6000

        in.now_ms = 6000 + 44000;       // 44s of silence
        auto d = m.step(in);
        CHECK(d.action == RelayAction::None,
              "at forty-four seconds it is still holding on");

        in.now_ms = 6000 + 45000;       // exactly the grace period
        d = m.step(in);
        CHECK(d.action == RelayAction::Kill,
              "at forty-five seconds it drops the connection deliberately");
        CHECK(m.state() == RelayState::Reconnecting, "and moves to reconnecting");
        CHECK(!m.last_error().empty(),
              "with a reason the operator can read on screen");
    }

    // ── Running close to the live edge ───────────────────────────────────────
    // The next fragment is routinely a second late. That is not a problem and
    // must not be announced as one, or the screen flickers through an event.
    {
        RelayMachine m;
        auto in = live_at(0, 100);
        m.step(in);
        in.child_alive = true;
        m.step(in);                     // feed 70

        int announcements = 0;
        for (int cycle = 0; cycle < 5; ++cycle) {
            const int64_t base = 6000 + cycle * 6000;
            in.now_ms = base;           // due, nothing there yet
            in.next_segment_ready = false;
            auto d = m.step(in);
            if (!d.note.empty()) ++announcements;

            in.now_ms = base + 1500;    // it arrives 1.5s late
            in.next_segment_ready = true;
            d = m.step(in);
            if (!d.note.empty()) ++announcements;
            CHECK(d.action == RelayAction::FeedSegment,
                  cycle == 0 ? "a late fragment is still sent" : "and again");
        }
        CHECK(announcements == 0,
              "five late fragments in a row produce no state changes at all");
        CHECK(m.state() == RelayState::Streaming, "and it never left Sending");
    }

    // ── A child that dies on its own ─────────────────────────────────────────
    {
        RelayMachine m;
        auto in = live_at(0, 100);
        m.step(in);
        in.child_alive = true;
        m.step(in);                     // fed 70

        in.now_ms = 1000;
        in.child_alive = false;         // killed externally
        auto d = m.step(in);
        CHECK(m.state() == RelayState::Reconnecting,
              "an unexpected exit is noticed");
        m.note_child_exited(1000, false);

        // The backoff is served before trying again.
        in.now_ms = 1500;
        d = m.step(in);
        CHECK(d.action == RelayAction::None, "it waits out a short backoff");

        in.now_ms = 2100;
        d = m.step(in);
        CHECK(d.action == RelayAction::Spawn, "then starts again");
        CHECK(m.head() == 71,
              "resuming where it left off, so nothing is skipped");
        CHECK(m.restarts() == 1, "and the restart is counted for the operator");
    }

    // ── A clean end ──────────────────────────────────────────────────────────
    {
        RelayMachine m;
        auto in = live_at(0, 71);       // only 71 and 72 to send
        in.delay_s = 0;                 // start at the edge for a short test
        m.step(in);
        in.child_alive = true;
        CHECK(m.head() == 71, "positioned at the live edge");
        m.step(in);                     // feed 71

        // The main site publishes one last segment, then ends the broadcast.
        in.room = RoomState::Ended;
        in.latest_seq = 72;
        in.now_ms = 6000;
        auto d = m.step(in);
        CHECK(d.action == RelayAction::FeedSegment && d.seq == 72,
              "the last segment still goes out");

        in.now_ms = 12000;
        d = m.step(in);
        CHECK(d.action == RelayAction::CloseInput,
              "then the pipe is closed so ffmpeg flushes the tail");
        CHECK(m.state() == RelayState::Ending, "and it is finishing off");

        in.child_alive = false;         // ffmpeg finished on its own
        d = m.step(in);
        CHECK(m.state() == RelayState::Stopped, "ending cleanly reaches Stopped");
    }

    // An encoder that died mid-event still gets what it managed to produce.
    {
        RelayMachine m;
        auto in = live_at(0, 70);
        in.delay_s = 0;
        m.step(in);
        in.child_alive = true;
        m.step(in);                     // feed 70
        in.room = RoomState::Interrupted;
        in.now_ms = 6000;
        auto d = m.step(in);
        CHECK(d.action == RelayAction::CloseInput &&
              d.note.find("unexpectedly") != std::string::npos,
              "an interrupted event is sent out and described as cut short");
    }

    // ── Things that must never be sent ───────────────────────────────────────
    {
        RelayMachine m;
        auto in = live_at(0, 100);
        in.plan_ok = false;
        in.plan_problem = "This event is being recorded as hevc video";
        auto d = m.step(in);
        CHECK(d.action == RelayAction::None && m.state() == RelayState::Blocked,
              "a feed that cannot be sent never spawns anything");
        CHECK(m.last_error() == in.plan_problem,
              "and the reason is what the operator sees");

        // A running relay whose feed turns bad is stopped, not left running.
        RelayMachine m2;
        auto ok = live_at(0, 100);
        m2.step(ok);
        ok.child_alive = true;
        ok.plan_ok = false;
        ok.plan_problem = "the chosen sound feed has gone";
        d = m2.step(ok);
        CHECK(d.action == RelayAction::Kill,
              "a feed that turns bad mid-event is stopped rather than "
              "sending the wrong thing");

        // ...and recovers by itself once it is valid again.
        ok.child_alive = false;
        ok.plan_ok = true;
        d = m2.step(ok);
        CHECK(m2.state() != RelayState::Blocked,
              "and unblocks when the problem goes away");
    }

    // ── Rebroadcasting a finished event ────────────────────────────────────
    // A recording played out as if it were live. It falls out of what is
    // already here: a finished event plays and then ends (§7.5), which is what
    // a rebroadcast is, so only where it starts differs.
    {
        RelayMachine m;
        auto in = live_at(0, 100);
        in.room = RoomState::Ended;         // a finished event
        in.from_beginning = true;
        in.first_available_seq = 0;
        auto d = m.step(in);
        CHECK(d.action == RelayAction::Spawn && m.head() == 0,
              "a rebroadcast starts at the beginning, not behind the live edge");

        in.child_alive = true;
        d = m.step(in);
        CHECK(d.action == RelayAction::FeedSegment && d.seq == 0,
              "and sends the first segment of the event");

        // It plays at 1x like anything else, rather than sprinting.
        d = m.step(in);
        CHECK(d.action == RelayAction::None, "at real time, not as fast as it can");

        // ...and ends by itself at the end of the recording.
        RelayMachine m2;
        auto e = live_at(0, 1);
        e.room = RoomState::Ended;
        e.from_beginning = true;
        m2.step(e);
        e.child_alive = true;
        m2.step(e);                         // 0
        e.now_ms = 6000;
        m2.step(e);                         // 1
        e.now_ms = 12000;
        d = m2.step(e);
        CHECK(d.action == RelayAction::CloseInput,
              "and closes cleanly at the end of the recording");
    }

    // ── A rebroadcast bounded by two cues ────────────────────────────────────
    // Two cues chosen as in and out points make the recording an excerpt: it
    // starts at the first and closes at the second, whatever the room is still
    // doing. Zero means "no bound", which is the behaviour tested just above.
    {
        RelayMachine m;
        auto in = live_at(0, 100);
        in.room = RoomState::Ended;
        in.from_beginning = true;
        in.start_seq = 5;                  // the in-point cue
        in.end_seq   = 7;                  // the out-point cue
        auto d = m.step(in);
        CHECK(d.action == RelayAction::Spawn && m.head() == 5,
              "a bounded rebroadcast starts at its in-point cue");

        in.child_alive = true;
        d = m.step(in);
        CHECK(d.action == RelayAction::FeedSegment && d.seq == 5,
              "and sends the in-point segment first");

        in.now_ms = 6000;  m.step(in);      // 6
        in.now_ms = 12000; m.step(in);      // 7 — the out-point segment itself
        in.now_ms = 18000;
        d = m.step(in);
        CHECK(d.action == RelayAction::CloseInput,
              "then closes at the out-point, not at the end of the recording");
    }

    // An out-point ends the excerpt while the event is still live, too — the
    // whole point of choosing one.
    {
        RelayMachine m;
        auto in = live_at(0, 100);
        in.from_beginning = true;
        in.end_seq = 3;
        m.step(in);                         // spawn at 0
        in.child_alive = true;
        m.step(in);                         // 0
        in.now_ms = 6000;  m.step(in);      // 1
        in.now_ms = 12000; m.step(in);      // 2
        in.now_ms = 18000; m.step(in);      // 3
        in.now_ms = 24000;
        auto d = m.step(in);                // 4 > 3
        CHECK(d.action == RelayAction::CloseInput,
              "an out-point closes even while the room is still live");
    }

    // ── Editing a destination ────────────────────────────────────────────────
    // An edit rebuilds the stream, but it is not a fault: it must not be
    // counted as a reconnection, must not serve a backoff, and must take up
    // position again in case the delay is what changed.
    {
        RelayMachine m;
        auto in = live_at(0, 100);
        m.step(in);
        in.child_alive = true;
        m.step(in);
        CHECK(m.head() == 71 && m.restarts() == 0, "running normally");

        m.reconfigured(5000);
        CHECK(m.restarts() == 0, "an edit is not counted as a reconnection");
        CHECK(!m.has_position(),
              "and position is given up, in case the delay is what changed");

        in.now_ms = 5000;
        in.child_alive = false;
        in.delay_s = 600;               // the operator asked for ten minutes
        auto d = m.step(in);
        CHECK(d.action == RelayAction::Spawn,
              "it starts again immediately, with no backoff");
        CHECK(m.head() == 0,
              "at the new delay — ten minutes back is before this event began");
        CHECK(m.restarts() == 0, "still not a reconnection");
    }

    // Renaming a destination is not a change to the stream at all.
    {
        Destination a = dest("Main Mix");
        Destination b = a;
        b.name = "YouTube (main event)";
        CHECK(!affects_stream(a, b), "renaming leaves a live stream alone");
        b = a; b.audio.label = "Sermon ISO";
        CHECK(affects_stream(a, b), "changing the sound feed does not");
        b = a; b.delay_s = 600;
        CHECK(affects_stream(a, b), "nor changing the delay");
        b = a; b.stream_key = "different";
        CHECK(affects_stream(a, b), "nor a new stream key");
    }

    // ── The operator's switch ────────────────────────────────────────────────
    {
        RelayMachine m;
        auto in = live_at(0, 100);
        m.step(in);
        in.child_alive = true;
        in.operator_wants_running = false;
        auto d = m.step(in);
        CHECK(d.action == RelayAction::Kill && m.state() == RelayState::Idle,
              "Stop stops it immediately, whatever else is going on");
    }

    // ── The opening data ─────────────────────────────────────────────────────
    {
        RelayMachine m;
        auto in = live_at(0, 100);
        in.init_ready = false;
        auto d = m.step(in);
        CHECK(d.action == RelayAction::None && m.state() == RelayState::Waiting,
              "nothing spawns until the opening data is downloaded");
        // ...but the position must be taken up anyway, because that is what
        // tells the downloader which part of the event to fetch. Waiting
        // for the download before choosing a position is a deadlock: it sits
        // waiting for segments nothing has been asked to bring down.
        CHECK(m.has_position() && m.head() == 70,
              "it still takes up position, so the download knows where to go");

        in.init_ready = true;
        d = m.step(in);
        CHECK(d.action == RelayAction::Spawn,
              "and it starts as soon as that arrives");
    }

    // Segment zero is an ordinary position, not a missing one — an event
    // that has only just started is relayed from its very beginning.
    {
        RelayMachine m;
        auto in = live_at(0, 3);        // 18s old, and a 3-minute delay wanted
        in.init_ready = false;
        m.step(in);
        CHECK(m.has_position() && m.head() == 0,
              "an event younger than the delay starts at segment zero");
        CHECK(m.has_position(), "and that counts as having a position");
    }

    // Nothing is spawned with nothing to send it.
    {
        RelayMachine m;
        auto in = live_at(0, 100);
        in.next_segment_ready = false;
        auto d = m.step(in);
        CHECK(d.action == RelayAction::None,
              "no ffmpeg is started before there is content for it");
        in.next_segment_ready = true;
        d = m.step(in);
        CHECK(d.action == RelayAction::Spawn, "only once there is");
    }

    // ── Waiting for an event that has not started ───────────────────────────
    {
        RelayMachine m;
        auto in = live_at(0, 0);
        in.room = RoomState::Offline;
        auto d = m.step(in);
        CHECK(d.action == RelayAction::None && m.state() == RelayState::Waiting,
              "with nothing on air it waits rather than failing");
    }


    // ── Nothing is going out ────────────────────────────────────────────────
    // The other half of the finding this machine is built around. ffmpeg says
    // nothing when it is starved, and it says nothing when the far end stops
    // reading either — so both have to be noticed here, and they mean
    // different things depending on which end opened the connection.
    {
        RelayMachine m;
        auto in = live_at(1000, 100);
        m.step(in);                       // spawn
        in.child_alive = true;
        m.step(in);                       // first segment out

        // A destination we called that stops taking content is a fault. It is
        // ridden out first, because a receiver pausing for a moment is not
        // worth splitting the recording over.
        in.output_accepting = false;
        in.output_ever_accepted = true;
        auto d = m.step(in);
        CHECK(d.action == RelayAction::None &&
              m.state() == RelayState::Streaming,
              "a destination that briefly stops reading is ridden out");

        in.now_ms += 44000;
        d = m.step(in);
        CHECK(d.action == RelayAction::None,
              "still ridden out at forty-four seconds");

        in.now_ms += 2000;
        d = m.step(in);
        CHECK(d.action == RelayAction::Kill,
              "at forty-five it is dropped, like any other lost connection");
        CHECK(m.state() == RelayState::Reconnecting, "and rebuilt");
        CHECK(m.last_error().find("accepting") != std::string::npos,
              "with a reason that says the destination stopped taking it, "
              "not that the main site stopped sending");
    }
    {
        // The same silence, on a destination that waits to be connected TO.
        // Nothing has attached yet, which is the resting state of a listener
        // and must never be given up on: a broadcast partner may well attach
        // twenty minutes into the event.
        RelayMachine m;
        auto in = live_at(1000, 100);
        in.awaits_receiver = true;
        m.step(in);
        in.child_alive = true;
        in.output_accepting = false;
        in.output_ever_accepted = false;

        auto d = m.step(in);
        CHECK(m.state() == RelayState::Awaiting,
              "a listener with nobody attached is waiting, not sending");
        CHECK(d.action == RelayAction::None, "and nothing is fed into it");

        // Well past the grace period, and past the point where a caller would
        // have been torn down twice over.
        for (int i = 0; i < 20; ++i) { in.now_ms += 60000; d = m.step(in); }
        CHECK(m.state() == RelayState::Awaiting,
              "twenty minutes later it is still waiting, not reconnecting");
        CHECK(d.action != RelayAction::Kill,
              "and has never been torn down for it");
        CHECK(m.restarts() == 0, "nor counted against it as a fault");

        // Position keeps moving with the event, so whoever finally attaches
        // gets what is happening now rather than the twenty minutes they
        // missed — and the cache is not pinned open holding it for them.
        in.latest_seq = 300;
        m.step(in);
        CHECK(m.head() == seq_behind_live(300, 0, 6.0, 180),
              "and it has kept up with the event while it waited");

        // Somebody attaches.
        const uint64_t waiting_at = m.head();
        in.output_accepting = true;
        d = m.step(in);
        CHECK(m.state() == RelayState::Streaming,
              "when the far end connects it starts sending");
        CHECK(d.action == RelayAction::FeedSegment && d.seq == waiting_at,
              "beginning with where it had got to");

        // Pacing is anchored afresh, or everything that came due during the
        // wait would be released in one burst.
        d = m.step(in);
        CHECK(d.action == RelayAction::None,
              "and at real time, not all the waiting at once");
    }
    {
        // A listener that HAD a partner and lost it is a fault like any other:
        // the distinction is whether anything ever went out, not the mode.
        RelayMachine m;
        auto in = live_at(1000, 100);
        in.awaits_receiver = true;
        m.step(in);
        in.child_alive = true;
        m.step(in);

        in.output_accepting = false;
        in.output_ever_accepted = true;
        auto d = m.step(in);
        CHECK(d.action == RelayAction::None && m.state() != RelayState::Awaiting,
              "a listener that HAS carried content is not back to waiting");
        in.now_ms += 46000;
        d = m.step(in);
        CHECK(d.action == RelayAction::Kill,
              "it is dropped and rebuilt, like any other lost connection");
    }

    // ── What counts as on air ───────────────────────────────────────────────
    // Read by three things that must agree: the page, the aggregate outbound
    // figure, and the monitoring heartbeat's cadence. It was written out twice
    // before this, which is how two of them could have drifted apart without
    // anything failing.
    {
        CHECK(relay_state_is_sending(RelayState::Streaming),
              "streaming is on air");
        CHECK(relay_state_is_sending(RelayState::Stalled),
              "so is stalled — the connection is up and being held open");
        CHECK(relay_state_is_sending(RelayState::Ending),
              "and ending, which is still sending the last of it");

        CHECK(!relay_state_is_sending(RelayState::Idle), "idle is not");
        CHECK(!relay_state_is_sending(RelayState::Waiting),
              "nor waiting for a service to start");
        CHECK(!relay_state_is_sending(RelayState::Reconnecting),
              "nor reconnecting — nothing is reaching the destination");
        CHECK(!relay_state_is_sending(RelayState::Blocked),
              "nor blocked, which needs an operator rather than a fast beat");
        CHECK(!relay_state_is_sending(RelayState::Stopped),
              "nor stopped");
    }

    std::printf("\n%s\n", g_fail == 0 ? "ALL RELAY STATE TESTS PASSED"
                                      : "SOME TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
