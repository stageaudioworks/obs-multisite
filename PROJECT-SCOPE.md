# Self-Hosted Multisite Streaming Platform — Project Scope

A free, open-source, self-hosted platform for distributing a live event from a
main campus to any number of satellite campuses **reliably**, over commodity
hardware and unreliable venue internet. It runs as a pair of OBS Studio plugins
and uses nothing but an S3-compatible bucket you control — no central server, no
database, no vendor. Intelligence lives entirely in the edge plugins; the bucket
is a dumb file store.

> **⚠️ Alpha — development build.** This is pre-release software under active
> development. A six-hour continuous soak has been run end to end several
> times, with consistent results each time (see the README's Status
> section), but it has not yet carried a real congregation's event.
> Interfaces, settings and the storage protocol may still change without a
> migration path, and there is no support contract, warranty or uptime
> guarantee of any kind.
>
> Production use comes with caveats. Run it only with a tested fallback in
> place, a technical person on hand, and the assumption that any given event
> may have to go ahead without it. Treat a successful rehearsal as necessary
> rather than sufficient.

---

## 1. Design priorities (ranked)

1. **Reliability above all.** A live event must not drop frames at a campus
   because the main site's internet hiccupped. Every segment is durable,
   retried, and verifiable; nothing is silently lost.
2. **Feature completeness** for how events actually run — production audio
   distribution, markers/cues, pause-and-hold, resume-after-crash, multisite.
3. **Simplicity of operation.** Decentralized and file-based. An operator's whole
   mental model is "hit Go Live" at the main site and "add the source" at a
   campus.
4. **Latency is last.** Minutes of latency are acceptable. We buffer heavily and
   trade latency for resilience at every decision point.

This ranking is the tie-breaker for every design choice.

---

## 2. Headline capabilities

- **Store-and-forward delivery.** The encoder writes each segment to a durable
  local queue first, then uploads with retry. If the network drops, capture keeps
  queuing to disk; on reconnect the queue drains in order. Nothing is lost.
- **Timeslipping (per-campus live-DVR).** Each receiving site can **pause** the
  incoming feed — to hold for its own welcome or announcements — and later
  **resume from exactly where it paused**, while the plugin keeps downloading the
  live feed into a local cache the whole time. Sites can also **jump to live**,
  **scrub** within what's retained, and see **how far behind live** they are.
- **Multi-track production audio.** Delivers up to all 6 OBS audio tracks — main
  mix, ISOs of specific mics, click track — muxed into the same fragment and so
  locked to the picture and to each other. At an OBS satellite each track is a
  separate source, all fed by one decoder, for local mixing, monitoring and
  in-ears. This is the primary mode (§4.3); packed multi-channel (§4.3.1)
  remains available for sites whose output is a single multi-channel device.
- **Markers / cues.** The main site drops named markers into the stream ("Sermon
  Start", "Offering", "Go to local"), manually or on a schedule; satellites see
  them, jump to them, and can trigger local automation.
- **Crash & outage resilience.** OBS crash or power loss at either end is
  recoverable: the encoder resumes the same event and sequence; decoders hold the
  last frame and resume seamlessly when the feed returns.
- **Out to the public, from the same upload.** A small self-hosted event
  reads the segments already in the bucket and pushes them to YouTube, Facebook
  or any RTMP destination, so the main site uploads once whether the event is
  going to two campuses or to two campuses and the internet. It runs a few
  minutes behind on purpose, so a wobble at the main site delays the public
  stream rather than breaking it (§8.2).
- **Bring-your-own storage.** Works with any S3-compatible endpoint — Cloudflare
  R2, AWS S3, Backblaze B2, Wasabi, or self-hosted Garage. Cost is just storage.
- **Satellites can be appliances.** A campus that only needs to *receive* runs a
  headless Linux decoder box driving SDI/HDMI out, controlled from a phone or
  tablet over the local network. No OBS to learn, nothing to misconfigure, and
  it starts on power-up. Campuses that also mix local cameras or graphics run
  the OBS source plugin instead; both share the same core.
- **The origin is not tied to hardware.** Encoder and decoder ship in one module,
  so any machine running OBS can take either role, and what originates an event
  is a laptop with OBS on it. A broadcast can come from a guest speaker's laptop, a
  conference venue for one week, a campus hosting this week's combined event,
  or a site set up at short notice; adding an origin costs a room name and a key
  that can write to it. Nothing ships, clears customs, or is licensed per
  location. Store-and-forward matters *more* for an occasional origin than a
  permanent one: a speaker on hotel wifi or a phone hotspot has the worst
  connection in the chain and can least afford a dropout mid-sermon, and a
  broadcast written to disk and resent survives a link that would kill a direct
  stream.

---

## 3. Architecture

```
 MAIN CAMPUS (encode)          S3-COMPATIBLE BUCKET (dumb store)        SATELLITE CAMPUSES (decode)
┌─────────────────────┐                                              ┌─────────────────────┐
│ OBS + output plugin │  PUT init.mp4 / .m4s / manifest / markers    │ OBS + source plugin │
│  capture → encode   │ ───────────────────────────────────────────▶│  poll → download →  │
│  → CMAF segment     │        rooms/{room}/live.json                │  verify → cache →   │
│  → checksum         │        events/{ulid}/…                       │  local DVR playout  │
│  → durable queue    │◀── polled + GET + verified by every decoder ─│  → Projector → HDMI │
│  → upload w/ retry  │                                              │  (pause/resume/live)│
└─────────────────────┘                                              └─────────────────────┘
```

- **Two kinds of satellite, one core.** The receive logic (discovery, cache,
  timeslipping, decode) is a library with no dependency on OBS or Qt. It is
  driven either by the **OBS source plugin** (for campuses that mix locally) or
  by a **headless appliance** (for campuses that just play the feed out). The
  appliance is the expected deployment for most sites.
- **Decentralized.** No control plane. The media path and the signaling path are
  the same path: objects in a bucket.
- **Read-only decoders.** Satellites need only `GetObject` + `ListBucket`.
- **Addressable by number.** Deterministic segment names (`{seq:08d}`) let any
  node fetch any segment without a directory event.

---

## 4. Storage protocol

### 4.1 Namespace (single bucket per organization)

```
rooms/{room_id}/
    live.json                     # pointer to the current live event_id (+ heartbeat)
    events/{event_id}.json        # per-room index entry, written once at "Go Live"
events/{event_id}/                # event_id = ULID minted by the encoder at "Go Live"
    event.json                    # static: video + audio-track layout, codecs, first_seq
    init.mp4                      # ONE CMAF init: video + every enabled audio track
    segments/{seq:08d}.m4s        # ONE fragment per seq carrying video + all audio tracks
    manifest.json                 # rolling window of recent segments (live edge)
    markers.json                  # append-only cue/marker list
```

- `room_id` is the broadcast source (e.g. `main-auditorium`).
- The per-room index exists because `events/` is a flat global namespace:
  nothing in an event's key says which room it belongs to, only `event.json`
  does. Without the index, listing one room's events means listing every event
  ever recorded and reading each descriptor to discard most of them. Writing it
  is deliberately non-fatal — an event must not be held off air because an
  index entry failed.
- **The index is a shortcut, not the register.** An event with no entry — one
  recorded before the index existed, or one whose entry failed to write — must
  still list, so discovery is the *union* of the index and a scan of `events/`
  rather than the index when it has anything and the scan when it does not.
  Treating a non-empty index as the whole truth hid every older event the
  moment one indexed event appeared. The scan costs a descriptor read only
  for the ids the index did not already name.
- `event_id` is a ULID minted locally by the encoder at "Go Live".
- Decoders need only read access; the encoder needs write access scoped to its
  own room/event paths.

### 4.2 Segment format & codecs

- **Container: CMAF / fragmented MP4** (`init.mp4` + independent `.m4s`
  fragments). It seeks cleanly (needed for timeslipping), carries video plus
  multiple audio tracks in one fragment, and is the native format for HLS/DASH —
  so the same objects can later feed a browser/mobile simulcast with no
  re-packaging.
- **Muxing uses FFmpeg's fMP4 muxer** for correctness; the decode path uses
  FFmpeg as well.
- **Duration: 6 s default (configurable 2–15 s).** The keyframe interval strictly
  equals the segment duration. Longer segments mean fewer requests, better
  compression, and fewer opportunities to drop a request — all reliability wins.
- **Video codec roadmap:** **H.264** first (universal decode), then **HEVC** for
  bandwidth, then **AV1**. Codec identity lives in `event.json`/`manifest.json`,
  and the muxer wrapper and decoder are codec-agnostic, so adding a codec is a
  capability change rather than a rewrite.

### 4.3 Production audio: multi-track (primary mode)

The primary delivery mode is **one audio stream per enabled OBS track** — main
mix, mic ISOs, click — all muxed into the same fragment and delivered as a unit.

- **Native to how OBS already works.** An operator assigns sources to tracks in
  Advanced Audio Properties, exactly as they would for a multi-track recording.
  Nothing new to learn, and the encoder side needs no special configuration.
- **No global layout requirement.** Each track is its own stream with its own
  channel count: a stereo main mix beside a mono click beside mono ISOs. OBS can
  stay in plain stereo at both ends.
- **Per-track channel layout is preserved** — a click or ISO may be mono while
  the main mix is stereo or wider.
- **Sync is structural, not incidental.** Every track shares one fragment, one
  timeline and one playback clock. They cannot drift from the video or from each
  other, because they are demuxed from the same object and played from one
  playout base.
- **Capacity:** up to 6 tracks (OBS's limit).
- **Codec:** AAC to start; Opus is a later option.
- **Far-site output:** the Multisite Source carries the video plus one chosen
  track — track 1 by default, so a campus that only wants the programme does
  nothing. Each further track is exposed by adding a **Multisite Audio Track**
  source, which attaches to the decoder already following that room rather than
  opening its own. The segment is therefore downloaded once and decoded once
  however many tracks a campus uses, and every track is emitted from the same
  playout base. The local engineer routes those sources to mixer tracks, monitor
  sends or in-ears.

Why this is the primary mode and packed is not: OBS resamples every source to
its **global** layout, so packed multi-channel requires both ends to be set to
7.1, and a satellite that is not silently downmixes — summing the ISOs and the
click into the programme. That failure destroys production audio without anyone
noticing until it is on air, and no amount of warning text makes it a good
default. Multi-track has no such mode.

### 4.3.1 Production audio: packed multi-channel (alternative mode)

A **single multi-channel track** — one 8-channel AAC stream carrying main mix,
ISOs and click in fixed channel positions. Supported for sites with a
multi-channel interface that would rather have one stream than several.

- **Sample-accurate by construction.** One stream, one clock. This is a real
  property, but it is not an advantage over multi-track *here*: our tracks
  already share a fragment and a clock.
- **Channel order is the interface.** Channel 4 must be the click at both ends,
  so the mapping is *published* in `event.json` and `manifest.json` as
  `channel_labels` and a satellite routes by name rather than guessing.
  Positional meanings from the speaker layout (FL/FR/LFE/…) are deliberately
  ignored — the layout is only a channel-count carrier.
- **Prerequisite, and the reason this is not the default:** both encoder and
  satellite OBS must be set to **7.1** in Settings → Audio → Channels. Both
  plugins detect a narrower layout and log an explicit error, because otherwise
  the extra channels are downmixed and destroyed silently.
- **Capacity:** 8 channels total, e.g. stereo main mix + 6 mono ISOs.
- **Audio interfaces:** ASIO or Blackmagic DeckLink devices. Mapping packed
  channels onto device output channels at the satellite is **out of scope**,
  not merely unbuilt: in OBS,
  [atkAudio's plugin suite](https://github.com/atkAudio/PluginForObsRelease)
  already routes audio to ASIO, CoreAudio and Windows Audio devices and hosts
  VST3/AU/LV2 plugins alongside, which is a superset of what a de-interleaver
  of ours would have done. It is a separate install under AGPL-3.0; nothing
  here links against it or requires it. Verified 2026-09-07: an 8-channel /
  7.1 feed carried on one track survives this pipeline with its channel order
  intact, which is the part that is ours to get right.

**Which mode suits which satellite.** The two modes are not competing for the
same sites. An **OBS satellite** wants multi-track: OBS routes sources
independently, so separate tracks land on separate destinations with no
routing plugin needed at all. An **appliance** driving HDMI or SDI wants packed: its output
is one multi-channel device, and eight channels in one stream map straight onto
HDMI's eight (§8.1). The encoder can send either; the choice belongs to the
receiving end, which is why both remain supported.

### 4.4 Manifest (live-edge discovery)

`manifest.json` carries a rolling window of recent segments plus the range
metadata a timeslipping decoder needs:

```json
{
  "event_id": "01J8XG7QK3ZC9F8P6M2R4T5V7W",
  "status": "live",
  "updated_at_ms": 1719484800000,
  "first_available_seq": 300,   // oldest segment still retained (for timeslip)
  "window_start_seq": 412,      // oldest listed in this manifest
  "latest_seq": 462,            // live edge
  "init": "init.mp4",
  "video": { "codec": "h264", "width": 1920, "height": 1080, "fps": 30 },
  "audio_tracks": [             // streams inside each segment
    { "idx": 0, "label": "Main Mix",   "codec": "aac", "channels": 2, "sample_rate": 48000 },
    { "idx": 1, "label": "Sermon ISO", "codec": "aac", "channels": 1, "sample_rate": 48000 },
    { "idx": 2, "label": "Click",      "codec": "aac", "channels": 1, "sample_rate": 48000 }
    // … up to 6, one per enabled OBS track
  ],
  "segments": [ /* last ~50: seq, duration, checksum (one file carries all tracks) */ ]
}
```

Decoders discover the live edge from the window but can address any segment from
`first_available_seq` to `latest_seq` by deterministic key, so playback is not
limited to the manifest window — essential for timeslipping.

### 4.5 Markers

`markers.json` is append-only:

```json
{ "markers": [
  { "seq": 420, "at_ms": 1719484860000, "type": "cue", "label": "Sermon Start", "id": "01J8…" }
] }
```

### 4.6 Lifecycle / retention

- Retention is handled by a **bucket lifecycle rule keyed on prefix and age**:
  delete objects under `events/` older than 7 days. Set-and-forget, configured
  once in the storage provider's console.
- Object *tagging* is deliberately not used. S3 supports it, but Cloudflare R2
  rejects requests carrying `x-amz-tagging`, so tag-driven expiry is not
  portable. Tagging remains available as an option for stores that support it,
  off by default.
- Every `PUT` carries `Cache-Control: max-age=604800` for any CDN in front of
  the bucket.
- Old segments are removed by lifecycle expiry, not active deletion, so a
  paused or behind-live decoder can still fetch older segments for the full
  retention window — enabling deep DVR rather than a short buffer.
- **The rule must cover `rooms/` as well as `events/`.** The per-room index
  entry for an event is a few hundred bytes and outlives nothing on its own, so
  a rule that expires only the media leaves the event list advertising events
  whose segments have gone. The catalog handles it — such an event is counted
  as skipped rather than offered — but the list degrades over time for no
  reason. Same age on both prefixes.
- Retention is the set-and-forget default. The encoder UI can additionally
  delete specific events on demand — one at a time, or everything older than a
  chosen number of days — with a confirmation and a verification pass, but the
  codebase otherwise does not delete as it goes, so a paused or behind-live
  campus can still fetch older segments for the whole retention window. The
  event live.json currently names is never deletable.
  The listing is a two-part operation by design: the events are read from the
  manifests first and each one's size measured afterwards, six at a time, so the window is usable on a bucket holding months of events. A size that
  cannot be measured is reported as unknown rather than as zero, and closing the
  window cancels the work in flight.

### 4.7 Write-ordering invariant

A segment is never listed in the manifest until it is durably in storage:
`write local → checksum → PUT segment (+tag) → await 200 OK → update manifest →
PUT manifest`. If a decoder can see a manifest entry, the segment is guaranteed
to exist.

### 4.8 Protocol version

Every document the encoder writes carries `protocol_version`, a single integer.
`live.json`, `event.json`, the room index entry, `manifest.json` and
`markers.json` all have it, so any one of them can be judged on its own by
whatever reads it first.

One integer rather than a semver, because a reader has exactly one question —
*can I still understand this?* — and one number answers it. It starts at **1**.

**It is bumped only for a change that would make an older reader misread a
bucket.** Never for an addition an older reader can safely ignore, which is what
every field added so far has been: `name`, `layout`, `channel_labels` and the
rest all arrived as optional fields with defaults, and an older decoder meeting
one simply does not see it. Growing the format is not the same as breaking it,
and only breaking it moves this number.

**An absent version is version 1.** Every bucket written before the field
existed goes on working untouched, exactly as an absent tile layout parses as
1x1. There is no migration and no flag day. So does a version that is present
but unclear — a string, a null, a zero, a negative, a float — because this
arrives from a bucket that any encoder version may have written, and the oldest
protocol is the safest assumption under which to read an unclear document.
Parsing never throws on it: a malformed version must not be the thing that stops
an event playing.

**Older is readable; newer is refused.** A decoder goes on reading everything it
once wrote, because a recording from last year is exactly what somebody wants to
play back. A document from a protocol it does not know is refused with a message
that says so — *"this event needs a newer version of the plugin"* — rather than
half-read.

That refusal is the entire point of the field. The failure it prevents is not a
decoder that stops; it is a decoder that carries on: a newer bucket half-read as
an older one shows up as a stutter, a failed checksum, or a clock time that is
wrong by an hour, and every one of those sends an operator to look at the
network when the answer is that the plugin is out of date. The check is made
against `manifest.json`, because that is the document whose misreading does the
damage — segment sequence, checksums and timing all come from it — and because
`live.json` yields only an event id, which being wrong is caught at the manifest
anyway.

The event listing is deliberately **not** filtered by version. An unreadable
event stays in the list and explains itself when someone tries to play it, which
is more use to an operator than an event that silently is not there.

---

## 5. Reliability

- **Durable encoder queue.** Segments are written to disk before upload, bounded
  only by disk. Survives OBS crash and power loss. Plain files with atomic
  write-then-rename, deliberately not a database: that gives the durability
  guarantee needed here and is trivial to reason about and to test.
- **Retry with backoff.** Failed uploads retry with exponential backoff and
  jitter, in strict sequence order, for as long as the event is live. No segment
  is abandoned.
- **Checksums.** Each segment's hash is recorded in the manifest; decoders verify
  after download and re-fetch on mismatch.
- **Resume-after-crash.** Event state (event_id, last sequence, queue) is
  persisted, so a crashed or power-cycled encoder can continue the same event
  rather than starting a new one with a gap in the middle. Who actually
  decides that, and when it is safe to decide it silently, is §5.1.
- **Decoder-side durability.** Downloads are cached locally and verified;
  missing or corrupt segments are re-requested. A gap causes a wait-and-retry,
  never a crash.
- **Stale detection.** If a room's `live.json`/manifest has not updated within a
  threshold (e.g. 10 minutes), decoders treat the room as **Offline** instead of
  polling a dead event forever.
- **Sequence-driven sync.** All ordering and synchronization is by integer `seq`;
  campus wall clocks are never assumed to agree.

---

## 5.1 Resume-after-crash: who decides, and when?

**Status: built.** What follows describes the mechanism actually implemented
(`SpoolState::last_activity_ms`, `SessionConfig::resume_stale_after_ms`,
`peek_resumable()`, `EncoderDock::onGoLive()`), kept in its original,
before-the-fact form rather than rewritten as a changelog entry — the
reasoning here is still the reasoning for why it works the way it does.

Before this, Go Live called `Session::check_resumable()`, and if it found an
unfinished event on disk it resumed it — **silently, unconditionally, every
time** (`multisite_output.cpp`, `out_start`). There was no prompt. The
comment above the call even said `// Offer resume`; it did not offer
anything.

Unconditional auto-resume is the right default for the case it was built for
— a genuine crash, restarted within the same service, continuing the same
recording without a gap. It was the wrong default for a case the old code
could not tell apart from that one: an encoder left with an unfinished event
from **last week**, because nobody happened to press End, silently
swallowing today's broadcast into it. Nothing on screen would have said this
happened; the operator would only have noticed from sequence numbers that
made no sense, if they noticed at all.

There was a second problem underneath it. If an operator could ever choose
"start new" over a resumable event, `SpoolQueue::begin_event()` deletes every
`.seg`/`.meta` file in the spool unconditionally — including segments that
were captured but never confirmed uploaded. "Start new" is not a neutral
choice between two equally-valid options; it is destructive to whatever the
old event hadn't finished sending. Any design here has to say that plainly
before it happens, not leave it implicit in a wipe on disk.

**The mechanism: a staleness cutoff, not a permanent choice.**

- `SpoolState` gains `last_activity_ms` — the wall-clock time of the last
  `enqueue()` or `confirm()` — persisted in `state.json` alongside the fields
  already there.
- `SessionConfig` gains `resume_stale_after_ms` (default 30 minutes), the same
  shape as the decoder's own `stale_after_ms` (10 minutes), which already
  exists for exactly this kind of judgment: telling a genuinely quiet room
  from a dead one.
- `Session::check_resumable()`'s `ResumeInfo` reports whether the resumable
  event is stale: `now_ms() - last_activity_ms > resume_stale_after_ms`. A
  free function, `peek_resumable(spool_dir, threshold)`, answers the same
  question from the spool directory alone — no `Session`, no `Transport` —
  because the dock has to decide whether to ask *before* Go Live creates
  anything, and a deferred-start encoder (VideoToolbox and the like) may not
  construct its `Session` until well after that click.
- **Not stale (the common case — a real crash, minutes old):** behaviour is
  unchanged. Auto-resume, no click, nothing in the way of getting back on air.
  What changes is that it stops being invisible: `Session::Status` carries
  what happened (event id, when it started, how many segments are already
  confirmed), and the dock shows a persistent — not auto-dismissing — line:
  *"Resumed event from 10:42, 340 segment(s) already confirmed"*, with an
  *"End this and start fresh"* button beside it. Seen, not assumed.
- **Stale (hours or days old — the ambiguous case):** auto-resume is refused.
  Go Live instead shows a dialog: *"An interrupted event from [date/time] was
  found. Resume it, or start a new event?"* — and if pending segments would
  be lost by choosing new, the dialog says so by name: *"Starting a new event
  abandons N segment(s) that were never confirmed uploaded."*
- The same "abandons N segments" warning covers both entry points into
  "start new" — the stale Go-Live dialog and the "End this and start fresh"
  button on an already-resumed event — rather than being written twice and
  drifting apart. Mechanically, the operator's choice becomes one boolean
  (`force_new_event`, threaded through `obs_output_create`'s settings blob
  like every other Go-Live setting, but never persisted — it means nothing
  outside the one Go Live it was set for); the encoder's own start-up logic
  just obeys it, deciding nothing itself.

**What this must never do:**

- Never block Go Live with a dialog for the ordinary crash-and-restart case.
  That is the case this feature exists to protect, and costing it a click
  would be solving the rare problem by taxing the common one.
- Never delete anything the operator wasn't told about first. A wipe that
  happens because thirty minutes silently defaulted a choice is exactly the
  kind of failure this write-up exists to rule out.
- Never require the operator to remember what happened. The resumed state is
  shown for as long as it's true, not just logged once and forgotten.

**Open question, not yet decided:** whether 30 minutes is the right cutoff, or
whether it should scale with the event's own `segment_duration_s` /
`manifest_window` the way the decoder's staleness threshold does. Thirty
minutes covers a reboot and a coffee break; it does not obviously cover a
long intermission with the encoder deliberately left running idle. Pick a
number, ship it, and let a real false-positive (or the lack of one) settle it
rather than guessing further here.

---

## 6. Timeslipping (per-campus live-DVR)

Each decoder maintains a **playback head** independent of the **live edge**:

- **Local cache & download-ahead.** The decoder continuously downloads new
  segments into a local cache regardless of where playback currently sits.
  Paused or behind live, it keeps filling.
- **Pause / Resume.** Pause freezes the playback head (holds the last frame); the
  cache keeps filling. Resume continues from the exact paused position.
- **Jump to Live.** Snaps the head to the live edge, with a configurable catch-up
  (hard cut by default, or a gentle speed-up).
- **Scrub / seek.** Move the head anywhere between `first_available_seq` and
  `latest_seq` — i.e. anywhere still retained, up to the full retention window.
- **Behind-live indicator.** Always shows how far behind live the campus is.
- **Restart recovery.** The playback position is persisted; after an OBS restart
  a campus resumes where it was or jumps to live (configurable).

Backed by durable object storage, this is "pause live TV," per campus.

---

## 7. Markers & cues

- **Shared, not mains-only.** Every site can drop a cue, and every site sees the
  same merged list. Cues live ONE OBJECT PER AUTHOR —
  `events/{id}/cues/{site}.json` — so the encoder and any number of satellites
  can each add one without overwriting another's. The encoder keeps writing
  `markers.json`, which is its own file and what older decoders still read; a
  reader merges both into one time-ordered list. One writer per object is what
  makes this safe with no locking and no lost cues.
- **Authoring from any end, with any name.** The **Cues dock** is present in
  both roles — the same list and the same controls whichever this machine is.
  The name is typed, not picked from a fixed set: a service has no fixed set of
  moments. Each cue carries the **site name** that set it, so a cue dropped at
  another campus is never mistaken for the main site's.
- **How a satellite publishes one.** With a bucket configured it writes its own
  cue object directly, under scoped write access to `cues/{its-own-site}.json`
  only — it can add cues and nothing else. With NO bucket it hands the cue to
  the encoder instead (`LanObjectServer`'s cue hub, §8.7), which writes it, so a
  LAN-only box stays read-only and needs no bucket credentials at all. That
  precedence is deliberate: cue authoring must never depend on the encoder being
  reachable when the box could have written the cue itself.
- **Consumption (satellites).** Decoders display upcoming and passed cues on a
  timeline, can jump to one, and can fire local automation from one (e.g. a
  "Go to local" cue triggering a campus scene switch). Cues ride the same
  durable object path as the media.
- **A cue lands where the operator is watching, and carries the event's own
  time.** It is stamped at the playhead, not the live edge — the live edge of a
  finished event *is* its end, so stamping that put every cue dropped on a
  recording at the end of it; and a campus sitting behind live means its own
  position, not the encoder's. Its time is the content's time within the event,
  so a recording's cues read 00:00 to the end while a live event's read as times
  of day.
- **Ordered by the event, not by any site's clock.** Each cue carries the
  segment it was dropped at, and the merged list is ordered by that first. A
  box whose clock is minutes out therefore still puts its cue in the right place
  on every other site's timeline, and the displayed time is derived from the
  event's own start rather than from the author's clock.

---

## 7.5 Finished events: video-on-demand

An event that has ended is not a failure state — it is a complete recording,
and a satellite must be able to load and play it exactly like a live feed that
happens not to be advancing.

- **Loading a finished event starts at the beginning**, not at the live edge.
  Treating a completed event as "live" meant loading it and landing seconds
  from the close.
- **Ending a broadcast mid-playback changes nothing for the satellite**: it
  keeps playing through the remaining segments to the end. Nothing is cut off.
- **The reported time never runs past the end of the recording.** Once playback
  passes the last segment the playhead points at a position that does not
  exist; the displayed clock is clamped to the true end.
- **The UI switches vocabulary.** For a live event it reports how far behind
  live the campus is. For a finished one it reports **position out of total
  length** the way a media player does — "24:15 / 1:24:30" — because that is
  what an operator needs when deciding whether a recording fits the slot.
  "Behind live" means nothing once there is no live edge.
- **A finished recording's timeline spans its whole length**, from the moment it
  started to its true end, and does not move during playback. While live the
  right edge is the live edge and necessarily grows; once ended it must not,
  or positions on the bar mean nothing.
- The manifest lists only a rolling window of segments, so neither the length
  nor the timeline bounds may be derived from it. The event's start time and
  last sequence give the true extent.
- Distinguishing a *clean end* from a *lost connection* matters: a clean end is
  reported as a finished recording, while a manifest that simply stops
  advancing is reported as offline after the stale threshold.
- **"Ended" covers two situations that must not read the same.** A broadcast
  that finished *while the satellite was watching* is reported as
  "BROADCAST ENDED" — the event has just closed and the recording is playing
  out. An event that was *already finished when loaded* is reported as
  "RECORDING (not live)" — this is a past event, and nothing has just
  happened. The satellite remembers whether it ever saw the event live, and
  the memory resets when the event changes.

### 7.5.1 Event browsing

Built. The decoder lists a room's events and each entry carries **its own
current state**, not just a date:

- **LIVE** — this event is the one `live.json` points at and its manifest is
  still advancing. At most one event is live at a time.
- **RECORDING** — a finished event, playable as video-on-demand.
- **INTERRUPTED** — the manifest stopped advancing without a clean end, i.e.
  the encoder died. Still playable up to wherever it got to, but the operator
  should know it is incomplete.

A campus will usually see one live event among many recordings, so the state is
what makes the list scannable — the date alone does not say which one is
happening now. Entries are labelled by start date and time, newest first, with
the live one pinned to the top.

Choosing an event pins playback to it. An event starting mid-watch does **not**
steal the playback; the operator is told something is live and offered the
switch, because being pulled out of a recording part-way through is worse than
being told about it.

The same applies to the event being followed when it *finishes*: watching it
live and then sitting in the recording of it is the same commitment in practice
as having chosen it, so the next event starting does not take the picture away
either — the operator presses **Back to live** to move on. (The Raspberry Pi
appliance turns this off, because an unattended box is there to relay whatever
the room does next; see `DecoderConfig::hold_finished_event`.)

### Loading a recording: the workflow, and why the old one misled

Loading one event while another is playing had no progress, no ready signal, and
three quietly wrong readings. Written down before it was built, because the shape
took a decision rather than a fix.

**What was wrong.** `loading_event` (the flag behind "LOADING…") cleared as soon
as *anything* was cached, not when the event was playable; the dock decided
"ready" from the start-buffer setting, which is right for live and wrong for a
recording — `DecoderSession::start()` needs a whole reserve window for a live
event and exactly **one segment** for a finished one; and Play was enabled
whenever nothing was on air, so pressing it too early declined silently and left
an indefinite "BUFFERING…". Three readings that disagreed with each other and
with the session.

**The rule, now stated once.** Readiness is the session's to decide: a single
`start_plan` inside `DecoderSession` computes where playback would start and how
much must be present, and `start()` and the operator's display both read it. A
number the UI derives separately is a number that will disagree.

**The states, and what each says.** One line, one colour, no guessing:

| state | the dock says | on air |
|---|---|---|
| Nothing loaded | as before | idle |
| Loading | "Finding <name>…" | idle |
| Preparing | "Preparing — 22 s of 60 s · ready in ~18 s" | idle |
| Ready | "Ready to play" | idle |
| On air | "PLAYING" | picture |
| Blocked | the reason, named: not found / no access / unreachable / empty | idle |

Two consequences of doing it this way: a finished recording reaches **Ready**
almost at once (it needs one segment, and it should feel instant rather than show
a 60-second countdown), and a failure is **named** rather than timing out into
"I tried something". Play is enabled at Ready, and the reason is shown when it is
not — an operator may still force it, but never by accident. **Stop** is the escape from a load that is not becoming
ready — it cancels what is in flight and takes the decoder down, which is the
honest "give up on this" in a model where a load is applied the moment it is
asked for. A dedicated *Cancel* only means something different once there is
something to go back to, so it belongs with prepare-then-take rather than here;
**Back to live** remains the other exit.

**What is on air while it loads is a decision, not an oversight.** Today the
picture stops the moment a different event is loaded, which for a room full of
people is dead air. Stop-and-hold is cheap and honest and is what is built first;
**prepare-then-take** — keep the current event playing, load the next in the
background, switch on a press — is the kinder answer and is materially bigger,
because it needs either a second `DecoderSession` or a session that can hold two
targets. It is deliberately left as its own phase.

**Better errors are part of this, not a separate polish.** "Could not load" is
not a diagnosis: a 404, a 403, a timeout and an empty event are four different
faults with four different fixes, and each should be said in the operator's words
rather than left to a status code.

## 8. User interface

Two Qt docks, plus hotkeys. The core reliability and media path work with no UI
at all, which is what lets the same engine drive the planned appliance.

**Encoder dock (main site)**

- Storage settings, entered once per machine and committed with **Apply** — so
  opening the settings to look at them and closing again changes nothing, which
  is what makes it safe to do mid-broadcast.
- Video encoder chosen from what the machine actually has (x264, NVENC,
  QuickSync, AMF), hardware first.
- **Go live / End broadcast**, with failures shown in the dock rather than left
  in the log.
- Cues dropped from the shared **Cues dock** (§7) — the same dock a satellite
  gets — with a one-press button per cue name the event already has.
- The reliability readout that matters mid-event: how much of the event has
  been sent, how much is waiting, retries, and link health.

**Decoder dock (satellite)**

- **Load** then **Play**: loading fills the buffer, Play puts it to air.
- A timeline in clock time showing what is in storage, what is downloaded here,
  the playhead and markers. Hovering reports the recorded time under the
  cursor; clicking goes there.
- Hold picture / Continue / Catch up to now, jog in ±1 s to ±1 min steps, and
  "stay behind live by N minutes".
- **Lock**, to stop anything being changed by accident during an event.
- Position and state in plain language, switching vocabulary between a live
  event and a finished recording.

**Language.** The interface never mentions segments, buffers in the abstract,
or live edges. It reports times ("Showing 10:41:03"), durations ("Could
broadcast for 12 min") and plain states. The audience is a volunteer, not the
person who wrote it.

**Hotkeys** cover play, stop, hold, resume, catch-up, jog and marker drops, and
work without Qt — useful for an operator running the event from the keyboard,
and the fallback when a build has no docks.

---

## 8.1 Satellite appliance (headless decoder)

The primary satellite deployment: a small Linux box at the campus that receives,
decodes and plays out, with no operator-facing desktop software.

**Why an appliance rather than a workstation.** A receive-only campus gains
nothing from a full OBS install and loses a great deal: scene collections to
corrupt, updates that change the UI, a desktop that can be left in the wrong
state, and a volunteer expected to understand a production tool. An appliance
boots into its job, restarts itself on failure, and presents one simple screen.

**Shape**

ARM64 / Raspberry Pi, HDMI out — for a site that needs the feed on a screen and
into a small console. This is the only receive-appliance tier this project
builds or documents. Larger hardware tiers are a separate Stage Audio Works
product line, planned and built outside this repository.

**ARM64 / Raspberry Pi notes**

- The portable core cross-compiles for ARM64 today, and CI builds and tests it
  on an ARM64 runner so a regression is caught before it reaches hardware.
- **Video decode.** The Pi 5 (BCM2712) has **no H.264 hardware decoder** — only
  HEVC 4K60. Its NEON software H.264 decoder is reportedly faster than the old
  hardware block and handles 1080p comfortably, so a 1080p30 contribution feed
  is well within it. The Pi 4 *does* have H.264 hardware decode, capped at
  1080p. The decoder should therefore prefer a hardware decoder when one
  exists (`h264_v4l2m2m` on Pi 4, HEVC on Pi 5) and fall back to software
  rather than assuming either.
- This also strengthens the HEVC step on the codec roadmap: HEVC is exactly
  what the Pi 5 accelerates.
- **AV1 decodes in software at full rate, measured on a Pi 5** (2026-09-20):
  libdav1d held a steady 30.0 fps on a 1080p AV1 recording for the whole of a
  ten-minute playout, dropping frames only at the seeks that restart the
  decoder. That was not assumed — the Pi 5 has no AV1 hardware decoder, and
  the expectation was that AV1 would need one.
  It means the low-cost tier is not confined to what the hardware block
  accelerates: AV1 can be chosen for the bandwidth saving on the strength of
  software decode alone, which matters most for the sites on the worst links.
  HEVC remains the only codec the Pi 5 accelerates, so it is still the choice
  where headroom matters — a box also running the AES67 daemon and its own
  interface has less of it than this test did.
  Note that libdav1d runs its own thread pool rather than FFmpeg's, so it
  reports no thread count. `decoding on its own threads` in the log is that,
  not single-threaded decode.
- **Audio.** HDMI carries up to 8 channels of LPCM, which suits the packed
  multi-channel layout: an inexpensive HDMI audio de-embedder recovers the
  individual channels at the campus. That gives a third audio path alongside
  ASIO and DeckLink, and is what makes the low-cost tier viable for production
  audio rather than stereo only. Packed is therefore the appliance's mode even
  though multi-track is the primary one overall (§4.3): an appliance has one
  output device, not a mixer. Fed a multi-track event it plays **one chosen
  track** — the first by default, selectable in Settings for a campus whose
  origin puts the house mix elsewhere. Distributing several tracks across
  output channels is not built, and playing all of them is not a fallback: six
  tracks into a device that accepts one is six times real time of audio, and
  the back-pressure starves the video sharing that thread.
- **Audio over the network (AES67).** A fourth audio path, for a campus that
  wants the sound on its own console rather than only inside the HDMI picture.
  The appliance registers a virtual sound card — Merging's open RAVENNA kernel
  module — and the GPL `aes67-daemon` publishes what the player writes to it as
  an eight-channel AES67 stream, so it appears in Dante Controller and routes to
  anything that takes AES67 directly. Installed by
  `scripts/player/merging-aes67.sh`; the stream is created by that install and
  switched afterwards from the player's own page, which also reports the
  multicast address, the port and the clock state. Switching it on moves the
  player's sound onto the AES67 card — that card being what the daemon publishes
  — and switching it off puts the sound back on the chosen output device; there
  is one output device here, not two. The daemon is a PTP slave, so a master has
  to exist on the network or nothing flows.
- **Storage.** The segment cache writes roughly 3 GB per hour at 6 Mbps. That
  will wear out an SD card, so a USB SSD is required rather than recommended,
  and the cache location must be configurable.
- **Thermals.** Sustained decode needs active cooling; a passively cooled case
  will throttle during a long event.
- **Output:** DeckLink (video plus embedded multichannel audio, which suits the
  packed channel layout), or DRM/KMS for direct display. Audio to ALSA/JACK or
  embedded in SDI. Packed channels go out in the order they arrived, which is
  what an eight-channel HDMI de-embedder or an SDI de-embedder expects — there
  is nothing to de-interleave when the output device is itself
  multi-channel.
- **Control:** a small built-in web server. Operators use a phone, tablet or any
  browser on the church network — hold, resume, catch up to now, jump to a
  moment, and see what is playing and how far behind. No app to install.
- **Operation:** starts on power-up (systemd), restarts on failure, keeps its
  local cache across reboots so a restart mid-event resumes rather than
  restarting.

**What it reuses.** Everything in the receive path: room and event discovery,
the durable segment cache, checksum verification, timeslipping (the playback
head, pause/resume, catch-up, scrub), markers, and the CMAF decoder. These are
already free of OBS and Qt and are covered by the existing tests, so the
appliance is a new *output and control* layer rather than a second
implementation.

**Web UI.** The decoder dock is already a thin view over a status snapshot; the
same snapshot serialises to JSON, so the browser UI is that view rendered in
HTML with a WebSocket for live updates. It should carry the same plain language
— clock times, "hold picture", "catch up to now" — and never mention segments.

**Settled while building**

- **HDMI/DRM first.** The appliance claims the KMS connector itself and sets
  the output mode, with no desktop involved. That is what gives exact control
  of resolution and frame rate, removes a desktop that can be left in the
  wrong state, and suits Raspberry Pi OS Lite, which is the right image for a
  box that only ever does one thing.
- **Yes to a holding screen, and it is the default.** The first problem with
  an appliance is finding it: until somebody knows its address there is
  nothing to type into a phone. So the first thing it does with the display is
  put its own address, name, room and state on it. Idle behaviour is
  configurable — black, hold the last picture, the identity screen, or a
  campus-supplied slide — because a screen a congregation can see is not
  always best left showing the last frame of an event.
- **A script on a stock distribution, not an image.** One command on stock
  Raspberry Pi OS installs the dependencies, builds, installs the event and
  enables it. It works from day one with no release infrastructure, updates
  are the same command again, and it does not tie the project to particular
  hardware. A prebuilt package can follow once the player has settled.
- **The preview is a copy of the output, not a second head.** The web UI shows
  the picture going out, refreshed a few times a second, so an operator can
  watch from a phone without changing what is on the screen in the room. There
  is one playhead, so the preview mirrors it rather than looking ahead; a
  genuinely decoupled look-ahead preview would need a second decode path and is
  deliberately out of scope for the single-box appliance. Where the crop makes
  the two genuinely different pictures — a `tile_index` set on a composited
  feed — the interface lets the operator pick: the region going out, or the
  whole feed as received (`/preview.jpg?view=feed`). The default is the output.

## 8.2 Public simulcast relay

**Status: built.**

The relay reads the same segments from the bucket that feed the campuses and
pushes them to YouTube, Facebook or any RTMP destination. A separate sub-project
in `relay/`, one Docker container on a small VPS; not part of the plugins and
unknown to the core. History: `docs/scope/project-scope-relay-control.md`.

**One destination per audience, each with its own sound.** Destinations are a
list, not a setting: each has its own address, its own audio track — chosen by
the name the main site published, never by track number — and its own
supervision and reconnection. The *video* is identical on every destination: a
copy remux with no transcoder, so there are no per-language bitrates and no
adaptive ladder. Every destination costs the relay's own uplink another copy of
the bitrate, so the status page reports the total going out.

**Two protocols, told apart by the address alone.** RTMP covers YouTube,
Facebook and a church's own server; SRT is for broadcast partners, hardware
decoders and contribution CDNs, and retransmits lost packets rather than letting
them become a glitch. There is no protocol setting: `rtmp://` and `srt://` are
unmistakable, and the relay infers the protocol from the address.

**SRT latency is 2000ms by default,** overridable under Advanced. ffmpeg's own
default of 120ms is enough only on a path short enough to answer almost
immediately; 2000ms is invisible because the relay already sits three minutes
behind the event, and it buys recovery across a much longer path.

**SRT listener mode** is requested by an address with nothing before the port,
`srt://:9000`; it is never the default. It requires opening an inbound port on a
machine otherwise kept closed. A listener nobody has attached to yet is not a
fault and is not reported — it keeps taking up position behind the live edge.
Once it has carried content, losing it is a fault like any other.

**Copy remux, never a silent transcode.** Segments are pushed on unchanged: no
decode, no encode, no quality loss. What cannot be sent that way is refused
rather than adapted:

- **AV1** is allowed over RTMP and refused over SRT. Over RTMP it goes out as
  Enhanced RTMP; whether the destination takes AV1 is the operator's call
  (YouTube documents AV1 ingest and nothing else obviously does), and the page
  carries that caveat above every destination. Over SRT there is nothing to
  send: ffmpeg cannot mux AV1 into MPEG-TS.
- **Packed multi-channel audio (§4.3.1)** — the mix, ISOs and click sharing one
  track — is refused; sending it unchanged would put a mic ISO or the click out
  to the public. Multi-track events (§4.3) are fine: each destination carries one
  track, chosen by the label the main site published.
- **An unknown codec is refused outright** on both protocols.

**Supervision is the point, not a refinement.** Each destination has one ffmpeg
child and one thread that owns it; a child that dies is restarted and resumes
from the segment it was on, so nothing is skipped. A silence shorter than 45 seconds is
ridden out without dropping the connection — fragment timestamps are absolute,
so content resumes exactly where it stopped. Beyond that the connection is
dropped deliberately and rebuilt, and reported as such.

**Finished events,** both gated on the event actually being finished (§7.5.1):
download as one MP4, streamed from storage as requested rather than assembled on
the server, carrying every audio track (the ISOs and the click are what a
post-production edit needs); and replay to a destination, playing a finished
event at normal speed as though live — proof of concept, one at a time, by hand.

**Access.** The relay requires a login on every endpoint but the sign-in itself,
stores the password as PBKDF2-HMAC-SHA256 over a random salt, and binds to
localhost so that exposing it is a decision. It does not terminate TLS: a proxy
in front of it does, shipped as a working example.

**Not built.** Re-encoding; splitting packed audio; SRT listener mode being
reachable through anything (the port has to be published, and nothing is shipped
to help); signing in to YouTube (a stream key is pasted, and the broadcast is
still created in YouTube's own page); and starting by itself, either on a
schedule or when the encoder goes live. The intended trigger is `live.json`
going live, optionally bounded by a time window, and `markers.json` makes "start
the public stream at Sermon Start" possible.

## 8.3 External control API (planned)

**Status: built — sub-phases 1 and 2.**

Operators reach for a physical button, not a dock: a volunteer on a Stream Deck
should hold, resume and catch up without finding a window, and the main site
should go live from a button. The target is **Bitfocus Companion**.

**One surface, two transports.** The appliance exposes its controls as HTTP
routes (§8.1): `/api/play`, `/api/hold`, `/api/seek`, `/api/status` and the
rest. The OBS plugins expose *the same command names with the same payloads*
over **obs-websocket vendor requests** — one API, two ways in, so a control
surface written against either works against both.

**Why vendor requests rather than a server inside the plugin.** obs-websocket
ships with OBS 28 and later and its `obs-websocket-api.h` is header-only, so the
plugin gains no link dependency and nothing needs installing; if obs-websocket
is absent every call returns false after one log line — control disappears,
nothing breaks. A bespoke HTTP server would duplicate the authentication,
listening socket and TLS already solved there, and open a second port on a
machine otherwise kept closed.

**Both ends, equal weight.** The encoder needs go-live, stop, drop-marker and
status; the decoder needs play, stop, hold, resume, catch-up, seek, jog, delay,
load-event, follow-live, audio-track selection, status and the event list. The
command set already exists as `EncoderControls` and `DecoderControls`, so the
API layer is an adapter over what the docks and hotkeys already call.

**Sub-phase 1, the vendor API — built.** Every command of both halves is a
vendor request under `obs-multisite` (`encoder/go-live`, `decoder/hold`, …),
with vendor events on change and a `status` request for polling. The names live
in one list in the portable core (`src/core/control_api.h`) that the HTTP routes
read too, so "the same command names" is true by construction and a core test
pins it. It registers from `obs_module_post_load()`, as the header requires.

**Sub-phase 2, the Companion module — built.** A Bitfocus Companion module
lives at
[stageaudioworks/companion-module-obs-multisite](https://github.com/stageaudioworks/companion-module-obs-multisite).
Every action is one vendor request and every feedback is one status field: Go
live, End and marker buttons for a main site; transport and timeslipping for a
campus; on-air/held/behind-live/offline feedbacks; variables for both halves;
two preset banks; and a pass-through action for any vendor request by name. The
same module also drives a campus player appliance directly over its HTTP API
(§8.1). Its own obs-websocket connection and its source/distribution licensing
were forced rather than chosen (archived in
`docs/scope/project-scope-relay-control.md`).

Both ends have been driven for real, and the module is tagged **v0.2.0**. Two
things are still open, and neither is code: nothing has yet run a whole event,
and the module is not in the Companion store — until it is, Companion loads it
as a *developer module* and needs **Companion 4.0 or later**.

---

## 8.4 Control pages served from the plugin

Both halves of the plugin serve an operator page on the church network, out of
OBS itself: the same interface the appliance has (§8.1), so somebody who has
learned one does not have to learn the other. It is the appliance's page on
purpose, and the words on it are the words of an event rather than of a video
pipeline.

**What it is for.** The desk is not always where the event is. A marker has to be
pressable from the back of the room, the sending queue watched from the foyer,
and the reason nothing is happening read by somebody holding a phone and no
access to the machine.

**Shape.**

- **Sending side** — Go live and End the broadcast, the editable event name, the
  cue buttons (the event's own cue names), and the reliability readout an
  operator watches mid-event:
  confirmed pieces, what is waiting to send, retries, bytes sent, the measured
  upload rate, the colo serving the bucket, and the last error.
- **Receiving side** — play, hold picture, catch up to now, jog, stay behind
  live, a clickable timeline, the recordings list, and how long this campus could
  keep playing through an outage.
- **Both** — the log, for somebody who cannot reach Help → Log Files, and a
  **Lock** that refuses anything which would change what is on air.

**Which pages exist follows the machine's role**, as the docks already do: a main
site serves the encoder page and has no decoder routes at all, a satellite the
other way round, and a machine set to Both serves both and links them.

**One server, three users.** The pages run on the core's own HTTP server
(`src/core/http_server.cpp`), shared with the relay and the appliance rather than
reimplemented three times. That is why the server is tested on every platform CI
builds (`tests/test_http_server.cpp`) rather than only where a POSIX socket is
available, and why a routing mistake is treated as a defect rather than a
cosmetic one: it is an operator's click doing nothing, or somebody reaching a
page they should not.

**Trust, stated plainly.** There is no password and no TLS. It binds every
interface, for the same reason the appliance's page does — a page that answers
only `localhost` cannot be reached from the tablet it exists for — and the
building's own network is the guard. It is on by default on port 8080 and can be
switched off or moved in **Settings → Remote control** in either dock, which also
shows the address to type into a phone. Editing storage from the page is allowed;
retyping a secret key is not required, and a stored key is never sent to a
browser. **Lock** is deliberately not remembered across a restart: a lock that
survived one would leave a campus unable to broadcast with no obvious reason why,
and the tablet that set it is long since charged and put away.

---

## 8.5 Storage credentials: direct or brokered (planned)

**Status: not built** — designed here; Phase 12 in §10 delivers it.

**Brokered** credentials are added beside the typed keys, never instead of
them: the plugin holds a device identity and a service URL and fetches
short-lived credentials from a broker that manages the bucket for the operator.
**Direct** (endpoint, bucket, key, secret, region) stays unchanged and never
deprecated. Pairing is a **device-code flow**, the one a television uses to sign
into a video service: a short code shown in the dock and entered on the broker's
page from any device, never a password typed into OBS or a key pasted from an
email. It is **inert until asked** — nothing contacts anything but the configured
bucket until an operator presses Connect — and the **broker URL is a field, not
a constant**: it may ship with a default and must be editable. It lands in the
greyed-out **"Multisite Cloud"** entry already sitting in §8.6's dropdown; the
sequence, constraints and unsettled questions are archived in
`docs/scope/project-scope-phases.md`.

---

## 8.6 Storage provider selection

**Status: built — the two OBS docks first, the Raspberry Pi appliance's web
settings page after an operator noticed it was missing there, the simulcast
relay's alongside its own LAN work, and the OBS plugin's own remote-control
pages last of all (§8.4) — an operator on a phone had exactly the six raw
fields the docks left behind years ago.** What follows
is kept in its original, before-the-fact form, with deviations from it noted
where they happen — the reasoning here is still the reasoning for why it
works the way it does.

Setting up storage used to mean an operator typing six fields — account ID or
endpoint, bucket, key, secret, region — into the encoder dock's Storage tab,
having first worked out which of those six their provider actually needs and
what shape the endpoint hostname has to be in. Cloudflare R2 needs an account
ID and no region. AWS S3, Backblaze B2 and Wasabi each need a region and
derive their hostname from it, in three slightly different ways. A church
volunteer configuring this for the first time had no way to know any of
that, and the dock didn't tell them either — it showed the same six blank
fields regardless of where the bucket actually lived.

**A provider dropdown, not a new field shape.** `S3Config` — bucket, key,
secret, and either an account id or an endpoint host plus region — does not
change. What changes is what builds it. A `Provider` choice
(*Cloudflare R2 · AWS S3 · Backblaze B2 · Wasabi · Custom / other
S3-compatible*) selects which of a small set of templates derives the
endpoint from the fields that provider actually needs, and the dock shows
only those fields:

- **Cloudflare R2** — Account ID, Bucket, Key, Secret. Region is fixed
  (`auto`); the endpoint is `https://<account>.r2.cloudflarestorage.com`.
- **AWS S3 / Backblaze B2 / Wasabi** — Region, Bucket, Key, Secret. Each
  derives its own hostname template from the region (`s3.<region>.amazonaws.com`
  and so on).
- **Custom / other S3-compatible** — today's full form, unchanged: raw
  endpoint, bucket, key, secret, region. This is not a legacy fallback to be
  deprecated later — someone running MinIO in their own rack, or a provider
  not in the list, is a first-class user of this option, exactly as §8.5
  already says of "Direct" credentials generally.

The templates themselves live in one place, `src/core/storage_providers.h/.cpp`
— read by every settings surface that needs them, not duplicated per surface,
the same "decided in one place" instinct behind `audio_plan.h` and
`disk_health.h` elsewhere in this codebase, and fully unit-tested with no Qt
and no OBS (`tests/test_storage_providers.cpp`). That portability is what
made the appliance's own dropdown a small job once it was noticed missing:
`src/appliance/api.cpp` derives the same way `onSaveSettings()` does in both
docks (a new `/api/storage/providers` route lists the choices; `apply_edit()`
derives `endpoint_host`/`r2_account_id`/`region` from whichever one field the
operator typed, exactly the OBS logic, just in C++ building JSON instead of
setting `S3Config` members directly), and `Config` gained the matching
`storage_provider` field with the identical empty-means-guess-from-
`detect_provider()` fallback for a config saved before this existed.
**One deviation from the original plan here:** this was designed as a bundled `data/providers.json`, editable
without a rebuild, the same way locale strings and web pages are. Built as a
compiled table instead — these five providers' hostname conventions are a
technical fact that essentially never changes, not operator-facing content
an integrator would want to edit on a live machine, so the file-loading
machinery (`obs_module_file()` path resolution, one more thing to get wrong
on three platforms) bought less than it cost. Adding a provider is still a
small, self-contained change; it just needs a rebuild, the same as any other
fix in this codebase. A network-fetched manifest was considered too, for the
same "add a provider without a release" property, and rejected for the same
reason as the file: this project's general reluctance to add a network
dependency where none was truly needed. Phase 11's update-check manifest
(§10) is the place that pattern already belongs, if it's ever needed here.

**Where "Multisite Cloud" fits.** §8.5 already frames the future brokered
option as a second producer of the same `S3Config` — "two providers, one
`S3Config`." This dropdown is the literal UI expression of that sentence,
generalized to N providers instead of two: *Multisite Cloud* becomes one
more entry, greyed out until built, and when selected shows §8.5's
device-code pairing flow (a *"Connect to a storage service"* button, a
status line, a Disconnect button) instead of key/secret fields. No new
settings surface is built for it later — it slots into the one that
already exists for R2, AWS, Backblaze and Wasabi.

**Not settled.** Whether region-based providers (AWS/Backblaze/Wasabi) offer
a dropdown of common regions, free text, or both — free text alone repeats
today's "which exact string does the hostname want" problem; a dropdown
needs to be honest about the fact that not every provider's regions are the
same list.

---

## 8.7 LAN / direct delivery

**Status: built, both halves, and every kind of receiver.** A receiver — the OBS
decoder plugin, the Raspberry Pi appliance, or the simulcast relay — on the same
network as the encoder, or reachable over an existing site-to-site VPN,
downloads directly from it: manifest, init segment, media fragments and markers,
instead of from the bucket, preferring that path when it answers and falling
back to cloud, per request, when it doesn't. Cloud delivery can also be turned
off entirely. The appliance's `Config`/`Player` and the relay's
`ConfigStore`/`RoomFeeder` gained the same `lan_host`/`lan_port`/`lan_auth_token`
fields and the same `LanTransport`/`FallbackTransport` wiring as the OBS decoder
— one codebase (`src/core/`), three settings surfaces, no divergence.

**What it adds, and what it does not replace — unless told to.** By default the
encoder keeps uploading to the bucket exactly as it does today, unconditionally:
a cloud-only decoder, a LAN decoder whose link just dropped, and the archival
recording all still depend on it. It serves the *identical* object shape a cloud
decoder already reads, so media and signaling remain one path with a second,
local way to walk it. The relay's past-events browsing, download and rebroadcast
stay cloud-only: they are `list()`-based, which `LanObjectServer` does not serve.

**Turning cloud off entirely.** Cloud-off hands `Session` a `NullTransport` in
place of the real transport: every PUT reports instant success, so the spool →
retry-uploader → manifest pipeline runs exactly as always — segments confirm
immediately, LAN hooks fire on schedule — and nothing leaves the machine. It is
not a separate code path, just the same one pointed at a transport that keeps
nothing. The dock refuses to go live with both cloud and LAN off at once, and
the cloud-off checkbox only appears once LAN delivery is on.

**LAN serving keeps its own cache, not the spool.** The spool holds a segment
only until the bucket confirms it, then deletes it, so the segment a LAN decoder
most wants is by design the one the spool no longer has. `LanObjectServer`
(`src/core/lan_object_server.h`) therefore combines a bounded `SegmentCache` —
the class a decoder already uses for its own cache — with `Session` hooks fired
as the bytes or JSON are in hand; `Session` stays unaware LAN delivery exists,
and the hooks cost nothing when unset. The cache lives in the operator's **Cache
folder** under `lan_cache`, or the plugin's config when that is blank. Routes
served (`http_server.h`'s longest-prefix-first `route_prefix()`, since a segment
path names a sequence number): `manifest.json`, `event.json`, `init.mp4`,
`segments/{seq}.m4s`, `markers.json`, `rooms/{room}/live.json`.

- **Discovery — built, deliberately manual.** Host, port and optional token are
  typed into the decoder dock, the same place cloud credentials go; mDNS was set
  aside (it does nothing for the VPN case and is one more thing to fail silently
  on a locked-down network). A LAN host with no cloud credentials works LAN-only,
  and a room-following satellite learns the live event id from `live.json`.
- **Auth — built, still a shared bearer token, not yet paired.** `Authorization:
  Bearer <token>` is enforced end to end; getting that token onto a decoder is
  still typing it into both docks, not yet §8.5's device-code pairing — the goal
  is one pairing flow used for both. A plain LAN inside one building is already
  the trust boundary elsewhere here; a token matters more across a VPN.
- **Preference and fallback — built, per request, not per session.**
  `FallbackTransport` tries LAN first and only reaches for cloud if LAN didn't
  answer, so one aged-out segment falls back alone without flipping the session
  — simpler by design than per-transport `LinkHealth` hysteresis, with no timer,
  state machine or operator-visible mode.
- **Visibility — built.** The dock's storage-link line names the path the most
  recent fetch took — *"via LAN"* or *"via cloud"* — and appears only once a LAN
  host is configured.

**Deferred rather than decided against.** Whether LAN mode serves a segment the
moment it's spooled (lower latency) or only once the bucket confirms it (cloud's
consistency guarantee, simpler) is left for a later, explicitly opt-in mode. A
first build serves only what's already confirmed.

> Full rationale, internals and measurements: `docs/scope/project-scope-lan-delivery.md`.

---

## 8.8 Captions

Captions ride **inside the video bitstream** as CEA-708 SEI, not as a caption
track. libobs does the hard part: `add_caption()` in `obs-output.c` splices the
SEI into the encoded video packet, and it runs inside `send_interleaved()` — the
same path that hands packets to our output's `encoded_packet` callback. A
caption given to our output is inside the H.264/HEVC/AV1 bytes before we see
them.

Everything downstream is byte-transparent and needed no changes at all:

| Stage | What it does with the bytes |
|---|---|
| `CmafMuxer` | `av_write_frame` verbatim; no bitstream filter |
| `multisite_output` | `cp.data.assign(pkt->data, …)`, a straight copy |
| Storage / manifest | Describes video and audio; the payload is opaque |
| Satellite decoder | Hands the same bytes to FFmpeg |
| Relay | `-c copy`, and SEI travels inside the video NALs |

A caption track would have meant new work in all five.

**Where captions come from: whatever already has them.** The default is
Automatic, which subscribes to every source and forwards captions the feed
already carries, byte for byte, through `obs_output_caption`. A source announces
them with `obs_source_output_cea708`; DeckLink does this today, parsing CDP out
of SDI VANC (it needs "Allow 10 Bit" switched on), and anything else may
tomorrow — nothing here knows which plugin is involved. Embedded captions are
never turned into text and re-encoded, so the upstream encoder's timing,
roll-up and positioning survive.

**The text fallback is a deliberate choice, not part of Automatic.** A captioner
that cannot emit CEA-708 — LocalVocal, and the Google, Deepgram, ElevenLabs and
Speechmatics plugins — writes its words into a text source, and naming that
source switches this on. It is excluded from Automatic because a scene is full
of text sources that are not captions, and captioning a lower third would be
worse than captioning nothing.

**What the wire carries.** A CEA-708 wrapper around a **CEA-608 payload**:
`sei_from_caption_frame` builds an ITU-T T.35 SEI containing `cc_data`. That is
what broadcast and YouTube mean by closed captions and what they ingest, but it
is not DTVCC 708. OBS cannot carry that: `add_caption()` drops every packet
whose `cc_type` is not 0, keeping 608 field 1 alone, so an SDI feed carrying
true 708 services loses them. Expect 608's constraints — about 32 characters a
row, a limited character set, pop-on style.

**Length, on the text path only.** libobs holds a caption in a 128-byte buffer
and fills it with one `snprintf`, truncating, beneath a comment claiming it
splits. A spoken sentence passes 128 bytes regularly, so `caption_text.h` splits
on word and UTF-8 boundaries first. A long sentence then occupies several
caption slots and takes longer to clear — the honest trade against losing half
of it. Embedded captions never go through this: they are bytes, not text.

**Not built, and known.** Nothing renders captions at the campus end. OBS has no
caption renderer, and a survey found no third-party plugin that decodes or
displays CEA-608/708 from a source — every OBS caption plugin is a generator. So
captions reach a satellite and pass through to anything it restreams, but the Pi
player cannot yet burn them into HDMI. That is its own work: SEI extraction, a
CEA-708 decoder and compositing onto the frame.

## 9. Capability overview

What this project does, and where each piece stands. Status is against the
codebase, not against anything else on the market — where a commercial platform
is the better answer for a given church, section 12 says so plainly.

| Capability | Status |
|---|---|
| Resilient store-and-forward upload | built |
| Multisite to any number of campuses (storage cost only) | built |
| Markers / event cues | built |
| Pause & hold at a campus, resuming exactly where it stopped | built |
| Per-campus independent DVR position | built |
| Multi-track production audio (main / ISOs / click), up to 6 tracks | built — one source per track at the satellite (§4.3) |
| Event browsing with live / recording / interrupted state | built |
| Storage management from the encoder dock (list a room's events with sizes, delete one or older-than-N) | built (§4.6) |
| Operator page served from the plugin to a phone or tablet, following the machine's role | built (§8.4) |
| Video-on-demand playback of past and interrupted events | built |
| Any OBS machine can originate a broadcast | built |
| Dedicated receive appliance (Raspberry Pi) | built and proven on a Pi 5; not yet run through an event (§10 Phase 6) |
| Self-hosted, on storage you own | built |
| Open protocol, no vendor lock-in | by design — the whole protocol is §4 |
| Public simulcast to YouTube / Facebook / RTMP or SRT | built and pushing live to YouTube; not yet through a full event. H.264, HEVC and AV1 over either protocol, each subject to the destination taking it — and AV1 has now carried a real event to YouTube (§8.2); HEVC's own last mile is the one still unproven |
| One event to several destinations, each with its own audio track | built — the relay's destination list, sound feed chosen per destination by name, and a different language or feed can go to each stream from the same upload (§8.2) |
| SRT output, caller or listener | built and receiving on a real client; not yet run through a full event (§8.2) |
| HEVC out, over SRT or Enhanced RTMP | built, and the remux verified at the byte level — but not yet carried from a real HEVC encoder to a real destination. AV1, on the same code path, has now been carried to YouTube (§8.2) |
| Download a finished event as an MP4, all audio tracks | built (§8.2) |
| Replay a finished event to a destination | built — one at a time; two of the event's cues bound it as in and out points, so a replay is an excerpt rather than the whole recording (§8.2) |
| Per-channel routing of packed audio at an OBS satellite | out of scope — use [atkAudio's OBS plugins](https://github.com/atkAudio/PluginForObsRelease) (§4.3.1) |
| Re-encoding an HEVC feed for a streaming site | not built; an SRT destination carries HEVC unchanged instead (§8.2) |
| External control API (obs-websocket vendor requests, §8.3) | built — every command of both halves, with vendor events |
| Bitfocus Companion module (buttons, feedbacks, variables) | built — [companion-module-obs-multisite](https://github.com/stageaudioworks/companion-module-obs-multisite), driving OBS or a campus player; not yet in the store |
| Control from a Stream Deck via OBS hotkey triggers | available now, no parameters or feedback |
| Web / mobile simulcast from the same files | planned; CMAF makes it feasible |
| Scheduling / auto-go-live | planned — for the relay as well as the encoder |
| Redundant storage: two independent S3 targets | built — both receive everything, reads fall back per request and switch ends when the one being read stops advancing, and completeness is checkable on demand; seamless live cutover is deliberately NOT claimed (§10 Phase 9) |
| Tile layout: a 2x1 or 2x2 feed split into discrete sources, owned by the decoder | built — OBS plugin sources it; campus player shows one chosen tile on its single screen (§10 Phase 10) |
| Fullscreen output assignment driven by the decoder plugin | built — each tile is a source with its own **Send to screen**, opening one of OBS's own fullscreen projectors; SDI is whatever OBS's DeckLink or AJA output already does, and several outputs from one appliance box is out of scope (§10 Phase 10) |
| ABR transcoder ("relay plus"): a ladder written to a bucket that is its own HLS/DASH origin | **not part of this project** — moved to a separate hosted service (§10) |
| End-to-end low latency over ZeroTier, with WebRTC or SRT | **dropped** — use SRT, already in OBS (§10) |
| Knowing a newer build exists, and applying it without a manual reinstall | notification built — the plugin and the player each ask once per run and say so; whether an update applies itself is still undecided (§10 Phase 11) |
| Connecting a bucket by pairing rather than by pasting keys, against a broker anyone can run | planned (§8.5, §10 Phase 12) |
| Choosing a storage provider from a list instead of typing raw endpoint fields | built (§8.6, §10 Phase 13) |
| Monitoring heartbeat to a collector (off by default; OBS + Pi, manual or paired) | built — each role's filtered status plus the appliance host block, every 30 s active / 5 min idle, fire-and-forget; device-code pairing preferred, typed URL/id/token as fallback |
| Satellite receiving directly from the encoder over a LAN or existing VPN, cloud as automatic fallback | built, both sides (§8.7, §10 Phase 14) |
| Lossless high-quality mode: FLAC audio + ~10 Mbps HEVC, players only, no relay/web path | planned (§10 Phase 15) |

Further directions to explore: web/mobile simulcast served directly from the
bucket (which needs no relay at all — the CMAF objects are already the right
shape for it), and local insertion windows for campus announcements.
Multi-bucket mirroring has graduated from a direction to explore into Phase 9
(§10).

**HLS push as a third relay destination, alongside RTMP and SRT** (raised
2026-09-14, not yet a phase — still being thought through). YouTube's HLS
ingest is real and documented: encoders `PUT`/`POST` a rolling MPEG-TS
playlist and segments to a YouTube-issued URL, which is architecturally
closer to what the relay already does than it first sounds — the relay
already remuxes to MPEG-TS for SRT (§8.2), and ffmpeg's own `hls` muxer can
do the PUT publishing itself with `-method PUT`, so no playlist/upload logic
needs hand-writing. What it would actually buy: HDR/HEVC to YouTube, a
higher bitrate ceiling than RTMP, and a path in for a venue network that
blocks RTMP but allows HTTPS.

The catch is smaller than it first looked, but still real. Checked against
YouTube's own HLS ingestion docs directly (not just a summary of them):
**1–4 s is only "recommended,"** for lower latency and better encoding
efficiency — the actual hard limit, stated as "must not," is that **a Media
Segment must not be longer than 5 seconds**, and a Media Playlist must not
carry more than five outstanding (unacknowledged) segments. Neither page
states what happens if a stream violates either rule — no "will be
rejected" language anywhere near the duration text — so whether YouTube
hard-fails an over-length segment or just serves it worse (or serves it
fine) is itself unconfirmed and worth testing before assuming the worst.

Either way, the relay's 6 s default (§4.2) sits on the wrong side of the
hard 5 s ceiling, if that ceiling is actually enforced — by only a second,
not the 2+ seconds it would take to get into the merely-"recommended" 1–4 s
band. The relay copy-remuxes only — no decode, no encode, by design, which
is what lets it run on a $5 VPS (relay/README.md) — so ffmpeg's HLS muxer
still can't cut a segment shorter than the source's actual keyframe
interval without re-encoding. But landing at, say, 4.5–5 s instead of 6 s is
a much smaller ask of the encoder's global segment-duration setting (already
configurable 2–15 s, §4.2) than dropping all the way to 4 s would have been,
and costs correspondingly less of the reliability margin that default buys
everywhere else. Worth an actual test against YouTube's ingest — does it
reject a 6 s segment outright, or just complain — before deciding whether
this needs any tradeoff at all.

---

## 10. Delivery phases

Each phase leaves the project in a testable, usable state. Phases 1–5
(Reliability core, Format/namespace/audio, Timeslipping, Markers & cues, User
interface) are built and have been run end to end. The status of each later
phase, with one or two lines of what it does, is below; the full rationale,
measurements and "not built" notes are archived in
`docs/scope/project-scope-phases.md`.

**Phases 11, 12 and 13 carry weight together** — between them they are most of
the distance between a project a technician can deploy and one an ordinary church
can: knowing a new build exists and installing it without a manual reinstall,
connecting a bucket without minting a token by hand, and choosing a provider from
a list. Phase 13 was deliberately the smaller half of what 12 needs and is done
first; "Multisite Cloud" already sits in its dropdown, greyed out, waiting for
12. Phase 14 answers a different question — cost and reliability for a campus on
the main site's own network — and depends on none of the others. None of the four
depends on 9 or 10.

- **Phase 6 — Satellite appliance. Status: built** for the ARM64/HDMI tier it
  defines: a headless Linux decoder with HDMI output and a browser operator UI
  (§8.1) on the existing receive core, proven on a Pi 5 (2026-09-07) carrying
  `v0.1.12-alpha`, though not through a congregation's event. One item remains
  within the phase — hardware-decoder selection on Pi 4 (`h264_v4l2m2m`);
  DeckLink SDI and the channel de-interleaver are out of scope (§4.3.1).
- **Phase 7 — Extensions. Status: partial.** Built: the public simulcast relay
  (§8.2) in `relay/` — copy-remux to RTMP or SRT, per-destination audio, a delay
  buffer, supervised reconnection — not yet through an event. Not started:
  re-encoding, web/mobile simulcast from the bucket, scheduling, local insertion.
- **Phase 8 — External control API. Status: built**, both halves. Each plugin's
  command surface is an obs-websocket vendor request over one shared command
  layer, and a Bitfocus Companion module provides buttons, feedbacks, variables
  and presets (§8.3). Driven against a real OBS and a real campus player, but no
  whole event yet, and not in the Bitfocus store.
- **Phase 9 — Redundant storage. Status: built.** Upload to two independent S3
  targets so a provider outage, regional failure, account lockout or console
  accident stops being a single point of failure for every campus. Two modes per
  room: **active/active** (all media to both) and **active/passive** (manifests
  and `live.json` to both, media to the primary until failover). The promise is
  that an event is never lost to one provider failing. All five slices are done.
- **Phase 10 — Tile layout and assigned outputs. Status: built.** A layout
  (`1x1`, `2x1`, `2x2`) declared in `event.json`; each region becomes its own
  pre-cropped source, assignable through OBS's own fullscreen projector to a
  screen, a DeckLink or a video wall. Assigning tiles to several outputs from one
  box needs hardware beyond the Pi and is out of scope.
- **Phase 11 — Keeping installations current. Status: not built.** Tell the
  operator a newer build exists (one HTTPS request, stated in the dock), and
  decide how an update is applied; OBS offers no update entry point. The
  packaging prerequisite — the recommended Windows layout — is done.
- **Phase 12 — Storage credentials and pairing. Status: not built.** A
  device-code pairing flow against a credential broker, beside the typed keys and
  never instead of them. Designed in §8.5; depends on nothing else here.
- **Phase 13 — Storage provider selection. Status: built.** A provider dropdown
  (Cloudflare R2, AWS S3, Backblaze B2, Wasabi, Custom) that shows only the
  fields each needs and derives the rest, read by all five settings surfaces; a
  saved setup reads back as its matching provider, or "Custom". Phase 12's seam.
- **Phase 14 — LAN / direct delivery. Status: built.** A satellite on the main
  site's network or an existing VPN downloads directly from the encoder,
  LAN-preferred with per-request cloud fallback; cloud upload stays on by default
  but can be switched off entirely. Still not built: anything beyond manual
  host:port discovery (no mDNS — §8.7) and the §8.5 device-code pairing flow.
- **Phase 15 — Lossless high-quality mode. Status: not built.** Opt-in FLAC in
  place of AAC on every track plus ~10 Mbps HEVC, per event not per track, for
  archival rather than broadcast; researched (2026-09-14). No web ingest takes
  FLAC, so only the two purpose-built players can play it back.

### Three things that were on this list

The full reasoning is archived in `docs/scope/project-scope-phases.md`; none of
the three was built, so nothing anyone has is affected.

- **The ABR transcoder ("relay plus")** has moved, not been cancelled: a
  rendition ladder serves an open-internet audience, a different question from
  carrying an event between a church's sites. It is now the foundation of a
  hosted service Stage Audio Works intends to build separately; the relay stays
  here, free and undegraded.
- **End-to-end low latency** is dropped: it inverts §1 (this project's worth
  comes from being allowed to buffer), timeslipping cannot survive it, and it
  would generate support calls on exactly the connections this project exists to
  tolerate. Where conversational latency is needed, SRT already exists in OBS.
- **Appliance hardware beyond the Pi** has moved, not been dropped — a scope
  decision: this project's hardware is the OBS plugin pair and the Raspberry Pi
  appliance (Phase 6) and nothing wider. Stage Audio Works builds and sells
  further hardware and its software under its own name (**MultisiteOS**); where
  it reuses this project's core, that code stays GPLv3, in this repository.

---

## 11. Baseline configuration

- **Retention window:** 7 days (sets how far back campuses can timeslip).
- **Segment length:** 6 seconds default, configurable.
- **Video codecs:** H.264 first; HEVC then AV1 on the roadmap; codec-agnostic
  pipeline.
- **Audio:** every enabled OBS output track is delivered (up to 6), each labelled
  once by the operator; multichannel/surround allowed per track; AAC to start.
  A satellite plays track 1 unless told otherwise, so a stereo-only site needs
  no audio configuration at either end.
- **Pause behaviour:** hold the last frame.
- **First storage target:** Cloudflare R2 (any S3-compatible store supported).
- **Platform order:** Windows first, macOS second.
- **Security:** encoders use write access scoped to their room/event paths;
  decoders use read-only access to the bucket.

---

## 12. Why this exists

This project is developed by the projects team at **Stage Audio Works**, a
worship AVL integrator working across Africa, to support churches that are
growing into multiple locations. Multisite streaming is a solved problem for a
large church in a well-connected part of the world; the commercial platforms
that solve it are good, and largely unavailable or unaffordable where these
churches are.

The full argument — what this is and is not, why it is not a managed event,
why latency is traded for resilience, what it asks of a venue's network, and
the clean-room and licensing position — is in the README, under
[Why this exists](README.md#why-this-exists). It was duplicated here word for
word, which meant two places to keep in step and one of them silently going
stale. This document covers the design; the README covers the case for it.
