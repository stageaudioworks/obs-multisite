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
    void onMarker(int index);
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
    QCheckBox* m_tags = nullptr;
    QLabel*    m_storage = nullptr;   // colo + observed upload rate
    QLabel*    m_version = nullptr;

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

    // media
    QComboBox* m_encoder = nullptr;
    QDoubleSpinBox* m_segDur = nullptr;
    QSpinBox* m_vBitrate = nullptr;
    QSpinBox* m_aBitrate = nullptr;
    QSpinBox* m_tracks = nullptr;
    QLineEdit* m_trackLabels = nullptr;
    QLineEdit* m_channelLabels = nullptr;
    QLineEdit* m_markerLabels = nullptr;
    // How this feed is composited, if it carries more than one picture.
    QComboBox* m_tileLayout = nullptr;
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
    QPushButton* m_markers[4] = { nullptr, nullptr, nullptr, nullptr };
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
    QDialog* m_settings = nullptr;
    QPushButton* m_settingsBtn = nullptr;
};

} // namespace multisite_obs
