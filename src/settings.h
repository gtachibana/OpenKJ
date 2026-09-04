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

#ifndef SETTINGS_H
#define SETTINGS_H

#include <QHeaderView>
#include <QObject>
#include <QSettings>
#include <QSplitter>
#include <QTableView>
#include <QTreeView>
#include <QWidget>
#include <QMetaType>
#include <QKeySequence>

struct SfxEntry
{
    SfxEntry();
    QString name;
    QString path;


}; Q_DECLARE_METATYPE(SfxEntry)


typedef QList<SfxEntry> SfxEntryList;

Q_DECLARE_METATYPE(QList<SfxEntry>)

class Settings : public QObject
{
    Q_OBJECT

private:
    QSettings *settings;
    // Process-wide, deliberately. Every other piece of state in this class lives in
    // QSettings and is therefore shared by all instances; this one did not, so the flag
    // main() set on its own Settings object was invisible to the 33 instances that
    // actually call the restore functions below - which meant answering "yes, load safe
    // settings" after a failed startup restored the broken geometry anyway.
    inline static bool m_safeStartupMode{false};
    QString modeScopedKey(const QString &key, int mode) const;
    QVariant scopedValue(const QString &key, const QVariant &defaultValue, bool fallbackLegacy = true, int mode = -1) const;
    void setScopedValue(const QString &key, const QVariant &value, int mode = -1);

public:
    enum AppMode {
        ClassicMode = 0,
        LocalMode = 1
    };
    enum {
        LOG_LEVEL_DISABLED,
        LOG_LEVEL_CRITICAL,
        LOG_LEVEL_ERROR,
        LOG_LEVEL_WARNING,
        LOG_LEVEL_INFO,
        LOG_LEVEL_DEBUG,
        LOG_LEVEL_TRACE
    };
    int getConsoleLogLevel();
    int getFileLogLevel();
    bool tickerReducedCpuMode();
    void setTickerReducedCpuMode(bool enabled);
    void setConsoleLogLevel(int level);
    void setFileLogLevel(int level);
    int lastRunRotationTopSingerId();
    void setLastRunRotationTopSingerId(int id);
    [[nodiscard]] bool lastStartupOk() const;
    void setStartupOk(bool ok);
    [[nodiscard]] QString lastRunVersion() const;
    void setLastRunVersion(const QString &version);
    [[nodiscard]] bool safeStartupMode() const;
    void setSafeStartupMode(bool safeMode);
    [[nodiscard]] int historyDblClickAction() const;
    void setHistoryDblClickAction(int index);
    int getSystemRamSize();
    int remainRtOffset();
    int remainBtmOffset();
    qint64 hash(const QString & str);
    bool progressiveSearchEnabled();
    QString storeDownloadDir();
    QString logDir();
    // Where a duplicate set of files is moved when it cannot simply be recycled
    // - see LibraryMerger. Empty means "beside the source directory", which is
    // resolved per song by LibraryMerger::resolveReviewDir.
    QString libraryReviewDir();
    bool logShow();
    bool logEnabled();
    void setPassword(QString password);
    void clearPassword();
    bool chkPassword(QString password);
    bool passIsSet();
    void setCC(QString ccn, QString month, QString year, QString ccv, QString passwd);
    void setSaveCC(bool save);
    bool saveCC();
    void clearCC();
    void clearKNAccount();
    void setSaveKNAccount(bool save);
    bool saveKNAccount();
    bool testingEnabled();
    bool hardwareAccelEnabled();
    bool dbDoubleClickAddsSong();
    QString getCCN(const QString &password);
    QString getCCM(const QString &password);
    QString getCCY(const QString &password);
    QString getCCV(const QString &password);
    void setKaroakeDotNetUser(const QString &username, const QString &password);
    void setKaraokeDotNetPass(const QString &KDNPassword, const QString &password);
    QString karoakeDotNetUser(const QString &password);
    QString karoakeDotNetPass(const QString &password);
    enum BgMode { BG_MODE_IMAGE = 0, BG_MODE_SLIDESHOW };
    enum PreviewSize { Small, Medium, Large };
    explicit Settings(QObject *parent = 0);
    bool cdgWindowFullscreen();
    bool showCdgWindow();
    void setCdgWindowFullscreenMonitor(int monitor);
    int  cdgWindowFullScreenMonitor();
    void saveWindowState(QWidget *window);
    bool restoreWindowState(QWidget *window);
    void saveColumnWidths(QTreeView *treeView);
    void saveColumnWidths(QTableView *tableView);
    void restoreColumnWidths(QTreeView *treeView);
    bool restoreColumnWidths(QTableView *tableView);
    void saveSplitterState(QSplitter *splitter);
    void restoreSplitterState(QSplitter *splitter);
    void setTickerFont(const QFont &font);
    void setApplicationFont(const QFont &font);
    QFont tickerFont();
    [[nodiscard]] QFont applicationFont() const;
    int tickerHeight();
    void setTickerHeight(int height);
    int tickerSpeed();
    void setTickerSpeed(int speed);
    QColor tickerTextColor();
    void setTickerTextColor(QColor color);
    bool cdgRemainEnabled();
    bool cdgPitchCueEnabled();
    void setCdgPitchCueEnabled(bool enabled);
    // Governs the whole cheers feature, not just the drawing: the embedded API
    // reports it as a capability and rejects POST /local/cheer while it is off, so
    // turning it off here takes the button off the phones too.
    bool cheersEnabled();
    void setCheersEnabled(bool enabled);
    QColor tickerBgColor();
    void setTickerBgColor(QColor color);
    bool tickerFullRotation();
    void setTickerFullRotation(bool full);
    int tickerShowNumSingers();
    void setTickerShowNumSingers(int limit);
    void setTickerEnabled(bool enable);
    bool tickerEnabled();
    QString tickerCustomString();
    void setTickerCustomString(const QString &value);
    bool tickerShowRotationInfo();
    [[nodiscard]] AppMode appMode() const;
    void setAppMode(AppMode mode);
    [[nodiscard]] QString appModeName() const;
    bool requestServerEnabled();
    void setRequestServerEnabled(bool enable);
    QString requestServerUrl();
    void setRequestServerUrl(QString url);
    int requestServerVenue();
    void setRequestServerVenue(int venueId);
    QString requestServerApiKey();
    void setRequestServerApiKey(QString apiKey);
    bool requestServerIgnoreCertErrors();
    void setRequestServerIgnoreCertErrors(bool ignore);
    bool audioUseFader();
    bool audioUseFaderBm();
    void setAudioUseFader(bool fader);
    void setAudioUseFaderBm(bool fader);
    int audioVolume();
    void setAudioVolume(int volume);
    QString cdgDisplayBackgroundImage();
    void setCdgDisplayBackgroundImage(QString imageFile);
    BgMode bgMode();
    void setBgMode(BgMode mode);
    QString bgSlideShowDir();
    void setBgSlideShowDir(QString dir);
    bool audioDownmix();
    void setAudioDownmix(bool downmix);
    bool audioDownmixBm();
    void setAudioDownmixBm(bool downmix);
    bool audioDetectSilence();
    bool audioDetectSilenceBm();
    void setAudioDetectSilence(bool enabled);
    void setAudioDetectSilenceBm(bool enabled);
    QString audioOutputDevice();
    QString audioOutputDeviceBm();
    void setAudioOutputDevice(QString device);
    void setAudioOutputDeviceBm(QString device);
    int audioBackend();
    void setAudioBackend(int index);
    QString recordingContainer();
    void setRecordingContainer(QString container);
    QString recordingCodec();
    void setRecordingCodec(QString codec);
    QString recordingInput();
    void setRecordingInput(QString input);
    QString recordingOutputDir();
    void setRecordingOutputDir(QString path);
    bool recordingEnabled();
    void setRecordingEnabled(bool enabled);
    QString recordingRawExtension();
    void setRecordingRawExtension(QString extension);
    int cdgOffsetTop();
    int cdgOffsetBottom();
    int cdgOffsetLeft();
    int cdgOffsetRight();
    bool ignoreAposInSearch();
    int videoOffsetMs();
    bool bmShowFilenames();
    void bmSetShowFilenames(bool show);
    bool bmShowMetadata();
    void bmSetShowMetadata(bool show);
    int bmVolume();
    void bmSetVolume(int bmVolume);
    int bmPlaylistIndex();
    void bmSetPlaylistIndex(int index);
    int mplxMode();
    void setMplxMode(int mode);
    bool karaokeAutoAdvance();
    bool karaokeNormalizeLoudness();
    void setKaraokeNormalizeLoudness(bool enabled);
    bool karaokeAutoPlayFirstSong();
    void setKaraokeAutoPlayFirstSong(bool enabled);
    int karaokeAATimeout();
    void setKaraokeAATimeout(int secs);
    bool karaokeAAAlertEnabled();
    void setKaraokeAAAlertEnabled(bool enabled);
    QFont karaokeAAAlertFont();
    void setKaraokeAAAlertFont(QFont font);
    bool showQueueRemovalWarning();
    bool showSingerRemovalWarning();
    bool showSongInterruptionWarning();
    bool showSongPauseStopWarning();
    QColor alertTxtColor();
    QColor alertBgColor();
    bool bmAutoStart();
    void setBmAutoStart(bool enabled);
    int cdgDisplayOffset();
    QFont bookCreatorTitleFont();
    QFont bookCreatorArtistFont();
    QFont bookCreatorHeaderFont();
    QFont bookCreatorFooterFont();
    QString bookCreatorHeaderText();
    QString bookCreatorFooterText();
    bool bookCreatorPageNumbering();
    int bookCreatorSortCol();
    double bookCreatorMarginRt();
    double bookCreatorMarginLft();
    double bookCreatorMarginTop();
    double bookCreatorMarginBtm();
    int bookCreatorCols();
    int bookCreatorPageSize();
    bool eqKBypass();
    int getEqKLevel(int band);
    bool eqBBypass();
    int getEqBLevel(int band);
    int requestServerInterval();
    bool embeddedApiEnabled();
    bool embeddedApiAccepting();
    int embeddedApiSerial();
    int embeddedApiPort();
    QString embeddedApiBindAddress();
    // How many turns ahead of the mic a singer gets an "up next" event pushed to
    // their phone. 0 turns the events off entirely.
    int embeddedApiUpNextTurns();
    // Addresses whose forwarded-client headers may be believed, comma separated.
    // Loopback is always trusted, which covers a tunnel or reverse proxy running on
    // this machine and pointed at localhost; this is only needed when the proxy
    // reaches the server over the network instead. Registry-only - there is no field
    // for it in the settings dialog, because getting it wrong hands anyone who can
    // reach the port the ability to forge their own address.
    QString embeddedApiTrustedProxies();
    void setEmbeddedApiEnabled(bool enabled);
    void setEmbeddedApiPort(int port);
    void setEmbeddedApiBindAddress(const QString &address);
    void setEmbeddedApiUpNextTurns(int turns);
    // Lets singers request a song off YouTube from their phone. Off by default: it
    // needs yt-dlp present, it downloads media to disk, and whether that is
    // acceptable is the KJ's call, not ours. The embedded API reports it as a
    // capability, so turning it off takes the feature off the phones too.
    bool youtubeRequestsEnabled();
    void setYoutubeRequestsEnabled(bool enabled);
    // Where fetched videos are cached. Must not sit inside a library source dir -
    // DbUpdater only enumerates dbsongs rows under source dirs, so keeping the cache
    // outside them is what stops a library rescan flagging every cached video as a
    // missing file and offering to delete it.
    QString youtubeCacheDir();
    void setYoutubeCacheDir(const QString &path);
    int youtubeCacheMaxGb();
    void setYoutubeCacheMaxGb(int gb);
    int youtubeCacheKeepDays();
    void setYoutubeCacheKeepDays(int days);
    // Empty means "the copy shipped next to the binary".
    QString youtubeDlpPath();
    void setYoutubeDlpPath(const QString &path);
    // The raw setting, empty when the KJ never chose one. youtubeDlpPath() resolves
    // that emptiness; this is what the settings dialog edits, so tabbing through the
    // dialog cannot silently promote a resolved default into a chosen path.
    QString youtubeDlpPathSetting();
    // True when the KJ pointed at a specific binary, false when the copy next to the
    // app is used. A missing default copy can be re-downloaded automatically, but a
    // missing configured path is left alone - the KJ chose it, and it may be a mount
    // that is simply not up yet.
    bool youtubeDlpPathConfigured();
    // Where the automatic download put yt-dlp when the directory next to the app was
    // not writable. Kept apart from the chosen path so deleting that copy re-arms the
    // download instead of looking like a deliberate choice that has gone missing.
    QString youtubeDlpAutoPath();
    void setYoutubeDlpAutoPath(const QString &path);
    // stable or nightly. Nightly by default because YouTube extractor fixes land
    // there days before they reach a stable release, and a broken extractor during a
    // show is the failure that actually costs the KJ something.
    QString youtubeDlpChannel();
    void setYoutubeDlpChannel(const QString &channel);
    int youtubeMaxHeight();
    void setYoutubeMaxHeight(int height);
    // Rejects the hour-long "top 100 karaoke hits" uploads that would otherwise eat
    // the cache and a singer's whole turn.
    int youtubeMaxDurationSecs();
    void setYoutubeMaxDurationSecs(int secs);
    int youtubeMaxConcurrentFetches();
    void setYoutubeMaxConcurrentFetches(int count);
    // Whether videos already downloaded appear in the singers' song search and
    // browse, alongside the KJ's own library. On means the library grows itself and a
    // cached song is instant for the next singer who wants it; off keeps the catalog
    // strictly to what the KJ scanned in. Only ever applies to videos already on
    // disk - one still downloading or failed is never searchable.
    bool youtubeCachedSearchable();
    void setYoutubeCachedSearchable(bool searchable);
    // Whether a video a singer bails out of in the first few seconds is deleted again.
    // The singer who asked for it is the one who stopped it, on the only play it has
    // ever had, so the video was almost certainly the wrong one - a lyric video, a
    // cover, no karaoke track. Nothing is lost that a second request won't fetch back.
    bool youtubeDropOnEarlySkip();
    void setYoutubeDropOnEarlySkip(bool drop);
    QString localUiUrl();
    void setLocalUiUrl(const QString &url);
    bool bmKCrossFade();
    bool requestRemoveOnRotAdd();
    bool requestDialogAutoShow();
    bool checkUpdates();
    int updatesBranch();
    [[nodiscard]] int theme() const;
    const QPoint durationPosition();
    bool dbDirectoryWatchEnabled();
    SfxEntryList getSfxEntries();
    void addSfxEntry(SfxEntry entry);
    void setSfxEntries(SfxEntryList entries);
    [[nodiscard]] int estimationSingerPad() const;
    void setEstimationSingerPad(int secs);
    [[nodiscard]] int estimationEmptySongLength() const;
    void setEstimationEmptySongLength(int secs);
    [[nodiscard]] bool estimationSkipEmptySingers() const;
    void setEstimationSkipEmptySingers(bool skip);
    [[nodiscard]] bool rotationDisplayPosition() const;
    void setRotationDisplayPosition(bool show);
    int currentRotationPosition();
    bool dbSkipValidation();
    bool dbLazyLoadDurations();
    int systemId();
    QFont cdgRemainFont();
    QColor cdgRemainTextColor();
    QColor cdgRemainBgColor();
    [[nodiscard]] bool rotationShowNextSong() const;
    void sync();
    bool previewEnabled();
    bool showMainWindowVideo();
    bool showMainWindowSoundClips();
    void setShowMplxControls(bool show);
    bool showMplxControls();
    void setShowMainWindowSoundClips(const bool &show);
    bool showMainWindowNowPlaying();
    void setShowMainWindowNowPlaying(const bool &show);
    int mainWindowVideoSize();
    void setMainWindowVideoSize(const PreviewSize &size);
    bool enforceAspectRatio();
    void setEnforceAspectRatio(const bool &enforce);
    QString auxTickerFile();
    QString uuid();
    uint slideShowInterval();
    int lastSingerAddPositionType();
    void saveShortcutKeySequence(const QString &name, const QKeySequence &sequence);
    QKeySequence loadShortcutKeySequence(const QString &name);
    bool cdgPrescalingEnabled();
    [[nodiscard]] bool rotationAltSortOrder() const;
    bool treatAllSingersAsRegs();

signals:
    void treatAllSingersAsRegsChanged(bool enabled);
    void enforceAspectRatioChanged(const bool &enforce);
    void requestServerVenueChanged(int venueId);
    void mplxModeChanged(int mode);
    void karaokeAutoAdvanceChanged(bool enabled);
    void karaokeNormalizeLoudnessChanged(bool enabled);
    void showQueueRemovalWarningChanged(bool enabled);
    void showSingerRemovalWarningChanged(bool enabled);
    void showSongInterruptionWarningChanged(bool enabled);
    void showSongStopPauseWarningChanged(bool enabled);
    void requestServerIntervalChanged(int interval);
    void requestServerEnabledChanged(bool enabled);
    void appModeChanged(AppMode mode);
    void rotationDisplayPositionChanged(bool show);
    void rotationDurationSettingsModified();
    void rotationShowNextSongChanged(bool show);
    void remainOffsetChanged(int offsetR, int offsetB);
    void previewEnabledChanged(bool enabled);
    void videoOffsetChanged(int offsetMs);
    void lastSingerAddPositionTypeChanged(int type);
    void shortcutsChanged();


public slots:
    void setShowMainWindowVideo(bool show);
    void setTreatAllSingersAsRegs(bool enabled);
    void setRotationAltSortOrder(bool enabled);
    void setCdgPrescalingEnabled(bool enabled);
    void setSlideShowInterval(int secs);
    void setHardwareAccelEnabled(bool enabled);
    void setDbDoubleClickAddsSong(bool enabled);
    void setDurationPosition(QPoint pos);
    void resetDurationPosition();
    void setRemainRtOffset(int offset);
    void setRemainBtmOffset(int offset);
    void dbSetLazyLoadDurations(bool val);
    void dbSetSkipValidation(bool val);
    void setBmKCrossfade(bool enabled);
    void setShowCdgWindow(bool show);
    void setCdgWindowFullscreen(bool fullScreen);
    void setCdgOffsetTop(int pixels);
    void setCdgOffsetBottom(int pixels);
    void setCdgOffsetLeft(int pixels);
    void setCdgOffsetRight(int pixels);
    void setShowQueueRemovalWarning(bool show);
    void setShowSingerRemovalWarning(bool show);
    void setKaraokeAutoAdvance(bool enabled);
    void setShowSongInterruptionWarning(bool enabled);
    void setAlertBgColor(QColor color);
    void setAlertTxtColor(QColor color);
    void setIgnoreAposInSearch(bool ignore);
    void setShowSongPauseStopWarning(bool enabled);
    void setBookCreatorArtistFont(QFont font);
    void setBookCreatorTitleFont(QFont font);
    void setBookCreatorHeaderFont(QFont font);
    void setBookCreatorFooterFont(QFont font);
    void setBookCreatorHeaderText(QString text);
    void setBookCreatorFooterText(QString text);
    void setBookCreatorPageNumbering(bool show);
    void setBookCreatorSortCol(int col);
    void setBookCreatorMarginRt(double margin);
    void setBookCreatorMarginLft(double margin);
    void setBookCreatorMarginTop(double margin);
    void setBookCreatorMarginBtm(double margin);
    void setEqKBypass(bool bypass);
    void setEqKLevel(int band, int level);
    void setEqBBypass(bool bypass);
    void setEqBLevel(int band, int level);
    void setRequestServerInterval(int interval);
    void setEmbeddedApiAccepting(bool accepting);
    void setEmbeddedApiSerial(int serial);
    void setTickerShowRotationInfo(bool show);
    void setRequestRemoveOnRotAdd(bool remove);
    void setRequestDialogAutoShow(bool enabled);
    void setCheckUpdates(bool enabled);
    void setUpdatesBranch(int index);
    void setTheme(int theme);
    void setBookCreatorCols(int cols);
    void setBookCreatorPageSize(int size);
    void setStoreDownloadDir(QString path);
    void setLogEnabled(bool enabled);
    void setLogVisible(bool visible);
    void setLogDir(QString path);
    void setLibraryReviewDir(QString path);
    void setCurrentRotationPosition(int position);
    void dbSetDirectoryWatchEnabled(bool val);
    void setSystemId(int id);
    void setCdgRemainEnabled(bool enabled);
    void setCdgRemainFont(QFont font);
    void setCdgRemainTextColor(QColor color);
    void setCdgRemainBgColor(QColor color);
    void setRotationShowNextSong(bool show);
    void setProgressiveSearchEnabled(bool enabled);
    void setPreviewEnabled(bool enabled);
    void setVideoOffsetMs(int offset);
    void setLastSingerAddPositionType(int type);
};

#endif // SETTINGS_H
