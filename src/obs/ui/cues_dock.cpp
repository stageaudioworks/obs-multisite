// SPDX-License-Identifier: GPL-3.0-or-later
#include "cues_dock.h"

#include "../multisite_ui.h"

#include <obs-module.h>

#include <QComboBox>
#include <QDateTime>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <string>
#include <vector>

namespace multisite_obs {

static QString tr_(const char* key) {
    return QString::fromUtf8(obs_module_text(key));
}

// Elapsed time within an event — "4:05", or "1:24:15" for a long one. Distinct
// from a time of day, which is what a cue on a LIVE event carries.
static QString elapsed(long long ms) {
    if (ms < 0) ms = 0;
    const long long total = ms / 1000;
    const long long h = total / 3600;
    const long long m = (total % 3600) / 60;
    const long long s = total % 60;
    return h > 0
        ? QString("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0'))
              .arg(s, 2, 10, QChar('0'))
        : QString("%1:%2").arg(m).arg(s, 2, 10, QChar('0'));
}

CuesDock::CuesDock(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);

    auto* title = new QLabel(tr_("Dock.Cues"), this);
    QFont f = title->font();
    f.setBold(true);
    title->setFont(f);
    root->addWidget(title);

    auto* hint = new QLabel(tr_("Dock.CuesHint"), this);
    hint->setWordWrap(true);
    root->addWidget(hint);

    // The merged list, and a jump for whichever decoder is following this room.
    auto* row = new QHBoxLayout();
    m_cues = new QComboBox(this);
    m_cues->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_jump = new QPushButton(tr_("Dock.Jump"), this);
    row->addWidget(m_cues, 1);
    row->addWidget(m_jump);
    root->addLayout(row);

    // Drop a cue with any name. Custom on purpose: there is no fixed set of
    // moments a service has, and the operator knows what this one is.
    auto* dropRow = new QHBoxLayout();
    m_name = new QLineEdit(this);
    m_name->setPlaceholderText(tr_("Dock.CueName"));
    m_drop = new QPushButton(tr_("Dock.DropCue"), this);
    dropRow->addWidget(m_name, 1);
    dropRow->addWidget(m_drop);
    root->addLayout(dropRow);

    // One-press names: a button per cue name this event already has, so a
    // common cue is a tap and a new one is still typed above. This is what the
    // encoder's four configured marker buttons used to be — now the same
    // mechanism on both ends, named from the event rather than a settings field.
    m_quickWrap = new QWidget(this);
    m_quickRow = new QHBoxLayout(m_quickWrap);
    m_quickRow->setContentsMargins(0, 0, 0, 0);
    m_quickRow->setSpacing(4);
    m_quickWrap->setVisible(false);
    root->addWidget(m_quickWrap);

    m_note = new QLabel(this);
    m_note->setWordWrap(true);
    m_note->setStyleSheet("color: palette(text); opacity: 0.7;");
    root->addWidget(m_note);
    root->addStretch(1);

    // The same footer the other two docks carry, worded identically: the
    // project's name, where the manual is, and the version a bug report should
    // quote. No logo here for the same reason as those two — the mark is an
    // SVG and the docks do not link QtSvg.
    auto* version = new QLabel(QString("obs-multisite %1").arg(PLUGIN_VERSION), this);
    version->setStyleSheet("color: palette(text); opacity: 0.55;");
    root->addWidget(version);

    auto* brand = new QLabel(
        QStringLiteral(
            "obs-multisite &middot; "
            "<a href=\"https://stageaudioworks.github.io/obs-multisite/\">"
            "manual, downloads and source</a>"),
        this);
    brand->setOpenExternalLinks(true);
    brand->setWordWrap(true);
    root->addWidget(brand);

    connect(m_jump, &QPushButton::clicked, this, &CuesDock::onJump);
    connect(m_drop, &QPushButton::clicked, this, &CuesDock::onDrop);
    connect(m_name, &QLineEdit::returnPressed, this, &CuesDock::onDrop);

    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &CuesDock::refresh);
    m_timer->start(1000);
    refresh();
}

void CuesDock::refresh() {
    std::vector<CueEntry> cues;
    // Prefer a decoder's view: it is already the merged list from every site,
    // and jumping is a decoder action. A machine that is only encoding has no
    // decoder, so fall through to the encoder's own list — the same list, and
    // the same dock, on either end.
    const bool have_decoder = decoder_cues(cues);
    if (!have_decoder) encoder_cues(cues);

    // A recording's cue times run 00:00 to the end of the event; live they are
    // times of day. The snapshot is what says which this is.
    DecoderSnapshot snap;
    const bool have_snap = have_decoder && decoder_snapshot(snap);
    // NOTE: there was a `vod` flag here, written by hand as
    // `snap.ended || snap.interrupted` — a fourth copy of the question
    // DecoderSession::plays_as_recording() answers, and missing the pinned term
    // exactly as the other three were (BUGS D1). It is gone rather than fixed:
    // a cue's time now reads the same whether the event is live or recorded, so
    // nothing here needs to know.
    const long long started = have_snap ? snap.started_ms : 0;

    EncoderStats es;
    const bool encoding = encoder_stats(es);

    QString sig;
    for (const auto& c : cues)
        sig += QString::fromStdString(c.id) + "|" + QString::fromStdString(c.label) +
               "|" + QString::fromStdString(c.author) + "|" +
               QString::number((long long)c.at_media_ms) + "\n";
    if (sig != m_signature) {
        m_signature = sig;
        const QString keep = m_cues->currentData().toString();
        m_cues->clear();
        for (const auto& c : cues) {
            QString when;
            // How far into the programme, always — live or recorded. A cue
            // means "this moment in the service", and the media anchor says
            // that directly with nothing to convert and nothing to drift.
            if (c.at_media_ms >= 0) {
                when = elapsed(c.at_media_ms);
            } else if (c.at_ms > 0 && started > 0 && c.at_ms >= started) {
                // Older than at_media_ms: its time of day is all it carries.
                when = elapsed(c.at_ms - started);
            }
            QString text = when.isEmpty()
                ? QString::fromStdString(c.label)
                : when + "   " + QString::fromStdString(c.label);
            // Who set it, so a cue dropped at another campus is never mistaken
            // for the main site's.
            if (!c.author.empty())
                text += "   \u2014 " + QString::fromStdString(c.author);
            m_cues->addItem(text, QString::fromStdString(c.id));
        }
        const int idx = m_cues->findData(keep);
        if (idx >= 0) m_cues->setCurrentIndex(idx);
    }

    m_jump->setEnabled(have_decoder && m_cues->count() > 0);
    m_can_drop = encoding || have_decoder;
    m_drop->setEnabled(m_can_drop);

    // One button per distinct name this event already has, so the names a
    // service uses become the names it can drop in one press.
    {
        std::vector<QString> names;
        for (const auto& c : cues) {
            const QString label = QString::fromStdString(c.label);
            if (label.isEmpty()) continue;
            bool seen = false;
            for (const auto& n : names) if (n == label) { seen = true; break; }
            if (!seen) names.push_back(label);
            if (names.size() >= 6) break;
        }
        QString quickSig;
        for (const auto& n : names) quickSig += n + "\n";
        if (quickSig != m_quickSig) {
            m_quickSig = quickSig;
            while (QLayoutItem* it = m_quickRow->takeAt(0)) {
                if (QWidget* w = it->widget()) w->deleteLater();
                delete it;
            }
            for (const auto& n : names) {
                auto* b = new QPushButton(n, m_quickWrap);
                connect(b, &QPushButton::clicked, this,
                        [this, n] { dropNamed(n); });
                m_quickRow->addWidget(b);
            }
            m_quickWrap->setVisible(!names.empty());
        }
    }

    if (!m_error.isEmpty())
        m_note->setText(m_error);
    else if (!m_can_drop)
        m_note->setText(tr_("Dock.CuesNoEvent"));
    else if (!have_decoder)
        m_note->setText(tr_("Dock.CuesAuthorHere"));
    else
        m_note->clear();
}

void CuesDock::onDrop() {
    dropNamed(m_name->text());
}

void CuesDock::dropNamed(const QString& name) {
    QString label = name.trimmed();
    if (label.isEmpty()) label = tr_("Dock.CueDefault");

    // While this machine is encoding it OWNS the event, so the cue goes
    // straight into its own session. A satellite has no event of its own to
    // write, so it stamps and sends one under its site name.
    EncoderStats es;
    if (encoder_stats(es)) {
        forward_marker_to_encoder(label.toStdString());
        m_error.clear();
        m_name->clear();
    } else {
        std::string err;
        if (decoder_add_cue(label.toStdString(), err)) {
            m_error.clear();
            m_name->clear();
        } else {
            m_error = QString::fromStdString(err);
        }
    }
    refresh();
}

void CuesDock::onJump() {
    const QString id = m_cues->currentData().toString();
    if (id.isEmpty()) return;
    decoder_jump_to_marker(id.toStdString());
}

} // namespace multisite_obs
