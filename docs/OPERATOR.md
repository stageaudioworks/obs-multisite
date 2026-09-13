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

On **AWS S3, MinIO, Backblaze B2 or Wasabi**: the equivalent lifecycle
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
speak the S3 API, so a self-hosted object store works exactly like MinIO
does above. [Alarik](https://github.com/achtungsoftware/alarik) is one such
project: S3-compatible with lifecycle rules included, and it runs on hardware
you own — on premises, at a colo, wherever. It is a separate project, in
beta, and one we have not run this pipeline against ourselves; nothing here
depends on it, the same as any other storage provider on this page.

Object *tagging* is off by default and is deliberately not the mechanism: R2
rejects `x-amz-tagging`, and a tag never deletes anything by itself. Enable it
only if your store expires by tag and you have a rule that matches.

Expiry is passive on purpose. A paused or behind-live campus can still fetch
older segments for the whole retention window, which is what makes deep
timeslipping possible; an encoder that actively deleted as it went would take
that away.

### Main site (encoder)

1. Open the **Multisite Encoder** dock (View → Docks).
2. **Settings…** — enter your bucket details, choose a video encoder, name your
   markers. Settings are saved as you type.
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
   **Settings…**. These are stored per machine, so every source you add
   afterwards is already configured.
2. Add a **Multisite Source (Decoder)** to a scene.
3. **Load event**, let the buffer fill, then **Play** when you are ready. Use
   **Lock** during the event so nothing can be clicked by accident.

   The decoder holds playback until a whole minute of the event is buffered
   (set in **Settings… → Start after this much is ready**). The buffer fills
   first, then the picture starts — so it does not chase the live edge and
   stall after a single piece on a slow or uneven connection.

The **Internet** line in the Status box tells you whether the box can reach the
bucket, separately from whether anything is on air. If it flips to red
("no connection") mid-event, the **Could broadcast for** figure is how long
the picture will keep going from what is already downloaded — enough notice to
act, rather than a surprise when the picture freezes.

To play something other than the live event, use the **Recordings** list:
pick a past event and press **Load recording**. Playback then stays on it —
if a new event starts mid-watch the dock offers the switch rather than taking
it, because being pulled out of a recording you are part-way through is worse
than being told. **Back to live** returns to following the room.

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
- The four **marker** buttons, named in Settings.
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
| `encoder/end`, `encoder/marker` | end the broadcast; drop a marker (`label`) |
| `encoder/settings`, `decoder/settings` | read, and apply a partial document |
| `decoder/play`, `stop`, `hold`, `continue`, `catch-up` | the transport controls |
| `decoder/jog` (`seconds`), `decoder/seek` (`ms`), `decoder/delay` (`seconds`) | navigate |
| `decoder/marker` (`id`), `decoder/load-event` (`event_id`), `decoder/return-to-live` | markers and recordings |
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


