// SPDX-License-Identifier: GPL-3.0-or-later
#include "encoder_dock.h"

#include "../broadcast_controller.h"
#include "../multisite_ui.h"
#include "../plugin_log.h"
#include "../storage_secondary.h"
#include "../core/mirror_verify.h"
#include "../core/s3_transport.h"
#include "../reporter.h"
#include "../update_check.h"
#include "role_selector.h"
#include "status_text.h"
#include "web_box.h"
#include "secondary_box.h"
#include "storage_dialog.h"

#include "../../core/s3_transport.h"
#include "../../core/storage_providers.h"

#include <obs-module.h>

#include <QCheckBox>
#include <thread>
#include <QMetaObject>
#include <QCoreApplication>
#include <QPointer>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <media-io/audio-io.h>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFrame>
#include <QScreen>
#include <QScrollArea>
#include <QTabWidget>
#include "settings_tabs.h"
#include <QGuiApplication>

namespace multisite_obs {

static QString tr_(const char* key) {
    return QString::fromUtf8(obs_module_text(key));
}

// Where the durable spool goes when the field is left blank. Shown as the
// field's placeholder, so an operator can see where the queue actually is.
static QString default_spool_dir() {
    char* p = obs_module_config_path("spool");
    const QString s = p ? QString::fromUtf8(p) : QStringLiteral("./multisite_spool");
    bfree(p);
    return s;
}

// Where an operator is sent when a newer build exists: the releases page lists
// what changed and carries the download, which is the whole answer.
static const char* kReleasesUrl =
    "https://github.com/stageaudioworks/obs-multisite/releases";

// Plain-language duration: an operator reads "3 min 6 sec", not "31 segments".
static QString friendly_duration(double seconds) {
    if (seconds < 1.0) return QObject::tr("none");
    const int total = (int)(seconds + 0.5);
    const int mins = total / 60;
    const int secs = total % 60;
    if (mins == 0) return QObject::tr("%1 sec").arg(secs);
    if (secs == 0) return QObject::tr("%1 min").arg(mins);
    return QObject::tr("%1 min %2 sec").arg(mins).arg(secs);
}

// The link line and the elided status values live in status_text.h, included
// above, so this dock and the decoder's cannot drift apart on wording.

EncoderDock::EncoderDock(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(8);

    // ── Status first: what an operator looks at mid-event ─────────────────
    auto* statusBox = new QGroupBox(tr_("Dock.Status"), this);
    auto* grid = new QGridLayout(statusBox);
    grid->setHorizontalSpacing(14);
    grid->setVerticalSpacing(4);

    auto addStat = [&](int row, int col, const char* labelKey, QLabel*& out) {
        auto* cap = new QLabel(tr_(labelKey), statusBox);
        // Dim but still legible: palette(mid) is nearly invisible on
        // OBS's dark theme, which left the numbers looking unlabelled.
        cap->setStyleSheet("color: palette(text); opacity: 0.75;");
        out = new QLabel("—", statusBox);
        grid->addWidget(cap, row, col * 2);
        grid->addWidget(out, row, col * 2 + 1);
    };

    m_state = new QLabel(tr_("Dock.Idle"), statusBox);
    m_state->setStyleSheet("font-weight: bold;");
    grid->addWidget(m_state, 0, 0, 1, 2);
    addStat(0, 1, "Dock.Uptime",    m_uptime);
    addStat(1, 0, "Dock.Confirmed", m_confirmed);
    addStat(1, 1, "Dock.Queue",     m_queue);
    addStat(2, 0, "Dock.Retries",   m_retries);
    addStat(2, 1, "Dock.Uploaded",  m_data);
    addStat(3, 0, "Dock.Link",      m_link);
    // Where storage is being served from and what the upload is managing.
    // A main site whose queue will not drain has no other way to tell a slow
    // link from a distant one. Labelled "Storage" rather than "Bucket": it has
    // never shown the bucket — it shows the endpoint's colo and host, and a
    // Custom endpoint is a URL, which under a "Bucket" label read as the wrong
    // value rather than as the wrong label.
    addStat(3, 1, "Dock.Storage",   m_storage);
    // The disk the durable spool lives on. Checked whether idle or live, so
    // a nearly-full drive is visible before Go Live rather than discovered
    // mid-event when the cap starts dropping queued segments.
    addStat(4, 0, "Dock.Disk",      m_disk);
    // LAN / direct delivery (PROJECT-SCOPE.md §8.7) — only meaningful while
    // live, since the server starts at Go Live; shows "off" otherwise.
    addStat(4, 1, "Dock.Lan",       m_lan);

    m_error = new QLabel(QString(), statusBox);
    m_error->setWordWrap(true);
    m_error->setStyleSheet("color: #e5484d;");
    m_error->hide();
    grid->addWidget(m_error, 5, 0, 1, 4);

    // The second bucket (PROJECT-SCOPE.md §10 Phase 9). Its own line rather
    // than a grid cell, because the useful messages are sentences — "4
    // segments behind — the link is busy with the live feed" — and a cell is
    // meant to be one short value. Hidden entirely when no second bucket is
    // configured, so a one-bucket machine looks exactly as it always did.
    m_second = new QLabel(QString(), statusBox);
    m_second->setWordWrap(true);
    m_second->hide();
    grid->addWidget(m_second, 6, 0, 1, 4);

    // Shown for as long as the live broadcast is actually a resumed one —
    // not just logged once and forgotten — so an operator can always see
    // what happened and undo it. See PROJECT-SCOPE.md §5.1.
    m_resumedNote = new QLabel(QString(), statusBox);
    m_resumedNote->setWordWrap(true);
    m_resumedNote->setStyleSheet("color: palette(text); opacity: 0.85;");
    m_resumedNote->hide();
    grid->addWidget(m_resumedNote, 7, 0, 1, 3);
    m_endAndFresh = new QPushButton(tr_("Dock.EndAndStartFresh"), statusBox);
    m_endAndFresh->hide();
    grid->addWidget(m_endAndFresh, 7, 3);
    connect(m_endAndFresh, &QPushButton::clicked,
            this, &EncoderDock::onEndAndStartFresh);

    root->addWidget(statusBox);

    // ── Event name / Go live ─────────────────────────────────────────────────
    // The name is per-event and editable, pre-filled with the current
    // date/time so an event left unnamed still shows a recognisable label.
    auto* nameRow = new QHBoxLayout();
    m_eventName = new QLineEdit(this);
    m_eventName->setToolTip(tr_("Dock.EventNameHint"));
    nameRow->addWidget(new QLabel(tr_("Dock.EventName"), this));
    nameRow->addWidget(m_eventName, 1);
    root->addLayout(nameRow);
    m_eventNameDefault = defaultEventName();
    m_eventName->setText(m_eventNameDefault);

    auto* row = new QHBoxLayout();
    m_goLive = new QPushButton(tr_("Dock.GoLive"), this);
    m_end    = new QPushButton(tr_("Dock.End"), this);
    m_end->setEnabled(false);
    row->addWidget(m_goLive);
    row->addWidget(m_end);
    root->addLayout(row);

    // Cues are dropped from the Multisite Cues dock, which is the same dock a
    // satellite gets: one mechanism, one place to name a cue, and the event's
    // own cue names as one-press buttons there. The four configurable marker
    // buttons that used to live here were a second way to do the same thing.

    // Settings button — opens the dialog built below.
    m_settingsBtn = new QPushButton(tr_("Dock.Settings"), this);
    root->addWidget(m_settingsBtn);
    connect(m_settingsBtn, &QPushButton::clicked,
            this, &EncoderDock::onOpenSettings);

    // Storage management — list the bucket's events and delete them.
    m_manageStorage = new QPushButton(tr_("Dock.ManageStorage"), this);
    root->addWidget(m_manageStorage);
    connect(m_manageStorage, &QPushButton::clicked,
            this, &EncoderDock::onManageStorage);

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

    // Where everything else about this lives. Deliberately not styled the dim
    // grey above: dim grey is right for a version number nobody is meant to
    // click and wrong for the one thing on this dock that is. Left in Qt's own
    // link colour so it reads on a light theme and a dark one, which is the
    // problem that styling everything by hand has caused here before.
    //
    // No logo: the mark is an SVG and the docks do not link QtSvg, so a picture
    // would mean a new dependency for a footer. The web pages carry it.
    auto* brand = new QLabel(
        QStringLiteral(
            "obs-multisite &middot; "
            "<a href=\"https://stageaudioworks.github.io/obs-multisite/\">"
            "manual, downloads and source</a>"),
        this);
    brand->setOpenExternalLinks(true);
    brand->setWordWrap(true);
    root->addWidget(brand);

    // ── Settings dialog ──────────────────────────────────────────────────────
    m_settings = new SettingsDialog(this);
    m_settings->setWindowTitle(tr_("Dock.SettingsTitle"));
    auto* dlgRoot = new QVBoxLayout(m_settings);

    // Three tabs rather than one long strip. This dialog carries storage, media
    // settings, the role selector and the remote control box; stacked, that was
    // taller than a laptop screen, and the buttons — the part you need to
    // dismiss it — were what fell off the bottom.
    //
    // Scrolling fixed being trapped but left an operator panning a long column
    // to find one field. These settings fall into groups that are thought about
    // separately, so they are separated: where the video goes, how it is
    // encoded, and what this machine is. Each page fits without scrolling.
    auto* tabs = new QTabWidget(m_settings);

    // ── Storage ──────────────────────────────────────────────────────────────
    auto* storePage = new QWidget(tabs);
    auto* storePageLayout = new QVBoxLayout(storePage);
    auto* storeBox = new QGroupBox(tr_("Dock.Storage"), storePage);
    auto* form = new QFormLayout(storeBox);
    m_provider  = new QComboBox(storeBox);
    m_provider->setToolTip(tr_("StorageProviderHint"));
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
    m_accountId->setToolTip(tr_("R2AccountIDHint"));
    m_endpoint  = new QLineEdit(storeBox);
    m_endpoint->setToolTip(tr_("EndpointHostHint"));
    m_bucket    = new QLineEdit(storeBox);
    m_bucket->setToolTip(tr_("BucketHint"));
    m_keyId     = new QLineEdit(storeBox);
    m_keyId->setToolTip(tr_("AccessKeyIDHint"));
    m_secret    = new QLineEdit(storeBox);
    m_secret->setToolTip(tr_("SecretKeyHint"));
    m_secret->setEchoMode(QLineEdit::Password);
    m_region    = new QLineEdit(storeBox);
    m_region->setToolTip(tr_("RegionHint"));
    m_room      = new QLineEdit(storeBox);
    m_room->setToolTip(tr_("RoomIDHint"));
    m_siteName  = new QLineEdit(storeBox);
    m_siteName->setToolTip(tr_("Dock.SiteNameHint"));
    m_tags      = new QCheckBox(tr_("SendExpiryTag"), storeBox);
    // The caveats are a tooltip, not part of the label: as a label they were a
    // single unwrapped line that set the width of the entire dialog.
    m_tags->setToolTip(tr_("SendExpiryTagHint"));
    // Lives here, not in the LAN box below: it says what happens to CLOUD
    // storage, so it belongs with the rest of the cloud storage settings.
    // Cloud is the primary, default path — this checkbox is the opt-in
    // exception to it, so it reads "disable", not "enable", and only shows
    // up once LAN is on (disabling cloud with LAN off would mean nothing is
    // delivered anywhere — see BroadcastController::go_live()'s guard
    // against exactly that). updateLanFields() shows/hides this row even
    // though it lives in a different group box than the checkbox that
    // controls its visibility.
    m_disableCloud = new QCheckBox(tr_("Dock.DisableCloud"), storeBox);
    m_disableCloud->setToolTip(tr_("Dock.DisableCloudHint"));
    form->addRow(tr_("Dock.StorageProvider"), m_provider);
    form->addRow(tr_("R2AccountID"), m_accountId);
    form->addRow(tr_("EndpointHost"), m_endpoint);
    form->addRow(tr_("Bucket"), m_bucket);
    form->addRow(tr_("AccessKeyID"), m_keyId);
    form->addRow(tr_("SecretKey"), m_secret);
    form->addRow(tr_("Region"), m_region);
    // Replaces the hidden typed fields when Multisite Cloud is the provider.
    // Read-only: the bucket and its credentials come from the collector, so
    // there is nothing here to type.
    m_pairedStorageNote = new QLabel(storeBox);
    m_pairedStorageNote->setWordWrap(true);
    m_pairedStorageNote->setVisible(false);
    form->addRow(QString(), m_pairedStorageNote);
    form->addRow(tr_("RoomID"), m_room);
    form->addRow(tr_("Dock.SiteName"), m_siteName);
    m_cacheDir = new QLineEdit(storeBox);
    m_cacheDir->setToolTip(tr_("Dock.CacheDirHint"));
    // The effective default, in grey: the encoder's queue location should be
    // visible without anyone having to set it.
    m_cacheDir->setPlaceholderText(default_spool_dir());
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
                typed.isEmpty() ? default_spool_dir() : typed);
            if (!dir.isEmpty()) {
                m_cacheDir->setText(dir);
                m_dirty = true;
            }
        });
        form->addRow(tr_("Dock.CacheDir"), wrap);
    }
    form->addRow(QString(), m_tags);
    form->addRow(QString(), m_disableCloud);
    storePageLayout->addWidget(storeBox);

    // Test the primary bucket with the values AS TYPED rather than as saved:
    // the point of a test button is to find a wrong endpoint or a key without
    // write access before Apply, and before an event depends on it.
    m_testConnection = new QPushButton(tr_("Dock.TestConnection"), storePage);
    m_testConnection->setToolTip(tr_("Dock.TestConnectionHintMain"));
    storePageLayout->addWidget(m_testConnection);
    m_testConnectionResult = new QLabel(QString(), storePage);
    m_testConnectionResult->setWordWrap(true);
    storePageLayout->addWidget(m_testConnectionResult);
    connect(m_testConnection, &QPushButton::clicked, this, [this] {
        // Multisite Cloud has no typed host to probe — the bucket, endpoint and
        // keys all come from the collector. Testing "the connection" would mean
        // probing an empty endpoint, which fails with "could not resolve host
        // name" and says nothing true. What CAN be reported is the pairing,
        // which is the real connection this provider uses.
        if (m_provider->currentData().toString() == "multisite_cloud") {
            auto id = reporter_cloud_identity(multisite::CloudRole::Encoder);
            if (!id || !id->paired())
                m_testConnectionResult->setText(tr_("Dock.TestCloudNotPaired"));
            else if (!id->credentials().present())
                m_testConnectionResult->setText(tr_("Dock.TestCloudNoCreds"));
            else
                m_testConnectionResult->setText(tr_("Dock.TestCloudOk")
                    .arg(QString::fromStdString(id->credentials().bucket)));
            return;
        }
        const std::string bucket = m_bucket->text().trimmed().toStdString();
        if (bucket.empty()) {
            m_testConnectionResult->setText(tr_("Dock.TestConnectionNoBucket"));
            return;
        }
        multisite::S3Config cfg;
        fill_s3_config(cfg, m_provider->currentData().toString().toStdString(),
                       m_accountId->text().trimmed().toStdString(),
                       m_endpoint->text().trimmed().toStdString(),
                       m_region->text().trimmed().toStdString(), bucket,
                       m_keyId->text().trimmed().toStdString(),
                       m_secret->text().toStdString());
        m_testConnection->setEnabled(false);
        m_testConnectionResult->setStyleSheet(QString());
        m_testConnectionResult->setText(tr_("Dock.TestConnectionRunning"));
        QPointer<EncoderDock> self(this);
        std::thread([self, cfg] {
            const ProbeResult r = probe_bucket(cfg, true, std::string());
            // qApp, not `self`: a QPointer used as the receiver is read on THIS
            // thread while the UI thread may be destroying the dock. See the
            // note in storage_dialog.cpp::runAsync().
            QMetaObject::invokeMethod(qApp, [self, r] {
                if (self) self->showTestConnection(r);
            }, Qt::QueuedConnection);
        }).detach();
    });

    // ── The measured burst ──────────────────────────────────────────────────
    // Operator-initiated and nothing else. It is a burst of real traffic and a
    // venue's link is not ours to fill uninvited; but it is also the ONLY way
    // to know spare capacity before an event, because the live stream never
    // produces more than its own bitrate and so can never reveal what is left.
    //
    // HERE, not inside the second bucket's box, where it started. It measures
    // the link — which an operator needs to know about whether or not they keep
    // a second copy — and putting it in that box made it unreachable without
    // one, and invisible while the box was collapsed. It tests the primary with
    // the values AS TYPED, like the button above it, and the second bucket as
    // well when one is configured, because "can this link carry both copies at
    // once" is the question it exists to answer.
    m_testUplink = new QPushButton(tr_("Dock.TestUplink"), storePage);
    m_testUplink->setToolTip(tr_("Dock.TestUplinkHint"));
    storePageLayout->addWidget(m_testUplink);
    m_testUplinkResult = new QLabel(QString(), storePage);
    m_testUplinkResult->setWordWrap(true);
    storePageLayout->addWidget(m_testUplinkResult);
    connect(m_testUplink, &QPushButton::clicked, this, [this] {
        // Multisite Cloud has no typed host, so the burst would be sent to an
        // empty endpoint — which is why this reported "Couldn't resolve host
        // name" on a machine that was working. The speed figure this button
        // exists for is the UPLINK, and on a paired machine that is measured
        // from real uploads as they happen; a burst here would spend real
        // bandwidth on the paired path to learn something already known.
        if (m_provider->currentData().toString() == "multisite_cloud") {
            m_testUplinkResult->setStyleSheet(QString());
            m_testUplinkResult->setText(tr_("Dock.TestUplinkCloud"));
            return;
        }
        const std::string bucket = m_bucket->text().trimmed().toStdString();
        if (bucket.empty()) {
            m_testUplinkResult->setStyleSheet(QString());
            m_testUplinkResult->setText(tr_("Dock.TestUplinkNone"));
            return;
        }
        multisite::S3Config primary;
        fill_s3_config(primary, m_provider->currentData().toString().toStdString(),
                       m_accountId->text().trimmed().toStdString(),
                       m_endpoint->text().trimmed().toStdString(),
                       m_region->text().trimmed().toStdString(), bucket,
                       m_keyId->text().trimmed().toStdString(),
                       m_secret->text().toStdString());
        multisite::S3Config second;
        const bool have_second = secondary_s3_config(second);

        m_testUplink->setEnabled(false);
        m_testUplinkResult->setStyleSheet(QString());
        m_testUplinkResult->setText(tr_("Dock.TestUplinkRunning"));
        QPointer<EncoderDock> self(this);
        std::thread([self, primary, second, have_second] {
            const UplinkTestResult a = uplink_test(primary);
            UplinkTestResult b;
            // Sequentially, never together: two bursts at once would measure
            // them competing with each other rather than measuring the link.
            if (have_second) b = uplink_test(second);
            QMetaObject::invokeMethod(qApp, [self, a, b, have_second] {
                if (!self) return;
                self->m_testUplink->setEnabled(true);
                QStringList lines;
                auto say = [&](const QString& which, const UplinkTestResult& r) {
                    lines << (r.ok ? tr_("Dock.TestUplinkResult")
                                         .arg(which).arg(QString::number(r.mbps, 'f', 1))
                                   : tr_("Dock.TestUplinkFailed").arg(which) + " (" +
                                         QString::fromStdString(r.error) + ")");
                };
                say(tr_("Dock.TestUplinkPrimary"), a);
                if (have_second) say(tr_("Dock.TestUplinkSecond"), b);
                self->m_testUplinkResult->setText(lines.join("\n"));
                const bool all_ok = a.ok && (!have_second || b.ok);
                self->m_testUplinkResult->setStyleSheet(
                    all_ok ? "color: #35c489;" : "color: #e5484d;");
            }, Qt::QueuedConnection);
        }).detach();
    });

    // The second bucket (PROJECT-SCOPE.md §10 Phase 9). Machine-wide rather
    // than part of these settings, because the other dock's half reads the
    // same answer — see storage_secondary.h. Nothing is written to it yet:
    // the fields exist and are saved, and the mirroring that uses them is the
    // next slice.
    m_secondary = new SecondaryTargetBox(true, storePage);
    storePageLayout->addWidget(m_secondary);
    connect(m_secondary, &SecondaryTargetBox::changed,
            this, [this] { if (!m_loading) m_dirty = true; });

    // Verifying the copy needs BOTH ends' credentials, and the primary's are
    // this machine's encoder settings — so unlike the box above, this cannot
    // live in the shared widget the decoder dock also uses.
    m_checkSecond = new QPushButton(tr_("Dock.CheckSecond"), storePage);
    m_checkSecond->setToolTip(tr_("Dock.CheckSecondHint"));
    storePageLayout->addWidget(m_checkSecond);
    m_checkSecondResult = new QLabel(QString(), storePage);
    m_checkSecondResult->setWordWrap(true);
    storePageLayout->addWidget(m_checkSecondResult);
    connect(m_checkSecond, &QPushButton::clicked, this, &EncoderDock::onCheckSecond);

    // ── LAN / direct delivery (PROJECT-SCOPE.md §8.7) ───────────────────────
    // Off by default: opening a port is exactly the kind of thing an
    // operator turns on, not discovers. Cloud upload is unaffected either
    // way, whether this is on, off, or fails to bind its port.
    auto* lanBox = new QGroupBox(tr_("Dock.LanDelivery"), storePage);
    auto* lform = new QFormLayout(lanBox);
    m_lanEnabled = new QCheckBox(tr_("Dock.LanEnabled"), lanBox);
    m_lanEnabled->setToolTip(tr_("Dock.LanEnabledHint"));
    m_lanPort = new QSpinBox(lanBox);
    m_lanPort->setToolTip(tr_("Dock.LanPortHint"));
    m_lanPort->setRange(1, 65535);
    m_lanToken = new QLineEdit(lanBox);
    m_lanToken->setToolTip(tr_("Dock.LanTokenHint"));
    lform->addRow(QString(), m_lanEnabled);
    lform->addRow(tr_("Dock.LanPort"), m_lanPort);
    lform->addRow(tr_("Dock.LanToken"), m_lanToken);
    storePageLayout->addWidget(lanBox);

    // ── Monitoring heartbeat (reporter-brief) ──────────────────────────
    // Off by default: until an operator fills this in and switches it on,
    // nothing leaves the machine. Its own box, not folded into storage or
    // LAN, because it reports on either path rather than belonging to one.
    auto* repBox = new QGroupBox(tr_("Dock.Reporter"), storePage);
    auto* rform = new QFormLayout(repBox);
    m_reporterEnabled = new QCheckBox(tr_("Dock.ReporterEnabled"), repBox);
    m_reporterEnabled->setToolTip(tr_("Dock.ReporterEnabledHint"));
    m_reporterUrl = new QLineEdit(repBox);
    m_reporterUrl->setToolTip(tr_("Dock.ReporterUrlHint"));
    m_reporterUrl->setPlaceholderText("https://");
    m_reporterId = new QLineEdit(repBox);
    m_reporterId->setToolTip(tr_("Dock.ReporterIdHint"));
    m_reporterToken = new QLineEdit(repBox);
    m_reporterToken->setToolTip(tr_("Dock.ReporterTokenHint"));
    m_reporterToken->setEchoMode(QLineEdit::Password);
    m_reporterState = new QLabel(repBox);
    m_reporterState->setWordWrap(true);
    rform->addRow(QString(), m_reporterEnabled);
    rform->addRow(tr_("Dock.ReporterUrl"), m_reporterUrl);
    rform->addRow(tr_("Dock.ReporterId"), m_reporterId);
    rform->addRow(tr_("Dock.ReporterToken"), m_reporterToken);
    rform->addRow(QString(), m_reporterState);
    // Device-code pairing (TELEMETRY.md §4): preferred over typing an ID and
    // token by hand. The worker does the asking and the polling; this page
    // only shows the code and stops it.
    {
        auto* pairRow = new QWidget(repBox);
        auto* pairLayout = new QHBoxLayout(pairRow);
        pairLayout->setContentsMargins(0, 0, 0, 0);
        m_pairBtn = new QPushButton(tr_("Dock.ReporterConnect"), pairRow);
        m_pairBtn->setToolTip(tr_("Dock.ReporterConnectHint"));
        m_pairCancel = new QPushButton(tr_("Dock.ReporterCancel"), pairRow);
        m_pairCancel->setEnabled(false);
        pairLayout->addWidget(m_pairBtn);
        pairLayout->addWidget(m_pairCancel);
        pairLayout->addStretch(1);
        rform->addRow(QString(), pairRow);
        connect(m_pairBtn, &QPushButton::clicked, this, &EncoderDock::onPairBegin);
        connect(m_pairCancel, &QPushButton::clicked, this, &EncoderDock::onPairCancel);
    }
    m_pairStatus = new QLabel(repBox);
    m_pairStatus->setWordWrap(true);
    rform->addRow(QString(), m_pairStatus);
    // The Cloud section goes FIRST: the collector connection is the origin of
    // both the heartbeat and (when Multisite Cloud is chosen) the storage,
    // so it is the first thing an operator configures, not the last thing
    // on the page. It used to sit at the bottom, named after only one of
    // its two jobs.
    storePageLayout->insertWidget(0, repBox);

    storePageLayout->addStretch(1);
    add_settings_tab(tabs, storePage, tr_("Dock.Storage"));

    connect(m_provider, &QComboBox::currentIndexChanged,
            this, &EncoderDock::updateProviderFields);
    connect(m_lanEnabled, &QCheckBox::toggled, this, &EncoderDock::updateLanFields);

    // ── Media ────────────────────────────────────────────────────────────────
    auto* mediaPage = new QWidget(tabs);
    auto* mediaPageLayout = new QVBoxLayout(mediaPage);
    auto* mediaBox = new QGroupBox(tr_("Dock.Media"), mediaPage);
    auto* mform = new QFormLayout(mediaBox);
    m_segDur = new QDoubleSpinBox(mediaBox);
    m_segDur->setRange(2.0, 15.0);
    m_segDur->setToolTip(tr_("SegmentDurationHint"));
    m_segDur->setSingleStep(0.5);
    m_segDur->setSuffix(" s");
    m_vBitrate = new QSpinBox(mediaBox);
    m_vBitrate->setToolTip(tr_("VideoBitrateHint"));
    m_vBitrate->setRange(500, 50000);
    m_vBitrate->setSingleStep(500);
    m_vBitrate->setSuffix(" kbps");
    m_aBitrate = new QSpinBox(mediaBox);
    m_aBitrate->setToolTip(tr_("AudioBitrateHint"));
    m_aBitrate->setRange(64, 512);
    m_aBitrate->setSingleStep(32);
    m_aBitrate->setSuffix(" kbps");
    m_tracks = new QSpinBox(mediaBox);
    m_tracks->setToolTip(tr_("AudioTracksHint"));
    m_tracks->setRange(1, 6);
    m_trackLabels   = new QLineEdit(mediaBox);
    m_trackLabels->setToolTip(tr_("TrackLabelsHint"));
    m_channelLabels = new QLineEdit(mediaBox);
    m_channelLabels->setToolTip(tr_("ChannelLabelsHint"));
    // Encoder choice, populated from what OBS actually has here. A hardware
    // encoder leaves the CPU free for everything else the main site is doing.
    // Filled by populateEncoders() rather than here — see that function.
    m_encoder = new QComboBox(mediaBox);
    m_encoder->setToolTip(tr_("Dock.EncoderHint"));
    mform->addRow(tr_("Dock.Encoder"), m_encoder);

    mform->addRow(tr_("SegmentDuration"), m_segDur);
    mform->addRow(tr_("Dock.VideoBitrate"), m_vBitrate);
    mform->addRow(tr_("Dock.AudioBitrate"), m_aBitrate);
    mform->addRow(tr_("Dock.AudioTracks"), m_tracks);

    // How the feed is composited, for rooms that send several cameras as one
    // picture. A list rather than free text: these are the only shapes the
    // satellite can pull apart, and a typo would be discovered at the other end
    // of the country during a service.
    m_tileLayout = new QComboBox(mediaBox);
    m_tileLayout->addItem(tr_("TileLayout.1x1"), "1x1");
    m_tileLayout->addItem(tr_("TileLayout.2x1"), "2x1");
    m_tileLayout->addItem(tr_("TileLayout.1x2"), "1x2");
    m_tileLayout->addItem(tr_("TileLayout.2x2"), "2x2");
    m_tileLayout->setToolTip(tr_("TileLayout.Help"));
    mform->addRow(tr_("TileLayout"), m_tileLayout);
    // "Audio names" label OBS mixer TRACKS and only matter when sending more
    // than one. "Channel names" label channels INSIDE a multi-channel track and
    // only matter when OBS is running a surround layout. Showing both to
    // everyone invited exactly the question "what are these for?", so each is
    // shown only when it applies.
    mform->addRow(tr_("TrackLabels"), m_trackLabels);
    m_trackLabelRow = m_trackLabels;
    mform->addRow(tr_("ChannelLabels"), m_channelLabels);
    m_channelLabelRow = m_channelLabels;
    m_audioNote = new QLabel(tr_("Dock.AudioNote"), mediaBox);
    m_audioNote->setWordWrap(true);
    m_audioNote->setStyleSheet("color: palette(text); opacity: 0.75;");
    mform->addRow(QString(), m_audioNote);
    mediaPageLayout->addWidget(mediaBox);
    mediaPageLayout->addStretch(1);
    add_settings_tab(tabs, mediaPage, tr_("Dock.Media"));

    // ── This machine ─────────────────────────────────────────────────────────
    // What the box itself is and how it is reached, as opposed to what it
    // sends. Both of these are in BOTH docks on purpose: choosing a role hides
    // the other dock, so a control living in only one of them could hide the
    // only way back.
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
    // Closing does NOT apply. An event may be live, and merely opening the
    // settings or changing your mind must not reconfigure a running broadcast.
    // Only Apply commits; a live action (Go live, End) reads the fields itself.
    connect(buttons, &QDialogButtonBox::rejected, m_settings, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked,
            this, &EncoderDock::onApplySettings);

    connect(m_goLive, &QPushButton::clicked, this, &EncoderDock::onGoLive);
    connect(m_end,    &QPushButton::clicked, this, &EncoderDock::onEnd);

    // Fields no longer save on every edit. Settings are committed with Apply,
    // or read by an action that needs them (Go live, End), so typing a value
    // and closing the dialog changes nothing — which matters most while an
    // event is live. The one exception is switching cloud delivery off, which
    // has its own confirmation below.
    connect(m_disableCloud, &QCheckBox::toggled, this,
            &EncoderDock::onDisableCloudToggled);

    loadIntoFields();
    updateAudioFields();
    connect(m_tracks, &QSpinBox::valueChanged, this,
            &EncoderDock::updateAudioFields);

    // Closing with unapplied edits asks first (see settings_dialog.h). That is
    // the other half of "closing is not a commit": closing must not silently
    // throw away what was typed either.
    m_settings->is_dirty = [this] { return m_dirty; };
    m_settings->apply_changes = [this] { onApplySettings(); };
    // textEdited, not textChanged: only a person's typing counts, and the
    // setText in loadIntoFields must not. Everything else is guarded by
    // m_loading, since setValue/setChecked/setCurrentIndex all raise signals.
    for (QLineEdit* e : { m_accountId, m_endpoint, m_bucket, m_keyId, m_secret,
                          m_region, m_room, m_siteName, m_cacheDir,
                          m_trackLabels, m_channelLabels, m_lanToken,
                          m_reporterUrl, m_reporterId, m_reporterToken,
                          m_eventName })
        connect(e, &QLineEdit::textEdited, this, [this] { m_dirty = true; });
    for (QCheckBox* cb : { m_tags, m_lanEnabled, m_disableCloud, m_reporterEnabled })
        connect(cb, &QCheckBox::toggled, this,
                [this](bool) { if (!m_loading) m_dirty = true; });
    for (QComboBox* combo : { m_provider, m_encoder, m_tileLayout })
        connect(combo, &QComboBox::currentIndexChanged, this,
                [this](int) { if (!m_loading) m_dirty = true; });
    for (QSpinBox* sb : { m_lanPort, m_vBitrate, m_aBitrate, m_tracks })
        connect(sb, &QSpinBox::valueChanged, this,
                [this](int) { if (!m_loading) m_dirty = true; });
    connect(m_segDur, &QDoubleSpinBox::valueChanged, this,
            [this](double) { if (!m_loading) m_dirty = true; });

    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &EncoderDock::refresh);
    m_timer->start(1000);
    refresh();
}

// Show each audio-naming field only in the setup where it does something.
void EncoderDock::updateAudioFields() {
    const bool multiTrack = m_tracks && m_tracks->value() > 1;

    // Channel names only matter beyond stereo, which depends on OBS's global
    // audio layout rather than on anything in this dock.
    int globalCh = 2;
    struct obs_audio_info oai = {};
    if (obs_get_audio_info(&oai)) globalCh = (int)get_audio_channels(oai.speakers);
    const bool multiChannel = globalCh > 2;

    if (auto* form = qobject_cast<QFormLayout*>(m_trackLabels->parentWidget()->layout())) {
        form->setRowVisible(m_trackLabels, multiTrack);
        form->setRowVisible(m_channelLabels, multiChannel);
    }
    if (m_audioNote) {
        if (multiChannel)
            m_audioNote->setText(tr_("Dock.AudioNoteChannels").arg(globalCh));
        else if (multiTrack)
            m_audioNote->setText(tr_("Dock.AudioNoteTracks"));
        else
            m_audioNote->setText(tr_("Dock.AudioNote"));
    }
}

void EncoderDock::updateProviderFields() {
    if (!m_provider) return;
    auto provider = multisite::provider_from_key(
        m_provider->currentData().toString().toStdString());
    const auto& info = multisite::provider_info(provider);
    // Multisite Cloud needs NO storage field: the collector supplies the
    // bucket, endpoint and credentials, so every typed field is hidden and a
    // read-only summary takes their place. Leaving editable boxes for a value
    // the box will never read is worse than no box — it looks like it does
    // something, and the operator types into it and wonders.
    const bool cloud = (provider == multisite::StorageProvider::MultisiteCloud);
    if (auto* form = qobject_cast<QFormLayout*>(m_accountId->parentWidget()->layout())) {
        form->setRowVisible(m_accountId, !cloud && info.needs_account_id);
        form->setRowVisible(m_endpoint,  !cloud && info.needs_endpoint);
        form->setRowVisible(m_region,    !cloud && info.needs_region);
        // These three have no needs_* flag — they belong to every typed-key
        // provider — so they were never hidden at all until now.
        form->setRowVisible(m_bucket,    !cloud);
        form->setRowVisible(m_keyId,     !cloud);
        form->setRowVisible(m_secret,    !cloud);
    }
    if (m_pairedStorageNote) {
        m_pairedStorageNote->setVisible(cloud);
        if (cloud) refreshPairedStorageNote();
    }
    // Selecting Multisite Cloud while looking at the Storage box means the
    // control the operator now needs — Connect — is in the Cloud box ABOVE,
    // off the top of a scrolling page. Bring it into view rather than leaving
    // them to find it: pressing a provider and being shown no way to use it is
    // the dead end this whole section exists to avoid.
    if (cloud && m_reporterEnabled) {
        if (auto* scroll = qobject_cast<QScrollArea*>(
                m_reporterEnabled->window()->findChild<QScrollArea*>()))
            scroll->ensureWidgetVisible(m_reporterEnabled);
    }
}

void EncoderDock::updateLanFields() {
    if (!m_lanEnabled) return;
    const bool on = m_lanEnabled->isChecked();
    if (auto* form = qobject_cast<QFormLayout*>(m_lanPort->parentWidget()->layout())) {
        form->setRowVisible(m_lanPort,  on);
        form->setRowVisible(m_lanToken, on);
    }
    // m_disableCloud lives in the storage box, not this one — a different
    // QFormLayout, so it needs its own parent's layout looked up separately.
    if (m_disableCloud)
        if (auto* cform = qobject_cast<QFormLayout*>(m_disableCloud->parentWidget()->layout()))
            cform->setRowVisible(m_disableCloud, on);
    // Turning LAN off with cloud already disabled would silently leave
    // nothing delivered anywhere at all — the checkbox is about to be
    // hidden, so re-enable cloud now (uncheck "disable") rather than leave
    // that state stranded where the operator can no longer even see it.
    if (!on && m_disableCloud && m_disableCloud->isChecked())
        m_disableCloud->setChecked(false);
}

void EncoderDock::onDisableCloudToggled(bool checked) {    if (!checked) { onSaveSettings(); return; }   // turning cloud back on is the safe direction
    if (QMessageBox::question(this, tr_("Dock.DisableCloud"),
                              tr_("Dock.CloudDisableConfirm"),
                              QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No) != QMessageBox::Yes) {
        const QSignalBlocker noRecurse(m_disableCloud);
        m_disableCloud->setChecked(false);
        return;
    }
    onSaveSettings();
}

void EncoderDock::refreshPairedStorageNote() {
    if (!m_pairedStorageNote) return;
    // Reads the plugin's ONE cloud identity — the same one the encoder writes
    // through — so what the panel says is what the session does, not a
    // reconstruction from form fields.
    auto id = reporter_cloud_identity(multisite::CloudRole::Encoder);
    QString t;
    if (!id || !id->paired()) {
        t = tr_("Dock.PairedNotYet");
    } else if (!id->credentials().present()) {
        t = tr_("Dock.PairedNoCreds");
    } else {
        const multisite::Credentials c = id->credentials();
        t = id->credentials().from_last_good
            ? tr_("Dock.PairedStale").arg(QString::fromStdString(c.bucket))
            : tr_("Dock.PairedBucket").arg(QString::fromStdString(c.bucket));
    }
    if (m_pairedStorageNote->text() != t) m_pairedStorageNote->setText(t);
}

void EncoderDock::onPairBegin() {
    // Commit the fields first: the worker pairs against the SAVED collector
    // URL, so a typed-but-unapplied one would pair against the old address.
    // Same rule as Start, which saves before it connects.
    onSaveSettings();
    m_pairWasDone = false;
    if (!reporter_pair_begin("obs-encoder"))
        m_pairStatus->setText(tr_("Dock.ReporterNeedUrl"));
}

void EncoderDock::onPairCancel() {
    reporter_pair_cancel("obs-encoder");
}

void EncoderDock::refreshPairing() {
    if (!m_pairStatus) return;
    const PairView v = reporter_pair_view("obs-encoder");
    const QString reason = !v.note.empty() ? QString::fromStdString(v.note)
                                           : QString::fromStdString(v.error);
    switch (v.phase) {
        case 1: {  // waiting: the code is the whole point, shown first
            const QString t = tr_("Dock.ReporterCode").arg(
                QString::fromStdString(v.user_code),
                QString::fromStdString(v.verification_url));
            if (m_pairStatus->text() != t) m_pairStatus->setText(t);
            m_pairBtn->setEnabled(false);
            m_pairCancel->setEnabled(true);
            break;
        }
        case 2: {  // done: credentials are saved — show them, then stand down
            // ...once the save has actually landed. Acknowledging first and
            // filling the fields from a save still in flight is what used to
            // wipe a fresh claim on the next Apply: the fields showed stale
            // emptiness and Apply wrote it back over the claim.
            if (!v.saved) {
                m_pairBtn->setEnabled(false);
                m_pairCancel->setEnabled(true);
                break;
            }
            if (!m_pairWasDone) {
                m_pairWasDone = true;
                // The saved ID and token, not last session's typing.
                loadIntoFields();
                reporter_pair_cancel("obs-encoder");
            }
            if (m_pairStatus->text() != tr_("Dock.ReporterDone"))
                m_pairStatus->setText(tr_("Dock.ReporterDone"));
            m_pairBtn->setEnabled(true);
            m_pairCancel->setEnabled(false);
            break;
        }
        case 3:
            if (m_pairStatus->text() != tr_("Dock.ReporterExpired"))
                m_pairStatus->setText(tr_("Dock.ReporterExpired"));
            m_pairBtn->setEnabled(true);
            m_pairCancel->setEnabled(false);
            break;
        case 4: {
            const QString t = tr_("Dock.ReporterFailed").arg(reason);
            if (m_pairStatus->text() != t) m_pairStatus->setText(t);
            m_pairBtn->setEnabled(true);
            m_pairCancel->setEnabled(false);
            break;
        }
        default:
            // Idle says nothing, unless it just finished connecting — the
            // filled-in ID and token are the proof, and the line says so.
            if (!m_pairWasDone && !m_pairStatus->text().isEmpty())
                m_pairStatus->setText(QString());
            m_pairBtn->setEnabled(true);
            m_pairCancel->setEnabled(false);
            break;
    }
}

void EncoderDock::onOpenSettings() {
    if (!m_settings) return;
    populateEncoders();        // by now every module has registered its own
    updateAudioFields();       // OBS's audio layout may have changed
    // Show what is actually saved: the dialog is reused, so without this it
    // would show last session's edits — including ones never applied.
    loadIntoFields();

    // Fit the screen it is about to open on, not the one it was built on. The
    // scroll area means everything can be reached, but Qt will still size the
    // dialog to its natural height and hand back a window taller than the
    // display. Done here rather than at construction because an operator may
    // have moved OBS to another monitor, or docked a laptop, since then.
    if (QScreen* sc = m_settings->screen() ? m_settings->screen()
                                           : QGuiApplication::primaryScreen()) {
        const QRect avail = sc->availableGeometry();
        // Nine tenths, not all: a dialog exactly the height of the work area
        // has its title bar under the menu bar on macOS and cannot be moved.
        m_settings->setMaximumHeight((int)(avail.height() * 0.9));
        m_settings->setMaximumWidth((int)(avail.width() * 0.9));
        if (m_settings->height() > m_settings->maximumHeight())
            m_settings->resize(m_settings->width(), m_settings->maximumHeight());
        if (m_settings->width() > m_settings->maximumWidth())
            m_settings->resize(m_settings->maximumWidth(), m_settings->height());
    }

    m_settings->exec();
    // Deliberately nothing after exec(): closing is not a commit. Apply
    // commits, and a close leaves a running broadcast untouched.
}

// Refill from what OBS has registered at this moment.
//
// This used to run in the constructor, and the constructor runs inside
// obs_module_load — so the list was whatever happened to be registered by the
// time this module loaded, and OBS loads modules in alphabetical order. On a
// machine with NVENC, QuickSync and x264, `obs-multisite` sorts before
// `obs-nvenc`, `obs-qsv11` and `obs-x264`, so none of them existed yet and the
// operator was offered the two AV1 encoders from `obs-ffmpeg` and nothing
// else. Even the x264 safety net could not help: obs-x264 had not registered,
// so asking for its codec returned nothing.
//
// Called when Settings is opened, which is the only time the list is seen and
// is always long after loading has finished.
void EncoderDock::populateEncoders() {
    if (!m_encoder) return;

    // Whatever is selected now, or the saved choice if nothing is.
    QString want = m_encoder->currentData().toString();
    if (want.isEmpty()) {
        auto cfg = BroadcastController::instance().settings();
        want = QString::fromStdString(cfg.video_encoder_id);
    }

    m_encoder->blockSignals(true);
    m_encoder->clear();
    for (const auto& e : available_video_encoders()) {
        // Show the codec explicitly: two entries can have similar names, and
        // the codec is what actually matters at the satellite.
        QString label = QString::fromStdString(e.name);
        label += "  —  " + QString::fromStdString(e.codec).toUpper();
        if (e.hardware) label += tr_("Dock.HardwareSuffix");
        if (e.codec == "av1") label += tr_("Dock.ExperimentalSuffix");
        m_encoder->addItem(label, QString::fromStdString(e.id));
    }
    const int idx = want.isEmpty() ? -1 : m_encoder->findData(want);
    if (idx >= 0) m_encoder->setCurrentIndex(idx);
    m_encoder->blockSignals(false);

    // Say how many were found and whether the saved one is among them. A
    // missing encoder falls back to x264 at go-live, and the log is where an
    // operator finds out why the picture is software-encoded.
    if (!want.isEmpty() && idx < 0)
        mlog_warn("encoder dock: %d encoder(s) available; the saved choice "
                  "'%s' is not among them", m_encoder->count(),
                  want.toStdString().c_str());
    else
        mlog_info("encoder dock: %d video encoder(s) available",
                  m_encoder->count());
}

void EncoderDock::loadIntoFields() {
    m_loading = true;   // the setValue/setChecked below are not the operator's edits

    // setChecked()/setCurrentIndex() below emit toggled()/currentIndexChanged()
    // whenever the loaded value differs from whatever a freshly constructed
    // widget defaulted to — and every one of those signals is wired to
    // onSaveSettings(), which unconditionally reads EVERY field on the page,
    // including ones this function has not reached yet. Left unblocked, that
    // fires mid-load and immediately writes a settings object built from a
    // mix of just-loaded and still-default widgets back to disk — silently
    // overwriting real settings (a saved AWS/Backblaze/Wasabi/Custom
    // provider, in particular) with blanks, or a just-added field like
    // lan_port with its just-constructed default rather than the value two
    // lines below about to set it correctly. This is what actually happened
    // building LAN delivery: turning it on tripped exactly this and wrote
    // port 1 to disk before this function got to the real value.
    const QSignalBlocker noSaveTags(m_tags);
    const QSignalBlocker noSaveProvider(m_provider);
    const QSignalBlocker noSaveEncoder(m_encoder);
    const QSignalBlocker noSaveLan(m_lanEnabled);
    const QSignalBlocker noSaveCloud(m_disableCloud);

    auto cfg = BroadcastController::instance().settings();
    cfg.load();
    BroadcastController::instance().set_settings(cfg);

    // An empty storage_provider means this was saved before the provider
    // dropdown existed (or migrated from one that predates it): fall back to
    // guessing from the raw fields rather than defaulting blindly to R2, so
    // an upgrade never misrepresents a working AWS/Backblaze/Wasabi/Custom
    // setup as something it isn't.
    auto provider = cfg.storage_provider.empty()
        ? multisite::detect_provider(cfg.endpoint_host, cfg.r2_account_id)
        : multisite::provider_from_key(cfg.storage_provider);
    {
        const int idx = m_provider->findData(
            QString::fromStdString(multisite::provider_key(provider)));
        m_provider->setCurrentIndex(idx >= 0 ? idx : 0);
    }
    m_accountId->setText(QString::fromStdString(cfg.r2_account_id));
    m_endpoint->setText(QString::fromStdString(cfg.endpoint_host));
    m_bucket->setText(QString::fromStdString(cfg.bucket));
    m_keyId->setText(QString::fromStdString(cfg.access_key_id));
    m_secret->setText(QString::fromStdString(cfg.secret_access_key));
    m_region->setText(QString::fromStdString(cfg.region));
    m_room->setText(QString::fromStdString(cfg.room_id));
    m_siteName->setText(QString::fromStdString(cfg.site_name));
    m_cacheDir->setText(QString::fromStdString(cfg.cache_dir));
    m_tags->setChecked(cfg.send_expiry_tag);
    updateProviderFields();
    {
        const int idx = m_encoder->findData(
            QString::fromStdString(cfg.video_encoder_id));
        if (idx >= 0) m_encoder->setCurrentIndex(idx);
    }
    m_segDur->setValue(cfg.segment_duration_s);
    m_vBitrate->setValue(cfg.video_bitrate_kbps);
    m_aBitrate->setValue(cfg.audio_bitrate_kbps);
    m_tracks->setValue(cfg.audio_tracks);
    m_trackLabels->setText(QString::fromStdString(cfg.track_labels));
    m_channelLabels->setText(QString::fromStdString(cfg.channel_labels));
    {
        // findData rather than an index: the stored value is the layout string
        // itself, so reordering or adding entries later cannot silently change
        // what a saved configuration means.
        const int i = m_tileLayout->findData(
            QString::fromStdString(cfg.tile_layout));
        m_tileLayout->setCurrentIndex(i >= 0 ? i : 0);   // unknown reads as 1x1
    }
    m_lanEnabled->setChecked(cfg.lan_enabled);
    m_lanPort->setValue(cfg.lan_port);
    m_lanToken->setText(QString::fromStdString(cfg.lan_auth_token));
    m_disableCloud->setChecked(!cfg.cloud_enabled);
    m_reporterEnabled->setChecked(cfg.reporter_enabled);
    m_reporterUrl->setText(QString::fromStdString(cfg.reporter_url));
    m_reporterId->setText(QString::fromStdString(cfg.reporter_appliance_id));
    m_reporterToken->setText(QString::fromStdString(cfg.reporter_token));
    // Machine-wide, so read from its own store rather than from cfg — and read
    // here, on every open, so a change made in the other dock's dialog shows up.
    m_checkUpdates->setChecked(update_check_enabled());
    m_secondary->loadFromStore();
    updateLanFields();
    m_loading = false;
    m_dirty = false;
}

void EncoderDock::showTestConnection(const ProbeResult& r) {
    m_testConnection->setEnabled(true);
    if (r.ok) {
        m_testConnectionResult->setText(tr_("Dock.TestConnectionOkMain"));
        m_testConnectionResult->setStyleSheet("color: #35c489;");
    } else {
        m_testConnectionResult->setText(
            tr_("Dock.TestConnectionFailed").arg(QString::fromStdString(r.detail)));
        m_testConnectionResult->setStyleSheet("color: #e5484d;");
    }
}

void EncoderDock::onCheckSecond() {
    const auto st = BroadcastController::instance().status();
    if (st.event_id.empty()) {
        m_checkSecondResult->setText(tr_("Dock.CheckSecondNoEvent"));
        return;
    }

    // The primary, in the same shape the output builds it from — so the check
    // compares exactly the two places the bytes were meant to go.
    multisite::S3Config primary_cfg;
    {
        const BroadcastSettings cfg = BroadcastController::instance().settings();
        auto provider = multisite::provider_from_key(cfg.storage_provider);
        if (provider == multisite::StorageProvider::Custom) {
            primary_cfg.endpoint_host = cfg.endpoint_host;
            primary_cfg.region        = cfg.region;
        } else {
            const std::string input =
                provider == multisite::StorageProvider::CloudflareR2 ? cfg.r2_account_id
                                                                     : cfg.region;
            auto derived = multisite::derive(provider, input);
            primary_cfg.r2_account_id = derived.r2_account_id;
            primary_cfg.endpoint_host = derived.endpoint_host;
            primary_cfg.region        = derived.region;
        }
        primary_cfg.bucket            = cfg.bucket;
        primary_cfg.access_key_id     = cfg.access_key_id;
        primary_cfg.secret_access_key = cfg.secret_access_key;
    }
    multisite::S3Config second_cfg;
    if (!secondary_s3_config(second_cfg)) {
        m_checkSecondResult->setText(tr_("Dock.CheckSecondNone"));
        return;
    }
    if (primary_cfg.bucket.empty()) {
        m_checkSecondResult->setText(tr_("Dock.CheckSecondNoPrimary"));
        return;
    }

    const std::string ev = st.event_id;
    m_checkSecond->setEnabled(false);
    m_checkSecondResult->setStyleSheet(QString());
    m_checkSecondResult->setText(tr_("Dock.CheckSecondRunning"));

    // Off the UI thread, and guarded: two manifests is quick, but it is two
    // network round trips and neither belongs on the paint thread.
    QPointer<EncoderDock> self(this);
    std::thread([self, primary_cfg, second_cfg, ev] {
        multisite::S3Transport a(primary_cfg), b(second_cfg);
        const multisite::MirrorDiff d = multisite::compare_targets(a, b, ev);
        QMetaObject::invokeMethod(qApp, [self, d] {
            if (self) self->showCheckSecond(d);
        }, Qt::QueuedConnection);
    }).detach();
}

void EncoderDock::showCheckSecond(const multisite::MirrorDiff& d) {
    m_checkSecond->setEnabled(true);
    if (!d.ok()) {
        m_checkSecondResult->setText(
            tr_("Dock.CheckSecondError").arg(QString::fromStdString(d.error)));
        m_checkSecondResult->setStyleSheet("color: #e5484d;");
    } else if (d.complete) {
        m_checkSecondResult->setText(tr_("Dock.CheckSecondComplete"));
        m_checkSecondResult->setStyleSheet("color: #35c489;");
    } else {
        // Named, not just counted: which segments and how they differ is what
        // tells an operator whether this is a gap to worry about.
        m_checkSecondResult->setText(tr_("Dock.CheckSecondDiff")
                                         .arg((qulonglong)d.only_primary.size())
                                         .arg((qulonglong)d.only_second.size())
                                         .arg((qulonglong)d.checksum_mismatch.size()));
        m_checkSecondResult->setStyleSheet("color: #e0a020;");
    }
}

void EncoderDock::onSaveSettings() {
    // Trim everything: values are usually pasted from a dashboard and a
    // trailing space in a key or bucket produces failures that look nothing
    // like their cause.
    BroadcastSettings cfg;
    // The provider decides how the fields the operator actually typed become
    // S3Config's raw shape (endpoint_host / r2_account_id / region) — see
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
    cfg.room_id           = m_room->text().trimmed().toStdString();
    cfg.site_name         = m_siteName->text().trimmed().toStdString();
    cfg.cache_dir         = m_cacheDir->text().trimmed().toStdString();
    cfg.send_expiry_tag   = m_tags->isChecked();
    cfg.video_encoder_id   = m_encoder->currentData().toString().toStdString();
    cfg.segment_duration_s = m_segDur->value();
    cfg.video_bitrate_kbps = m_vBitrate->value();
    cfg.audio_bitrate_kbps = m_aBitrate->value();
    cfg.audio_tracks       = m_tracks->value();
    cfg.track_labels       = m_trackLabels->text().toStdString();
    cfg.channel_labels     = m_channelLabels->text().toStdString();
    cfg.tile_layout        = m_tileLayout->currentData().toString().toStdString();
    cfg.lan_enabled    = m_lanEnabled->isChecked();
    cfg.lan_port       = m_lanPort->value();
    cfg.lan_auth_token = m_lanToken->text().trimmed().toStdString();
    cfg.cloud_enabled  = !m_disableCloud->isChecked();
    cfg.reporter_enabled      = m_reporterEnabled->isChecked();
    cfg.reporter_url          = m_reporterUrl->text().trimmed().toStdString();
    cfg.reporter_appliance_id = m_reporterId->text().trimmed().toStdString();
    cfg.reporter_token        = m_reporterToken->text().trimmed().toStdString();
    // A claim that landed after the fields were last loaded lives in settings
    // but not yet in these widgets; writing the widgets back would wipe it.
    if (reporter_pair_view("obs-encoder").phase == 2) {
        const BroadcastSettings live =
            BroadcastController::instance().settings_copy();
        cfg.reporter_appliance_id = live.reporter_appliance_id;
        cfg.reporter_token = live.reporter_token;
        cfg.reporter_device_id = live.reporter_device_id;
    }
    // The event name is per-event, not a saved setting. Send it only when the
    // operator has typed their own; an untouched date/time default is sent
    // empty so the satellite falls back to the time and a resumed event keeps
    // its original name.
    ensureEventName();
    const QString typedName = m_eventName->text().trimmed();
    cfg.event_name = (typedName.isEmpty() || typedName == m_eventNameDefault)
                         ? std::string()
                         : typedName.toStdString();
    BroadcastController::instance().set_settings(cfg);
    // Its own store, and committed with Apply like everything else on this
    // dialog — so opening the settings to look at something still changes
    // nothing, which is the promise the Apply button exists to make.
    update_check_set_enabled(m_checkUpdates->isChecked());
    m_secondary->saveToStore();
    m_dirty = false;
}

void EncoderDock::onApplySettings() {
    // Commit the fields now. set_settings(), inside onSaveSettings(), is what
    // applies them to a running session.
    onSaveSettings();
}

QString EncoderDock::defaultEventName() const {
    return QDateTime::currentDateTime().toString("ddd d MMM yyyy, HH:mm");
}

void EncoderDock::ensureEventName() {
    const QString typed = m_eventName->text().trimmed();
    if (typed.isEmpty() || typed == m_eventNameDefault) {
        m_eventNameDefault = defaultEventName();
        m_eventName->setText(m_eventNameDefault);
    }
}

void EncoderDock::onGoLive() {
    onSaveSettings();

    // Checked from disk alone, before anything else exists — a deferred-start
    // encoder may not construct its Session until well after this click, which
    // would be too late to ask. See PROJECT-SCOPE.md §5.1: a crash minutes ago
    // just resumes, as it always has; anything older is asked about, because
    // silently continuing a leftover from last week is worse than a click.
    bool forceNew = false;
    auto resume = BroadcastController::instance().check_resumable_before_go_live();
    if (resume.resumable && resume.stale) {
        QString when = resume.last_activity_ms > 0
            ? QDateTime::fromMSecsSinceEpoch(resume.last_activity_ms)
                  .toString("ddd d MMM yyyy, HH:mm")
            : tr_("Dock.ResumeUnknownTime");
        QString text = tr_("Dock.ResumeStaleBody").arg(when);
        if (resume.pending_count > 0)
            text += QString(" ") + tr_("Dock.ResumeAbandonsPending")
                        .arg((qulonglong)resume.pending_count);

        QMessageBox box(this);
        box.setIcon(QMessageBox::Question);
        box.setWindowTitle(tr_("Dock.ResumeStaleTitle"));
        box.setText(text);
        QPushButton* resumeBtn = box.addButton(tr_("Dock.ResumeEvent"),
                                               QMessageBox::AcceptRole);
        QPushButton* newBtn = box.addButton(tr_("Dock.StartNewEvent"),
                                            QMessageBox::DestructiveRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(resumeBtn);
        box.exec();
        if (box.clickedButton() == newBtn) {
            forceNew = true;
        } else if (box.clickedButton() != resumeBtn) {
            return;   // Cancel — do not go live at all
        }
    }

    std::string err;
    if (!BroadcastController::instance().go_live(err, forceNew)) {
        // Show the reason here rather than making the operator find the log.
        QMessageBox::warning(this, tr_("Dock.GoLiveFailed"),
                             QString::fromStdString(err));
        return;
    }
    setLiveState(true);
}

void EncoderDock::onEnd() {
    BroadcastController::instance().end_broadcast();
    setLiveState(false);
}

void EncoderDock::onEndAndStartFresh() {
    if (QMessageBox::question(this, tr_("Dock.EndAndStartFresh"),
                              tr_("Dock.EndAndStartFreshConfirm"),
                              QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No) != QMessageBox::Yes)
        return;
    BroadcastController::instance().end_broadcast();
    setLiveState(false);
    std::string err;
    if (!BroadcastController::instance().go_live(err, /*force_new_event=*/true)) {
        QMessageBox::warning(this, tr_("Dock.GoLiveFailed"),
                             QString::fromStdString(err));
        return;
    }
    setLiveState(true);
}

void EncoderDock::onManageStorage() {
    const BroadcastSettings& cfg = BroadcastController::instance().settings();

    // A paired machine keeps its bucket, endpoint and keys in the pairing, not
    // in these typed fields — so the emptiness test below would refuse a box
    // that is recording fine, and the S3Config built from the fields would name
    // no host at all. The window is about the bucket being recorded TO, so on
    // Multisite Cloud that is the paired bucket, read through the credentials
    // the reporter is already holding.
    multisite::S3Config s3;
    if (cfg.storage_provider == "multisite_cloud") {
        auto id = reporter_cloud_identity(multisite::CloudRole::Encoder);
        if (!id || !id->paired() || !id->credentials().present()) {
            QMessageBox::information(this, tr_("Dock.ManageStorage"),
                                     tr_("Storage.CloudNotReady"));
            return;
        }
        s3 = multisite::s3_config_from_credentials(id->credentials());
    } else {
        if (cfg.bucket.empty() ||
            (cfg.endpoint_host.empty() && cfg.r2_account_id.empty())) {
            QMessageBox::information(this, tr_("Dock.ManageStorage"),
                                     tr_("Storage.NotConfigured"));
            return;
        }
        s3.endpoint_host    = cfg.endpoint_host;
        s3.r2_account_id    = cfg.r2_account_id;
        s3.bucket           = cfg.bucket;
        s3.access_key_id    = cfg.access_key_id;
        s3.secret_access_key = cfg.secret_access_key;
        s3.region           = cfg.region;
    }

    // The settings, not a ready-made transport: the window builds one per
    // operation, so closing it stops the work in flight without leaving a
    // cancelled transport behind for the next delete to trip over.
    StorageDialog dlg(this, cfg.room_id, s3);
    dlg.exec();
}

void EncoderDock::setLiveState(bool live) {
    m_goLive->setEnabled(!live);
    m_end->setEnabled(live);
    // Storage cannot change mid-broadcast.
    for (QWidget* w : { (QWidget*)m_provider, (QWidget*)m_accountId,
                        (QWidget*)m_endpoint,
                        (QWidget*)m_bucket, (QWidget*)m_keyId,
                        (QWidget*)m_secret, (QWidget*)m_region,
                        (QWidget*)m_room, (QWidget*)m_siteName,
                        (QWidget*)m_cacheDir, (QWidget*)m_segDur,
                        (QWidget*)m_tracks, (QWidget*)m_lanEnabled,
                        (QWidget*)m_lanPort, (QWidget*)m_lanToken,
                        (QWidget*)m_disableCloud })
        w->setEnabled(!live);
}

void EncoderDock::refresh() {
    // The update check is about this machine, not about the broadcast, so it is
    // answered first and is shown whether or not anything is going out. Nothing
    // is said when the check could not reach GitHub, or when this build is
    // current: only a newer build produces a line.
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

    auto st = BroadcastController::instance().status();
    setLiveState(st.live);

    // The internet reading, shown whether or not a broadcast is running. While
    // live the words carry the store-and-forward reassurance; while idle they
    // are plainer, because nothing is being saved to this computer yet.
    auto showLink = [&](bool live) {
        if (!st.link_known) {
            m_link->setText(QString("—"));
            m_link->setStyleSheet(QString());
            return;
        }
        switch (st.link_health) {
            case 0:
                m_link->setText(tr_("Dock.LinkHealthy"));
                m_link->setStyleSheet("color: #35c489;");
                break;
            case 1:
                m_link->setText(live ? tr_("Dock.LinkDegradedLive")
                                     : tr_("Dock.LinkDegraded"));
                m_link->setStyleSheet("color: #e0a020;");
                break;
            default:
                m_link->setText(live ? tr_("Dock.LinkOfflineLive")
                                     : tr_("Dock.LinkOffline"));
                m_link->setStyleSheet("color: #e5484d;");
                break;
        }
    };

    // The disk the durable spool lives on, whether idle or live: a nearly-full
    // drive is worth knowing before Go Live, not just when the cap starts
    // dropping queued segments (see SessionConfig::max_spool_bytes).
    auto showDisk = [&] {
        if (!m_disk) return;
        if (!st.disk_known) {
            m_disk->setText(QString("—"));
            m_disk->setStyleSheet(QString());
            return;
        }
        QString free = QString::number(
            st.disk_free_bytes / (1024.0 * 1024.0 * 1024.0), 'f', 1) + " GB";
        switch (st.disk_health) {
            case 0:
                m_disk->setText(tr_("Dock.DiskHealthy") + " (" + free + ")");
                m_disk->setStyleSheet("color: #35c489;");
                break;
            case 1:
                m_disk->setText(tr_("Dock.DiskLow") + " (" + free + ")");
                m_disk->setStyleSheet("color: #e0a020;");
                break;
            default:
                m_disk->setText(tr_("Dock.DiskCritical") + " (" + free + ")");
                m_disk->setStyleSheet("color: #e5484d;");
                break;
        }
    };

    // LAN / direct delivery (PROJECT-SCOPE.md §8.7). Meaningful only while
    // live — the server starts at Go Live and stops at End — so idle just
    // says whether it's turned on for next time.
    // The second bucket: how far behind it is, and why. Three states matter and
    // they are acted on differently, so they are said differently:
    //
    //   complete            — both buckets hold everything; nothing to do
    //   waiting on the live feed — the link cannot carry both at once, and the
    //                         copy will finish after the event
    //   not answering       — the second bucket itself is the problem
    //
    // Shown only when one is configured, so a one-bucket machine is unchanged.
    auto showSecond = [&] {
        if (!m_second) return;
        if (!st.mirror_configured) { m_second->hide(); return; }
        m_second->show();
        if (st.mirror_complete) {
            m_second->setText(tr_("Dock.SecondUpToDate"));
            m_second->setStyleSheet("color: #35c489;");
        } else if (st.mirror_unreachable) {
            m_second->setText(tr_("Dock.SecondUnreachable")
                                  .arg((qulonglong)st.mirror_behind));
            m_second->setStyleSheet("color: #e5484d;");
        } else if (st.mirror_waiting_on_primary) {
            // Not a fault and must not read as one: this is the yield rule
            // doing its job, and the copy finishes after the event.
            m_second->setText(tr_("Dock.SecondYielding")
                                  .arg((qulonglong)st.mirror_behind));
            m_second->setStyleSheet("color: #e0a020;");
        } else {
            m_second->setText(tr_("Dock.SecondBehind")
                                  .arg((qulonglong)st.mirror_behind));
            m_second->setStyleSheet("color: #3b82c4;");
        }
    };

    auto showLan = [&] {
        if (!m_lan) return;
        if (!st.lan_enabled) {
            m_lan->setText(tr_("Dock.LanOff"));
            m_lan->setStyleSheet(QString());
            m_lan->setToolTip(QString());
            return;
        }
        // Cloud upload is unaffected either way, EXCEPT when the operator
        // deliberately turned it off — worth surfacing, but as a tooltip
        // rather than appended to the status text: a stat cell in this grid
        // is meant to be one short line, and the full sentence here was
        // wide enough to widen the whole dock around it.
        m_lan->setToolTip(st.cloud_enabled ? QString() : tr_("Dock.CloudOffSuffix"));
        if (!st.live) {
            m_lan->setText(tr_("Dock.LanWillStart"));
            m_lan->setStyleSheet(QString());
        } else if (st.lan_running) {
            m_lan->setText(tr_("Dock.LanRunning").arg(st.lan_port)
                               .arg((qulonglong)st.lan_cached_segments));
            m_lan->setStyleSheet("color: #35c489;");
        } else {
            m_lan->setText(st.lan_error.empty()
                ? tr_("Dock.LanFailed")
                : tr_("Dock.LanFailed") + " (" + QString::fromStdString(st.lan_error) + ")");
            m_lan->setStyleSheet("color: #e5484d;");
        }
    };

    if (!st.live) {
        m_state->setText(tr_("Dock.Idle"));
        m_state->setStyleSheet("font-weight: bold; color: palette(mid);");
        m_uptime->setText("—");
        showLink(false);
        showDisk();
        showLan();
        showSecond();
        // The idle probe's colo and host, so the operator can see where the
        // bucket answers from before they go live.
        multisite_ui::set_value(m_storage, multisite_ui::link_summary(
            QString::fromStdString(st.colo),
            QString::fromStdString(st.storage_host),
            st.upload_bytes_per_s, st.upload_samples));
        m_error->hide();
        m_resumedNote->hide();
        m_endAndFresh->hide();
        // Pairing is shown while IDLE, which is when it happens — before
        // anyone goes live. This used to fall through past the call, so the
        // dock's pairing label was only ever updated during a broadcast: the
        // worker generated the code, logged it, and the dock showed nothing,
        // because it was not live at the time. See BUGS.md entry 2-style:
        // a code that exists and is never displayed.
        refreshPairing();
        return;
    }

    if (m_storage)
        multisite_ui::set_value(m_storage, multisite_ui::link_summary(
            QString::fromStdString(st.colo),
            QString::fromStdString(st.storage_host),
            st.upload_bytes_per_s, st.upload_samples));

    m_state->setText(tr_("Dock.Broadcasting"));
    m_state->setStyleSheet("font-weight: bold; color: #35c489;");

    const int mins = (int)(st.uptime_s / 60.0);
    const int secs = (int)st.uptime_s % 60;
    m_uptime->setText(QString("%1:%2").arg(mins).arg(secs, 2, 10, QChar('0')));
    // How much of the event has been sent, in time — the count of segments
    // is an implementation detail nobody needs.
    const double seg = m_segDur ? m_segDur->value() : 6.0;
    m_confirmed->setText(friendly_duration((double)st.confirmed * seg));
    m_queue->setText(st.pending == 0
                       ? tr_("Dock.NothingWaiting")
                       : friendly_duration((double)st.pending * seg));
    m_retries->setText(st.retries == 0 ? tr_("Dock.None")
                                       : QString::number(st.retries));
    m_data->setText(QString::number(st.bytes / (1024.0 * 1024.0), 'f', 0) + " MB");

    showLink(true);
    showDisk();
    showLan();
    showSecond();

    // A real error wins the red line. A clock far enough from the store's earns
    // it too — not because times read oddly, which stopped being true when
    // positions became elapsed, but because requests are SIGNED with this
    // clock and the store refuses them once it is far enough out. See
    // clock_skew_level() for the bands and the reasoning.
    QString warn = QString::fromStdString(st.last_error);
    const auto level = multisite_ui::clock_skew_level(st.clock_skew_ms);
    if (warn.isEmpty() && level != multisite_ui::ClockSkew::Fine) {
        const QString by = multisite_ui::clock_skew_text(st.clock_skew_ms);
        warn = (level == multisite_ui::ClockSkew::Urgent
                    ? tr_("Dock.ClockOutUrgent") : tr_("Dock.ClockOut")).arg(by);
    }
    if (!warn.isEmpty()) {
        m_error->setText(warn);
        m_error->show();
    } else {
        m_error->hide();
    }

    // Persistent for as long as it's true, not just logged once — an
    // operator should always be able to see that this broadcast continues an
    // earlier one, and undo that in one click. See PROJECT-SCOPE.md §5.1.
    if (!st.resumed_event_id.empty()) {
        QString when = st.resumed_event_started_ms > 0
            ? QDateTime::fromMSecsSinceEpoch(st.resumed_event_started_ms)
                  .toString("ddd d MMM yyyy, HH:mm")
            : tr_("Dock.ResumeUnknownTime");
        m_resumedNote->setText(
            tr_("Dock.ResumedFrom").arg(when)
                .arg((qulonglong)st.resumed_already_confirmed));
        m_resumedNote->show();
        m_endAndFresh->show();
    } else {
        m_resumedNote->hide();
        m_endAndFresh->hide();
    }

    // The heartbeat's last answer, in the settings dialog — so an operator
    // who just switched it on sees the first 200 arrive without reopening
    // anything. Compared before setting: setText re-parses every call.
    if (m_reporterState) {
        const QString t =
            QString::fromStdString(reporter_last_result("obs-encoder"));
        if (m_reporterState->text() != t) m_reporterState->setText(t);
    }
    refreshPairing();
}

} // namespace multisite_obs
