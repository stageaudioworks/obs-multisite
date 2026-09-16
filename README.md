<h1 align="center">obs-multisite</h1>

<p align="center">
  <strong>Multisite church streaming, through storage you own.</strong><br>
  Send a live service from a main campus to any number of satellite campuses
  over ordinary venue internet, using nothing but an <strong>S3-compatible
  bucket you control</strong>. No subscription, no central server, no inbound
  ports at any site.
</p>

<p align="center">
  <a href="https://github.com/stageaudioworks/obs-multisite/releases"><img src="https://img.shields.io/github/v/release/stageaudioworks/obs-multisite?include_prereleases&label=release" alt="Latest release"></a>
  <img src="https://img.shields.io/badge/status-alpha-orange" alt="Status: alpha">
  <img src="https://img.shields.io/badge/licence-GPL--3.0--or--later-blue" alt="Licence: GPL-3.0-or-later">
  <img src="https://img.shields.io/badge/OBS%20Studio-32.2.2%2B-302e31" alt="OBS Studio 32.2.2 or newer">
  <img src="https://img.shields.io/badge/platforms-Windows%20%7C%20macOS%20%7C%20Linux-4cc2ff" alt="Windows, macOS and Linux">
</p>

<p align="center">
  <a href="https://github.com/stageaudioworks/obs-multisite/actions/workflows/obs-plugin.yml"><img src="https://github.com/stageaudioworks/obs-multisite/actions/workflows/obs-plugin.yml/badge.svg" alt="obs plugin build"></a>
  <a href="https://github.com/stageaudioworks/obs-multisite/actions/workflows/core-tests.yml"><img src="https://github.com/stageaudioworks/obs-multisite/actions/workflows/core-tests.yml/badge.svg" alt="core reliability tests"></a>
  <a href="https://github.com/stageaudioworks/obs-multisite/actions/workflows/analysis.yml"><img src="https://github.com/stageaudioworks/obs-multisite/actions/workflows/analysis.yml/badge.svg" alt="sanitizers and static analysis"></a>
  <a href="https://github.com/stageaudioworks/obs-multisite/actions/workflows/relay-container.yml"><img src="https://github.com/stageaudioworks/obs-multisite/actions/workflows/relay-container.yml/badge.svg" alt="relay container"></a>
  <a href="https://github.com/stageaudioworks/obs-multisite/actions/workflows/dco.yml"><img src="https://github.com/stageaudioworks/obs-multisite/actions/workflows/dco.yml/badge.svg" alt="DCO"></a>
</p>

> **⚠️ Alpha — development build.** This is pre-release software under active
> development. A six-hour continuous soak has been run end to end (see
> [Status](#status)), but it has not yet carried a real congregation's event.
> Interfaces, settings and the storage protocol may still change without a
> migration path, and there is no support contract, warranty or uptime
> guarantee of any kind.
>
> Production use comes with caveats. Run it only with a tested fallback in
> place, a technical person on hand, and the assumption that any given event
> may have to go ahead without it. Treat a successful rehearsal as necessary
> rather than sufficient.

---

## What it is

Two OBS Studio plugins in one module: an **encoder** at the main site that
publishes the programme as CMAF segments, and a **decoder** at each satellite
that receives, buffers deeply, and plays it out with per-campus timeslipping.
There is no central server, no database and no vendor — the bucket is a dumb
file store, and all the intelligence is at the edges.

```text
     MAIN CAMPUS                       YOUR BUCKET                       EACH CAMPUS
┌────────────────────┐            ┌────────────────────┐            ┌────────────────────┐
│ OBS + Multisite    │upload ────→│ S3-compatible      │            │ OBS + Multisite    │
│ Encoder            │            │ bucket you own     │            │ Decoder            │
│                    │  ←── poll  │                    │            │     or             │
│ capture -> encode  │            │ rooms/{room}/...   │            │ Raspberry Pi 5     │
│ -> CMAF -> queue   │            │ events/{ulid}/...  │            │ campus player      │
│ -> upload, retry   │            │ no server, no DB   │            │ deep buffer        │
└────────────────────┘            └────────────────────┘            └────────────────────┘
                             every campus polls the same objects
```

| Piece | Runs on | What it does |
|---|---|---|
| **Multisite Encoder** | OBS at the main site | Muxes OBS's own encoded frames into CMAF segments, writes each one to a durable local queue before it goes anywhere, then uploads with retry. A dropped link fills the queue rather than the gap. |
| **Multisite Decoder** | OBS at each campus | Finds what is live, downloads ahead of playout, verifies every segment, and exposes the feed as sources — the picture, plus each audio track separately, for local mixing and in-ears. |
| **Campus player** | A Raspberry Pi 5, no PC | The same receive core, headless: HDMI out, a browser control page, timeslipping, and AES67 onto the network if the room wants it. |
| **Simulcast relay** | Any Docker host | Reads the same files and pushes them to YouTube, Facebook or any RTMP/SRT destination. Optional, and never in the path between the sites. |
| **Companion module** | Bitfocus Companion | [Its own repository](https://github.com/stageaudioworks/companion-module-obs-multisite): buttons and feedback on a Stream Deck, driving OBS or an appliance. |

Design priority, in order: **reliability**, then quality, then simplicity, and
**latency last** — a satellite that is a minute behind but never drops is worth
far more than one that is two seconds behind and stutters.

---

## At a glance

- **Latency — minutes, on purpose.** A campus can hold the picture for its own
  welcome and then resume exactly where it paused, or stay a set number of
  minutes behind live all event. Latency is what this project spends to buy
  reliability, and it spends it willingly.
- **Bandwidth — about 2.7 GB an hour** of event at 6 Mbps. Every campus reads
  the same objects from the same bucket, so what it costs is your provider's
  egress policy — Cloudflare R2 charges none.
- **Audio — up to all 6 OBS tracks**: the programme mix, mics on their own
  tracks, a click for the band. They travel in the same fragment as the picture,
  so they cannot drift from it or from each other.
- **Video — whatever OBS already encodes**, H.264 or HEVC, through x264, NVENC,
  QuickSync or AMF.
- **Network — outbound HTTPS only.** No inbound ports, no port forwarding, no
  static IP, no VPN, and no firewall rules to negotiate with a building's IT.
- **Retention — yours to set**, in your provider's own console, and it doubles as
  your DVR depth: a campus can rewind as far back as it allows. Seven days is the
  design default, and nothing here deletes anything by itself.
- **Platforms — Windows, macOS (Apple Silicon) and Linux** at both ends, plus an
  ARM64 Raspberry Pi 5 tier for campuses that would rather not run a PC at all.

---

## Quick start

**A campus with no PC** — one command on a fresh Raspberry Pi 5 running
Raspberry Pi OS:

```sh
curl -fsSL --retry 5 https://raw.githubusercontent.com/stageaudioworks/obs-multisite/main/scripts/player/install.sh | sudo bash
```

**Both ends running OBS** — install the plugin from
[Releases](https://github.com/stageaudioworks/obs-multisite/releases), then:

1. **Set retention on the bucket first.** Nothing in this project deletes
   anything by itself: add a lifecycle rule for `events/` and another for
   `rooms/`.
2. **Main site.** Open the **Multisite Encoder** dock, enter the bucket details,
   pick a feed name, press **Go live**.
3. **Each campus.** Open the **Multisite Decoder** dock, enter the same bucket,
   add a **Multisite Source (Decoder)** for the same feed name, then **Load
   event** and **Play**.
4. **Lock the event** so nothing gets clicked by accident mid-service.

About twenty minutes, bucket included. The full walkthrough is
[QUICKSTART.md](QUICKSTART.md), and the readable introduction is the
[project website](https://stageaudioworks.github.io/obs-multisite/).

---

## Where to go next

This README is the technical overview: what this is, how far along it is, and
where it falls short. What works, what does not yet, and what is planned next is
in [Status](#status), [Known gaps](#known-gaps) and [Roadmap](#roadmap) below.

| You want to… | Start here |
|---|---|
| Read the readable introduction | [Project website](https://stageaudioworks.github.io/obs-multisite/) |
| Read the full manual online | [Manual](https://stageaudioworks.github.io/obs-multisite/docs.html) |
| Get broadcasting in about twenty minutes | [QUICKSTART.md](QUICKSTART.md) |
| See what changed in the latest build | [Releases](https://github.com/stageaudioworks/obs-multisite/releases) |
| Install, configure and operate in depth | [Operator guide](docs/OPERATOR.md) |
| Choose between a PC and the Pi box | [Choosing a satellite](docs/SATELLITE.md) |
| Put the appliance's sound on the network (AES67) | [AES67 audio](docs/SATELLITE.md#aes67-audio-on-the-network) |
| Send the event to YouTube or Facebook | [Streaming to the public](docs/STREAMING.md) |
| Control it from a Stream Deck | [Companion module](https://github.com/stageaudioworks/companion-module-obs-multisite) |
| Build and test | [Developer guide](docs/DEVELOPER.md) |
| Send a change, or report a bug well | [CONTRIBUTING.md](CONTRIBUTING.md) |
| Read the design and storage protocol | [PROJECT-SCOPE.md](PROJECT-SCOPE.md) |

---

## Why this exists

<details>
<summary>The longer story — who this is for, what it deliberately is not, what
it asks of your network, and the licensing position.</summary>

This project is developed by the projects team at **Stage Audio Works**, a
worship AVL integrator working across Africa, to support churches that are
growing into multiple locations.

Multisite streaming is a solved problem if you are a large church in a
well-connected part of the world. The commercial platforms that solve it are
good, and the teams behind them have earned their place. But they are largely
unavailable outside the developed world, and where they are available the
recurring cost is out of reach for a congregation whose entire annual AV budget
is smaller than a year of subscription.

### What this is not

**It is not a managed event.** The commercial products are, and that is worth
paying for. Someone answers the phone. Someone watches the infrastructure.
Someone ships you a decoder that boots and works. If your church can afford one
and it is available where you are, you should probably buy it.

This is a set of tools instead. Setting it up requires a reasonably technical
person, or support from an integrator with the relevant expertise. There is no
support contract, no uptime guarantee, and no one to call. What there is
instead: you own your storage, you own your content, your ongoing cost is a few
dollars a month of object storage, and nothing can be taken away from you or
priced beyond your reach later.

**It is not low latency, and it is not two-way.** This carries an event from
one site to others with a delay measured in tens of seconds. It cannot support a
live conversation between campuses, a two-way interview, or anything else where
people need to respond to each other in real time. For that, use SRT or WebRTC:
both are in OBS already, and there are many good hardware products built on
them. Those approaches trade differently, sitting much closer to the raw
condition of the connection at the moment you need it. (The relay can *send*
SRT — see [Streaming to the public](docs/STREAMING.md) — but it sends
from the bucket, minutes behind, so it inherits this project's trade rather
than SRT's own.)

This project takes the opposite trade deliberately. Content is written to disk
before it is sent, sent again until the storage confirms it, and buffered deeply
at the far end before it is played. Minutes of the event can be held at the
satellite in advance, so an outage part-way through is something the
congregation never sees. Latency is the price, and for an event being relayed
rather than a conversation being held, it is a price worth paying.

### What it asks of your network

Very little, and this is deliberate. Everything moves over ordinary HTTPS to
object storage. There are no inbound connections, no port forwarding, no static
IP, no VPN, and no firewall rules to negotiate with a building's IT.

That means it works on connections that would defeat a direct stream: mobile
data, LEO satellite, consumer fibre, and networks behind carrier-grade NAT. If a
laptop at the site can load a web page, it can usually send or receive a
event.

### On intellectual property

This is a clean-room implementation built on published, open standards: CMAF
fragmented MP4, the S3 object API, and OBS Studio's public plugin interface. It
is not derived from, and does not reverse-engineer, any commercial product.

Where our design resembles existing products, it is because we are solving the
same problem under the same constraints and arriving at similar answers, or
because we have deliberately followed conventions that operators already
understand. Familiarity is a feature in a room where a volunteer is running the
event.

It is released under the **GPLv3** in support of kingdom expansion and the
enabling of local churches: free for any church to run, adapt and keep running
— and if you distribute a changed version, those changes have to reach the next
church too. That is the whole point of the choice. There is no intent to tread
on anyone's intellectual property. If you believe something here does, please
raise it with us and we will address it properly.

### Contributing

If this is useful to your church, use it. If you improve it, we would be glad to
see the change come back. If it fails you in an interesting way, a good bug
report is a real contribution: much of what works well here was fixed because
someone took the time to paste a log.

Commits need a `Signed-off-by` line — `git commit -s` — certifying that the
change is yours to give. That is the [DCO](DCO), and it is the only thing asked
for beyond the GPL. **There is deliberately no Contributor Licence Agreement**:
a CLA would give us the right to relicense your work into a closed product, we
have no plan to use that right, and asking for it would be asking for something
in exchange for nothing. What you contribute stays GPLv3, for the next church
as much as for this one. See [CONTRIBUTING.md](CONTRIBUTING.md).

</details>

---

## Status

Phases 1–5 are built and running against real Cloudflare R2: capture, upload,
the storage protocol, receive, timeslipping, markers, and the operator UI. It
has been run between two Windows machines through a **six-hour continuous soak
test**: 3,661 segments, over 15 GB uploaded, **zero retries and zero upload
failures**, and 10 lagged frames out of 658,837 (0.0%). Six audio tracks were
carried throughout, timeslipping held a campus a steady two and then three
minutes behind live for hours, a scrub back nearly three hours and a return to
live both recovered cleanly, and the satellite played out to the end of the
recording when the broadcast stopped.

Audio and video stayed in sync across the whole run, checked by eye and ear as
well as by the reported A/V offset, which held between 0.005 s and 0.021 s
through several decoder restarts. That run is not a one-off: it has been
repeated several times since, at the same length, with the same shape of
result each time — the project holds up under sustained real-world load
rather than having gotten lucky once.

The Raspberry Pi appliance's AES67 output has separately been run through its
own multi-hour test, picture and sound watched and listened to together the
whole way through: no drift, checked by eye and ear rather than measured, and
none found. Worth being precise about why: audio and video are scheduled off
the exact same delivery-queue clock and the same first-frame anchor
(`Player::anchor_pts()`, `src/appliance/player.cpp`), so the two cannot drift
apart from each other regardless of what the output device is — that is what
this test confirms held over hours, not that playback is somehow slaved to
the AES67 card's own PTP-disciplined sample clock. (That clock is real, and
matters for a receiving console's own sync to the network — but it is the
RAVENNA driver's doing, external to this project entirely.) Still
unmeasured: PTP lock accuracy at a receiving console over that same stretch.

It has still **not carried a real congregation's event** — a soak test on
looping media is not a Sunday morning with people in the room.

A campus can receive in either of two ways — the OBS decoder on a PC, or the
Raspberry Pi appliance — and both are built. See
[Choosing a satellite](docs/SATELLITE.md). The appliance has not run a
event either.

The public simulcast relay is built and is the first piece of Phase 7. It has
pushed live streams to YouTube, sends over SRT as well as RTMP, and survives
having its encoder killed mid-stream — but it has not yet been through a full
event. HEVC can now go out over SRT; that path is verified against ffmpeg
but has not yet carried real encoder output.

**What works**

- Durable store-and-forward upload: nothing is lost through an outage, a crash,
  or a mid-event restart.
- CMAF segments from any OBS encoder — H.264 or HEVC via x264, NVENC, QuickSync
  or AMF.
- Satellite receive with a deep local buffer, checksum verification, and
  timeslipping — hold, resume, catch up, scrub, jump to a marker. Playback
  waits until a minute of the event is buffered before starting, so the
  picture never chases the live edge.
- **Multi-track audio, up to 6 tracks.** Whatever the room puts on them — a
  programme mix, mics on their own tracks, a click for the band — travels in the
  same fragment and is exposed at the satellite as separate sources, sharing one
  download and one playout clock.
- **AES67 audio on the appliance.** A campus can put the player's sound onto the
  network as an AES67 stream instead of leaving it inside the HDMI picture, so
  its own console can take the feed whether or not a screen is attached. Built
  on Merging's open RAVENNA kernel module and the GPL `aes67-daemon`, installed
  by one script, and switched from the player's own page — on or off, the
  multicast address, and the channel count, with what is actually being sent and
  whether the clock is locked shown next to it. It passes eight channels on a
  bench Pi, but has not yet been through an event. See
  [AES67 audio](docs/SATELLITE.md#aes67-audio-on-the-network).
- **One feed, several pictures.** A room that composites two or four cameras
  into a single feed says so at the main site, and each region arrives at a
  satellite as its own already-cropped source — ready to drop into a scene or
  send to a screen of its own, downloaded once and decoded once however many
  pictures are taken out of it. A campus player can put one chosen region on its
  single display, and its page offers both the region going out and the whole
  feed. See
  [Choosing a satellite](docs/SATELLITE.md#what-running-the-decoder-in-obs-makes-possible).
- **Event browsing.** The decoder lists what a room has recorded, shows which is
  on air, which are finished recordings and which were cut short by an encoder
  that died, and plays any of them back.
- Finished *and interrupted* events play as video-on-demand from the beginning —
  an event whose encoder crashed is still watchable afterwards.
- Operator docks in plain language, plus hotkeys.
- **Storage management from the encoder dock.** *Manage storage…* lists the
  room's events with their sizes, and lets one be deleted — or everything older
  than a chosen number of days — with a confirmation and a verification pass
  afterwards. The events appear at once and the sizes fill in beside them, a
  size that cannot be measured is reported as unknown rather than as zero, and
  the event on air is never offered for deletion. Closing the window stops the
  work.
- **A remote-control page on both sides.** The encoder and decoder docks each
  serve the campus player's own operator interface on the church network — one
  page, polled twice a second, in the plain language of an event — so a marker
  can be pressed from the back of the room and the buffer depth checked from a
  phone. No password and no TLS: the building's network is the guard, exactly as
  it is for the appliance. What exists follows the machine's role.
- **Public simulcast.** A separate container reads the same segments and pushes
  them to YouTube, Facebook or any RTMP destination — or over SRT, to a
  broadcast partner, a hardware decoder or a contribution CDN — a few minutes
  behind on purpose. See [Streaming to the public](docs/STREAMING.md).

**What does not, yet** — see [Known gaps](#known-gaps).

---

## How it works

The encoder muxes OBS's encoded frames into CMAF fragments and writes them to a
bucket. The decoder polls two small JSON files to discover what is live, then
downloads segments ahead of playback into a local cache.

```
rooms/{room_id}/live.json        which event is live in this room
rooms/{room_id}/events/{ulid}.json   one entry per event, so a room's
                                 recordings list in a single request
events/{ulid}/event.json         codec config, audio layout, start time
events/{ulid}/init.mp4           codec configuration for the event
events/{ulid}/segments/…m4s      the media
events/{ulid}/manifest.json      rolling window of confirmed segments
events/{ulid}/markers.json       cues dropped by the main site
```

**The invariant that makes it reliable:** a segment is listed in the manifest
only *after* the bucket has confirmed it stored. If a decoder can see an entry,
the object exists. Everything else — retries, crash resume, deep buffering —
builds on that.

Every one of those JSON documents carries a `protocol_version`. A decoder goes
on reading anything it once wrote — a recording from last year is exactly what
someone wants to play back — and refuses a bucket from a protocol it does not
know, saying so, rather than half-reading it and stuttering. A document without
the field is version 1, so nothing already in a bucket has to change. See
[PROJECT-SCOPE.md §4.8](PROJECT-SCOPE.md#48-protocol-version).

For the full design, see [PROJECT-SCOPE.md](PROJECT-SCOPE.md).

---

## Repository layout

```
src/core/       the portable engine: protocol, reliability, muxing, decoding.
                No OBS, no Qt. Shared with the appliance.
src/obs/        the OBS bindings: output, source, hotkeys, settings, and the
                wiring that serves the control pages from OBS itself.
src/obs/ui/     the Qt docks. The only place Qt appears.
src/obs/web/    the control pages a phone or tablet uses, and the JSON API
                behind them — HTTP, not Qt, because the machine that most needs
                them is the one nobody is sitting at.
src/obs/websocket/  the same commands again as obs-websocket vendor requests, so
                a Stream Deck or any automation can drive either half. Reads its
                names from src/core/control_api.h, which the pages read too, so
                the two cannot drift.
src/vendor/     third-party headers kept in-tree: nlohmann/json, and
                obs-websocket's header-only vendor API. No shipped dependency.
src/appliance/  the headless campus player: DRM/KMS and ALSA output, the
                splash and idle screens, and the web control surface.
src/appliance/web/  the operator interface. No framework, no CDN — a campus
                box often has no internet.
data/           the locale strings, and the operator pages the plugin serves
                out of its own process (data/web/encoder, data/web/decoder).
relay/          the public simulcast relay: a container that pushes the same
                segments to YouTube, Facebook or any RTMP destination, or over
                SRT. Uses the core; the core knows nothing about it.
tests/          every guarantee above has a test.
cmake/          the driver scripts the muxer round-trip tests run through.
test-data/      fixtures, and a note on how the CMAF ones are produced.
scripts/player/ the install scripts and systemd units for the appliance,
                including AES67 audio onto the network.
scripts/        optional Lua control script, superseded by the encoder dock.
```

**`src/core/` must stay free of OBS and Qt.** It is the shared engine behind
both the plugin and the headless appliance, and a test enforces this on every
build rather than trusting the convention. The appliance is what proves the
rule holds: it is a new output and control layer over the same receive core,
not a second implementation.

The relay is the same rule again, one step further out: it is a separate
sub-project that depends on the core and is never depended on by it. It builds
only when asked (`-DMULTISITE_BUILD_RELAY=ON`), so a plugin build is not made
to find SQLite for something it does not use.

---

## Known gaps

<details>
<summary>What does not work yet, and what has not yet been proven. The short
version: never carried a real event, packed-channel routing is deliberately
out of scope, and the relay cannot re-encode.</summary>

- **Routing packed channels to separate outputs is not our job.** A packed
  feed arrives as one multi-channel stream, and in OBS
  [atkAudio's plugin suite](https://github.com/atkAudio/PluginForObsRelease)
  already does the routing — to ASIO, CoreAudio or Windows Audio devices, with
  VST3/AU hosting alongside. A de-interleaver of our own was planned and has
  been dropped: it would have been a worse version of something that exists.
  Multi-track audio needs none of it, since each track is already its own
  source. On the appliance the packed channels go out of HDMI in order, which
  is what an eight-channel de-embedder expects.
- **AES67 audio on the appliance is installed but unproven over an event.**
  Eight channels were received on a bench Pi, so the card, the daemon and the
  player's plumbing do work together, and the stream is created by the install
  and switched from **Settings → Network audio output**. What is not measured is
  lip sync across a two-hour service and the PTP accuracy a Pi's network
  interface can reach without hardware timestamping, and no event has been
  through it. A PTP master must also exist on the network or nothing flows — the
  daemon slaves to a clock, it does not hand one out.
  See [BUGS.md entry 3](BUGS.md).
- **AV1 is carried but lightly exercised**, unlike H.264 and HEVC.
- **Seeking is accurate to about a second**, not to a frame.
- **The relay sends HEVC to a streaming site only if that site speaks Enhanced
  RTMP.** YouTube documents H.264, H.265 and AV1 for RTMP/RTMPS ingest, so an
  HEVC site reaches it unchanged; a destination that has never implemented the
  extension drops the stream the moment it starts, and re-encoding on the way
  out is not built. SRT carries HEVC regardless, so where the RTMP half fails
  there is still somewhere to send it. H.264 remains the default: it decodes
  fine everywhere, Raspberry Pi campuses included, and it needs nothing of the
  far end.
- **No HEVC event has yet gone out from a real encoder to a real
  destination.** The remux is verified at the byte level — over MPEG-TS it
  reads back as HEVC, and over Enhanced RTMP the tag comes out with the
  extended header set and a FourCC of `hvc1` — but the last mile has not been
  rehearsed. Do that before relying on it.
- **SRT in listener mode needs a port opened**, and nothing is shipped to help.
  Publish it on the container and open it on the firewall yourself; unlike the
  web interface there is no proxy in front of it.
- **The relay cannot split packed multi-channel audio**, and cannot start
  itself on a schedule or when the encoder goes live.
- **Replaying a past event is a proof of concept.** One at a time, started by
  hand, and it cannot be scheduled, looped, or started part-way in.
- **The relay speaks plain HTTP** and relies on something in front of it for
  TLS. It binds to localhost so that is a deliberate choice rather than an
  accident, but it does not terminate TLS itself.
- **Alignment between separate audio tracks is unverified.** The soak confirmed
  audio stays locked to the *picture*, but not that a click on one track lands
  at the same instant as the programme on another. Each track is emitted by its
  own OBS source, and OBS buffers sources independently — the timestamps are
  derived from one anchor by construction, but nobody has measured the result.
  A few milliseconds would be inaudible against video and useless to a band.
  To test it: send identical audio on two tracks, play one through the main
  source and one through a companion into the same mix, and listen for comb
  filtering.
- **Installing the plugin is manual, and so is updating it.** Unzip, move files,
  restart OBS, and on macOS clear the quarantine flag (see
  [OPERATOR.md](docs/OPERATOR.md)). Nothing tells an operator that a newer build
  exists, and there is no OBS-level mechanism to add one — no update or upgrade
  entry point exists in `libobs` or `obs-frontend-api`. This is Phase 11 in the
  [Roadmap](#roadmap). Its prerequisite is done: the Windows build and the
  install instructions now use the layout OBS recommends
  (`C:\ProgramData\obs-studio\plugins\obs-multisite\`) rather than the one it
  has said will stop working.
- **Not yet used for a real event.** A six-hour soak has been run (see
  [Status](#status)) but no congregation has watched anything through this. The
  soak covered sustained upload, timeslipping and playout; it did not cover a
  room full of people, a volunteer under pressure, or a venue's actual network
  on a Sunday. The relay has run 44 minutes unattended without a fault, which
  is encouraging and is not an event.

</details>

---

## Roadmap

<details>
<summary>What is planned next: phases 6 to 15, from storage redundancy to
keeping installations current, an easier way to connect a bucket, choosing a
storage provider from a list, a satellite receiving directly from the encoder
on the same network, and a lossless high-quality mode for players only — plus
three things that are no longer part of this project, and why.</summary>

- **Phase 6 — Satellite appliance.** Built for the ARM64 / Raspberry Pi HDMI
  tier and proven on a Pi 5, though not yet through an event — that tier is done.
  Hardware-decoder selection on Pi 4 remains within this same phase. AES67
  audio, which puts the sound onto the network rather than leaving it in the
  picture, is installed and passing eight channels on a bench Pi.
- **Phase 7 — Extensions.** The public simulcast relay is built and has pushed
  live streams to YouTube; SRT in and out is in. Still to come: re-encoding,
  signing in to YouTube instead of pasting a stream key, and starting by itself.
- **Phase 8 — External control API.** Both halves are built. Every command is an
  obs-websocket vendor request (`obs-multisite.decoder/hold` and the rest), with
  vendor events on state change and a `status` request for polling; and
  [companion-module-obs-multisite](https://github.com/stageaudioworks/companion-module-obs-multisite)
  puts them on a Stream Deck, with buttons that light up — on air, held, behind
  live, link offline. That module also drives a **campus player appliance**
  directly, over the appliance's own HTTP API, so a satellite needs no OBS at
  all. The thirteen hotkeys the plugins already register can be triggered from
  Companion as well, without parameters or feedback. All of it has been driven
  against a real OBS and a real campus player; nothing has yet run a full event.
- **Phase 9 — Redundant storage.** Two independent S3 targets: mirrored
  throughout, or holding the manifests only until a failover.
- **Phase 10 — Tile layout.** ✅ Built. A 2×1 or 2×2 feed declared at the main
  site arrives as discrete, already-cropped sources: the layout and crop
  geometry are in the core, the OBS plugin exposes each region as its own
  source with its own **Send to screen** — which drives OBS's own fullscreen
  projector, so a region reaches a second monitor or a DeckLink without this
  code knowing what either of those is — and the campus player can show one
  chosen region full-screen on its single display, with the web preview
  offering both what is going out and the whole feed. Assigning tiles to
  *several* outputs from one box needs hardware beyond the Pi, which is out of
  this project's scope — see below.
- **Phase 11 — Keeping installations current.** Today the plugin is a set of
  files an operator replaces by hand, and the only way anyone learns a newer
  build exists is to go and look. Notifying them is the small half: the plugin
  already speaks HTTPS through the libcurl it links for uploads — and already
  bundles on Windows — so it can ask what the latest release is and say so in the
  dock an operator already has open. Applying an update by itself is a different
  size of job, and it is not one job: Windows cannot overwrite a DLL that OBS has
  loaded and needs elevation to write where it lives, macOS makes the swap easy
  but quarantines an unsigned download so that OBS then loads nothing and says
  nothing, and a Flatpak install has to go through Flatpak. The prerequisite for
  any of it is packaging into the directory layout OBS now recommends rather than
  the one it has said will stop working.
- **Phase 12 — Storage credentials and pairing.** A second way to answer "which
  bucket, and with what keys": pair the plugin to a credential service with a
  short code, the way a television signs in, beside the typed keys that exist
  now and never instead of them. Creating a cloud account, scoping a token
  correctly and writing a lifecycle rule are the three steps that decide whether
  a church can deploy this unaided, and they are the three this removes. The
  service address is a setting, not a constant — anyone can run their own — and
  nothing contacts anything until an operator asks it to. Designed in
  [PROJECT-SCOPE.md §8.5](PROJECT-SCOPE.md#85-storage-credentials-direct-or-brokered-planned).
- **Phase 13 — Storage provider selection.** ✅ A dropdown — Cloudflare R2,
  AWS S3, Backblaze B2, Wasabi, or a custom endpoint — that shows only the
  fields each provider actually needs and works out the rest, instead of six
  blank fields and a hostname convention nobody outside this project has
  memorized. The underlying settings didn't change; this sits in front of
  them, and a setup saved before this existed reads back as whichever
  provider its endpoint actually matches, or "Custom" if none does. Built for
  both OBS docks first; the Raspberry Pi appliance's and the simulcast
  relay's web settings pages got the identical dropdown in later passes,
  reading the same underlying table. It's also where Phase 12's paired
  credentials will eventually show up —
  "Multisite Cloud" is already a greyed-out entry in the same list, not a
  second settings screen waiting to be built. Built per
  [PROJECT-SCOPE.md §8.6](PROJECT-SCOPE.md#86-storage-provider-selection).
- **Phase 14 — LAN / direct delivery.** ✅ A satellite on the same network as
  the main site, or reachable over a VPN the church already runs, downloads
  straight from the encoder instead of the bucket — automatically preferred
  when it answers, falling back to cloud per request the instant it doesn't.
  Cloud upload stays on by default (every other satellite and the archival
  recording still depend on it), but can now be turned off entirely for an
  operator who wants everything to stay on one network. Both sides are
  built and tested over real loopback sockets — the encoder's object
  server, the decoder's matching transport and its LAN-preferred/cloud-
  fallback logic — and every kind of receiver: the OBS decoder plugin, the
  Raspberry Pi appliance and the simulcast relay all share the same core
  classes and settings shape. (The relay's own past-events browsing,
  download and rebroadcast stay cloud-only regardless — they list the
  bucket, which the encoder's LAN server was never built to serve.)
  Designed and built in
  [PROJECT-SCOPE.md §8.7](PROJECT-SCOPE.md#87-lan--direct-delivery).
- **Phase 15 — Lossless high-quality mode.** FLAC instead of AAC on every
  audio track, alongside roughly 10 Mbps HEVC video — an opt-in, whole-event
  mode for when bandwidth genuinely isn't the constraint, not a per-track
  option. Researched, not yet built: OBS already ships a FLAC encoder
  through the same machinery as its AAC one, and the mux/decode core here is
  already codec-agnostic, so this is a plugin-layer change. The Pi appliance
  needs nothing at all — it decodes through the same core path regardless of
  codec. The catch is downstream rather than technical, and it is the sound
  rather than the picture: FLAC has no home on the public web — no ingest takes
  it — so this mode is a dead end for the simulcast relay and anything reached
  through it, even though the video half of that argument has since gone (HEVC
  now travels over both protocols). Only the OBS decoder plugin and the Pi
  appliance can
  ever play it back. Designed in
  [PROJECT-SCOPE.md §10, Phase 15](PROJECT-SCOPE.md#10-delivery-phases).

Phases 11, 12 and 13 are, between them, most of the distance between
something a technician can deploy and something an ordinary church can —
13 is done, was deliberately the smaller half of what 12 needs anyway, and
"Multisite Cloud" is already sitting in its dropdown greyed out, waiting
for 12 to make it real. Phase 14 answers a different question, cost and
reliability for a campus already on the same network, and depends on none
of the others. None of the four depends on phases 9 or 10.

Three things that used to be on this list are not any more. **The ABR
transcoder** is no longer part of this project: a rendition ladder exists to
serve an audience on the open internet, which is a different question from
carrying an event between sites a church runs, and it has moved to a hosted
service Stage Audio Works intends to build separately. The existing relay
stays here, free and undiminished. **End-to-end low latency** is dropped
outright — it inverted the design priority this project is built on,
timeslipping could not survive it, and it would have generated support calls
on exactly the connections this project exists to tolerate; where a site
genuinely needs conversational latency, SRT is already in OBS. **Appliance
hardware beyond the Pi** — an x86/DeckLink production tier, a headless
RK3588 encoder — has moved too, but for a different reason: not a rejected
idea, a scope decision. This project's hardware is the OBS plugin pair and
the Raspberry Pi appliance, nothing wider. Stage Audio Works builds and sells
further hardware, and the software that runs on it (**MultisiteOS**), as a
separate product line; where it reuses this project's core, that code stays
exactly what it already is — GPLv3, in this repository. None of the three
was built. All three are written up with the reasoning in
[PROJECT-SCOPE.md §10](PROJECT-SCOPE.md#10-delivery-phases).

Each phase is described in full in
[PROJECT-SCOPE.md §10](PROJECT-SCOPE.md#10-delivery-phases), which is also where
the design questions each one leaves open are written down.

</details>

---

## License

**GPL-3.0-or-later** — see [LICENSE](LICENSE), and [COPYRIGHT](COPYRIGHT) for
the notice and the third-party components. Copyright (C) 2026 Stage Audio
Works.

What it means in practice: run it, adapt it, install it for as many churches as
you like. If you distribute a modified version — as a binary or as source —
those modifications are GPLv3 too, and recipients get the source. It places no
condition on the events you broadcast with it, or on anything in your bucket.

Releases up to and including **v0.1.4-alpha were MIT**, and that grant cannot
be withdrawn: anyone who has those versions keeps their MIT rights to them.

This is compatible with OBS, which is **GPL-2.0-or-later** — the "or later" is
what makes a GPLv3 plugin lawful in a GPLv2 host. Vendored `nlohmann/json`
stays MIT, which is GPL-compatible and not ours to relicense.

---

<p align="center">
  <a href="https://www.stageaudioworks.com">
    <picture>
      <source media="(prefers-color-scheme: dark)" srcset="site/assets/saw-logo-white.svg">
      <img alt="Stage Audio Works" src="site/assets/saw-logo-black.svg" width="190">
    </picture>
  </a><br>
  <sub>Built by the projects team at
  <a href="https://www.stageaudioworks.com">Stage Audio Works</a>, a worship AVL
  integrator working across Africa.</sub>
</p>
