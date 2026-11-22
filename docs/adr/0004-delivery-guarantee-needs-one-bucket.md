# ADR-0004: The delivery guarantee needs one bucket; a second is optional

**Status:** Accepted
**Date:** 2026-11-22

## Context

We want to tell prospective users that what is encoded reaches the cloud and
every campus (`docs/scope/upload-delivery-assurance.md`). The obvious way to
repair a segment the store confirmed and then lost or damaged is to re-send it.
That is the claimed element of Resi's upload patents (ADR-0003 §1). The next
most obvious way is a second bucket, which the mirror (PROJECT-SCOPE §10
Phase 9) already provides. But a second bucket means a second storage account,
a second set of credentials and a second bill for every church. That is a real
barrier to adoption for a small church, and it is set up by people who are not
storage engineers.

## Decision

The delivery guarantee is met with **one bucket**:
- The store verifies each segment's checksum inside the PUT.
- Recovery data (parity objects every K segments) is stored in the same
  bucket, so a campus can rebuild a damaged or missing segment.

The second bucket stays supported and **optional**. No feature, promise or
receipt may require it.

## Consequences

- A church gets the full guarantee with one storage account.
- It costs about 1/K extra upload and storage (10 % at K = 10). Parity also
  helps only behind the live edge, once a whole group has uploaded.
- A single bucket's own limits remain: a provider outage longer than a
  campus's buffer, or data loss at scale on the provider's side. Only the
  optional mirror covers those, and the sales wording has to say so rather
  than imply otherwise.
- Building parity is a protocol addition: a new optional manifest field, no
  version bump. The campus also gains a rebuild step.

## Alternatives considered

**Require the mirror.** Covers everything parity covers, plus provider outages.
Rejected as the default because of the account, credentials and cost it puts on
every church, for a failure (a store losing a confirmed object) that should be
rare.

**Re-send from the encoder on a campus report.** Needs only one bucket and no
extra bandwidth, but it does the same job in a similar way to the claimed
test-and-resend. Doctrine-of-equivalents risk (ADR-0003 §1). Not built without
counsel's opinion.

**Do nothing beyond in-request checksums.** Covers damage in transit, which is
the common case. Rejected because it leaves a confirmed-then-lost segment
unrecoverable, and that is the case a "we guarantee delivery" claim has to
answer.
