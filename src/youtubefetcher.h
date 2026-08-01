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
#include <spdlog/spdlog.h>
#include <spdlog/async_logger.h>
#include <spdlog/fmt/ostr.h>

#include "okjfmt.h"

class Settings;

// The discid stamped on dbsongs rows backed by a fetched video. Follows the
// !!DROPPED!! / !!BAD!! convention already used for rows that aren't part of the
// KJ's scanned library, which the catalog queries filter on.
inline constexpr auto kYoutubeDiscId = "!!YOUTUBE!!";

// Video ids become filenames and process arguments, so anything that fails this
// never reaches the disk or a command line. YouTube ids are 11 characters of
// base64url; nothing else is accepted, in particular no dots or separators.
bool isValidYoutubeVideoId(const QString &videoId);

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

    void enqueue(const QString &videoId, int songId, const QString &requestedBy);
    void cancel(const QString &videoId);

signals:
    void fetchProgress(const QString &videoId, int percent);
    void fetchFinished(const QString &videoId, int songId, bool ok, const QString &error);
    void availabilityChanged(bool available);

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
