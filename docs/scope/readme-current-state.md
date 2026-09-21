# README current-state detail (archive)

The Status, Known gaps and Roadmap prose that used to be most of `README.md`.
The README now states each fact in one line and links here. Kept verbatim.

> As it stood on 2026-09-21.

---

## Status

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
event. HEVC can go out over SRT; that path is verified against ffmpeg but has not
yet carried real encoder output. AV1 *has* now carried real encoder output: an
AV1 event went through the relay to YouTube and played there for over ten
minutes, which is the first of the three to make that last mile.

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

## Known gaps

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
- **AES67 audio on the appliance has run a multi-hour test; the PTP accuracy a
  receiver sees is still unmeasured.** Eight channels were received on a bench
  Pi, and a multi-hour run has since carried picture and sound together with no
  drift found — so the card, the daemon and the player's plumbing work together,
  and lip sync is no longer the open question. What is still open is how tightly
  a Pi's network interface holds PTP without hardware timestamping, measured at a
  receiving console over a service. The Pi's own half of that comparison is now
  logged once a minute and shown on its page. A PTP master must also exist on the
  network or nothing flows: the daemon slaves to a clock, it does not hand one
  out. See [BUGS.md entry 1](BUGS.md).
- **AV1 is carried end to end, round-tripped by a test, and has now carried a
  real event** — to YouTube, through the relay. The campus tier is the part still
  unmeasured: no Pi decodes AV1 in hardware, so an appliance would be
  software-decoding it, which is exactly the tier this project exists to keep
  cheap. The encoder docks still label AV1 encoders experimental for that
  reason.
- **Seeking is accurate to about a second**, not to a frame.
- **AV1 reaches a streaming site only where that site documents AV1 ingest.**
  **YouTube does, and that is now measured rather than read:** an AV1 event went
  through the relay and played there for over ten minutes without a fault. It
  settles YouTube and nothing else, so the relay still warns above every
  destination before you start, and reports it honestly if the far end drops the
  stream. AV1 over SRT is refused and always will be until ffmpeg can put it in
  MPEG-TS, which it cannot today. HEVC, which used to carry the same caveat, now
  reaches anywhere that speaks Enhanced RTMP, unchanged — but its own last mile
  is still unrehearsed.
- **AV1 aside, no codec has carried real encoder output to a real
  destination through the relay.** AV1 now has — YouTube, ten minutes, no fault
  — and HEVC has not, even though its remux is verified at the byte level in the
  same way (over MPEG-TS it reads back as HEVC; over Enhanced RTMP the tag comes
  out with the extended header set and a FourCC of `hvc1`). The two share a code
  path, so the AV1 result is encouraging for HEVC rather than proof of it:
  rehearse before relying on it. H.264, the default, is the codec that has
  carried events to YouTube for real.
- **SRT in listener mode needs a port opened**, and nothing is shipped to help.
  Publish it on the container and open it on the firewall yourself; unlike the
  web interface there is no proxy in front of it.
- **The relay cannot split packed multi-channel audio**, and cannot start
  itself on a schedule or when the encoder goes live.
- **Replaying a past event is built but cannot run itself.** One at a time,
  started by hand; two of the event's cues can be chosen as in and out points, so
  a repeat can be an excerpt, but it cannot be scheduled or looped.
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
  [OPERATOR.md](docs/OPERATOR.md)). The plugin now *tells* an operator a newer
  build exists — one check per start, switchable off — but nothing installs it:
  `libobs` and `obs-frontend-api` offer no update or upgrade entry point, which
  is why Phase 11 starts with a notification and leaves applying it undecided.
  One prerequisite is done, at least: the Windows build and the install
  instructions now use the layout OBS recommends
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
