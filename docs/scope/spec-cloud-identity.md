# Spec: `cloud-identity`

**Status: proposed, awaiting review. Depends on the capability map**
(`phase12-capability-map.md`), which must be approved first. `cloud-storage`
and `cloud-heartbeat` depend on this module and are specified after it.

## Objective

One owner of "who this device is to the collector". Today a machine has a
pairing identity (settings typed per role, `reporter_*`) and a storage identity
(`S3Config`, key pasted by hand) that share nothing — so a device can heartbeat
under one identity while reading a bucket under another, which is the live
failure. This module makes that unrepresentable: after it, a paired device's
storage credentials and its heartbeat both come from the same enrolment.

It also, by existing, makes Phase 12's user-facing promise true — a volunteer
can connect a bucket by entering a short code on a phone instead of creating a
cloud account, scoping a token and writing a lifecycle rule.

## What it owns

- The collector connection: base URL, appliance id, appliance bearer.
- The pairing state machine (already built: `src/core/heartbeat_reporter.*`,
  `Pairing`) — **reused, not rewritten.**
- The credential lifecycle: `GET {collector_url}/v1/credentials` with the
  appliance bearer, on boot and again before `expires_at`, refreshing at
  roughly half the TTL.
- What "paired" means as a value other modules consume — not three loose
  strings.

## What it does not own

- The `Transport` that uses the credentials (`cloud-storage`).
- The heartbeat cadence and payload (`cloud-heartbeat`, which re-homes onto
  this module's connection but keeps `heartbeat_reporter`'s wire contract).
- Which bucket: never guessed. The collector's reply is the answer.

## Interface at the boundary (shape, not final signature)

`cloud-identity` exposes, to `cloud-storage` and `cloud-heartbeat` alike:

- **paired** — a discriminating state, not `!url.empty()`. Unpaired and
  paired-but-stale are different states and must not collapse.
- **current credentials** — bucket, endpoint, `session_token`, `expires_at`,
  and the role granted (read vs read/write), or the last-good set with the
  staleness marked.
- **the collector connection** — URL and bearer, for `cloud-heartbeat`.

The exact type is this module's to design; the requirement is that a consumer
cannot see a bucket without also seeing whether the credentials behind it are
live or last-good, and cannot see an identity without its role.

## Wire contract (measured against the live collector 2026-09-22)

```
GET {collector_url}/v1/credentials
  Authorization: Bearer {appliance_token}
  → 200 {
          endpoint,                        // full URL, scheme included
          bucket, region,
          access_key_id, secret_access_key, // the key PAIR
          session_token,                    // + the token that goes with it
          role,                             // "encoder" / "decoder"
          expires_at                        // ISO-8601 UTC string
        }
  → 403 the device was unpaired   → STOP, do not retry
  → 5xx / unreachable             → keep last-good, degrade, never stop the event
```

**This section used to state the reply from the design, and the design was
wrong in two ways.** Both were found by `scripts/probe_credentials.cpp` driving
the real service, before any host depended on them:

1. **`expires_at` is an ISO-8601 string** (`"2026-09-22T08:18:03.586Z"`), not an
   integer of milliseconds. Read as an integer it yielded 0, so every credential
   set was treated as already expired — the fault would have been invisible
   against a mock that spoke our own guessed shape.
2. **The reply carries a key PAIR plus a session token**, not a token alone. A
   consumer must pass `access_key_id`, `secret_access_key` AND `session_token`
   onward; a token with no key pair signs with nothing.

- `access_key_id` + `secret_access_key` sign the request; `session_token` is
  sent onward as **`X-Amz-Security-Token`** and is part of the signed headers.
- `endpoint` arrives with its scheme; `S3Transport` strips it. The returned
  bucket and endpoint are used **exactly as received**; never guessed from a
  typed value.
- `region` is `auto` for R2.
- **Never fall back to a configured key when a fetch fails.** Degrade to the
  last good credentials. A collector outage must not stop a live event.
- **403 means the device was unpaired**: stop, rather than retry. Retrying a
  403 is how a revoked device becomes a stuck one.

## Commands

No new build system. The existing ones, unchanged:

```
cmake --build build                         # core + tests
ctest --test-dir build --output-on-failure  # the suite this must extend
# OBS plugin: cmake --build build-obs --config Release --target obs-multisite
```

## Project structure

Follows the repo's existing split, no new top-level:

```
src/core/cloud_identity.{h,cpp}   the state machine + credential lifecycle (portable, no OBS/Qt/curl)
src/obs/                          host: owns the thread, the HTTP, the settings
src/appliance/                    host: same, plus one status word on the identity screen
tests/test_cloud_identity.cpp     the core's arithmetic and state transitions
```

The core owns the state and the arithmetic; hosts own the network and the
clock, exactly as `heartbeat_reporter` does. This is what keeps it testable
without a collector.

## Code style

Follows `docs/CODING-STANDARDS.md`. Two of its rules bite hardest here:

- **§2 one authority per quantity.** The appliance id/token must have exactly
  one home. Today `reporter_appliance_id`/`reporter_token` are per-role
  settings; this module must not add a third copy.
- **§11/§10.** Prose in the entry/archive, not the header.

A real snippet showing the shape the state must take — a consumer cannot get
credentials without the liveness:

```cpp
// Not this — three strings that can disagree:
//   std::string bucket, endpoint, session_token;

// This — the liveness travels with the values:
struct Credentials {
    std::string bucket, endpoint, session_token;
    long long   expires_at_ms = 0;
    bool        read_write = false;
    bool        from_last_good = false;   // a fetch failed; this is the old set
    bool        live(long long now_ms) const {
        return !from_last_good && now_ms < expires_at_ms;
    }
};
```

## Testing strategy

`tests/test_cloud_identity.cpp`, in the core suite (no network, no OBS):

- TTL arithmetic: refresh is scheduled at ~half the TTL, not at expiry.
- A failed fetch keeps last-good and marks it; it does not clear credentials.
- A 403 moves to unpaired and **stops** (no scheduled retry).
- `live()` is false for last-good and for past-expiry, true only for a fresh
  set — the discriminator `cloud-storage` depends on.
- Even the download, if it lands as a `FunctionOrString`, is handled.

Per standards §5, each case is verified to fail against the behaviour it
replaces where a prior behaviour exists.

## Boundaries

- **Always:** never stop a live event for a credential problem; never guess a
  bucket; keep the last-good set rather than clearing it; hold the appliance
  id/token in exactly one place.
- **Ask first:** changing the `/v1/credentials` response shape (it is a
  contract with the collector); adding a settings field; touching the pairing
  state machine's transitions.
- **Never:** fall back to a typed key when paired; retry a 403; send the
  bearer anywhere but the configured collector; log the bearer or the
  `session_token`.

## User experience (acceptance criteria)

This is what the module must make true on screen. It is not decoration —
"brokered must not come to mean opaque" is a constraint from §8.5, and an
operator must be able to answer "what am I connected to, and is it working"
without reading a log.

**Encoder and decoder docks, Storage tab (Qt).** The provider dropdown gains
the no-longer-greyed **Multisite Cloud** entry. Selecting it replaces the
key/secret fields with:

| State | What is shown |
|---|---|
| Unpaired | `Connect…` button; pressing it shows the code and the verification URL, e.g. `Enter this code at app.multisite-cloud.…: JNB-4K7M` |
| Waiting | the code, a Cancel button, and that it is waiting for approval |
| Paired | `Connected to: Multisite Cloud` · `Bucket: <from the broker>` · `Endpoint: <from the broker>` · `Role: Encoder — reads and writes` / `Decoder — reads only` · `Credentials: refresh every N min` · a `Disconnect` button |
| Degraded | `Credentials: last good — refresh failing (collector unreachable)` and the remaining validity, **with the event still running** |
| Unpaired by the collector (403) | says the device was unpaired and that it has stopped; offers `Connect…` again |

Wording follows the existing locale file (`data/locale/en-GB.ini`,
`Dock.Reporter*`); new strings go there, not inline.

**Appliance HDMI identity screen — one word, and nothing else.** The screen
may gain a single status line saying **"paired to Multisite Cloud"** (or not,
when it isn't). That is the entire change.

**Explicitly out of scope: the QR code and the landing page.** Neither is
touched. The QR tile already encodes the box's own address for its
remote-control page, and the landing behaviour around it is settled; pairing
does not get to repurpose either. This reverses an earlier draft of this spec
that proposed showing the pairing code on the splash — **do not implement
that.** Pairing on the appliance happens on its web page, like every other
appliance setting.

**Relay settings page.** Same dropdown and states as the docks (Phase 13
already ported the provider dropdown there).

**Colour is never the only signal** — every state above carries words, so it
survives a colourblind operator and a photograph of the screen.

## Success criteria

1. A fresh install that is never paired makes **no collector contact** — no
   heartbeat, no registration, no credential fetch. (The pre-existing
   GitHub update check is a separate, opt-out feature and is not what this
   criterion is about; it must not quietly become the place a collector URL
   is contacted from either.)
2. Pairing a device yields credentials that `cloud-storage` can use to read or
   write the returned bucket, with `session_token` as `X-Amz-Security-Token`.
3. With the collector unreachable, a live event **continues** on last-good
   credentials and the dock says so.
4. A 403 stops the device rather than retrying, and says it was unpaired.
5. There is exactly one place the appliance id/token lives, and the heartbeat
   and storage both read it.
6. Disconnect is a real button that offers the underlying bucket details.

## Open questions — ANSWERED 2026-09-22

1. **Lapsed subscription — decided.** The last-good credentials **run to their
   expiry and any live event continues to the end** on them. After expiry, a
   **new** event refuses to go live, with the reason stated, rather than
   starting on expired keys. Nothing ever cuts an event that is already on air.
   This fixes the shape of the 403 path: 403 stops the *device* from fetching
   again (no retry) but does **not** tear down a running event — the last-good
   set covers it until expiry, exactly as an unreachable collector does.
2. **Decoder pairing: independent, per-device — decided.** A campus pairs its
   own device with its own code, using the same `Pairing` flow the encoder
   uses. Identity is a property of the device, not of a room. Consequence:
   `cloud-identity` needs no per-room indirection, and the room a device writes
   under stays `room-identity`'s separate concern (the capability map's fourth
   module). Inheriting a pairing from the encoder across sites is explicitly
   **not** built and is not a goal of this module.
