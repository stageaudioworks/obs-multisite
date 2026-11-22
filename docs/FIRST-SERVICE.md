# A first service: rehearsal and fault drills

For the technical lead putting obs-multisite in front of a congregation for the
first time, with **OBS at the main site and OBS at every campus**. The
volunteers' one-page version is [VOLUNTEER-CARD.md](VOLUNTEER-CARD.md).

The six-hour soak (README, *Status*) already proved sustained upload,
timeslipping and playout on a bench. What it did not cover is **this venue's
network, this hardware, these settings and these people**. That is all this
rehearsal is for. Run it on the real machines, on the real uplink, at least
two days before the service, so there is time to act on what it finds.

## 1. Set up for resilience, not for latency

This project's safety margin is time. Spend it.

| Setting | Where | Trial value | Why |
| --- | --- | --- | --- |
| Release | GitHub releases | the same tagged build on every machine | A dev build from `main` is not what was tested. |
| Video encoder | Encoder dock → Settings | **H.264** | HEVC on VideoToolbox had an End/stop fault fixed but not re-run in OBS. |
| Audio | OBS Output → Recording tracks | **multi-track** (one source per track) | Packed 8-channel's click fix is not yet heard in a room. |
| Second bucket | Encoder dock → Settings → Second bucket | **on**, its own retention rule | An event is never lost to one provider. Run **Test upload speed** first. |
| Retention rule | Both buckets' consoles | `events/` and `rooms/`, 7 days | Nothing in the project deletes; without it the bucket grows ~2.7 GB/h. |
| Start buffer | Decoder dock → Settings → Start after this much is ready | 60 s or more | A campus starts with a reserve, not on the live edge. |
| Stay behind live by | Decoder dock | **3–5 minutes** at every campus | This is what rides out a stutter or a short outage with nothing on screen. |
| Site name | Each dock → Settings | the campus name | Cues are labelled with it; without one a box can't set cues. |
| Lock | Decoder dock and phone page | on once playing | Nothing changes what's on air by accident. |
| Monitoring heartbeat | Each dock → Settings | on, if a collector is available | One screen showing every campus's state. |

Check the **Internet** line on every dock is green before anything else. It is
live before going on air.

## 2. The rehearsal

Run for at least the length of the service plus 30 minutes, with real content
(cameras, the real audio console, the real click), on the venue's network at
the time of day the service runs if possible.

Before starting, note on each machine: OBS version, plugin version (dock
footer), free disk on the cache folder's drive.

Watch and note, every 15 minutes, at the main site and one campus:

- Encoder: confirmed segments, *waiting to send*, retries, measured upload
  rate, second bucket status.
- Campus: *behind* figure, *Could broadcast for*, dropped frames in OBS's
  stats window.

## 3. Fault drills

Do each one deliberately during the rehearsal, one at a time, and write down
what happened. Every one has a designed behaviour; anything else is a finding.

| # | Do this | Expected | If not |
| --- | --- | --- | --- |
| 1 | Unplug the **main site's** internet for 3 minutes | Encoder dock: *no connection — saving to this computer, nothing lost*. Campuses keep playing from their reserve. On reconnect the backlog drains in order and campuses catch back up to their delay. | Stop. This is the core promise. |
| 2 | Unplug **one campus's** internet for 2 minutes | That campus keeps playing; *Could broadcast for* counts down. On reconnect it refills. Other campuses unaffected. | Note the *Could broadcast for* figure when it reconnected. |
| 3 | **Force-quit OBS** at the main site mid-event and reopen it within a few minutes; press Go live | Resumes the same event with no dialog. The dock shows *Resumed event from …, N segment(s) already confirmed.* Campuses see a short gap, not a new event. | Collect the OBS log (Help → Log Files). |
| 4 | **Force-quit OBS** at a campus and reopen it | Source comes back; Follow live, buffer fills, Play. | Note how long until picture. |
| 5 | **Hold** at a campus for 2 minutes, then **Continue** | Picture resumes where held; no jump, no stall. | BUGS.md #2 is the history. |
| 6 | **Drop a cue** at the main site, then **Jump** to it at a campus | Cue appears at every site, labelled with the site name; jump lands on it. | |
| 7 | **End broadcast** | Campuses stop promptly. Any backlog finishes uploading after. Then **Check the second copy** reports both complete. | |
| 8 | **Load the recording** at a campus | The finished event plays from the start, timeline 00:00 to its end. | |

Drill 1 is the one that matters most. If time allows only one, run that.

## 4. The fallback

Every campus needs something to put on screen that does not depend on this
system, and the volunteer must know how to switch to it:

- an OBS scene with a holding slide or a pre-recorded welcome loop, and
- if the ministry already streams publicly, that stream (YouTube, Vimeo) open
  in a browser source on another scene.

Rehearse the switch once. It should take one click.

## 5. On the day

- Every machine on the tested build, the tested settings, the tested network.
- OBS and the machines restarted that morning, not mid-week.
- Main site live **at least 15 minutes** before the service, so every campus has
  its buffer and its delay in hand before it's needed.
- Someone technical reachable, with remote access to the main site.
- Keep the OBS log from every machine afterwards, good day or bad.

## 6. What to report back

For each machine: the plugin version, the log, and anything from §3 that did
not match *Expected*. A fault in a real service goes into `BUGS.md` first.
