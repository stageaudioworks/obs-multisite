# Upload delivery assurance: what happens when a segment fails

Review of 2026-11-22. Question asked: compared with Resi's RSP, how can we make
sure that what is encoded reaches the cloud and every campus, and how can we
show prospective users that it does? This file maps every failure path as the
code stands, names where the guarantee really breaks, and ranks the fixes.
Authority for the design is `PROJECT-SCOPE.md` §4.7, §5 and §5.1. Patent limits
are in `docs/adr/0003`. Read item 1 there before touching any retry code.

## Status (2026-11-22, branch `claude/protocol-reliability-uploads-6db510`)

The failure map below describes the code **before** this branch. What the
branch built:

| Item | State | Pinned by |
| --- | --- | --- |
| R1 delivery receipt | Built, encoder side: `receipt.json` at End, rewritten by EventFinisher, shown in the dock | `test_receipt` |
| R2 spool sized from free disk | Built, with "time left if the internet drops" in the dock | `test_spool_budget` |
| R4 uploader fixes, Content-MD5 | Built | `test_upload_integrity`, `test_s3_put_integrity` |
| R5 segment groups (index + parity) | Built, encoder and campus (and the relay, which reads the same way) | `test_parity` |
| R7 soak | Software half built: 1,500 segments, outages of 5/30/90 min, store faults → 1500/1500 at both campuses | `test_soak` |
| R3 local archive | Not built: R5 repairs; the archive was only ever a last resort |  |
| R6 live-first catch-up | Not built: a protocol bump, worth it only if E2 shows up in the field |  |

Also found and fixed on the way: a campus more than 50 segments behind live
stored segments **unchecked** (checksums came only from the manifest's window;
the group index now covers the whole event), and the dock read the mirror's
status only when LAN delivery was on.

Still open:
- **Hardware half of R7.** Real links, real disks, OBS, and Content-MD5 accepted
  by each store we support (R2, B2, others). Only a loopback server has seen
  the header so far.
- **Groups for backlogs finished after End.** EventFinisher uploads segments
  but forms no groups for them, and does not upload group objects left on disk
  by an End that ran out of time (the next Go Live sends those).
- **Groups are not copied to the mirror bucket.**
- **The campus half of the receipt** — what each campus received — belongs to
  the collector in multisite-cloud: the campus counts (`rebuilt`,
  `verified_by_group`, `gap_skips`) are in `DecoderSession::Stats` but not on
  the heartbeat yet.
- **Patent counsel** on FEC over segments (R5), before release.
- **On screen.** The dock's new lines are built with Qt but not yet seen in OBS.

## 1. RSP compared with ours

Resi's public description of RSP (marketing copy, not a spec): the encoder
captures and stores video before sending it, then caches, verifies and resends
until the cloud confirms it. They also quote about 90 s to 2 min of delay to
social platforms. Sources: resi.io/rsp, resi.io/features/rsp.

| Property | RSP (as marketed) | obs-multisite today |
| --- | --- | --- |
| Store before send | Yes | Yes: spool, atomic write then rename, one folder per event (§5) |
| Retry until confirmed | Yes | Yes: backoff with jitter, `max_attempts = 0`, strict order (`retry_uploader.cpp`) |
| Integrity check | "Verified" after upload, then resend | Checked by the store **inside** the PUT (signed `x-amz-content-sha256`). Decoders check SHA-256 from the manifest. No test after a 2xx, by design (ADR-0003 §1) |
| Survives an encoder crash | Claimed | Yes: resume continues the same event (§5.1) |
| Finishes after End | Not stated | Yes: `EventFinisher` uploads the backlog, including after a relaunch |
| Second copy | Not stated | Optional mirror bucket, plus `mirror_verify` comparing checksums |
| Receiver fallback | Not stated | LAN first, then cloud; second bucket when the primary misses (`mirror_read_transport.h`) |
| Proof of delivery shown to the customer | Not stated | **None.** This is the gap that matters for sales (R1) |

The architecture is already store-and-forward, the same idea RSP sells. What
we lack is (a) a few places where the guarantee quietly lapses, and (b) any
artefact that *shows* a customer that delivery was complete.

## 2. "Selective re-upload": what is safe to build

Two different things go by this name:

- **Re-sending what was never confirmed.** We already do this. Every segment
  without a 2xx stays in the spool and is retried, in order, for ever. This is
  ordinary retry and is safe.
- **Testing a segment after a successful upload, then re-sending it.** This is
  the claimed element of Resi 9,602,846 / 9,936,228 / 10,327,013, which run to
  2036-08-31. ADR-0003 §1 says **never** let a check after a 2xx trigger a
  re-upload. An end-of-event "list the bucket and re-send what's missing"
  job is the same pattern and needs counsel's opinion first.

Any repair of a segment that *was* confirmed has to come from somewhere other
than the uploader testing its own work. In the plan below, that "somewhere" is
parity data the campus uses to rebuild the segment (R5). Nothing is re-sent.

**A clean-room implementation does not help here.** Clean room protects against
copyright and trade-secret claims, by proving nothing was copied. A patent
covers the method however it was arrived at, so an independent reinvention
still infringes. What works is a design-around: delivering the same result
without one element of the claim.

### Design-arounds considered

Resi's upload claim needs all three of: (1) an upload thread sends a segment,
(2) runs an integrity test **after a successful upload**, (3) **repeats** the
upload if the test fails.

| Route | Element removed | Decision |
| --- | --- | --- |
| **A.** The store verifies the checksum inside the PUT | 2: a damaged upload is never "successful" | **Adopted** (R4) |
| **B.** Send every segment to two buckets, always | 2 and 3 | **Optional only.** The guarantee must not require a second bucket (ADR-0004) |
| **C.** Parity objects every K segments, so the campus rebuilds a bad segment | 3: nothing is ever re-sent | **Adopted** (R5) |
| **D.** Campus reports a bad segment, then the operator re-sends it from a local archive | Arguably 2, but the same function done in a similar way | **Not built.** Doctrine-of-equivalents risk. Needs counsel, or wait until 2036 |

## 3. Failure map

Each row: what fails, what the code does, what the campus sees, and whether
anything is lost.

### Encoder side

| # | Failure | What happens | Campus sees | Lost? |
| --- | --- | --- | --- | --- |
| E1 | PUT times out, 5xx, 408, 429, or network down | Retried with backoff 250 ms → 15 s cap, ±30 % jitter, for ever. Link shows Degraded, then Offline. One log line at the start and one at recovery | Waits at the edge ("gaps_waited"), resumes when it lands | No |
| E2 | Long outage | Spool grows on disk. Live campuses fall behind, because upload is strictly in order and the backlog drains oldest first | Falls behind live by the length of the outage, then catches up only as fast as spare uplink allows | No, until E3 |
| E3 | Spool hits `max_spool_bytes` (default **4 GiB**) | **Oldest unconfirmed segment is deleted** and listed in `gaps` | Skips the range at once | **Yes, permanently.** Arithmetic, not measured: 4 GiB lasts about 72 min at 8 Mbit/s total, 95 min at 6 Mbit/s |
| E4 | Spooled file damaged on disk (power cut, bad sector) | Checksum taken at capture no longer matches, so it is not uploaded and is listed as a gap (`damaged_skipped`) | Skips it | Yes, that segment |
| E5 | 4xx other than 408/429 (wrong key, wrong bucket, clock skew) | Logged as "refused permanently, uploading has stopped". **In fact `run()` sleeps 200 ms and tries the same segment again**, logging an ERROR each time: about 5 requests and 5 log lines a second until fixed | Live edge stops | No, if fixed before E3. Needs a test to confirm the loop behaviour |
| E6 | Store refuses because bytes were damaged in transit (S3 `XAmzContentSHA256Mismatch` / `BadDigest`, both HTTP 400) | Falls into E5: correct outcome (retried), but the message blames credentials | Brief wait | No |
| E7 | Manifest PUT fails | Coalesced. The next confirmed segment republishes it. At End, a failure sets `end_unpublished` and the finisher publishes later | Slightly late edge | No |
| E8 | OBS or machine crashes | Spool survives. Restart within 30 min resumes silently. Later, it asks first | Gap in time, no gap in sequence | No |
| E9 | End pressed with a backlog | Drains to a deadline, then `EventFinisher` uploads the rest in the background (also after relaunch) | Recording becomes complete later | No |
| E10 | Operator discards a backlog, or the encoder's disk dies before upload | Gone. Spool is the only copy (OBS's own recording aside) | Gap | **Yes** |
| E11 | Store returns 2xx but did not keep the object, or keeps damaged bytes | First 3 uploads checked by HEAD (diagnostic note only). After that nothing, by design. Spool file is deleted on confirm, so **no local copy exists to repair from** | Refuses it 3 times behind the edge, then skips | Yes today. Very rare. R5 (parity) would let the campus rebuild it |
| E12 | Mirror bucket down | Mirror yields; primary unaffected. Catches up later | Nothing | No |

### Bucket and campus side

| # | Failure | What happens | Lost? |
| --- | --- | --- | --- |
| C1 | Campus link down | Holds position; resumes from cache. A campus on timeslip rides it out if its buffer is deep enough | No |
| C2 | Download fails checksum | Re-fetched. Behind the edge, 3 refusals means skip | Only if the bucket copy is bad (E11) |
| C3 | Primary bucket missing an object | Read from the second bucket if configured | No, with mirror |
| C4 | Retention expiry (7-day lifecycle rule) | Old events deleted by design | By design |

## 4. Where "everything arrives" is not yet true

Ordered by how likely each is at a real church:

1. **E3, the 4 GiB cap.** A Sunday-morning outage of just over an hour throws
   away the start of the service on an encoder that probably has hundreds of
   GB free. This is the most likely real loss.
2. **E2, catch-up after an outage.** Nothing is lost, but a live campus is
   stuck behind the backlog for as long as the outage lasted, and longer on a
   slow uplink. From the campus that can look like a failure.
3. **E11.** Once a segment is confirmed, the spool deletes it, so a bucket
   copy that is lost or damaged cannot be repaired. Parity (R5) fixes this
   without a second bucket. **E10**, the encoder's disk dying before upload,
   stays a loss: only the encoder ever held those bytes.
4. **No proof.** Even when every segment arrives, no one can show it.

## 5. Recommendations, ranked

**R1. Delivery receipt (do first: it is the sales answer).** At End, and again
when the finisher completes, produce a per-event report: segments captured,
confirmed in primary, confirmed in mirror, gaps with reason (evicted, damaged
on disk), and per campus how many segments it received or skipped (campuses
already send a heartbeat to the collector). Show it in the encoder dock and in
multisite-cloud: "Service 09:00: 1,200 / 1,200 segments in cloud, 4 / 4
campuses complete". Built from `mirror_verify`, spool counters and heartbeats.
Reporting only, so no ADR-0003 issue.

**R2. Size the spool cap from free disk, not a fixed 4 GiB.** For example,
keep 20 GB or 10 % free, whichever is larger, using `disk_health`. Show "time
until segments are dropped" in the dock while there is a backlog. Turns E3 from
about an hour into many hours on a normal machine.

**R3. Keep confirmed segments locally for the retention window.** A local
archive (same folder, files moved rather than deleted) gives a full local
recording of what was sent. It is a recording and a last resort, **not the
repair path**: R5 repairs. Nothing re-sends from the archive automatically. Any
re-send from it is route D in §2, which needs counsel first.

**R4. Small correctness fixes in the uploader.**
- E5: back off on permanent errors too (for example 30 s), and log once per
  change of state, not every 200 ms. Make the message match: "retrying every
  30 s".
- E6: class `XAmzContentSHA256Mismatch`, `BadDigest` and `RequestTimeout` as
  retryable transmission errors, with their own message.
- Add `x-amz-checksum-sha256` to segment PUTs. This is the hardening ADR-0003
  already names: the store checks in-request even where it ignores the signed
  hash. Check R2 and B2 accept it first.
Each needs a failing test first (standards §5).

**R5. Parity objects in the same bucket (route C).** After every K segments
(fixed, default 10), the encoder uploads `events/{id}/parity/{group:08d}.bin`.
That object is the XOR of the group's segments padded to the longest one, plus
each segment's length. The manifest lists each parity object, but only once it
is confirmed (§4.7). That is a new optional field, so there is no
`protocol_version` bump (§4.8).
- **Rebuilding a segment.** When a campus would skip a segment it was refused
  behind the live edge (missing, or failing its checksum), it first fetches
  the other K−1 segments and the parity, XORs them, and checks the result
  against the manifest checksum. The checksum identifies which segment is bad,
  so this is an erasure and XOR can recover one bad segment per group. Use
  Reed-Solomon only if the field ever shows two bad segments in one group.
- **Cost.** About 1/K more upload and storage (10 % at K = 10). Parity
  waits in the spool behind its group, at the same priority as segments.
- **Where it applies.** Only behind the live edge, because a group's parity
  exists only once the whole group is uploaded. That is fine: at the edge a
  missing segment is still uploading, not lost.
- **Patent limits.** The uploader runs no test and nothing is re-sent. K is
  fixed or configured, never adjusted to measured latency or loss, which keeps
  it clear of ADR-0003 §5. Counsel should check FEC-over-segments patents
  before release.
- **Groups with a gap.** A group containing a listed gap (E3, E4) still gets
  parity over the segments that exist. A listed gap is never rebuilt, because
  its bytes were never uploaded.

**R6. Live-first catch-up after an outage.** When the backlog passes N
segments, upload the newest first so campuses return to live. Then backfill
holes behind it. This **breaks a protocol assumption**: §5 says that behind the
edge a refusal is final because uploads are in order, so an old decoder would
skip a hole that is merely pending. It needs a `pending` range list in the
manifest and a `protocol_version` bump (§4.8). It also needs the write-ordering
rule in §4.7 restated per segment. Use a fixed number of in-flight requests
only, never threads in proportion to the backlog (ADR-0003 §2). Biggest change
here. Do it after R1 to R4, and only if E2 shows up in the field.

**R7. Prove it with a soak test.** Run a 2 h test event and cut the uplink
with `netem` / pfctl for 5, 30 and 90 min. For R5, delete or corrupt segments
in the bucket by hand and check that the campus rebuilds them.
- **Pass.** The receipt says N / N in the cloud and at every campus, or names
  exactly the gaps the cap caused.
- **Then publish the measured result.** That is the claim we can put in front
  of a customer.

### The mirror after this plan

The second bucket stays supported and optional. With A + R5 in place, one
bucket gives the full delivery guarantee. The mirror adds one thing only:
protection against the provider being down longer than a campus's buffer, or
losing data at scale. That is the honest limit of a single bucket. Before
quoting a provider's durability or uptime figures to a customer, check them
against that provider's current terms. See ADR-0004.

## 6. What we can honestly tell a prospective user

With this branch released, and once the hardware half of R7 confirms it:

> Every segment is written to the encoder's disk before it is sent, and is
> retried until the cloud confirms it — through outages, crashes and power
> cuts, and after the service has ended. The encoder keeps as much backlog as
> its disk can spare, which on a typical machine is many hours, and shows how
> long that is while you are live. The cloud checks each upload's fingerprint
> as it arrives, and every campus checks every segment again before playing.
> If the cloud ever loses or damages a stored segment, each campus rebuilds it
> from recovery data sent alongside, with no second storage account needed.
> After every service you get a receipt saying how much of it is in the cloud.

What it must not say: that nothing can ever be lost. An encoder disk that
fails before upload loses what only it held, a disk that fills drops the
oldest unsent minutes (and the receipt says so), and a provider outage longer
than a campus's buffer is covered only by the optional second bucket.

Figures: "many hours" follows from the free-disk cap (spool_budget.h) and is
arithmetic until the hardware soak measures it; 10 % is the parity overhead
at k = 10. The software soak's numbers are in the R7 commit, not here.
