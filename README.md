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

**Built and bench-proven; has not yet carried a real congregation's event.**
That is the one sentence that matters. A six-hour soak ran end to end repeatedly
with zero upload failures and A/V in sync throughout; the pi appliance and the
relay have each run their own multi-hour tests. None of them was a Sunday
morning with people in the room. See
[the full current-state record](docs/scope/readme-current-state.md) for the
numbers.

**What works**

- Durable store-and-forward upload: nothing is lost through an outage, a crash,
  or a mid-event restart. Six-hour soak: 3,661 segments, >15 GB, zero retries,
  zero failures, 10 lagged frames of 658,837.
- CMAF segments from any OBS encoder — H.264 or HEVC via x264, NVENC, QuickSync
  or AMF.
- Satellite receive with a deep local buffer, checksum verification, and
  timeslipping — hold, resume, catch up, scrub, jump to a marker. Playback
  waits until a minute is buffered before starting, so the picture never chases
  the live edge.
- **Multi-track audio, up to 6 tracks**, in the same fragment as the picture,
  exposed at the satellite as separate sources.
- **AES67 audio on the appliance** — sound onto the network instead of HDMI,
  switched from the player's page. Eight channels on a bench Pi, and PTP proven
  into a receiving console over a 24-hour run. See
  [AES67 audio](docs/SATELLITE.md#aes67-audio-on-the-network).
- **One feed, several pictures** — a 2×1 or 2×2 composite arrives as discrete,
  already-cropped sources, downloaded once and decoded once. See
  [Choosing a satellite](docs/SATELLITE.md#what-running-the-decoder-in-obs-makes-possible).
- **Event browsing** — the decoder lists a room's events, marks which is on
  air, which finished and which was cut short, and plays any of them.
- Finished *and interrupted* events play as video-on-demand from the beginning.
- Operator docks in plain language, plus hotkeys.
- **Storage management from the encoder dock** — list, size, delete by event or
  by age, with confirmation and a verification pass; the live event is never
  offered.
- **A remote-control page on both sides** — the campus player's interface,
  served from the plugin or the appliance, polled twice a second. Trusted
  network only: no password, no TLS.
- **Public simulcast** — a container reads the same segments and pushes to
  YouTube, Facebook or any RTMP/SRT destination, minutes behind on purpose. See
  [Streaming to the public](docs/STREAMING.md).

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
events/{ulid}/markers.json      the main site's cues
events/{ulid}/cues/{site}.json  one object per site that has set a cue
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
<summary>Short version: never carried a real event; packed-channel routing is
deliberately out of scope; the relay cannot re-encode. Each line below is the
gap, not the story behind it — that is in
<a href="docs/scope/readme-current-state.md">the archive</a>.</summary>

- **Not yet used for a real event.** Six-hour soaks, a 44-minute unattended
  relay run — none was a room full of people on a Sunday. This is the gap that
  matters most.
- **Packed-channel routing is not our job.** [atkAudio's plugin
  suite](https://github.com/atkAudio/PluginForObsRelease) already routes packed
  multi-channel to separate outputs; a de-interleaver of ours was dropped as a
  worse version of something that exists. Multi-track needs none of it.
- **AES67 PTP into a receiving console is proven.** A console took the stream
  and ran 24 hours without a fault, which is the receiver-side read the entry was
  waiting for. The console's exact jitter figure is not recorded. A PTP master
  must exist on the network, or nothing flows. See [BUGS.md #1](BUGS.md).
- **AV1 reaches only destinations that document AV1 ingest.** YouTube does, and
  it is now measured — an AV1 event played there ten minutes without a fault.
  AV1 over SRT is refused, and always will be, until ffmpeg can put it in
  MPEG-TS. The campus tier is also unmeasured: no Pi decodes AV1 in hardware, so
  the docks label it experimental.
- **HEVC's last mile is unrehearsed.** Its remux is verified at the byte level
  and now reaches anywhere speaking Enhanced RTMP, but only H.264 and AV1 have
  carried real encoder output to a real destination.
- **Seeking is accurate to about a second**, not to a frame.
- **Separate audio tracks are unverified against each other.** Audio stays locked
  to the picture; that a click on one track lands at the same instant as another
  has not been measured. Test: identical audio on two tracks, both into one mix,
  listen for comb filtering.
- **The relay cannot split packed audio, re-encode, start itself, or terminate
  TLS** (a proxy does), and **SRT listener mode needs a port opened** — nothing
  is shipped to help. Replaying a past event is built but cannot be scheduled or
  looped.
- **Installing and updating the plugin is manual.** The plugin *tells* an
  operator a newer build exists; nothing installs it. Windows/macOS layout is
  now the OBS-recommended one. See [OPERATOR.md](docs/OPERATOR.md).

</details>

---

## Roadmap

<details>
<summary>Phases 6–15: what is left, one line each. Full design in
<a href="PROJECT-SCOPE.md#10-delivery-phases">PROJECT-SCOPE §10</a>; the longer
history of each is in <a href="docs/scope/readme-current-state.md">the archive</a>.</summary>

- **Phase 6 — Satellite appliance.** ✅ ARM64/Raspberry Pi HDMI tier, proven on
  a Pi 5 (not yet through an event). Pi 4 hardware-decoder selection remains.
  AES67 installed, eight channels on a bench Pi.
- **Phase 7 — Extensions.** Relay built, pushed to YouTube; SRT in and out. Left:
  re-encoding, signing in to YouTube instead of pasting a key, and starting
  itself.
- **Phase 8 — External control API.** ✅ Both halves — obs-websocket vendor
  requests and the
  [Companion module](https://github.com/stageaudioworks/companion-module-obs-multisite)
  (buttons with feedback, and it drives an appliance directly too). Driven
  against real OBS and a real campus player; no full event yet.
- **Phase 9 — Redundant storage.** Two independent S3 targets: mirrored
  throughout, or holding manifests only until a failover. **Not built.**
- **Phase 10 — Tile layout.** ✅ A 2×1/2×2 feed arrives as discrete, cropped
  sources; each region has its own **Send to screen** (OBS's own projector), and
  the campus player can show one region full-screen. Assigning tiles to several
  outputs needs hardware beyond the Pi — out of scope.
- **Phase 11 — Keeping installations current.** Plugin tells an operator a newer
  build exists (built); applying it is not, and is not one job — Windows cannot
  overwrite a loaded DLL and needs elevation, macOS quarantines an unsigned
  download, Flatpak goes through Flatpak. The OBS-recommended directory layout
  prerequisite is done. **Partially built.**
- **Phase 12 — Storage credentials and pairing.** Pair the plugin to a
  credential service with a short code, beside typed keys and never instead of
  them. Removes the three steps that decide whether a church can deploy unaided:
  create a cloud account, scope a token, write a lifecycle rule. Designed in
  [PROJECT-SCOPE §8.5](PROJECT-SCOPE.md#85-storage-credentials-direct-or-brokered-planned).
  **Not built** — the biggest user-facing lever left.
- **Phase 13 — Storage provider selection.** ✅ One dropdown (R2, AWS, B2,
  Wasabi, Custom) showing only the fields each needs; reaches both docks, the
  appliance and the relay. "Multisite Cloud" already sits in it greyed out,
  waiting for Phase 12.
- **Phase 14 — LAN / direct delivery.** ✅ A satellite on the same network or VPN
  downloads straight from the encoder, falling back to cloud per request; cloud
  can be turned off entirely. Proven over loopback sockets across all three
  receiver kinds. See
  [PROJECT-SCOPE §8.7](PROJECT-SCOPE.md#87-lan--direct-delivery).
- **Phase 15 — Lossless high-quality mode.** FLAC instead of AAC, ~10 Mbps HEVC,
  opt-in and whole-event. Researched, not built; a plugin-layer change. The
  catch is downstream: no public ingest takes FLAC, so only the OBS decoder and
  the appliance can ever play it back. Designed in
  [PROJECT-SCOPE §10](PROJECT-SCOPE.md#10-delivery-phases).

Phases 11, 12 and 13 are most of the distance between something a technician can
deploy and something an ordinary church can. Phase 14 answers a different
question — cost and reliability for a campus already on the same network.

**Three things that left this list, with why.** The **ABR transcoder** moved to a
separate hosted service (a rendition ladder serves the open internet, a different
question from carrying an event between a church's own sites); the existing relay
stays here, undiminished. **End-to-end low latency** is dropped outright — it
inverts the design priority and timeslipping cannot survive it; SRT is already in
OBS where conversational latency is genuinely needed. **Appliance hardware beyond
the Pi** (x86/DeckLink, RK3588) moved to Stage Audio Works' separate
**MultisiteOS** product line, for scope rather than rejection; code reusing this
core stays GPLv3 here. None was built; all three are written up in
[PROJECT-SCOPE §10](PROJECT-SCOPE.md#10-delivery-phases).

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
