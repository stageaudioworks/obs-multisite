// SPDX-License-Identifier: GPL-3.0-or-later
#include "decoder_dock.h"

#include "../multisite_ui.h"
#include "../decoder_settings.h"
#include "../plugin_log.h"
#include "../update_check.h"
#include "role_selector.h"
#include "status_text.h"
#include "web_box.h"

#include "../../core/position_interp.h"
#include "../../core/storage_providers.h"

#include <obs-module.h>

#include <QComboBox>
#include <QCheckBox>
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
// "24:15" or "1:24:15". Distinct from clock_time(), which is the time of day.
static QString position(long long ms) {
    if (ms < 0) ms = 0;
    const long long total = ms / 1000;
    const long long h = total / 3600, m = (total % 3600) / 60, s = total % 60;
    if (h > 0)
        return QString("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0'))
                                  .arg(s, 2, 10, QChar('0'));
    return QString("%1:%2").arg(m).arg(s, 2, 10, QChar('0'));
}

// Clock time of a position in the event, e.g. "10:42:06".
static QString clock_time(long long ms) {
    if (ms <= 0) return QString("--:--");
    return QDateTime::fromMSecsSinceEpoch((qint64)ms).toString("HH:mm:ss");
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
static QString axis_time(long long media_ms, long long origin, bool seconds) {
    if (origin > 0)
        return QDateTime::fromMSecsSinceEpoch((qint64)(origin + media_ms))
            .toString(seconds ? "HH:mm:ss" : "HH:mm");
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
        const QString label = axis_time(t, m_clock_origin, false);
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
        const QString label = axis_time(t, m_clock_origin, true);
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
    m_jumpMarker = new QPushButton(tr_("Dock.Jump"), this);
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
    m_endpoint  = new QLineEdit(storeBox);
    m_bucket    = new QLineEdit(storeBox);
    m_keyId     = new QLineEdit(storeBox);
    m_secret    = new QLineEdit(storeBox);
    m_secret->setEchoMode(QLineEdit::Password);
    m_region    = new QLineEdit(storeBox);
    m_roomId    = new QLineEdit(storeBox);
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
    m_lanPortField->setRange(1, 65535);
    m_lanToken = new QLineEdit(lanBox);
    m_lanToken->setToolTip(tr_("Dock.LanTokenHint"));
    lform->addRow(tr_("Dock.LanHost"), m_lanHost);
    lform->addRow(tr_("Dock.LanPort"), m_lanPortField);
    lform->addRow(tr_("Dock.LanToken"), m_lanToken);
    storePageLayout->addWidget(lanBox);

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
                          m_lanHost, m_lanToken })
        connect(e, &QLineEdit::textEdited, this, [this] { m_dirty = true; });
    for (QSpinBox* sb : { m_prebuffer, m_startBufferS, m_bufferMins,
                          m_pollMs, m_keepBehind, m_lanPortField })
        connect(sb, &QSpinBox::valueChanged, this,
                [this](int) { if (!m_loading) m_dirty = true; });
    connect(m_provider, &QComboBox::currentIndexChanged, this,
            [this](int) { if (!m_loading) m_dirty = true; });

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
    // Machine-wide, so read from its own store rather than from cfg — and read
    // here, on every open, so a change made in the other dock's dialog shows up.
    m_checkUpdates->setChecked(update_check_enabled());
    updateProviderFields();
    m_loading = false;
    m_dirty = false;
}

void DecoderDock::onApplySettings() {
    onSaveSettings();
    decoder_reconfigure_all();   // apply now, without closing the dialog
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
    DecoderSettings cfg = decoder_settings();
    // The provider decides how the fields the operator actually typed become
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
    set_decoder_settings(cfg);
    // Its own store, and committed with Apply like everything else on this
    // dialog — so opening the settings to look at something still changes
    // nothing, which is the promise the Apply button exists to make.
    update_check_set_enabled(m_checkUpdates->isChecked());
    m_dirty = false;
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
    // The bar hands back MEDIA time now; the session seeks by segment.
    if (m_mediaSegMs <= 0) return;
    long long seq = media_ms / m_mediaSegMs;
    if (seq < 0) seq = 0;
    decoder_seek((unsigned long long)seq);
}

// ── Recordings ───────────────────────────────────────────────────────────────
void DecoderDock::onRefreshEvents() { decoder_refresh_events(); }

void DecoderDock::onLoadEvent() {
    if (!m_events) return;
    auto* item = m_events->currentItem();
    if (!item) return;
    const QString id = item->data(Qt::UserRole).toString();
    if (id.isEmpty()) return;
    // Loading buffers; it does not go to air. Play is a separate, deliberate
    // press — the same rule as loading the live feed.
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
    m_play->setEnabled(ctl && !s.playing);
    m_stop->setEnabled(ctl && s.playing);
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
    // Nothing is on air yet, so the only thing an operator can act on is how
    // much of the buffer is actually down against the gate that Play is waiting
    // for. Rounded to seconds: the raw double flickers, and nobody reads past
    // the whole second.
    const int bufGate = s.start_buffer_s;
    const int bufHave = (int)(s.buffered_span_s + 0.5);

    if (s.stopped) {
        m_playback->setText(tr_("Dock.Pb.Stopped"));
        m_playback->setStyleSheet("color: #8b9198; font-weight: bold;");
    } else if (s.loading) {
        m_playback->setText(tr_("Dock.Loading"));
        m_playback->setStyleSheet("color: #3b82c4; font-weight: bold;");
    } else if (s.seek_target_ms > 0 || s.buffering) {
        m_playback->setText(bufGate > 0
                                ? tr_("Dock.BufferingProgress").arg(bufHave).arg(bufGate)
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
    } else {
        // Configured and buffering ahead, but not on air: what Load leaves
        // behind, waiting for Play on cue. Say how far the buffer has actually
        // got — "Ready" on its own is indistinguishable from a link that has
        // stalled, and the operator's next move (wait, or press Play anyway)
        // depends on the difference.
        if (bufGate > 0 && bufHave < bufGate) {
            m_playback->setText(tr_("Dock.FillingBuffer").arg(bufHave).arg(bufGate));
            m_playback->setStyleSheet("color: #3b82c4; font-weight: bold;");
        } else {
            m_playback->setText(tr_("Dock.Pb.Ready"));
            m_playback->setStyleSheet("color: #8b9198; font-weight: bold;");
        }
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
    const double seg_s = s.segment_duration_s > 0.1 ? s.segment_duration_s : 6.0;
    m_mediaSegMs = (long long)(seg_s * 1000.0 + 0.5);
    m_posClockOriginMs = (long long)s.started_ms;
    const auto media = [this](unsigned long long seq) {
        return (long long)seq * m_mediaSegMs;
    };

    m_posValid    = true;
    m_posFixed    = true;
    m_posAnimate  = false;
    m_posVod      = false;
    m_posBoundMs  = 0;
    m_posTooltip  = QString();
    m_posBaseMs     = media(s.playhead_seq);
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
        m_posText  = tr_("Dock.GoingTo").arg(clock_time(s.seek_target_ms));
        m_posStyle = "font-size: 18px; font-weight: 500; color: #3b82c4;";
    } else if (s.loading) {
        m_posText  = tr_("Dock.LoadingRecording");
        m_posStyle = "font-size: 18px; font-weight: 500; color: #3b82c4;";
    } else if (!s.playing && bufGate > 0 && bufHave < bufGate) {
        // Load pressed, buffer still filling, nothing on air yet. This is the
        // largest text on the dock, so the progress belongs here as much as in
        // the state line — it is what the operator is watching while they wait.
        m_posText  = tr_("Dock.FillingBuffer").arg(bufHave).arg(bufGate);
        m_posStyle = "font-size: 18px; font-weight: 500; color: #3b82c4;";
    } else if (s.ended) {
        // A finished recording: show where you are in it and how much is left.
        // How far through, out of its total length — the way a media player
        // reads. "Behind live" means nothing once the event has finished, and
        // the total length is what an operator actually wants when deciding
        // whether it will fit the slot.
        m_posFixed     = false;
        m_posVod       = true;
        m_posAtEnd     = s.at_end;
        m_posStartedMs = 0;                       // elapsed, from the start
        m_posTotalMs   = media(s.live_edge + 1);  // the whole recording
        // A recording cannot play past its own end, so interpolation must not
        // walk past it either.
        if (s.live_edge > 0) m_posBoundMs = m_posTotalMs;
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
        if (s.live_edge > 0) m_posBoundMs = media(s.live_edge + 1);
        m_posAnimate = s.playing && !s.paused && !s.buffering;
    }

    // Timeline entirely in clock time now, with the real downloaded ranges.
    // A finished recording spans its WHOLE length — from the moment it started
    // to its true end — so the bar stops growing and the scale means something.
    // While live, the right edge is the live edge and the bar necessarily grows
    // with it.
    if (s.ended && s.end_ms > 0) {
        // A recording spans its whole length, labelled as elapsed time.
        m_timeline->setSpan(0, media(s.live_edge + 1));
        m_timeline->setClockOrigin(0);
    } else {
        // Live: the left edge is what storage still holds, and the labels read
        // as times of day.
        m_timeline->setSpan(media(s.first_available), media(s.live_edge + 1));
        m_timeline->setClockOrigin(m_posClockOriginMs);
    }
    // Which of the ordinary "nothing to draw yet" states this is. Only used
    // when the span is empty; the bar ignores it otherwise.
    m_timeline->setPlaceholder(
        s.event_id.empty()
            ? (s.room_state == 1 /* offline */ ? tr_("Dock.TimelineOffline")
                                               : tr_("Dock.TimelineNothing"))
            : tr_("Dock.TimelineWaiting"));
    {
        std::vector<std::pair<long long, long long>> dl;
        for (const auto& r : s.cached_seq_spans)
            dl.emplace_back(media(r.first), media(r.second + 1));
        m_timeline->setDownloaded(std::move(dl));
    }
    {
        std::vector<long long> mt;
        for (const auto& m : s.markers) mt.push_back(media(m.seq));
        m_timeline->setMarkers(std::move(mt));
    }
    // Confirm the click on the bar itself, not only in the position line above
    // it. The target is a wall time; the bar's axis is media time, and the two
    // differ by the event's own start.
    m_timeline->setPending((s.seek_target_ms > 0 && s.started_ms > 0)
                               ? (long long)(s.seek_target_ms - s.started_ms)
                               : -1);

    // Rebuild the marker list only when it changes, so the combo doesn't
    // reset while an operator is using it.
    if (s.markers.size() != m_marker_count) {
        m_marker_count = s.markers.size();
        const QString keep = m_markers->currentData().toString();
        m_markers->clear();
        for (const auto& m : s.markers) {
            // A recording's cue times run 00:00 to the end of the event; live
            // they are times of day. Same rule the playhead readout follows —
            // "behind live" means nothing once there is no live edge.
            const bool vod = s.ended || s.interrupted;
            QString when;
            if (m.at_ms > 0) {
                when = (vod && s.started_ms > 0 && m.at_ms >= s.started_ms)
                         ? position(m.at_ms - s.started_ms)
                         : clock_time(m.at_ms);
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

    // A real error wins the red line, but a clock far from the store's earns
    // the same attention: it is what puts this site's cue times out of step.
    QString warn = QString::fromStdString(s.last_error);
    const long long skew = s.clock_skew_ms;
    if (warn.isEmpty() && (skew >= 5000 || skew <= -5000))
        warn = tr_("Dock.ClockOut") + " (" + QString::number(skew / 1000) + " s)";
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
        // The clock time of the recorded moment stays available, just smaller.
        m_behind->setToolTip(tr_("Dock.Showing").arg(clock_time(m_posClockOriginMs + head)));
        return;
    }

    // Behind live. Only the clock time advances between samples; how far behind
    // we are does not, because the live edge is moving at the same rate.
    style("font-size: 18px; font-weight: 500; color: #e0a020;");
    m_behind->setText(tr_("Dock.Showing").arg(clock_time(m_posClockOriginMs + head))
                      + "  —  "
                      + tr_("Dock.BehindBy").arg(friendly_duration(m_posBehindS)));
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
