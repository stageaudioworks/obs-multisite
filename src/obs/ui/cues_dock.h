// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// cues_dock.h — the Cues dock: one list and one place to drop a cue, whatever
// role this machine is. The point is commonality: a cue looks and behaves the
// same whether the main site or a campus set it, and whichever end you are
// sitting at you see the same merged list. See PROJECT-SCOPE.md §7.
//
#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QHBoxLayout;
class QTimer;

namespace multisite_obs {

class CuesDock : public QWidget {
    Q_OBJECT
public:
    explicit CuesDock(QWidget* parent = nullptr);

private slots:
    void refresh();
    void onDrop();
    void onJump();

private:
    // Drop a cue with this name. The free-text field and the one-press buttons
    // are the same action, so it lives in one place.
    void dropNamed(const QString& name);

    QComboBox*   m_cues = nullptr;
    QPushButton* m_jump = nullptr;
    QLineEdit*   m_name = nullptr;
    QPushButton* m_drop = nullptr;
    // One button per cue name this event already has, so the one-press speed
    // the old encoder marker buttons gave is kept now that they are gone.
    QWidget*     m_quickWrap = nullptr;
    QHBoxLayout* m_quickRow = nullptr;
    QLabel*      m_note = nullptr;
    QTimer*      m_timer = nullptr;

    QString m_signature;      // the list as last drawn, so it is only rebuilt on change
    QString m_quickSig;       // …and the same for the one-press buttons
    QString m_error;          // last refusal, kept until a drop succeeds
    bool    m_can_drop = false;
};

} // namespace multisite_obs
