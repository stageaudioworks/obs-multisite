// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// settings_dialog.h — a settings dialog that asks before throwing edits away.
//
// Closing the settings is deliberately NOT a commit: Apply is, so that looking
// at the settings mid-event changes nothing. The other half of that promise is
// this — a close with edits that were never applied must not silently discard
// them. Both the Close button and the window's own X/Escape come through here,
// and the operator is offered Apply, Discard or keep editing.
//
// Header-only and with no Q_OBJECT, so it needs no moc and no CMake entry.
//
#include <obs-module.h>

#include <QAbstractButton>
#include <QCloseEvent>
#include <QDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QShowEvent>

#include <functional>

namespace multisite_obs {

class SettingsDialog : public QDialog {
public:
    using QDialog::QDialog;

    // Supplied by the dock: only it knows what "changed" means for its fields.
    std::function<bool()> is_dirty;       // true when there are unapplied edits
    std::function<void()> apply_changes;  // what Apply does

protected:
    void reject() override {
        if (!confirm_close()) return;     // the operator said "keep editing"
        QDialog::reject();
    }
    void closeEvent(QCloseEvent* e) override {
        if (!confirm_close()) { e->ignore(); return; }
        // QDialog::reject() rather than QDialog::closeEvent(): the base
        // closeEvent would call the virtual reject() again and ask twice.
        QDialog::reject();
    }
    void showEvent(QShowEvent* e) override {
        m_confirmed = false;              // a fresh showing asks afresh
        QDialog::showEvent(e);
    }

private:
    bool m_confirmed = false;

    bool confirm_close() {
        if (m_confirmed) return true;
        if (!is_dirty || !is_dirty()) return true;

        QMessageBox box(this);
        box.setIcon(QMessageBox::Question);
        box.setWindowTitle(QString::fromUtf8(obs_module_text("Dock.UnsavedTitle")));
        box.setText(QString::fromUtf8(obs_module_text("Dock.UnsavedText")));
        box.setInformativeText(QString::fromUtf8(obs_module_text("Dock.UnsavedInfo")));
        auto* apply = box.addButton(QString::fromUtf8(obs_module_text("Dock.Apply")),
                                    QMessageBox::AcceptRole);
        auto* discard = box.addButton(QString::fromUtf8(obs_module_text("Dock.Discard")),
                                      QMessageBox::DestructiveRole);
        box.addButton(QString::fromUtf8(obs_module_text("Dock.KeepEditing")),
                      QMessageBox::RejectRole);
        box.setDefaultButton(apply);
        box.exec();

        if (box.clickedButton() == apply) {
            if (apply_changes) apply_changes();
            m_confirmed = true;
            return true;
        }
        if (box.clickedButton() == discard) {
            m_confirmed = true;
            return true;
        }
        return false;   // keep editing
    }
};

} // namespace multisite_obs
