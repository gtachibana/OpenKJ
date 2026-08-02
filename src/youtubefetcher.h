#ifndef YOUTUBEFETCHER_H
#define YOUTUBEFETCHER_H

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QProcess>
#include <QQueue>
#include <QString>
#include <QStringList>
#include <QTimer>
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

    // False when yt-dlp is missing or unusable. The embedded API reports this as a
    // capability so the feature disappears from the phones rather than offering a
    // button that will always fail.
    [[nodiscard]] bool isAvailable() const { return m_available; }

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
    QString m_version;
    QQueue<QString> m_queued;
    QHash<QString, Job *> m_active;
    QProcess *m_versionProcess{nullptr};
    QProcess *m_updateProcess{nullptr};
    QDateTime m_lastUpdateAttempt;
    QTimer m_sweepTimer;

    // A stalled download must not hold a rotation slot hostage for the whole show.
    static constexpr int kJobTimeoutSecs = 600;
    // One self-update attempt per hour, so a burst of queued requests failing on the
    // same breakage doesn't launch a burst of updaters.
    static constexpr int kUpdateCooldownSecs = 3600;
    // A failed fetch is retried once, and only after a successful self-update.
    static constexpr int kMaxAttempts = 2;

    [[nodiscard]] QString dlpPath() const;
    [[nodiscard]] bool ensureCacheDir() const;
    [[nodiscard]] QStringList fetchArgs(const QString &videoId) const;
    [[nodiscard]] QString partPathFor(const QString &videoId) const;

    void probeAvailability();
    void setAvailable(bool available);
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
