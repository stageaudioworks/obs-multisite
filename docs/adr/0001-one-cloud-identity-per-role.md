# ADR-0001: One cloud identity per role, not per machine

**Status:** Accepted
**Date:** 2026-09-22

## Context

A machine running the OBS plugin can be an encoder, a decoder, or both. Under
Multisite Cloud each role pairs with the collector through its own device-code
flow and becomes its own appliance: a Mac running both was found holding
`apl_5d62…` for the encoder and `apl_2a7b…` for the decoder, each heartbeating
under its own id and kind.

The plugin nevertheless kept **one** `CloudIdentity`, fed from the encoder's
pairing and falling back to the decoder's, on the stated assumption that "the
machine is one appliance to the collector whatever it is doing locally". So the
decoder heartbeated as `apl_2a7b` and read storage with credentials fetched
under `apl_5d62` — the encoder's, with a read-write role. That is the state
Phase 12 exists to make unrepresentable (`docs/scope/phase12-capability-map.md`,
"The failure this exists to prevent"), and it left a decoder holding write
access it never needs.

This was the map's open question 2 — does a decoder pair independently or
inherit from the encoder? — answered by accident in the code, in a different
direction from what pairing actually did.

## Decision

Each role has its own `CloudIdentity`. The encoder's pairing drives encoder
storage and the encoder heartbeat; the decoder's drives decoder storage and the
decoder heartbeat. Nothing crosses between them, and there is no fallback: a
role that is not paired is not paired, and says so.

`reporter_cloud_identity()` takes the role and has no role-less form, so every
caller has to name which one it means.

## Consequences

- A role's storage and its heartbeat can never name different appliances, which
  is the Phase 12 rule applied where it can actually be kept.
- A decoder reads with its own pairing's credentials. Whether those are
  read-only is the collector's choice per appliance, and now observable in the
  log (`cloud credentials (decoder, apl_…): fetched … (role …)`).
- No collector change: this matches pairing as it already works.
- **A machine doing both must be paired twice.** A decoder set to Multisite
  Cloud that was quietly riding on the encoder's pairing now refuses and asks to
  be paired. That is the rule, not a regression — but it is a real cost to an
  operator, and the one this decision knowingly accepts.
- Two appliances per dual-role machine. If a subscription counts appliances,
  that machine counts twice.

## Alternatives considered

**One identity per machine.** The machine pairs once; both roles heartbeat as
one appliance, told apart by `kind`, sharing one set of credentials. Simpler for
the operator. Rejected for now: pairing fixes one `intended_kind` per code and
the collector gates plan tiers by kind, so it needs cloud-side work first; a
decoder would share the encoder's read-write access; and it turns "how many
seats is a dual-role machine?" into a billing decision that has not been made.
Reopen it if pairing twice proves to be real friction — but the collector
changes first, and then this ADR is superseded rather than edited.

**Leave it undecided and re-home onto the single identity.** What #11 as
written would have done. Rejected because it makes the accident permanent.
