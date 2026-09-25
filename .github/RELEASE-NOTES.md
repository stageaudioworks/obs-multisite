## ⚠️ Alpha — read this first

This is pre-release software. A six-hour continuous soak test has been run end
to end several times, with consistent results each time — the first run's own
numbers were 3,661 segments, over 15 GB, zero retries and zero upload
failures, 10 lagged frames in 658,837 — but it has still not carried a real
congregation's event. Interfaces, settings and the storage protocol may
change without a migration path. There is no warranty, no support contract
and no uptime guarantee.

If you put this in front of a congregation, do it with a tested fallback in
place, a technical person on hand, and the assumption that any given event may
have to go ahead without it.

## Before you start: set a retention rule

**Nothing in this project deletes anything.** The plugins only write and read.
Expiry is a bucket lifecycle rule you configure once in your storage provider's
console, and without one every event you broadcast stays for ever — roughly
**2.7 GB per hour** at 6 Mbps.

Add a rule for the prefix `events/` and another for `rooms/`, both deleting
objects after the same number of days. Seven days is the design default, and
**the rule is also your DVR depth**: a campus can timeslip back only as far as
retention allows.

## Licence

This project is **GPL-3.0-or-later** (it moved from MIT at v0.1.5-alpha).
Releases up to and including v0.1.4-alpha were MIT, and that grant cannot be
withdrawn: anyone holding those versions keeps their MIT rights to that code.
Third-party terms are set out in `COPYRIGHT`.

## What's new in v0.1.26-alpha

A small release, for campuses receiving from the new MultisiteOS encoder box.

**Audio tracks of different sizes now all play correctly.** When an event's
audio tracks had different channel counts, a campus decoded every track after
the first as if it had the first's channel count. A stereo mix beside a mono
microphone track could crash the decoder, and an eight-channel track beside a
stereo one came back as noise across all eight channels. Events from OBS have
not hit this, because OBS sends every track with the same channel count, but an
event from a MultisiteOS encoder, which sends a stereo programme with all eight
channels beside it, would. This fixes the OBS plugin and the campus player
alike. A test now plays a stereo, a mono and an eight-channel track together
and checks every channel of every track comes back in order.

**The relay sends the programme when an event also carries every channel.** A
MultisiteOS encoder sends its stereo programme as track 1 and every AES67
channel as one packed track beside it, for campuses with a multi-channel
output. The relay used to refuse the whole event because of that packed track.
It now sends the programme, and never sends the packed track or offers it as a
choice, since that would put the microphones and the click on air. An event
with nothing but a packed track is still refused, as before.

**Known, not fixed: packed audio from OBS loses the highs on channel 4.** In
packed multi-channel mode OBS sends eight channels as 7.1, and in 7.1 the
fourth channel is the low-frequency (subwoofer) channel, which AAC keeps only
the lowest frequencies of. With the default channel names that is "Click", so
a click can reach a campus as a thud or not at all. Until this is fixed, put
nothing on channel 4 of a packed feed that needs more than bass. Separate
audio tracks, the default mode, are not affected. (BUGS.md entry 3.)

**Captions are still not in this release**, for the same reasons as before:
they have never run end to end, and they would be on by default.

## What's new in v0.1.25-alpha

**The End broadcast button says "End broadcast" again.** In v0.1.24-alpha it
read "Dock.End" on US English installs. A missing line break in the
translation file had glued that entry onto the one before it, which also
spoiled the message shown while a machine waits for its Multisite Cloud
credentials. A test now checks every translation file is one entry per line,
and that every string the plugin asks for exists.

**A campus on Multisite Cloud can drop cues.** A campus reads Multisite Cloud
with a read-only permission, so until now its cues were refused. It now asks
Multisite Cloud for a second permission when it joins an event, one that can
write its own cue file and nothing else, and writes the cue with it. Because
it is fetched on joining rather than when the button is pressed, cues keep
working if Multisite Cloud becomes unreachable during the service. A box that
never got one hands its cue to the main site over the LAN when it can, and
otherwise says plainly that the cue was not saved. Verified on the OBS plugin;
the campus player has the same code and is not yet proven on a Pi.

Every cue a site drops now leaves a line in the log, set or not: which
segment, which file, and how it got there. It used to leave nothing.

**The OBS docks show the link figures on Multisite Cloud.** The data centre,
host, transfer speed and clock check were blank on a machine storing or
playing through Multisite Cloud, because the docks only asked the typed-key
connection. They are filled in on both docks now, and they no longer blank
for a moment each time the credentials refresh. The decoder's heartbeat also
sends the site's name now, so the Multisite Cloud dashboard can say which box
it is. It had only been sending it from the encoder and the Pi.

**The campus player on Multisite Cloud.** Several faults, all on a Pi set to
Multisite Cloud, which is **built but not yet proven on a Pi**:

- It restarted playback every time its credentials refreshed, about every
  7.5 minutes, losing its position. While Multisite Cloud was unreachable it
  rebuilt every five seconds and could not play at all. It now rebuilds only
  when it is given a different bucket.
- Changing the storage provider on its page did not take effect until then.
  It does at once now.
- Its page could not list recordings, its storage health said "the player is
  not running", and its clock check always read zero. All three now use the
  Multisite Cloud connection.

**Stopping just after a credential refresh no longer hangs.** A download that
began before the refresh was not cancelled by a stop, so the stop could wait
out that download's full timeout. This affected the OBS plugin too.

**The campus player, on other hardware.**

- **Rockchip hardware decode.** On a Rockchip board running its vendor
  kernel, a ROCK 5B for example, video now decodes in hardware through MPP
  for H.264, HEVC, VP9 and AV1. Everywhere else, including every Raspberry
  Pi and the OBS plugin, decoding is unchanged.
- **A sound card that stops taking audio no longer freezes the player.** On
  a ROCK 5B the HDMI audio stopped when the display was re-detected mid-play,
  and the picture and the page froze behind it. The audio now waits in short
  slices, restarts the card once, and otherwise drops that audio so the
  picture carries on.
- It reports the kind of box it is and the board's serial when pairing,
  rather than calling everything a Pi, and it passes an update token on to
  appliance software when Multisite Cloud issues one. Existing Pis are
  unchanged.
- The installer can pin an exact tag or commit (`REF=`), so an appliance
  installs the same player every time.

**The relay can pair with Multisite Cloud**, for its heartbeat and, if you
choose Multisite Cloud as its storage, for reading. It refuses credentials
that could write, since it only ever reads and is the component exposed to
the internet. Off by default; nothing changes for an existing relay.

**For developers.** The core installs as a CMake package another project can
build against (`MULTISITE_INSTALL_CORE=ON`). CI now builds only what a push
changed, and builds a burst of pushes once.

**Captions are still not in this release**, for the same reasons as before:
they have never run end to end, and they would be on by default.

## What's new in v0.1.24-alpha

**Resuming after a hold no longer freezes the picture.** Hold, then resume, and
the picture could sit still for about six seconds before playing on. The hold
was dropping the fragment the decoder was about to be handed — a whole segment
lost — so the frames after it arrived stamped six seconds in the future and
delivery waited for them. A hold now keeps that fragment and only a real jump
drops it. Verified on a real hold, and the decision is pinned by a test so it
cannot slide back.

Three smaller faults around a hold went with it. The first frame after a hold
began is no longer dropped. The feed no longer counts held time as time fallen
behind, which had left it running about 24 seconds ahead for the rest of the
session. And the decoder dock's elapsed time no longer jumps a segment forward on
resume and then stands still until the next one: it stops where the picture
stopped and counts up the moment you resume.

**Multisite Cloud storage, in the OBS plugin.** For a site with a Multisite
Cloud account, a machine can now be paired from the dock's **Cloud** section and
then record to, or play from, the account's bucket with no bucket, endpoint or
keys typed in. Credentials are short-lived and fetched as needed. Choosing
Multisite Cloud as the storage provider hides the fields that such a machine
never reads, and every surface that deals with storage — Manage storage, the
upload test, the idle monitor, Go Live's own checks — now knows about it.
Verified on a Mac with both halves: an encoder recording to the bucket and a
decoder playing it back.

A machine that is both encoder and decoder is **one identity per role**, so the
dashboard sees the decoder as a read-only reader in its own right rather than as
the encoder's write access. Typed keys work exactly as before, and a machine
with typed keys is never moved onto brokered storage behind your back.

**The Pi player has the same Cloud section on its page**, and the code to play
from a Cloud bucket. That path is **not yet proven on a Pi**: the unit it was
tested on reads a bucket with typed keys and is paired for monitoring only. If
you try it, a log is very welcome.

**Monitoring heartbeat.** The plugin and the Pi can report their health to a
monitoring collector such as the Multisite Cloud dashboard, now including the
site's name so a dashboard can say which box is which. It is off by default, and
sends the status the dock already shows — no pictures, no sound, no credentials,
no IP addresses. `docs/OPERATOR.md` says exactly what goes.

**OBS no longer freezes when uploads stall.** A status read waited behind a
network upload, so a stalled link could lock the whole interface. Manifest writes
have their own thread now, and a test pins the wait at zero.

**A likely cause of the Windows crash is fixed.** A worker thread could deliver
to a dock widget that was already gone. That has been removed everywhere it
occurred. It matches the crash's shape but has not been confirmed against one,
so the Windows build now also ships its debug symbols as a build artifact: if it
does happen again, the dump will be readable.

**Smaller things.** A clean End Broadcast no longer logs a failed upload and a
retry for the request that stopping cancelled. A paired machine no longer logs
"not paired yet" as an error at every launch while its credentials arrive. The
Pi keeps the fragment in hand on a hold, as the plugin does. Pairing says why it
failed, every time.

**Captions are still not in this release**, for the same reasons as last time:
the work is on `main`, it has never run end to end, and it would be on by
default — an untested change to the encoded video for anyone whose feed carries
captions. Three races in it were fixed this week, from reading the code; that is
not the same as seeing captions arrive.

## What's new in v0.1.23-alpha

A short release, cut for two faults that v0.1.22-alpha shipped. If you are on
v0.1.22-alpha, both affect you.

**The campus player's timeline and scrubbing were dead.** Not degraded —
nothing at all: the bar drew empty and every click and drag was ignored.
v0.1.22-alpha moved positions from times of day to elapsed time, and elapsed
time starts at zero. Four guards in the player's page tested that position with
`if (!x)`, which reads zero as "missing" — and zero is the ordinary state for a
recording whose first segment is still in storage. A fifth fault in the same
family placed cue marks on the bar using a time of day against a scale starting
at zero, so every cue sat off the right-hand end.

The page's arithmetic now lives apart from the page, in `media.js`, with tests
that run in CI. It had none before, because it needed a browser and the
arithmetic does not — which is exactly why this shipped.

**The decoder dock read "29831921 min 43 sec behind".** That number is the
current Unix time in minutes, which is the signature of this kind of fault: the
live edge was still held as a time of day while the playhead had become an
elapsed time, so the readout subtracted a position from an epoch. Both sides are
now the same kind of quantity. The figure the appliance reports was never
affected, and neither was the timeline — this was one readout.

**The second bucket's settings open properly.** The panel appeared half open —
three credential fields showing through a box that was supposed to be shut — and
only collapsed correctly after you ticked and unticked it. Two pieces of code
owned which rows were visible and disagreed; one does now.

**And the upload speed test measures your link, not just the second bucket.** It
was inside the second bucket's panel and only ever tested that bucket, so an
operator without a second bucket could not measure their uplink at all, and the
button was invisible whenever the panel was collapsed. It now sits beside the
connection test, measures the main bucket with the values as typed, and measures
the second as well when one is configured — sequentially, because two bursts at
once measure each other rather than the link.

**The campus player says how hard the box is working**, on the page that answers
for it, and a decoder now reports when it has opened its own threads. The AES67
daemon is no longer left holding the player's own port. And the Pi build sizes
itself by available memory rather than by core count, so a small Pi survives it.

**Captions are not in this release.** The work exists and is on `main`: closed
captions carried inside the video bitstream, so they reach every campus and pass
through the relay to YouTube unchanged, taken either from a feed that already
carries them or from a captioning plugin's text source. It is held back because
it has never run end to end — every link is verified by reading the code and
nothing has been observed arriving anywhere — and because it would have been on
by default, which means an untested change to the encoded video for anyone whose
feed carries captions. That is not a thing to hand out unasked. It ships when a
real event has proved it.

## What's new in v0.1.22-alpha

**A second bucket, written independently.** The promise this tier makes is
"never lost", not "never interrupted", and until now one bucket was one place
for an event to go missing. A second storage target can now be configured and
is written **independently** rather than mirrored from the first: two uploads
from the same spool, each with its own confirmed position, so a stall or an
outage at one target cannot hold the other up or leave it a step behind. The
encoder dock says whether the copy is keeping up and, when it is not, why. A
measured capacity burst can be run **before** an event, so spare uplink is a
known number rather than a hope.

**And the decoder reads from whichever copy is there.** LAN first, then the
primary cloud, then the second — so a campus keeps playing through the loss of
either bucket. A failover ends when the copy being read stops advancing, which
is the condition that actually matters, rather than on a timer.

**The decoder now behaves like a recorder.** This is the largest single change
in the release and it took most of a day of measurement to get right. Holding
playback used to quietly eat programme: the picture stopped but the decoder
carried on, so resuming continued from wherever it had got to rather than from
where you stopped it — 0.20 s lost after a 1.4 s hold, 1.09 s after 11 s, and
3.90 s after 67 s. A hold now freezes the read head and leaves the write head
running, the way a recorder does. Position loss after a hold is a constant
~360 ms instead of a figure that grows with how long you held it.

**Scrub the timeline, and it lands where you clicked.** Drag to scrub, nudge
with the arrow keys, click anywhere. A click used to divide by the segment
length and throw the remainder away, so it could land up to six seconds from
where you made it; segments are the unit of transfer, not the unit of seeking.
Seeking is now accurate to 21–58 ms, and seek-to-picture came down from several
seconds to 77–263 ms — five separate faults, each measured before and after.
The largest was the playout gate: every frame the seek was about to discard was
first waited for at playout rate, so seeking 1.8 s into a fragment cost 1.8 s of
real time doing nothing.

**Times are elapsed, never a time of day.** A position in this project used to
be a wall-clock instant, derived from a mapping between media time and time of
day that was pinned, re-pinned and drifting — by about 1.1%, position-dependent,
with the cue system resting on it. That entire concept is gone. A position is
now the frame's own presentation timestamp: how far into the programme it sits,
with time behind live for a stream that is still running. Nothing to pair,
nothing to re-pin, nothing that can drift away from the picture. It is also what
an operator actually wants to read, and it is how a Resi decoder presents it.
The docks, the Pi player's page and the appliance's API all changed together.

**Cues land on the moment, not the segment.** They now carry where they fall in
the programme directly, so a jump to a cue goes to the cue rather than to the
start of the six seconds containing it. Older cues, which carry only a time of
day, are converted through the event's start — a conversion that is now exact,
because it was the drifting mapping above that made it inexact. The change is
additive and the protocol version is deliberately **not** bumped: an old build
ignores the new field, a new build falls back to the old one, and there is no
flag day in either direction.

**Audio and video no longer part company after a seek.** Found on real hardware
after the seek work landed. The sub-segment skip had been moved to where frames
are made, which is right and is most of the speed-up, but it relied on frames
arriving in presentation order — true where it used to live, false where it went.
Audio always spoke first and claimed the shared origin ~311 ms ahead of video, so
every video frame was measured from too early a start and kept 311 ms of picture
the audio had already discarded. The measured interleave gap after a seek went
from 300–319 ms to **1–17 ms**.

**And playback no longer stalls under it.** The delivery queue was bounded by a
frame count sized as "twelve frames is 400 ms at 30 fps" — but twelve frames
span eleven intervals, so it held 367 ms against a 400 ms gate. A cap below the
gate it feeds is a permanent jam. The bound is now a duration, the same for both
streams, tied to the gate by a compile-time assertion so the two cannot drift
apart again. It also removes a frame-rate dependence nobody had noticed: at
60 fps the old twelve frames held 183 ms, under half the gate. The cost is real
and is written down — about 93 MB of queued video at 1080p30 where it was 37 MB,
which is the price of wanting a 400 ms lead at all.

**Loading an event is a state you can see.** A load is acknowledged, says what
it is doing, and asks before interrupting one already in progress. A pinned
event is not reported ready until it is the thing actually playing, a pinned
event plays as a recording whatever its manifest claims, and **Stop is no longer
reported as a fault** — it is a thing you chose.

**Storage cleanup does what the button says.** Deleting events older than N days
previously appeared to do nothing, with no progress shown and nothing in the
log. It now runs visibly, reports what it is removing, and says so in the log.

**Smaller things.** The buffering readout quoted the wrong number. Both plugins
say when a newer build exists. The bucket can be tested before an event instead
of during one. Settings that had no explanation have one. `scripts/setup-mac-build.sh`
stands up a macOS plugin build, including dependencies that survive a reboot.

**A note on how this one was found.** Almost every fix above was measured before
it was made and measured again after — and several times the first explanation
was wrong and the instrumentation said so. The A/V split and the queue bound were
both found by an operator on real hardware and diagnosed from log lines that had
been printing the answer for weeks. Where something is a defensive fix rather
than a diagnosis, `BUGS.md` says which.

## What's new in v0.1.21-alpha

**Cues belong to the event, not to the main site.** A cue — "Sermon Start",
"Go to local" — has always been the main site's to drop and everyone else's to
watch. It is now everyone's. A dedicated **Cues dock** is present whichever role
a machine is, shows one merged list, and lets any site add to it. Names are
typed rather than picked from a fixed set, because a service has no fixed set of
moments, and each cue carries the name of the site that set it — so a cue
dropped at another campus is never mistaken for the main site's.

**A campus can drop a cue without being able to write your bucket.** Each cue
lives in one object per author, `events/{id}/cues/{site}.json`, so a satellite
given a key scoped to its own file can add cues and nothing else. On a LAN with
no bucket at all the cue is handed to the main site, which writes it, so the
satellite stays read-only. The **Raspberry Pi player** can drop cues from its
own page as well — the first of the cue work to land on the tier MultisiteOS is
being built from.

**Cue times no longer depend on anybody's clock being right.** Cues are ordered
by where they fall in the event, not by the time of day a box claims; and a box
whose clock is plainly wrong is now told so. The store answers every request
with its own NTP-disciplined time in a `Date` header, and the difference is
measured from traffic already being sent — no NTP client, no extra request, no
privilege. Both docks and the Pi page say when a machine is out past a few
seconds, because that is what makes its clock times read oddly.

**A replay can now be an excerpt.** The relay could already play a finished
event out to a destination as though it were happening now; it can now be
bounded by two cues, an in point and an out point, chosen per event and loaded
when you open the picker.

**The Pi player's stall has a floor under it now.** A player could stall
indefinitely while downloads kept succeeding — the picture frozen, the cache
still filling, nothing in the log but the shape of it. The suspected mechanism
was the feed loop parked for ever inside the decoder's back-pressure wait. That
wait is now bounded by whether the decoder is still *producing*: ten seconds
with no output and the decoder is declared wedged, the loop is released, and the
player rebuilds it. It is a defensive fix, not a diagnosis — the thread dump is
still what would settle the last of it — but the worst case is now a reported
event that costs at most one segment instead of a silent freeze.

**And the teardown has a floor under it too.** The first fix released the feed
loop; a decode thread wedged *inside* FFmpeg still parked the join that rebuilds
it, so stopping a wedged player could hang the process instead of recovering it.
Tearing a decoder down now waits a bounded grace period and, if the thread will
not return, abandons it and lets the caller build a fresh one. Same defensive
posture as above, same remaining question — the thread dump is still what names
the root cause.

**Watching an event end no longer lets the next one take the picture away.**
Choosing a recording always pinned playback to it, so a rehearsal beginning in
the room could not yank an operator out of it. Following the live feed and then
sitting in the recording of it was the same commitment in practice but not in the
code, so when `live.json` moved on to the next event, playback jumped. A finished
event is now held exactly as if it had been chosen, and **Back to live** is how
you follow the room on. The Pi turns this off — an unattended box is there to
relay whatever the room does next — and that is now a setting on its own page
rather than a hard-coded choice.

**You can see the buffer fill.** Before Play there is no playback head to measure,
so the dock had nothing to show but a static **READY** while the link was plainly
busy. It now counts up against the start gate — *Filling buffer — 23 s of 60 s* —
so a slow link looks slow instead of looking stalled.

**The docks fit the window you have.** The decoder dock could not be shrunk below
its own content, so on a laptop it wanted more height than OBS itself had: the
scroll areas reported their content's minimum as their own, and both docks and
both Settings dialogs inherited it. They scroll properly now and the dialogs cap
themselves to the screen, so nothing has to be dragged past the edges of the
display to reach a button.

**"Load event" is now "Follow live".** Two buttons both said Load — one followed
the room's live feed, the other pinned a chosen recording from the list — which
made the difference between them hard to see. Only the second is a choice of
*what* to play, so the first is renamed. The encoder's cache folder, meanwhile,
now holds its outgoing queue, its downloaded video and the copies it serves over
the LAN in the one place an operator chose, rather than the queue there and the
LAN copies somewhere else.

**Settings are one page, and applied when you say so.** The decoder's feed name,
buffering, cache location and poll interval moved out of each source's own
properties dialog and into the dock's Settings, where the storage already was —
a second editable copy per scene was what let a dock edit appear to do nothing.
Both docks also gained an **Apply** button: opening the settings to look at
them, and closing them again, no longer saves or reconfigure anything, which
makes it safe to do mid-event.

**The cache folder is yours to choose.** Where downloaded video is kept is a
setting on both the plugin and the Pi, rather than a fixed directory under OBS's
plugin config.

**Pi 4 hardware decoding.** The campus player prefers the hardware H.264 decoder
where one exists and falls back to software otherwise, which is the difference
between a Pi 4 keeping up and spending three of its four cores trying.

**clang-tidy is a gate now.** The static-analysis job held a short list of
checks as errors after a triage pass against this tree, rather than advice
nobody acts on. The list and the reasons the noisy checks are excluded are in
`.clang-tidy`.

**MinIO is no longer recommended.** Its community edition reached end of life —
repository archived, binaries stopped, security fixes no longer backported — so
the documentation now points at Garage for a self-hosted bucket, and mentions
RustFS as the closest drop-in successor once its lifecycle support ships. An
existing MinIO deployment still works: the endpoint does not care.

## What's new in v0.1.20-alpha

**If you stream to YouTube or Facebook, this is the release that gives you two
more codecs.** Choosing HEVC for the campuses used to cost a church its public
stream outright, and AV1 was refused on both kinds of destination — on the
grounds that RTMP means FLV and FLV means H.264. That was true when it was
written, and it had stopped being true some time before anyone noticed. Both go
out now, unchanged and with nothing re-encoded, from the same single upload.

**Re-pull the relay container to get it.** The image moved to a newer Debian
because of one thing: the ffmpeg in it could not write the format a streaming
site needs, and the newer one can. `:v0.1.20-alpha` is the first version tag that
can send HEVC or AV1 to a streaming site — the older tags are images from before
that change, and will refuse.

The rest of this release is mostly things that were quietly wrong: a recordings
list that could sit on "Looking for recordings…" for ever, event names that never
reached the relay, a status row labelled **Bucket** that was showing a URL, and a
README that read like a wall of text.

### HEVC and AV1 both reach a streaming site now

The relay refused both, and the reasoning had gone stale in a way worth
describing, because it is the kind of mistake that is invisible from inside: it
was measured once, correctly, and never measured again. What a destination will
take has since changed — **Enhanced RTMP** carries HEVC and AV1 in the same
container H.264 has always used, and YouTube documents H.264, H.265 and AV1 for
RTMP/RTMPS ingest and recommends H.265 over RTMP(S) for HDR.

- **HEVC** goes out over both protocols now, unchanged. Over SRT it always did;
  over RTMP it travels as Enhanced RTMP.
- **AV1** goes out over RTMP — and has been **measured** doing it: an AV1 event
  was pushed from a real encoder through the relay to YouTube and played there
  for over ten minutes without a fault. It is still refused over SRT, and that
  half is ffmpeg's doing rather than a policy: MPEG-TS has no AV1 stream type at
  all, so there is nothing to send.
- **What is still unknown is the destination**, not the transport. YouTube
  documents all three; nobody else obviously documents AV1. So where support
  varies by destination, the page now says so **before** you start rather than
  refusing the event outright: only a site that documents AV1 ingest will take it
  over RTMP, and one that does not will drop the stream as soon as it starts.
- **The refusal that sent you somewhere else has been fixed.** An AV1 event aimed
  at an RTMP destination was told to "send this to an SRT destination instead" —
  which cannot carry AV1 either. That advice was wrong for as long as it existed.

### An event published with cloud upload off can never be listed, and now says so

Recording with cloud upload disabled is a legitimate thing to do — it is how a
site keeps an event inside its own network — but the consequences were not stated
anywhere. Recordings are enumerated out of the bucket and only the bucket, so an
event published over the LAN alone cannot appear in any recordings list, on any
machine, including the one that recorded it. The list is empty rather than wrong,
and the box used to sit on "Looking for recordings…" for ever, which reads as a
broken list rather than as an answer.

It now says which it is, and the two places where that decision is actually made
say what it costs: the confirmation when you turn cloud upload off, and the dock
while it is off. Worth knowing, and written down rather than "fixed": a LAN-only
recording is **not durable** — the local spool is cleared when the next event
starts, and the LAN server only ever serves the event that is on air. The bucket
is still the only archive.

### The relay shows what you called the service

Its list of past events showed a date and a time and nothing else, because the
name — which the encoder had already read back from the manifest — was dropped at
the API boundary. It shows the name now, with the date and time underneath, and
falls back to the date and time for events nobody named.

### The status row labelled "Bucket" was showing a URL

It has never shown the bucket: it shows where storage answers from and how fast
the transfer is going. With a Custom endpoint, which is stored as a full URL, the
fallback took the first segment of that URL, so the row could read
`https://minio` — and because a long value in that column sets the width of the
whole dock, it could push the dock wider than the screen. The row is now labelled
**Storage**, the scheme and path are stripped before anything is shown, and every
status value is elided to a sensible width with the whole value in its tooltip.

### It now says where it came from

All four interfaces — the two OBS docks, the plugin's own phone pages, the campus
player and the relay — carry the project's name, a link to the manual and
downloads, and the Stage Audio Works mark. Deliberately served from each page's
own directory rather than linked from the website: a campus box often has no way
out to the internet, and a logo that only loads online would be missing exactly
where it is most useful.

### Also

- **AV1 is round-tripped by the tests for the first time.** It was the codec this
  pipeline had always claimed to carry and had never once decoded in a test,
  which is the difference between a codec that happens to work and one that is
  supported. AV1 muxing and decoding are covered end to end now, along with the
  recordings list and a real session's ending.
- **A data race in the uploader's "verified" note**, found by the sanitizer job
  on a commit that touched nothing but documentation. That note is logged in the
  dock during the first uploads of every event; it was being read under a lock
  the uploader never took.
- **One content-type table.** The plugin kept a smaller second copy of it that had
  never learned `.svg`, so the new mark would have been served as an anonymous
  blob — which a browser is entitled to refuse to draw, silently.
- **The website has a "Next door" section.** Teaching across sites is the case
  named, because it uses the one thing a live stream cannot do — a facilitator
  holding the feed to run a discussion and resuming exactly where it paused — and
  the neighbouring cases get a sentence each.
- **One service, several languages** is documented now, on the site and in the
  operator guide: the desk's interpreter feeds travel as separate tracks and the
  relay sends a different one to each destination, all from the single upload the
  main site already made. The limits are on the same page as the answer — the
  picture is identical everywhere because there is no transcoder, each language
  needs its own stream key, and each costs the relay's uplink another copy of the
  bitrate.
- **The README leads with what the project is** rather than five paragraphs of
  prose, and one piece of the record has been corrected: tiles were built, tested
  and released in v0.1.15-alpha while the roadmap still said Phase 10 had not been
  started.

## What's new in v0.1.19-alpha

**If you are running v0.1.18-alpha, update before your next service.** That
release could finish a broadcast without ever recording that it had finished.
Everything looked right in OBS, and the campuses watching were left polling a
room that never said it was done — eventually reporting a service that ended
perfectly normally as *interrupted*. The cause, the fix, and the two data
races found underneath it are below. The simulcast relay also catches up with
the LAN delivery work the plugins got last release.

### End Broadcast could publish nothing at all

The shutdown hang fixed in v0.1.18-alpha was fixed by cancelling the upload
transport before joining its thread, so a stuck request could not hold OBS
hostage. That was right. What was missed is that cancelling is deliberately
*sticky* — the transport refuses every request afterwards, which is exactly
what makes it safe to cancel a thread you are about to join — and that
`Session::end()` calls the cancel and then keeps using the same transport for
everything that still has to happen: draining whatever is left in the spool,
writing the final `manifest.json`, and writing the `live.json` that marks the
event ended.

So from v0.1.18-alpha, every clean End Broadcast switched the transport off
one line before the work that needed it. The spool drain uploaded nothing and
spent its full deadline failing, and neither closing file was ever written.

The lost segment was the visible symptom and the smaller problem: it stays in
the local spool and goes up on the next resume, exactly as designed. The
event never being marked ended is the real one, because nothing downstream
can tell that apart from a main site that went off the air mid-service.

The transport is now resumed the moment the upload thread has been joined,
and again whenever a session begins, so a transport inherited from a previous
session cannot start life switched off. `Session::end()` now returns whether
it actually managed to mark the event ended, and the OBS layer logs a plain
error naming the consequence when it did not, instead of reporting a clean
stop regardless.

Worth saying why 41 passing tests did not catch this: the test double
inherited the interface's do-nothing `cancel_pending()`, so it was more
forgiving than the real transport and the bug was invisible to it. The mock
now models the stickiness, and the new test fails against the old code.

### The suite now runs under sanitizers, and found two more races

Nothing in this tree had ever been checked by a tool rather than by a test,
while thirteen files here spawn threads — so a passing test that had raced
read exactly like a passing test that had not. The same 41 tests now build
and run under ThreadSanitizer and under ASan/UBSan on every push, plus
clang-tidy against a curated check list.

It found two genuine races in the HTTP server that serves the remote-control
pages, both about shutdown. One was the listening socket handle being written
by `stop()` on one thread while the accept loop read it on another — which
also meant `accept()` could be called on a handle already closed and reissued
to something else, and this process runs a second HTTP server whose sockets
are precisely what that number gets reused for. The other, reported only on
Linux, was connection threads still touching the server after releasing the
shutdown that was waiting to destroy it: in practice, unloading the plugin or
switching remote control off while a phone still had the control page open.

ASan and UBSan came back clean across all 41 tests on both platforms.

### The simulcast relay catches up on LAN delivery

The relay read its live feed through cloud storage only, so a relay sitting
on the same network as the encoder still round-tripped every segment through
the bucket, and an encoder running LAN-only with cloud storage disabled could
not be relayed from at all. It now uses the same direct-then-fallback
arrangement the OBS decoder and the Pi appliance already do, keyed off
whichever of cloud or LAN is configured.

Browsing, downloading and rebroadcasting past events stay cloud-only, by
nature rather than by omission: they list the bucket, and an encoder's LAN
side only ever holds the event currently in progress.

The relay's settings page also gains the storage provider dropdown
(Cloudflare R2 / AWS S3 / Backblaze B2 / Wasabi / Custom) the two OBS docks
and the appliance page have had for several releases.

### The plugin's own web remotes match everything else now

The encoder and decoder remote-control pages — the ones you open on a phone
rather than in OBS — were the last surface still on the original flat
settings layout with six raw storage fields. They now have the same
collapsible sections, the same provider dropdown, and the LAN and
cloud-disable settings they had been missing entirely, including the same
confirmation prompt and the same refusal to leave an event with no delivery
path at all.

### Also

A lossless high-quality mode is on the roadmap as Phase 15 — FLAC audio on
every track with around 10 Mbps HEVC video, for venues with bandwidth to
spare. It would not be usable through the web relay, only with the player
applications. Nothing is built yet.

## What's new in v0.1.18-alpha

A campus on the same network as the main site, or reachable over a VPN the
church already runs, can now receive over that path directly instead of
through the bucket — on both the OBS decoder plugin and the Raspberry Pi
appliance. Cloud storage can also be turned off entirely, for a single
building with no interest in an off-site copy at all. Two real bugs were
found live-testing this, both fixed, both written up below rather than
quietly folded in.

### LAN / direct delivery

An encoder can now serve satellites directly over plain HTTP on the same
network or an existing VPN, instead of every satellite going through the
bucket regardless of where it actually sits. A satellite prefers that path
automatically whenever it answers, and falls back to cloud per request —
not per session — the instant it doesn't, so one segment that happens to
have aged out of the LAN side's retention window falls back for that
segment alone rather than dropping the whole connection to cloud.

Cloud upload stays on by default throughout: every other satellite, and the
archival recording, still depend on it. It can now be switched off entirely
for an event — a new checkbox in the encoder's Storage settings, shown only
once LAN delivery is turned on, with a confirmation prompt before it takes
effect (turning it back on needs no confirmation, since that's always the
safe direction). With it off, nothing for that event ever reaches the
bucket at all; every satellite still gets the full event, provided it can
reach the encoder directly.

The Raspberry Pi appliance gained the identical capability in the same
pass — LAN host/port/token fields in its web settings page, the same
automatic preference and fallback, and the same ability to run cloud-free.

### A shutdown hang that could look like a crash

Ending a broadcast could freeze OBS's main thread for as long as a stuck
upload had left to retry — with retries set to continue forever in
production, that had no real upper bound. A hang long enough looks, from
the outside, exactly like a crash: the window stops responding, and
force-quitting it is indistinguishable from OBS actually having crashed on
the next launch. Found after a report of exactly that on Windows, and
reproduced on macOS too once we knew what to look for.

The drain now genuinely respects its own deadline — bounded to 8 seconds by
default, down from a technical (but non-functional) 30-second cap — so a
segment that's still stuck when time runs out is left in the local queue
for the next resume, exactly as a real crash would have left it, instead of
holding the whole application hostage to it.

### The decoder's "receiving via LAN" indicator could say the wrong thing

Built alongside LAN delivery, and caught before it reached anyone: the
indicator tracked which path answered the single most recent request,
which is the wrong question. A request for something LAN legitimately
doesn't have yet — a marker before the first one was dropped, a segment
that aged out of the retention window — isn't evidence that LAN itself is
down, but it was being read as exactly that, and the display would flip to
"via cloud" on requests that had nothing to do with the actual link. It now
distinguishes a real connection failure from an ordinary miss, and reports
accordingly. The same fix applies to the Pi appliance's equivalent readout.

### The Raspberry Pi appliance catches up

Beyond LAN delivery: the storage provider dropdown (Cloudflare R2 / AWS S3 /
Backblaze B2 / Wasabi / Custom) that the two OBS docks have had since
v0.1.13-alpha now appears on the appliance's own web settings page too — it
had never reached there until now. And that settings page, grown long
across several releases of new fields, now collapses into named sections
you open one at a time instead of scrolling past everything to find the
one you came for.

## What's new in v0.1.17-alpha

### A satellite on the network audio card could put digital noise on air

A box with **Sound on the network** switched on could, within a minute of
starting to play an event, put loud digital noise onto the AES67 stream during
any quiet stretch — the moment nothing else was queued to send. It looked like
a hardware fault; it was a software one.

While nothing is playing, the satellite keeps the sound card open by feeding it
silence, so the stream stays live and a receiver never has to resubscribe. The
buffer holding that silence was sized from the wrong number — the card's
smallest internal chunk, rather than the larger amount actually written to keep
it fed — so every top-up read past the end of a small buffer of zeros into
whatever else was in memory, and played that as audio. This is now fixed at the
source: the buffer is sized for the largest write that can ever be made from
it, and a write that would exceed it is refused outright rather than allowed to
run past the end.

If your network audio output has ever gone briefly to static or a loud hiss
right after starting an event, this was it.

### Pressing Stop on a satellite now actually stops the sound

**Stop** cleared what was queued and marked the box stopped, but nothing in the
audio path actually checked that flag — so the next moment of programme
refilled the queue anyway and kept playing. The box would report `stopped`
while sound kept coming out of it. Delivery now genuinely gates on play state:
**Stop** discards anything queued and stays silent; **Hold** still keeps its
queue, so **Continue** picks up exactly where it left off. Those were already
meant to behave differently from each other — now they do.

### The encoder's local disk can no longer fill up unnoticed

The encoder writes every segment to a local durable queue before it is ever
uploaded, precisely so nothing is lost if the link drops. That queue was also
allowed to grow without limit: a link that stayed down, or stayed too slow to
keep up, for long enough would eventually fill the encoder machine's disk.

It now has a cap (4 GB by default). Past it, the OLDEST still-unsent segment is
the one let go — never the one that just arrived, so a recovering link is never
blocked behind a segment it will never get to — and every satellite still
downstream is told plainly that segment is gone rather than being left to wait
on it forever. A satellite that was sitting on exactly that segment jumps
forward instead of freezing.

Both the encoder dock and a Pi satellite's own operator page now show a plain
reading of the local disk — healthy, getting low, or almost full — checked
whether or not anything is currently broadcasting, so a drive running low is
something you notice before it starts costing you segments rather than after.

### Also

A related but separate gap was found and written up rather than fixed here: a
satellite that crashes mid-event (rather than switching away from it cleanly)
can leave behind a cache folder nothing ever cleans up afterwards. It costs
disk slowly rather than breaking anything, and is tracked in `BUGS.md`.

## What's new in v0.1.16-alpha

Three things that stopped work, all of which looked like something else.

### Hardware encoding works on a Mac

Choosing any of the Apple VideoToolbox encoders failed to go live at all,
with *"NO VIDEO EXTRADATA available from the encoder"*. On a Mac that is every
hardware option there is, so the only way to broadcast was x264 on the CPU.

Nothing was wrong with the encoder or the setting. Some encoders can describe
what they are about to produce before they produce anything; VideoToolbox
cannot, and only says once it has encoded its first frame. We asked too early
and took the silence for a failure.

Going live now waits for that answer when it has to, and holds the first moment
of the broadcast until it arrives rather than discarding it. Encoders that can
answer immediately are unaffected — including still refusing to start at all
when the storage settings are wrong, which is worth keeping: you find out
before OBS tells you you are on air.

Tested on an M5 with the HEVC hardware encoder, start to finish.

### OBS could refuse to quit after a broadcast

Closing OBS after broadcasting could hang with no window, no error and no way
forward except forcing it to quit — which OBS then reports as a crash the next
time it opens.

It was never a crash. Two parts of the plugin each held something the other was
waiting for, so shutdown stopped rather than finished. Reproduced deliberately,
found, fixed, and reproduced again to confirm: OBS now closes in about two
seconds, and no longer reports anything amiss on the next launch.

If you saw a crash report after quitting, this was almost certainly it, and
nothing was lost — the broadcast had already finished and uploaded.

### Play after Stop works again

At a satellite, pressing **Stop** and then **Play** left the picture blank.
Recovering it took a seek, which is not something anyone should have to
discover.

Stop releases the decoder — that is what makes it a real stop rather than a
pause — but it did not tell the feed that the next one would need its setup
data again. It does now. This was introduced by the Stop changes in
v0.1.12-alpha.

### A satellite's network audio goes to the right channels

Three faults that all presented as a silent room, and none of which said so.
The one worth knowing about: a box with network audio switched on opened its
card before any feed had arrived, guessed two channels, and then held that
guess against an eight-channel feed for the life of the process. Every listener
heard channels 1 and 2 of eight. Nothing was logged, because nothing had
failed — the card opened and the stream was published; the sound was simply in
the wrong places.

The channel count is now decided in one place, and with nothing to go on the
answer is "not yet" rather than a guess. The card is also held open and metered,
so a silent room can be told apart from a stopped one.

### Also

The satellite's preview can now show either what is going to the screen or the
whole feed behind it — useful when a tile is being used, where those are no
longer the same picture.

Contributors are asked for a sign-off rather than a copyright assignment, and
the relay ships an nginx configuration for putting TLS in front of it.

## What's new in v0.1.15-alpha

One new thing a room can do, a crash on quit, and several fixes — including
one correction to what the previous notes told you.

### Several cameras in one feed, pulled apart at the far end

A room that needs two or four separate pictures at its satellites has always
been able to composite them into a single feed and send that. Taking them apart
again was the satellite's problem, solved by hand with crop filters, at every
satellite, every time.

The main site can now say how it composited. **Settings → Media → Pictures in
this feed** offers one picture, two side by side, two stacked, or four in a
square. At the satellite, each region becomes a source of its own — **Multisite
Picture (Decoder)** — already cropped, ready to drop into a scene or send
fullscreen to a screen of your choosing. A campus player can be pointed at one
of them too.

It costs nothing to receive. The feed is downloaded once and decoded once
however many pictures you pull out of it; each one is a view of that, not
another stream.

**It has to be set before you go live.** The layout is written down when the
event starts, so changing it part way through does nothing until the next one.

**It cannot be worked out from the picture, which is why you have to say.** A
very wide frame is a perfectly good single picture as well as a plausible pair,
and a wrong guess would cut a programme in half. Left alone it stays on one
picture, which is what every room sending one camera wants and what every
recording made before this existed already is.

### OBS could crash when you quit after a broadcast

On Windows, closing OBS after broadcasting could end in a crash dialog. The
broadcast itself was fine and nothing was lost — the fault was on the way out,
as OBS closed its plugins down.

This plugin was talking to obs-websocket while OBS was already unloading. Which
plugin goes first is not decided, so if obs-websocket went first, we were
speaking to something that no longer existed. There was nothing to be gained by
that conversation at that point, so it no longer happens.

### The settings dialogs fit the screen again

Both settings dialogs had grown taller than a laptop screen — and the part that
fell off the bottom was the buttons, so there was no way to close the window.

They are in tabs now: where the video goes, how it is encoded, and what this
machine is. Each page fits without scrolling, and the buttons are always there.
The **Tag uploads for expiry** line carried its warnings in the label, which is
one long unwrapping line that was setting the width of the whole dialog; those
have moved to a tooltip.

### A companion source stops asking which room

The audio track and picture sources each had a room box that was almost always
left empty, because empty already means "the room the Multisite Source is
following". It now shows which room it is following, and only asks when there is
genuinely more than one to choose between.

### The stream keeps running when nothing is playing

On a receiver, the AES67 source used to disappear the moment the picture
stopped. The daemon publishes what is written to the AES67 card, and the player
only writes when there is a frame to deliver — so an idle box left a card that
was open and never written to, and a card in that state stops producing samples.
Nothing was wrong at the receiver; there was simply nothing being sent.

The card is now kept fed with silence while the box is idle. That is audible as
nothing at all, and visible as a receiver that stays online.

The first attempt at it changed nothing, and the reason is worth recording: it
kept one *period* of audio queued, which is only a cushion on a card with a large
period. The AES67 card's is one millisecond, so it under-ran on every cycle. It
keeps twenty milliseconds now, checked four times over.

**The trade, said plainly:** while the box is idle, up to 20 ms of silence sits
in front of the sound when playback starts again. That is inside what the card
buffers anyway.

A second thing came out of the same work. The card is opened as soon as the box
is idle rather than at the first audio frame, so a satellite that has not played
anything since it was switched on still has a stream on the network — which is
the state a satellite is in for most of the week.

### Putting the sound on the network moves the sound

**The previous notes were wrong about this, and the error sent somebody looking
in the wrong place.** They said the HDMI output and the network stream both
carried the audio. There is one output device on this box, not two, and the
daemon publishes what is written to the AES67 card — so a player writing to HDMI
leaves the stream silent however healthy it looks.

Choosing **yes** now moves the sound onto the AES67 card, and choosing **no**
puts it back on the device it was on before. The output device picker is greyed
out while it is on, because the two disagreeing is the fault worth preventing
rather than reporting.

### Renamed, and the audio is the campus's business

**Settings → Sound on the network** is now **Settings → Network audio output**,
in the player and everywhere it is written about.

The wording around it no longer assumes what the audio contains. It used to say
the stream carries "the production bus: main mix, mic ISOs and click", which is
one kind of event's answer to a question that belongs to whoever is running it.
The box carries up to eight channels and up to six tracks; what is on them is
their decision, and where an example helps it is now offered as one.

### Also fixed

Saving the settings page quietly discarded the network audio output's switch: the
control sat inside the settings form but was applied by its own button, so
pressing **Save** put the old value back and the choice looked like it had not
stuck. Saving now applies it along with everything else.

## What's new in v0.1.14-alpha

The appliance can now put its sound on the network without anybody logging into
the daemon to arrange it, and it says why it is quiet when it is.

### One switch, in the player's own page

A campus that wants its audio on a console rather than only on the HDMI socket
runs one installer, and from then on the player's own page carries the control.
**Settings → Network audio output** is on or off, the multicast address to
publish to, and the channel count — one button for all three, because they are
one decision rather than three. Switching it on starts the daemon, sets it to
come back after a power cut, and creates the stream; switching it off *stops* the
stream rather than deleting it, so the address survives and switching it back on
is one click and not a re-entry of everything.

**This box → Network audio output** is the other half, and it is the half worth
having: what is actually being sent, read back from the daemon rather than
assumed from the settings beside it. Whether it is running, whether its clock is
locked and to which grandmaster, the address and port on the wire, and the
session description it publishes — which is the thing a console's engineer will
ask you for.

The stream is created as part of the install, so a box that has just been
prepared is already sending — eight channels, 48 kHz, one-millisecond packets —
rather than waiting for somebody to add a source by hand. The daemon's own
interface is still there on port **8081** for everything else the daemon can do.

### Silence now has a reason attached

Four faults look identical from a settings page, and the page names them: the
clock is not locked, the stream is switched off, the sound card is not registered
with ALSA, or **the player is still writing the sound to HDMI**. The last is the
one worth having in writing — the stream configured, enabled, announced, and
carrying nothing.

The clock is the one this cannot fix, and it does not pretend to. The daemon is a
PTP slave: with nothing on the network handing out the clock it sends no audio at
all, and that is a network question rather than a fault in the box. The page says
so in as many words, instead of leaving it to be discovered at the receiver.

### The route is Merging's open stack, and the old one has gone

Audio leaves on the network through Merging's open RAVENNA kernel module and the
GPL `aes67-daemon`. That route can be aimed — multicast address, port and channel
map are all settable — which the licensed virtual sound card it replaced could
not do, and its buffer is a normal one, so the under-run that gapped the sound
once per frame on the old card does not happen.

The player no longer carries anything that knows about that old card. If this box
was set up with it, **read the note under Installing / upgrading before you
update** — removing it changes what happens on a box that still has it.

### Still not proven

Eight channels have arrived cleanly at a bench Pi. What is *not* measured is
whether the picture and the sound stay together across a two-hour service, and
how accurate PTP becomes when a Pi's network interface does no hardware
timestamping. Both have to be measured at the receiver, on a real event; ten
seconds of test tone settles neither. The detail is in BUGS.md entry 3.

## What's new in v0.1.13-alpha

Two things an operator does constantly — scrubbing to a moment, and reading how
far behind the main site they are — now do what they say. Both were wrong in
ways that are easy to work around once you know, and hard to trust around if
you don't.

### Scrubbing to a time goes there, and the clock agrees

Choosing a time on the timeline used to start playing immediately from the
position you had just left, run on for several seconds, jump somewhere else,
and settle showing a time that was not the one you picked — on a long recording,
minutes out. The picture and the readout disagreed, so neither could be
trusted for lining up a cue.

Underneath, the seek itself had been picking the right segment all along. What
went wrong was everything built on top of it. A decoder holds several seconds
of already-decoded pictures, and clearing the queue on a seek did not stop it
handing those over — so the position you had left kept playing, and worse, the
first of those frames defined the clock that every displayed time was then
measured from. Because that mapping is learned once and kept, a single frame
from the wrong place put the whole readout out for as long as playback
continued.

A frame now carries which timeline it belongs to, and anything from a position
already left is discarded rather than believed. The clock is fixed to the
fragment the seek landed on, which is what makes the reported time the
requested one: measured against a live event, asking for 14:40:43.768 now
reports 14:40:43.769.

Seeking within a segment is also honest now. Segments are six seconds long and
landing part-way into one means skipping the frames before your moment; the
readout used to describe the start of the segment rather than where it had
actually landed, up to six seconds early.

### "Behind live" holds still

The delay readout swung by six seconds while nothing about the delay had
changed. It was counting whole segments on both sides, and each side steps
independently as segments publish and playback advances.

It is now the gap between two real times, so setting a two-minute delay reads
as two minutes and stays there instead of flicking between 1:54 and 2:00. The
main site's content advances continuously; we simply learn about it in six
second pieces, so the live edge is carried forward between updates rather than
waiting for the next one.

### Smaller things in the decode dock

The playhead is redrawn between state updates instead of stepping twice a
second, so the scrub bar moves the way one should.

The header is now two readings rather than one: what this box is doing —
**PLAYING**, **HELD**, **STOPPED**, **READY** — and, separately, what the main
site is doing. A single chip could only ever show one of them, so playing a
finished recording read **BROADCAST ENDED** with nothing to say it was playing,
and **Hold** on a finished recording showed no indication at all.

### For anyone working on the decoder

The rules governing what a decoded frame may do, and in what order, are now one
tested component (`src/core/playout_timeline.h`) that the delivery loop calls,
rather than several blocks whose order mattered and was not written down.
Seeking broke five times in one afternoon getting here, every one of them found
by an operator rather than the suite, because nothing tested that path;
`tests/test_playout_timeline.cpp` fails on all five.

One known limit, unchanged by this release: on an event whose encoder has been
restarted, a segment's recorded wall-clock time and its position in the media
can disagree by up to a minute. Seeking is unaffected, because the same measure
is used at both ends, but the two are not interchangeable and a few places still
estimate one from the other for segments outside the current manifest window.

## What's new in v0.1.12-alpha

### Holding the picture no longer hands the screen to the box's own address

On a campus player, pressing **Hold picture** could be followed a second or two
later by the identity screen — hostname, IP, room — appearing in place of the
picture the operator had just frozen. On the screen in the room it reads as the
player having died at the exact moment somebody asked it to hold.

The cause was a collision between two things that meant different things by the
same word. Holding the picture was recognised only when the box's own idle mode
was set to *Hold the last picture*, so on a box left on the default idle screen
the poll loop found neither frames arriving nor a reason to leave the picture
alone, and fell through to drawing the identity screen. Everything else was
working: the hold itself, the audio, the cache, and — reassuringly — the
preview, which is rendered from the last decoded frame and so kept showing the
frozen picture the screen no longer had.

A held picture now outranks the idle screen whatever the idle mode is set to.
An idle screen is for having nothing to show: waiting for the main site, or
stopped, and both still put one up as before. **Stop** and waiting are
unchanged *in this respect*; on *hold the last picture* with nothing ever
decoded the identity screen still comes up, because there is no frame to hold
and a blank screen says nothing about which box has come up empty.

The rule is now a function of four booleans rather than a chain of conditions
inside the poll loop, and a new test checks all of its combinations — including
the default one on the identity screen — with no display, no decoder and no
network. That combination could only be reproduced by hand with a live event and
an HDMI socket attached, which is how it reached a congregation in the first
place.

### Stopping, and the header above it, say what they mean

Two changes to the decode dock, both about a control or a readout reporting
something other than what it was doing.

**Stop now releases the picture.** It was already ending playback, but the
source kept polling and kept pulling in flight, so a stopped decoder went on
downloading. Now Stop cancels the requests already in flight, stops the poll
loop issuing new ones, and lets go of the decoder. It deliberately keeps the
cache: Play resumes from disk rather than sitting through the full start-up
buffer again, which is what you want from a button you press and press back.
The header reads **Stopped — nothing downloading**, and Hold is labelled **Hold
picture (keeps recording)** so that its contrast with Stop is on the button.

**The header no longer hides one answer behind another.** It had been a single
label carrying three unrelated facts — what the main site is doing, what this
decoder is doing, and how the link is — behind a precedence order, so only one
could ever be visible. Playing a finished recording therefore read **BROADCAST
ENDED**, which was true of the venue and silent about the playback, and holding
a finished recording showed nothing about the hold at all. There are now two
labels, each answering one question:

- **Playback** — `STOPPED`, `LOADING…`, `BUFFERING…`, `HELD`, `PLAYING`,
  `READY`
- **Source** — `LIVE`, `BROADCAST ENDED`, `INTERRUPTED`, `RECORDING (not live)`,
  `OFFLINE`, `CONNECTING`, `CONNECTION LOST`

Playback sits first because it is what you are acting on. Neither can now
misstate the other, because neither can express the other's facts. `READY` is
new and names what **Load** leaves behind — buffered, not on air, waiting for
**Play** on cue, which previously had no name and read as whatever the room
happened to be doing.

Neither is a setting and nothing moves; the strings are in both locale files,
and the web remote — which already separated the two — needed only the stopped
state to be added to its status.

## What's new in v0.1.11-alpha

### Markers reach a control surface as they happen

The vendor events the plugin pushes carried the transport state but not the
markers, so a cue dropped at the main site reached an obs-websocket client only
on the next poll — up to five seconds later. `markers` and `marker_labels` are
now part of what the state event watches, so a cue reaches a Stream Deck's marker
button as it is dropped rather than when something else happens to change.

Nothing to configure, and nothing changes for a client that does not use it: the
same events, carrying the same document.

This is what the Companion module's generated marker buttons rely on. Every cue
the main site is configured with becomes a button of its own, and a new cue is
offered within about a second.

## What's new in v0.1.10-alpha

### Control it from a Stream Deck

Every command the plugin offers is now also an **obs-websocket vendor request**
under the vendor `obs-multisite`, and the plugin emits **vendor events** when the
state changes. Any obs-websocket client can therefore drive it — a script, an
automation system, or a Stream Deck through Bitfocus Companion.

There is nothing to switch on. obs-websocket ships with OBS 28 and later and is
enabled in Tools → WebSocket Server Settings; with it off, or absent, the plugin
logs one line and everything else carries on exactly as before.

The commands are the ones the plugin's own remote-control pages already use,
under the same names, so the two cannot drift — the list lives in one place and a
test pins it. Go live, End and the marker buttons for a main site; play, stop,
hold, resume, catch up, jog, seek, delay, markers, recordings and return-to-live
for a campus; a status request and events for everything watching.

### A Companion module

There is now a purpose-built Bitfocus Companion module —
[companion-module-obs-multisite](https://github.com/stageaudioworks/companion-module-obs-multisite)
— so those commands arrive as buttons that light up: on air, held, buffering,
behind live, link offline, with variables for the same figures and two preset
banks to drag straight onto a page.

> **Alpha, and not in the Companion store yet.** Until it is listed, Companion
> loads it as a *developer module* — the module's README has the steps. It needs
> **Companion 4.0 or later**, and this release of the plugin.

Tested against a real OBS, but not yet through a whole event.

## What's new in v0.1.9-alpha

For v0.1.8's users: the Manage storage tool arrived in the previous
release; this one is what makes it usable on a full bucket.

### Storage you can actually see and clear

**Manage storage…** in the encoder dock lists a room's events and lets them be
deleted — one, or everything older than a chosen number of days — with a
confirmation and a check afterwards. On a full bucket it used to sit on
"Looking…" for minutes while it walked every object of every event to add up
bytes, one request after another, and it logged nothing at all while it did.

It is two halves now. The events are listed first, from the manifests and the
live pointer, so something appears at once. Each event's size is then measured
beside it, six at a time, each row filling in as it lands — so the wait is the
largest single event rather than the sum of all of them. A size that cannot be
measured is reported as unknown rather than as zero, because "0 B" on an event
holding gigabytes is not cosmetic in a window whose whole purpose is deleting
things. Closing the window cancels the work in flight, and every listing and
sizes pass is logged with its counts and elapsed time.

### The documentation

The storage-management description, the layout notes and the counts in the
guides had drifted from the code; they are corrected. PROJECT-SCOPE.md now
carries a roadmap for the next milestones: storage redundancy, output
routing, headless appliances, ABR and low latency.

## What's new in v0.1.8-alpha

### The encoder and the decoder can be run from a phone

Both halves of the plugin now serve an operator page on the church network: the
same interface the Raspberry Pi appliance has, out of OBS itself. One page,
polled twice a second, in the plain language of an event rather than of a video
pipeline.

- **Sending side** — Go live and End the broadcast, the editable event name, the
  four marker buttons, and the reliability readout an operator watches
  mid-event: confirmed pieces, what is waiting to send, retries, bytes sent, the
  measured upload rate, the Cloudflare edge serving the bucket, and the last
  error.
- **Receiving side** — play, hold picture, catch up to now, jog, stay behind
  live, a timeline that can be clicked, the recordings list, and the readout
  that says how long this campus could keep playing through an outage.
- **Both** — the log, so somebody with a phone and no access to the desk can see
  why nothing is happening, and a Lock, so a tablet left on a music stand cannot
  stop a broadcast by being leant on.

It is the appliance's page on purpose: somebody who has learned one should not
have to learn the other, and the words on it are the words of an event. There is
no password and no TLS, exactly as for the appliance — the building's own network
is the guard — and it is on by default on port **8080**. It can be switched off
or moved to another port in **Settings → Remote control** in either dock, which
also shows the address to type into a phone.

Which pages exist follows the machine's role, as the docks already do: a main
site has no decoder routes at all, and a satellite none of the encoder's.

### One HTTP server, three users

The appliance's small HTTP server moved into the shared core and learned to
speak Winsock, so the plugin, the relay and the player now run one
implementation rather than three. It is covered by a new test
(`tests/test_http_server.cpp`) that speaks real HTTP over loopback, on every
platform CI builds: routing, verbs, the static web root, keep-alive, a handler
that throws, and the refusal of a path that climbs out of the web root.

### The campus player can be reached without a drive to the campus

What makes a wrong setting at a campus so expensive is that fixing it means
somebody driving there. The player now ships with the two optional tools that
remove that drive, and either can be set up from its own web page:

- **ZeroTier** puts the box on a private network that follows it, so it is
  reachable from the office wherever it is plugged in. The network key is given
  during setup — passed as `ZT_NETWORK_ID=…` alongside the installer, or typed
  at its prompt — and can be changed later under Settings → Remote access.
- **cloudflared** publishes the operator page on a public hostname with no
  port-forward and no static address. Give the installer `CF_TUNNEL_TOKEN=…`,
  or paste the token into the same panel.
- **The box's ZeroTier address is printed on its screen**, clearly labelled
  **REMOTE ACCESS IP**, underneath the room's own addresses. Those are a
  different thing — they work only inside the building and are what a phone in
  the room types — and the label exists precisely so the two are not confused.

Both are optional and neither is required to play an event. A box with neither
installed behaves exactly as before, and its screen says nothing about remote
access at all.

## Fixed since v0.1.6-alpha

**The campus player leaked memory on every idle-screen redraw.** The new
identity screen's FreeType text renderer opened and parsed a font file on
every redraw and never released it — one `FT_Library` and one font face,
abandoned each time. Not a single leak and not bounded by anything an
operator does: the idle screen redraws whenever its content changes, which
includes the box's own IP address, so even a DHCP renewal on an otherwise
quiet box would trigger it. A box left running for days between events
would have accumulated this the whole time.

Confirmed both ways before calling it fixed: a standalone harness built
against the released v0.1.6-alpha source called the renderer 3,000 times and
watched memory climb continuously, still rising when the run ended; the same
harness against this fix plateaus within two calls and stays flat for the
rest. If you have installed v0.1.6-alpha's campus player on a box you intend
to leave running, update it.

Nothing else changes in this release. v0.1.6-alpha's own notes are below.

## What's new since v0.1.5-alpha

### The campus player got a proper identity screen

The Raspberry Pi appliance's idle screen — the first thing a room sees — has
been rebuilt:

- **A QR code to the control page.** Point a phone at the screen and it opens
  the operator UI in the browser. No typing, no laptop; the address is still
  printed alongside for anyone who prefers it.
- **A five-second boot splash.** On power-up the identity screen now shows for
  five seconds whatever is configured — even with auto-play and a live event
  already arriving — so the box visibly proves it is alive before the picture
  takes over. (The box comes in a few seconds into the event; it sits minutes
  behind live anyway.)
- **A modern look.** The screen has a gradient background and anti-aliased text
  rendered from the system font through FreeType, instead of the 5×7 bitmap
  font. A build without FreeType falls back to the bitmap font, so nothing
  here is a hard dependency.

### The one-line installer is now reliable

`curl …/install.sh | sudo bash` has been hardened after GitHub's raw CDN took a
few days off, and after a branch-switch bug made an update fail without a
message:

- The installer **retries its GitHub fetch** and reports what is wrong instead
  of stopping silently — the failure that previously looked like "nothing
  happened".
- It **switches branches correctly** (a plain fetch wrote only `FETCH_HEAD`, so
  the first change of branch failed).
- It **force-updates its tracking ref**, so a shallow clone no longer rejects an
  update it cannot see as a fast-forward.

The command is unchanged in shape, just sturdier:

```bash
curl -fsSL --retry 5 https://raw.githubusercontent.com/stageaudioworks/obs-multisite/main/scripts/player/install.sh | sudo bash
```

### The documentation is restructured

The README was an 800-line wall that tried to be the pitch, the operator
manual, the appliance guide and the developer guide at once. It is now a
landing page that points at four focused guides under `docs/` — the operator,
choosing a satellite and the appliance, public streaming, and building and
testing. Nothing was deleted: the long-form material was moved, and the old
links still resolve.

### Decoder and relay fixes

- **The decoder dock's clock was a third short.** The time base was set by the
  last segment listed, which for a finished recording is the partial fragment
  the broadcast ended on. "Behind live", the timeline axis, the rewindable
  figures and a recording's total length were all wrong by the same factor; the
  estimator now uses the median segment, which one atypical sample cannot move.
  Three smaller faults found while tracing it — a stale playback head across an
  event change, a snapshot that bypassed its own clamp, clipped axis labels —
  are fixed too, and a test locks the estimator to the numbers from the
  recording that exposed it.
- **The playing clock no longer pairs two different fragments.** A jump could
  label the new position with the old fragment's time base; it is latched now.
- **The relay reports its version.** `--version`, the startup log line and
  `/api/status` now say what is actually deployed — which matters for a
  container you cannot simply look at.
- **ffmpeg's stderr is redacted.** The command line was redacted, but ffmpeg
  echoed the full output URL — stream key and SRT passphrase included — back in
  its error message, which reached the browser and the container log. It no
  longer does. The message keeps the hostname and the reason, so nobody has to
  open a support call to find out where the problem was.

## Installing / upgrading

**The plugins.** Builds attach to each release, one per platform, built against
OBS **32.2.2**. A different major version may not load them.

- **Windows** — unzip and copy `obs-plugins` and `data` into the OBS install
  directory (typically `C:\Program Files\obs-studio\`), merging with what is
  there.
- **macOS** (Apple Silicon) — move `obs-multisite.plugin` into
  `~/Library/Application Support/obs-studio/plugins/`, then clear the
  quarantine flag before restarting OBS (these builds are not signed yet):

  ```sh
  xattr -dr com.apple.quarantine ~/Library/Application\ Support/obs-studio/plugins/obs-multisite.plugin
  ```

- **Linux** — place `obs-multisite.so` in the OBS plugin directory
  (commonly `~/.config/obs-studio/plugins/obs-multisite/bin/64bit/`) with the
  contents of `data/` alongside.

Restart OBS. The encoder appears as an output and the decoder as a source, with
**Multisite Encoder** and **Multisite Decoder** docks under View → Docks.

You need an S3-compatible bucket and a key that can read and write it. For the
decoder's event list the key also needs `s3:ListBucket` — Cloudflare's "Object
Read & Write" token includes it, an object-scoped token does not, and the dock
will say so rather than showing an empty list.

**The campus player** installs and updates with one command on stock Raspberry
Pi OS Lite (64-bit):

```bash
curl -fsSL --retry 5 https://raw.githubusercontent.com/stageaudioworks/obs-multisite/main/scripts/player/install.sh | sudo bash
```

Updating is the same command again; the settings and the segment cache are
kept.

**If this box was set up with the earlier licensed virtual sound card**, take
that stack off as part of this update. The player used to recognise that card by
name and write straight into its daemon's memory; it no longer does, so the card
is opened like any other — and that driver pins an eight-millisecond buffer that
cannot hold a decoded frame, which is exactly where the old "sound has broken
up" came from. Updating without doing anything about it is the one combination
that sounds worse than before: the card is still registered, so it is still the
device the player is told to open, and it is still a device that cannot keep up.

Either of these settles it, and both are one command:

- **Put the sound on the network the open way** — `scripts/player/merging-aes67.sh`,
  as described above. Run the script under `scripts/player/` that removes the old
  install first; it also puts the player's sound back on its ordinary output, so
  nothing is left pointing at a card that is about to disappear.
- **Or leave the sound on HDMI.** Set **Sound → Output device** back to
  *default* under Settings. The sound then leaves by the HDMI socket as it did
  before and the old stack sits idle — registered, running, and being written
  nothing.

## Known gaps

- **Not yet used for a real event.** The soak covered sustained upload,
  timeslipping and playout. It did not cover a room full of people, a volunteer
  under pressure, or a venue's network on a Sunday.
- **The campus player has not carried an event either**, though it now holds
  30 fps on a Pi 5 through several runs, with a handful of dropped frames.
- **Alignment between separate audio tracks is unverified.** Audio stays locked
  to the picture — measured, and checked by ear — but nobody has confirmed that
  a click on one track lands at the same instant as the programme on another.
- **Routing packed channels to separate outputs is out of scope**, not
  pending. In OBS,
  [atkAudio's plugin suite](https://github.com/atkAudio/PluginForObsRelease)
  does it — and more — better than a de-interleaver of ours would have; it is a
  separate install under AGPL-3.0. On the appliance, one chosen track is played
  when fed multi-track, and packed channels go out of HDMI in order.
- **AES67 audio is installed but has not been through an event.** Eight channels
  arrive cleanly at a bench Pi, so the card, the daemon and the player's plumbing
  do work together. What is unmeasured is lip sync across a two-hour service, and
  the PTP accuracy a Pi's network interface can reach with no hardware
  timestamping. A PTP master must also exist on the network or nothing flows —
  the daemon slaves to a clock, it does not hand one out.
- **The relay has pushed live streams to YouTube** but has not been through a
  full event.
- **The relay was not part of the elapsed-time change.** Positions are now
  elapsed time throughout the encoder, the decoder, the docks and the appliance;
  the relay was not revisited, so anywhere it reasons about time of day is
  unreviewed rather than known-good.
- **The encoder stamping a cue with its position is untested.** The behaviour is
  straightforward and the round trip through storage is covered, but nothing in
  the suite asserts that the encoder writes the right value in the first place.
- **HEVC to a streaming site needs the site to speak Enhanced RTMP.** YouTube
  documents H.264, H.265 and AV1 for RTMP/RTMPS ingest, so an HEVC feed reaches
  it unchanged; somewhere that has never implemented the extension drops the
  stream as it starts. SRT carries HEVC regardless, so there is still somewhere
  to send it. No HEVC event has yet gone out from a real encoder to a real
  destination, so rehearse it before relying on it.
- **AV1 is sent over RTMP, but only a destination that documents AV1 ingest will
  take it** — YouTube does, and nothing else obviously does, so the page warns
  before you start. AV1 over SRT is refused because ffmpeg has no AV1 in
  MPEG-TS to send.
- **The relay does not terminate TLS.** It binds to localhost and expects a
  proxy in front of it; a working Caddy config is included.
- **Replaying a past event is a proof of concept** — one at a time, started
  by hand, with no scheduling.
- **AV1 is carried, round-tripped by a test, and has now carried a real event** —
  to YouTube, through the relay. What is still unmeasured is the campus tier: no
  Pi decodes AV1 in hardware, so an appliance would be decoding it in software.
  Seeking is accurate to about a
  second, not to a frame.
- **The appliance is still missing DeckLink SDI output** and Pi 4
  hardware-decoder selection.
- **A size in *Manage storage…* can read "unknown".** A measurement that failed
  is reported that way deliberately rather than as `0 B`. Refresh the listing to
  try again, and note that nothing is ever offered for deletion on the strength
  of a size that could not be measured.

## A note on how this release was made

v0.1.6-alpha was produced under a different authoring workflow — VS Code with
Deepseek V4-Flash/Pro — and its memory leak is what this release exists to
fix, reviewed and confirmed back under Claude Code. Whichever tool drafts a
given release, the code, tests and documentation remain human-checked.

A good bug report is a real contribution — much of what works well here was
fixed because someone took the time to paste a log.
