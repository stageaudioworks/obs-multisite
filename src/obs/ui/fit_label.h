// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// fit_label.h — a one-line value that fits the width it is given, and never
// sets the width of the dock around it.
//
// Every value cell in the docks' Status grids is one of these. A plain QLabel
// will not shrink below its whole text, so one long value — an AES67 track
// named after its sending machine — held the whole decoder dock wide (see
// fit_text.h for the incident). This one:
//   - asks for only a few characters as its MINIMUM, so no value can widen the
//     dock;
//   - PREFERS its whole text up to a modest width, so a dock laid out fresh is
//     not wide either;
//   - SHOWS as much as it has room for, cut in the middle (fit_middle), redone
//     on every resize — the whole value whenever the dock is wide enough;
//   - puts the whole value in its tooltip whenever it is cut, above any hint
//     the dock set for the cell.
//
// setText() and setToolTip() hide QLabel's own, so a dock's existing calls keep
// working once the member is declared as a FitLabel. Plain text only — the
// cells colour themselves by stylesheet, never with markup, and a cut through
// a tag would break it.

#include "fit_text.h"

#include <QEvent>
#include <QFontMetrics>
#include <QLabel>
#include <QResizeEvent>

namespace multisite_ui {

class FitLabel : public QLabel {
public:
    explicit FitLabel(const QString& text, QWidget* parent = nullptr)
        : QLabel(parent), m_full(text) {
        setTextFormat(Qt::PlainText);
        refit();
    }

    void setText(const QString& text) {
        if (text == m_full) return;
        m_full = text;
        refit();
        updateGeometry();   // the preferred width follows the text
    }
    // A hint for the cell, shown in its tooltip — below the whole value when
    // the value is cut, on its own otherwise.
    void setToolTip(const QString& hint) {
        if (hint == m_hint) return;
        m_hint = hint;
        apply_tooltip();
    }
    const QString& fullText() const { return m_full; }

    QSize minimumSizeHint() const override {
        const QFontMetrics fm(font());
        const QMargins m = contentsMargins();
        return QSize(fm.horizontalAdvance(QStringLiteral("0000…")) + m.left() + m.right(),
                     QLabel::minimumSizeHint().height());
    }
    QSize sizeHint() const override {
        const QFontMetrics fm(font());
        const QMargins m = contentsMargins();
        const int want = fm.horizontalAdvance(m_full);
        return QSize((want < kPreferredPx ? want : kPreferredPx) + m.left() + m.right(),
                     QLabel::sizeHint().height());
    }

protected:
    void resizeEvent(QResizeEvent* e) override {
        QLabel::resizeEvent(e);
        refit();
    }
    void changeEvent(QEvent* e) override {
        QLabel::changeEvent(e);
        if (e->type() == QEvent::FontChange || e->type() == QEvent::StyleChange)
            refit();
    }

private:
    // What a layout is asked for when there is room: the width set_value() used
    // to cut to, so a fresh dock looks as it did — but a MINIMUM no longer.
    static constexpr int kPreferredPx = 240;

    void refit() {
        const QFontMetrics fm(font());
        const int avail = contentsRect().width();
        const std::string shown = fit_middle(
            m_full.toStdString(), avail,
            [&fm](const std::string& s) {
                return fm.horizontalAdvance(QString::fromStdString(s));
            });
        const QString q = QString::fromStdString(shown);
        m_cut = (q != m_full);
        if (QLabel::text() != q) QLabel::setText(q);
        apply_tooltip();
    }
    void apply_tooltip() {
        QString tip = m_hint;
        if (m_cut) tip = m_hint.isEmpty() ? m_full : m_full + "\n\n" + m_hint;
        if (toolTip() != tip) QLabel::setToolTip(tip);
    }

    QString m_full;
    QString m_hint;
    bool    m_cut = false;
};

} // namespace multisite_ui
