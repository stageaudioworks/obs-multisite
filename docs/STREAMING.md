# Streaming to the public

The campuses are not always the only audience. `relay/` is a small self-hosted
event that reads the same segments and pushes them out to YouTube, Facebook,
or any RTMP destination — and over SRT to anywhere that prefers it.

It relays from the bucket rather than adding a second output to OBS, which
matters twice over. The main site uploads once whether the event is going to
two campuses or to two campuses and the internet — often the difference between
possible and not on a venue connection. And the public stream inherits the
buffering the campus feed already has: it runs a few minutes behind on purpose,
so a dropout at the main site delays it rather than breaking it.

```bash
docker run -d --name multisite-relay \
  -p 8080:8080 \
  -v multisite-relay-data:/data \
  -e RELAY_ROOM=main-auditorium \
  ghcr.io/stageaudioworks/multisite-relay:latest
```

`:latest` follows `main` and is rebuilt whenever the relay changes; tagged
releases also get a `vX.Y.Z` image. `relay/` builds the same image locally if
you would rather not pull it.

Then open it in a browser, put in the bucket details, and add a destination.
A $5/month VPS is the target rather than a stretch, because nothing is being
re-encoded.

- **It has a login, and binds to localhost by default.** This event decides
  where your events are sent, so exposing it is a decision rather than a
  default. Put HTTPS in front of it; `relay/Caddyfile.example` (or
  `relay/nginx.conf.example` if you run nginx) is a working
  config.
- **One chosen sound feed per destination**, picked by the name the main site
  gave it — "Main Mix", "Sermon ISO" — never a track number. A future
  "clean feed to Facebook, main mix to YouTube" is just two destinations.
- **A delay you choose**, three minutes by default. This is the setting worth
  understanding: it is how much of the event the relay holds in hand, and so
  how long an outage at the main site can last before the public sees it.
- **It reconnects by itself** and resumes from where it stopped, so nothing is
  skipped. A silence under 45 seconds is ridden out without even dropping the
  connection. It watches both directions: content failing to arrive from the
  main site and content failing to leave for the destination look identical to
  ffmpeg, which reports neither, so the relay notices both itself and says
  which one happened.
- **RTMP or SRT, decided by the address you paste.** There is no protocol
  setting: `rtmp://` and `srt://` are unmistakable, and asking a volunteer
  which one they were given is asking them to get it wrong. SRT can also
  *listen*, for a broadcast partner or hardware decoder that pulls from you
  rather than being pushed to — written down by leaving the host out of the
  address, `srt://:9000`, which is deliberately the only way to ask for one,
  because it opens a port on a machine otherwise kept closed.
- **HEVC goes out unchanged, either way.** Over SRT it travels in MPEG-TS,
  which has carried HEVC for a decade. Over RTMP it travels as **Enhanced
  RTMP** — an extended video tag carrying the codec's FourCC — which YouTube
  documents alongside H.264 and AV1 for RTMP/RTMPS, and recommends over
  RTMP(S) for HDR. Choosing HEVC for the campuses no longer costs a church any
  part of its public stream. A destination that has never implemented Enhanced
  RTMP will drop the stream rather than complain usefully, so an HEVC push
  somewhere new that dies immediately is worth trying over SRT instead.
- **It refuses rather than guesses.** Packed multi-channel audio is declined
  with a plain explanation, because sending it onward means a mic ISO going out
  to the public. Two codecs used to be in that list and are now handled per
  destination instead: HEVC and AV1 both travel over RTMP as Enhanced RTMP, so
  what matters is whether the far end takes them — and the page says so before
  the stream starts rather than after it dies. AV1 over SRT is still refused,
  and that one is ffmpeg's doing rather than a policy: MPEG-TS has no AV1 stream
  type to put it in.

### Can one service go out in several languages?

Yes, and from the same upload. The interpreter feeds from the desk arrive as
separate OBS audio tracks — each one named — and the relay sends **a different
track to each destination**: English to one YouTube stream, Spanish to another,
a clean feed to a broadcast partner. Every destination reads the same segments
from the same bucket, so the main site uploads once no matter how many languages
there are, which is the whole point when the service is going out from a place
with a poor connection. Nothing is re-encoded on the way.

What it does not do, stated plainly, because the answer above sounds better than
the whole truth:

- **The video is the same on every destination.** Only the sound can differ —
  there is no transcoder here, so there are no per-language bitrates and no
  adaptive ladder.
- **Each language needs its own stream key or channel at the far end.**
- **Every destination costs the relay's own uplink another copy of the bitrate.**
  Three 1080p languages is roughly three times the egress from that server. The
  page shows the total going out for exactly this reason.
- **There is no in-browser player.** "Web" today means handing the stream to
  YouTube or Facebook, or to an SRT receiver. Playing from the bucket into a
  browser is planned and not built.

It also does two things with events that have already finished:

- **Download one as an MP4**, streamed straight from storage — nothing is
  assembled on the server, so a two-hour event costs no disk. The file
  carries every audio track the main site sent, not just the streamed one, so
  the ISOs and the click are there for whoever edits it.
- **Replay one to a destination** as though it were happening now, for a
  second congregation or an evening repeat. Two of the event's cues can be
  chosen as in and out points, so the replay can be an excerpt rather than the
  whole recording. This is a proof of concept: one at a time, started by hand,
  no scheduling yet.

Full deployment notes, including bandwidth and disk, are in
[relay/README.md](../relay/README.md).
