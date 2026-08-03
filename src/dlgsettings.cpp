/*
 * Copyright (c) 2013-2019 Thomas Isaac Lightburn
 *
 *
 * This file is part of OpenKJ.
 *
 * OpenKJ is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "dlgsettings.h"
#include "ui_dlgsettings.h"
#include <QGuiApplication>
#include <QFontDialog>
#include <QColorDialog>
#include <QFileDialog>
#include <QStandardPaths>
#include <QMessageBox>
#include <QLabel>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QDir>
#include <QFileInfo>
#include <QSqlQuery>
#include <QtSql>
#include <QVBoxLayout>
#include <QXmlStreamWriter>
#include <QNetworkReply>
#include <QAuthenticator>
#include <QKeySequenceEdit>
#include <QScrollArea>
#include "audiorecorder.h"
#include "dlgyoutubecache.h"
#include <QScreen>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

DlgSettings::DlgSettings(MediaBackend &AudioBackend, MediaBackend &BmAudioBackend, OKJSongbookAPI &songbookAPI,
                         QWidget *parent) :
        QDialog(parent),
        kAudioBackend(AudioBackend),
        bmAudioBackend(BmAudioBackend),
        songbookApi(songbookAPI),
        ui(new Ui::DlgSettings) {
    m_logger = spdlog::get("logger");
    m_pageSetupDone = false;
    networkManager = new QNetworkAccessManager(this);
    ui->setupUi(this);
    // The size the dialog was drawn at, before any saved geometry replaces it -
    // it is the floor for how small the dialog opens on a first run.
    const QSize designedSize = size();
    const bool restoredGeometry = m_settings.restoreWindowState(this);
    setStyleSheet(R"(
        QDialog {
            background: palette(window);
        }
        QTabWidget::pane {
            border: 1px solid rgba(127,127,127,0.28);
            border-radius: 8px;
        }
        QTabBar::tab {
            padding: 8px 14px;
            margin: 2px;
            border-radius: 6px;
            min-height: 22px;
        }
        QGroupBox {
            font-weight: 600;
            border: 1px solid rgba(127,127,127,0.24);
            border-radius: 8px;
            margin-top: 12px;
            padding-top: 10px;
        }
        QGroupBox::title {
            left: 10px;
            padding: 0 4px;
        }
        QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox, QFontComboBox, QKeySequenceEdit {
            min-height: 28px;
            padding: 3px 8px;
            border-radius: 6px;
        }
        QPushButton {
            min-height: 28px;
            padding: 4px 10px;
            border-radius: 6px;
        }
    )");
    ui->checkBoxHardwareAccel->setChecked(m_settings.hardwareAccelEnabled());
    setFont(m_settings.applicationFont());
#ifdef Q_OS_MACOS
    ui->checkBoxHardwareAccel->setHidden(true);
#endif
    ui->comboBoxConsoleLogLevel->addItems(
            {
                    "Disabled",
                    "Critical",
                    "Error",
                    "Warning",
                    "Info",
                    "Debug",
                    "Trace"
            }
    );
    ui->comboBoxFileLogLevel->addItems(
            {
                    "Disabled",
                    "Critical",
                    "Error",
                    "Warning",
                    "Info",
                    "Debug",
                    "Trace"
            }
    );
    ui->comboBoxConsoleLogLevel->setCurrentIndex(m_settings.getConsoleLogLevel());
    ui->comboBoxFileLogLevel->setCurrentIndex(m_settings.getFileLogLevel());
    connect(ui->comboBoxConsoleLogLevel, qOverload<int>(&QComboBox::currentIndexChanged), this, &DlgSettings::comboBoxConsoleLogLevelChanged);
    connect(ui->comboBoxFileLogLevel, qOverload<int>(&QComboBox::currentIndexChanged), this, &DlgSettings::comboBoxFileLogLevelChanged);
    ui->tabWidgetMain->setCurrentIndex(0);
    setupModeWidgets();
    ui->checkBoxDbSkipValidation->setChecked(m_settings.dbSkipValidation());
    ui->checkBoxLazyLoadDurations->setChecked(m_settings.dbLazyLoadDurations());
    ui->checkBoxMonitorDirs->setChecked(m_settings.dbDirectoryWatchEnabled());
    ui->groupBoxShowDuration->setChecked(m_settings.cdgRemainEnabled());
    ui->cbxRotShowNextSong->setChecked(m_settings.rotationShowNextSong());
    ui->checkBoxCdgPrescaling->setChecked(m_settings.cdgPrescalingEnabled());
    ui->checkBoxPitchCue->setChecked(m_settings.cdgPitchCueEnabled());
    ui->checkBoxCheers->setChecked(m_settings.cheersEnabled());
    ui->checkBoxCurrentSingerTop->setChecked(m_settings.rotationAltSortOrder());
    audioOutputDevices = kAudioBackend.getOutputDevices();
    ui->comboBoxKAudioDevices->addItems(audioOutputDevices);
    ui->checkBoxShowAddDlgOnDbDblclk->setChecked(m_settings.dbDoubleClickAddsSong());
    int selDevice = audioOutputDevices.indexOf(m_settings.audioOutputDevice());
    if (selDevice == -1) {
        ui->comboBoxKAudioDevices->setCurrentIndex(0);
    } else {
        ui->comboBoxKAudioDevices->setCurrentIndex(selDevice);
    }
    ui->comboBoxBAudioDevices->addItems(audioOutputDevices);
    selDevice = audioOutputDevices.indexOf(m_settings.audioOutputDeviceBm());
    if (selDevice == -1)
        ui->comboBoxBAudioDevices->setCurrentIndex(0);
    else {
        ui->comboBoxBAudioDevices->setCurrentIndex(selDevice);
    }
    ui->checkBoxProgressiveSearch->setChecked(m_settings.progressiveSearchEnabled());
    ui->horizontalSliderTickerSpeed->setValue(m_settings.tickerSpeed());
    QString ss = ui->pushButtonTextColor->styleSheet();
    QColor clr = m_settings.tickerTextColor();
    ss.replace("0,0,0", QString(QString::number(clr.red()) + "," + QString::number(clr.green()) + "," +
                                QString::number(clr.blue())));
    ui->pushButtonTextColor->setStyleSheet(ss);
    ss = ui->pushButtonBgColor->styleSheet();
    clr = m_settings.tickerBgColor();
    ss.replace("0,0,0", QString(QString::number(clr.red()) + "," + QString::number(clr.green()) + "," +
                                QString::number(clr.blue())));
    ui->pushButtonBgColor->setStyleSheet(ss);
    ss = ui->btnAlertTxtColor->styleSheet();
    clr = m_settings.alertTxtColor();
    ss.replace("0,0,0", QString(QString::number(clr.red()) + "," + QString::number(clr.green()) + "," +
                                QString::number(clr.blue())));
    ui->btnAlertTxtColor->setStyleSheet(ss);
    ss = ui->btnAlertBgColor->styleSheet();
    clr = m_settings.alertBgColor();
    ss.replace("0,0,0", QString(QString::number(clr.red()) + "," + QString::number(clr.green()) + "," +
                                QString::number(clr.blue())));
    ui->btnAlertBgColor->setStyleSheet(ss);

    ss = ui->btnDurationFontColor->styleSheet();
    clr = m_settings.cdgRemainTextColor();
    ss.replace("0,0,0", QString(QString::number(clr.red()) + "," + QString::number(clr.green()) + "," +
                                QString::number(clr.blue())));
    ui->btnDurationFontColor->setStyleSheet(ss);

    ss = ui->btnDurationBgColor->styleSheet();
    clr = m_settings.cdgRemainBgColor();
    ss.replace("0,0,0", QString(QString::number(clr.red()) + "," + QString::number(clr.green()) + "," +
                                QString::number(clr.blue())));
    ui->btnDurationBgColor->setStyleSheet(ss);

    if (m_settings.tickerFullRotation()) {
        ui->radioButtonFullRotation->setChecked(true);
        ui->spinBoxTickerSingers->setEnabled(false);
    } else {
        ui->radioButtonPartialRotation->setChecked(true);
        ui->spinBoxTickerSingers->setEnabled(true);
    }
    ui->spinBoxTickerSingers->setValue(m_settings.tickerShowNumSingers());
    ui->groupBoxRequestServer->setChecked(m_settings.requestServerEnabled());
    ui->lineEditUrl->setText(m_settings.requestServerUrl());
    ui->lineEditApiKey->setText(m_settings.requestServerApiKey());
    ui->checkBoxIgnoreCertErrors->setChecked(m_settings.requestServerIgnoreCertErrors());
    if ((m_settings.bgMode() == m_settings.BG_MODE_IMAGE) || (m_settings.bgSlideShowDir() == ""))
        ui->rbBgImage->setChecked(true);
    else
        ui->rbSlideshow->setChecked(true);
    ui->lineEditCdgBackground->setText(m_settings.cdgDisplayBackgroundImage());
    ui->lineEditSlideshowDir->setText(m_settings.bgSlideShowDir());
    ui->checkBoxFader->setChecked(m_settings.audioUseFader());
    ui->checkBoxDownmix->setChecked(m_settings.audioDownmix());
    ui->checkBoxNormalizeLoudness->setChecked(m_settings.karaokeNormalizeLoudness());
    ui->checkBoxSilenceDetection->setChecked(m_settings.audioDetectSilence());
    ui->checkBoxFaderBm->setChecked(m_settings.audioUseFaderBm());
    ui->checkBoxDownmixBm->setChecked(m_settings.audioDownmixBm());
    ui->checkBoxSilenceDetectionBm->setChecked(m_settings.audioDetectSilenceBm());
    ui->spinBoxInterval->setValue(m_settings.requestServerInterval());
    ui->spinBoxSystemId->setMaximum(songbookApi.entitledSystemCount());
    ui->spinBoxSystemId->setValue(m_settings.systemId());
    ui->spinBoxCdgOffsetTop->setValue(m_settings.cdgOffsetTop());
    ui->spinBoxCdgOffsetBottom->setValue(m_settings.cdgOffsetBottom());
    ui->spinBoxCdgOffsetLeft->setValue(m_settings.cdgOffsetLeft());
    ui->spinBoxCdgOffsetRight->setValue(m_settings.cdgOffsetRight());
    ui->spinBoxSlideshowInterval->setValue(m_settings.slideShowInterval());

    AudioRecorder recorder;
    QStringList inputs = recorder.getDeviceList();
    QStringList codecs = recorder.getCodecs();
    ui->groupBoxRecording->setChecked(m_settings.recordingEnabled());
    ui->comboBoxDevice->addItems(inputs);
    ui->comboBoxCodec->addItems(codecs);
    QString recordingInput = m_settings.recordingInput();
    if (recordingInput == "undefined")
        ui->comboBoxDevice->setCurrentIndex(0);
    else
        ui->comboBoxDevice->setCurrentIndex(ui->comboBoxDevice->findText(m_settings.recordingInput()));
    QString recordingCodec = m_settings.recordingCodec();
    if (recordingCodec == "undefined")
        ui->comboBoxCodec->setCurrentIndex(1);
    else
        ui->comboBoxCodec->setCurrentIndex(ui->comboBoxCodec->findText(m_settings.recordingCodec()));
    ui->comboBoxUpdateBranch->addItem("Stable");
    ui->comboBoxUpdateBranch->addItem("Development");
    ui->cbxTheme->addItem("OS Native");
    ui->cbxTheme->addItem("Fusion Dark");
    ui->cbxTheme->addItem("Fusion Light");
    ui->cbxTheme->setCurrentIndex(m_settings.theme());
    ui->lineEditOutputDir->setText(m_settings.recordingOutputDir());
    ui->cbxTickerShowRotationInfo->setChecked(m_settings.tickerShowRotationInfo());
    ui->groupBoxTicker->setChecked(m_settings.tickerEnabled());
    ui->lineEditTickerMessage->setText(m_settings.tickerCustomString());
    ui->fontComboBox->setFont(m_settings.applicationFont());
    ui->spinBoxAppFontSize->setValue(m_settings.applicationFont().pointSize());
    ui->checkBoxIncludeEmptySingers->setChecked(!m_settings.estimationSkipEmptySingers());
    ui->spinBoxDefaultPadTime->setValue(m_settings.estimationSingerPad());
    ui->spinBoxDefaultSongDuration->setValue(m_settings.estimationEmptySongLength());
    ui->checkBoxDisplayCurrentRotationPosition->setChecked(m_settings.rotationDisplayPosition());
    ui->spinBoxAADelay->setValue(m_settings.karaokeAATimeout());
    ui->checkBoxKAA->setChecked(m_settings.karaokeAutoAdvance());
    ui->checkBoxAutoPlayFirst->setChecked(m_settings.karaokeAutoPlayFirstSong());
    ui->checkBoxShowKAAAlert->setChecked(m_settings.karaokeAAAlertEnabled());
    ui->cbxQueueRemovalWarning->setChecked(m_settings.showQueueRemovalWarning());
    ui->cbxSingerRemovalWarning->setChecked(m_settings.showSingerRemovalWarning());
    ui->cbxSongInterruptionWarning->setChecked(m_settings.showSongInterruptionWarning());
    ui->cbxBmAutostart->setChecked(m_settings.bmAutoStart());
    ui->cbxIgnoreApos->setChecked(m_settings.ignoreAposInSearch());
    ui->spinBoxVideoOffset->setValue(m_settings.videoOffsetMs());
    ui->cbxStopPauseWarning->setChecked(m_settings.showSongPauseStopWarning());
    ui->cbxCheckUpdates->setChecked(m_settings.checkUpdates());
    ui->comboBoxUpdateBranch->setCurrentIndex(m_settings.updatesBranch());
    ui->lineEditDownloadsDir->setText(m_settings.storeDownloadDir());
    ui->lineEditLogDir->setText(m_settings.logDir());
    ui->checkBoxEnforceAspectRatio->setChecked(m_settings.enforceAspectRatio());
    ui->checkBoxTreatAllSingersAsRegs->setChecked(m_settings.treatAllSingersAsRegs());
    ui->cbxCrossFade->setChecked(m_settings.bmKCrossFade());
    tickerFontChanged();
    tickerBgColorChanged();
    tickerTextColorChanged();
    tickerOutputModeChanged();
    tickerCustomStringChanged();
    tickerSpeedChanged();
    connect(ui->checkBoxTreatAllSingersAsRegs, &QCheckBox::toggled, this, &DlgSettings::treatAllSingersAsRegsChanged);
    connect(ui->checkBoxEnforceAspectRatio, &QCheckBox::toggled, this, &DlgSettings::enforceAspectRatioChanged);
    connect(ui->spinBoxCdgOffsetTop, qOverload<int>(&QSpinBox::valueChanged), &m_settings, &Settings::setCdgOffsetTop);
    connect(ui->spinBoxCdgOffsetTop, qOverload<int>(&QSpinBox::valueChanged), this, &DlgSettings::cdgOffsetsChanged);
    connect(ui->spinBoxCdgOffsetBottom, qOverload<int>(&QSpinBox::valueChanged), &m_settings,
            &Settings::setCdgOffsetBottom);
    connect(ui->spinBoxCdgOffsetBottom, qOverload<int>(&QSpinBox::valueChanged), this, &DlgSettings::cdgOffsetsChanged);
    connect(ui->spinBoxCdgOffsetLeft, qOverload<int>(&QSpinBox::valueChanged), &m_settings,
            &Settings::setCdgOffsetLeft);
    connect(ui->spinBoxCdgOffsetLeft, qOverload<int>(&QSpinBox::valueChanged), this, &DlgSettings::cdgOffsetsChanged);
    connect(ui->spinBoxCdgOffsetRight, qOverload<int>(&QSpinBox::valueChanged), &m_settings,
            &Settings::setCdgOffsetRight);
    connect(ui->spinBoxCdgOffsetRight, qOverload<int>(&QSpinBox::valueChanged), this, &DlgSettings::cdgOffsetsChanged);
    connect(ui->spinBoxVideoOffset, qOverload<int>(&QSpinBox::valueChanged), &m_settings, &Settings::setVideoOffsetMs);
    connect(ui->spinBoxVideoOffset, qOverload<int>(&QSpinBox::valueChanged), this, &DlgSettings::videoOffsetChanged);
    connect(ui->spinBoxSlideshowInterval, qOverload<int>(&QSpinBox::valueChanged), &m_settings,
            &Settings::setSlideShowInterval);
    connect(ui->spinBoxSlideshowInterval, qOverload<int>(&QSpinBox::valueChanged), this,
            &DlgSettings::slideShowIntervalChanged);
    connect(ui->checkBoxKAA, &QCheckBox::toggled, this, &DlgSettings::karaokeAutoAdvanceChanged);
    connect(&m_settings, &Settings::showQueueRemovalWarningChanged, ui->cbxQueueRemovalWarning, &QCheckBox::setChecked);
    connect(&m_settings, &Settings::showSingerRemovalWarningChanged, ui->cbxSingerRemovalWarning,
            &QCheckBox::setChecked);
    connect(&m_settings, &Settings::showSongInterruptionWarningChanged, ui->cbxSongInterruptionWarning,
            &QCheckBox::setChecked);
    connect(&m_settings, &Settings::showSongStopPauseWarningChanged, ui->cbxStopPauseWarning, &QCheckBox::setChecked);
    connect(ui->cbxIgnoreApos, &QCheckBox::toggled, &m_settings, &Settings::setIgnoreAposInSearch);
    connect(ui->cbxCrossFade, &QCheckBox::toggled, &m_settings, &Settings::setBmKCrossfade);
    connect(ui->cbxCheckUpdates, &QCheckBox::toggled, &m_settings, &Settings::setCheckUpdates);
    connect(ui->comboBoxUpdateBranch, qOverload<int>(&QComboBox::currentIndexChanged), &m_settings,
            &Settings::setUpdatesBranch);
    connect(ui->checkBoxDbSkipValidation, &QCheckBox::toggled, &m_settings, &Settings::dbSetSkipValidation);
    connect(ui->checkBoxLazyLoadDurations, &QCheckBox::toggled, &m_settings, &Settings::dbSetLazyLoadDurations);
    connect(ui->checkBoxMonitorDirs, &QCheckBox::toggled, &m_settings, &Settings::dbSetDirectoryWatchEnabled);
    connect(ui->spinBoxSystemId, qOverload<int>(&QSpinBox::valueChanged), &m_settings, &Settings::setSystemId);
    connect(ui->checkBoxTreatAllSingersAsRegs, &QAbstractButton::toggled, &m_settings,
            &Settings::setTreatAllSingersAsRegs);
    connect(ui->checkBoxShowAddDlgOnDbDblclk, &QCheckBox::stateChanged, [&](auto state) {
        if (state == 0)
            m_settings.setDbDoubleClickAddsSong(false);
        else
            m_settings.setDbDoubleClickAddsSong(true);
    });
    connect(networkManager, &QNetworkAccessManager::finished, this, &DlgSettings::onNetworkReply);
    connect(networkManager, &QNetworkAccessManager::sslErrors, this, &DlgSettings::onSslErrors);
    connect(ui->cbxQueueRemovalWarning, &QCheckBox::toggled, &m_settings, &Settings::setShowQueueRemovalWarning);
    connect(ui->cbxSingerRemovalWarning, &QCheckBox::toggled, &m_settings, &Settings::setShowSingerRemovalWarning);
    connect(ui->cbxSongInterruptionWarning, &QCheckBox::toggled, &m_settings,
            &Settings::setShowSongInterruptionWarning);
    connect(ui->cbxStopPauseWarning, &QCheckBox::toggled, &m_settings, &Settings::setShowSongPauseStopWarning);
    connect(ui->cbxTickerShowRotationInfo, &QCheckBox::toggled, &m_settings, &Settings::setTickerShowRotationInfo);
    connect(ui->cbxTickerShowRotationInfo, &QCheckBox::toggled, this, &DlgSettings::tickerOutputModeChanged);
    connect(&songbookApi, &OKJSongbookAPI::entitledSystemCountChanged, this, &DlgSettings::entitledSystemCountChanged);
    connect(ui->cbxRotShowNextSong, &QCheckBox::toggled, &m_settings, &Settings::setRotationShowNextSong);
    connect(ui->cbxRotShowNextSong, &QCheckBox::toggled, this, &DlgSettings::rotationShowNextSongChanged);
    setupHotkeysForm();
    refreshNetworkModeUi();
    makeTabsScrollable();
    if (!restoredGeometry) {
        adjustSize();
        resize(size().expandedTo(designedSize));
    }
    clampToScreen();
    m_pageSetupDone = true;
}

void DlgSettings::makeTabsScrollable() {
    // Settings keep getting added and a page that outgrows the screen has no way
    // to reach its bottom half, so every page scrolls. This also keeps the pages
    // usable at the larger application font sizes the Display tab offers.
    for (int page = 0; page < ui->tabWidgetMain->count(); page++) {
        auto *tab = ui->tabWidgetMain->widget(page);
        auto *tabLayout = tab->layout();
        if (tabLayout == nullptr)
            continue;
        // Pages drawn around a scroll area of their own are already covered.
        if (tabLayout->count() == 1 && qobject_cast<QScrollArea *>(tabLayout->itemAt(0)->widget()) != nullptr)
            continue;

        // Adopting the layout carries the widgets it holds across with it.
        auto *contents = new QWidget;
        contents->setLayout(tabLayout);

        auto *scrollArea = new QScrollArea(tab);
        scrollArea->setWidgetResizable(true);
        // The pane the tab widget draws is border enough.
        scrollArea->setFrameShape(QFrame::NoFrame);
        scrollArea->setWidget(contents);

        auto *host = new QVBoxLayout(tab);
        host->setContentsMargins(0, 0, 0, 0);
        host->addWidget(scrollArea);
    }
}

void DlgSettings::clampToScreen() {
    const QScreen *screen = QGuiApplication::screenAt(frameGeometry().center());
    if (screen == nullptr)
        screen = QGuiApplication::primaryScreen();
    if (screen == nullptr)
        return;

    // On Windows nothing stops adjustSize() - or a geometry saved on a bigger
    // monitor - from handing back a window taller than the display it is on,
    // which puts the buttons along the bottom out of reach. Room is left for the
    // window frame so the title bar and bottom edge stay grabbable.
    const QRect available = screen->availableGeometry().adjusted(0, 0, -40, -60);
    // Only the size is touched: restoreGeometry() already drags a window saved
    // off the edge of a monitor that is no longer there back into view, and a
    // dialog that has never been placed has to be left alone for Qt to centre it
    // over the main window.
    const QSize bounded = size().boundedTo(available.size());
    if (bounded != size())
        resize(bounded);
}

DlgSettings::~DlgSettings() {
    delete ui;
}

QStringList DlgSettings::getMonitors() {
    QStringList screenStrings;
    for (int i = 0; i < QGuiApplication::screens().size(); i++) {
        auto sWidth = QGuiApplication::screens().at(i)->size().width();
        auto sHeight = QGuiApplication::screens().at(i)->size().height();
        screenStrings
                << "Monitor " + QString::number(i) + " - " + QString::number(sWidth) + "x" + QString::number(sHeight);
    }
    return screenStrings;
}

void DlgSettings::setupModeWidgets()
{
    auto *networkLayout = qobject_cast<QVBoxLayout *>(ui->tabWidgetPage3->layout());
    if (!networkLayout) {
        return;
    }

    auto *modeBox = new QGroupBox(tr("Operating Mode"), ui->tabWidgetPage3);
    auto *modeLayout = new QFormLayout(modeBox);
    m_comboAppMode = new QComboBox(modeBox);
    m_comboAppMode->addItem("Classic", Settings::ClassicMode);
    m_comboAppMode->addItem("Local Mode", Settings::LocalMode);
    m_comboAppMode->setCurrentIndex(m_settings.appMode() == Settings::LocalMode ? 1 : 0);
    modeLayout->addRow(tr("Mode"), m_comboAppMode);
    networkLayout->insertWidget(0, modeBox);

    m_groupBoxLocalMode = new QGroupBox(tr("Local Mode"), ui->tabWidgetPage3);
    auto *localLayout = new QFormLayout(m_groupBoxLocalMode);
    m_lineEditLocalUiUrl = new QLineEdit(m_settings.localUiUrl(), m_groupBoxLocalMode);
    m_lineEditEmbeddedBindAddress = new QLineEdit(m_settings.embeddedApiBindAddress(), m_groupBoxLocalMode);
    m_spinBoxEmbeddedPort = new QSpinBox(m_groupBoxLocalMode);
    m_spinBoxEmbeddedPort->setRange(1, 65535);
    m_spinBoxEmbeddedPort->setValue(m_settings.embeddedApiPort());
    m_spinBoxUpNextTurns = new QSpinBox(m_groupBoxLocalMode);
    m_spinBoxUpNextTurns->setRange(0, 5);
    m_spinBoxUpNextTurns->setValue(m_settings.embeddedApiUpNextTurns());
    m_spinBoxUpNextTurns->setSpecialValueText(tr("Off"));
    m_spinBoxUpNextTurns->setSuffix(tr(" turn(s) away"));
    m_spinBoxUpNextTurns->setToolTip(tr("Push an alert to a singer's phone when they get this close to the mic."));
    localLayout->addRow(tr("Karaoke UI URL"), m_lineEditLocalUiUrl);
    localLayout->addRow(tr("Embedded API bind"), m_lineEditEmbeddedBindAddress);
    localLayout->addRow(tr("Embedded API port"), m_spinBoxEmbeddedPort);
    localLayout->addRow(tr("Up next alert"), m_spinBoxUpNextTurns);
    networkLayout->insertWidget(1, m_groupBoxLocalMode);

    connect(m_comboAppMode, qOverload<int>(&QComboBox::currentIndexChanged), this, [&](int index) {
        const auto mode = static_cast<Settings::AppMode>(m_comboAppMode->itemData(index).toInt());
        m_settings.setAppMode(mode);
        ui->groupBoxRequestServer->setChecked(m_settings.requestServerEnabled());
        ui->lineEditUrl->setText(m_settings.requestServerUrl());
        ui->lineEditApiKey->setText(m_settings.requestServerApiKey());
        ui->checkBoxIgnoreCertErrors->setChecked(m_settings.requestServerIgnoreCertErrors());
        ui->spinBoxSystemId->setValue(m_settings.systemId());
        ui->spinBoxInterval->setValue(m_settings.requestServerInterval());
        m_lineEditLocalUiUrl->setText(m_settings.localUiUrl());
        m_lineEditEmbeddedBindAddress->setText(m_settings.embeddedApiBindAddress());
        m_spinBoxEmbeddedPort->setValue(m_settings.embeddedApiPort());
        m_spinBoxUpNextTurns->setValue(m_settings.embeddedApiUpNextTurns());
        refreshNetworkModeUi();
        emit appModeChanged(mode);
    });
    connect(m_lineEditLocalUiUrl, &QLineEdit::editingFinished, this, [&]() {
        m_settings.setLocalUiUrl(m_lineEditLocalUiUrl->text().trimmed());
    });
    connect(m_lineEditEmbeddedBindAddress, &QLineEdit::editingFinished, this, [&]() {
        m_settings.setEmbeddedApiBindAddress(m_lineEditEmbeddedBindAddress->text().trimmed());
    });
    connect(m_spinBoxEmbeddedPort, qOverload<int>(&QSpinBox::valueChanged), this, [&](int port) {
        m_settings.setEmbeddedApiPort(port);
    });
    connect(m_spinBoxUpNextTurns, qOverload<int>(&QSpinBox::valueChanged), this, [&](int turns) {
        m_settings.setEmbeddedApiUpNextTurns(turns);
    });

    setupYoutubeWidgets(networkLayout);
}

void DlgSettings::setupYoutubeWidgets(QVBoxLayout *networkLayout)
{
    m_groupBoxYoutube = new QGroupBox(tr("YouTube requests"), ui->tabWidgetPage3);
    m_groupBoxYoutube->setCheckable(true);
    m_groupBoxYoutube->setChecked(m_settings.youtubeRequestsEnabled());
    m_groupBoxYoutube->setToolTip(tr("Lets singers search YouTube from their phone and queue a video.\n"
                                     "Requires yt-dlp, which downloads the video to this machine before it plays."));
    auto *ytLayout = new QFormLayout(m_groupBoxYoutube);

    m_lineEditDlpPath = new QLineEdit(m_settings.youtubeDlpPath(), m_groupBoxYoutube);
    auto *dlpRow = new QHBoxLayout;
    auto *dlpBrowse = new QPushButton(tr("Browse..."), m_groupBoxYoutube);
    dlpRow->addWidget(m_lineEditDlpPath);
    dlpRow->addWidget(dlpBrowse);
    ytLayout->addRow(tr("yt-dlp path"), dlpRow);

    m_labelDlpStatus = new QLabel(m_groupBoxYoutube);
    m_labelDlpStatus->setWordWrap(true);
    m_buttonDlpUpdate = new QPushButton(tr("Update yt-dlp now"), m_groupBoxYoutube);
    auto *statusRow = new QHBoxLayout;
    statusRow->addWidget(m_labelDlpStatus, 1);
    statusRow->addWidget(m_buttonDlpUpdate);
    ytLayout->addRow(tr("Status"), statusRow);

    m_comboDlpChannel = new QComboBox(m_groupBoxYoutube);
    m_comboDlpChannel->addItem(tr("Nightly (recommended)"), "nightly");
    m_comboDlpChannel->addItem(tr("Stable"), "stable");
    m_comboDlpChannel->setCurrentIndex(m_settings.youtubeDlpChannel() == "stable" ? 1 : 0);
    m_comboDlpChannel->setToolTip(tr("YouTube extractor fixes reach nightly days before stable.\n"
                                     "A broken extractor mid-show is the failure worth avoiding."));
    ytLayout->addRow(tr("Update channel"), m_comboDlpChannel);

    m_lineEditYtCacheDir = new QLineEdit(m_settings.youtubeCacheDir(), m_groupBoxYoutube);
    auto *cacheRow = new QHBoxLayout;
    auto *cacheBrowse = new QPushButton(tr("Browse..."), m_groupBoxYoutube);
    cacheRow->addWidget(m_lineEditYtCacheDir);
    cacheRow->addWidget(cacheBrowse);
    ytLayout->addRow(tr("Cache folder"), cacheRow);

    m_labelYtCacheWarning = new QLabel(m_groupBoxYoutube);
    m_labelYtCacheWarning->setWordWrap(true);
    m_labelYtCacheWarning->setStyleSheet("color: firebrick;");
    m_labelYtCacheWarning->setVisible(false);
    ytLayout->addRow(QString(), m_labelYtCacheWarning);

    m_spinBoxYtMaxDuration = new QSpinBox(m_groupBoxYoutube);
    m_spinBoxYtMaxDuration->setRange(1, 120);
    m_spinBoxYtMaxDuration->setValue(m_settings.youtubeMaxDurationSecs() / 60);
    m_spinBoxYtMaxDuration->setSuffix(tr(" minutes"));
    m_spinBoxYtMaxDuration->setToolTip(tr("Requests longer than this are refused, which keeps hour-long\n"
                                          "compilation uploads out of the rotation."));
    ytLayout->addRow(tr("Longest video"), m_spinBoxYtMaxDuration);

    m_spinBoxYtMaxHeight = new QSpinBox(m_groupBoxYoutube);
    m_spinBoxYtMaxHeight->setRange(360, 2160);
    m_spinBoxYtMaxHeight->setSingleStep(360);
    m_spinBoxYtMaxHeight->setValue(m_settings.youtubeMaxHeight());
    m_spinBoxYtMaxHeight->setSuffix(tr("p"));
    ytLayout->addRow(tr("Max quality"), m_spinBoxYtMaxHeight);

    m_spinBoxYtCacheMaxGb = new QSpinBox(m_groupBoxYoutube);
    m_spinBoxYtCacheMaxGb->setRange(1, 1000);
    m_spinBoxYtCacheMaxGb->setValue(m_settings.youtubeCacheMaxGb());
    m_spinBoxYtCacheMaxGb->setSuffix(tr(" GB"));
    ytLayout->addRow(tr("Cache size limit"), m_spinBoxYtCacheMaxGb);

    m_spinBoxYtKeepDays = new QSpinBox(m_groupBoxYoutube);
    m_spinBoxYtKeepDays->setRange(1, 3650);
    m_spinBoxYtKeepDays->setValue(m_settings.youtubeCacheKeepDays());
    m_spinBoxYtKeepDays->setSuffix(tr(" days"));
    m_spinBoxYtKeepDays->setToolTip(tr("One-off requests are dropped after this long.\n"
                                       "Videos that get sung again are kept."));
    ytLayout->addRow(tr("Keep unused videos"), m_spinBoxYtKeepDays);

    m_spinBoxYtConcurrent = new QSpinBox(m_groupBoxYoutube);
    m_spinBoxYtConcurrent->setRange(1, 4);
    m_spinBoxYtConcurrent->setValue(m_settings.youtubeMaxConcurrentFetches());
    m_spinBoxYtConcurrent->setToolTip(tr("Downloads running at once. Each one competes for the venue's\n"
                                         "connection while the show is running."));
    ytLayout->addRow(tr("Simultaneous downloads"), m_spinBoxYtConcurrent);

    m_checkBoxYtSearchable = new QCheckBox(tr("Let singers find downloaded videos in song search"),
                                           m_groupBoxYoutube);
    m_checkBoxYtSearchable->setChecked(m_settings.youtubeCachedSearchable());
    m_checkBoxYtSearchable->setToolTip(tr("Downloaded videos appear alongside your library, so the next singer who\n"
                                          "wants one gets it with no wait and the collection grows itself.\n"
                                          "Videos still downloading are never listed."));
    ytLayout->addRow(QString(), m_checkBoxYtSearchable);

    m_checkBoxYtDropOnEarlySkip = new QCheckBox(tr("Delete a video the singer skips in the first 30 seconds"),
                                                m_groupBoxYoutube);
    m_checkBoxYtDropOnEarlySkip->setChecked(m_settings.youtubeDropOnEarlySkip());
    m_checkBoxYtDropOnEarlySkip->setToolTip(tr("Someone who ends their own song from their phone seconds after it\n"
                                               "starts has almost always been given the wrong video - a lyric video,\n"
                                               "a cover, no karaoke track. Only applies to a video's first play, and\n"
                                               "only to videos nobody else has queued. Requesting it again fetches\n"
                                               "it back."));
    ytLayout->addRow(QString(), m_checkBoxYtDropOnEarlySkip);

    auto *manageButton = new QPushButton(tr("Manage downloaded videos..."), m_groupBoxYoutube);
    ytLayout->addRow(QString(), manageButton);

    networkLayout->insertWidget(2, m_groupBoxYoutube);

    connect(manageButton, &QPushButton::clicked, this, [this]() {
        if (!m_youtubeFetcher)
            return;
        DlgYoutubeCache dlg(*m_youtubeFetcher, m_settings, this);
        dlg.exec();
    });

    connect(m_groupBoxYoutube, &QGroupBox::toggled, this, [this](const bool enabled) {
        m_settings.setYoutubeRequestsEnabled(enabled);
        // start() reads the setting and either spins the fetcher up - probe, resume
        // anything left pending, arm the timeout sweep - or shuts it back down.
        if (m_youtubeFetcher)
            m_youtubeFetcher->start();
        refreshDlpStatusLabel();
    });
    connect(dlpBrowse, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(this, tr("Select yt-dlp"),
                                                          QFileInfo(m_lineEditDlpPath->text()).absolutePath());
        if (path.isEmpty())
            return;
        m_lineEditDlpPath->setText(path);
        m_settings.setYoutubeDlpPath(path);
        if (m_youtubeFetcher)
            m_youtubeFetcher->refreshAvailability();
    });
    connect(m_lineEditDlpPath, &QLineEdit::editingFinished, this, [this]() {
        m_settings.setYoutubeDlpPath(m_lineEditDlpPath->text().trimmed());
        if (m_youtubeFetcher)
            m_youtubeFetcher->refreshAvailability();
    });
    connect(m_buttonDlpUpdate, &QPushButton::clicked, this, [this]() {
        if (!m_youtubeFetcher)
            return;
        m_buttonDlpUpdate->setEnabled(false);
        m_labelDlpStatus->setText(tr("Updating..."));
        m_youtubeFetcher->updateNow();
    });
    connect(m_comboDlpChannel, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](const int index) {
        m_settings.setYoutubeDlpChannel(m_comboDlpChannel->itemData(index).toString());
    });
    connect(cacheBrowse, &QPushButton::clicked, this, [this]() {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Select cache folder"),
                                                              m_lineEditYtCacheDir->text());
        if (dir.isEmpty())
            return;
        m_lineEditYtCacheDir->setText(dir);
        m_settings.setYoutubeCacheDir(dir);
        refreshYtCacheWarning();
    });
    connect(m_lineEditYtCacheDir, &QLineEdit::editingFinished, this, [this]() {
        m_settings.setYoutubeCacheDir(m_lineEditYtCacheDir->text().trimmed());
        refreshYtCacheWarning();
    });
    connect(m_spinBoxYtMaxDuration, qOverload<int>(&QSpinBox::valueChanged), this, [this](const int minutes) {
        m_settings.setYoutubeMaxDurationSecs(minutes * 60);
    });
    connect(m_spinBoxYtMaxHeight, qOverload<int>(&QSpinBox::valueChanged), this, [this](const int height) {
        m_settings.setYoutubeMaxHeight(height);
    });
    connect(m_spinBoxYtCacheMaxGb, qOverload<int>(&QSpinBox::valueChanged), this, [this](const int gb) {
        m_settings.setYoutubeCacheMaxGb(gb);
    });
    connect(m_spinBoxYtKeepDays, qOverload<int>(&QSpinBox::valueChanged), this, [this](const int days) {
        m_settings.setYoutubeCacheKeepDays(days);
    });
    connect(m_spinBoxYtConcurrent, qOverload<int>(&QSpinBox::valueChanged), this, [this](const int count) {
        m_settings.setYoutubeMaxConcurrentFetches(count);
    });
    connect(m_checkBoxYtSearchable, &QCheckBox::toggled, this, [this](const bool searchable) {
        m_settings.setYoutubeCachedSearchable(searchable);
    });
    connect(m_checkBoxYtDropOnEarlySkip, &QCheckBox::toggled, this, [this](const bool drop) {
        m_settings.setYoutubeDropOnEarlySkip(drop);
    });

    refreshDlpStatusLabel();
    refreshYtCacheWarning();
}

void DlgSettings::setYoutubeFetcher(YoutubeFetcher *fetcher)
{
    m_youtubeFetcher = fetcher;
    if (!m_youtubeFetcher)
        return;
    connect(m_youtubeFetcher, &YoutubeFetcher::availabilityChanged, this, [this](bool) {
        refreshDlpStatusLabel();
    });
    connect(m_youtubeFetcher, &YoutubeFetcher::updateFinished, this, [this](const bool ok, const QString &message) {
        if (m_buttonDlpUpdate)
            m_buttonDlpUpdate->setEnabled(true);
        refreshDlpStatusLabel();
        if (!ok && isVisible())
            QMessageBox::warning(this, tr("yt-dlp update failed"), message);
    });
    refreshDlpStatusLabel();
}

void DlgSettings::refreshDlpStatusLabel()
{
    if (!m_labelDlpStatus)
        return;
    if (m_buttonDlpUpdate)
        m_buttonDlpUpdate->setEnabled(m_youtubeFetcher != nullptr);

    if (!m_youtubeFetcher) {
        m_labelDlpStatus->setText(tr("Unavailable in this mode."));
        return;
    }
    if (!QFileInfo::exists(m_settings.youtubeDlpPath())) {
        m_labelDlpStatus->setText(tr("<b>Not found.</b> Download yt-dlp and point this at it."));
        return;
    }
    if (!m_youtubeFetcher->isAvailable()) {
        m_labelDlpStatus->setText(tr("<b>Found but not responding.</b> Requests are refused until it works."));
        return;
    }
    m_labelDlpStatus->setText(tr("Ready - version %1").arg(m_youtubeFetcher->version()));
}

void DlgSettings::refreshYtCacheWarning()
{
    if (!m_labelYtCacheWarning)
        return;

    const QString cacheDir = QDir(m_settings.youtubeCacheDir()).absolutePath();
    QSqlQuery query;
    QString clash;
    if (query.exec("SELECT path FROM sourceDirs")) {
        while (query.next()) {
            const QString sourceDir = QDir(query.value(0).toString()).absolutePath();
            if (sourceDir.isEmpty())
                continue;
            // Case-insensitive throughout: on Windows these are the same folder, and a
            // warning that depends on how the KJ typed the drive letter is worse than
            // no warning at all. The trailing separator is what keeps a sibling folder
            // like "LibraryOther" from matching "Library".
            if (QString::compare(cacheDir, sourceDir, Qt::CaseInsensitive) == 0 ||
                cacheDir.startsWith(sourceDir + '/', Qt::CaseInsensitive)) {
                clash = sourceDir;
                break;
            }
        }
    }

    if (clash.isEmpty()) {
        m_labelYtCacheWarning->setVisible(false);
        return;
    }
    // A library scan only enumerates songs under source dirs. Put the cache inside
    // one and every downloaded video becomes a scanned library song - and every
    // pending one becomes a "missing file" the KJ gets offered the chance to delete.
    m_labelYtCacheWarning->setText(tr("This folder is inside the library source folder %1. "
                                      "Move it somewhere outside your library, or a database scan will "
                                      "treat cached videos as library songs and offer to delete pending ones.")
                                           .arg(clash));
    m_labelYtCacheWarning->setVisible(true);
}

void DlgSettings::refreshNetworkModeUi()
{
    const bool classicMode = m_settings.appMode() == Settings::ClassicMode;
    ui->groupBoxRequestServer->setTitle(classicMode ? tr("Classic requests") : tr("Classic compatibility"));
    ui->label_6->setText(classicMode ? tr("Server URL") : tr("Classic server URL"));
    ui->label_7->setText(classicMode ? tr("API key") : tr("Classic API key"));
    ui->label_30->setText(classicMode ? tr("System ID") : tr("Classic system ID"));
    ui->label_22->setText(classicMode ? tr("Refresh interval") : tr("Classic refresh interval"));
    ui->lineEditUrl->setPlaceholderText(classicMode ? tr("https://api.okjsongbook.com/api.php")
                                                    : tr("Classic mode server endpoint"));
    ui->groupBoxRequestServer->setEnabled(classicMode);
    ui->spinBoxSystemId->setEnabled(classicMode);
    ui->spinBoxInterval->setEnabled(classicMode);
    if (m_groupBoxLocalMode) {
        m_groupBoxLocalMode->setVisible(true);
        m_groupBoxLocalMode->setEnabled(!classicMode);
    }
    if (m_groupBoxYoutube) {
        // Requests only reach this build through the embedded API, which Classic mode
        // does not run.
        m_groupBoxYoutube->setEnabled(!classicMode);
        refreshDlpStatusLabel();
    }
}

void DlgSettings::setupHotkeysForm() {
    std::vector<KeyboardShortcut> shortcuts;
    shortcuts.emplace_back(KeyboardShortcut{
            "Karaoke - Volume up",
            "kVolUp"
    });
    shortcuts.emplace_back(KeyboardShortcut{
            "Karaoke - Volume down",
            "kVolDn"
    });
    shortcuts.emplace_back(KeyboardShortcut{
            "Karaoke - Volume mute/unmute",
            "kVolMute"
    });
    shortcuts.emplace_back(KeyboardShortcut{
            "Karaoke - Pause/unpause",
            "kPause"
    });
    shortcuts.emplace_back(KeyboardShortcut{
            "Karaoke - Stop",
            "kStop"
    });
    shortcuts.emplace_back(KeyboardShortcut{
            "Karaoke - Jump back (5s)",
            "kRwnd"
    });
    shortcuts.emplace_back(KeyboardShortcut{
            "Karaoke - Jump forward (5s)",
            "kFfwd"
    });
    shortcuts.emplace_back(KeyboardShortcut{
            "Karaoke - Restart track",
            "kRestartSong"
    });
    shortcuts.emplace_back(KeyboardShortcut{
            "Karaoke - Play next unsung queued karaoke song",
            "kPlayNextUnsung"
    });
    shortcuts.emplace_back(KeyboardShortcut{
            "Karaoke - Select next singer w/ unsung queued songs",
            "kSelectNextSinger"
    });
    shortcuts.emplace_back(KeyboardShortcut{
            "Break Music - Volume up",
            "bVolUp"
    });
    shortcuts.emplace_back(KeyboardShortcut{
            "Break Music - Volume down",
            "bVolDn"
    });
    shortcuts.emplace_back(KeyboardShortcut{
            "Break Music - Volume mute/unmute",
            "bVolMute"
    });
    shortcuts.emplace_back(KeyboardShortcut{
            "Break Music - Pause/unpause",
            "bPause"
    });
    shortcuts.emplace_back(KeyboardShortcut{
            "Break Music - Stop",
            "bStop"
    });
    shortcuts.emplace_back(KeyboardShortcut{
            "Break Music - Jump back (5s)",
            "bRwnd"
    });
    shortcuts.emplace_back(KeyboardShortcut{
            "Break Music - Jump forward (5s)",
            "bFfwd"
    });
    shortcuts.emplace_back(KeyboardShortcut{
            "Break Music - Restart track",
            "bRestartSong"
    });
    shortcuts.emplace_back(KeyboardShortcut{
            "Toggle singer/video window visibility",
            "toggleSingerWindow"
    });
    shortcuts.emplace_back(KeyboardShortcut{
            "Show add singer dialog",
            "addSinger"
    });
    shortcuts.emplace_back(KeyboardShortcut{
            "Show regular singers dialog",
            "loadRegularSinger"
    });
    shortcuts.emplace_back(KeyboardShortcut{
            "Show the incoming requests dialog (if enabled)",
            "showIncomingRequests"
    });
    shortcuts.emplace_back(KeyboardShortcut{
            "Jump to karaoke search (second press clears search)",
            "jumpToSearch"
    });

    for (auto &shortcut : shortcuts) {
        auto clearButton = new QPushButton(this);
        auto sequenceEdit = new QKeySequenceEdit(this);
        sequenceEdit->setObjectName(shortcut.sequenceName);
        sequenceEdit->setKeySequence(m_settings.loadShortcutKeySequence(shortcut.sequenceName));
        clearButton->setIcon(QIcon(":/theme/Icons/okjbreeze-dark/actions/22/edit-clear.svg"));
        connect(sequenceEdit, &QKeySequenceEdit::keySequenceChanged, this, &DlgSettings::keySequenceEditChanged);
        connect(clearButton, &QPushButton::pressed, sequenceEdit, &QKeySequenceEdit::clear);
        QHBoxLayout *layout = new QHBoxLayout();
        layout->addWidget(sequenceEdit);
        layout->addWidget(clearButton);
        ui->formLayoutHotkeys->addRow(new QLabel(shortcut.description, this), layout);
    }
}

void DlgSettings::keySequenceEditChanged(QKeySequence sequence) {
    m_settings.saveShortcutKeySequence(sender()->objectName(), sequence);
    emit shortcutsChanged();
}


void DlgSettings::onNetworkReply(QNetworkReply *reply) {
    Q_UNUSED(reply);
}

void DlgSettings::onSslErrors(QNetworkReply *reply) {
    if (m_settings.requestServerIgnoreCertErrors())
        reply->ignoreSslErrors();
    else {
        QMessageBox::warning(this, "SSL Error", "Unable to establish secure conneciton with server.");
    }
}


void DlgSettings::on_btnClose_clicked() {
    close();
}

void DlgSettings::on_pushButtonFont_clicked() {
    bool ok;
    QFont font = QFontDialog::getFont(&ok, m_settings.tickerFont(), this, "Select ticker font");
    if (ok) {
        m_settings.setTickerFont(font);
        emit tickerFontChanged();
    }
}

void DlgSettings::on_horizontalSliderTickerSpeed_valueChanged(int value) {
    if (!m_pageSetupDone)
        return;
    m_settings.setTickerSpeed(value);
    emit tickerSpeedChanged();
}

void DlgSettings::on_pushButtonTextColor_clicked() {
    QColor clr = QColorDialog::getColor(m_settings.tickerTextColor(), this, "Select ticker text color");
    if (clr.isValid()) {
        QString ss = ui->pushButtonTextColor->styleSheet();
        QColor oclr = m_settings.tickerTextColor();
        ss.replace(QString(QString::number(oclr.red()) + "," + QString::number(oclr.green()) + "," +
                           QString::number(oclr.blue())),
                   QString(QString::number(clr.red()) + "," + QString::number(clr.green()) + "," +
                           QString::number(clr.blue())));
        ui->pushButtonTextColor->setStyleSheet(ss);
        m_settings.setTickerTextColor(clr);
        emit tickerTextColorChanged();
    }
}

void DlgSettings::on_pushButtonBgColor_clicked() {
    QColor clr = QColorDialog::getColor(m_settings.tickerBgColor(), this, "Select ticker background color");
    if (clr.isValid()) {
        QString ss = ui->pushButtonBgColor->styleSheet();
        QColor oclr = m_settings.tickerBgColor();
        ss.replace(QString(QString::number(oclr.red()) + "," + QString::number(oclr.green()) + "," +
                           QString::number(oclr.blue())),
                   QString(QString::number(clr.red()) + "," + QString::number(clr.green()) + "," +
                           QString::number(clr.blue())));
        ui->pushButtonBgColor->setStyleSheet(ss);
        m_settings.setTickerBgColor(clr);
        emit tickerBgColorChanged();
    }
}

void DlgSettings::on_radioButtonFullRotation_toggled(bool checked) {
    if (!m_pageSetupDone)
        return;
    m_settings.setTickerFullRotation(checked);
    ui->spinBoxTickerSingers->setEnabled(!checked);
    emit tickerOutputModeChanged();
}


void DlgSettings::on_spinBoxTickerSingers_valueChanged(int arg1) {
    if (!m_pageSetupDone)
        return;
    m_settings.setTickerShowNumSingers(arg1);
    emit tickerOutputModeChanged();
}

void DlgSettings::on_groupBoxTicker_toggled(bool arg1) {
    if (!m_pageSetupDone)
        return;
    m_settings.setTickerEnabled(arg1);
    emit tickerEnableChanged();
}

void DlgSettings::on_lineEditUrl_editingFinished() {
    m_settings.setRequestServerUrl(ui->lineEditUrl->text());
}

void DlgSettings::on_checkBoxIgnoreCertErrors_toggled(bool checked) {
    if (!m_pageSetupDone)
        return;
    m_settings.setRequestServerIgnoreCertErrors(checked);
}

void DlgSettings::on_groupBoxRequestServer_toggled(bool arg1) {
    if (!m_pageSetupDone)
        return;
    m_settings.setRequestServerEnabled(arg1);
    emit requestServerEnableChanged(arg1);
}

void DlgSettings::on_pushButtonBrowse_clicked() {
#ifdef Q_OS_LINUX
    QString imageFile = QFileDialog::getOpenFileName(this, QString("Select image file"),
                                                     QStandardPaths::writableLocation(QStandardPaths::PicturesLocation),
                                                     QString("Images (*.png *.jpg *.jpeg *.gif)"), nullptr,
                                                     QFileDialog::DontUseNativeDialog);
#else
    QString imageFile = QFileDialog::getOpenFileName(this, QString("Select image file"),
                                                     QStandardPaths::writableLocation(QStandardPaths::PicturesLocation),
                                                     QString("Images (*.png *.jpg *.jpeg *.gif)"), nullptr);
#endif
    if (imageFile != "") {
        QImage image(imageFile);
        if (!image.isNull()) {
            m_settings.setCdgDisplayBackgroundImage(imageFile);
            emit cdgBgImageChanged();
            ui->lineEditCdgBackground->setText(imageFile);
        } else {
            QMessageBox::warning(this, tr("Image load error"), QString("Unsupported or corrupt image file."));
        }
    }
}

void DlgSettings::on_checkBoxFader_toggled(bool checked) {
    if (!m_pageSetupDone)
        return;
    m_settings.setAudioUseFader(checked);
    emit audioUseFaderChanged(checked);
}

void DlgSettings::on_checkBoxFaderBm_toggled(bool checked) {
    if (!m_pageSetupDone)
        return;
    m_settings.setAudioUseFaderBm(checked);
    emit audioUseFaderChangedBm(checked);
}

void DlgSettings::on_checkBoxSilenceDetection_toggled(bool checked) {
    if (!m_pageSetupDone)
        return;
    m_settings.setAudioDetectSilence(checked);
    emit audioSilenceDetectChanged(checked);
}

void DlgSettings::on_checkBoxSilenceDetectionBm_toggled(bool checked) {
    if (!m_pageSetupDone)
        return;
    m_settings.setAudioDetectSilenceBm(checked);
    emit audioSilenceDetectChangedBm(checked);
}

void DlgSettings::on_checkBoxDownmix_toggled(bool checked) {
    if (!m_pageSetupDone)
        return;
    m_settings.setAudioDownmix(checked);
    emit audioDownmixChanged(checked);
}

void DlgSettings::on_checkBoxNormalizeLoudness_toggled(bool checked) {
    if (!m_pageSetupDone)
        return;
    m_settings.setKaraokeNormalizeLoudness(checked);
}

void DlgSettings::on_checkBoxDownmixBm_toggled(bool checked) {
    if (!m_pageSetupDone)
        return;
    m_settings.setAudioDownmixBm(checked);
    emit audioDownmixChangedBm(checked);
}

void DlgSettings::on_comboBoxDevice_currentIndexChanged(const QString &arg1) {
    if (!m_pageSetupDone)
        return;
    m_settings.setRecordingInput(arg1);
}

void DlgSettings::on_comboBoxCodec_currentIndexChanged(const QString &arg1) {
    if (!m_pageSetupDone)
        return;
    m_settings.setRecordingCodec(arg1);
    if (arg1 == "audio/mpeg")
        m_settings.setRecordingRawExtension("mp3");
}

void DlgSettings::on_groupBoxRecording_toggled(bool arg1) {
    if (!m_pageSetupDone)
        return;
    m_settings.setRecordingEnabled(arg1);
}

void DlgSettings::on_buttonBrowse_clicked() {
#ifdef Q_OS_LINUX
    QString dirName = QFileDialog::getExistingDirectory(
            this,
            "Select output directory",
            QStandardPaths::standardLocations(QStandardPaths::MusicLocation).at(0),
            QFileDialog::ShowDirsOnly | QFileDialog::DontUseNativeDialog
    );
#else
    QString dirName = QFileDialog::getExistingDirectory(
            this,
            "Select output directory",
            QStandardPaths::standardLocations(QStandardPaths::MusicLocation).at(0),
            QFileDialog::ShowDirsOnly
    );
#endif
    if (dirName != "") {
        m_settings.setRecordingOutputDir(dirName);
        ui->lineEditOutputDir->setText(dirName);
    }
}

void DlgSettings::on_pushButtonClearBgImg_clicked() {
    m_settings.setCdgDisplayBackgroundImage(QString());
    ui->lineEditCdgBackground->setText(QString());
    emit cdgBgImageChanged();
}

void DlgSettings::on_pushButtonSlideshowBrowse_clicked() {
    QString initialPath = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (m_settings.bgSlideShowDir() != "")
        initialPath = m_settings.bgSlideShowDir();
#ifdef Q_OS_LINUX
    QString dirName = QFileDialog::getExistingDirectory(
            this,
            "Select the slideshow directory",
            initialPath,
            QFileDialog::DontUseNativeDialog | QFileDialog::ShowDirsOnly
    );
#else
    QString dirName = QFileDialog::getExistingDirectory(
            this,
            "Select the slideshow directory",
            initialPath,
            QFileDialog::ShowDirsOnly
    );
#endif
    if (dirName != "") {
        m_settings.setBgSlideShowDir(dirName);
        emit bgSlideShowDirChanged(dirName);
        ui->lineEditSlideshowDir->setText(dirName);
    }
}

void DlgSettings::on_rbSlideshow_toggled(bool checked) {
    if (!m_pageSetupDone)
        return;
    Settings::BgMode mode = (checked) ? m_settings.BG_MODE_SLIDESHOW : m_settings.BG_MODE_IMAGE;
    m_settings.setBgMode(mode);
    emit bgModeChanged(mode);
}

void DlgSettings::on_rbBgImage_toggled(bool checked) {
    if (!m_pageSetupDone)
        return;
    Settings::BgMode mode = (checked) ? m_settings.BG_MODE_IMAGE : m_settings.BG_MODE_SLIDESHOW;
    m_settings.setBgMode(mode);
    emit bgModeChanged(mode);
}

void DlgSettings::on_lineEditApiKey_editingFinished() {
    m_settings.setRequestServerApiKey(ui->lineEditApiKey->text());
}

void DlgSettings::on_checkBoxShowKAAAlert_toggled(bool checked) {
    if (!m_pageSetupDone)
        return;
    m_settings.setKaraokeAAAlertEnabled(checked);
}

void DlgSettings::on_checkBoxKAA_toggled(bool checked) {
    if (!m_pageSetupDone)
        return;
    m_settings.setKaraokeAutoAdvance(checked);
}

void DlgSettings::on_checkBoxAutoPlayFirst_toggled(bool checked) {
    if (!m_pageSetupDone)
        return;
    m_settings.setKaraokeAutoPlayFirstSong(checked);
}

void DlgSettings::on_spinBoxAADelay_valueChanged(int arg1) {
    if (!m_pageSetupDone)
        return;
    m_settings.setKaraokeAATimeout(arg1);
}

void DlgSettings::on_btnAlertFont_clicked() {
    bool ok;
    QFont font = QFontDialog::getFont(&ok, m_settings.karaokeAAAlertFont(), this, "Select alert font");
    if (ok) {
        m_settings.setKaraokeAAAlertFont(font);
        emit karaokeAAAlertFontChanged(font);
    }
}

void DlgSettings::on_btnAlertTxtColor_clicked() {
    QColor clr = QColorDialog::getColor(m_settings.alertTxtColor(), this, "Select alert text color");
    if (clr.isValid()) {
        QString ss = ui->btnAlertTxtColor->styleSheet();
        QColor oclr = m_settings.alertTxtColor();
        ss.replace(QString(QString::number(oclr.red()) + "," + QString::number(oclr.green()) + "," +
                           QString::number(oclr.blue())),
                   QString(QString::number(clr.red()) + "," + QString::number(clr.green()) + "," +
                           QString::number(clr.blue())));
        ui->btnAlertTxtColor->setStyleSheet(ss);
        m_settings.setAlertTxtColor(clr);
        emit alertTxtColorChanged(clr);
    }
}

void DlgSettings::on_btnAlertBgColor_clicked() {
    QColor clr = QColorDialog::getColor(m_settings.alertBgColor(), this, "Select alert background color");
    if (clr.isValid()) {
        QString ss = ui->btnAlertBgColor->styleSheet();
        QColor oclr = m_settings.alertBgColor();
        ss.replace(QString(QString::number(oclr.red()) + "," + QString::number(oclr.green()) + "," +
                           QString::number(oclr.blue())),
                   QString(QString::number(clr.red()) + "," + QString::number(clr.green()) + "," +
                           QString::number(clr.blue())));
        ui->btnAlertBgColor->setStyleSheet(ss);
        m_settings.setAlertBgColor(clr);
        emit alertBgColorChanged(clr);
    }
}

void DlgSettings::on_cbxBmAutostart_clicked(bool checked) {
    m_settings.setBmAutoStart(checked);
}

void DlgSettings::on_spinBoxInterval_valueChanged(int arg1) {
    if (!m_pageSetupDone)
        return;
    m_settings.setRequestServerInterval(arg1);
    emit requestServerIntervalChanged(arg1);
}

void DlgSettings::on_cbxTheme_currentIndexChanged(int index) {
    if (!m_pageSetupDone)
        return;
    m_settings.setTheme(index);
}

void DlgSettings::on_btnBrowse_clicked() {
#ifdef Q_OS_LINUX
    QString fileName = QFileDialog::getExistingDirectory(
            this,
            "Select directory to put store downloads in",
            m_settings.storeDownloadDir(),
            QFileDialog::ShowDirsOnly | QFileDialog::DontUseNativeDialog
    );
#else
    QString fileName = QFileDialog::getExistingDirectory(
            this,
            "Select directory to put store downloads in",
            m_settings.storeDownloadDir(),
            QFileDialog::ShowDirsOnly
    );
#endif
    if (fileName != "") {
        QFileInfo fi(fileName);
        if (!fi.isWritable() || !fi.isReadable()) {
            QMessageBox msgBox;
            msgBox.setWindowTitle("Directory not writable!");
            msgBox.setText("You do not have permission to write to the selected directory, aborting.");
            msgBox.exec();
        }
        m_settings.setStoreDownloadDir(fileName + QDir::separator());
        ui->lineEditDownloadsDir->setText(fileName + QDir::separator());
    }
}

void DlgSettings::on_fontComboBox_currentFontChanged(const QFont &f) {
    if (!m_pageSetupDone)
        return;
    QFont font = f;
    font.setPointSize(ui->spinBoxAppFontSize->value());
    m_settings.setApplicationFont(font);
    emit applicationFontChanged(font);
    setFont(font);
}

void DlgSettings::on_spinBoxAppFontSize_valueChanged(int arg1) {
    if (!m_pageSetupDone)
        return;
    QFont font = m_settings.applicationFont();
    font.setPointSize(arg1);
    m_settings.setApplicationFont(font);
    emit applicationFontChanged(font);
    setFont(font);
}

void DlgSettings::on_btnTestReqServer_clicked() {
    auto api = new OKJSongbookAPI(this);
    connect(api, &OKJSongbookAPI::testFailed, this, &DlgSettings::reqSvrTestError);
    connect(api, &OKJSongbookAPI::testSslError, this, &DlgSettings::reqSvrTestSslError);
    connect(api, &OKJSongbookAPI::testPassed, this, &DlgSettings::reqSvrTestPassed);
    api->test();

    delete api;
}

void DlgSettings::reqSvrTestError(QString error) {
    QMessageBox msgBox;
    msgBox.setWindowTitle("Request server test failed!");
    msgBox.setText("Request server connection test was unsuccessful!");
    msgBox.setInformativeText("Error msg:\n" + error);
    msgBox.exec();
}

void DlgSettings::reqSvrTestSslError(QString error) {
    QMessageBox msgBox;
    msgBox.setWindowTitle("Request server test failed!");
    msgBox.setText("Request server connection test was unsuccessful due to SSL errors!");
    msgBox.setInformativeText("Error msg:\n" + error);
    msgBox.exec();
}

void DlgSettings::reqSvrTestPassed() {
    QMessageBox msgBox;
    msgBox.setWindowTitle("Request server test passed");
    msgBox.setText("Request server connection test was successful.  Server info and API key appear to be valid");
    msgBox.exec();
}

void DlgSettings::on_checkBoxIncludeEmptySingers_clicked(bool checked) {
    m_settings.setEstimationSkipEmptySingers(!checked);
    emit rotationDurationSettingsModified();
}

void DlgSettings::on_spinBoxDefaultPadTime_valueChanged(int arg1) {
    if (!m_pageSetupDone)
        return;
    m_settings.setEstimationSingerPad(arg1);
    emit rotationDurationSettingsModified();
}

void DlgSettings::on_spinBoxDefaultSongDuration_valueChanged(int arg1) {
    if (!m_pageSetupDone)
        return;
    m_settings.setEstimationEmptySongLength(arg1);
    emit rotationDurationSettingsModified();
}

void DlgSettings::on_checkBoxDisplayCurrentRotationPosition_clicked(bool checked) {
    m_settings.setRotationDisplayPosition(checked);
}

void DlgSettings::entitledSystemCountChanged(int count) {
    ui->spinBoxSystemId->setMaximum(count);
    if (m_settings.systemId() <= count) {
        ui->spinBoxSystemId->setValue(m_settings.systemId());
    }
}

void DlgSettings::on_groupBoxShowDuration_clicked(bool checked) {
    m_settings.setCdgRemainEnabled(checked);
    emit cdgRemainEnabledChanged(checked);
}

void DlgSettings::on_checkBoxPitchCue_clicked(bool checked) {
    m_settings.setCdgPitchCueEnabled(checked);
}

void DlgSettings::on_checkBoxCheers_clicked(bool checked) {
    m_settings.setCheersEnabled(checked);
}

void DlgSettings::on_btnDurationFont_clicked() {
    bool ok;
    QFont font = QFontDialog::getFont(&ok, m_settings.cdgRemainFont(), this, "Select CDG duration display font");
    if (ok) {
        m_settings.setCdgRemainFont(font);
        emit cdgRemainFontChanged(font);
    }
}

void DlgSettings::on_btnDurationFontColor_clicked() {
    QColor clr = QColorDialog::getColor(m_settings.cdgRemainTextColor(), this,
                                        "Select CDG duration display text color");
    if (clr.isValid()) {
        QString ss = ui->btnDurationFontColor->styleSheet();
        QColor oclr = m_settings.cdgRemainTextColor();
        ss.replace(QString(QString::number(oclr.red()) + "," + QString::number(oclr.green()) + "," +
                           QString::number(oclr.blue())),
                   QString(QString::number(clr.red()) + "," + QString::number(clr.green()) + "," +
                           QString::number(clr.blue())));
        ui->btnDurationFontColor->setStyleSheet(ss);
        m_settings.setCdgRemainTextColor(clr);
        emit cdgRemainTextColorChanged(clr);
    }
}

void DlgSettings::on_btnDurationBgColor_clicked() {
    QColor clr = QColorDialog::getColor(m_settings.cdgRemainBgColor(), this,
                                        "Select CDG duration display background color");
    if (clr.isValid()) {
        QString ss = ui->btnDurationBgColor->styleSheet();
        QColor oclr = m_settings.cdgRemainBgColor();
        ss.replace(QString(QString::number(oclr.red()) + "," + QString::number(oclr.green()) + "," +
                           QString::number(oclr.blue())),
                   QString(QString::number(clr.red()) + "," + QString::number(clr.green()) + "," +
                           QString::number(clr.blue())));
        ui->btnDurationBgColor->setStyleSheet(ss);
        m_settings.setCdgRemainBgColor(clr);
        emit cdgRemainBgColorChanged(clr);
    }
}

void DlgSettings::on_btnLogDirBrowse_clicked() {
#ifdef Q_OS_LINUX
    QString fileName = QFileDialog::getExistingDirectory(
            this,
            "Select directory to put logs in",
            m_settings.logDir(),
            QFileDialog::ShowDirsOnly | QFileDialog::DontUseNativeDialog
    );
#else
    QString fileName = QFileDialog::getExistingDirectory(
            this,
            "Select directory to put logs in",
            m_settings.logDir(),
            QFileDialog::ShowDirsOnly
    );
#endif
    if (fileName != "") {
        QFileInfo fi(fileName);
        if (!fi.isWritable() || !fi.isReadable()) {
            QMessageBox msgBox;
            msgBox.setWindowTitle("Directory not writable!");
            msgBox.setText("You do not have permission to write to the selected directory, aborting.");
            msgBox.exec();
        }
        m_settings.setLogDir(fileName + QDir::separator());
        ui->lineEditLogDir->setText(fileName + QDir::separator());
    }
}

void DlgSettings::on_checkBoxProgressiveSearch_toggled(bool checked) {
    if (!m_pageSetupDone)
        return;
    m_settings.setProgressiveSearchEnabled(checked);
}

void DlgSettings::on_cbxPreviewEnabled_toggled(bool checked) {
    if (!m_pageSetupDone)
        return;
    m_settings.setPreviewEnabled(!checked);
}

void DlgSettings::on_comboBoxKAudioDevices_currentIndexChanged(int index) {
    if (!m_pageSetupDone)
        return;
    QString device = ui->comboBoxKAudioDevices->itemText(index);
    if (m_settings.audioOutputDevice() == device)
        return;
    qInfo() << "Changing karaoke audio output device to: " << index << ")" << device;
    m_settings.setAudioOutputDevice(device);
    kAudioBackend.setAudioOutputDevice(device);
}

void DlgSettings::on_comboBoxBAudioDevices_currentIndexChanged(int index) {
    if (!m_pageSetupDone)
        return;
    QString device = ui->comboBoxBAudioDevices->itemText(index);
    if (m_settings.audioOutputDeviceBm() == device)
        return;
    qInfo() << "Changing karaoke audio output device to: " << index << ")" << device;
    m_settings.setAudioOutputDeviceBm(device);
    bmAudioBackend.setAudioOutputDevice(device);
}

void DlgSettings::on_checkBoxEnforceAspectRatio_clicked(bool checked) {
    m_settings.setEnforceAspectRatio(checked);
}

void DlgSettings::on_pushButtonApplyTickerMsg_clicked() {
    m_settings.setTickerCustomString(ui->lineEditTickerMessage->text());
    emit tickerCustomStringChanged();
}

void DlgSettings::on_pushButtonResetDurationPos_clicked() {
    m_settings.resetDurationPosition();
    emit durationPositionReset();
}

void DlgSettings::on_lineEditTickerMessage_returnPressed() {
    m_settings.setTickerCustomString(ui->lineEditTickerMessage->text());
    emit tickerCustomStringChanged();
}

void DlgSettings::on_checkBoxHardwareAccel_toggled(bool checked) {
    if (!m_pageSetupDone)
        return;
    m_settings.setHardwareAccelEnabled(checked);
}

void DlgSettings::on_checkBoxCdgPrescaling_stateChanged(int arg1) {
    if (!m_pageSetupDone)
        return;
    if (arg1 == 0)
        m_settings.setCdgPrescalingEnabled(false);
    else
        m_settings.setCdgPrescalingEnabled(true);
}

void DlgSettings::on_checkBoxCurrentSingerTop_toggled(bool checked) {
    if (!m_pageSetupDone)
        return;
    m_settings.setRotationAltSortOrder(checked);
}


void DlgSettings::closeEvent([[maybe_unused]]QCloseEvent *event) {
    m_settings.saveWindowState(this);
    deleteLater();
}

void DlgSettings::done(int) {
    close();
}

void DlgSettings::comboBoxConsoleLogLevelChanged(int index) {
    m_settings.setConsoleLogLevel(index);
    auto sink = m_logger->sinks().at(0);
    switch (index) {
        case Settings::LOG_LEVEL_CRITICAL:
            sink->set_level(spdlog::level::critical);
            break;
        case Settings::LOG_LEVEL_ERROR:
            sink->set_level(spdlog::level::err);
            break;
        case Settings::LOG_LEVEL_WARNING:
            sink->set_level(spdlog::level::warn);
            break;
        case Settings::LOG_LEVEL_INFO:
            sink->set_level(spdlog::level::info);
            break;
        case Settings::LOG_LEVEL_DEBUG:
            sink->set_level(spdlog::level::debug);
            break;
        case Settings::LOG_LEVEL_TRACE:
            sink->set_level(spdlog::level::trace);
            break;
        default:
            sink->set_level(spdlog::level::off);
    }
    if (m_logger->level() < sink->level())
        m_logger->set_level(sink->level());
}

void DlgSettings::comboBoxFileLogLevelChanged(int index) {
    m_settings.setFileLogLevel(index);
    auto sink = m_logger->sinks().at(1);
    switch (index) {
        case Settings::LOG_LEVEL_CRITICAL:
            sink->set_level(spdlog::level::critical);
            break;
        case Settings::LOG_LEVEL_ERROR:
            sink->set_level(spdlog::level::err);
            break;
        case Settings::LOG_LEVEL_WARNING:
            sink->set_level(spdlog::level::warn);
            break;
        case Settings::LOG_LEVEL_INFO:
            sink->set_level(spdlog::level::info);
            break;
        case Settings::LOG_LEVEL_DEBUG:
            sink->set_level(spdlog::level::debug);
            break;
        case Settings::LOG_LEVEL_TRACE:
            sink->set_level(spdlog::level::trace);
            break;
        default:
            sink->set_level(spdlog::level::off);
    }
    if (m_logger->level() < sink->level())
        m_logger->set_level(sink->level());
}
