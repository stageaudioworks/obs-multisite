// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// encoder_dock.h — the main campus operator panel.
//
// Replaces the Lua control script: storage settings, Go Live / End, a live
// reliability readout (queue depth, retries, link health) and marker buttons,
// all in a dock OBS remembers the position of.
//
#include <QWidget>

#include "settings_dialog.h"

#include "../core/mirror_verify.h"
#include "../storage_probe.h"

class QLineEdit;
class QSpinBox;
class QDoubleSpinBox;
class QPushButton;
class QLabel;
class QTimer;
class QCheckBox;
class QDialog;
class QComboBox;

namespace multisite_obs {

class SecondaryTargetBox;

class EncoderDock : public QWidget {
    Q_OBJECT
public:
    explicit EncoderDock(QWidget* parent = nullptr);

private slots:
    void onOpenSettings();
    void onGoLive();
    void onEnd();
    // The escape hatch on a resumed event's persistent status line: ends the
    // current (resumed) broadcast and immediately goes live into a brand new
    // event. See PROJECT-SCOPE.md §5.1.
    void onEndAndStartFresh();
    void onSaveSettings();
    void onCheckSecond();
    void showCheckSecond(const multisite::MirrorDiff& d);
    void onApplySettings();
    void onManageStorage();
    void updateAudioFields();
    // Shows only the storage fields the selected provider actually needs
    // (PROJECT-SCOPE.md §8.6) — an account id for R2, a region for
    // AWS/Backblaze/Wasabi, both endpoint and region for Custom.
    void updateProviderFields();
    // Shows the port/token fields only while LAN delivery is turned on.
    void updateLanFields();
    // Confirms before actually turning cloud upload off — nothing else warns
    // an operator that an event is about to stop leaving this machine at
    // all. Reverts the checkbox (without saving) if the operator backs out.
    // Turning cloud back ON (unchecking this) needs no confirmation: that's
    // the safe direction.
    void onDisableCloudToggled(bool checked);
    void onPairBegin();
    void onPairCancel();
    void refreshPairing();
    void refresh();

private:
    void loadIntoFields();
    // Refill the encoder list from what OBS has registered *now*, preserving
    // the operator's choice. Must not be left to construction time: the docks
    // are built during obs_module_load, when modules later in the alphabet
    // have not registered their encoders yet.
    void populateEncoders();
    void setLiveState(bool live);
    // The event-name field is pre-filled with the current date/time. Keep it
    // fresh while untouched, so a field left alone always names "now".
    void ensureEventName();
    QString defaultEventName() const;

    // storage
    // Which provider's fields are showing. Storage.NotConfigured / S3Config
    // itself never learns about this choice directly — onSaveSettings()
    // derives the raw endpoint/region/account-id fields from it before they
    // ever reach BroadcastSettings. See PROJECT-SCOPE.md §8.6.
    QComboBox* m_provider = nullptr;
    QLineEdit* m_accountId = nullptr;
    QLineEdit* m_endpoint = nullptr;
    QLineEdit* m_bucket = nullptr;
    QLineEdit* m_keyId = nullptr;
    QLineEdit* m_secret = nullptr;
    QLineEdit* m_region = nullptr;
    QLineEdit* m_room = nullptr;
    // What this machine calls itself, stamped on the cues it drops. The same
    // field the decoder has — the Cues dock is the same on either end.
    QLineEdit* m_siteName = nullptr;
    // Where the durable store-and-forward queue lives. The encoder's cache
    // folder, the counterpart to the decoder's.
    QLineEdit* m_cacheDir = nullptr;
    QPushButton* m_cacheBrowse = nullptr;
    // Machine-wide, like the role selector beside it: whether this box asks
    // GitHub once per run whether a newer build exists.
    QCheckBox* m_checkUpdates = nullptr;
    // The second bucket's fields (PROJECT-SCOPE.md §10 Phase 9). Commits to its
    // own machine-wide store when Apply is pressed.
    SecondaryTargetBox* m_secondary = nullptr;
    QCheckBox* m_tags = nullptr;
    QLabel*    m_storage = nullptr;   // colo + observed upload rate
    QLabel*    m_version = nullptr;
    // The second bucket's state, when one is configured (Phase 9). Hidden
    // otherwise. Its own sentence rather than a grid cell — see the dock.
    QLabel*    m_second = nullptr;
    // Verifying the second copy (Phase 9). Needs BOTH ends' credentials and the
    // primary's are this machine's encoder settings, which is why it lives here
    // rather than in the shared second-bucket box the decoder also uses.
    // Test the PRIMARY bucket, with the values as typed (Phase 9's idea applied
    // to the bucket everything depends on).
    QPushButton* m_testConnection = nullptr;
    QLabel*      m_testConnectionResult = nullptr;
    // The measured burst. On the storage page rather than inside the second
    // bucket's box, because it measures the LINK: an operator with no second
    // bucket still needs to know what their uplink has spare.
    QPushButton* m_testUplink = nullptr;
    QLabel*      m_testUplinkResult = nullptr;
    void showTestConnection(const ProbeResult& r);
    // Fills the caption combo from the sources that exist now, keeping
    // `selected` chosen if it is still among them.
    void refreshCaptionSources(const std::string& selected);

    QPushButton* m_checkSecond = nullptr;
    QLabel*      m_checkSecondResult = nullptr;
    // "A newer build is available", shown only when the once-per-run check
    // found one. Hidden the rest of the time, including when the check could
    // not reach GitHub at all.
    QLabel*    m_update = nullptr;

    // LAN / direct delivery (PROJECT-SCOPE.md §8.7) — off by default; port
    // and token only matter, and only show, once enabled.
    QCheckBox* m_lanEnabled = nullptr;
    QSpinBox*  m_lanPort = nullptr;
    QLineEdit* m_lanToken = nullptr;
    // Cloud storage is the primary, default delivery path — this is an
    // opt-in exception to it, so checked means DISABLED, not enabled. Only
    // shown while m_lanEnabled is checked — disabling cloud with LAN off
    // would leave nothing delivered anywhere at all.
    QCheckBox* m_disableCloud = nullptr;

    // Monitoring heartbeat (reporter-brief) — off by default. The outcome
    // line says the last POST's answer, refreshed with everything else.
    QCheckBox* m_reporterEnabled = nullptr;
    QLineEdit* m_reporterUrl = nullptr;
    QLineEdit* m_reporterId = nullptr;
    QLineEdit* m_reporterToken = nullptr;
    QLabel*    m_reporterState = nullptr;
    // Device-code pairing (TELEMETRY.md §4, preferred over typing): the code
    // on screen while the collector waits for approval, then the saved
    // credentials. m_pairWasDone keeps the "connected" line up after the
    // worker's Done is acknowledged and cleared.
    QPushButton* m_pairBtn = nullptr;
    QPushButton* m_pairCancel = nullptr;
    QLabel*      m_pairStatus = nullptr;
    bool         m_pairWasDone = false;

    // media
    QComboBox* m_encoder = nullptr;
    QDoubleSpinBox* m_segDur = nullptr;
    QSpinBox* m_vBitrate = nullptr;
    QSpinBox* m_aBitrate = nullptr;
    QSpinBox* m_tracks = nullptr;
    QLineEdit* m_trackLabels = nullptr;
    QLineEdit* m_channelLabels = nullptr;
    // How this feed is composited, if it carries more than one picture.
    QComboBox* m_tileLayout = nullptr;
    // Where captions come from: Automatic, no captions, or a named source.
    QComboBox* m_captionSource = nullptr;
    // These only apply in particular audio setups, so they are shown
    // conditionally rather than confusing everyone else.
    QWidget* m_trackLabelRow = nullptr;
    QWidget* m_channelLabelRow = nullptr;
    QLabel*  m_audioNote = nullptr;

    // controls + status
    QLineEdit*   m_eventName = nullptr;   // editable title, pre-filled with now
    QString      m_eventNameDefault;
    QPushButton* m_manageStorage = nullptr;
    QPushButton* m_goLive = nullptr;
    QPushButton* m_end = nullptr;
    QLabel* m_state = nullptr;
    QLabel* m_uptime = nullptr;
    QLabel* m_confirmed = nullptr;
    QLabel* m_queue = nullptr;
    QLabel* m_retries = nullptr;
    QLabel* m_data = nullptr;
    QLabel* m_link = nullptr;
    QLabel* m_disk = nullptr;
    QLabel* m_lan = nullptr;
    QLabel* m_error = nullptr;
    // Persistent — not auto-dismissing — line shown for as long as the live
    // broadcast is actually a resumed one, plus its escape hatch. Both hidden
    // otherwise. See PROJECT-SCOPE.md §5.1.
    QLabel*      m_resumedNote = nullptr;
    QPushButton* m_endAndFresh = nullptr;
    QTimer* m_timer = nullptr;

    // Settings live in a modal dialog rather than in the dock: they are
    // configured once, while the dock is what an operator watches mid-event.
    // Keeping them inline made the dock taller than the screen.
    SettingsDialog* m_settings = nullptr;
    QPushButton* m_settingsBtn = nullptr;
    // Whether the fields hold edits that have not been applied, so closing the
    // dialog can offer to apply them rather than discarding them silently.
    bool m_dirty = false;
    // True while loadIntoFields() is filling the widgets, so the change signals
    // it raises are not mistaken for the operator's edits.
    bool m_loading = false;
};

} // namespace multisite_obs
