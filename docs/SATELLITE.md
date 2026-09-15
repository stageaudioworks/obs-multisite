# Choosing a satellite

A campus can receive in one of two ways, and they suit different rooms.

**OBS on a PC** — the decoder is a *source in a scene*, so the campus can
produce around the relayed event. **The Pi appliance** — a fixed-function box
that plays the event and nothing else. Both are built; neither has yet run a
real event.

### What running the decoder in OBS makes possible

Because the relayed programme is an ordinary source, everything OBS does applies
to it. This is the reason to choose a PC over the appliance, and for many
churches it is the deciding factor.

**Local content over the relayed event**

- Lower thirds, campus announcements, scripture graphics, a countdown before the
  event, a logo bug — keyed over the incoming picture with OBS's normal
  sources and filters.
- Cut away entirely to a local camera for a campus host, a local worship set or
  notices, then back to the relay. The decoder keeps downloading while it is off
  screen, so returning does not mean re-buffering.
- Record the campus feed locally and simulcast it to YouTube or Facebook at the
  same time as it plays in the room.

**Video in and out**

- **Blackmagic DeckLink** and **AJA** are supported by OBS itself, in and out.
  A campus can take SDI to the house system and bring SDI in from a local
  camera on the same machine.
- **NDI** in and out through the DistroAV plugin (formerly obs-ndi), where the
  house system already runs NDI.
- Anything else OBS can see: HDMI capture cards, USB cameras, screen capture.

**More than one camera angle, in guaranteed sync**

The protocol carries exactly one video stream per event — there is no
equivalent of multi-track audio on the video side, and adding one would be a
much bigger change than audio tracks were, which just reuse the fragment
multiplexing that was already there. So a room that needs several angles
delivered to a satellite — a wide shot and a stage-left ISO, say, or four SDI
sources for a video wall — has to get them there some other way.

Our recommendation: **composite the feeds into one canvas at the main site**
before they ever reach the encoder, and split them back apart at the
satellite. Two SDI inputs side by side make a 3840×1080 canvas; four in a
grid make 3840×2160. That single wide frame is the one thing this pipeline
sends and guarantees in sync — every camera is a region of the same decoded
picture, so there is no possibility of the angles drifting apart the way
independently-encoded streams could.

At the decoder machine, add the Multisite Source once and pull the angles
back apart onto their own outputs with OBS's own **Transform** and **Crop/Pad**
filters — one scene item per angle, each cropped to its region of the
composite and routed to wherever it needs to go (a DeckLink output, a
separate program feed, a video wall processor). Nothing here needs a plugin;
crop and transform are built into OBS.

> **Partly built.** A tile layout declared at the main site (`1x1`, `2x1`,
> `2x2`) now exposes each region as its own source, already cropped, so a 2×1 or
> 2×2 feed can be routed to its own output instead of being cut up with filters.
> The headless campus player does the single-screen half of it: a **Composited
> feed** setting picks which region a box shows, in reading order, with the whole
> picture as the default. Assigning tiles to *several* outputs from one box needs
> hardware beyond the Pi appliance, which is outside this project's scope — see
> ["Appliance hardware beyond the Pi"](../PROJECT-SCOPE.md#10-delivery-phases).
> The filters below are how to do it in OBS.

The cost is bandwidth: a 3840×2160 canvas costs roughly what a single 4K
stream does, whatever number of cameras are inside it. Worth it for a room
that needs several angles kept in lock-step; not worth building for a room
that only ever needs one.

**Audio into the house system**

- **Dante** via Dante Virtual Soundcard or a Dante-enabled interface: OBS sees
  it as a normal output device, so the relayed programme lands on the Dante
  network alongside everything else the church already runs. The same approach
  works for AES67/AVB interfaces, USB interfaces, or an analogue break-out.
- Audio leaves OBS through its monitoring device, so whichever interface the
  room uses is the one to select there.

Multi-track audio is what makes that practical: each track is a separate source
in OBS, so the main mix can go to the house system while the click goes to
in-ears, routed independently like any other source.

One caveat worth knowing before planning around this: every third-party plugin
named above is someone else's project, on its own release schedule.

If the main site sends *packed* multi-channel rather than separate tracks, the
channels arrive as one stream and something has to route them to their
destinations. That is not built here, deliberately — it is a solved problem in
OBS. [atkAudio's plugin suite](https://github.com/atkAudio/PluginForObsRelease)
hosts VST3/AU/LV2 plugins, mixes OBS sources, and routes audio to ASIO,
CoreAudio and Windows Audio devices, which covers channel mapping better than a
narrow de-interleaver of our own would. It is a separate install under the
AGPL-3.0 licence and nothing here depends on it; a packed feed carries eight
channels through this pipeline with the channel order intact either way.

### Any location can be the origin

Both plugins are one module, so any machine running OBS can take either role.
What originates an event is a laptop with OBS on it, so a broadcast can start
anywhere someone can run it:

- a guest speaker or travelling pastor, publishing from wherever they are;
- a conference or camp venue, for a week, and then never again;
- a second campus hosting this week's combined event, with the usual main
  site receiving for once;
- a temporary or overflow site set up at short notice.

Adding an origin costs a room name and a key that can write to it. There is no
hardware to specify a year ahead, nothing to ship or clear through customs, and
nothing licensed per location — which matters most in exactly the places this
project is for.

The reliability argument is *stronger* for an occasional origin than for a
permanent one. A speaker broadcasting from a hotel, a phone hotspot or a venue
nobody surveyed has the worst connection anyone in the chain will have, and can
least afford a dropout halfway through a sermon. Because segments are written to
disk and resent until storage confirms them, that broadcast survives a link
which would kill a direct stream — it arrives whole or visibly incomplete, never
broken in the middle.

The latency rule is unchanged: tens of seconds each way means this relays a
event, it does not hold a conversation between sites.

Keep rooms separate — a guest publishes to `guest-speaker`, not to
`main-auditorium` — so an occasional broadcast can never be mistaken for the
main programme.

### When the appliance is the better answer

The appliance gives all of that up on purpose. No scene, no overlays, no local
sources: it plays the relayed event, on a box that costs less than a monitor,
boots into the event on power-up, and is driven from a phone with no desktop
to leave in the wrong state.

Choose it where a campus needs the event on a screen and nothing more — an
overflow room, a chapel, a plant meeting in a school hall. Choose OBS where the
campus produces around the relay, or where it has to reach existing SDI, NDI or
Dante infrastructure.

---

## The campus player (satellite appliance)

The alternative to running the decoder in OBS: a small box at a campus that
receives, decodes and plays out, with no operator-facing desktop software. See
[Choosing a satellite](#choosing-a-satellite) for which suits a given room. On
stock **Raspberry Pi OS Lite (64-bit)**:

```bash
curl -fsSL --retry 5 https://raw.githubusercontent.com/stageaudioworks/obs-multisite/main/scripts/player/install.sh | sudo bash
```

That installs the dependencies, builds the player, installs it as an event
that starts on power-up, and puts a screen up on the HDMI output showing the
box's own address and a QR code of it: point a phone at the screen and the
control page opens, nothing to type. Everything else is done from a phone or
tablet on the same network — storage credentials, which room to follow, the
output resolution and frame rate, the sound device, the clock, and the
transport controls during an event.

- **It owns the display.** The player sets the KMS mode itself, so the output
  resolution and frame rate are exactly what was asked for and there is no
  desktop to be left in the wrong state. Pi OS Lite is the right image.
- **Production audio over HDMI.** Up to eight channels of LPCM, recovered at
  the campus with a de-embedder. If the device will not take every channel the
  feed carries, it says so loudly rather than silently dropping the click. This
  is the one place *packed* multi-channel is the better mode: eight channels in
  one stream map straight onto HDMI's eight, in order, with nothing to route.
  An appliance fed multi-track plays one chosen track — the first by default,
  with a picker in Settings; combining several tracks onto output channels
  there is not built, and packed is the answer for a campus that needs more
  than one.
- **Hold beats the idle screen.** `idle_mode` says what the box shows when
  there is nothing to play — the box's own details, black, or a holding slide.
  Pressing **Hold picture** is not that: it is an operator asking for the frame
  in front of them to stay, so the held frame stays up whatever `idle_mode` is
  set to, and changing the idle screen mid-event cannot replace a picture that
  was deliberately frozen. **Stop** and waiting for the main site are the
  deliberate acts the idle screen is for, and both still show it. On
  `idle_mode: "hold"` with nothing ever decoded there is no frame to hold, so
  the identity screen comes up rather than a blank nobody can explain.
- **The preview is a copy, not a second output.** The web UI shows the picture
  going out, refreshed at a rate the browser chooses. Watching it does not
  change or interrupt what is on the screen in the room — it is the same moment,
  sampled a few times a second. There is one playhead, so the preview cannot
  look ahead of the picture it mirrors. The one place it can differ from the
  screen is the crop: with a `tile_index` set the screen shows one region, so
  the interface offers both — *what's going out* (that region, and the whole
  picture when no tile is selected) and *the whole feed* (everything the box
  received, tiles and all). There is a fallback frame per view, so switching
  between them never blanks the picture.
- **The cache belongs on a USB SSD.** It writes roughly 3 GB an hour, which
  will wear an SD card out. The installer looks for a USB drive and uses it;
  if there is none, both it and the interface say so.
- **Remote access, so the box does not need a visit.** What makes a wrong
  setting at a campus expensive is that fixing it means driving there. The
  installer brings up two optional tools and either can be changed later from
  Settings → Remote access. **ZeroTier** puts the box on a private network that
  follows it, so it is reachable from the office wherever it is plugged in:
  pass `ZT_NETWORK_ID=…` to the installer or type it at its prompt. **cloudflared**
  publishes this control page on a public hostname with no port-forward and no
  static address: pass `CF_TUNNEL_TOKEN=…`. The box's ZeroTier address is put on
  its own screen, labelled **REMOTE ACCESS IP** and kept well apart from the
  in-room addresses — those are typed into a phone standing in the building,
  this one is not, and confusing the two is the mistake worth designing out.
  Neither tool is required to play an event; a box with neither says nothing
  about remote access and behaves exactly as before.

Run it by hand while setting one up:

```bash
sudo multisite-player --config /etc/multisite-player/config.json --verbose
```

`journalctl -u multisite-player -f` is the whole diagnostic story; the last few
hundred lines are also in the interface, under Log, for an operator with a
phone and no SSH.

### AES67 audio on the network

Out of the box the sound leaves on the HDMI socket with the picture, so it
reaches whatever is plugged into the Pi and nothing else. A campus that wants
the sound on its own console — a separate feed, its own level control, working
whether or not a screen is attached — needs it on the network, and on a church
network that means AES67. `scripts/player/merging-aes67.sh` installs an open
AES67 stack so the player's audio arrives as a stream instead of staying inside
the picture: Merging's `ravenna-alsa-lkm` kernel module, which registers an
ordinary ALSA sound card, and the GPL `aes67-daemon` that talks to it and does
RTP, SDP/SAP and PTP. Merging's own daemon ("Butler") is the amd64-only licensed
part; this one stands in for it, which is why it runs on a Pi:

```bash
sudo bash scripts/player/merging-aes67.sh
```

On a box that has never seen this repository, fetch the script the way the
player installer is fetched:

```bash
curl -fsSL https://raw.githubusercontent.com/stageaudioworks/obs-multisite/main/scripts/player/merging-aes67.sh \
  | sudo bash
```

It builds from source, so give it a few minutes and let it finish.

- **One run does all of it.** The script installs the build dependencies, builds
  the kernel module for the running kernel, builds the daemon, installs both with
  a systemd unit, writes `/etc/daemon.conf`, and arranges for the module to load
  at boot. It is safe to run again: it reuses an existing build tree, and an
  existing `/etc/daemon.conf` is left alone unless you pass `--rewrite-config`.
  `--check` reports what is already on the box and changes nothing, which is the
  first thing to run on a machine you did not install.
- **The player is moved onto the card separately.** The installer leaves the
  player alone unless you ask, so an install can be proven before anything the
  congregation sees is touched. When the daemon's WebUI reports the clock
  `locked`, move it with:

  ```bash
  sudo bash scripts/player/merging-aes67.sh --point-player
  ```

  This sets the player's `alsa_device` to `plughw:CARD=RAVENNA` and restarts it.
  It is an ordinary ALSA card: the player needs no special handling, and the card
  grants a normal buffer, so the `sound has broken up` under-runs of the earlier
  virtual-card route do not occur.
- **A PTP master has to exist on the network, or nothing flows.** The daemon
  slaves to a clock; it does not hand one out. With no master — a Dante device, a
  console, an Anubis — the WebUI never says `locked` and there is silence. This
  is the most likely reason for quiet after a clean install, and it is a network
  question rather than a fault in the install. The WebUI is at
  `http://<pi>:8081` — 8081, not the project's 8080, because the player's own
  interface already uses 8080 on this box.
- **A forwardable stream: 8 channels, multicast, 1 ms packets.** Those are the
  AES67 defaults the daemon announces. The stream this box publishes is created
  by the installer and controlled from the player's own page, above; the daemon's
  configuration and its WebUI are where the rest of the daemon's settings live —
  a second stream, a sink, a different clock domain — rather than the everyday
  switch.
- **Dante.** The source appears in Dante Controller, but the route from it to a
  receiver is made by hand in that application. Test against the real Dante
  hardware a site will use.
- **A kernel upgrade means rerunning this script.** The module is built from
  source against the running kernel and is deliberately *not* put through DKMS,
  because its build takes a branch of the submodule and a compiler choice a DKMS
  hook cannot reconstruct reliably. Rerun after an upgrade if the kernel moves.
- **What is verified, and what is not.** On a bench Pi the module built, the
  daemon came up, the card appeared, the player opened it, and eight channels of
  clean audio arrived. Not yet verified: that the picture and the sound stay
  together across a two-hour service, and how accurate PTP becomes, since a Pi's
  network interface does no hardware timestamping and the result is whatever the
  software manages. Measure both at the receiver, on a real event — ten seconds
  of test tone cannot settle either. The detail is in
  [BUGS.md entry 3](../BUGS.md#3-aes67-audio-works-on-the-bench-unproven-over-an-event).

### Controlling it from the player's own page

The installer sets up one stream as part of the install — eight channels, L24, at
the multicast address named in `/etc/daemon.conf` — so a box that has just been
set up is already sending, without anybody opening an interface. The player then
keeps that stream in shape and gives it a switch, which leaves the daemon's own
WebUI for the rest of the daemon's settings rather than for the everyday one.

Two places in the player's page (port 8080, the usual one):

- **Settings → Network audio output.** The switch, the multicast address, and the
  channel count. One button applies all three, because they are one decision:
  switching it on starts the daemon, sets it to come back after a power cut, and
  creates or corrects the stream. Switching it off *stops* the stream rather than
  deleting it, so the address and width are kept and switching it back on is one
  click and not a re-entry of everything. It sits behind **Lock**, like the other
  settings that change what is on air.
- **This box → Network audio output.** What is actually being sent, read back
  from the daemon rather than assumed from the settings: whether the daemon is
  running, whether the clock is locked and to which grandmaster, the address and
  port on the wire, and the SDP it publishes — which is the thing a console's
  engineer will ask for. Anything that would stop the audio is said in a sentence
  underneath, including the states that look identical from the settings and are
  not: a stream that is configured but switched off, and a stream that is carrying
  nothing because the player is writing the sound somewhere else.

**What the switch does, which is more than it first looks.** It *moves the
sound*. There is one output device on this box, not two, and the daemon publishes
what is written to the AES67 card — so putting the sound on the network means the
player has to be writing to that card, and turning it off means putting the sound
back where it was. That is why the output device picker under **Sound** is greyed
out while this is on: letting the two disagree produces the worst of the
available failures, a stream that looks healthy and carries nothing.

What it cannot do is lock the clock for you — if the page says the clock is not
locked, the fault is on the network, not on this box.

**If the AES67 stack is not installed**, both places say so rather than offering
a switch that would only fail. Install it with the script above and they fill in.

### Hearing what is leaving the box

The meters under the Play tab, and the figures beside them, are taken from the
one place that decides everything above: **where the samples are handed to the
sound card.** That is deliberate, and it is the difference between a meter that
answers a question and one that answers the wrong one. A meter of the decoded
feed says an event is carrying sound; it cannot tell you whether the box is
putting it anywhere. Every fault this box has — a card that would not open, a
mute somebody forgot, a stream that is off the air while the feed is perfectly
healthy — is invisible on a meter of the feed and obvious on a meter of the
output.

So the bars fall to nothing, and the reason underneath says which of these it is:

| Reason | What it means | What to do |
|---|---|---|
| `playing` | Card open, sound switched on, frames arriving with signal in them | Nothing |
| `feed-silent` | Frames arriving, but there is no signal in them | The event carries no sound — a muted microphone upstream, or a track that was never in the feed |
| `idle` | Card open and being fed, but nothing is being delivered | Nothing, if the box is stopped, held or between events |
| `muted` | Switched off in Settings | Switch it back on under **Sound** |
| `card-closed` | The card is not open | A fault: the reason it would not open is beside it in the Sound readout |

Two of those are worth sitting with, because they look the same and are not.
**Muted** and **card-closed** are both a flat meter; the first is somebody's
decision and the second is a fault, and the words say which. Likewise a flat
meter with frames still arriving is the *event*, not the box — the appliance is
doing exactly what it was asked to do, and the fix is at the other end.

Muting writes silence to a card that stays open rather than closing it. On a box
whose sound leaves over the network that is not a nicety: a closed card takes the
stream off air and receivers drop it, and un-muting does not get it back until
they re-subscribe. Muted means *a stream that is up and carrying silence*, which
is what a mute should sound like, and the meters fall for it because they are
reading what the card was given.

The panel is drawn at the width of the **card**, not the feed. A stereo card fed
a six-track feed shows two bars, because two is what is leaving the box — four
bars of a signal nobody can hear would be worse than showing nothing.

### Uninstalling the player

To take the player back off a box, run the mirror of the installer:

```bash
sudo bash scripts/player/uninstall.sh
```

or, on a box with no checkout, fetch it the way the installer is fetched:

```bash
curl -fsSL https://raw.githubusercontent.com/stageaudioworks/obs-multisite/main/scripts/player/uninstall.sh \
  | sudo bash
```

It stops and disables the service and removes its unit, the program, the web
page, the checkout and its build tree, the settings and the downloaded event —
including a cache the installer put on a USB SSD rather than under
`/var/lib/multisite-player`. Every step looks before it acts and the run ends by
checking the same list again, so a box that is already clean is left alone.

- **Look before you leap.** `--check` reports what is on the box and changes
  nothing; `--dry-run` prints every change and makes none. Both are safe to run
  anywhere, including a laptop, which is where this wants to be read through
  first.
- **Keeping a box for a reinstall.** `--keep-config` removes the software but
  leaves `/etc/multisite-player` and the downloaded event; `--keep-cache` keeps
  only the event.
- **Remote access and AES67 are decisions, not defaults.** A box can keep either
  after the player has gone — the tunnel may still be how it is reached, and the
  sound may still be on the network — so neither is touched unless asked.
  `--purge-remote-access` removes ZeroTier and cloudflared; `--purge-aes67`
  removes Merging's kernel module and `aes67-daemon`, their configuration and
  the build tree, and puts PulseAudio back. The legacy Digisynthetic stack is
  never touched here; that is
  [`purge-digisyn.sh`](../scripts/player/purge-digisyn.sh).
- **Back to stock in one command, with the way in left intact.** `--stock` is
  the whole job asked once: the player, the AES67 stack under it, and the
  packages that existed only to build the two of them. ZeroTier is the
  exception, deliberately — it is a remote-access tool rather than part of the
  player, and a box that has just been cleared with nobody able to log into it is
  worse than one that was never cleared. So a run that leaves it does not merely
  abandon it: it checks the daemon is enabled and running, starts it if it is
  not, and prints the address and the networks the box is on. Combining
  `--stock` with `--purge-remote-access` is refused rather than obeyed, because
  that pair means *clean this box and lock me out of it*.

## Remote control from a phone

The decoder in OBS serves the same page the appliance does — play, hold, catch
up, jog, stay behind live, the recordings list, and the readout that says how
long this campus could keep playing through an outage. It is configured in
**Settings → Remote control** in the dock, which shows the address to type into
a phone, and is on by default on port **8080**.

The page reaches the same controls the hotkeys use, so a page and a keypress
cannot disagree about what they did. **Lock** in the top bar refuses anything
that would change what is on air, for the tablet left on a music stand; the
dock's own Lock is shown as well, because "why will this not respond" has two
different answers.

There is no password and no TLS — the building's network is the guard, exactly
as for the appliance's page. Switch it off in the same group if that is not the
trust you want, and allow it through the Windows firewall on private networks
the first time, or nothing else on the LAN will reach it.
