// SPDX-License-Identifier: GPL-3.0-or-later
#include "secondary_box.h"

#include "../../core/storage_providers.h"
#include "../storage_secondary.h"

#include <obs-module.h>

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QStandardItemModel>

namespace multisite_obs {

static QString tr_(const char* key) {
    return QString::fromUtf8(obs_module_text(key));
}

SecondaryTargetBox::SecondaryTargetBox(bool main_site, QWidget* parent)
    : QGroupBox(tr_("Dock.SecondBucket"), parent), m_main_site(main_site) {
    auto* form = new QFormLayout(this);

    m_enabled = new QCheckBox(m_main_site ? tr_("Dock.SecondBucketEnableMain")
                                          : tr_("Dock.SecondBucketEnableSat"),
                              this);
    m_enabled->setToolTip(m_main_site ? tr_("Dock.SecondBucketHintMain")
                                      : tr_("Dock.SecondBucketHintSat"));
    form->addRow(m_enabled);

    m_provider = new QComboBox(this);
    m_provider->setToolTip(tr_("StorageProviderHint"));
    for (const auto& info : multisite::all_providers()) {
        m_provider->addItem(QString::fromStdString(info.display_name),
                            QString::fromStdString(info.key));
        if (!info.available) {
            // Same reason as the primary: listed so an operator knows it is
            // coming, not selectable yet (PROJECT-SCOPE.md §8.5).
            if (auto* model = qobject_cast<QStandardItemModel*>(m_provider->model()))
                if (auto* item = model->item(m_provider->count() - 1))
                    item->setEnabled(false);
        }
    }
    m_accountId = new QLineEdit(this);
    m_accountId->setToolTip(tr_("R2AccountIDHint"));
    m_endpoint  = new QLineEdit(this);
    m_endpoint->setToolTip(tr_("EndpointHostHint"));
    m_bucket    = new QLineEdit(this);
    m_bucket->setToolTip(tr_("BucketHint"));
    m_keyId     = new QLineEdit(this);
    m_keyId->setToolTip(tr_("AccessKeyIDHint"));
    m_secret    = new QLineEdit(this);
    m_secret->setEchoMode(QLineEdit::Password);
    m_secret->setToolTip(tr_("SecretKeyHint"));
    m_region    = new QLineEdit(this);
    m_region->setToolTip(tr_("RegionHint"));

    form->addRow(tr_("Dock.StorageProvider"), m_provider);
    form->addRow(tr_("R2AccountID"), m_accountId);
    form->addRow(tr_("EndpointHost"), m_endpoint);
    form->addRow(tr_("Bucket"), m_bucket);
    form->addRow(tr_("AccessKeyID"), m_keyId);
    form->addRow(tr_("SecretKey"), m_secret);
    form->addRow(tr_("Region"), m_region);

    connect(m_enabled, &QCheckBox::toggled, this, [this] {
        updateRows();
        if (!m_loading) emit changed();
    });
    connect(m_provider, &QComboBox::currentIndexChanged, this, [this] {
        updateRows();
        if (!m_loading) emit changed();
    });
    for (QLineEdit* e : { m_accountId, m_endpoint, m_bucket, m_keyId, m_secret, m_region })
        connect(e, &QLineEdit::textEdited, this,
                [this] { if (!m_loading) emit changed(); });

    updateRows();
}

// Which rows are visible, decided in ONE place from BOTH conditions.
//
// HIDDEN, not merely greyed out. An operator who is not using a second bucket
// should not have to look past seven empty credential fields to find the rest
// of the storage settings; ticking the box is what asks for them.
//
// This used to be two functions, and they fought. `updateEnabled` hid all seven
// rows when the box was unticked, and `updateProviderFields` then re-showed
// account id, endpoint and region if the provider wanted them — without
// consulting the tick at all. The constructor ran them in that order, so a
// fresh dock opened with the block half exposed: three credential fields
// visible through a box that was supposed to be shut. The `toggled` handler
// called only the first, so ticking and unticking let the hide win uncontested
// and the block finally closed properly. The correct state was reachable only
// by toggling, which is exactly how an operator reported it.
//
// A row's visibility depends on two facts, so one function reads both. Do not
// split this again: the second pass will not know about the first.
void SecondaryTargetBox::updateRows() {
    auto* form = qobject_cast<QFormLayout*>(layout());
    if (!form) return;

    const bool on = m_enabled->isChecked();
    const auto& info = multisite::provider_info(multisite::provider_from_key(
        m_provider->currentData().toString().toStdString()));

    form->setRowVisible(m_provider,  on);
    form->setRowVisible(m_bucket,    on);
    form->setRowVisible(m_keyId,     on);
    form->setRowVisible(m_secret,    on);
    form->setRowVisible(m_accountId, on && info.needs_account_id);
    form->setRowVisible(m_endpoint,  on && info.needs_endpoint);
    form->setRowVisible(m_region,    on && info.needs_region);
}

void SecondaryTargetBox::loadFromStore() {
    m_loading = true;
    const SecondaryTarget t = secondary_target();

    m_enabled->setChecked(t.enabled);
    auto provider = multisite::provider_from_key(t.storage_provider);
    const int idx = m_provider->findData(
        QString::fromStdString(multisite::provider_key(provider)));
    m_provider->setCurrentIndex(idx >= 0 ? idx : 0);
    m_accountId->setText(QString::fromStdString(t.r2_account_id));
    m_endpoint->setText(QString::fromStdString(t.endpoint_host));
    m_bucket->setText(QString::fromStdString(t.bucket));
    m_keyId->setText(QString::fromStdString(t.access_key_id));
    m_secret->setText(QString::fromStdString(t.secret_access_key));
    m_region->setText(QString::fromStdString(t.region));

    updateRows();
    m_loading = false;
}

void SecondaryTargetBox::saveToStore() {
    SecondaryTarget t;
    t.enabled = m_enabled->isChecked();

    // The same normalisation the primary's fields get, from the same helper:
    // what an operator types is not always the shape the raw config holds.
    auto provider = multisite::provider_from_key(
        m_provider->currentData().toString().toStdString());
    t.storage_provider = multisite::provider_key(provider);
    if (provider == multisite::StorageProvider::Custom) {
        t.r2_account_id = "";
        t.endpoint_host = m_endpoint->text().trimmed().toStdString();
        t.region        = m_region->text().trimmed().toStdString();
    } else {
        const std::string input =
            provider == multisite::StorageProvider::CloudflareR2
                ? m_accountId->text().trimmed().toStdString()
                : m_region->text().trimmed().toStdString();
        auto derived = multisite::derive(provider, input);
        t.r2_account_id = derived.r2_account_id;
        t.endpoint_host = derived.endpoint_host;
        t.region        = derived.region;
    }
    t.bucket            = m_bucket->text().trimmed().toStdString();
    t.access_key_id     = m_keyId->text().trimmed().toStdString();
    t.secret_access_key = m_secret->text().trimmed().toStdString();

    set_secondary_target(t);
}

} // namespace multisite_obs
