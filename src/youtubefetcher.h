#ifndef YOUTUBEFETCHER_H
#define YOUTUBEFETCHER_H

#include <QDateTime>
#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QQueue>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVector>
#include <spdlog/spdlog.h>
#include <spdlog/async_logger.h>
#include <spdlog/fmt/ostr.h>

#include "okjfmt.h"

class Settings;

// The discid stamped on dbsongs rows backed by a fetched video. Follows the
// !!DROPPED!! convention already used for rows that aren't part of the KJ's
// scanned library, which the catalog queries filter on. (Songs marked bad used
// to be flagged the same way; they have their own column now.)
inline constexpr auto kYoutubeDiscId = "!!YOUTUBE!!";

// Video ids become filenames and process arguments, so anything that fails this
// never reaches the disk or a command line. YouTube ids are 11 characters of
// base64url; nothing else is accepted, in particular no dots or separators.
bool isValidYoutubeVideoId(const QString &videoId);

// Whether a queued song can be started right now.
//
// True for every ordinary library song, including one whose file has gone missing -
// that is a different failure with its own handling at play time, and changing it
// here would quietly alter how the rotation treats a slow network share.
//
// False only for a video whose download has not finished. Callers walking the
// rotation use this to pass over a singer rather than stall the show or burn their
// turn on a file that isn't there yet; the request stays queued and comes back
// around on the next pass.
bool songPathIsPlayable(const QString &path);

// How far into a video a singer can bail out and still have it read as "this was the
// wrong video" rather than "I changed my mind". Long enough to sit through the intro
// of a track that turns out to have no lyrics on it, short enough that nobody who
// meant to sing reaches it.
inline constexpr qint64 kEarlySkipCutoffMs = 30000;

// The video a cached file belongs to, or empty for anything that is not one. Every
// file in the cache is named after its video id, so this is pure string work - no
// lookup, and safe to call on library paths and temp files alike.
QString videoIdForCachePath(const QString &path);

// Downloads requested YouTube videos to a local cache with yt-dlp, one QProcess per
// job, so the rest of the app only ever deals with a normal file on disk.
//
// Runs on the main thread alongside the UI and the embedded API. Nothing here blocks:
// process launches are fire-and-forget and results arrive on signals.
class YoutubeFetcher : public QObject {
Q_OBJECT

public:
    enum class State {
        Pending,
        Fetching,
        Ready,
        Failed
    };

    // One downloaded video, for the cache manager and the eviction sweeps.
    struct CacheEntry {
        QString videoId;
        int songId{0};
        QString artist;
        QString title;
        QString path;
        qint64 bytes{0};
        int plays{0};
        QDateTime lastPlay;
        // Queued and unsung. Never evicted - somebody is waiting to sing it.
        bool queued{false};
    };

    struct Status {
        // False when no one has ever requested this video. Distinct from Pending,
        // which means requested and waiting its turn to download - a caller that
        // conflates the two tells singers a video is downloading when nothing is.
        bool known{false};
        State state{State::Pending};
        int progress{0};
        QString error;
    };

    explicit YoutubeFetcher(Settings &settings, QObject *parent = nullptr);
    ~YoutubeFetcher() override;

    // Probes yt-dlp and re-enqueues anything left pending by a previous run. Safe to
    // call before the cache directory exists.
    void start();

    // False when yt-dlp is missing or unusable, or when ffmpeg has not been found
    // (beside yt-dlp, on PATH, or auto-downloaded) - without ffmpeg the DASH merge the
    // fetch format prefers fails every time. The embedded API reports this as a
    // capability so the feature disappears from the phones rather than offering a
    // button that will always fail.
    [[nodiscard]] bool isAvailable() const { return m_available; }

    // True while a missing default yt-dlp copy is being downloaded.
    [[nodiscard]] bool isBootstrapping() const { return m_bootstrapReply != nullptr; }

    // True while a missing ffmpeg is being downloaded and extracted.
    [[nodiscard]] bool isFfmpegBootstrapping() const { return m_ffmpegReply != nullptr; }

    // True while a missing JS runtime is being downloaded and extracted.
    [[nodiscard]] bool isJsRuntimeBootstrapping() const { return m_jsRuntimeReply != nullptr; }

    // Whether a JS runtime (Deno) was found beside yt-dlp, on PATH, or auto-downloaded.
    // Deliberately not part of isAvailable(): YouTube's player challenge needs it to
    // reach the better formats, but yt-dlp degrades rather than failing outright, so a
    // venue that cannot fetch it still gets a working feature.
    [[nodiscard]] bool jsRuntimeAvailable() const { return m_jsRuntimeAvailable; }

    // Clears the per-binary download backoff. Called when the KJ re-enables the feature,
    // which is the one unambiguous "try that again" gesture the dialog offers.
    void resetBootstrapCooldowns();

    // Whether a usable ffmpeg was found beside yt-dlp, on PATH, or auto-downloaded.
    // isAvailable() is gated on this too - without it the DASH merge the fetch format
    // prefers fails every time, so advertising the feature is only honest once both
    // binaries are in place.
    [[nodiscard]] bool ffmpegAvailable() const { return m_ffmpegAvailable; }

    // Where a video will live once fetched. Deterministic and known before the
    // download starts, which is what lets the dbsongs row be written up front.
    [[nodiscard]] QString cachePathFor(const QString &videoId) const;
    [[nodiscard]] Status status(const QString &videoId) const;
    [[nodiscard]] bool isReady(const QString &videoId) const;
    // Requests still waiting on a download. Lets the KJ be told the rotation is
    // waiting on a fetch rather than the flatly wrong "no unsung songs are queued".
    [[nodiscard]] int pendingCount() const;
    // Version string from the last successful probe, empty if yt-dlp never answered.
    [[nodiscard]] QString version() const { return m_version; }
    // Re-probes yt-dlp. For the settings dialog, after the KJ edits the path.
    void refreshAvailability();
    // Operator-initiated update, distinct from the automatic one that follows a
    // suspected extractor breakage: it ignores the hourly cooldown, because someone
    // is standing there having asked for it.
    void updateNow();

    void enqueue(const QString &videoId, int songId, const QString &requestedBy);
    void cancel(const QString &videoId);

    // Everything currently downloaded, newest play first.
    [[nodiscard]] QVector<CacheEntry> cacheEntries() const;
    [[nodiscard]] qint64 cacheBytes() const;

    // Drops one cached video: deletes the file and forgets the download, but leaves
    // the dbsongs row in place. That row is referenced by favorites, regulars and
    // queue history, and it carries the play counts the eviction policy reads - so it
    // stays as a stub, and a later request simply downloads the video again.
    // Refuses while the video is queued unsung or still downloading.
    bool evict(const QString &videoId, QString *error = nullptr);

    // Age sweep then, if still over the size cap, a least-recently-played sweep.
    // Returns how many videos were dropped.
    int runCacheMaintenance();

signals:
    void fetchProgress(const QString &videoId, int percent);
    void fetchFinished(const QString &videoId, int songId, bool ok, const QString &error);
    void availabilityChanged(bool available);
    // Outcome of an update attempt, so the settings dialog can report it. Also fires
    // for the automatic post-breakage update, which is worth surfacing.
    void updateFinished(bool ok, const QString &message);
    // Download of the default yt-dlp copy kicked off, and its outcome. The settings
    // dialog shows "downloading" while this runs so the status line does not sit on
    // "not found" for a binary that is on its way.
    void bootstrapStarted();
    void bootstrapFinished(bool ok);
    // Same, for the ffmpeg that yt-dlp needs to merge the DASH streams this feature
    // requests. Fires after the yt-dlp probe succeeds, when ffmpeg is neither beside
    // yt-dlp nor on PATH.
    void ffmpegBootstrapStarted();
    void ffmpegBootstrapFinished(bool ok);
    // Same again for the JS runtime yt-dlp needs to solve YouTube's player challenge.
    // Availability changes separately from the download outcome: it also flips when one
    // is found beside yt-dlp or on PATH, where no download ever runs.
    void jsRuntimeBootstrapStarted();
    void jsRuntimeBootstrapFinished(bool ok);
    void jsRuntimeAvailabilityChanged(bool available);
    // Percentage for whichever binary is currently coming down, so a KJ watching a
    // 100 MB archive arrive on venue wifi can tell it from a hang. -1 when the server
    // sent no content length. `what` is the binary's name, ready for a status line.
    void bootstrapProgress(const QString &what, int percent);

private slots:
    // Connected to every fetch process. The job is recovered from the sender's
    // videoId property rather than a per-process member, so one handler serves all of
    // them however many run at once.
    void onProcessReadyRead();
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onProcessErrorOccurred(QProcess::ProcessError error);

private:
    struct Job {
        QString videoId;
        int songId{-1};
        QProcess *process{nullptr};
        QString stderrTail;
        // Monotonic: yt-dlp reports 0-100 once per stream when video and audio are
        // downloaded separately, and a bar that restarts reads as a stall to a singer
        // watching their phone.
        int reportedProgress{0};
        QDateTime started;
    };

    Settings &m_settings;
    std::string m_loggingPrefix{"[YoutubeFetcher]"};
    std::shared_ptr<spdlog::logger> m_logger;

    bool m_available{false};
    bool m_ffmpegAvailable{false};
    bool m_jsRuntimeAvailable{false};
    QString m_version;
    QQueue<QString> m_queued;
    QHash<QString, Job *> m_active;
    QProcess *m_versionProcess{nullptr};
    QProcess *m_updateProcess{nullptr};
    QProcess *m_ffmpegProcess{nullptr};
    QProcess *m_jsRuntimeProcess{nullptr};
    QNetworkAccessManager *m_downloadManager{nullptr};
    QNetworkReply *m_bootstrapReply{nullptr};
    QNetworkReply *m_ffmpegReply{nullptr};
    QNetworkReply *m_jsRuntimeReply{nullptr};
    QDateTime m_lastUpdateAttempt;
    // One per binary, so a failed ffmpeg fetch does not also block yt-dlp's.
    QDateTime m_lastDlpBootstrap;
    QDateTime m_lastFfmpegBootstrap;
    QDateTime m_lastJsRuntimeBootstrap;
    QTimer m_sweepTimer;

    // A stalled download must not hold a rotation slot hostage for the whole show.
    static constexpr int kJobTimeoutSecs = 600;
    // One self-update attempt per hour, so a burst of queued requests failing on the
    // same breakage doesn't launch a burst of updaters.
    static constexpr int kUpdateCooldownSecs = 3600;
    // A failed fetch is retried once, and only after a successful self-update.
    static constexpr int kMaxAttempts = 2;
    // A failed download is not retried for this long. Without it, every probe - and the
    // settings dialog fires one whenever the path field loses focus - would restart a
    // 100 MB archive from zero on a connection that has already shown it cannot take it.
    static constexpr int kBootstrapCooldownSecs = 900;

    [[nodiscard]] QString dlpPath() const;
    [[nodiscard]] bool ensureCacheDir() const;
    [[nodiscard]] QStringList fetchArgs(const QString &videoId) const;
    [[nodiscard]] QString partPathFor(const QString &videoId) const;

    void probeAvailability();
    void setAvailable(bool available);
    void startBootstrap();
    void onBootstrapFinished();
    [[nodiscard]] QUrl bootstrapUrl() const;
    bool writeBootstrapBinary(const QString &path, const QByteArray &data);
    // ffmpeg is what yt-dlp merges DASH video+audio with, and it looks for it next to
    // its own binary or on PATH. When neither holds one, fetch it (Windows and macOS)
    // and put it beside yt-dlp so the merge works without the KJ touching anything.
    void ensureFfmpeg();
    void startFfmpegBootstrap();
    void onFfmpegDownloadFinished();
    [[nodiscard]] QUrl ffmpegUrl() const;
    [[nodiscard]] QString binaryBesideDlp(const QString &name) const;
    [[nodiscard]] QString ffmpegBesidePath() const;
    // yt-dlp's post-merge fixups shell out to ffprobe. It rides along in the Windows
    // archive, so it lands beside ffmpeg without a second download.
    [[nodiscard]] QString ffprobeBesidePath() const;
    // Deno. YouTube's player challenge is solved by running the site's own JavaScript,
    // and yt-dlp needs an external runtime for it - without one the good formats quietly
    // stop being offered. Every Deno release asset is a zip holding one static binary,
    // on every platform, so unlike ffmpeg this can be offered on Linux too.
    [[nodiscard]] QString jsRuntimeBesidePath() const;
    void ensureJsRuntime();
    void setJsRuntimeAvailable(bool available);
    void startJsRuntimeBootstrap();
    void onJsRuntimeDownloadFinished();
    [[nodiscard]] QUrl jsRuntimeUrl() const;
    // Pulls one named file out of an in-memory zip, ignoring the directory the build
    // wraps around it. Goes through QSaveFile rather than miniz's own stdio path, which
    // would mangle a target path the local 8-bit codec cannot represent.
    bool extractZipBinary(const QByteArray &zipData, const QString &memberName, const QString &targetPath);
    // ffmpeg plus the ffprobe that rides along in the same archive.
    bool extractFfmpeg(const QByteArray &zipData, const QString &targetPath);
    // Environment for a spawned yt-dlp: our own binary directory first on PATH.
    [[nodiscard]] QProcessEnvironment childEnvironment() const;
    // Shared download plumbing - progress relay and the per-binary backoff.
    QNetworkReply *startDownload(const QUrl &url, const QString &what, int timeoutMs);
    bool bootstrapAllowed(QDateTime &lastAttempt, const QString &what);
    void dispatchNext();
    void startJob(const QString &videoId, int songId);
    void finishJob(Job *job, bool ok, const QString &error);
    void parseProgress(Job *job, const QString &line);
    void sweepTimeouts();
    void maybeSelfUpdate(const QString &stderrTail);
    void runUpdate();
    void requeueAfterUpdate();

    // Persistence. local_youtube_fetches is the source of truth across restarts; the
    // in-memory maps only track what is running right now.
    void dbUpsert(const QString &videoId, int songId, const QString &requestedBy);
    void dbSetState(const QString &videoId, State state, int progress, const QString &error);
    void dbIncrementAttempts(const QString &videoId);
    void resumePending();
    // Overwrites dbsongs.duration from the fetched file. The value the phone posted is
    // only ever a display hint, and it feeds every wait_seconds estimate in the queue.
    void updateSongDuration(int songId, const QString &path);
};

#endif // YOUTUBEFETCHER_H
