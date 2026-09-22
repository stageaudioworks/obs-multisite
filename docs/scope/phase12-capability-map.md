# Capability Map: Phase 12 — Multisite Cloud

**Status: proposed, awaiting review. No code until this map is agreed.**

Phase 12 as originally written is *credential pairing* (`PROJECT-SCOPE §8.5`).
The operator's brief widens it into one subsystem: **storage access and
monitoring are the same thing and must be built as one module.** That is a
larger claim than the existing design makes, and it bundles capabilities that
can ship and be verified separately — so the map comes before any spec.

The map is gated: module boundaries, dependency direction and build order are
reviewed here before a module spec is written. Getting this wrong is expensive;
reviewing it is not.

## The failure this exists to prevent

> "A device that heartbeats to us but reads storage with a locally configured
> long-lived key is the exact failure we have now, and splitting the two across
> the codebase is how it stays broken."

Today the heartbeat (`src/core/heartbeat_reporter.*` + hosts) and storage
credentials (`S3Config`, typed by hand) share nothing. A paired device reports
its health under its appliance identity while reading a bucket with a key an
operator pasted — two identities, one machine. Every capability below exists to
make that state unrepresentable, not merely discouraged.

## Capability map

| Module id | Responsibility | Depends on |
|---|---|---|
| `cloud-identity` | The paired appliance: collector URL, appliance id/token, the credential lifecycle (`GET /v1/credentials`, refresh at ~half-TTL, 403 ⇒ unpaired). One owner of "who this device is to the collector". | — |
| `cloud-storage` | The `Transport` the session/player/relay actually use when paired: bucket + endpoint + `X-Amz-Security-Token` from `cloud-identity`, honouring the role (encoder writes, decoder reads). Refuses to fall back to a typed key on fetch failure; degrades to last-good. | `cloud-identity` |
| `cloud-heartbeat` | The existing heartbeat, re-homed: the collector connection, bearer and cadence come from `cloud-identity` rather than from per-role settings that duplicate them. | `cloud-identity` |
| `room-identity` | One authority for the string a device writes under and reports. `rooms/{room_id}/…` and `status.room_id` must be the same value, joined by the scanner and the fleet. | — |

**Build order:** `room-identity` (small, independent, fixes a live inconsistency)
→ `cloud-identity` → `cloud-storage`, `cloud-heartbeat` (parallel; both consume
identity and do not depend on each other).

`room-identity` is deliberately first and deliberately separate: it has no
dependency on the collector at all, it is the one capability that pays out
immediately, and it is small enough to land and verify on its own.

## Progress (2026-09-22)

| Module | State |
|---|---|
| `cloud-identity` | **Built** — `src/core/cloud_identity.{h,cpp}`, `tests/test_cloud_identity.cpp` |
| `collector_client` (support, not in the map) | **Built** — one authority for the endpoint paths and the HTTP call, so the hosts cannot drift apart on them |
| `cloud-storage` | **Built** — `src/core/cloud_storage.{h,cpp}`, `tests/test_cloud_storage.cpp` |
| `cloud-heartbeat` | Core done via the two above; the **host re-home** is [#11](https://github.com/stageaudioworks/obs-multisite/issues/11) |
| `room-identity` | Not started. The `room_id` mismatch is still unconfirmed in the code — settle it from a running box first (see the note below) |
| Appliance wiring | [#12](https://github.com/stageaudioworks/obs-multisite/issues/12) — the first host where the whole path runs on a box |

The frontier is **#12** (appliance) or **#11** (OBS plugin): both are wiring and
neither gates the other. The appliance is the smaller surface and already owns
both its transport and its reporter, so it goes first.

## Interfaces at the boundary

The map records *that* `cloud-storage` and `cloud-heartbeat` depend on
`cloud-identity`. The contract between them — what `cloud-identity` exposes,
and what "paired" means as a type rather than three strings — belongs in
`cloud-identity`'s own spec, not here.

## Open questions this map does not answer

Both are design questions, not details, and the archive already flags them
(`docs/scope/project-scope-phases.md` §8.5 "Not settled"):

1. **What does a broker owe a device when a subscription lapses?** The answer
   must not be "the event stops". This shapes `cloud-identity`'s failure modes.
2. **Does a decoder pair independently, or inherit from the encoder that
   already knows the room?** This decides whether `cloud-identity` is
   per-device or per-room, which changes the module's shape.

Neither should be answered by the first implementation that happens to work.

## A finding to resolve before `room-identity` is specified

The brief says the stack writes `rooms/main/…` while reporting
`main-auditorium`. **Static reading does not confirm this and does not explain
it.** Every default in the tree is `main-auditorium`:
`SessionConfig`, `DecoderConfig`, `LanServerConfig`, `BroadcastSettings`,
`DecoderSettings`, the appliance `Config`, and both web-command fallbacks
(`commands.cpp:133,198`). The path is built from the *same* `room_id` that is
reported (`model.cpp` `live_pointer_key`, `Session::publish_live`). So either:

- the box carries a **saved `room_id` of `main`** from an older config, and the
  mismatch is data, not code; or
- a surface not yet found derives the path from something other than `room_id`.

**This must be settled from the running box before a line of `room-identity`
is written** — capture the encoder's saved `room_id`, the `live.json` path it
wrote, and `status.room_id` from the same machine. Specifying a fix for a bug
that is actually a stale config would add a second answer, which is the failure
mode this whole phase is against.

## What this map is not

- Not a spec. No module spec is written until this is approved.
- Not the original Phase 12. That was pairing alone; this is pairing **as the
  one identity storage and monitoring both use**.
- Not a plan or a task list. Those follow per module, in dependency order.
