# Public simulcast relay and external control API (archive)

The rationale, history and measurements that used to sit inline in
`PROJECT-SCOPE.md` §8.2 and §8.3. The scope doc now states the specification
only, in a few lines each; this file keeps the full reasoning verbatim,
including every measurement, byte value, "not built" note and reverted attempt.

> As it stood on 2026-09-21.

---

## 8.2 Public simulcast relay

The same segments that feed the campuses, pushed out to YouTube, Facebook or
any RTMP destination. A separate sub-project in `relay/`, deployed as one
Docker container on a small VPS. It is not part of the plugins and the core
knows nothing about it.

**Why relay from the bucket rather than add a second OBS output.** The main
site uploads once however many places the event goes, which is what makes
this possible at all on a venue connection that will not carry a second
upload. The public stream also inherits the buffering the campus feed already
has: the relay deliberately runs a configurable time behind the event —
three minutes by default — so a dropout at the main site is absorbed instead
of reaching air. It is the same trade as §1, applied to the public stream:
latency spent to buy resilience.

**One destination per audience, each with its own sound.** Destinations are a
list, not a setting: each has its own address, its own audio track — chosen by
the name the main site published, never by track number — and its own
supervision and reconnection. That is what makes the multi-lingual case work: the
desk's interpreter feeds travel as separate tracks, so English can go to one
stream, Spanish to another and a clean feed to a broadcast partner, all from the
single upload the main site already made and with nothing re-encoded anywhere.
Two limits belong written down rather than discovered. The *video* is identical
on every destination, because this is a copy remux and there is no transcoder —
there are no per-language bitrates and no adaptive ladder. And every destination
costs the relay's own uplink another copy of the bitrate, which is why the status
page reports the total going out rather than a per-destination rate alone.

**Two protocols, told apart by the address alone.** RTMP is what every public
streaming site accepts, so one mechanism covers YouTube, Facebook and a
church's own server. SRT is what broadcast partners, hardware decoders and the
better contribution CDNs ask for, and it is what a lossy path between the VPS
and the destination wants: it retransmits lost packets instead of letting them
become a glitch. There is no protocol setting and no radio button — `rtmp://`
and `srt://` are unmistakable, and asking a volunteer to declare which one
they pasted is asking them to get it wrong.

The two used to differ in one way that mattered upward: RTMP meant FLV, and
FLV meant H.264. That is no longer true of either half. FLV carries HEVC
through **Enhanced RTMP** — a released specification (§8.2's refusal rule
below has the detail) — so **the codec rule is no longer written per
protocol**: HEVC goes out over both, unchanged, and SRT's advantage is its
loss recovery rather than the codecs it can carry.

That matters more than it sounds. Until SRT existed here, choosing HEVC for the
campuses cost a church its public stream outright, which made a real bandwidth
saving unusable for anyone who also streams. It then cost them the *RTMP*
destinations only, and now costs them nothing.

**SRT settles for a longer latency than ffmpeg's own.** Its `latency` is the
window in which a lost packet can be asked for again; ffmpeg's default of 120ms
is enough only on a path short enough that the answer comes back almost
immediately. The relay sends 2000ms unless told otherwise. That is §1 applied
where it is cheapest — the relay is already sitting three minutes behind the
event, so two seconds is invisible, and it buys recovery across a path many
times longer than the default can manage. It is on the form, under Advanced,
for the case where it is not enough.

**SRT can also be listened for rather than called out to,** for a broadcast
partner or a hardware decoder that pulls from us. It is never the default and
is never inferred from a setting: an address with nothing before the port —
`srt://:9000` — is how one is written down, and writing it that way is how one
is asked for. It does mean opening an inbound port on a machine we have
otherwise been careful to keep closed, which is why it takes a deliberately
odd-looking address to get one.

A listener nobody has attached to yet is the reason §8.2's supervision grew a
second half. The finding it was built on is that ffmpeg says nothing when it
is *starved*; the same is true when the far end stops *reading*. Both have to
be noticed by watching, and they mean opposite things depending on which end
opened the connection. A destination we called that stops taking content has
gone wrong and is dropped and rebuilt like any other lost connection. A
listener that has never carried anything is simply waiting, possibly for the
whole first half of an event, and is neither reported nor acted on as a
failure — it keeps taking up position behind the live edge while it waits, so
whoever finally attaches gets the event as it is now rather than the forty
minutes they missed. Once a listener has carried content, losing it is a fault
like any other: the distinction is whether anything ever went out, not the
mode.

Watching the outbound side at all is new with SRT and fixes a latent hole in
the RTMP path too — before it, a destination that quietly stopped reading was
fed for ever into a pipe nobody was emptying.

**Copy remux, never a silent transcode.** Segments are pushed on unchanged: no
decode, no encode, no quality loss, and little enough CPU that the cheapest VPS
tier is the target rather than a stretch. What cannot be sent that way is
refused rather than adapted, in the two cases where adapting it silently would
put the wrong thing on air:

- **AV1.** Allowed to a streaming site, refused over SRT — and the two halves
  have entirely different reasons, which is why the rule is written per protocol
  rather than once.

  **Over RTMP it goes out**, as Enhanced RTMP, the same mechanism HEVC uses
  (verified at the byte level: the tag comes out with the extended header set
  and a FourCC of `av01`, and reads back as AV1). What remains is a *destination*
  question — YouTube documents AV1 ingest and nothing else obviously does — and
  that is the operator's to weigh rather than ours to refuse. It was refused for
  a while on exactly that basis, which was the same mistake as refusing HEVC on
  an assumption that had stopped being true; now the page carries the caveat
  above every destination ("only a site that documents AV1 ingest will take it
  over RTMP… an SRT destination cannot carry AV1 at all") and the relay's own
  supervision reports a destination that drops the stream instead of hiding it.

  **Over SRT there is nothing to send.** ffmpeg cannot put AV1 into MPEG-TS: its
  muxer has no AV1 stream type, falls back to private data and says so ("codec
  av1, is muxed as a private data stream and may not be recognized upon
  reading"), and its own demuxer reads the result back as `bin_data`. There is
  an AOMedia mapping for AV1 in MPEG-2 TS; ffmpeg implements it in neither
  direction. Carrying AV1 over SRT would mean patching ffmpeg *and* finding a
  receiver that understands the result — a different size of job, and not one to
  do for a path whose only known destination is reachable over RTMP anyway.

  An unknown codec is still refused outright on both, which is the line the gate
  draws: what we can name and carry, we carry; what we cannot, we decline with a
  sentence rather than guessing.

  This rule used to have HEVC in it too, on the grounds that FLV cannot carry
  HEVC. That was wrong twice over, and worth writing down because of how it was
  arrived at. **Enhanced RTMP** is a released specification — E-RTMP v2, whose
  contributors include Adobe, Google, Meta, Twitch, FFmpeg and OBS — which
  extends the FLV video tag with `isExVideoHeader`, a `VideoPacketType` and a
  FourCC (`avc1`, `hvc1`, `av01`). ffmpeg has written it since **6.1**
  ("Support HEVC,VP9,AV1 codec in enhanced flv format"), and YouTube documents
  H.264, H.265 and AV1 for RTMP/RTMPS ingest, recommending H.265 over RTMP(S)
  for HDR. So the destination was never refusing HEVC as such.

  What was actually happening is that the relay's container ran Debian
  bookworm's **ffmpeg 5.1**, whose FLV muxer has no HEVC in its codec-tag table
  at all and fails outright — so a test of "HEVC to RTMP" from that image
  could not have produced an Enhanced RTMP stream to be accepted or rejected.
  The constraint was measured, and measured on the wrong artefact: a toolchain
  limit and a destination limit look identical when only one of them is in
  front of you. The container is trixie now (ffmpeg 7.1), and the extended tag
  is verified at the byte level — a copy remux through the relay's own argument
  vector emits `0x90` (isExVideoHeader set, KeyFrame, SequenceStart) with a
  FourCC of `hvc1`, where H.264 emits the legacy `0x17`.

  H.264 remains the default and the roadmap's first codec precisely because it
  decodes everywhere, a Pi 5 included (§8.1: software decode handles 1080p
  comfortably), so a site that has not gone out of its way to change codec can
  stream publicly with nothing to reconsider. Re-encoding on the way out is
  still the eventual answer for the RTMP case, is not built, and would end the
  $5-a-month claim when it is.
- **Packed multi-channel audio (§4.3.1),** where the mix, the ISOs and the
  click share one track. Selecting a pair out of it is not built, and sending
  it unchanged would put a mic ISO or the click out to the public. Multi-track
  events (§4.3, the primary mode) are handled: each destination carries one
  track, chosen by the label the main site published.

**Supervision is the point, not a refinement.** Most destinations end a
broadcast after roughly a minute without data, so an unattended relay that
cannot recover is worse than none. Each destination has one ffmpeg child and
one thread that owns it; a child that dies is restarted and resumes from the
segment it was on, so nothing is skipped. A silence shorter than 45 seconds is
ridden out without dropping the connection at all — fragment timestamps are
absolute, so content resumes exactly where it stopped and a destination that
tolerates the pause never knows. Beyond that the connection is dropped
deliberately and rebuilt, which splits the recording at the far end and is
reported as such.

Detecting that silence is the relay's own job: ffmpeg given a pipe that stops
producing blocks quietly and holds the socket open indefinitely without
reporting anything, so waiting for the child to complain is waiting for ever.

**What it reuses.** The receive path, unchanged: event discovery, the durable
cache, checksum verification, and the live/ended/interrupted classification of
§7.5. It is the same code a campus runs, so the relay and a campus can never
disagree about whether an event is still running — and an event that ends
cleanly is played out to its last segment and then closed deliberately, rather
than being cut off or left to time out.

**Finished events.** The relay also does two things with events that have
already ended, both gated on the event actually being finished (§7.5.1's
classification, so it and a campus agree on what "finished" means):

- **Download as one MP4**, streamed from storage as it is requested rather than
  assembled on the server, so a two-hour event costs no disk and several
  people can download at once. It carries every audio track, not just the
  streamed one — the ISOs and the click are what a post-production edit needs.
- **Replay to a destination**, playing a finished event out at normal speed
  as though it were live, for a second congregation or an evening repeat. This
  falls out of §7.5 rather than being new machinery: a finished event already
  plays and then ends, which is what a replay is. Proof of concept — one at a
  time, started by hand.

**Access.** The relay can change where a church broadcasts, so unlike the
campus appliance it cannot rely on being on a trusted network. It requires a
login on every endpoint but the sign-in itself, stores the password as
PBKDF2-HMAC-SHA256 over a random salt, and binds to localhost so that exposing
it is a decision. It does not terminate TLS: a proxy in front of it does, and
one is shipped as a working example.

**Not built.** Re-encoding; splitting packed audio; SRT in listener mode being
reachable through anything (the port has to be published, and nothing is
shipped to help); signing in to YouTube (a stream key is pasted, and the
broadcast is still created in YouTube's own page); and starting by itself,
either on a schedule or when the encoder goes live. Scheduling matters most,
because events start late — the intended trigger is `live.json` actually
going live, optionally bounded by a time window, and `markers.json` makes
"start the public stream at Sermon Start" possible.

## 8.3 External control API (planned)

Operators reach for a physical button, not a dock. A volunteer running a
event on a Stream Deck should be able to hold, resume and catch up without
finding a window, and the main site should be able to go live from a button.
The target is **Bitfocus Companion**, which is what churches in this bracket
actually use.

**One surface, two transports.** The appliance already exposes its controls as
HTTP routes (§8.1): `/api/play`, `/api/hold`, `/api/seek`, `/api/status` and the
rest. The OBS plugins will expose *the same command names with the same
payloads* over **obs-websocket vendor requests**. One API to learn and document,
two ways in, and a control surface written against either works against both.

**Why vendor requests rather than a server inside the plugin.** obs-websocket
ships with OBS 28 and later, so there is nothing for a church to install. Its
`obs-websocket-api.h` is header-only and works through OBS's proc handler, so
the plugin gains no link dependency, and if obs-websocket is absent every call
returns false after one log line — control disappears, nothing breaks.
Authentication, the listening socket and TLS are already solved there. A
bespoke HTTP server inside the plugin would duplicate all of it and open a
second port on a machine we have otherwise been careful to keep closed.

**Both ends, equal weight.** The encoder needs go-live, stop, drop-marker and
status; the decoder needs play, stop, hold, resume, catch-up, seek, jog, delay,
load-event, follow-live, audio-track selection, status and the event list. The
command set already exists as `EncoderControls` and `DecoderControls`, so the
API layer is an adapter over what the docks and hotkeys already call — no new
control logic, and no second path to keep in step.

**Two phases, because Companion needs more than actions.**

1. **The vendor API.** Every command registered as a vendor request, plus vendor
   events on state change and a `status` request for polling. Companion can
   drive all of it immediately through its OBS module's *Custom Vendor Request*
   action, and so can any obs-websocket client — scripts, Stream Deck plugins,
   another automation system.
2. **A Companion module.** *Custom Vendor Request* is an action only: it cannot
   light a button red while an event is live, or show "12 s behind" on a
   display. Feedbacks, variables and presets need a purpose-built Companion
   module (Node.js, submitted to Bitfocus) subscribing to the vendor events.
   That is a separate deliverable in a separate repository, and it depends on
   phase 1 existing first.

**Available before any of this:** the plugins already register thirteen named
hotkeys, and Companion's OBS module can trigger hotkeys by id. Play, stop, hold,
resume, catch-up, jog and drop-marker are therefore controllable from a Stream
Deck today, without parameters or feedback. Worth wiring up before building
anything, both because it is free and because it will show which commands
operators actually reach for.

**Built: sub-phase 1, the vendor API.** Every command of both halves is
registered as a vendor request under `obs-multisite` — `encoder/go-live`,
`decoder/hold`, `decoder/jog` and the rest — with vendor events
(`encoder/state`, `decoder/state`) emitted on change and a `status` request for
polling. It is an adapter: each request calls the same function the docks, the
hotkeys and the pages already call, so there is no second code path to keep in
step. The names live in one list in the portable core (`src/core/control_api.h`)
that the HTTP routes read too, which is what makes "the same command names" true
by construction rather than by discipline, and which a core test pins. It
registers from `obs_module_post_load()` — the header's requirement, and the
reason a vendor registered in `obs_module_load()` is never seen. With
obs-websocket absent the plugin logs one line and everything else carries on.

**Built: sub-phase 2, the Companion module.** A Bitfocus Companion module lives
at [stageaudioworks/companion-module-obs-multisite](https://github.com/stageaudioworks/companion-module-obs-multisite).
It is an adapter in the same sense as sub-phase 1: every action is one vendor
request and every feedback is one field of the status document, so a Stream Deck
button and a keypress on the desk cannot disagree. It offers Go live, End and the
marker buttons for a main site, the transport controls and timeslipping for a
campus, feedbacks that light a button while an event is on air or a campus is
held, behind live or offline, variables for both halves, and two preset banks
with the feedbacks already attached. It also carries a pass-through action that
calls any vendor request by name, so a command this plugin adds later is
reachable without waiting for a module release.

**And it drives either end of the system.** The same module talks to a campus
player appliance as well as to OBS: the appliance serves the same controls on its
own HTTP API (§8.1) and has no OBS in it at all, so which end is a choice on the
connection while the actions, feedbacks, variables and presets are written once.
The two places the appliance names a command differently — `follow-live` for
`return-to-live`, `load` for `load-event` — are a route table in one file of the
module, which is what keeps that difference from ever reaching an operator.

Two things about it are worth recording, because both were forced rather than
chosen. It opens **its own obs-websocket connection**, so the operator types the
host, port and password a second time — Companion modules each own their
connection, and there is no way around it. And its **source is MIT** while the
module is *distributed* under `GPL-3.0-only`: the Companion module store
requires module source to be MIT, so the plugin's `-or-later` cannot be carried
across. The manifest states the distribution licence, which is what an installer
sees.

Since then both ends have been driven for real: the plugin against an OBS, and
the module against an OBS and a campus player. The module is tagged **v0.2.0**.
Two things are still open, and neither is code: nothing has yet run a whole
event, and the module is not in the Companion store — until it is, Companion
loads it as a *developer module* (a directory set in the launcher's Developer
section; each folder in it with a `companion/manifest.json` is one module), and
that needs **Companion 4.0 or later**.

**Two names still differ from the appliance's** — the plugin's
`decoder/return-to-live` and `decoder/load-event` against the appliance's
`/api/follow-live` and `/api/load`. The Companion module maps them, so nothing
an operator sees depends on the two being brought together any more: it is
tidiness now, not a gap. The plugin's names are already served to the v0.1.8
pages, so unifying them is a change to both surfaces at once rather than a
rename in one place.

