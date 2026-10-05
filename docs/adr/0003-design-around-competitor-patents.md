# ADR-0003: Seven features are built around competitors' patent claims, not along them

**Status:** Accepted, pending a freedom-to-operate opinion from patent counsel
**Date:** 2026-10-05

## Context

A patent search in October 2026 found live US patents close to this design,
held mostly by our two direct competitors in church multisite: Resi Media
(formerly Living As One, owned by Pushpay) and BoxCast. The research is an
engineering reading of independent claims, not legal advice. No patent was
found that the code infringes today. In every close case, though, the margin is
one design detail that a reasonable "improvement" would remove. That detail is
why this is an ADR: each item below rules out the option someone will propose
next.

A patent is infringed only when **every** element of one claim is present. To
design around one, you remove a single element and still deliver the function
by a different route. Counsel should also check each route marked *(DoE)*
against the doctrine of equivalents. Under that doctrine, a substitute that
does the same thing in substantially the same way can still infringe.

Some of the prior art bounds what any of these patents can claim, and is worth
handing to counsel:

- draft-pantos-http-live-streaming-00 (May 2009): segments on a plain HTTP
  server, with clients polling the playlist.
- McDonald, ioncannon.net (July 2009): a local segmenter, a threaded upload
  queue to S3, and a polled playlist.
- TiVo's time-shifting patents, expired around 2018–19.
- Haivision Connect DVR (2014): per-venue playout control for church sites.

## Decision

Each feature below keeps its function and drops the claimed element. The
"Never" line is the change to bring to counsel first.

### 1. Upload integrity: the store checks during the upload, the uploader does not test afterwards and resend

*Resi 9,602,846 / 9,936,228 / 10,327,013 (exp. 2036-08-31). Claimed: an upload
thread transmits a container, runs an integrity test **upon a successful
upload**, and **repeats** the upload if the test fails.*

- **How we get the same guarantee.** Every PUT is signed over the real SHA-256
  of its body (`x-amz-content-sha256`, [aws_sigv4.cpp](../../src/core/aws_sigv4.cpp)).
  A store that verifies the hash refuses the request when the bytes are
  damaged. That refusal is an ordinary failed upload, retried like any other,
  so a successful upload is never followed by a test. The decoder checks each
  segment against the SHA-256 recorded in the manifest, and re-downloads on a
  mismatch. That is the receiving side, not the uploader.
- **Hardening, if a store is ever found to ignore the signed hash.** Add
  `Content-MD5` or `x-amz-checksum-sha256` to the PUT, so that store verifies
  in-request too. Detection after the fact belongs to `mirror_verify`, which
  reports a gap for an operator *(DoE)*.
- **The HEAD size check** after the first `verify_first_n` uploads
  ([retry_uploader.cpp](../../src/core/retry_uploader.cpp)) is diagnostic. It
  records a note and nothing else.
- **Never:** make the HEAD check, or any check after a 2xx, trigger a re-upload.

### 2. Upload concurrency is fixed, not scaled to the backlog

*BoxCast 12,155,879 (exp. ~2031). Claimed: a plurality of threads **generated
based on the number of outstanding blocks**, plus validation and resend
(item 1).*

- **Current design.** One upload thread per target, in strict sequence order.
- **If an outage backlog ever needs to drain faster:**
  - Use a fixed number of in-flight requests, set in configuration or from
    measured link throughput.
  - Preferably run them from one thread through libcurl's multi interface, so
    there is no plurality of threads at all.
- The resend in this claim is absent too (item 1), which is a second missing
  element.
- **Never:** start upload threads in proportion to the backlog.

### 3. A download streams as it is built, and never waits for the whole clip first

*Resi 11,412,272 / 11,758,200 / 12,088,859 / 12,610,092 (exp. 2036). Claimed:
add a requested portion's frames or segments to a media file and, **based on a
determination that the file includes the whole portion**, provide it.*

- **Current design.** The relay's event download
  ([relay/README.md](../../relay/README.md), *Download*) starts sending
  immediately, straight from storage, and holds no complete file at any point.
- **For a classic MP4 with its index at the front (faststart):** build the
  `moov` from the segment sizes and timing that the manifest already lists,
  then stream the media data after it. Delivery still starts before the media
  is assembled.
- **For clip export:** the same rule applies. Alternatively, serve the
  requested range as a playlist over the existing segments.
- **Never:** assemble the file on the server and only then hand it over,
  including "to add faststart" or "to make a clip".

### 4. Encryption at rest and a playout delay are never coupled the way the claims couple them

*Resi 11,936,923 / 12,610,093 / 11,405,665 (exp. 2036). Claimed: **encrypted**
segments, and a decoder **disallowed from playing any segment until a
predetermined delay** has passed, the delay measured from when the **server
provided** the segments to the decoder. 11,405,665 instead has the **encoder**
generate the delay that stops the decoder downloading.*

- **Current design.** Segments are not encrypted at rest. A campus's position
  is its own choice (timeshifting, PROJECT-SCOPE §6). The relay's lag behind
  the live edge is a choice of *where to start* in the manifest
  (`start_seq_for_delay`, [relay_state.h](../../relay/src/relay_state.h)). It
  is not a gate on playing segments already fetched, and it is not measured
  from when they were fetched.
- **If encryption at rest is wanted for privacy, build it on these terms:**
  - No playback gate is tied to the time a segment was delivered.
  - Any fixed delay is anchored to capture time.
  - The encoder never controls when a decoder may download.
- **Never:** add encryption together with a fixed delay measured from download
  or delivery time.

### 5. Redundancy sends to every target all the time; it does not switch channels on failure

*BoxCast 12,126,873 / 11,483,626 / 11,330,341 (exp. ~2037). Claimed: a backhaul
protocol sending **N packets, with N chosen dynamically to minimise latency**;
when the first channel fails, **open a second channel** and resend.*

- **Current design.**
  - The unit is a whole object of fixed segment duration (6 s), sent over plain
    HTTP. It is not a packet count tuned for latency.
  - The mirror uploads every segment to the second bucket as standard, so
    redundancy is duplication, not failover.
  - The mirror's yield rule (in `UploaderConfig::may_upload`) only orders the
    work between the two buckets. **That rule is the nearest thing to failover
    in the code. Show it to counsel.**
- **Never:** build a packet-level transport that resizes its batches for
  latency and fails over between connections. Contribution over SRT uses
  unmodified libsrt, which carries MPL-2.0's patent licence from its
  contributors.

### 6. Campus-to-campus sharing goes through a configured source, not peer to peer

*TVU 8,904,456 / 9,860,602 / 11,317,164 (exp. ~Aug 2027). Claimed: dedicated
caching servers, plus receivers that **push availability information to each
other unprompted** and download from **other receivers**.*

- **Current design.** LAN delivery (PROJECT-SCOPE §8.7) pulls from the encoder,
  with the bucket as fallback.
- **If a campus is to re-serve the feed to its neighbours:**
  - Make it a statically configured `LanObjectServer` that the others pull
    from.
  - Have no unprompted availability messages, and no swarm.
- **Never:** build peer discovery or exchange between receivers before
  September 2027.

### 7. Any bitrate adaptation uses local signals, never latency synchronised with the receiver

*Dejero 9,042,444. Claimed: an encoder that changes how much it encodes based on
feedback that includes **network latency determined by synchronising the
transmitter's clock with the receiver's**.*

- **Current design.** We have no adaptive bitrate. Store-and-forward absorbs a
  slow link as backlog instead (PROJECT-SCOPE §1, latency is last).
- **If we ever adapt:** drive it from local measurements only, such as spool
  depth and upload throughput. Or publish a fixed ladder of renditions.
- **Never:** derive an encode rate from receiver clock synchronisation.

## Consequences

- None of the seven features loses its function. Integrity, faster draining,
  downloads, privacy, redundancy, local sharing and congestion handling all
  remain buildable. What each "Never" rules out is one particular way of
  building it.
- **This ADR has an expiry.** Each item can be revisited when its patent lapses:
  - item 6 in late 2027;
  - items 2 and 5 around 2031 and 2037;
  - items 1, 3 and 4 on 2036-08-31.

  Lapsing also includes unpaid maintenance fees, which nobody has checked yet.
  Resi continuations granted in April 2026, and one still unpublished could
  change any of this.
- The research is an engineer's reading of claims, not a legal opinion. Get
  counsel's freedom-to-operate opinion before anything is sold, MultisiteOS
  above all. GPL licensing gives no protection against a third party's patents.
- A change that touches any of these seven areas should cite this ADR in its
  commit, so the reasoning is still visible to the next agent.

## Alternatives considered

**Ignore patents until someone writes.** A common open-source stance, and the
search found no history of Resi asserting its patents. Rejected because the
commercial products built on this code would be the target, and BoxCast has
sued Resi before. The cost of a "Never" line now is small, while finding out
from a demand letter means a redesign under deadline.

**License from Resi or BoxCast.** Rejected as unnecessary while the design-arounds
cost almost nothing, and as unworkable for a GPL project: a patent licence that
does not pass through to every recipient conflicts with GPLv3 §11.

**Wait for the patents to expire.** This is what item 6 does in practice. For
the others, the expiry is 2031–2037, longer than the project can wait.
