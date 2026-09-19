// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// secondary_box.h — the second-bucket fields, shared by both docks.
//
// One widget rather than two copies: the fields are identical on either side
// (the encoder mirrors to it, the decoder falls back to it) and two copies of a
// credential form is exactly the drift this project keeps designing out.
//
// It commits to the machine-wide store itself — see storage_secondary.h for why
// that is not per role — so a dock only has to call loadFromStore() when its
// settings open and saveToStore() when Apply is pressed, alongside its own
// fields.
//
#include <QGroupBox>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QLabel;
class QPushButton;

namespace multisite_obs {

class SecondaryTargetBox : public QGroupBox {
    Q_OBJECT
public:
    // `main_site` changes what the box SAYS, because the same bucket means two
    // different things: a main site writes a copy to it, a campus reads from it
    // when the primary cannot be reached. That is now all it changes — the
    // upload test this box used to carry moved to the encoder dock's storage
    // page, since it measures the link rather than this bucket.
    explicit SecondaryTargetBox(bool main_site, QWidget* parent = nullptr);

    void loadFromStore();
    void saveToStore();

signals:
    // An operator edited something. The dock uses this for its unsaved-changes
    // prompt, exactly as it does for its own fields.
    void changed();

private:
    // ONE authority for which rows are visible. It used to be two — an
    // "is the box ticked" pass and a "does this provider want this field"
    // pass — which fought over the same three rows and left the block half
    // open until something toggled it. See the comment on the definition.
    void updateRows();

    QCheckBox* m_enabled   = nullptr;
    QComboBox* m_provider  = nullptr;
    QLineEdit* m_accountId = nullptr;
    QLineEdit* m_endpoint  = nullptr;
    QLineEdit* m_bucket    = nullptr;
    QLineEdit* m_keyId     = nullptr;
    QLineEdit* m_secret    = nullptr;
    QLineEdit* m_region    = nullptr;
    // loadFromStore() sets fields programmatically; those setText calls must
    // not read as the operator editing something.
    bool m_loading = false;
    bool m_main_site = false;
};

} // namespace multisite_obs
