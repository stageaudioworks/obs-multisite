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
#include <QObject>
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
// How far this machine's clock is from the storage service's, and whether that
// is worth saying.
//
// THIS IS NOT ABOUT DISPLAYED TIMES. It used to be — the warning existed
// because a wrong clock made this site's times read oddly against the others —
// and that reason went away when positions became elapsed time. The reason it
// still matters is larger: `SigV4Signer::sign()` builds X-Amz-Date from
// `std::time(nullptr)`, and S3 and R2 refuse any request signed more than about
// FIFTEEN MINUTES from their own clock (RequestTimeTooSkewed, HTTP 403). Past
// that point this machine cannot upload or download at all — not a cosmetic
// fault, a total one.
//
// So the bands are set by that cliff rather than by tidiness:
//
//   under a minute   nothing. Ordinary drift on a machine without NTP, and
//                    harmless: signing has ~15 minutes of room.
//   one to ten min   worth fixing, not yet breaking. Said plainly, once.
//   over ten minutes the red line, with five minutes of headroom left before
//                    the store starts refusing requests.
//
// The old threshold was five SECONDS, which cried wolf at something entirely
// harmless — and a warning an operator has learned to ignore is worse than no
// warning at the moment it finally means something.
//
// One authority for both docks (standards §2). The figure comes from the
// store's own Date header, so it describes this machine's error and not a guess.
enum class ClockSkew { Fine, Worth_Saying, Urgent };

inline ClockSkew clock_skew_level(long long skew_ms) {
    const long long a = skew_ms < 0 ? -skew_ms : skew_ms;
    if (a < 60000)  return ClockSkew::Fine;
    if (a < 600000) return ClockSkew::Worth_Saying;
    return ClockSkew::Urgent;
}

// `fmt` is the localised sentence, taking the skew as %1 (already worded) and
// carrying its own explanation of what breaks.
inline QString clock_skew_text(long long skew_ms) {
    const long long a = skew_ms < 0 ? -skew_ms : skew_ms;
    const long long mins = a / 60000;
    return mins >= 1 ? QObject::tr("%1 min").arg(mins)
                     : QObject::tr("%1 s").arg(a / 1000);
}

inline void set_value(QLabel* label, const QString& text, int max_px = 240) {
    if (!label) return;
    label->setToolTip(text);
    const QFontMetrics fm(label->font());
    label->setText(fm.elidedText(text, Qt::ElideMiddle, max_px));
}

} // namespace multisite_ui
