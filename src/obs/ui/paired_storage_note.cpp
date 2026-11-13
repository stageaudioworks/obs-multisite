// SPDX-License-Identifier: GPL-3.0-or-later
#include "paired_storage_note.h"

#include "../reporter.h"

#include <obs-module.h>

namespace multisite_obs {

static QString tr_(const char* key) {
    return QString::fromUtf8(obs_module_text(key));
}

QString paired_storage_note(multisite::CloudRole role) {
    // The plugin's ONE cloud identity for this role — the same one the session
    // signs with — so the panel states what the box does, not what its saved
    // form fields say.
    auto id = reporter_cloud_identity(role);
    if (!id || !id->paired()) return tr_("Dock.PairedNotYet");
    const multisite::Credentials c = id->credentials();
    if (!c.present()) return tr_("Dock.PairedNoCreds");

    const QString bucket = QString::fromStdString(c.bucket);
    // An encoder writes the event into the bucket; a campus only reads it.
    // Saying "reading and writing" to a decoder is a different, and false,
    // statement about what that machine can do.
    const bool writes = (role == multisite::CloudRole::Encoder);
    if (c.from_last_good)
        return tr_(writes ? "Dock.PairedStale" : "Dock.PairedStaleRead")
            .arg(bucket);
    return tr_(writes ? "Dock.PairedBucket" : "Dock.PairedBucketRead")
        .arg(bucket);
}

} // namespace multisite_obs
