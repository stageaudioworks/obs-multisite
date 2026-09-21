# Delivery phases and brokered credentials — design rationale (archive)

The reasoning, measurements, decisions and history behind `PROJECT-SCOPE.md`'s
specification for the delivery phases (§10) and brokered storage credentials
(§8.5). Both sections' short, specification-only form lives in
PROJECT-SCOPE.md; this is the detail moved out of the reader's way, kept
verbatim. Read this before changing the phase plan or the pairing design.

> As it stood on 2026-09-21.

---

## 8.5 Storage credentials: direct or brokered (planned)

Setting this up asks a volunteer to create a cloud account, mint an API token
with exactly the right scope — `s3:ListBucket` included, which the obvious
object-scoped token omits — and write a lifecycle rule that is also, without
saying so, the DVR depth. Those three steps are the wall. Everything else in
QUICKSTART is copying files.

So there should be a second way to answer "which bucket, and with what keys",
without removing the first.

**Two providers, one `S3Config`.** Today one thing builds the `S3Config` the
transport takes: the fields an operator typed. A second producer is added
beside it, and nothing downstream learns which one it got.

- **Direct** — endpoint, bucket, key, secret, region. What exists now, unchanged
  and never deprecated. Somebody running MinIO in their own rack is a first-class
  user of this project, not a legacy case.
- **Brokered** — the plugin holds a device identity and a service URL, and
  fetches short-lived credentials from a broker that manages the bucket on the
  operator's behalf.

**Pairing is a device-code flow**, the one a television uses to sign into a
video service — not a password typed into OBS, and not a key pasted from an
email:

```
1. First run, offline. The plugin mints a device id locally. No network.
2. The operator presses "Connect to a storage service".
3. The plugin asks the broker for a code and shows it:   JNB-4K7M
4. The operator opens the broker's page on any device and enters the code.
5. The plugin polls, receives credentials, and caches them.
```

The same flow serves the appliance, which is the reason to prefer it over
anything bespoke: a headless player with no keyboard shows the code on its own
screen and is paired from a phone, using the code path the plugin already has.

**What holds this honest.** A brokered mode is a place where a plugin could
quietly start working for somebody other than the person running it, so the
constraints matter more than the mechanism and belong here rather than in a
commit message:

- **Inert until asked.** Nothing contacts anything but the configured bucket
  until an operator presses Connect. No registration on first run, no version
  check riding along, no telemetry. A fresh install that is never paired must
  produce no traffic a packet capture would surprise anyone with.
- **The broker URL is a field, not a constant.** It may ship with a default,
  and it must be editable. Anyone can run a broker — an integrator looking
  after a dozen churches has better reason to than most — and the protocol
  between plugin and broker is documented here for that reason.
- **Cached credentials are never a precondition.** If the broker cannot be
  reached, the last good credentials are used and the event goes live. A
  service that can stop a Sunday is not one this project will depend on.
- **Always visible.** Mode, bucket, endpoint and expiry are shown in the dock.
  Brokered must not come to mean opaque.
- **Disconnect is a real button**, and it offers the underlying bucket details
  on the way out. The promise that nothing here can be taken away is worth
  little if leaving is undocumented, and the code path that proves it should
  exist whether or not it is ever used.

One side effect worth stating, because it runs against the intuition that
managed means less safe: a brokered credential is short-lived, where today a
long-lived key sits in the settings in plain text. The managed path is the more
defensible of the two at rest, not the less.

**Not settled.** What a broker owes a plugin when a subscription lapses — the
answer must not be "the event stops" — and whether a decoder pairs
independently or inherits from the encoder that already knows the room. Both are
design questions, not details, and neither should be answered by the first
implementation that happens to work.

---

## 10. Delivery phases

Each phase leaves the project in a testable, usable state. Phases 1–5 are
built and have been run end to end. Phase 6 is built, proven on a Pi 5 and
carrying `v0.1.12-alpha`; the hardware beyond it — SDI output, larger signal
paths — is a separate Stage Audio Works product line now, not built here.
Phase 7 is built but has not yet carried an event. Phase 8 is built — the vendor
API and the Companion module — and both have been driven against a real OBS, the
module also against a real campus player, though nothing has yet run a whole
event. Phase 10 is built, and its own entry below says what is not: assigning
tiles to several outputs from one box needs hardware beyond the Pi, which is out
of this project's scope. Phase 13 is built. Phase 14 is built.
Phases 12 and 15 have not been started. Phase 9 has been (all five slices —
see the Phase 9 entry below, whose ⬜ predates its own commits).

**Phases 11, 12 and 13 carry weight together.** Between them they are most
of the distance between a project a technician can deploy and one an
ordinary church can — knowing a new build exists and installing it without a
manual reinstall, connecting a bucket without minting a token by hand, and
choosing a provider from a list rather than typing a hostname convention
nobody outside this project has memorized. 13 was deliberately the smaller
half of what 12 needs anyway, and is done first for that reason — "Multisite
Cloud" is already sitting in the dropdown, greyed out, waiting for 12 to make
it real. Phase 14 answers a different question — cost and reliability for a
campus already on the same network or VPN as the main site — and depends on
none of the others. None of the four depends on 9 or 10, and all were
written as late phases when the list
assumed the hardest problem was features rather than deployment.

This project's scope is now the OBS plugin pair and the Raspberry Pi
appliance — nothing wider. Three things that used to be on this list are not
any more. They are written up after the phases, with the reasoning, rather
than deleted.

- **Phase 1 — Reliability core.** ✅ Durable upload queue, retry/backoff, checksums,
  resume-after-crash, decoder cache with verification, and stale detection. This
  is format-agnostic and lands before the media format work. Resume-after-crash
  now also asks rather than deciding silently, when it matters — see §5.1.
- **Phase 2 — Format, namespace & audio.** ✅ FFmpeg CMAF muxing (`init.mp4` +
  `.m4s`), codec-agnostic wrapper (H.264, HEVC and AV1 all round-tripped end to
  end under test; HEVC and AV1 still the less travelled of the three in the
  field), packed multi-channel production audio, the
  `rooms/live.json` + `events/{ulid}` model, keyframe-aligned segments,
  prefix/age lifecycle, and generalized S3 endpoint configuration.
- **Phase 3 — Timeslipping.** ✅ Decoder DVR: playback head vs live edge, deep local
  cache, pause/resume/jump-to-live/scrub, behind-live indicator, restart
  recovery.
- **Phase 4 — Markers & cues.** ✅ Authoring from any site, a shared per-author
  cue list (§7), consumption and jump-to-cue at the satellite, and a Cues dock
  present in both roles.
- **Phase 5 — User interface.** ✅ Encoder and decoder Qt docks, hotkeys, and
  plain-language status, plus event browsing (section 7.5.1), which needs
  bucket listing.
- **Phase 6 — Satellite appliance.** ✅ The low-cost tier: a headless Linux decoder
  with HDMI output and a browser-based operator UI (section 8.1), built on the
  existing receive core. Built: the player engine, DRM/KMS display output with
  its own modesetting, ALSA multichannel audio, the splash and idle screens, the
  web control surface (decoder controls, event list, storage and system
  settings, decoupled preview), and the systemd/install path that makes it start
  on power-up, **proven on a Pi 5 on 2026-09-07** and carrying `v0.1.12-alpha`,
  though not yet through a congregation's event. Complete **for the tier it
  defines**, in section 8.1's terms: the ARM64/HDMI appliance is what this phase
  set out to build. One item remains within this same phase rather than moved
  elsewhere, since it needs no wider hardware tier to answer: hardware-decoder
  selection on Pi 4 (`h264_v4l2m2m`), alongside the software path Pi 5 already
  uses — prefer a hardware decoder when one exists, fall back to software.
  DeckLink SDI output belongs to hardware beyond the Pi, which is out of this
  project's scope — see "Appliance hardware beyond the Pi" near the end of
  this section. The channel de-interleaver has been dropped from scope rather
  than deferred (§4.3.1).
- **Phase 7 — Extensions.** 🟨 Built: the public simulcast relay (§8.2), as a
  separate container in `relay/` — copy remux to one or more RTMP **or SRT**
  destinations, per-destination audio selection, a delay buffer, and
  supervised reconnection, with its own browser UI. Like the appliance it has
  not yet carried an event. HEVC goes out over both protocols now — over SRT
  as MPEG-TS, over RTMP as Enhanced RTMP, which this container's ffmpeg could
  not write until it moved off Debian bookworm's 5.1. Both remuxes are
  verified, and AV1's — the same code path — has carried real encoder output
  through the relay to YouTube and played there for over ten minutes. HEVC's
  has not been through that, and the shared code path makes the AV1 result
  encouraging rather than conclusive.
  Not started: re-encoding, web/mobile simulcast
  served from the bucket, scheduling and auto-go-live, redundancy, and local
  insertion.

- **Phase 8 — External control API.** ✅ Both halves. The command surface of each
  plugin is an obs-websocket vendor request, mirroring the control pages name for
  name and driven through one shared command layer; and a Bitfocus Companion
  module provides the buttons, feedbacks, variables and presets (§8.3). What is
  left is not code: it has been driven against a real OBS and a real campus
  player but has not run a whole event, and the module is not in the Bitfocus
  store yet. Hotkey-based control from Companion works as well, and needs
  nothing.

- **Phase 9 — Redundant storage.** ⬜ Upload to two independent S3 targets, so a
  provider outage, a regional failure, an account lockout or an accident in one
  console stops being a single point of failure for every campus at once. Two
  modes, chosen per room: **active/active**, where all media goes to both, and
  **active/passive**, where the manifests and `live.json` go to both but the
  media only to the primary until a failover.
  The question that needs answering before any code is what *confirmed* means
  with two targets. The whole reliability claim rests on a segment appearing in
  the manifest only after storage has acknowledged it (§4.7); requiring both
  acknowledgements doubles the exposure to the slower link and lets one bad
  provider stall the feed. The original proposal — whose failover half the
  decision below replaces — was to keep the invariant per target: manifest on
  primary confirmation, secondary mirrored best-effort, each target's state
  carried in the manifest, with a failover state machine over the link health
  that already exists. The decoder side follows from `live.json` naming both
  targets, and the checksums already in the protocol are the proof that a mirror
  is a true copy rather than a hopeful one.
  Costs to state plainly: double storage and double origin writes, lifecycle
  rules needed on **both** buckets, *Manage storage…* extended to say which
  target an event is in and to delete from both — and, the one that bites during
  an event rather than after it, **the mirror competes for the same uplink as the
  live feed**. It has to yield to it, or a best-effort second copy becomes the
  stalling this phase was warned about.

  **Decided (2026-09-17): the promise is that an event is never lost to one
  provider failing.** Not "broadcasting continues through a provider outage" —
  that is a larger, different claim, and this phase does not make it.

  What that means in the workflow, which is the part that had to be settled:

  1. **Both targets receive everything, for the whole event** — media,
     `manifest.json`, `live.json`, `event.json`, the cue objects. Not a
     control-objects-first stage: an event that is only half in the second
     bucket cannot be played from it, so a partial mirror is not insurance at
     all. There is a threshold here, not a gradient.
  2. **Both targets are written independently, and the manifest is published on
     the preferred available target's acknowledgement.** Each target gets its
     own upload stream and its own confirmed position; publication waits only
     for the preferred target that is currently working — the primary while it
     is up. So the slower link never sets the pace, *and* the event does not
     stall when one of them is down. Writing to both and publishing on whichever
     is preferred-and-available is the whole mechanism; there is no separate
     "mirror" step whose trigger could be the wrong target.
  3. **The decoder reads the primary, and the second target only for an object
     the primary cannot serve.** On a metered provider that egress is a real
     cost and a working primary should carry the load; it is the same
     per-request fallback the LAN path already uses (§8.7), and for the same
     reason — one object missing is not the target being down.
  4. **If one target fails mid-event, the other keeps receiving** — which falls
     out of (2) rather than needing its own machinery: the failed target's
     confirmed position simply stops advancing while the survivor carries the
     event to its end. The hole the failed target is left with is recorded and
     shown. This is the point of the whole phase: the spool alone rides out an
     outage, but it cannot survive a provider that never comes back, and the
     provider that never comes back is the account-lockout case.
  5. **Completeness is proved, not assumed.** The protocol already carries a
     checksum per segment, so the two targets can be compared — after an event,
     and on demand — and every object present in one and not the other is named.
     Without that, "mirrored" is a claim rather than a fact.

  **Deliberately not in scope: seamless live cutover.** A campus buffers minutes
  ahead, so a failover whose lag is below that buffer is invisible to one that
  was already playing — but a campus that *joins* during an outage has no buffer
  to cover the hole. That is a separate capability with its own limitation, and
  claiming it is what would make this phase's promise dishonest.

  **The uplink: the mirror has to yield, and the operator has to be told.** A
  second copy is a second upload, and most venues have one thin uplink. This is
  not a nicety to add later — without it the promise fails *silently*, because
  the second copy would simply never complete and nothing would say so.

  Two facts shape it. First, **spare capacity cannot be measured from the live
  stream**: the encoder only ever produces at its configured bitrate, so its
  achieved upload rate says whether the link is coping with the stream, never
  what is left over. On a 8 Mbps link a 6 Mbps stream reads exactly like a
  6 Mbps link, until something else asks for the difference. Second, the primary
  is what is on air, so the mirror may never be the reason it suffers.

  So:

  - **The mirror yields to a WORKING primary, not to a dead one.** The
    distinction is the whole rule, and it is easy to get wrong: "uploads only
    while the primary is caught up" sounds right and is not — a primary that has
    failed is never caught up, so the mirror would never run and the survivor
    would receive nothing, which breaks decision 4 in exactly the case this
    phase exists for. So the mirror waits while the primary is making progress
    and has backlog, and proceeds regardless once the primary has stopped making
    progress for a sustained window. The policy belongs to `Session`, which can
    see both streams; the uploader only asks whether it may go.
  - **It then catches up after the event.** The spool holds what the second
    target has not confirmed (see the per-target position above), so the mirror
    drains when the uplink is idle, and the second copy completes then. Costs:
    the box has to stay on, and local disk has to hold the event until it does.
    This is the graceful degradation — "your link cannot carry both at once"
    becomes "your link takes longer to do both" — and it is the same
    store-and-forward bargain the spool already makes for the primary.
  - **The operator is told, in time.** Three moments: while the second bucket is
    being configured, the achieved rate of the last event against this event's
    configured bitrate (weak, since it says nothing about headroom, but it is
    what is free); during an event, when the second target has made no progress
    for several minutes while the event runs — *the link cannot carry both,
    here is the lag* — and the reason, primary-never-caught-up or
    second-unreachable, which are different faults; and after, when the second
    copy is complete, so "mirrored" is a fact rather than an assumption.
  - **An on-demand uplink test**, operator-initiated and nothing else, because it
    is the only way to know spare capacity *before* an event: upload a measured
    payload and report the achieved rate beside the configured bitrate. Off by
    default and never automatic: it is a burst of traffic, and a venue's link is
    not ours to fill uninvited.

  **Where it goes, and why there is only one new seam.** Both halves already
  speak through a `Transport&` and nothing above it knows which store is really
  behind that reference — that is how LAN-vs-cloud fallback was added without
  `DecoderSession` learning it exists (§8.7). Redundancy is the same shape:

  - **Writes (encoder).** Both targets are written from one spool, so the spool
    stops being "what the bucket has not confirmed" and becomes "what at least
    one target has not confirmed": it tracks a confirmed position **per target**,
    and removes a segment's files only once *both* have it. That single change is
    what makes (4) true without a second copy of anything — a segment confirmed
    by the primary but not yet by the second simply stays on disk, and the
    second's uploader picks it up whenever it can. Under the disk cap the oldest
    unconfirmed segment is still dropped, and the hole that leaves is per
    target and is named, because a mirror that silently fell behind would be
    worse than none.
    Publication (the manifest) follows the preferred target that is currently
    working, so `Session` needs only to ask "which target may I publish on",
    not to know there are two.
    The small objects (`live.json`, `event.json`, `manifest.json`,
    `cues/*.json`) go to both directly — they are tiny, and they are what makes
    an event findable at all, so they must not be the ones left behind.
  - **Reads (decoder and appliance).** A second `FallbackTransport`, composed
    from two `S3Transport`s rather than LAN and cloud, chosen per request for
    the same reason: one object missing from the first target is not the target
    being down. Reading the primary by default is also what keeps the second
    provider's egress bill at zero in the ordinary case.
    One consequence to design for: when the encoder has failed over, the
    primary's `manifest.json` stops advancing but still answers 200. A decoder
    reading it would call the event interrupted rather than fall back, so the
    live object has to name both targets and the decoder has to prefer the one
    that is actually advancing.

  **Slices, so each step is reviewable:**

  1. ✅ **The decision above, and the settings surface** — the second target's
     fields in both docks and the machine-wide store behind them. Nothing writes
     anywhere different yet.
  2. ✅ **Everything to both** — a confirmed position per target in the spool, a
     second upload stream that yields to the primary, publication following the
     preferred available target, and the control objects written to both
     directly. Survivor completeness is a property of this step, not a step of
     its own.
  3. ✅ **Telling the operator** — the second target's lag and its reason in the
     dock, the after-the-event "the second copy is complete", the catch-up-when-
     idle behaviour made visible, and the on-demand uplink test. Without this
     step the promise can fail without anyone knowing, which is why it is a step
     and not a footnote.
  4. ✅ **Read fallback** — the decoder and the appliance prefer the primary and
     fall back per object to the second; `live.json` names both, and the decoder
     prefers whichever is actually advancing; *Manage storage…* says which
     target an event is in.
  5. ✅ **Verification** — compare the two targets by checksum and report what is
     in one and not the other, after an event and on demand.

- **Phase 10 — Tile layout and assigned outputs.** ✅ A room that needs two or
  four discrete pictures composites them at the main site today and pulls them
  apart at the satellite with OBS filters by hand ([Choosing a
  satellite](docs/SATELLITE.md)). This makes the split a property of the
  decoder instead: the encoder declares a layout (`1x1`, `2x1`, `2x2`) in
  `event.json`, and each region becomes its own video source, already cropped,
  assignable to a fullscreen output, a DeckLink or AJA output, or a video wall.
  The shape is deliberately the one audio already uses — one decoder, many
  sources, no extra download and no extra decode. Assignment is the plugin
  driving OBS's own outputs rather than a new output of its own: each tile
  source carries a **Send to screen** setting that opens one of OBS's own
  fullscreen projectors on the chosen display, which is what lets a region reach
  a second monitor or a DeckLink without this code knowing what either of those
  is. A satellite with fewer outputs than tiles needs nothing special — every
  tile is a source, each is sent to a screen or left unsent, and an unsent one is
  still there to put in a scene. Nothing has to survive a layout change
  mid-event, because there is none: the layout is written at Go Live, and a later
  event that declares a different one is picked up from the manifest on every
  poll, so no satellite is ever reconfigured. Audio does not follow the tile — a
  picture is a region of the frame, and the programme audio belongs to the feed
  rather than to one quarter of it.
  Built: the layout in `event.json` and the crop geometry live in
  `src/core/` (`TileLayout::tile_rect` and `tile_view()`), the OBS plugin
  exposes each region as its own source, and the headless campus player can put
  one chosen region on its single screen — a `tile_index` setting (`-1` whole
  picture, `0..3` in reading order) crops in the present path as a non-owning
  view, so no frame is copied and the audio is left where it is. The web
  preview offers both views of the same instant — the region going out and the
  whole feed — because once a tile is selected those are no longer the same
  picture, and an operator checking the crop needs to see the edges. Assigning
  tiles to several outputs from one box needs hardware beyond the Pi, which is
  out of this project's scope — see "Appliance hardware beyond the Pi" near
  the end of this section.
- **Phase 11 — Keeping installations current.** ⬜ Installing the plugin is a
  manual act — unzip, move files, restart OBS — and the only way to learn that a
  newer build exists is to go and look at the releases page. Two jobs live here
  and they are not the same size.

  **Telling the operator** is the small one. The plugin already speaks HTTPS
  through the libcurl it links for uploads and already bundles on Windows, so a
  version check costs one request and a comparison against `PLUGIN_VERSION`; it
  belongs off the render thread, cached, and stated in the dock the operator
  already has open rather than in a dialog nobody reads. OBS offers nothing to
  build on: there is no update or upgrade entry point in `libobs` or in
  `obs-frontend-api`, so this is written once, for all three platforms. The
  decision to settle first is whether the check runs by default, because it is
  the one request this project makes that tells a third party that OBS with this
  plugin is running — and a project whose deployments are otherwise entirely
  inside a church's own network should say that plainly rather than bury it in a
  settings page nobody opens. Publishing a small manifest beside the release —
  one request, one number, which build is for which OBS — also keeps the check
  off GitHub's API and its rate limit, and is where the platform and OBS-version
  matching has to live anyway, since OBS refuses a plugin built for a different
  major version and offering the wrong asset is worse than offering none.

  **Applying the update** is the part to be decided rather than assumed. Windows
  cannot overwrite a DLL OBS has loaded: it can be renamed, or a helper can be
  left behind to wait for OBS to exit, but either way the new version appears on
  the *next* start, and the install directory needs elevation to write into.
  macOS is the opposite — the per-user plugin directory is writable without
  privileges, so the swap itself is easy — but these builds are neither signed
  nor notarised, so an update arrives quarantined and OBS then loads nothing at
  all and logs nothing, which is exactly the failure OPERATOR.md already warns
  about; an updater would have to clear the flag itself. Linux is the easiest of
  the three and the Flatpak case the most awkward, because a plugin that cannot
  write its own directory should not pretend otherwise. OBS does not do this
  in-process for itself either: on macOS it hands updates to Sparkle.

  **The prerequisite is done.** Our Windows artifacts and the operator guide
  used to use the legacy layout — files merged into the OBS install directory
  under `C:\Program Files\obs-studio\` — which OBS's own plugins guide warns
  will stop working in a future version. Both now use the recommended
  `C:\ProgramData\obs-studio\plugins\obs-multisite\`, one self-contained
  directory holding `bin\64bit\` and `data\` (see `.github/workflows/obs-plugin.yml`
  and `docs/OPERATOR.md`). That was the actual blocker for a Windows update
  mechanism: the files an updater would replace are now data it can own,
  rather than files inside `Program Files`.

  Nothing here depends on phases 9–12, and it is small enough to pull forward if
  reinstalling by hand on each release is costing more than those phases are
  worth.

- **Phase 12 — Storage credentials and pairing.** ⬜ A second way to answer
  "which bucket, and with what keys" — a device-code pairing against a
  credential broker, beside the typed keys that exist now and never instead of
  them. Designed in §8.5, including the constraints that keep it honest: inert
  until an operator asks for it, a broker URL that is a field rather than a
  constant, cached credentials that never gate going live, and a disconnect
  that hands back the bucket details.

  This is on the list because of what §8.5 opens with. Creating a cloud
  account, scoping a token correctly and writing a lifecycle rule are the three
  steps that decide whether a church can deploy this at all, and they are the
  three a broker removes. Whatever the eventual split between what a church
  does itself and what it pays somebody for, the plugin needs the seam — and
  the seam is small, because `S3Config` is already a plain value struct that
  something builds rather than something the transport reaches out for.

  Depends on nothing else here. Along with Phase 11, the most useful work
  available once the plugins are finished.

- **Phase 13 — Storage provider selection.** ✅ A provider dropdown
  (Cloudflare R2, AWS S3, Backblaze B2, Wasabi, Custom / other
  S3-compatible) that shows only the fields each one actually needs and
  derives the rest, instead of six blank fields regardless of where the
  bucket lives. Built per §8.6, in both OBS docks and — added in later
  passes, once its absence there was noticed — the Raspberry Pi appliance's,
  the simulcast relay's and the OBS plugin's own remote-control pages too.
  `S3Config` itself did not change; this is a UI-layer derivation in front
  of it (`src/core/storage_providers.h/.cpp`), read by all five settings
  surfaces rather than reimplemented per surface, backward compatible with
  every saved setup — a configuration saved before
  this existed reads back as whichever provider its endpoint actually
  matches, or "Custom" if none does, never misrepresented as something it
  isn't. Also the seam Phase 12's brokered credentials will slot into later —
  "Multisite Cloud" is already a greyed-out entry in the same dropdown, not a
  second settings surface waiting to be built.

  Depended on nothing else here, and nothing here depends on it.

- **Phase 14 — LAN / direct delivery.** ✅ A satellite on the same network as
  the main site, or reachable over a VPN the church already runs, downloads
  directly from the encoder instead of from the bucket — automatically
  preferred when it answers, falling back to cloud per request the instant it
  doesn't. Designed and built in §8.7. Cloud upload keeps running by default
  throughout — this adds a second path to reach the same objects, it does
  not remove the first or introduce a control plane §3 doesn't already have
  — but can now be turned off entirely for an operator with no use for a
  cloud copy at all.

  **Built, both sides:** the encoder's `HttpServer::route_prefix()`, a
  `LanObjectServer` serving `manifest.json`/`event.json`/`init.mp4`/segments/
  `markers.json`/`live.json` from a bounded retention window fed by five
  `Session` hooks, an auth-token check; the decoder's `LanTransport` (the
  same `Transport` interface a decoder already downloads through, against
  those exact routes)
  and `FallbackTransport` (LAN preferred, cloud per request otherwise) — all
  proven over real loopback sockets
  (`tests/test_lan_object_server.cpp`, `tests/test_lan_transport.cpp`,
  `tests/test_fallback_transport.cpp`). A `NullTransport`
  (`tests/test_null_transport.cpp`) lets cloud delivery be switched off
  without Session knowing anything changed. The Raspberry Pi appliance
  carries the identical decoder-side wiring (`Config`, `Player`, the web
  settings page) — proven with a real `multisite-player` process against a
  real encoder, receiving an entire event over LAN with no bucket involved.
  The simulcast relay carries the same wiring for the live feed it reads
  before pushing onward (`RoomFeeder`, `ConfigStore`, the web settings
  page) — a relay in the same building as the encoder can now read the
  feed straight from it instead of round-tripping through the bucket, LAN-
  preferred with cloud fallback or LAN alone. Past events stay cloud-only
  on the relay, exactly as they do everywhere else this pattern appears:
  browsing them is a `list()` operation, and `LanObjectServer` only ever
  holds the event currently in progress. **Still not built:** anything
  beyond manual host:port discovery (no mDNS —
  see §8.7 for why), and the device-code pairing flow §8.5 designs for cloud
  credentials, which LAN's shared-token auth is meant to eventually share
  rather than duplicate.

  Depends on nothing else here. The pairing step designed for LAN auth is
  meant to eventually match §8.5's device-code flow, but does not require
  §8.5 or Phase 12 to be built first — the plain pre-shared-token exchange
  already built is enough on its own until brokered pairing exists to share.

- **Phase 15 — Lossless high-quality mode.** ⬜ FLAC in place of AAC on every
  audio track, alongside roughly 10 Mbps HEVC video, as an opt-in mode for an
  event where bandwidth genuinely is not the constraint — a from-the-source
  archival/monitoring copy rather than the bandwidth-tuned broadcast one this
  project is otherwise built around. Switched per event, not per track: an
  event runs either the normal delivery or this ceiling, not a mix.

  **Researched, not yet built** (2026-09-14). OBS already ships what this
  needs: `obs-ffmpeg` registers a FLAC encoder (`ffmpeg_flac`) through the
  identical machinery its AAC encoder uses — same lossless-aware creation
  path, same packet shape — and the mux/decode core here is already
  codec-agnostic: `CmafMuxer`/`CmafDecoder` (`src/core/cmaf_muxer.*`,
  `cmaf_decoder.cpp`) carry whatever `AVCodecID` and extradata they are
  given, which is why this is a plugin-layer change and not a core one. What
  would actually move: `BroadcastController`'s hardcoded
  `obs_audio_encoder_create("ffmpeg_aac", …)`, `multisite_output.cpp`'s
  hardcoded `AV_CODEC_ID_AAC`/`"aac"` track metadata (and its `frame_size`,
  which is never read off the real encoder today and silently defaults to
  AAC's 1024), and the `encoded_audio_codecs` compatibility string OBS
  checks (semicolon-separated — `"aac;flac"`). The Raspberry Pi appliance
  needs no changes at all: it decodes through this same `CmafDecoder`, and
  `pcm_convert.h` already works from however many samples a decoded frame
  actually carries rather than assuming AAC's frame size — FLAC decode is
  also native ffmpeg, no extra dependency, and lighter than the video decode
  the Pi already does.

  **The real constraint is downstream, not technical feasibility.** FLAC
  audio has no home on the public web at all — no streaming site's ingest or
  browser player takes it — and that, rather than the picture, is what makes
  high-quality mode a dead end for the simulcast relay and anything reached
  through it. The video half of that argument has since gone: HEVC travels over
  both protocols now, SRT as MPEG-TS and RTMP as Enhanced RTMP (§8.2). The
  sound half has not, and is not going to — no ingest takes FLAC. So choosing
  it is still choosing "campuses and
  archival only", not "also simulcast this to YouTube/Facebook or watch it
  in a browser". Only the two purpose-built players (the OBS decoder plugin,
  the Pi appliance) decode through `CmafDecoder` and can play it; nothing
  else in this project plays media back at all — the plugin's own web pages
  and the relay's own web page are control/status surfaces, never an
  in-browser player. Whatever UI this gets has to say so up front, before an
  operator discovers it by the relay quietly refusing the event instead.

  Depends on nothing else here.


### Three things that were on this list

None of the three was built, so nothing anyone has is affected. The reasoning
is kept rather than the entries deleted: the people who read a roadmap are the
ones who would notice things quietly disappearing from it.

### The ABR transcoder, "relay plus" — no longer part of this project

It answers a different question from the rest of this document. Everything
else here carries an event between sites a church runs; a rendition ladder
exists to serve an audience on the open internet, which is a delivery
business with its own cost curve, its own failure modes and its own
competitors. Building it here would have quietly changed what this
project is, and would have set an expectation of hosted delivery that a
repository of two OBS plugins cannot honour.

It is not cancelled, it has moved: it is the foundation of a hosted service
Stage Audio Works intends to build separately, fed by the encoder plugin or
by Stage Audio Works' own appliance hardware. The existing relay stays exactly as it is —
free, in this repository, and not degraded to make room for anything.

The engineering notes are kept below because they were dearly bought and
the next person to attempt this should not start from nothing.

The relay copies; this
re-encodes. Decode the incoming feed once, produce a ladder of renditions
(1080p, 720p, 480p), package it as CMAF for **both HLS and DASH**, and write
it to a bucket that is then the origin — so a browser or a phone plays ABR
straight from object storage with no origin server running anywhere. Encoders:
NVENC, VAAPI and x264, behind a capability probe, the way the relay already
probes ffmpeg for SRT. Input stays the bucket as now, but SRT and RTMP input
as well, which is what makes the container useful outside this project.
The hard part is not the encode. It is ladder *alignment* — closed GOPs, one
IDR per segment, identical boundaries across renditions — or ABR switching
glitches at every switch. That should be proven before anything else is built.
Note that the target hardware changes with the job: a $5 VPS has no encoder,
so this wants a small box with a usable iGPU, and the documentation should say
so rather than inherit the relay's VPS story. Built as a library first, the way
`relay_logic` is, so the same engine can later become an in-OBS multi-bitrate
encoder uploading resiliently from the machine that already has the picture.

### End-to-end low latency — dropped

The concept was a second delivery path in fractions of a second rather than
tens — ZeroTier to avoid port forwarding, WebRTC or SRT above it — running on
the same plugins and appliances.

It is dropped for three reasons, none of which were going to improve. It
inverts §1: everything that makes this project worth running comes from
being allowed to buffer, and a sub-second path gives that up on exactly the
venue connections this project exists to survive. Timeslipping does not
survive it — hold, resume and scrub are most of why a campus wants this, and
they are not features you can offer on one path and withdraw on the other
without confusing every operator who has to choose. And it would be a
support liability out of proportion to its use: a path whose quality tracks
the connection minute by minute generates calls that no amount of
documentation prevents.

Where a site genuinely needs conversational latency — a two-way interview, a
campus pastor taking questions — SRT already exists in OBS, there is good
hardware for it, and Stage Audio Works can engineer it per project on the SRT
infrastructure it already runs. That is a better answer than a second delivery
path in this repository, and it is available today rather than after a phase.

### Appliance hardware beyond the Pi — moved, not dropped

Unlike the two above, this isn't a rejected idea — it's a scope decision.
This project's receive and encode hardware is the Raspberry Pi appliance
(Phase 6) and nothing wider: no x86/DeckLink production tier, no RK3588
headless encoder, no rackmount box. Both were sketched here at one point —
an x86 satellite with SDI output and genlock, and a headless encoder on a
Mekotronics R58 (RK3588) using its `rk_hdmirx` capture path and
`librockchip_mpp` hardware encoder — and both are real, still being built,
just not in this repository.

Stage Audio Works builds and sells that hardware, and the software that runs
on it, under its own name (**MultisiteOS**), as a product line planned and
documented separately from this project. Where it reuses this project's core
— the CMAF muxer, the durable queue, the storage protocol — that code stays
exactly what it already is: GPLv3, in this repository, available to anyone
who wants to build their own version of that hardware rather than buy ours.
What moved out is the *planning and positioning* of the hardware itself, not
the license on the software underneath it.

Practically, this is why Phase 6's own text no longer defers Pi 4's
hardware-decoder selection to a later phase, and why Phase 10's tile-to-output
assignment stops where it does rather than promising a numbered phase to finish
it in: what the OBS plugin can do for itself is built, and giving one appliance
box several outputs needs the hardware described here, so that part is out of
scope instead of scheduled. Both were waiting on a hardware tier that isn't
part of this project's roadmap any more.

---

