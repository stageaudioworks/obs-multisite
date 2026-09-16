// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// status_text.h — the two docks word their status rows identically on purpose
// ("an operator who has seen one should not have to learn the other"), so the
// parts that must not drift live here rather than being copied: copying is how
// the encoder dock's copy learned to strip a URL out of a Custom endpoint and
// the decoder dock's did not.
//
// Qt lives under src/obs/ui/ and nowhere else, which is why this is a header
// of its own rather than something in src/core/.
//
#include <QFontMetrics>
#include <QLabel>
#include <QString>
#include <QStringList>

namespace multisite_ui {

// The endpoint as a human placeholder: no scheme, no path, no port, just the
// first label of the host. A Custom endpoint is stored as a full URL, and
// taking its first dot-segment gave "https://minio" — a URL fragment printed
// in a status row labelled "Bucket", which is what "the Bucket row
// occasionally shows the URL" turned out to mean.
inline QString host_label(const QString& host) {
    QString h = host.trimmed();
    const int scheme = h.indexOf("://");
    if (scheme >= 0) h = h.mid(scheme + 3);
    const int slash = h.indexOf('/');
    if (slash >= 0) h = h.left(slash);
    const int colon = h.indexOf(':');
    if (colon >= 0) h = h.left(colon);
    return h.section('.', 0, 0);
}

// Where storage answers from and what the transfer is managing, in one line.
// Only what has actually been measured — an unmeasured rate shown as 0 Mbps
// reads as a dead link rather than as an absence of evidence.
inline QString link_summary(const QString& colo, const QString& host,
                            double bytes_per_s, unsigned long long samples) {
    QStringList bits;
    if (!colo.isEmpty()) bits << colo;
    else if (!host.isEmpty()) bits << host_label(host);
    if (samples > 0 && bytes_per_s > 0.0)
        bits << QString::number(bytes_per_s * 8.0 / 1e6, 'f', 1) + " Mbps";
    return bits.isEmpty() ? QString("—") : bits.join(" · ");
}

// A status value, elided to a bounded width with the whole thing in its
// tooltip. Measured strings — a storage host, a colo code, an appended
// "· Via LAN" — have no length limit, and an unelided QLabel in a grid sets the
// dock's minimum width: one long value and OBS is asked for a dock wider than
// the screen, which is the flash this exists to stop. Elided in the middle,
// because the head and the tail of a hostname are the parts worth seeing.
inline void set_value(QLabel* label, const QString& text, int max_px = 240) {
    if (!label) return;
    label->setToolTip(text);
    const QFontMetrics fm(label->font());
    label->setText(fm.elidedText(text, Qt::ElideMiddle, max_px));
}

} // namespace multisite_ui
