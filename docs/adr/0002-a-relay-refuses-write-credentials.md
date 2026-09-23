# ADR-0002: A relay refuses credentials that can write

**Status:** Accepted
**Date:** 2026-09-23

## Context

ADR-0001 gave every role its own `CloudIdentity` and left the access level to
the collector: "whether those are read-only is the collector's choice per
appliance, and now observable in the log". For a decoder that is reasonable. A
decoder sits on a church LAN behind the building's own network, and an operator
who can reach it can already reach the encoder next to it.

The simulcast relay is not that. It is the one component in this project that
is *deliberately* exposed to the internet: it runs on a VPS, it serves a web
interface an operator reaches from anywhere, and its whole job is to hold a
stream key and push to YouTube. It is the least trusted box in the system by
design.

A relay only ever reads. It polls `live.json` and a manifest, fetches
fragments, and lists an event to download or replay it. There is no path in it
that writes to the bucket, and there never should be — writing is the encoder's
job, from a machine inside the building.

So a relay holding read-write credentials is access with no use and the widest
possible blast radius: a compromise of the most exposed box would reach every
recording the organisation has. The Phase 12 rule — that a role's storage and
its heartbeat can never name different appliances — makes the identity
coherent. It does not make the access minimal.

## Decision

`CloudIdentity` gains an opt-in `set_require_read_only()`. The relay sets it.

A credential set that can write is then **refused**: nothing is adopted, the
error says why, and any last-good set is left exactly as it was. The refusal is
not terminal — the refresh schedule carries on, so a collector corrected to
issue read-only recovers without anyone restarting the relay.

Off by default. Nothing changes for the encoder, the decoder, or the campus
player.

## Consequences

- The most exposed component in the system holds the least access it can do its
  job with, rather than whatever it was handed.
- **A collector that issues read-write to kind `relay` stops that relay
  working.** It will pair, heartbeat, and refuse to stream, saying why. That is
  a GPL-side feature gated on a cloud-side grant, and it is the cost this
  decision knowingly accepts — chosen over the alternative, which is an
  internet-facing box quietly holding write access to a church's entire
  archive.
- A refusal is not an unpairing. The device stays enrolled and keeps asking, so
  the failure is recoverable from the collector alone.
- Refusing without clearing matters more than it looks: clearing would mean a
  collector that started issuing read-write mid-event would END the event
  rather than decline the new set. The last-good set survives a refusal.

## Alternatives considered

**Log it and carry on, as the decoder does.** Consistent across hosts, needs no
collector change, and was the shape ADR-0001 implied. Rejected because the
consistency is with a component whose exposure is completely different; the
relay's threat model is the reason it is a separate decision rather than an
inherited one.

**Warn in the interface but use them.** Visible without blocking anyone.
Rejected because a warning nobody can act on without a cloud-side change is a
warning that gets dismissed, and the access is still held while it is ignored.

**Refuse in the relay rather than in the core.** Keeps the core unaware of any
host's threat model. Rejected because the check then lives where the core tests
cannot reach it, and this is exactly the kind of rule that should be pinned by
a test rather than by a host remembering to look.
