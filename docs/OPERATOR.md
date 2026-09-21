# Operator guide

The main campus publishes once; every satellite pulls the same files back down
and plays them out. The twenty-minute version of everything below is
[QUICKSTART.md](../QUICKSTART.md).

## Using it

**In a hurry?** [QUICKSTART.md](../QUICKSTART.md) is the short version of
everything below.

### Installing the plugin

Builds are attached to each [release](https://github.com/stageaudioworks/obs-multisite/releases),
one per platform. All of them are built against the OBS version named in the
release notes; a different major version of OBS may refuse to load them.

**macOS** (Apple Silicon) — unzip, move `obs-multisite.plugin` into
`~/Library/Application Support/obs-studio/plugins/`, then clear the download
quarantine flag before restarting OBS:

```sh
xattr -dr com.apple.quarantine ~/Library/Application\ Support/obs-studio/plugins/obs-multisite.plugin
```

That step is required because these builds are **not code-signed or
notarised**, and macOS refuses to load a quarantined unsigned bundle. What you
see if you skip it is nothing at all: OBS starts normally with no Multisite
source, output or docks, and its log does not say why. Signing is deferred
until there is a stable version to sign.

**Windows** — unzip, then copy the `obs-multisite` folder it contains into
`C:\ProgramData\obs-studio\plugins\`, so you end up with
`C:\ProgramData\obs-studio\plugins\obs-multisite\bin\64bit\obs-multisite.dll`
alongside its own `data\`. This is the layout OBS's own plugins guide
recommends; it needs no elevation, because `ProgramData` is writable without
admin rights, unlike `Program Files`.

> **Upgrading from a release older than v0.1.18-alpha?** Earlier builds were
> staged for the legacy layout — merged into
> `C:\Program Files\obs-studio\obs-plugins\` and `...\data\obs-plugins\`
> alongside OBS's own files. Delete `obs-plugins\64bit\obs-multisite.dll` and
> `data\obs-plugins\obs-multisite\` from inside your OBS install directory
> before installing the new build in its new location, so a stale copy of the
> plugin cannot load instead.

**Linux** — place `obs-multisite.so` in
`~/.config/obs-studio/plugins/obs-multisite/bin/64bit/` with the contents of
`data/` alongside. Links the system FFmpeg and libcurl.

Restart OBS. The encoder appears as an output and the decoder as a source,
with **Multisite Encoder** and **Multisite Decoder** docks under View → Docks.

You need an S3-compatible bucket and a key that can read and write it. For the
decoder's event list the key also needs `s3:ListBucket` — Cloudflare's "Object
Read & Write" token includes it, an object-scoped token does not, and the dock
says so rather than showing an empty list.

### First, a retention rule on the bucket

**Do this once, before your first broadcast.** Automatic expiry is a **bucket
lifecycle rule** you configure in your storage provider's console, and without
one every event you ever broadcast stays in the bucket and the bill grows
without limit. At 6 Mbps that is roughly **2.7 GB per hour** of event.

Two prefixes need a rule, both with the same age:

| Prefix | What it holds |
|---|---|
| `events/` | the media — all of the volume |
| `rooms/` | the per-room event index — tiny, but if it outlives the media the event list fills with recordings that cannot be played |

**Seven days is the design default, and the rule *is* your DVR depth** — a
campus can timeslip back only as far as retention allows, so this setting is
not merely housekeeping.

On **Cloudflare R2**: your bucket → Settings → Object lifecycle rules → Add
rule → prefix `events/`, delete objects 7 days after creation; then the same
for `rooms/`. (`rooms/{room}/live.json` is rewritten on every heartbeat, so it
stays fresh while a room is in use, and ageing out between events is
harmless — the next Go Live recreates it.)

On **AWS S3, Garage, Backblaze B2 or Wasabi**: the equivalent lifecycle
configuration with an Expiration rule per prefix.

You can also delete events by hand, without waiting for the rule: the encoder's
**Manage storage…** button lists the room's events and lets you delete one — or
everything older than a chosen number of days — with a confirmation and a
verification pass afterwards. This needs a key that can `s3:ListBucket` and
`s3:DeleteObject`; the dialog says so plainly if it cannot.

The listing is built to be usable on a bucket holding months of events. The
events appear as soon as the manifests have been read, with no size work in
front of them, and each event's size is then measured beside it — six at a time,
each row filling in as it lands. A size that could not be measured is reported
as unknown rather than as `0 B`, because a zero against an event holding
gigabytes is worse than no figure at all in a window whose whole purpose is
deleting things. Closing the window stops the work: an in-flight request is
aborted rather than left running with nothing watching it. The event on air is
never offered for deletion, and every listing and sizes pass is logged with its
counts and elapsed time, which is what to look at if the dialog appears to be
doing nothing.

**Wanting the storage itself self-hosted, not only self-configured?** Every
provider above still means somebody else's servers. If that matters to you —
data residency, a church that already runs its own infrastructure, or simply
keeping a third party out of the chain entirely — the bucket only needs to
speak the S3 API, so a self-hosted object store works exactly like the cloud
ones above. [Garage](https://garagehq.deuxfleurs.fr/) is one such project: a
single dependency-free binary that runs on modest hardware (1 GB of RAM, any
x86_64 or ARM machine), S3-compatible, with the lifecycle expiration rule
described above. It ships no web interface of its own, but
[Garage Web UI](https://github.com/khairul169/garage-webui) adds one as a
second container — cluster health, buckets, an object browser and key
management — so a volunteer still gets a page to click through. It is a
separate project, and one we have not run this pipeline against ourselves;
nothing here depends on it, the same as any other storage provider on this
page.

MinIO used to be the usual answer here, and an existing MinIO deployment
still works — the endpoint does not care. But its community edition is
end-of-life: the repository was archived in February 2026, official binaries
and images stopped in October 2025, and security fixes are no longer
backported. [RustFS](https://rustfs.com/) is the closest drop-in successor —
the same storage model in Rust, Apache-2.0, with the built-in console MinIO
had — but its lifecycle management is still marked under testing, so it is
not yet a like-for-like replacement for the rule described above.

Object *tagging* is off by default and is deliberately not the mechanism: R2
rejects `x-amz-tagging`, and a tag never deletes anything by itself. Enable it
only if your store expires by tag and you have a rule that matches.

Expiry is passive on purpose. A paused or behind-live campus can still fetch
older segments for the whole retention window, which is what makes deep
timeslipping possible; an encoder that actively deleted as it went would take
that away.

### Main site (encoder)

1. Open the **Multisite Encoder** dock (View → Docks).
2. **Settings…** — enter your bucket details and choose a video encoder, name the
   tracks, give this machine a **Site name** (what appears on the cues this box
   drops), then press **Apply** to commit them. **Cache folder** on the same page
   is where the outgoing queue, downloaded video and the copies served over the
   LAN are kept. Opening the settings and closing them again changes nothing, so
   it is safe to look mid-event.
3. **Go live.** Watch the status readout: how much of the event has been sent,
   how much is waiting, and the **Internet** line (green/amber/red). That line
   is live even before you go on air — the dock checks the bucket every few
   seconds — so a broken connection is visible before it costs you an event.

**Production audio** is set up in OBS itself, not in the dock. In Settings →
Output → Recording, enable the audio tracks you intend to send; in Advanced
Audio Properties (right-click the mixer), assign each source to its tracks — for
example your mix on track 1, mics on their own tracks, a click on another. Name
them under **Settings… → Track labels** so satellites see "Click" rather than
"Track 3".
Every enabled track travels in the same segment, locked to the picture.

Sending stereo only? Do nothing: track 1 is the default at both ends.

### Satellite (decoder)

1. Open the **Multisite Decoder** dock and enter the same bucket details under
   **Settings…**, then press **Apply** to commit them. Everything a satellite
   needs is set once for the machine here — storage, the **feed name** to
   follow, the buffering, and where downloaded video is kept — so every source
   you add afterwards is already configured. A source's own properties dialog
   no longer carries any of these; keeping them in one place is what stops a
   scene's saved copy silently overriding the dock. Opening Settings and
   closing it again changes nothing, so it is safe to look mid-event.
2. Add a **Multisite Source (Decoder)** to a scene.
3. **Follow live**, let the buffer fill, then **Play** when you are ready. The
   state line counts the buffer up against the start gate (*Filling buffer — 23 s
   of 60 s*), so a slow link is visible rather than looking stalled. Use
   **Lock** during the event so nothing can be clicked by accident.

   The decoder holds playback until a whole minute of the event is buffered
   (set in **Settings… → Start after this much is ready**). The buffer fills
   first, then the picture starts — so it does not chase the live edge and
   stall after a single piece on a slow or uneven connection.

   **Cache folder** on the same page is where downloaded segments are kept.
   Leave it blank for the built-in location under OBS's plugin settings, or
   point it at a large or fast disk on a machine that stores a lot.

### A second bucket, so an event is never lost to one provider failing

**Settings… → Second bucket** in the encoder dock turns on redundancy. Both
buckets then receive everything for the whole event — media and the small index
objects alike — because a copy that is only half there cannot be played from.

- **The live feed always wins.** The second copy uploads only while the primary
  has nothing outstanding, so it can never be the reason the broadcast suffers.
  On a thin uplink it simply falls behind and **finishes after the event**, from
  the same local queue: "my link cannot carry both at once" becomes "my link
  takes longer to do both".
- **The dock tells you where it stands**: *up to date*, *N to go*, *N behind —
  the link is busy with the live feed* (which is the design working, not a
  fault), or *not answering*.
- **Test upload speed** sends a measured burst to the second bucket and reports
  Mbps. It uses real bandwidth for a few seconds and is the only way to know
  spare capacity before an event — the live stream only ever produces at its own
  bitrate, so it can never show what is left over.
- **Check the second copy** compares what each bucket holds for the last event
  and names anything missing or different. Two manifests, so it is quick.
- A decoder reads the **primary** and reaches for the second only for an object
  the primary cannot serve — that egress costs money — and if the end it is
  reading stops advancing it moves across and says *via the second bucket*.

Both buckets need their own retention rule. This is not a claim that broadcasting
survives a provider outage: a campus already buffering minutes ahead rides one
out, but one that joins during an outage has nothing to cover the gap.

The **Internet** line in the Status box tells you whether the box can reach the
bucket, separately from whether anything is on air. If it flips to red
("no connection") mid-event, the **Could broadcast for** figure is how long
the picture will keep going from what is already downloaded — enough notice to
act, rather than a surprise when the picture freezes.

To play something other than the live event, use the **Recordings** list:
pick a past event and press **Load recording**. Playback then stays on it —
if a new event starts mid-watch the dock offers the switch rather than taking
it, because being pulled out of a recording you are part-way through is worse
than being told. The same holds for the event you were *following* once it
finishes: a recording that has just ended is not taken away by the next event
starting either. **Back to live** returns to following the room in both cases.

For production audio, the Multisite Source carries the video plus **one** audio
track (track 1 by default). To bring in another track as
well, add a **Multisite Audio Track (Decoder)** source for the same room and
pick the track. It attaches to the decoder already running, so it costs no extra
download: every track arrives in the same segment either way, and all of them
play from one clock.

Hotkeys for play, stop, hold, resume, catch-up, jog and markers are in
Settings → Hotkeys.

> The event list needs the **`s3:ListBucket`** permission. Cloudflare's "Object
> Read & Write" token has it; an object-scoped or read-only token often does
> not, and the dock will say so rather than showing an empty list.

## Cues

A cue is a named moment — "Sermon Start", "Go to local" — that every site can
see and jump to. Cues are set from the **Multisite Cues** dock, which is present
whichever role this machine is, so a cue looks and behaves the same wherever it
was set.

- **Drop a cue** with any name you type. There is no fixed list to choose from:
  a service has no fixed set of moments, so the name is whatever the operator
  says.
- **It lands where you are watching.** A cue goes at the current position, not
  at the live edge — so dropping one while watching a recording puts it where
  the picture is, and a campus sitting behind live means its own position. On a
  recording the times read 00:00 to the end of the event; on a live one they are
  times of day.
- **Jump** to any cue in the list. A cue is jumpable while the part of the event
  it points at is still retained — the same seven-day rule as the recording.
- **Every site sees every cue.** Each carries the name of the site that set it,
  so a cue dropped at another campus is never mistaken for the main site's. The
  list merges the main site's cues with every satellite's, oldest first.
- **A satellite can set one too.** Given storage credentials it writes its own
  cue file, so its key needs permission for that file alone. On a LAN with no
  bucket, the cue is handed to the main site, which writes it — so a campus box
  can set cues with no bucket credentials at all.

Set the box's **Site name** under **Settings…** first (for example "Campus B").
A box with no site name still receives and jumps to cues; it simply cannot set
any, and the dock says so rather than failing quietly.

Cues are ordered by where they fall in the event, not by any site's clock, so a
box whose clock is wrong still places its cue correctly on every other site's
timeline. **Settings…** also warns when this machine's clock is plainly out
against the store's, since that is what would make its clock times read oddly.

## Remote control from a phone

Both docks have a **Remote control** group in their settings. It serves the same
operator interface as the campus player's own — one page, polled twice a second,
in the plain language of an event — on the church network, so the markers can be
pressed from the back of the room and the queue watched from the foyer. The
address to type into a phone is shown in that group, and is selectable.

It is on by default on port **8080**, and it binds every interface exactly as the
appliance's page does, for the same reason: a page that only answers `localhost`
cannot be reached from the tablet it exists for. **There is no password and no
TLS** — the building's own network is the guard. If that is not the trust you
want, switch it off in the same group.

What the encoder's page offers:

- **Go live** and **End the broadcast**, with the same editable event name the
  dock has, pre-filled with the current date and time.
- **A row of cue buttons**, one per cue name this event already has — press one
  to drop that cue in a tap, with the field above for a new name. This is what
  the encoder's old four configured marker buttons were, now the same mechanism
  on both ends and named from the event rather than a settings field.
- A live readout: confirmed pieces, what is waiting to send, retries, bytes
  sent, the measured upload rate, the Cloudflare edge serving the bucket, and
  the last error if there is one.
- **Settings** — the same storage and media fields as the dock, with the secret
  key never shown. Editing storage from a phone is allowed; retyping the key is
  not required, and the field is left as dots to mean "unchanged".
- **Log** — the last few hundred `[multisite]` lines, which is what Help → Log
  Files shows, for somebody who is not sitting at the machine.

**Lock**, in the top bar, stops anything that would change what is on air while
it is on. It is deliberately not remembered across a restart: a lock that
survived one would leave a campus unable to broadcast with no obvious reason
why, and the tablet that set it is long since charged and put away.

Which pages exist follows the machine's **role**: a main site serves the encoder
page and has no decoder routes at all, a satellite the other way round, and a
machine set to Both serves both and links them. On Windows the first start
raises the usual firewall prompt — allow it for private networks, or the page
will not answer from another device.

## Monitoring heartbeat

Both halves of the plugin can report their state to a monitoring collector —
a small status document every 30 seconds while the role is active, every 5
minutes while idle. It is **off by default**: switched off, nothing leaves the
machine — no sockets, no bytes. Switch it on per role, in each dock's Storage
settings under **Monitoring heartbeat**.

**What it sends** is the same status the dock already shows — link health,
how far behind live the campus is, queue and buffer figures, the software
version — plus nothing else. No pictures, no sound, no credentials, no IP
addresses. A failed post is dropped, never queued, so monitoring never
competes with the programme for the venue's link.

**Connecting it** takes a collector URL and either of two credentials:

- **Connect…** (preferred) — asks the collector for a short code, which is
  entered on the collector's own page. The ID and token fill themselves in
  when it is approved there. Apply first, so the typed URL is what gets used.
- **By hand** — type the collector URL, the appliance ID and the token from
  wherever the collector keeps them.

The outcome line in the same box says the last answer: `accepted (200)`,
`disabled`, `not configured`, `not reached (dropped)`, or a named rejection
such as a token the collector does not know. A campus player reports the same
way from its own settings page, and additionally sends the box's health — CPU
and memory load, free disk on the cache drive, temperature and throttle
flags — since a stuttering box and a struggling link look identical from the
pew and are different faults.

## Control from a Stream Deck or automation

A volunteer running an event reaches for a physical button, not a window. Two
ways to give them one, and both are live in the plugin.

**Hotkeys, today.** Play, stop, hold, resume, catch-up, jog and markers are
registered in Settings → Hotkeys. Companion's OBS module can trigger a hotkey by
id, so they work on a Stream Deck now — but a hotkey carries no parameters and
shows no feedback, so "jog back ten seconds" is a whole action rather than a
choice, and the button cannot light up while an event is live.

**The obs-websocket API.** Every control the pages offer is also an
obs-websocket **vendor request**, so any obs-websocket client — Companion's
*Custom Vendor Request* action, a script, another automation system — can call
them. These are the same commands as the HTTP routes, under the vendor
`obs-multisite`:

| request | what it does |
|---|---|
| `encoder/status`, `decoder/status` | the same document the page polls |
| `encoder/go-live` | go live, with an optional `event_name` |
| `encoder/end`, `encoder/marker` (`label`) | end the broadcast; drop a cue at the main site |
| `encoder/settings`, `decoder/settings` | read, and apply a partial document |
| `decoder/play`, `stop`, `hold`, `continue`, `catch-up` | the transport controls |
| `decoder/jog` (`seconds`), `decoder/seek` (`ms`), `decoder/delay` (`seconds`) | navigate |
| `decoder/marker` (`id`), `decoder/cue` (`label`), `decoder/load-event` (`event_id`), `decoder/return-to-live` | cues and recordings |
| `decoder/events`, `decoder/events/refresh` | the recording list |

Each request answers with the new status, so a client never has to guess what its
own button did, and a refusal says why in an `error` field. Alongside the
requests, the plugin emits **events** — `encoder/state` and `decoder/state` —
whenever something an operator cares about changes, which is what lets a
Companion module light a button or show "12 s behind".

Nothing changes for a church that has not turned obs-websocket on: the plugin
logs one line and carries on. obs-websocket ships with OBS 28 and later and is
enabled in Tools → WebSocket Server Settings; there is nothing separate to
install.

**The ready-made module.** If you would rather not build the buttons by hand,
there is one:
[companion-module-obs-multisite](https://github.com/stageaudioworks/companion-module-obs-multisite)
is a Bitfocus Companion module carrying every action above, the feedbacks that
light a button while an event is live or a campus is held, behind live or
offline, the variables, and a preset bank for each half. It asks for the host,
port and password a second time, because each Companion module opens its own
connection.

> The decoder requests are these same names with `decoder/` in front, and the
> appliance's own routes (`/api/play`, `/api/hold` and the rest) are the same
> actions one machine further out. A control written against one is a small edit
> away from the other; the two leaf names that still differ — the plugin's
> `return-to-live` and `load-event` against the appliance's `follow-live` and
> `load` — are noted in the scope to be brought together.


