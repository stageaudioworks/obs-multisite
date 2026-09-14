# Quick start

Multisite church streaming through storage you own, as a pair of OBS plugins.
The main site uploads once; each campus pulls the same files back down and
plays them out — no subscription, no dedicated encoder or decoder hardware, no
inbound ports at any site.

> **This is alpha software.** A six-hour soak test has run end to end several
> times, with consistent results each time, and the Raspberry Pi campus
> player is working on real hardware, but **it has not yet carried a real
> congregation's event.** If you put it in front of one, do it with a tested
> fallback and a technical person on hand. See
> [Known gaps](README.md#known-gaps) before you plan around it.

## What you need

- **OBS Studio 32.2.2** at the main site and at each campus.
- **An S3-compatible bucket.** Cloudflare R2 is the assumed default — it
  charges no egress, which is what makes sending the same event to several
  campuses affordable.
- **An API token that can read *and* write the bucket**, including
  `s3:ListBucket`. Cloudflare's "Object Read & Write" token has it; an
  object-scoped token does not, and the recordings list will tell you so
  rather than showing you an empty list.
- Roughly **2.7 GB of storage per hour** of event at 6 Mbps.

## 1. Install the plugin

Download from [Releases](https://github.com/stageaudioworks/obs-multisite/releases)
and install on the main site and every campus.

- **Windows** — copy the `obs-multisite` folder from the zip into
  `C:\ProgramData\obs-studio\plugins\`.
- **macOS** (Apple Silicon) — move `obs-multisite.plugin` into
  `~/Library/Application Support/obs-studio/plugins/`, then **clear the
  quarantine flag or macOS will silently refuse to load it** — these builds are
  not signed yet:
  ```sh
  xattr -dr com.apple.quarantine ~/Library/Application\ Support/obs-studio/plugins/obs-multisite.plugin
  ```
- **Linux** — put `obs-multisite.so` in
  `~/.config/obs-studio/plugins/obs-multisite/bin/64bit/` with `data/`
  alongside.

Restart OBS. You should see **Multisite Encoder** and **Multisite Decoder**
under View → Docks.

## 2. Set a retention rule — do this before your first broadcast

**Nothing in this project deletes anything.** Without a lifecycle rule, every
event you ever broadcast stays in the bucket for ever.

In your storage provider's console, add a rule for the prefix `events/` and
another for `rooms/`, both deleting objects after the same number of days.
Seven days is the design default — and **the rule is also your DVR depth**, so
a campus can rewind only as far as retention allows.

## 3. Main site

1. Open the **Multisite Encoder** dock → **Settings…**
2. Enter the bucket details and pick a video encoder. Choose a **feed name** —
   every campus will type the same one. Settings save as you type.
3. Press **Go live.** The dock shows how much has been sent, how much is
   waiting, and whether the link is healthy.

If the internet drops, the encoder keeps recording to disk and sends the
backlog when it returns. Nothing is lost and nothing is skipped.

**Stereo only? You are done.** For more than stereo — a mix, mics on their own
tracks, a click for the band — enable the tracks in OBS's own
Settings → Output → Recording, assign
sources to tracks in Advanced Audio Properties, then name them under
**Settings… → Track labels** so the campus sees "Click" and not "Track 3".

## 4. Each campus

1. Open the **Multisite Decoder** dock → **Settings…** and enter the same
   bucket details. These are stored per machine, so any source you add later is
   already configured.
2. Add a **Multisite Source (Decoder)** to a scene and enter the same feed
   name.
3. Press **Load event**, let the buffer fill, then **Play** when you are ready.
4. Press **Lock** for the event so nothing gets clicked by accident.

Each campus runs its own clock: **hold** the picture for a local welcome,
**resume**, then **catch up to now** — or stay a set number of minutes behind
live all event. What one campus does has no effect on any other.

To play a past event instead, pick it from the **Recordings** list. To add an
ISO or the click, add a **Multisite Audio Track (Decoder)** for the same feed —
it costs no extra download, because every track already arrives in the same
segment.

## Then, if you want it

- **Stream to the public** without uploading twice — the relay container in
  [`relay/`](relay/README.md) reads the same files and pushes to YouTube,
  Facebook or any RTMP destination.
- **Download a finished event** as one MP4, with every audio track.
- **Control either side from a phone.** The encoder and decoder docks serve the
  campus player's own operator page on your church network — Go live and the
  markers on the sending side, play, hold, jog and the recordings list on the
  receiving side. The address is in the dock under **Settings → Remote
  control**; port 8080 unless you change it. No password: the building's
  network is the guard.
- **Run a campus without a PC** — a Raspberry Pi 5 appliance with HDMI output
  and a browser control panel:
  ```sh
  curl -fsSL --retry 5 https://raw.githubusercontent.com/stageaudioworks/obs-multisite/main/scripts/player/install.sh | sudo bash
  ```

## If something is wrong

Check the OBS log (Help → Log Files → Show Log Files) — both plugins log what
they are doing under `[multisite]`, including why they refused something.

A good bug report is a real contribution: much of what works well here was
fixed because somebody took the time to paste a log.

---

Website: [stageaudioworks.github.io/obs-multisite](https://stageaudioworks.github.io/obs-multisite/)
· full documentation: [README](README.md) · design and protocol:
[PROJECT-SCOPE](PROJECT-SCOPE.md) · GPL-3.0-or-later.
