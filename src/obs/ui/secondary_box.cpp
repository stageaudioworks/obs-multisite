// SPDX-License-Identifier: GPL-3.0-or-later
#include "secondary_box.h"

#include "../../core/storage_providers.h"
#include "../storage_secondary.h"

#include <obs-module.h>

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QStandardItemModel>

namespace multisite_obs {

static QString tr_(const char* key) {
    return QString::fromUtf8(obs_module_text(key));
}

SecondaryTargetBox::SecondaryTargetBox(QWidget* parent)
    : QGroupBox(tr_("Dock.SecondBucket"), parent) {
    auto* form = new QFormLayout(this);

    m_enabled = new QCheckBox(tr_("Dock.SecondBucketEnable"), this);
    m_enabled->setToolTip(tr_("Dock.SecondBucketHint"));
    form->addRow(m_enabled);

    m_provider = new QComboBox(this);
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
    m_endpoint  = new QLineEdit(this);
    m_bucket    = new QLineEdit(this);
    m_keyId     = new QLineEdit(this);
    m_secret    = new QLineEdit(this);
    m_secret->setEchoMode(QLineEdit::Password);
    m_region    = new QLineEdit(this);

    form->addRow(tr_("Dock.StorageProvider"), m_provider);
    form->addRow(tr_("R2AccountID"), m_accountId);
    form->addRow(tr_("EndpointHost"), m_endpoint);
    form->addRow(tr_("Bucket"), m_bucket);
    form->addRow(tr_("AccessKeyID"), m_keyId);
    form->addRow(tr_("SecretKey"), m_secret);
    form->addRow(tr_("Region"), m_region);

    connect(m_enabled, &QCheckBox::toggled, this, [this] {
        updateEnabled();
        if (!m_loading) emit changed();
    });
    connect(m_provider, &QComboBox::currentIndexChanged, this, [this] {
        updateProviderFields();
        if (!m_loading) emit changed();
    });
    for (QLineEdit* e : { m_accountId, m_endpoint, m_bucket, m_keyId, m_secret, m_region })
        connect(e, &QLineEdit::textEdited, this,
                [this] { if (!m_loading) emit changed(); });

    updateEnabled();
    updateProviderFields();
}

void SecondaryTargetBox::updateEnabled() {
    const bool on = m_enabled->isChecked();
    for (QWidget* w : { (QWidget*)m_provider, (QWidget*)m_accountId,
                        (QWidget*)m_endpoint, (QWidget*)m_bucket,
                        (QWidget*)m_keyId, (QWidget*)m_secret,
                        (QWidget*)m_region })
        w->setEnabled(on);
}

void SecondaryTargetBox::updateProviderFields() {
    auto provider = multisite::provider_from_key(
        m_provider->currentData().toString().toStdString());
    const auto& info = multisite::provider_info(provider);
    if (auto* form = qobject_cast<QFormLayout*>(layout())) {
        form->setRowVisible(m_accountId, info.needs_account_id);
        form->setRowVisible(m_endpoint,  info.needs_endpoint);
        form->setRowVisible(m_region,    info.needs_region);
    }
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

    updateEnabled();
    updateProviderFields();
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
