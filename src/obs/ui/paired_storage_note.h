// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// paired_storage_note.h — the read-only line a Multisite Cloud machine shows
// in place of the typed storage fields.
//
// There are two docks and one identity. Two copies of this wording is how the
// encoder ends up saying "not paired yet" while the decoder says "reading the
// bucket X" about the same collector connection — standards §2, one authority
// per quantity. Both docks call this.
//
#include "../../core/cloud_storage.h"   // CloudRole; cloud_identity.h comes with it

#include <QString>

namespace multisite_obs {

// One of: not paired / paired but waiting for credentials / the bucket in use
// (read-write for the encoder, read-only for the decoder) / last-known
// credentials with the reason. Always non-empty: the label takes the place of
// the hidden fields, so an empty string would leave the operator with nothing.
QString paired_storage_note(multisite::CloudRole role);

} // namespace multisite_obs
