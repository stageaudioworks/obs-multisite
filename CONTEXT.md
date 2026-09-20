# Domain glossary

The words this project uses, and uses precisely. Where a term has a settled
design behind it, the authority is named rather than restated — `PROJECT-SCOPE.md`
for how a thing is meant to work, `BUGS.md` for what went wrong when it didn't.

This file exists because vocabulary failures are this project's most expensive
bug class. Three of the four faults found in the week of 2026-09-19 were a value
in the wrong frame that type-checked perfectly: a media time subtracted from a
Unix epoch, a skip origin shared between two streams that do not share one, and
positions that legitimately start at zero being tested with `if (!x)`. None was
a logic error. Each was a word meaning two things.

## Time

**Media time** — how far into the programme a moment sits, in milliseconds from
the start of the event. A frame's own presentation timestamp. This is the ONLY
kind of position the system reports: the dock, the player page, the API and the
cues all speak it. Starts at **0**, which is a real value and not a missing one.

**Wall time / time of day** — a Unix epoch. Deliberately removed as a position
concept (BUGS #2b, #2c). It survives in exactly two places, both marked: legacy
cue compatibility (`Marker::at_ms`) and the external control API's
`seek_to_wall_ms`, which Companion speaks. **Never subtract one from a media
time.** If a readout ever shows roughly *now* in some unit, that is what has
happened (BUGS #5).

**Playhead** — the media time of the frame currently going to air.

**Live edge** — the media time of the newest published segment. What "behind
live" is measured against; both sides of that subtraction are media times.

**Behind live** — live edge minus playhead, in seconds. Meaningless for a
finished recording, and not shown for one.

**Timeslip** — a campus deliberately playing behind the main site, so a local
service can start later. The reason the decoder is a DVR rather than a viewer.

## Media

**Segment** — the unit of transfer: one published, immutable chunk of programme,
identified by a sequence number. Segments are the unit of transfer and **not the
unit of seeking** — a seek lands on the moment inside one (BUGS #3).

**Fragment** — the CMAF `moof`+`mdat` inside a segment. Near-synonymous with
segment in conversation; use *segment* for the stored object and *fragment* when
the byte layout is what matters.

**Init segment** — the `ftyp`+`moov` header a decoder needs before any fragment
makes sense. Written once per event.

**Manifest** — `manifest.json`, the rolling window of recent segments at the live
edge. A finished event's older segments outlive their entry in it.

## Structure

**Room** — an ongoing channel, e.g. `main-auditorium`. Persists across events.

**Event** — one broadcast of a room, identified by a ULID. Has a start, an end,
and its own immutable descriptor.

**Site** — a machine's name for itself, stamped on every cue it drops. Empty
reads as the main site.

**Main site** — the location that encodes and publishes. **Satellite** /
**campus** — a location that receives. The plugin is one binary; which role a
machine plays is a setting.

**Appliance / campus player** — the Raspberry Pi receive box. Same core, its own
video and audio output, a web page instead of OBS.

**Relay** — the simulcast service. Reads from storage exactly as a campus does,
and pushes to YouTube or SRT. It is a **peer of a campus, not upstream of one** —
which is why it cannot caption them (PROJECT-SCOPE §8.8).

## Cues

**Cue** — an operator-facing mark on the timeline: "Sermon Start", "Go to local".
Called a **Marker** in code (`struct Marker`). Anchored in media time
(`at_media_ms`); `at_ms` is the legacy time-of-day anchor, read only as a
fallback.

## Delivery

**Spool** — the encoder's durable store-and-forward queue. Survives a crash.

**Playout clock / playout base** — when a decoded frame is due to go to air,
derived from its pts. Distinct from media time: this is real time, for pacing.

**Epoch** — a generation counter. `timeline_epoch` changes on a seek,
`media_epoch` on a new event; a frame carries the epoch it was stamped under so
one from a timeline already left can be discarded (`playout_timeline.h`).

**Skip** — the sub-segment part of a seek: frames before the requested moment are
dropped. **One arm per stream** — audio and video do not share an origin
(`seek_skip.h`, BUGS #2d).

## Sentinels

Sequence numbers, positions and media times **start at 0**. `0` is therefore a
real value everywhere in this codebase, and `-1` (or `INT64_MIN`, or `null`) is
what absent looks like. `if (!x)` is not a test for missing — it is a test for
*missing or at the very beginning*, and the second is usually the ordinary case.
This trap has been hit six times; see BUGS #4 and #5.
