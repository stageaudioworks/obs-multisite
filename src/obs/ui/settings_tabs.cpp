// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings_tabs.h"

#include <QFormLayout>
#include <QFrame>
#include <QLayout>
#include <QScrollArea>
#include <QTabWidget>
#include <QWidget>

namespace multisite_obs {

void tighten(QWidget* w) {
    if (!w) return;
    QLayout* l = w->layout();
    if (!l) return;

    // A group box inside a tab inside a dialog already contributes three sets
    // of margins before anything is drawn. The outermost of those is the one
    // worth removing: the tab and the scroll area supply their own inset, so a
    // second one inside every page is pure emptiness.
    l->setContentsMargins(8, 8, 8, 8);
    l->setSpacing(6);

    if (auto* form = qobject_cast<QFormLayout*>(l)) {
        form->setHorizontalSpacing(10);
        form->setVerticalSpacing(5);
        // Labels beside their fields rather than above, and fields that do not
        // stretch to the full width of whatever the widest row happens to be.
        // Qt's default on macOS wraps long labels onto their own line, which is
        // where most of the height was going.
        form->setRowWrapPolicy(QFormLayout::DontWrapRows);
        form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    }

    // Nested boxes get the same treatment: the role selector and the remote
    // control box each build their own layout and would otherwise keep Qt's
    // defaults while everything around them tightened.
    for (QObject* child : w->children())
        if (auto* cw = qobject_cast<QWidget*>(child))
            if (cw->layout()) tighten(cw);
}

void add_settings_tab(QTabWidget* tabs, QWidget* content, const QString& title) {
    if (!tabs || !content) return;
    tighten(content);

    auto* scroll = new ShrinkableScrollArea(tabs);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    // Horizontal scrolling would mean the page is too narrow, which is a layout
    // fault to fix rather than something to make an operator pan around.
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(content);
    tabs->addTab(scroll, title);
}

} // namespace multisite_obs
