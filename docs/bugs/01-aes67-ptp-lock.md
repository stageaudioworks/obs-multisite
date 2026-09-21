# BUGS #1 — AES67 audio and PTP lock: full record

Archive of the original entry. The short entry is in `BUGS.md`. Read the short
one first; this holds the detail and the installer history.

> Original entry, as it stood on 2026-09-21.

---

### 1. AES67 audio: proven over an event, PTP lock accuracy at a receiver is not

**Status: eight channels of clean AES67 audio, proven on a bench Pi and since
run through a multi-hour test — picture and sound watched and listened to
together the whole way through, no drift found. Still unmeasured: the PTP
lock accuracy a Pi's network interface can actually hold, checked at a
receiver over that same length of time.**

The lip-sync half of this is not really an AES67 question: audio and video
are scheduled off the same delivery-queue clock and the same first-frame
anchor (`Player::anchor_pts()`, `src/appliance/player.cpp`) regardless of
which card the sound goes out on, so the two cannot drift apart from each
other whether the output is AES67, HDMI or USB. What the multi-hour run
actually proves is that this holds under sustained real load, not that
playback is slaved to the AES67 card's own PTP-disciplined sample clock —
that clock is the RAVENNA driver's doing, real and relevant to a *receiving*
console's own sync to the network, but external to this project entirely.

`scripts/player/merging-aes67.sh` installs an open AES67 stack: Merging's
`ravenna-alsa-lkm` kernel module, which registers a virtual ALSA card, plus the
GPL `aes67-daemon` from `aes67-linux-daemon` that talks to that module over
netlink and stands in for Merging's own Butler — the part that is amd64-only and
licensed, and the reason an open build runs on a Pi at all. The daemon does RTP,
SDP/SAP and PTP itself, and lets a stream be aimed at a chosen multicast address,
port and channel map. It has now been run on a bench Pi: the module built against
the running kernel, the daemon came up, the card appeared, the player opened it,
and eight channels of clean audio arrived. (An earlier route used a licensed
virtual sound card, which also played eight channels on the bench but pinned an
eight-millisecond buffer and could not be aimed at a chosen destination.) The
operator-facing version of this is
[docs/SATELLITE.md](../SATELLITE.md#aes67-audio-on-the-network).

The stream itself is no longer something an operator has to build in the daemon's
own interface. `merging-aes67.sh` creates one as part of the install — eight
channels, L24, at the multicast address its own configuration names — and the
player then keeps that source in shape and switches it: **Settings → Sound on the
network** for on/off, the address and the width, and **This box → Sound on the
network** for what is actually being sent, read back from the daemon. The
player's page is therefore the everyday route, and the daemon's own WebUI on 8081
is for the rest of what the daemon can do.

**What is actually left, in order:**

1. **A PTP master must exist on the network.** The daemon slaves to a clock; it
   does not hand one out. With no master — a Dante device, a console, an Anubis
   — it never reports "locked" and no audio flows. This is the most likely
   reason for silence after a clean install, and it is a network question rather
   than a fault in the install.
2. **PTP lock accuracy, measured at the receiver, not the Pi.** Lip sync over
   a multi-hour run is now settled (see the status line above). What is not:
   how well PTP itself holds — AES67 wants both ends within a millisecond,
   and a Pi's network interface does no hardware timestamping, so the
   achievable accuracy is whatever the software manages. That needs a
   receiving console's own read on it, over the same length of time. The Pi's
   own side of that comparison is now recorded (see below).
3. **Dante routing is by hand.** A source shows up in Dante Controller, but
   connecting it to a receiver is a manual step in that application.
4. **A kernel upgrade means rerunning the installer.** The module is built from
   source against the running kernel and is not put through DKMS, because its
   build takes a branch of the submodule and a compiler choice a DKMS hook
   cannot reconstruct reliably. Rerunning the script rebuilds it.

**Since this was written:** the Pi's half of that comparison is now recorded,
where before it was only readable live in the daemon's own WebUI. The 60-second
status line gains ` ptp=locked 12.3ns` (or `UNLOCKED`) whenever the AES67 probe
has read `/ptp/status`, and the box's own page shows the same jitter beside the
grandmaster it locked to. This does NOT answer point 2 — the number that decides
it is the *receiver's*, so a console's read over a long run is still what closes
this — but a full-length service now leaves a per-minute trace of how tightly
the Pi held the clock, so the two ends can be compared after the fact instead of
the question resting on nobody having looked. `src/appliance/player.{h,cpp}`,
`src/appliance/web/app.js`.

**Next step:** a full-length service on the picture and the sound together,
which is point 2 above — capturing the `ptp=` trace from the Pi's journal and
the receiving console's own lock figure over the same run.
