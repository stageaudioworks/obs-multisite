// SPDX-License-Identifier: GPL-3.0-or-later
#include "decoder_dock.h"

#include "../multisite_ui.h"
#include "../decoder_settings.h"
#include "../plugin_log.h"
#include "../reporter.h"
#include "../update_check.h"
#include "role_selector.h"
#include "status_text.h"
#include "web_box.h"
#include "secondary_box.h"

#include "../../core/position_interp.h"
#include "../../core/storage_providers.h"
#include "../../core/s3_transport.h"

#include <obs-module.h>

#include <QComboBox>
#include <QCheckBox>
#include <thread>
#include <QMetaObject>
#include <QCoreApplication>
#include <QPointer>
#include <QFormLayout>
#include <QStandardItemModel>
#include <QLineEdit>
#include <QSpinBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QLabel>
#include <QKeyEvent>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QDateTime>
#include <QStringList>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFrame>
#include <QScreen>
#include <QScrollArea>
#include <QTabWidget>
#include "settings_tabs.h"
#include <QGuiApplication>
#include <QListWidget>
#include <QListWidgetItem>
#include <QColor>
#include <QFont>

namespace multisite_obs {

static QString tr_(const char* key) {
    return QString::fromUtf8(obs_module_text(key));
}

// Where an operator is sent when a newer build exists: the releases page lists
// what changed and carries the download, which is the whole answer.
static const char* kReleasesUrl =
    "https://github.com/stageaudioworks/obs-multisite/releases";

// Where downloaded video goes when the field is left blank. Shown as the
// field's placeholder, so "empty" reads as a location rather than as unset.
static QString default_cache_dir() {
    char* p = obs_module_config_path("cache");
    const QString s = p ? QString::fromUtf8(p) : QStringLiteral("./multisite_cache");
    bfree(p);
    return s;
}

// Plain-language duration. Volunteers read "1 min 30 sec", not "90 s" and
// certainly not a count of segments.
static QString friendly_duration(double seconds) {
    if (seconds < 1.0) return QObject::tr("none");
    const int total = (int)(seconds + 0.5);
    const int mins = total / 60;
    const int secs = total % 60;
    if (mins == 0) return QObject::tr("%1 sec").arg(secs);
    if (secs == 0) return QObject::tr("%1 min").arg(mins);
    return QObject::tr("%1 min %2 sec").arg(mins).arg(secs);
}

// A position within a recording, as an operator would read a media player:
// "24:15" or "1:24:15" — how far into the programme, which is the only way
// this dock states a position. There is deliberately no time-of-day helper:
// see axis_time().
static QString position(long long ms) {
    if (ms < 0) ms = 0;
    const long long total = ms / 1000;
    const long long h = total / 3600, m = (total % 3600) / 60, s = total % 60;
    if (h > 0)
        return QString("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0'))
                                  .arg(s, 2, 10, QChar('0'));
    return QString("%1:%2").arg(m).arg(s, 2, 10, QChar('0'));
}

// A short form of whatever the caller has to name. The recording rows carry the
// name AND the date AND the length AND a state word, and putting all of that in
// a label wide enough to hold it stretched the whole dock; the header and the
// position line each want a glance, not a line.
static QString short_name(const QString& text, int max_chars = 34) {
    QString t = text.simplified();
    if (t.size() <= max_chars) return t;
    return t.left(max_chars - 1) + QChar(0x2026);   // …
}

// What the store said, in words an operator can act on. "HTTP 404 NoSuchKey"
// and "HTTP 403 AccessDenied" read as the same sentence to a volunteer and are
// fixed in completely different places.
static QString friendly_error(const std::string& raw) {
    if (raw.empty()) return QString();
    const QString text = QString::fromStdString(raw);
    const QString lower = text.toLower();
    if (lower.contains("403") || lower.contains("accessdenied") ||
        lower.contains("signaturedoesnotmatch") ||
        lower.contains("invalidaccesskeyid"))
        return tr_("Dock.ErrRefused");
    if (lower.contains("404") || lower.contains("nosuchkey") ||
        lower.contains("nosuchbucket"))
        return tr_("Dock.ErrNothingThere");
    if (lower.contains("timeout") || lower.contains("timed out") ||
        lower.contains("resolve") || lower.contains("connection") ||
        lower.contains("unreachable"))
        return tr_("Dock.ErrNoAnswer");
    return text;   // unknown: the provider's own words beat a guess
}

// ── TimelineBar ──────────────────────────────────────────────────────────────
TimelineBar::TimelineBar(QWidget* parent) : QWidget(parent) {
    setCursor(Qt::PointingHandCursor);
    setMouseTracking(true);          // needed for the hover readout
    // Arrow keys nudge, so the bar has to be able to hold focus. ClickFocus,
    // not StrongFocus: taking Tab out of OBS's own order would be rude for a
    // panel that is not part of a form.
    setFocusPolicy(Qt::ClickFocus);
    setToolTip(tr_("Dock.TimelineHint"));
}

QSize TimelineBar::minimumSizeHint() const { return QSize(160, 46); }

void TimelineBar::setPlaceholder(const QString& why) {
    if (m_placeholder == why) return;
    m_placeholder = why;
    update();
}

void TimelineBar::setSpan(long long earliest_ms, long long live_ms) {
    if (m_earliest == earliest_ms && m_live == live_ms) return;
    m_earliest = earliest_ms; m_live = live_ms;
    update();
}
void TimelineBar::setPlayhead(long long ms) {
    if (m_head == ms) return;
    m_head = ms; update();
}
void TimelineBar::setPending(long long media_ms) {
    if (m_pending == media_ms) return;
    m_pending = media_ms; update();
}
void TimelineBar::setDownloaded(std::vector<std::pair<long long, long long>> spans) {
    if (m_downloaded == spans) return;
    m_downloaded = std::move(spans); update();
}
void TimelineBar::setMarkers(std::vector<long long> times_ms) {
    if (m_markers == times_ms) return;
    m_markers = std::move(times_ms); update();
}

double TimelineBar::fraction(long long ms) const {
    if (m_live <= m_earliest) return 0.0;
    if (ms <= m_earliest) return 0.0;
    if (ms >= m_live) return 1.0;
    return (double)(ms - m_earliest) / (double)(m_live - m_earliest);
}

long long TimelineBar::timeAt(int x) const {
    if (m_live <= m_earliest || width() <= 0) return 0;
    double f = (double)x / (double)width();
    f = std::min(1.0, std::max(0.0, f));
    return m_earliest + (long long)(f * (double)(m_live - m_earliest));
}

// A position on the media axis, as text. With an origin (a live event) it reads
// as a time of day; without one (a recording) it reads as elapsed. The axis is
// media time either way, so a tick is always where the picture is.
// Elapsed into the programme, always. The axis used to switch to a time of day
// whenever a live event was playing, so the same bar read "4:47" for a recording
// and "13:49" for a live feed — two different quantities in one place, and the
// second needed a wall<->media mapping that drifted 1.11% (BUGS #2b). A
// position on a timeline means how far in you are, whatever is producing it.
static QString axis_time(long long media_ms, bool seconds) {
    (void)seconds;
    if (media_ms < 0) media_ms = 0;
    const long long t = media_ms / 1000;
    const long long h = t / 3600, m = (t % 3600) / 60, s = t % 60;
    if (h > 0)
        return QString("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0'))
              .arg(s, 2, 10, QChar('0'));
    return QString("%1:%2").arg(m).arg(s, 2, 10, QChar('0'));
}

void TimelineBar::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const int h = 10;
    const int y = 16;                 // room for the clock scale above
    const int w = width();

    // The recording that still exists in storage.
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x36, 0x3b, 0x41));
    p.drawRoundedRect(QRect(0, y, w, h), 4, 4);

    if (m_live <= m_earliest) {
        // No span to draw. Say why rather than leaving a bar that looks
        // broken: this is the ordinary state before an event is loaded, and
        // it should not be indistinguishable from one that has failed.
        if (!m_placeholder.isEmpty()) {
            p.setPen(QPen(QColor(0x8b, 0x91, 0x98)));
            p.drawText(QRect(0, y - 4, w, h + 8),
                       Qt::AlignCenter, m_placeholder);
        }
        return;
    }

    // Three states, three distinct colours — and no translucent overlays.
    // Layering a see-through "played" band over the downloaded band produced
    // a second greenish shade that meant nothing, which is exactly the kind of
    // thing an operator should never have to decode mid-event.
    //
    //   grey  = exists in storage, not downloaded here
    //   blue  = downloaded and already played
    //   green = downloaded and ready to play  ← the safety buffer
    const int headX = (int)(fraction(m_head) * w);
    for (const auto& sp : m_downloaded) {
        const int x1 = (int)(fraction(sp.first) * w);
        const int x2 = (int)(fraction(sp.second) * w);
        if (x2 < x1) continue;
        // Split each downloaded span at the playhead.
        const int mid = std::min(std::max(headX, x1), x2);
        if (mid > x1) {
            p.setBrush(QColor(0x35, 0x5a, 0x7a));          // played
            p.drawRect(QRect(x1, y, mid - x1, h));
        }
        if (x2 > mid) {
            p.setBrush(QColor(0x35, 0xc4, 0x89));          // ready to play
            p.drawRect(QRect(mid, y, std::max(1, x2 - mid), h));
        }
    }

    // Clock scale: a few labelled ticks so positions mean something.
    p.setPen(QPen(QColor(0x7f, 0x86, 0x8e)));
    QFont f = p.font(); f.setPointSizeF(f.pointSizeF() - 1.5); p.setFont(f);
    const int ticks = std::max(2, std::min(5, w / 90));
    for (int i = 0; i <= ticks; ++i) {
        const double frac = (double)i / ticks;
        const long long t = m_earliest +
            (long long)(frac * (double)(m_live - m_earliest));
        const int x = (int)(frac * w);
        p.drawLine(x, y - 4, x, y - 1);
        const QString label = axis_time(t, false);
        // Keep the end labels inside the widget. Centred on x, the first
        // rect started at -22 and the last ended at w+22, so both were
        // clipped by the widget edge and the scale read "5 … 1" instead of
        // "13:45 … 14:10" — a timeline whose two outermost times were single
        // digits, which is its own kind of confusing.
        QRect r = (i == 0)         ? QRect(0, 0, 44, 12)
                : (i == ticks)     ? QRect(w - 44, 0, 44, 12)
                                   : QRect(x - 22, 0, 44, 12);
        p.drawText(r, (i == 0 ? Qt::AlignLeft : (i == ticks ? Qt::AlignRight
                                                            : Qt::AlignHCenter))
                      | Qt::AlignVCenter, label);
    }

    // Marker ticks.
    p.setPen(QPen(QColor(0xe0, 0xa0, 0x20), 2));
    for (long long t : m_markers) {
        const int x = (int)(fraction(t) * w);
        p.drawLine(x, y - 2, x, y + h + 2);
    }

    // Live edge.
    p.setPen(QPen(QColor(0xe5, 0x48, 0x4d), 2));
    p.drawLine(w - 1, y - 3, w - 1, y + h + 3);

    // Playhead.
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0xdf, 0xe3, 0xe7));
    p.drawEllipse(QPoint(headX, y + h / 2), 6, 6);
    p.setPen(QPen(QColor(0x3b, 0x82, 0xc4), 2));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QPoint(headX, y + h / 2), 6, 6);

    // Where a jump is heading. While the mouse is down that is the finger's
    // position, because the release will land there; otherwise it is the target
    // the dock reports, which stays up until the picture actually arrives.
    // Said in the position line as well ("Going to 10:42:06") AND drawn here: a
    // click on the bar that leaves no mark on it is the one place an operator
    // looks, and its absence reads as the click having been dropped. Same blue
    // as that line, solid and thicker while dragging so the two cannot be
    // confused.
    int markX = -1;
    bool dragging = false;
    if (m_dragging && m_scrubX >= 0) {
        markX = m_scrubX;
        dragging = true;
    } else if (m_pending >= 0 && m_pending >= m_earliest && m_pending <= m_live) {
        markX = (int)(fraction(m_pending) * w);
    }
    if (markX >= 0) {
        p.setPen(QPen(QColor(0x3b, 0x82, 0xc4), dragging ? 3 : 2,
                      dragging ? Qt::SolidLine : Qt::DashLine));
        p.drawLine(markX, y - 6, markX, y + h + 6);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x3b, 0x82, 0xc4));
        const QPoint tri[3] = { QPoint(markX - 4, y - 13), QPoint(markX + 4, y - 13),
                                QPoint(markX, y - 5) };
        p.drawPolygon(tri, 3);
    }

    // Hover: a following marker and the recorded time under the cursor, so the
    // operator knows what they are jogging to BEFORE they click.
    if (m_hoverX >= 0) {
        const long long t = timeAt(m_hoverX);
        p.setPen(QPen(QColor(0xdf, 0xe3, 0xe7, 160), 1, Qt::DashLine));
        p.drawLine(m_hoverX, y - 6, m_hoverX, y + h + 6);
        const QString label = axis_time(t, true);
        p.setPen(QPen(QColor(0xff, 0xff, 0xff)));
        QRect box(m_hoverX - 30, y + h + 4, 60, 14);
        if (box.left() < 0) box.moveLeft(0);
        if (box.right() > w) box.moveRight(w);
        p.setBrush(QColor(0x1a, 0x1d, 0x20, 210));
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(box, 3, 3);
        p.setPen(QPen(QColor(0xff, 0xff, 0xff)));
        p.drawText(box, Qt::AlignCenter, label);
    }
}

void TimelineBar::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    // Start a scrub. The seek itself happens on release: one seek per gesture,
    // because every seek tears the decoder down and re-anchors the clock, and a
    // drag can produce dozens of mouse-moves a second.
    m_dragging = true;
    m_scrubX   = e->pos().x();
    update();
}

void TimelineBar::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    if (!m_dragging) return;
    m_dragging = false;
    const int x = e->pos().x();
    m_scrubX = -1;
    update();
    // A plain click never moved, and lands here too — which is what makes a
    // click and a drag the same gesture with the same result.
    const long long t = timeAt(x);
    if (t > 0) emit seekRequested(t);
}

void TimelineBar::mouseMoveEvent(QMouseEvent* e) {
    m_hoverX = e->pos().x();
    if (m_dragging) m_scrubX = m_hoverX;
    update();
}

void TimelineBar::keyPressEvent(QKeyEvent* e) {
    // Nudging is for lining a moment up precisely when the mouse is too coarse
    // — the dock's jog buttons are ±10 s and ±1 min, and this is the same
    // arithmetic from wherever the playhead sits.
    long long delta = 0;
    switch (e->key()) {
    case Qt::Key_Left:  delta = e->modifiers() & Qt::ShiftModifier ? -60000 : -10000; break;
    case Qt::Key_Right: delta = e->modifiers() & Qt::ShiftModifier ?  60000 :  10000; break;
    default: QWidget::keyPressEvent(e); return;
    }
    if (m_live <= m_earliest) return;
    long long target = (m_head > 0 ? m_head : m_earliest) + delta;
    if (target < m_earliest) target = m_earliest;
    if (target > m_live)     target = m_live;
    if (target > 0) emit seekRequested(target);
}

void TimelineBar::leaveEvent(QEvent*) {
    // While the button is down Qt keeps sending move and release events here,
    // so the drag survives the pointer leaving the widget; only the hover
    // readout goes.
    m_hoverX = -1;
    update();
}

// ── DecoderDock ──────────────────────────────────────────────────────────────
// The status rows are worded by status_text.h, shared with the encoder dock and
// for the same reason: an operator who has seen one should not have to learn the
// other, and the two copies had already drifted once.

DecoderDock::DecoderDock(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(8);

    // header: room, then what THIS box is doing, then what the MAIN SITE is
    // doing. Two labels, because they are two different facts and one label
    // could only ever show whichever happened to win a precedence chain — so
    // playing a finished recording read "BROADCAST ENDED" with nothing to say
    // it was playing, and Hold on that same recording showed nothing at all.
    // Playback first: it is the one the operator is acting on.
    auto* head = new QHBoxLayout();
    m_room = new QLabel(tr_("Dock.NoSource"), this);
    m_room->setStyleSheet("font-weight: bold;");
    m_playback = new QLabel(QString(), this);
    m_state = new QLabel(QString(), this);
    head->addWidget(m_room);
    head->addStretch(1);
    head->addWidget(m_playback);
    head->addSpacing(10);
    head->addWidget(m_state);
    root->addLayout(head);

    // behind-live readout: the number an operator watches
    m_behind = new QLabel("—", this);
    m_behind->setStyleSheet("font-size: 20px; font-weight: 500;");
    root->addWidget(m_behind);

    // timeline
    m_timeline = new TimelineBar(this);
    root->addWidget(m_timeline);
    connect(m_timeline, &TimelineBar::seekRequested,
            this, &DecoderDock::onSeek);

    // Legend: three colours, stated once, so nobody has to guess what the
    // bar is telling them.
    {
        auto* leg = new QHBoxLayout();
        leg->setSpacing(10);
        struct { const char* key; const char* colour; } items[] = {
            { "Dock.LegendReady",  "#35c489" },
            { "Dock.LegendPlayed", "#355a7a" },
            { "Dock.LegendStored", "#363b41" },
        };
        for (auto& it : items) {
            auto* sw = new QLabel(this);
            sw->setFixedSize(10, 10);
            sw->setStyleSheet(QString("background:%1; border-radius:2px;")
                                .arg(it.colour));
            auto* tx = new QLabel(tr_(it.key), this);
            tx->setStyleSheet("color: palette(text); opacity: 0.75;");
            QFont f = tx->font(); f.setPointSizeF(f.pointSizeF() - 1.0);
            tx->setFont(f);
            leg->addWidget(sw);
            leg->addWidget(tx);
        }
        leg->addStretch(1);
        root->addLayout(leg);
    }

    // Load, then play. Loading fills the buffer; Play puts it to air. Keeping
    // these separate is how an operator prepares before an event rather than
    // having playback start the moment enough has arrived.
    auto* startRow = new QHBoxLayout();
    m_start = new QPushButton(tr_("Dock.FollowLive"), this);
    m_start->setToolTip(tr_("Dock.FollowLiveHint"));
    m_play  = new QPushButton(tr_("Dock.Play"), this);
    m_stop  = new QPushButton(tr_("Dock.Stop"), this);
    startRow->addWidget(m_start);
    startRow->addWidget(m_play);
    startRow->addWidget(m_stop);
    root->addLayout(startRow);
    connect(m_start, &QPushButton::clicked, this, &DecoderDock::onStart);
    connect(m_play,  &QPushButton::clicked, this, &DecoderDock::onPlay);
    connect(m_stop,  &QPushButton::clicked, this, &DecoderDock::onStop);

    auto* row = new QHBoxLayout();
    m_pause  = new QPushButton(tr_("Pause"), this);
    m_resume = new QPushButton(tr_("Resume"), this);
    m_live   = new QPushButton(tr_("JumpToLive"), this);
    row->addWidget(m_pause);
    row->addWidget(m_resume);
    row->addWidget(m_live);
    root->addLayout(row);
    connect(m_pause,  &QPushButton::clicked, this, &DecoderDock::onPause);
    connect(m_resume, &QPushButton::clicked, this, &DecoderDock::onResume);
    connect(m_live,   &QPushButton::clicked, this, &DecoderDock::onJumpLive);

    // Jog: coarse and honest. Seeking lands within about a second, so
    // frame-level steps would be misleading.
    auto* jogRow = new QHBoxLayout();
    struct { const char* text; double secs; } jogs[] = {
        { "-1 min", -60.0 }, { "-10 s", -10.0 }, { "-1 s", -1.0 },
        { "+1 s", 1.0 }, { "+10 s", 10.0 }, { "+1 min", 60.0 },
    };
    for (auto& j : jogs) {
        auto* b = new QPushButton(QString::fromUtf8(j.text), this);
        b->setMaximumWidth(64);
        const double secs = j.secs;
        connect(b, &QPushButton::clicked, this, [this, secs] { onJog(secs); });
        jogRow->addWidget(b);
    }
    root->addLayout(jogRow);

    // Sit at a fixed delay behind the main site — the usual way a campus runs
    // when it wants a safety margin.
    auto* delayRow = new QHBoxLayout();
    delayRow->addWidget(new QLabel(tr_("Dock.DelayFromLive"), this));
    m_delayMins = new QSpinBox(this);
    m_delayMins->setToolTip(tr_("Dock.DelayHint"));
    m_delayMins->setRange(0, 120);
    m_delayMins->setSuffix(tr_("Dock.Minutes"));
    m_delayMins->setValue(0);
    m_goTo = new QPushButton(tr_("Dock.GoTo"), this);
    delayRow->addWidget(m_delayMins);
    delayRow->addWidget(m_goTo);
    delayRow->addStretch(1);
    m_lock = new QPushButton(tr_("Dock.Lock"), this);
    m_lock->setCheckable(true);
    m_lock->setToolTip(tr_("Dock.LockHint"));
    delayRow->addWidget(m_lock);
    root->addLayout(delayRow);
    connect(m_goTo, &QPushButton::clicked, this, &DecoderDock::onGoToDelay);
    connect(m_lock, &QPushButton::toggled, this, &DecoderDock::onLockToggled);

    // markers
    auto* mrow = new QHBoxLayout();
    m_markers = new QComboBox(this);
    m_markers->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_markers->setToolTip(tr_("Dock.MarkerHint"));
    m_jumpMarker = new QPushButton(tr_("Dock.Jump"), this);
    m_jumpMarker->setToolTip(tr_("Dock.JumpHint"));
    mrow->addWidget(m_markers, 1);
    mrow->addWidget(m_jumpMarker);
    root->addLayout(mrow);
    connect(m_jumpMarker, &QPushButton::clicked,
            this, &DecoderDock::onJumpMarker);

    // ── Recordings ──────────────────────────────────────────────────────────
    // Everything the room still holds, newest first, with anything live at the
    // top. Choosing one plays it instead of the live feed; the decoder stays on
    // it even if an event starts, and says so rather than switching.
    {
        auto* evBox = new QGroupBox(tr_("Dock.Recordings"), this);
        auto* evRoot = new QVBoxLayout(evBox);

        m_liveElsewhere = new QLabel(QString(), evBox);
        m_liveElsewhere->setWordWrap(true);
        m_liveElsewhere->setStyleSheet("color: #e5484d; font-weight: bold;");
        m_liveElsewhere->hide();
        evRoot->addWidget(m_liveElsewhere);

        m_events = new QListWidget(evBox);
        m_events->setAlternatingRowColors(true);
        // Deep enough to show a Sunday's worth without scrolling, shallow
        // enough to leave the transport controls above the fold.
        m_events->setMinimumHeight(110);
        m_events->setMaximumHeight(180);
        evRoot->addWidget(m_events);
        connect(m_events, &QListWidget::itemDoubleClicked,
                this, &DecoderDock::onEventActivated);

        m_eventsNote = new QLabel(QString(), evBox);
        m_eventsNote->setWordWrap(true);
        m_eventsNote->setStyleSheet("color: palette(text); opacity: 0.75;");
        evRoot->addWidget(m_eventsNote);

        auto* evRow = new QHBoxLayout();
        m_loadEvent     = new QPushButton(tr_("Dock.LoadRecording"), evBox);
        m_returnLive    = new QPushButton(tr_("Dock.ReturnToLive"), evBox);
        m_refreshEvents = new QPushButton(tr_("Dock.Refresh"), evBox);
        evRow->addWidget(m_loadEvent, 1);
        evRow->addWidget(m_returnLive);
        evRow->addWidget(m_refreshEvents);
        evRoot->addLayout(evRow);
        connect(m_loadEvent,     &QPushButton::clicked, this, &DecoderDock::onLoadEvent);
        connect(m_returnLive,    &QPushButton::clicked, this, &DecoderDock::onReturnToLive);
        connect(m_refreshEvents, &QPushButton::clicked, this, &DecoderDock::onRefreshEvents);

        root->addWidget(evBox);
    }

    // detail
    auto* box = new QGroupBox(tr_("Dock.Status"), this);
    auto* grid = new QGridLayout(box);
    auto addStat = [&](int r, int c, const char* key, QLabel*& out) {
        auto* cap = new QLabel(tr_(key), box);
        // Dim but still legible: palette(mid) is nearly invisible on
        // OBS's dark theme, which left the numbers looking unlabelled.
        cap->setStyleSheet("color: palette(text); opacity: 0.75;");
        out = new QLabel("—", box);
        grid->addWidget(cap, r, c * 2);
        grid->addWidget(out, r, c * 2 + 1);
    };
    addStat(0, 0, "Dock.Net",      m_net);
    addStat(0, 1, "Dock.Buffered", m_buffered);
    addStat(1, 0, "Dock.CanRewind", m_cached);
    addStat(1, 1, "Dock.Marker",   m_marker);
    addStat(2, 0, "Dock.Audio",    m_audio);
    // Where the bucket answers from and what the download is managing. A
    // campus that cannot hold a buffer has no other way to tell a slow link
    // from a distant bucket.
    addStat(2, 1, "Dock.Storage",  m_storage);
    m_error = new QLabel(QString(), box);
    m_error->setWordWrap(true);
    m_error->setStyleSheet("color: #e5484d;");
    m_error->hide();
    grid->addWidget(m_error, 3, 0, 1, 4);
    root->addWidget(box);

    // ── Storage: entered once for this machine ──────────────────────────────
    // Previously these lived only in each source's settings, so they were lost
    // if OBS exited uncleanly and had to be retyped for every source.
    m_settingsBtn = new QPushButton(tr_("Dock.Settings"), this);
    root->addWidget(m_settingsBtn);
    connect(m_settingsBtn, &QPushButton::clicked,
            this, &DecoderDock::onOpenSettings);

    m_settings = new SettingsDialog(this);
    m_settings->setWindowTitle(tr_("Dock.SettingsTitle"));
    auto* dlgRoot = new QVBoxLayout(m_settings);

    // Two tabs rather than one long strip — the same shape the encoder dock
    // uses, and for the same reason. Stacked, storage plus the role selector
    // plus the remote control box was taller than a laptop screen, and the
    // buttons were what fell off the bottom.
    //
    // What this machine RECEIVES and what this machine IS are separate
    // questions, so they are separate pages, and each fits without scrolling.
    auto* tabs = new QTabWidget(m_settings);

    auto* storePage = new QWidget(tabs);
    auto* storePageLayout = new QVBoxLayout(storePage);
    auto* storeBox = new QGroupBox(tr_("Dock.Storage"), storePage);
    auto* form = new QFormLayout(storeBox);
    m_provider  = new QComboBox(storeBox);
    m_provider->setToolTip(tr_("StorageProviderHint"));
    for (const auto& info : multisite::all_providers()) {
        m_provider->addItem(QString::fromStdString(info.display_name),
                            QString::fromStdString(info.key));
        if (!info.available) {
            // Multisite Cloud: listed so an operator knows it's coming, not
            // yet selectable (PROJECT-SCOPE.md §8.5 hasn't been built).
            if (auto* model = qobject_cast<QStandardItemModel*>(m_provider->model()))
                if (auto* item = model->item(m_provider->count() - 1))
                    item->setEnabled(false);
        }
    }
    m_accountId = new QLineEdit(storeBox);
    m_accountId->setToolTip(tr_("R2AccountIDHint"));
    m_endpoint  = new QLineEdit(storeBox);
    m_endpoint->setToolTip(tr_("EndpointHostHint"));
    m_bucket    = new QLineEdit(storeBox);
    m_bucket->setToolTip(tr_("BucketHint"));
    m_keyId     = new QLineEdit(storeBox);
    m_keyId->setToolTip(tr_("AccessKeyIDHint"));
    m_secret    = new QLineEdit(storeBox);
    m_secret->setEchoMode(QLineEdit::Password);
    m_secret->setToolTip(tr_("SecretKeyHint"));
    m_region    = new QLineEdit(storeBox);
    m_region->setToolTip(tr_("RegionHint"));
    m_roomId    = new QLineEdit(storeBox);
    m_roomId->setToolTip(tr_("RoomIDHint"));
    m_prebuffer = new QSpinBox(storeBox);
    m_prebuffer->setRange(0, 10);
    m_prebuffer->setToolTip(tr_("Dock.PrebufferHint"));
    form->addRow(tr_("Dock.StorageProvider"), m_provider);
    form->addRow(tr_("R2AccountID"), m_accountId);
    form->addRow(tr_("EndpointHost"), m_endpoint);
    form->addRow(tr_("Bucket"), m_bucket);
    form->addRow(tr_("AccessKeyID"), m_keyId);
    form->addRow(tr_("SecretKey"), m_secret);
    form->addRow(tr_("Region"), m_region);
    form->addRow(tr_("RoomID"), m_roomId);
    m_siteName = new QLineEdit(storeBox);
    m_siteName->setToolTip(tr_("Dock.SiteNameHint"));
    form->addRow(tr_("Dock.SiteName"), m_siteName);
    form->addRow(tr_("Prebuffer"), m_prebuffer);
    m_startBufferS = new QSpinBox(storeBox);
    m_startBufferS->setRange(0, 300);
    m_startBufferS->setSuffix(tr_("Dock.Seconds"));
    m_startBufferS->setToolTip(tr_("Dock.StartBufferHint"));
    form->addRow(tr_("StartBufferSeconds"), m_startBufferS);
    m_bufferMins = new QSpinBox(storeBox);
    m_bufferMins->setRange(1, 60);
    m_bufferMins->setSuffix(tr_("Dock.Minutes"));
    m_bufferMins->setToolTip(tr_("BufferMinutesHint"));
    form->addRow(tr_("BufferMinutes"), m_bufferMins);
    m_pollMs = new QSpinBox(storeBox);
    m_pollMs->setRange(500, 10000);
    m_pollMs->setSingleStep(500);
    m_pollMs->setSuffix(" ms");
    m_pollMs->setToolTip(tr_("Dock.PollHint"));
    form->addRow(tr_("PollInterval"), m_pollMs);
    m_keepBehind = new QSpinBox(storeBox);
    m_keepBehind->setRange(10, 2000);
    m_keepBehind->setSingleStep(10);
    m_keepBehind->setToolTip(tr_("Dock.KeepBehindHint"));
    form->addRow(tr_("KeepBehind"), m_keepBehind);
    m_cacheDir = new QLineEdit(storeBox);
    m_cacheDir->setToolTip(tr_("Dock.CacheDirHint"));
    // The effective default, in grey: an operator should be able to see where
    // the video is going without having to set anything.
    m_cacheDir->setPlaceholderText(default_cache_dir());
    {
        auto* wrap = new QWidget(storeBox);
        auto* hl = new QHBoxLayout(wrap);
        hl->setContentsMargins(0, 0, 0, 0);
        hl->addWidget(m_cacheDir, 1);
        m_cacheBrowse = new QPushButton(tr_("Dock.Browse"), wrap);
        hl->addWidget(m_cacheBrowse);
        connect(m_cacheBrowse, &QPushButton::clicked, this, [this] {
            const QString typed = m_cacheDir->text().trimmed();
            const QString dir = QFileDialog::getExistingDirectory(
                this, tr_("Dock.CacheDir"),
                typed.isEmpty() ? default_cache_dir() : typed);
            if (!dir.isEmpty()) {
                m_cacheDir->setText(dir);
                m_dirty = true;
            }
        });
        form->addRow(tr_("Dock.CacheDir"), wrap);
    }
    storePageLayout->addWidget(storeBox);

    // Test the primary bucket with the values AS TYPED. A campus reads
    // live.json, so that is what is asked for — a listing would need
    // ListBucket, which a read-only object-scoped key commonly lacks and does
    // not need.
    m_testConnection = new QPushButton(tr_("Dock.TestConnection"), storePage);
    m_testConnection->setToolTip(tr_("Dock.TestConnectionHintSat"));
    storePageLayout->addWidget(m_testConnection);
    m_testConnectionResult = new QLabel(QString(), storePage);
    m_testConnectionResult->setWordWrap(true);
    storePageLayout->addWidget(m_testConnectionResult);
    connect(m_testConnection, &QPushButton::clicked, this, [this] {
        const std::string bucket = m_bucket->text().trimmed().toStdString();
        if (bucket.empty()) {
            m_testConnectionResult->setText(tr_("Dock.TestConnectionNoBucket"));
            return;
        }
        multisite::S3Config cfg;
        fill_s3_config(cfg, m_provider->currentData().toString().toStdString(),
                       m_accountId->text().trimmed().toStdString(),
                       m_endpoint->text().trimmed().toStdString(),
                       m_region->text().trimmed().toStdString(), bucket,
                       m_keyId->text().trimmed().toStdString(),
                       m_secret->text().toStdString());
        const std::string room = m_roomId->text().trimmed().toStdString();
        m_testConnection->setEnabled(false);
        m_testConnectionResult->setStyleSheet(QString());
        m_testConnectionResult->setText(tr_("Dock.TestConnectionRunning"));
        QPointer<DecoderDock> self(this);
        std::thread([self, cfg, room] {
            const ProbeResult r = probe_bucket(cfg, false, room);
            // qApp, not `self` — see storage_dialog.cpp::runAsync().
            QMetaObject::invokeMethod(qApp, [self, r] {
                if (self) self->showTestConnection(r);
            }, Qt::QueuedConnection);
        }).detach();
    });

    // The second bucket (PROJECT-SCOPE.md §10 Phase 9). Machine-wide rather
    // than part of these settings, because the other dock's half reads the
    // same answer — see storage_secondary.h. A decoder uses it to fall back
    // when the first target is unreachable.
    m_secondary = new SecondaryTargetBox(false, storePage);
    storePageLayout->addWidget(m_secondary);
    connect(m_secondary, &SecondaryTargetBox::changed,
            this, [this] { if (!m_loading) m_dirty = true; });

    // ── LAN / direct delivery (PROJECT-SCOPE.md §8.7) ───────────────────────
    // A host typed in, by hand — no on/off checkbox: an empty host IS "not
    // configured", the same way an empty bucket already means that above.
    // Cloud storage above may be left blank entirely for a LAN-only machine
    // (DecoderSettings::configured() accepts either), or filled in alongside
    // this for automatic LAN-preferred, cloud-fallback delivery.
    auto* lanBox = new QGroupBox(tr_("Dock.LanDelivery"), storePage);
    auto* lform = new QFormLayout(lanBox);
    m_lanHost = new QLineEdit(lanBox);
    m_lanHost->setToolTip(tr_("Dock.LanHostHint"));
    m_lanPortField = new QSpinBox(lanBox);
    m_lanPortField->setToolTip(tr_("Dock.LanPortHint"));
    m_lanPortField->setRange(1, 65535);
    m_lanToken = new QLineEdit(lanBox);
    m_lanToken->setToolTip(tr_("Dock.LanTokenHint"));
    lform->addRow(tr_("Dock.LanHost"), m_lanHost);
    lform->addRow(tr_("Dock.LanPort"), m_lanPortField);
    lform->addRow(tr_("Dock.LanToken"), m_lanToken);
    storePageLayout->addWidget(lanBox);

    // ── Monitoring heartbeat (reporter-brief) ──────────────────────────
    // Off by default, worded exactly as the encoder dock words it: an
    // operator who has seen one settings dialog should not have to learn
    // the other.
    auto* repBox = new QGroupBox(tr_("Dock.Reporter"), storePage);
    auto* rform = new QFormLayout(repBox);
    m_reporterEnabled = new QCheckBox(tr_("Dock.ReporterEnabled"), repBox);
    m_reporterEnabled->setToolTip(tr_("Dock.ReporterEnabledHint"));
    m_reporterUrl = new QLineEdit(repBox);
    m_reporterUrl->setToolTip(tr_("Dock.ReporterUrlHint"));
    m_reporterUrl->setPlaceholderText("https://");
    m_reporterId = new QLineEdit(repBox);
    m_reporterId->setToolTip(tr_("Dock.ReporterIdHint"));
    m_reporterToken = new QLineEdit(repBox);
    m_reporterToken->setToolTip(tr_("Dock.ReporterTokenHint"));
    m_reporterToken->setEchoMode(QLineEdit::Password);
    m_reporterState = new QLabel(repBox);
    m_reporterState->setWordWrap(true);
    rform->addRow(QString(), m_reporterEnabled);
    rform->addRow(tr_("Dock.ReporterUrl"), m_reporterUrl);
    rform->addRow(tr_("Dock.ReporterId"), m_reporterId);
    rform->addRow(tr_("Dock.ReporterToken"), m_reporterToken);
    rform->addRow(QString(), m_reporterState);
    // Device-code pairing: the code while the collector waits for approval,
    // then the saved credentials. Worded as the encoder dialog words it.
    {
        auto* pairRow = new QWidget(repBox);
        auto* pairLayout = new QHBoxLayout(pairRow);
        pairLayout->setContentsMargins(0, 0, 0, 0);
        m_pairBtn = new QPushButton(tr_("Dock.ReporterConnect"), pairRow);
        m_pairBtn->setToolTip(tr_("Dock.ReporterConnectHint"));
        m_pairCancel = new QPushButton(tr_("Dock.ReporterCancel"), pairRow);
        m_pairCancel->setEnabled(false);
        pairLayout->addWidget(m_pairBtn);
        pairLayout->addWidget(m_pairCancel);
        pairLayout->addStretch(1);
        rform->addRow(QString(), pairRow);
        connect(m_pairBtn, &QPushButton::clicked, this, &DecoderDock::onPairBegin);
        connect(m_pairCancel, &QPushButton::clicked, this, &DecoderDock::onPairCancel);
    }
    m_pairStatus = new QLabel(repBox);
    m_pairStatus->setWordWrap(true);
    rform->addRow(QString(), m_pairStatus);
    storePageLayout->addWidget(repBox);

    storePageLayout->addStretch(1);
    add_settings_tab(tabs, storePage, tr_("Dock.Storage"));

    // Both of these are in BOTH docks on purpose: choosing a role hides the
    // other dock, so a control living in only one of them could hide the only
    // way back.
    auto* machinePage = new QWidget(tabs);
    auto* machineLayout = new QVBoxLayout(machinePage);
    machineLayout->addWidget(make_role_selector(machinePage));
    machineLayout->addWidget(make_remote_control_box(machinePage));
    // A machine-wide choice like the role above it, and in both docks for the
    // same reason: whichever role this box is, the setting is about the box.
    m_checkUpdates = new QCheckBox(tr_("Dock.CheckUpdates"), machinePage);
    m_checkUpdates->setToolTip(tr_("Dock.CheckUpdatesHint"));
    machineLayout->addWidget(m_checkUpdates);
    machineLayout->addStretch(1);
    add_settings_tab(tabs, machinePage, tr_("Dock.ThisMachine"));

    dlgRoot->addWidget(tabs, 1);            // the tabs take the stretch

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Close | QDialogButtonBox::Apply, m_settings);
    dlgRoot->addWidget(buttons, 0);         // …and the buttons never scroll
    // Closing does NOT apply. Opening this dialog to look, then closing it,
    // must leave a live event exactly as it was. The old shape saved and
    // reconfigured unconditionally after exec(), so merely looking at the
    // settings could tear down and rebuild a running receive session. Only
    // Apply commits (Start reads the fields itself).
    connect(buttons, &QDialogButtonBox::rejected, m_settings, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked,
            this, &DecoderDock::onApplySettings);

    // Keeping the provider's fields in step is a display concern, so it stays
    // live; nothing here saves.
    connect(m_provider, &QComboBox::currentIndexChanged,
            this, &DecoderDock::updateProviderFields);

    // Closing with unapplied edits asks first (see settings_dialog.h). That is
    // the other half of "closing is not a commit": closing must not silently
    // throw away what was typed either.
    m_settings->is_dirty = [this] { return m_dirty; };
    m_settings->apply_changes = [this] { onApplySettings(); };
    // textEdited, not textChanged: only a person's typing counts, and the
    // setText in loadIntoFields must not. The spin boxes and the dropdown have
    // no such distinction, so they are guarded by m_loading instead.
    for (QLineEdit* e : { m_accountId, m_endpoint, m_bucket, m_keyId, m_secret,
                          m_region, m_roomId, m_siteName, m_cacheDir,
                          m_lanHost, m_lanToken,
                          m_reporterUrl, m_reporterId, m_reporterToken })
        connect(e, &QLineEdit::textEdited, this, [this] { m_dirty = true; });
    for (QSpinBox* sb : { m_prebuffer, m_startBufferS, m_bufferMins,
                          m_pollMs, m_keepBehind, m_lanPortField })
        connect(sb, &QSpinBox::valueChanged, this,
                [this](int) { if (!m_loading) m_dirty = true; });
    connect(m_provider, &QComboBox::currentIndexChanged, this,
            [this](int) { if (!m_loading) m_dirty = true; });
    connect(m_reporterEnabled, &QCheckBox::toggled, this,
            [this](bool) { if (!m_loading) m_dirty = true; });

    loadIntoFields();

    root->addStretch(1);

    // Always on screen, so the version in a bug report is the real one and
    // nobody has to be told where to find it.
    m_version = new QLabel(QString("obs-multisite %1").arg(PLUGIN_VERSION), this);
    m_version->setStyleSheet("color: palette(text); opacity: 0.55;");
    root->addWidget(m_version);
    // Only ever filled in when a newer build actually exists — see refresh().
    m_update = new QLabel(QString(), this);
    m_update->setOpenExternalLinks(true);
    m_update->setWordWrap(true);
    m_update->setStyleSheet("color: #3b82c4;");
    m_update->hide();
    root->addWidget(m_update);

    // Where everything else about this lives, worded exactly as the encoder dock
    // words it and for the same reason its status rows are: an operator who has
    // seen one dock should not have to learn the other. Left in Qt's own link
    // colour rather than the dim grey above, so it reads on either theme.
    auto* brand = new QLabel(
        QStringLiteral(
            "obs-multisite &middot; "
            "<a href=\"https://stageaudioworks.github.io/obs-multisite/\">"
            "manual, downloads and source</a>"),
        this);
    brand->setOpenExternalLinks(true);
    brand->setWordWrap(true);
    root->addWidget(brand);

    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &DecoderDock::refresh);
    m_timer->start(500);

    // Repaint five times per state refresh so the playhead glides instead of
    // stepping twice a second. This timer NEVER samples state — it only
    // repaints from the model refresh() leaves behind, which is what keeps a
    // single writer on the widgets.
    m_smoothTimer = new QTimer(this);
    connect(m_smoothTimer, &QTimer::timeout, this, &DecoderDock::paintPosition);
    m_smoothTimer->start(100);
    refresh();
}

void DecoderDock::loadIntoFields() {
    m_loading = true;   // the setText/setValue below are not the operator's edits
    const DecoderSettings cfg = decoder_settings();
    // Empty storage_provider means this was saved before the provider dropdown
    // existed: fall back to guessing from the raw fields rather than defaulting
    // blindly to R2, so an upgrade never misrepresents a working
    // AWS/Backblaze/Wasabi/Custom setup as something it isn't.
    auto provider = cfg.storage_provider.empty()
        ? multisite::detect_provider(cfg.endpoint_host, cfg.r2_account_id)
        : multisite::provider_from_key(cfg.storage_provider);
    const int idx = m_provider->findData(
        QString::fromStdString(multisite::provider_key(provider)));
    m_provider->setCurrentIndex(idx >= 0 ? idx : 0);
    m_accountId->setText(QString::fromStdString(cfg.r2_account_id));
    m_endpoint->setText(QString::fromStdString(cfg.endpoint_host));
    m_bucket->setText(QString::fromStdString(cfg.bucket));
    m_keyId->setText(QString::fromStdString(cfg.access_key_id));
    m_secret->setText(QString::fromStdString(cfg.secret_access_key));
    m_region->setText(QString::fromStdString(cfg.region));
    m_roomId->setText(QString::fromStdString(cfg.room_id));
    m_siteName->setText(QString::fromStdString(cfg.site_name));
    m_prebuffer->setValue(cfg.prebuffer_segments);
    m_startBufferS->setValue(cfg.start_buffer_seconds);
    m_bufferMins->setValue(cfg.buffer_minutes);
    m_pollMs->setValue(cfg.poll_interval_ms);
    m_keepBehind->setValue(cfg.keep_behind_segments);
    m_cacheDir->setText(QString::fromStdString(cfg.cache_dir));
    m_lanHost->setText(QString::fromStdString(cfg.lan_host));
    m_lanPortField->setValue(cfg.lan_port);
    m_lanToken->setText(QString::fromStdString(cfg.lan_auth_token));
    m_reporterEnabled->setChecked(cfg.reporter_enabled);
    m_reporterUrl->setText(QString::fromStdString(cfg.reporter_url));
    m_reporterId->setText(QString::fromStdString(cfg.reporter_appliance_id));
    m_reporterToken->setText(QString::fromStdString(cfg.reporter_token));
    // Machine-wide, so read from its own store rather than from cfg — and read
    // here, on every open, so a change made in the other dock's dialog shows up.
    m_checkUpdates->setChecked(update_check_enabled());
    m_secondary->loadFromStore();
    updateProviderFields();
    m_loading = false;
    m_dirty = false;
}

void DecoderDock::onApplySettings() {
    onSaveSettings();
    decoder_reconfigure_all();   // apply now, without closing the dialog
}

void DecoderDock::onPairBegin() {
    // Commit first, so the worker pairs against the saved collector URL —
    // the same rule Start follows before it connects.
    onSaveSettings();
    m_pairWasDone = false;
    if (!reporter_pair_begin("obs-decoder"))
        m_pairStatus->setText(tr_("Dock.ReporterNeedUrl"));
}

void DecoderDock::onPairCancel() {
    reporter_pair_cancel("obs-decoder");
}

void DecoderDock::refreshPairing() {
    if (!m_pairStatus) return;
    const PairView v = reporter_pair_view("obs-decoder");
    const QString reason = !v.note.empty() ? QString::fromStdString(v.note)
                                           : QString::fromStdString(v.error);
    switch (v.phase) {
        case 1: {
            const QString t = tr_("Dock.ReporterCode").arg(
                QString::fromStdString(v.user_code),
                QString::fromStdString(v.verification_url));
            if (m_pairStatus->text() != t) m_pairStatus->setText(t);
            m_pairBtn->setEnabled(false);
            m_pairCancel->setEnabled(true);
            break;
        }
        case 2: {
            // Gate on the save, not just the approval — see the encoder
            // dock's note on the Apply-wipe this prevents.
            if (!v.saved) {
                m_pairBtn->setEnabled(false);
                m_pairCancel->setEnabled(true);
                break;
            }
            if (!m_pairWasDone) {
                m_pairWasDone = true;
                loadIntoFields();
                reporter_pair_cancel("obs-decoder");
            }
            if (m_pairStatus->text() != tr_("Dock.ReporterDone"))
                m_pairStatus->setText(tr_("Dock.ReporterDone"));
            m_pairBtn->setEnabled(true);
            m_pairCancel->setEnabled(false);
            break;
        }
        case 3:
            if (m_pairStatus->text() != tr_("Dock.ReporterExpired"))
                m_pairStatus->setText(tr_("Dock.ReporterExpired"));
            m_pairBtn->setEnabled(true);
            m_pairCancel->setEnabled(false);
            break;
        case 4: {
            const QString t = tr_("Dock.ReporterFailed").arg(reason);
            if (m_pairStatus->text() != t) m_pairStatus->setText(t);
            m_pairBtn->setEnabled(true);
            m_pairCancel->setEnabled(false);
            break;
        }
        default:
            if (!m_pairWasDone && !m_pairStatus->text().isEmpty())
                m_pairStatus->setText(QString());
            m_pairBtn->setEnabled(true);
            m_pairCancel->setEnabled(false);
            break;
    }
}

void DecoderDock::onOpenSettings() {
    if (!m_settings) return;

    // Show what is actually saved. The dialog is built once and reused, so
    // without this it would show whatever was left in the fields — including
    // edits that were never applied.
    loadIntoFields();

    // Fit the screen it is about to open on, not the one it was built on.
    //
    // The scroll area means the content can always be reached, but Qt will
    // still happily size the dialog to its natural height and hand you a window
    // taller than the display — which on a laptop put the buttons below the
    // bottom edge. Capping it here rather than at construction is deliberate:
    // an operator may have moved OBS to a different monitor, or docked a laptop,
    // since the dock was created.
    if (QScreen* sc = m_settings->screen() ? m_settings->screen()
                                           : QGuiApplication::primaryScreen()) {
        const QRect avail = sc->availableGeometry();
        // Nine tenths, not all of it: a dialog exactly the height of the work
        // area has its title bar under the menu bar on macOS and is then
        // impossible to move.
        m_settings->setMaximumHeight((int)(avail.height() * 0.9));
        m_settings->setMaximumWidth((int)(avail.width() * 0.9));
        if (m_settings->height() > m_settings->maximumHeight())
            m_settings->resize(m_settings->width(), m_settings->maximumHeight());
        if (m_settings->width() > m_settings->maximumWidth())
            m_settings->resize(m_settings->maximumWidth(), m_settings->height());
    }

    m_settings->exec();
    // Deliberately nothing after exec(): closing is not a commit. Apply
    // commits, and a close leaves the running session untouched.
}

void DecoderDock::updateProviderFields() {
    if (!m_provider) return;
    auto provider = multisite::provider_from_key(
        m_provider->currentData().toString().toStdString());
    const auto& info = multisite::provider_info(provider);
    if (auto* form = qobject_cast<QFormLayout*>(m_accountId->parentWidget()->layout())) {
        form->setRowVisible(m_accountId, info.needs_account_id);
        form->setRowVisible(m_endpoint,  info.needs_endpoint);
        form->setRowVisible(m_region,    info.needs_region);
    }
}

void DecoderDock::onSaveSettings() {
    // Trimmed: these are pasted from a dashboard, and a stray space produces
    // failures that look nothing like their cause.
    DecoderSettings cfg = decoder_settings();    // The provider decides how the fields the operator actually typed become
    // the raw shape DecoderSettings shares with S3Config — see
    // storage_providers.h. Custom's fields already ARE that shape, unchanged.
    auto provider = multisite::provider_from_key(
        m_provider->currentData().toString().toStdString());
    cfg.storage_provider = multisite::provider_key(provider);
    if (provider == multisite::StorageProvider::Custom) {
        cfg.r2_account_id = "";
        cfg.endpoint_host = m_endpoint->text().trimmed().toStdString();
        cfg.region        = m_region->text().trimmed().toStdString();
    } else {
        const std::string input = provider == multisite::StorageProvider::CloudflareR2
            ? m_accountId->text().trimmed().toStdString()
            : m_region->text().trimmed().toStdString();
        auto derived = multisite::derive(provider, input);
        cfg.r2_account_id = derived.r2_account_id;
        cfg.endpoint_host = derived.endpoint_host;
        cfg.region        = derived.region;
    }
    cfg.bucket            = m_bucket->text().trimmed().toStdString();
    cfg.access_key_id     = m_keyId->text().trimmed().toStdString();
    cfg.secret_access_key = m_secret->text().trimmed().toStdString();
    cfg.room_id           = m_roomId->text().trimmed().toStdString();
    // The feed name is now the only place this machine learns which room to
    // follow (the source properties no longer carry one), so an empty field
    // must not be allowed to leave every source asking for a nameless room.
    // Same guard the remote-control page applies.
    if (cfg.room_id.empty()) cfg.room_id = "main-auditorium";
    cfg.site_name         = m_siteName->text().trimmed().toStdString();
    cfg.prebuffer_segments = m_prebuffer->value();
    cfg.start_buffer_seconds = m_startBufferS->value();
    cfg.buffer_minutes     = m_bufferMins->value();
    cfg.poll_interval_ms   = m_pollMs->value();
    cfg.keep_behind_segments = m_keepBehind->value();
    cfg.cache_dir          = m_cacheDir->text().trimmed().toStdString();
    cfg.lan_host           = m_lanHost->text().trimmed().toStdString();
    cfg.lan_port           = m_lanPortField->value();
    cfg.lan_auth_token     = m_lanToken->text().trimmed().toStdString();
    cfg.reporter_enabled      = m_reporterEnabled->isChecked();
    cfg.reporter_url          = m_reporterUrl->text().trimmed().toStdString();
    cfg.reporter_appliance_id = m_reporterId->text().trimmed().toStdString();
    cfg.reporter_token        = m_reporterToken->text().trimmed().toStdString();
    // A claim that landed after the fields were last loaded lives in settings
    // but not yet in these widgets; writing the widgets back would wipe it.
    // Clearing stays possible: Cancel first (leaving Done), then clear.
    if (reporter_pair_view("obs-decoder").phase == 2) {
        const DecoderSettings live = decoder_settings_copy();
        cfg.reporter_appliance_id = live.reporter_appliance_id;
        cfg.reporter_token = live.reporter_token;
        cfg.reporter_device_id = live.reporter_device_id;
    }
    set_decoder_settings(cfg);
    // Its own store, and committed with Apply like everything else on this
    // dialog — so opening the settings to look at something still changes
    // nothing, which is the promise the Apply button exists to make.
    update_check_set_enabled(m_checkUpdates->isChecked());
    m_secondary->saveToStore();
    m_dirty = false;
}

void DecoderDock::showTestConnection(const ProbeResult& r) {
    m_testConnection->setEnabled(true);
    if (r.ok) {
        m_testConnectionResult->setText(r.found_live ? tr_("Dock.TestConnectionOkLive")
                                                     : tr_("Dock.TestConnectionOkSat"));
        m_testConnectionResult->setStyleSheet("color: #35c489;");
    } else {
        m_testConnectionResult->setText(
            tr_("Dock.TestConnectionFailed").arg(QString::fromStdString(r.detail)));
        m_testConnectionResult->setStyleSheet("color: #e5484d;");
    }
}

void DecoderDock::onStart() {
    // Load: apply settings, (re)connect and start filling the buffer. It does
    // NOT go to air — that is what Play is for.
    onSaveSettings();
    decoder_reconfigure_all();
    decoder_jump_live_all();
}

void DecoderDock::onPlay()  { decoder_play_all(); }
void DecoderDock::onStop()  { decoder_stop_all(); }
void DecoderDock::onJog(double seconds) { decoder_jog(seconds); }
void DecoderDock::onGoToDelay() {
    decoder_set_delay(m_delayMins ? m_delayMins->value() * 60.0 : 0.0);
}
void DecoderDock::onLockToggled(bool on) {
    decoder_set_locked(on);
    if (m_lock) m_lock->setText(on ? tr_("Dock.Unlock") : tr_("Dock.Lock"));
}

void DecoderDock::onPause()    { decoder_pause_all(); }
void DecoderDock::onResume()   { decoder_resume_all(); }
void DecoderDock::onJumpLive() { decoder_jump_live_all(); }

void DecoderDock::onJumpMarker() {
    const QString id = m_markers->currentData().toString();
    if (id.isEmpty()) return;
    decoder_jump_to_marker(id.toStdString());
}

void DecoderDock::onSeek(long long media_ms) {
    // Hand the clicked TIME over, not a segment number.
    //
    // This used to divide by the segment length and drop the remainder, so a
    // click landed up to six seconds from where it was made — and the segment
    // length is the SESSION's to know, not the dock's, which is why the
    // conversion belongs there (DecoderSession::seek_to_media_ms).
    if (media_ms < 0) media_ms = 0;
    decoder_seek_media(media_ms);
}

// ── Recordings ───────────────────────────────────────────────────────────────
void DecoderDock::onRefreshEvents() { decoder_refresh_events(); }

void DecoderDock::onLoadEvent() {
    if (!m_events) return;
    auto* item = m_events->currentItem();
    if (!item) return;
    const QString id = item->data(Qt::UserRole).toString();
    if (id.isEmpty()) return;

    // Replacing what is on air is worth one question. Loading buffers and does
    // not go to air, so the cost is not "the wrong event plays" — it is that the
    // picture STOPS and does not come back until Play, which is not what anyone
    // wants to discover mid-service.
    DecoderSnapshot now;
    if (decoder_snapshot(now) && (now.playing || now.paused) &&
        !now.event_id.empty() && now.event_id != id.toStdString()) {
        const auto answer = QMessageBox::question(
            this, tr_("Dock.ReplaceTitle"),
            tr_("Dock.ReplaceText").arg(item->text()),
            QMessageBox::Cancel | QMessageBox::Ok, QMessageBox::Cancel);
        if (answer != QMessageBox::Ok) return;
    }

    // Loading buffers; it does not go to air. Play is a separate, deliberate
    // press — the same rule as loading the live feed.
    m_loadingName = item->text();
    // Acknowledge the click HERE, not on the next 500 ms tick: a small
    // recording loads fast enough that the loading state could come and go
    // between two ticks, which is exactly the "I pressed Load and nothing
    // happened" this is for.
    m_playback->setText(tr_("Dock.Loading"));
    m_playback->setStyleSheet("color: #3b82c4; font-weight: bold;");
    m_posText  = tr_("Dock.LoadingNamed").arg(short_name(m_loadingName));
    m_posStyle = "font-size: 18px; font-weight: 500; color: #3b82c4;";
    paintPosition();
    decoder_pin_event(id.toStdString());
}

void DecoderDock::onEventActivated() { onLoadEvent(); }

void DecoderDock::onReturnToLive() {
    decoder_unpin_event();
}

// Build one row: the time an operator would recognise, what state it is in,
// and how long it runs.
static QString event_row_text(const EventEntry& e) {
    QString when = e.started_ms > 0
        ? QDateTime::fromMSecsSinceEpoch(e.started_ms).toString("ddd d MMM  HH:mm")
        : QString("(unknown time)");

    QString state;
    switch (e.state) {
        case 1: state = tr_("Dock.EventLive");        break;
        case 2: state = QString();                    break;  // an ordinary recording needs no label
        case 3: state = tr_("Dock.EventInterrupted"); break;
        default: state = tr_("Dock.EventUnknown");    break;
    }

    QString len;
    if (e.duration_s > 0) {
        const long long mins = e.duration_s / 60;
        len = mins >= 60
            ? QString("%1h %2m").arg(mins / 60).arg(mins % 60, 2, 10, QChar('0'))
            : QString("%1 min").arg(mins);
    }

    QString row;
    const QString name = QString::fromStdString(e.name).trimmed();
    if (!name.isEmpty()) row = name + "   " + when;   // title first, time after
    else                 row = when;
    if (!len.isEmpty())   row += "   " + len;
    if (!state.isEmpty()) row += "   " + state;
    return row;
}

void DecoderDock::refreshEvents(const DecoderSnapshot& s) {
    if (!m_events) return;

    EventListing listing;
    if (!decoder_event_listing(listing)) {
        m_events->clear();
        m_events_signature.clear();
        m_eventsNote->setText(QString());
        m_loadEvent->setEnabled(false);
        m_returnLive->setEnabled(false);
        m_refreshEvents->setEnabled(false);
        if (m_liveElsewhere) m_liveElsewhere->hide();
        return;
    }
    m_refreshEvents->setEnabled(!listing.loading && !s.locked);

    // Which row is playing comes from the live snapshot, not from the cached
    // listing. It used to be stamped into each row during the listing refresh,
    // which meant the marker showed whatever was playing at that instant — and
    // if a refresh landed between pinning an event and the poll that applied
    // it, the bold row stayed on the previous recording until the operator
    // pressed Refresh.
    const QString playing = QString::fromStdString(s.event_id);

    // Rebuild only on a real change, so a selection survives a refresh tick.
    // The playing event is part of that: the marker must move when it does.
    QString sig = playing + "#";
    for (const auto& e : listing.events)
        sig += QString::fromStdString(e.event_id) + ":" +
               QString::number(e.state) + ":" +
               QString::number(e.duration_s) + "|";
    if (sig != m_events_signature) {
        const QString keep = m_events->currentItem()
            ? m_events->currentItem()->data(Qt::UserRole).toString() : QString();
        m_events_signature = sig;
        m_events->clear();
        for (const auto& e : listing.events) {
            const QString id = QString::fromStdString(e.event_id);
            auto* item = new QListWidgetItem(event_row_text(e), m_events);
            item->setData(Qt::UserRole, id);
            // Numeric rather than "#rrggbb": QColor's string constructor
            // differs between Qt 5 and 6, and the painter above already uses
            // this form.
            if (e.state == 1)                                            // live
                item->setForeground(QColor(0xe5, 0x48, 0x4d));
            else if (e.state == 3)                                       // interrupted
                item->setForeground(QColor(0xe0, 0xa0, 0x20));
            // Bold alone was ambiguous against the selection highlight — the
            // operator could not tell which row was SELECTED from which was
            // LOADED, and they are often different rows on purpose.
            if (!playing.isEmpty() && id == playing) {
                QFont f = item->font();
                f.setBold(true);
                item->setFont(f);
                item->setText(item->text() + "   " + tr_("Dock.EventPlaying"));
            }
            if (!keep.isEmpty() && keep == id)
                m_events->setCurrentItem(item);
        }
    }

    // Why the list might be short or empty. "No recordings" and "this key
    // cannot list the bucket" look identical without this.
    QString note;
    if (listing.loading) {
        note = tr_("Dock.EventsLoading");
    } else if (!listing.error.empty()) {
        note = QString::fromStdString(listing.error);
    } else if (!listing.listed_once) {
        note = tr_("Dock.EventsLoading");
    } else if (listing.no_catalog) {
        // Nothing to list, and nothing here that could ever list it: recordings
        // are built from a bucket. "No recordings found" would be a lie by
        // omission — the operator turned cloud upload off and this is what it
        // costs, so say the cost rather than an empty box.
        note = tr_("Dock.EventsNoCloud");
    } else if (listing.events.empty()) {
        note = tr_("Dock.EventsNone");
    } else if (listing.fallback_scan) {
        // Worth saying: these were recorded before the room index existed, so
        // the list cost a request per event to build.
        note = tr_("Dock.EventsFallback");
    }
    m_eventsNote->setText(note);
    m_eventsNote->setVisible(!note.isEmpty());
    if (!listing.error.empty())
        m_eventsNote->setStyleSheet("color: #e5484d;");
    else
        m_eventsNote->setStyleSheet("color: palette(text); opacity: 0.75;");

    const bool ctl = !s.locked;
    // While a switch is in flight the button says so and refuses further
    // presses: an unresponsive-looking button is what makes an operator click
    // it repeatedly, queuing up switches nobody asked for.
    m_loadEvent->setText(s.loading ? tr_("Dock.LoadingShort")
                                   : tr_("Dock.LoadRecording"));
    m_loadEvent->setEnabled(ctl && !s.loading &&
                            m_events->currentItem() != nullptr);
    m_returnLive->setEnabled(ctl && !s.loading && !s.pinned_event_id.empty());

    // An event has started while a recording is pinned. The decoder stays
    // where it is on purpose — being pulled out of a recording mid-watch would
    // be worse — so the dock offers the switch rather than taking it.
    if (m_liveElsewhere) {
        const bool show = s.live_elsewhere && !s.pinned_event_id.empty();
        m_liveElsewhere->setVisible(show);
        if (show) m_liveElsewhere->setText(tr_("Dock.LiveElsewhere"));
    }
}

void DecoderDock::refresh() {
    // The update check is about this machine, not about the source, so it is
    // answered before the snapshot and is shown even when there is no source.
    // Nothing is said when the check could not reach GitHub, or when this build
    // is current: only a newer build produces a line.
    {
        QString text;
        if (update_check_state() == UpdateState::Newer) {
            const QString tag = QString::fromStdString(update_check_latest());
            text = QString("<a href=\"%1\">%2</a>")
                       .arg(QString::fromUtf8(kReleasesUrl),
                            tr_("Dock.UpdateAvailable").arg(tag, PLUGIN_VERSION));
        }
        if (m_update->text() != text) {
            m_update->setText(text);
            m_update->setVisible(!text.isEmpty());
        }
    }

    // The heartbeat's last answer, in the settings dialog — answered before
    // the snapshot so it shows with no source too. Compared before setting.
    if (m_reporterState) {
        const QString t =
            QString::fromStdString(reporter_last_result("obs-decoder"));
        if (m_reporterState->text() != t) m_reporterState->setText(t);
    }
    refreshPairing();

    DecoderSnapshot s;
    if (!decoder_snapshot(s)) {
        m_room->setText(tr_("Dock.NoSource"));
        m_state->setText(QString());
        m_playback->setText(QString());
        m_posValid = false;
        paintPosition();
        m_net->setText("—");
        m_net->setStyleSheet(QString());
        m_error->hide();
        m_pause->setEnabled(false);
        m_resume->setEnabled(false);
        m_live->setEnabled(false);
        m_jumpMarker->setEnabled(false);
        // Clear the bar rather than leaving whatever was last drawn on it.
        // A stale timeline under a dock that says "no source" is worse than
        // an empty one, and an empty one with no explanation is what made
        // this state look like a fault.
        m_timeline->setSpan(0, 0);
        m_timeline->setDownloaded({});
        m_timeline->setMarkers({});
        m_timeline->setPlaceholder(tr_("Dock.TimelineNoSource"));
        refreshEvents(s);
        return;
    }

    m_room->setText(QString::fromStdString(s.room_id));
    // Lock disables everything that changes what is on air.
    const bool ctl = !s.locked;
    m_pause->setEnabled(ctl && s.playing && !s.paused);
    m_resume->setEnabled(ctl && s.paused);
    m_live->setEnabled(ctl);
    // Play only when the SESSION would actually start. It used to be enabled
    // whenever nothing was on air, so pressing it too early declined silently
    // and left an indefinite "BUFFERING…" — an operator cannot tell that from a
    // dropped click.
    m_play->setEnabled(ctl && !s.playing && s.ready_to_play);
    if (!s.ready_to_play && !s.playing)
        m_play->setToolTip(tr_("Dock.PlayNotReady"));
    else
        m_play->setToolTip(QString());
    // Stop is also the way out of a load that is not becoming ready: it cancels
    // what is in flight and tears the decoder down, which is the honest "give
    // up on this" in a model where a load is applied the moment it is asked for.
    const bool busy = s.playing || s.loading ||
                      (!s.ready_to_play && s.gate_s > 0.0);
    m_stop->setEnabled(ctl && busy);
    m_stop->setToolTip(s.playing ? QString() : tr_("Dock.StopLoading"));
    m_start->setEnabled(ctl);
    m_goTo->setEnabled(ctl);
    m_jumpMarker->setEnabled(ctl && m_markers->count() > 0);
    if (m_lock && m_lock->isChecked() != s.locked) {
        m_lock->blockSignals(true);
        m_lock->setChecked(s.locked);
        m_lock->setText(s.locked ? tr_("Dock.Unlock") : tr_("Dock.Lock"));
        m_lock->blockSignals(false);
    }

    // ── What THIS box is doing ───────────────────────────────────────────────
    // Playback state only. An action in progress outranks the settled state:
    // the dock ticks every 500 ms, so this is what turns a click into
    // something visible long before the network has finished answering it.
    // Every branch is a state of this decoder — nothing about the main site
    // appears here, which is the whole point of the split.
    // The start gate and how much of it is down — the SESSION's figures, not a
    // second opinion computed here. The dock used to re-derive both from the
    // start-buffer setting, which is right for a live event and wrong for a
    // finished recording: that needs one segment, and was being shown a
    // sixty-second countdown.
    const int gateHave = (int)(s.ready_buffer_s + 0.5);
    const int gateWant = (int)(s.gate_s + 0.5);

    // How fast the gate is filling, from this dock's own two readings. The
    // session does not report a rate, and this is the honest kind — what is
    // visibly happening. Reset when the event changes, and omitted rather than
    // guessed when it cannot be measured yet.
    const long long nowWall = (long long)QDateTime::currentMSecsSinceEpoch();
    if (s.ready_buffer_s + 0.01 < m_fillPrevS) {
        m_fillRateS = 0.0;                       // a different event: start again
    } else if (m_fillPrevS >= 0.0 && s.ready_buffer_s > m_fillPrevS + 0.01 &&
               nowWall > m_fillPrevWallMs + 200) {
        m_fillRateS = (s.ready_buffer_s - m_fillPrevS) /
                      ((nowWall - m_fillPrevWallMs) / 1000.0);
    }
    m_fillPrevS = s.ready_buffer_s;
    m_fillPrevWallMs = nowWall;

    auto preparing_text = [&] {
        QString t = tr_("Dock.Preparing").arg(gateHave).arg(gateWant);
        if (m_fillRateS > 0.05 && s.ready_buffer_s < s.gate_s) {
            const int eta =
                (int)((s.gate_s - s.ready_buffer_s) / m_fillRateS + 0.5);
            if (eta >= 1 && eta <= 6000) t += "  ·  " + tr_("Dock.ReadyIn").arg(eta);
        }
        return t;
    };

    if (s.stopped) {
        m_playback->setText(tr_("Dock.Pb.Stopped"));
        m_playback->setStyleSheet("color: #8b9198; font-weight: bold;");
    } else if (s.loading && !s.ready_to_play) {
        // AND only while it is not yet playable. The source's loading flag is
        // cleared by its own rule, so it could still be set for a tick after the
        // session had already decided it could start — which is exactly how
        // "why can I play before loading has finished?" happened. Readiness is
        // the session's answer, so it wins here too.
        //
        // Name it, too: "LOADING…" about something anonymous does not tell an
        // operator whether their click landed on the right row.
        m_playback->setText(tr_("Dock.Loading"));
        m_playback->setStyleSheet("color: #3b82c4; font-weight: bold;");
    } else if (s.seek_target_ms > 0) {
        // Heading somewhere. The position line above names where, so this only
        // has to say that the picture has not arrived yet — quoting a buffer
        // figure here would be a figure about somewhere else.
        m_playback->setText(tr_("Dock.Seeking"));
        m_playback->setStyleSheet("color: #3b82c4; font-weight: bold;");
    } else if (s.buffering) {
        // Playing, but nothing has reached the screen yet. The number that
        // matters is how much is ready AHEAD of the playhead, since that is
        // what decides when the picture resumes. This used to quote the
        // longest cached run against the start gate and printed things like
        // "84 s of 60 s": the gate only means anything before Play is first
        // pressed (the branch further down), and the cached run is legitimately
        // minutes long once buffering ahead is doing its job.
        const int ahead = (int)(s.buffered_ahead_s + 0.5);
        m_playback->setText(ahead > 0
                                ? tr_("Dock.BufferingAhead").arg(ahead)
                                : tr_("Dock.Buffering"));
        m_playback->setStyleSheet("color: #3b82c4; font-weight: bold;");
    } else if (s.paused) {
        // Previously only reachable while the room was live, because it lived
        // inside the room-state switch — so holding a finished recording showed
        // no indication whatsoever that it was held.
        m_playback->setText(tr_("Dock.Held"));
        m_playback->setStyleSheet("color: #e0a020; font-weight: bold;");
    } else if (s.playing) {
        m_playback->setText(tr_("Dock.Pb.Playing"));
        m_playback->setStyleSheet("color: #8fd3b4; font-weight: bold;");
    } else if (!s.ready_to_play && s.gate_s > 0.0) {
        // Loaded, not on air, and not yet playable: how far the buffer has got,
        // against the gate the SESSION is actually waiting for, and roughly how
        // long it has left.
        m_playback->setText(preparing_text());
        m_playback->setStyleSheet("color: #3b82c4; font-weight: bold;");
    } else {
        // Ready: the session would go to air on a press. Green, because this is
        // a good state and the one moment worth noticing.
        m_playback->setText(tr_("Dock.Pb.Ready"));
        m_playback->setStyleSheet("color: #8fd3b4; font-weight: bold;");
    }

    // ── What the MAIN SITE is doing ──────────────────────────────────────────
    // Source state only. The readout below keys off `ended` the same way this
    // does, so the two cannot disagree — they did once, and the dock showed
    // "RECORDING (not live)" above "1 min 10 sec behind".
    if (s.link_known && s.link_health == 2) {
        // The venue's connection to the bucket is gone — which is NOT the same
        // as the main site going off air. Say so specifically, because the box
        // may still be playing the buffer while this is shown.
        m_state->setText(tr_("Dock.NetLost"));
        m_state->setStyleSheet("color: #e5484d; font-weight: bold;");
    } else
    switch (s.ended ? 3 : s.room_state) {
        case 2:  // Live
            m_state->setText(tr_("Dock.Live"));
            m_state->setStyleSheet("color: #e5484d; font-weight: bold;");
            break;
        case 3:
            // Three different situations, and an operator needs to tell them
            // apart: the encoder died mid-event, the event they were
            // watching has just finished, or this was already a recording when
            // they loaded it.
            if (s.interrupted) {
                // Not a fault at this end, and the recording is complete up to
                // the moment the encoder went — it simply stops there.
                m_state->setText(tr_("Dock.Interrupted"));
                m_state->setStyleSheet("color: #e0a020; font-weight: bold;");
            } else if (s.was_live) {
                m_state->setText(tr_("Dock.BroadcastEnded"));
                m_state->setStyleSheet("color: #e0a020; font-weight: bold;");
            } else {
                m_state->setText(tr_("Dock.NotLive"));
                m_state->setStyleSheet("color: #8fd3b4;");
            }
            break;
        case 1:
            m_state->setText(tr_("Dock.Offline"));
            m_state->setStyleSheet("color: #e5484d;");
            break;
        default:
            m_state->setText(tr_("Dock.Connecting"));
            m_state->setStyleSheet("color: palette(mid);");
            break;
    }

    // ── Position model ───────────────────────────────────────────────────────
    // Describe the readout; do not draw it. paintPosition() is the only writer
    // of m_behind and the playhead, so that the smooth timer between refreshes
    // cannot contradict this. See the note in the header for what happened the
    // last time two timers shared those widgets.
    //
    // Lead with the clock time being shown — the thing an operator can match
    // against what is happening in the room — and express the offset in plain
    // language rather than as a signed number.
    // Media time is the axis: segment number x one segment, so a position is
    // the place on screen rather than a stored clock time. The two disagree on
    // an event whose encoder restarted, and only the segment agrees with the
    // picture.
    // The snapshot carries media positions directly now, so this dock no
    // longer converts segment numbers into times at all.
    //
    // It used to: seq * its own copy of the segment length, taken from the
    // NOMINAL and with its own fallback — the duplicated derivation D2 warned
    // about. The nominal is a request to the encoder, not a description of
    // what it produced (a "6 s" keyframe interval is a whole number of frames,
    // so 6.033 or 6.067), and every position on this axis was therefore out by
    // seq * the difference: seconds by the end of a service. See BUGS #2b.
    //
    // m_mediaSegMs survives only to cap one interpolation step; it is not used
    // to place anything.
    const double seg_s = s.segment_duration_s > 0.1 ? s.segment_duration_s : 6.0;
    m_mediaSegMs = (long long)(seg_s * 1000.0 + 0.5);

    // The load is over: stop naming it, so a later wait cannot show a stale
    // name from a click that has long since finished.
    if (!s.loading || s.ready_to_play) m_loadingName.clear();

    m_posValid    = true;
    m_posFixed    = true;
    m_posAnimate  = false;
    m_posVod      = false;
    m_posBoundMs  = 0;
    m_posTooltip  = QString();
    m_posBaseMs     = (long long)s.playhead_ms;
    m_posBaseWallMs = (long long)QDateTime::currentMSecsSinceEpoch();

    // Anchor the interpolation to the on-screen segment. Media time advances at
    // 1x, so wall time since this anchor is what makes the bar glide rather
    // than step once per segment.
    if (s.playhead_seq != m_mediaAnchorSeq) {
        m_mediaAnchorSeq    = s.playhead_seq;
        m_mediaAnchorMs     = m_posBaseMs;
        m_mediaAnchorWallMs = m_posBaseWallMs;
    }

    if (s.stopped) {
        // First, ahead of everything else: a stopped source is not downloading,
        // so every other line here would be describing a position that cannot
        // move. Showing "2 minutes behind" under a stopped button is the kind
        // of stale number an operator reasonably acts on.
        m_posText  = tr_("Dock.Stopped");
        m_posStyle = "font-size: 18px; font-weight: 500; color: #8b9198;";
    } else if (s.seek_target_ms > 0) {
        // A jog or a timeline click. The position updates instantly; the
        // wording makes clear the picture has not caught up yet, so the
        // operator is neither left wondering nor misled.
        // Where in the programme, not what the clock said when it was
        // recorded. An operator lining up a cue is thinking "seven minutes in",
        // and a time of day is a number they have to convert in their head.
        // "Going to 17:59" is elapsed — seventeen minutes fifty-nine — but
        // M:SS is exactly how a time of day looks, and at 17:59 the two are
        // indistinguishable. Removing the clock from this dock is worth
        // nothing if what replaced it still READS as one. Saying it against
        // the total settles it: "17:59 of 24:16" can only be a position.
        // Already a position: the snapshot carries media time now, so there is
        // no event start to subtract.
        const long long target_in = s.seek_target_ms;
        m_posText = s.total_ms > 0
            ? tr_("Dock.GoingToOf").arg(position(target_in))
                                   .arg(position((long long)s.total_ms))
            : tr_("Dock.GoingTo").arg(position(target_in));
        m_posStyle = "font-size: 18px; font-weight: 500; color: #3b82c4;";
    } else if (s.loading && !s.ready_to_play) {
        m_posText  = m_loadingName.isEmpty()
                         ? tr_("Dock.LoadingRecording")
                         : tr_("Dock.LoadingNamed").arg(short_name(m_loadingName));
        m_posStyle = "font-size: 18px; font-weight: 500; color: #3b82c4;";
    } else if (!s.playing && !s.ready_to_play && s.gate_s > 0.0) {
        // Load pressed, buffer still filling, nothing on air yet. This is the
        // largest text on the dock, so the progress belongs here as much as in
        // the state line — it is what the operator is watching while they wait.
        m_posText  = preparing_text();
        m_posStyle = "font-size: 18px; font-weight: 500; color: #3b82c4;";
    } else if (s.plays_as_recording) {
        // A finished recording: show where you are in it and how much is left.
        // How far through, out of its total length — the way a media player
        // reads. "Behind live" means nothing once the event has finished, and
        // the total length is what an operator actually wants when deciding
        // whether it will fit the slot.
        m_posFixed     = false;
        m_posVod       = true;
        m_posAtEnd     = s.at_end;
        // Elapsed is measured from the recording's OWN first segment, not from
        // media zero. Storage may no longer hold its opening seconds — the
        // encoder's spool drops the oldest under pressure, and retention does it
        // deliberately — so a recording whose first available segment is #2
        // would otherwise open at 0:12 and look like it had skipped a start
        // rather than like the opening was gone.
        m_posStartedMs = (long long)s.earliest_ms;
        m_posTotalMs   = (long long)s.end_ms - m_posStartedMs;
        // A recording cannot play past its own end, so interpolation must not
        // walk past it either.
        if (s.end_ms > 0) m_posBoundMs = (long long)s.end_ms;
        // Animate only while it is genuinely running: not held, and not
        // already sitting at the end.
        m_posAnimate = s.playing && !s.paused && !s.at_end;
    } else if (s.head > s.live_edge && s.live_edge > 0) {
        // Caught right up: nothing new has been published yet. This is normal
        // and must not look like a fault.
        m_posText  = tr_("Dock.WaitingForMain");
        m_posStyle = "font-size: 18px; font-weight: 500; color: #8fd3b4;";
    } else if (s.behind_live_s < 1.0) {
        m_posText  = tr_("Dock.ShowingNow");
        m_posStyle = "font-size: 18px; font-weight: 500; color: #8fd3b4;";
    } else {
        // Behind live and playing: the clock time advances second by second,
        // which is the case that made the 500ms step visible in the first place.
        m_posFixed   = false;
        m_posVod     = false;
        m_posBehindS = s.behind_live_s;
        // The live edge is the furthest this can meaningfully go.
        if (s.end_ms > 0) m_posBoundMs = (long long)s.end_ms;
        m_posAnimate = s.playing && !s.paused && !s.buffering;
    }

    // Timeline entirely in clock time now, with the real downloaded ranges.
    // A finished recording spans its WHOLE length — from the moment it started
    // to its true end — so the bar stops growing and the scale means something.
    // While live, the right edge is the live edge and the bar necessarily grows
    // with it.
    // A pinned event is a recording even when it is NOT cleanly ended: an
    // interrupted one spans its own length like any other. Without that, an
    // interrupted recording was drawn across the whole retained window — a
    // forty-minute event on an eight-hour axis, which is the "weird time".
    // The SESSION's answer, not a second opinion. This was hand-written here
    // and was wrong for an interrupted pinned event — which drew a 40-minute
    // recording across the whole retained window.
    const bool as_recording = s.plays_as_recording;
    if (as_recording && (s.end_ms > 0 || s.live_edge > 0)) {
        // A recording spans its whole length, in elapsed time.
        m_timeline->setSpan(0, (long long)s.end_ms);
    } else {
        // Live: the left edge is what storage still holds. Elapsed too — the
        // axis no longer switches to times of day for a live event.
        m_timeline->setSpan((long long)s.earliest_ms, (long long)s.end_ms);
    }
    // Which of the ordinary "nothing to draw yet" states this is. Only used
    // when the span is empty; the bar ignores it otherwise.
    m_timeline->setPlaceholder(
        s.event_id.empty()
            ? (s.room_state == 1 /* offline */ ? tr_("Dock.TimelineOffline")
                                               : tr_("Dock.TimelineNothing"))
            : tr_("Dock.TimelineWaiting"));
    {
        // Already media time, straight from the session.
        std::vector<std::pair<long long, long long>> dl;
        for (const auto& r : s.cached_spans)
            dl.emplace_back(r.first, r.second);
        m_timeline->setDownloaded(std::move(dl));
    }
    {
        std::vector<long long> mt;
        // The cue's own media anchor, exact and sub-segment, rather than
        // its segment number rounded to a boundary.
        for (const auto& m : s.markers)
            if (m.at_media_ms >= 0) mt.push_back((long long)m.at_media_ms);
        m_timeline->setMarkers(std::move(mt));
    }
    // Confirm the click on the bar itself, not only in the position line above
    // it. Target and axis are both media time now, so there is nothing to
    // reconcile.
    m_timeline->setPending((s.seek_target_ms > 0)
                               ? (long long)s.seek_target_ms
                               : -1);

    // Rebuild the marker list only when it changes, so the combo doesn't
    // reset while an operator is using it.
    if (s.markers.size() != m_marker_count) {
        m_marker_count = s.markers.size();
        const QString keep = m_markers->currentData().toString();
        m_markers->clear();
        for (const auto& m : s.markers) {
            // A recording's cue times run 00:00 to the end of the event; live
            // they are times of day. The SESSION's classification, so this list
            // reads in the same frame as the position line above it — a third
            // hand-written copy of the same question, and two of them
            // disagreeing is worse than either being wrong.
            const bool vod = s.plays_as_recording;
            QString when;
            // The cue's own media anchor: where in the programme it sits, with
            // no clock subtracted from another clock. Time of day only while
            // following a live event.
            if (m.at_media_ms >= 0) {
                when = position(m.at_media_ms);
            } else if (m.at_ms > 0 && s.started_ms > 0 && m.at_ms >= s.started_ms) {
                // Older than at_media_ms: convert from its time of day, which
                // is all such a cue carries.
                when = position(m.at_ms - s.started_ms);
            }
            const QString label = when.isEmpty()
                ? QString::fromStdString(m.label)
                : when + "   " + QString::fromStdString(m.label);
            m_markers->addItem(label, QString::fromStdString(m.id));
        }
        const int idx = m_markers->findData(keep);
        if (idx >= 0) m_markers->setCurrentIndex(idx);
    }

    // The reliability figure: how long this campus could keep broadcasting if
    // the connection died right now.
    m_buffered->setText(friendly_duration(s.buffered_ahead_s));
    // When the link is down, the buffered figure is the number that matters —
    // colour it so it cannot be read as an ordinary figure.
    m_buffered->setStyleSheet(
        s.link_known && s.link_health == 2 ? "color: #e5484d;" : QString());

    // The internet reading, distinct from the room state: a room that is
    // offline with a good connection is "nothing on air", not an outage.
    if (!s.link_known) {
        m_net->setText("—");
        m_net->setStyleSheet(QString());
    } else switch (s.link_health) {
        case 0:
            m_net->setText(tr_("Dock.NetHealthy"));
            m_net->setStyleSheet("color: #35c489;");
            break;
        case 1:
            m_net->setText(tr_("Dock.NetDegraded"));
            m_net->setStyleSheet("color: #e0a020;");
            break;
        default:
            m_net->setText(tr_("Dock.NetOffline"));
            m_net->setStyleSheet("color: #e5484d;");
            break;
    }

    if (m_storage) {
        QString text = multisite_ui::link_summary(
            QString::fromStdString(s.colo),
            QString::fromStdString(s.storage_host),
            s.download_bytes_per_s, s.download_samples);
        // Visibility (§8.7): which path the most recent fetch actually took.
        // Only worth saying when LAN is even configured — otherwise this is
        // just the cloud path it has always been, and saying so would be
        // noise on every single machine that hasn't touched this feature.
        if (s.lan_configured)
            text += s.lan_active ? "  ·  " + tr_("Dock.ViaLan")
                                  : "  ·  " + tr_("Dock.ViaCloud");
        // And which BUCKET, when there are two: the reads moved because the end
        // that was being read stopped advancing, which an operator needs to know
        // rather than infer from a manifest they cannot see.
        if (s.reading_secondary)
            text += "  ·  " + tr_("Dock.ViaSecond");
        multisite_ui::set_value(m_storage, text);
    }
    m_buffered->setToolTip(tr_("Dock.BufferedHint"));
    // How far back the recording still exists in storage (not on this PC).
    {
        const double back = (s.playhead_ms > s.earliest_ms && s.earliest_ms > 0)
            ? (double)(s.playhead_ms - s.earliest_ms) / 1000.0 : 0.0;
        m_cached->setText(friendly_duration(back));
        m_cached->setToolTip(tr_("Dock.RewindHint"));
    }
    m_marker->setText(s.current_marker.empty()
                        ? QString("—")
                        : QString::fromStdString(s.current_marker));
    // Show what the audio actually contains, using the names the main site
    // published — otherwise those names are write-only and the channel count
    // means nothing to the person watching.
    if (s.audio_channels <= 0) {
        m_audio->setText(QString("—"));
        m_audio->setToolTip(QString());
    } else if (!s.channel_labels.empty()) {
        QStringList names;
        for (const auto& c : s.channel_labels)
            names << QString::fromStdString(c);
        m_audio->setText(tr_("Dock.Channels").arg(s.audio_channels));
        m_audio->setToolTip(names.join(", "));
        // First few inline so it is visible without hovering.
        const int show = qMin(3, names.size());
        m_audio->setText(tr_("Dock.Channels").arg(s.audio_channels) + "  (" +
                         QStringList(names.mid(0, show)).join(", ") +
                         (names.size() > show ? "…" : "") + ")");
    } else {
        m_audio->setText(tr_("Dock.Channels").arg(s.audio_channels) +
                         (s.audio_track_label.empty()
                            ? QString()
                            : "  " + QString::fromStdString(s.audio_track_label)));
        m_audio->setToolTip(QString());
    }

    // Same bands and same wording as the encoder dock — see clock_skew_level().
    // A satellite with a badly wrong clock cannot READ from the store either:
    // the request is signed the same way.
    QString warn = friendly_error(s.last_error);
    const auto level = multisite_ui::clock_skew_level(s.clock_skew_ms);
    if (warn.isEmpty() && level != multisite_ui::ClockSkew::Fine) {
        const QString by = multisite_ui::clock_skew_text(s.clock_skew_ms);
        warn = (level == multisite_ui::ClockSkew::Urgent
                    ? tr_("Dock.ClockOutUrgent") : tr_("Dock.ClockOut")).arg(by);
    }
    if (!warn.isEmpty()) {
        m_error->setText(warn);
        m_error->show();
    } else {
        m_error->hide();
    }

    refreshEvents(s);

    // Draw once now with this fresh sample, rather than leaving the readout a
    // tick behind whenever state changes. The smooth timer takes it from here.
    paintPosition();
}

// The only writer of m_behind and the playhead.
void DecoderDock::paintPosition() {
    if (!m_posValid) {
        m_behind->setText("—");
        m_behind->setToolTip(QString());
        m_timeline->setPlayhead(0);
        return;
    }

    const long long head = livePlayheadMs();
    m_timeline->setPlayhead(head);

    // QLabel::setText and TimelineBar::setPlayhead both no-op on an unchanged
    // value, but setStyleSheet re-parses the sheet every call — at ten calls a
    // second that is pure waste, so it is guarded by hand.
    const auto style = [this](const char* css) {
        if (m_posAppliedStyle == QLatin1String(css)) return;
        m_posAppliedStyle = QLatin1String(css);
        m_behind->setStyleSheet(m_posAppliedStyle);
    };

    if (m_posFixed) {
        if (m_posAppliedStyle != m_posStyle) {
            m_posAppliedStyle = m_posStyle;
            m_behind->setStyleSheet(m_posAppliedStyle);
        }
        m_behind->setText(m_posText);
        m_behind->setToolTip(m_posTooltip);
        return;
    }

    if (m_posVod) {
        const long long elapsed = head > m_posStartedMs ? head - m_posStartedMs : 0;
        const QString pos = position(elapsed) +
            (m_posTotalMs > 0 ? "  /  " + position(m_posTotalMs) : QString());
        if (m_posAtEnd) {
            style("font-size: 18px; font-weight: 500; color: #8b9198;");
            m_behind->setText(pos + "   " + tr_("Dock.AtEnd"));
        } else {
            style("font-size: 18px; font-weight: 500; color: #8fd3b4;");
            m_behind->setText(pos);
        }
        // No time-of-day tooltip. The line above already says where in the
        // recording this is, which is the only thing the position means; a
        // clock time is what the encoder's machine happened to read at the
        // time and tells an operator playing it back nothing they can use.
        m_behind->setToolTip(QString());
        return;
    }

    // Following a live event, the meaningful quantity is how far behind the
    // main site this campus is — not what time it is. That is the whole of what
    // an operator can act on, and it is what the picture is actually doing.
    style("font-size: 18px; font-weight: 500; color: #e0a020;");
    m_behind->setText(tr_("Dock.BehindBy").arg(friendly_duration(m_posBehindS)));
    m_behind->setToolTip(QString());
}

long long DecoderDock::livePlayheadMs() const {
    if (!m_posAnimate) return m_mediaAnchorMs ? m_mediaAnchorMs : m_posBaseMs;
    // Media plays at 1x, so wall time since the segment was anchored is the
    // media time advanced into it. Capped at one segment, so a stalled refresh
    // cannot run the playhead past the segment it belongs to; bounded by the
    // span so a recording cannot be drawn past its own end.
    long long adv = (long long)QDateTime::currentMSecsSinceEpoch() - m_mediaAnchorWallMs;
    if (adv < 0) adv = 0;
    if (m_mediaSegMs > 0 && adv > m_mediaSegMs) adv = m_mediaSegMs;
    long long head = m_mediaAnchorMs + adv;
    if (m_posBoundMs > 0 && head > m_posBoundMs) head = m_posBoundMs;
    return head;
}

} // namespace multisite_obs
