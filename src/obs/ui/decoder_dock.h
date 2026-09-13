// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// decoder_dock.h — the satellite operator panel.
//
// The DVR surface from the design: room state, how far behind live, a timeline
// showing the retained window / buffered content / playhead / markers, and
// Pause / Resume / Jump-to-live.
//
#include <QWidget>
#include <string>
#include <utility>
#include <vector>

class QLabel;
class QPushButton;
class QComboBox;
class QTimer;
class QLineEdit;
class QSpinBox;
class QDialog;
class QListWidget;

namespace multisite_obs {

struct DecoderSnapshot;

// Timeline strip: retained window, buffered region, playhead, live edge and
// marker ticks. Clicking seeks.
// Timeline in CLOCK TIME. Shows the recording that still exists in storage,
// which parts are downloaded here, where the playhead is, the live edge, and
// marker ticks. Hovering reports the recorded time under the cursor; clicking
// goes there.
class TimelineBar : public QWidget {
    Q_OBJECT
public:
    explicit TimelineBar(QWidget* parent = nullptr);

    void setSpan(long long earliest_ms, long long live_ms);
    void setPlayhead(long long ms);
    void setDownloaded(std::vector<std::pair<long long, long long>> spans);
    void setMarkers(std::vector<long long> times_ms);
    // Why the bar has nothing to show, when it has nothing to show. An empty
    // trough looked identical whether no source existed, nothing had been
    // loaded yet, or something had genuinely failed — which is exactly how a
    // normal startup state came to be reported as the timeline being broken.
    void setPlaceholder(const QString& why);

    QSize minimumSizeHint() const override;

signals:
    void seekRequested(long long wall_ms);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    double fraction(long long ms) const;
    long long timeAt(int x) const;

    long long m_earliest = 0, m_live = 0, m_head = 0;
    QString   m_placeholder;
    std::vector<std::pair<long long, long long>> m_downloaded;
    std::vector<long long> m_markers;
    int  m_hoverX = -1;
};

class DecoderDock : public QWidget {
    Q_OBJECT
public:
    explicit DecoderDock(QWidget* parent = nullptr);

private slots:
    void refresh();
    // Paints the position readout and the playhead. The ONLY writer of either,
    // called both by refresh() with a fresh sample and by the smooth timer in
    // between — see the note on the position model below for why that matters.
    void paintPosition();
    void onStart();
    void onSaveSettings();
    void onOpenSettings();
    // Shows only the storage fields the selected provider actually needs
    // (PROJECT-SCOPE.md §8.6) — the same dropdown as the encoder dock.
    void updateProviderFields();
    void onPause();
    void onResume();
    void onJumpLive();
    void onJumpMarker();
    void onSeek(long long wall_ms);
    void onPlay();
    void onStop();
    void onLockToggled(bool);
    void onJog(double seconds);
    void onGoToDelay();
    void onRefreshEvents();
    void onLoadEvent();
    void onReturnToLive();
    void onEventActivated();

private:
    // ── Position model ───────────────────────────────────────────────────────
    // refresh() runs twice a second, which is visibly steppy on a scrub bar
    // next to something that plays video for a living. So the playhead is
    // interpolated between samples — but the FIRST attempt at that shipped a
    // frozen timer and an apparently dead Play button, and the shape of the
    // fix is the important part.
    //
    // That version had two timers writing m_behind and the playhead directly:
    // refresh() every 500ms and a smooth tick every 100ms. The fast one always
    // had the last word, so a single bad interpolation input overwrote the
    // correct value five times per refresh and the display stuck there
    // permanently — with the backend healthy and refresh() running the whole
    // time, which is exactly why it was so hard to find. Play was working too;
    // it just had no way to show it.
    //
    // Hence one writer. refresh() only ever updates this model; paintPosition()
    // is the only thing that touches the widgets. A bad input can now produce
    // at most one wrong frame, which the next refresh corrects, instead of a
    // display no amount of correct state can reach.
    //
    // The second safeguard is that extrapolation is bounded relative to the
    // last authoritative sample rather than to some absolute end-of-recording
    // value. If refresh() ever stops, the readout settles a fraction of a
    // second past the last real number instead of being pinned to a clamp
    // computed from something else entirely — which is what the old
    // upper-bound clamp did.
    bool      m_posValid   = false;   // false: no source, show a dash
    bool      m_posFixed   = true;    // true: m_posText is the whole answer
    bool      m_posAnimate = false;   // does the playhead move between samples?
    long long m_posBaseMs     = 0;    // authoritative playhead…
    long long m_posBaseWallMs = 0;    // …and the wall-clock time it was taken
    long long m_posBoundMs    = 0;    // never interpolate past this; 0 = none
    QString   m_posText, m_posStyle, m_posTooltip;
    // Last stylesheet actually applied. setStyleSheet re-parses on every
    // call and does not no-op on an unchanged value, unlike setText.
    QString   m_posAppliedStyle;
    // Animated variants need these to re-render their text each tick.
    bool      m_posVod   = false;     // finished recording: elapsed / total
    bool      m_posAtEnd = false;
    long long m_posStartedMs = 0, m_posTotalMs = 0;
    double    m_posBehindS = 0;

    // The interpolated playhead right now, or the plain sample when not
    // animating. Clamped both ways — see the note above.
    long long livePlayheadMs() const;

    // Rebuilds the recordings list from the current listing. Separate from
    // refresh() only because that function is already long.
    void refreshEvents(const DecoderSnapshot& s);

    QLabel* m_room = nullptr;
    // What the main site is doing (LIVE, BROADCAST ENDED, OFFLINE…).
    QLabel* m_state = nullptr;
    // What this box is doing with it (PLAYING, HELD, STOPPED, LOADING…).
    QLabel* m_playback = nullptr;
    QLabel* m_behind = nullptr;
    QLabel* m_buffered = nullptr;
    QLabel* m_cached = nullptr;
    QLabel* m_marker = nullptr;
    QLabel* m_audio = nullptr;
    QLabel* m_net = nullptr;         // internet/connection health, coloured
    QLabel* m_error = nullptr;
    QLabel* m_storage = nullptr;   // colo + observed download rate
    QLabel* m_version = nullptr;
    TimelineBar* m_timeline = nullptr;
    QPushButton* m_pause = nullptr;
    QPushButton* m_resume = nullptr;
    QPushButton* m_live = nullptr;
    QPushButton* m_start = nullptr;
    QPushButton* m_play = nullptr;
    QPushButton* m_stop = nullptr;
    QPushButton* m_lock = nullptr;
    QSpinBox*    m_delayMins = nullptr;
    QPushButton* m_goTo = nullptr;
    QLabel*      m_hint = nullptr;
    QComboBox* m_markers = nullptr;
    QPushButton* m_jumpMarker = nullptr;

    // ── Recordings ───────────────────────────────────────────────────────────
    // The list is rebuilt only when its contents actually change: it refreshes
    // twice a second alongside everything else, and replacing the rows every
    // tick would discard the operator's selection as they reached for Load.
    QListWidget* m_events = nullptr;
    QPushButton* m_refreshEvents = nullptr;
    QPushButton* m_loadEvent = nullptr;
    QPushButton* m_returnLive = nullptr;
    QLabel*      m_eventsNote = nullptr;
    QLabel*      m_liveElsewhere = nullptr;
    QString      m_events_signature;
    QTimer* m_timer = nullptr;
    // Repaints between state refreshes so the playhead glides rather than
    // stepping twice a second. Paints only; it never samples state.
    QTimer* m_smoothTimer = nullptr;
    size_t m_marker_count = 0;

    // Machine-wide storage settings, entered once here.
    QComboBox* m_provider = nullptr;
    QLineEdit* m_accountId = nullptr;
    QLineEdit* m_endpoint = nullptr;
    QLineEdit* m_bucket = nullptr;
    QLineEdit* m_keyId = nullptr;
    QLineEdit* m_secret = nullptr;
    QLineEdit* m_region = nullptr;
    QLineEdit* m_roomId = nullptr;
    QSpinBox*  m_prebuffer = nullptr;
    QSpinBox*  m_startBufferS = nullptr;
    QSpinBox*  m_bufferMins = nullptr;
    // In a dialog rather than the dock, for the same reason as the encoder:
    // settings are set once, the dock is watched mid-event.
    QDialog* m_settings = nullptr;
    QPushButton* m_settingsBtn = nullptr;
};

} // namespace multisite_obs
