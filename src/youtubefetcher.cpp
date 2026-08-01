#include "youtubefetcher.h"

#include <algorithm>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include "settings.h"
#include "tagreader.h"

namespace {

    // Failures that mean "this particular video can't be had" rather than "yt-dlp no
    // longer understands YouTube". Updating yt-dlp will not fix any of these, and
    // treating them as breakage would launch an updater every time somebody requested
    // a private video.
    const QStringList kVideoSpecificFailures{
            "video unavailable",
            "private video",
            "this video is not available",
            "video has been removed",
            "removed by the uploader",
            "account associated with this video has been terminated",
            "sign in to confirm your age",
            "age-restricted",
            "members-only",
            "join this channel",
            "is not available in your country",
            "blocked it in your country",
            "copyright",
            "premieres in",
            "live event will begin",
            "this live event has ended"
    };

    QString stateToString(const YoutubeFetcher::State state) {
        switch (state) {
            case YoutubeFetcher::State::Fetching:
                return QStringLiteral("fetching");
            case YoutubeFetcher::State::Ready:
                return QStringLiteral("ready");
            case YoutubeFetcher::State::Failed:
                return QStringLiteral("failed");
            case YoutubeFetcher::State::Pending:
                break;
        }
        return QStringLiteral("pending");
    }

    YoutubeFetcher::State stateFromString(const QString &state) {
        if (state == QLatin1String("fetching"))
            return YoutubeFetcher::State::Fetching;
        if (state == QLatin1String("ready"))
            return YoutubeFetcher::State::Ready;
        if (state == QLatin1String("failed"))
            return YoutubeFetcher::State::Failed;
        return YoutubeFetcher::State::Pending;
    }

}

bool isValidYoutubeVideoId(const QString &videoId) {
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_-]{11}$"));
    return re.match(videoId).hasMatch();
}

bool songPathIsPlayable(const QString &path) {
    if (path.isEmpty()) {
        return false;
    }

    // Returns no rows for anything that isn't a fetched video, which is what makes
    // this a no-op for the library: one indexed lookup on dbsongs.path and out.
    QSqlQuery query;
    query.prepare("SELECT COALESCE(yf.state, '') FROM dbsongs d "
                  "LEFT JOIN local_youtube_fetches yf ON yf.songid = d.songid "
                  "WHERE d.path = :path AND d.discid = :discid");
    query.bindValue(":path", path);
    query.bindValue(":discid", QString(kYoutubeDiscId));
    if (!query.exec() || !query.next()) {
        return true;
    }

    // The row and the file both have to agree, so a cache the KJ emptied by hand
    // cannot leave the rotation trying to play something that is gone.
    return query.value(0).toString() == QLatin1String("ready") && QFileInfo::exists(path);
}

YoutubeFetcher::YoutubeFetcher(Settings &settings, QObject *parent)
        : QObject(parent), m_settings(settings) {
    m_logger = spdlog::get("logger");
    m_sweepTimer.setInterval(30000);
    connect(&m_sweepTimer, &QTimer::timeout, this, &YoutubeFetcher::sweepTimeouts);
}

YoutubeFetcher::~YoutubeFetcher() {
    m_sweepTimer.stop();
    // Downloads are resumable and the rows go back to pending on the next launch, so
    // killing them outright beats holding up shutdown.
    for (auto *job: m_active) {
        if (job->process) {
            job->process->disconnect(this);
            job->process->kill();
            job->process->waitForFinished(2000);
            delete job->process;
        }
        delete job;
    }
    m_active.clear();
}

void YoutubeFetcher::start() {
    if (!m_settings.youtubeRequestsEnabled()) {
        m_logger->info("{} YouTube requests are disabled, not starting", m_loggingPrefix);
        // Also covers the KJ switching the feature off from settings: nothing new is
        // dispatched and the timeout sweep stops. Downloads already running are left
        // to finish, since their rows are already queued against a singer.
        m_sweepTimer.stop();
        setAvailable(false);
        return;
    }
    probeAvailability();
    m_sweepTimer.start();
    // A show that ran without a restart for weeks would otherwise only ever evict on
    // the back of a new download.
    runCacheMaintenance();
}

QString YoutubeFetcher::dlpPath() const {
    return m_settings.youtubeDlpPath();
}

QString YoutubeFetcher::cachePathFor(const QString &videoId) const {
    if (!isValidYoutubeVideoId(videoId))
        return {};
    return QDir(m_settings.youtubeCacheDir()).absoluteFilePath(videoId + ".mp4");
}

QString YoutubeFetcher::partPathFor(const QString &videoId) const {
    return QDir(m_settings.youtubeCacheDir()).absoluteFilePath(videoId + ".part.mp4");
}

bool YoutubeFetcher::ensureCacheDir() const {
    QDir dir(m_settings.youtubeCacheDir());
    if (dir.exists())
        return true;
    if (QDir().mkpath(dir.absolutePath()))
        return true;
    m_logger->error("{} Could not create cache directory: {}", m_loggingPrefix, dir.absolutePath());
    return false;
}

void YoutubeFetcher::setAvailable(const bool available) {
    if (m_available == available)
        return;
    m_available = available;
    m_logger->info("{} yt-dlp availability changed to: {}", m_loggingPrefix, available);
    emit availabilityChanged(available);
}

void YoutubeFetcher::refreshAvailability() {
    probeAvailability();
}

void YoutubeFetcher::probeAvailability() {
    if (m_versionProcess)
        return;
    const QString path = dlpPath();
    if (!QFileInfo::exists(path)) {
        m_logger->warn("{} yt-dlp not found at: {}", m_loggingPrefix, path);
        m_version.clear();
        setAvailable(false);
        return;
    }

    m_versionProcess = new QProcess(this);
    connect(m_versionProcess, &QProcess::finished, this, [this](const int exitCode, QProcess::ExitStatus) {
        const QString version = QString::fromUtf8(m_versionProcess->readAllStandardOutput()).trimmed();
        const bool ok = exitCode == 0 && !version.isEmpty();
        m_version = ok ? version : QString();
        if (ok)
            m_logger->info("{} yt-dlp version: {}", m_loggingPrefix, version);
        else
            m_logger->error("{} yt-dlp did not report a version, exit code {}", m_loggingPrefix, exitCode);
        m_versionProcess->deleteLater();
        m_versionProcess = nullptr;
        setAvailable(ok);
        if (ok)
            resumePending();
    });
    connect(m_versionProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        m_logger->error("{} Could not run yt-dlp at: {}", m_loggingPrefix, dlpPath());
        if (m_versionProcess) {
            m_versionProcess->deleteLater();
            m_versionProcess = nullptr;
        }
        setAvailable(false);
    });
    m_versionProcess->start(path, {QStringLiteral("--version")});
}

QStringList YoutubeFetcher::fetchArgs(const QString &videoId) const {
    const int maxHeight = m_settings.youtubeMaxHeight();
    // Prefer streams that already sit in an mp4-compatible container so the remux is a
    // container swap rather than a re-encode. The trailing fallbacks keep an oddly
    // encoded upload playable instead of failing the request outright.
    const QString format = QStringLiteral(
            "bv*[ext=mp4][height<=%1]+ba[ext=m4a]/b[ext=mp4][height<=%1]/bv*[height<=%1]+ba/b[height<=%1]/b")
            .arg(maxHeight);

    return {
            QStringLiteral("--newline"),
            QStringLiteral("--no-color"),
            QStringLiteral("--no-playlist"),
            QStringLiteral("--progress-template"),
            QStringLiteral("download:OKJPROG %(progress._percent_str)s"),
            QStringLiteral("-f"),
            format,
            QStringLiteral("--merge-output-format"),
            QStringLiteral("mp4"),
            QStringLiteral("--remux-video"),
            QStringLiteral("mp4"),
            QStringLiteral("-o"),
            partPathFor(videoId),
            QStringLiteral("--"),
            QStringLiteral("https://www.youtube.com/watch?v=") + videoId
    };
}

void YoutubeFetcher::enqueue(const QString &videoId, const int songId, const QString &requestedBy) {
    if (!isValidYoutubeVideoId(videoId)) {
        m_logger->error("{} Refusing to fetch invalid video id: {}", m_loggingPrefix, videoId);
        return;
    }
    if (isReady(videoId)) {
        emit fetchFinished(videoId, songId, true, {});
        return;
    }

    dbUpsert(videoId, songId, requestedBy);
    if (m_active.contains(videoId) || m_queued.contains(videoId))
        return;

    m_queued.enqueue(videoId);
    dispatchNext();
}

void YoutubeFetcher::cancel(const QString &videoId) {
    m_queued.removeAll(videoId);
    if (auto *job = m_active.value(videoId, nullptr); job && job->process) {
        m_logger->info("{} Cancelling fetch for: {}", m_loggingPrefix, videoId);
        job->process->kill();
    }
}

void YoutubeFetcher::dispatchNext() {
    if (!m_available)
        return;
    while (!m_queued.isEmpty() &&
           m_active.size() < static_cast<qsizetype>(m_settings.youtubeMaxConcurrentFetches())) {
        const QString videoId = m_queued.dequeue();
        if (m_active.contains(videoId))
            continue;

        QSqlQuery query;
        query.prepare("SELECT songid FROM local_youtube_fetches WHERE video_id = :video_id");
        query.bindValue(":video_id", videoId);
        if (!query.exec() || !query.next()) {
            m_logger->warn("{} No fetch row for queued video, dropping: {}", m_loggingPrefix, videoId);
            continue;
        }
        startJob(videoId, query.value(0).toInt());
    }
}

void YoutubeFetcher::startJob(const QString &videoId, const int songId) {
    if (!ensureCacheDir()) {
        dbSetState(videoId, State::Failed, 0, QStringLiteral("Cache directory is not writable"));
        emit fetchFinished(videoId, songId, false, QStringLiteral("Cache directory is not writable"));
        return;
    }

    // A leftover from a killed run would otherwise be resumed into an unknown state.
    QFile::remove(partPathFor(videoId));

    auto *job = new Job;
    job->videoId = videoId;
    job->songId = songId;
    job->started = QDateTime::currentDateTime();
    job->process = new QProcess(this);
    job->process->setProperty("videoId", videoId);
    m_active.insert(videoId, job);

    connect(job->process, &QProcess::readyReadStandardOutput, this, &YoutubeFetcher::onProcessReadyRead);
    connect(job->process, &QProcess::readyReadStandardError, this, &YoutubeFetcher::onProcessReadyRead);
    connect(job->process, &QProcess::finished, this, &YoutubeFetcher::onProcessFinished);
    connect(job->process, &QProcess::errorOccurred, this, &YoutubeFetcher::onProcessErrorOccurred);

    dbIncrementAttempts(videoId);
    dbSetState(videoId, State::Fetching, 0, {});
    m_logger->info("{} Fetching video {} for songid {}", m_loggingPrefix, videoId, songId);
    job->process->start(dlpPath(), fetchArgs(videoId));
}

void YoutubeFetcher::onProcessReadyRead() {
    auto *process = qobject_cast<QProcess *>(sender());
    if (!process)
        return;
    auto *job = m_active.value(process->property("videoId").toString(), nullptr);
    if (!job)
        return;

    const QString out = QString::fromUtf8(process->readAllStandardOutput());
    for (const auto &line: out.split('\n', Qt::SkipEmptyParts))
        parseProgress(job, line.trimmed());

    const QString err = QString::fromUtf8(process->readAllStandardError()).trimmed();
    if (!err.isEmpty()) {
        // Only the tail matters - it is what gets classified and shown to the singer -
        // and an unbounded buffer would grow for the life of a stuck download.
        job->stderrTail.append(err).append('\n');
        if (job->stderrTail.size() > 4000)
            job->stderrTail = job->stderrTail.right(4000);
    }
}

void YoutubeFetcher::parseProgress(Job *job, const QString &line) {
    if (!line.startsWith(QLatin1String("OKJPROG")))
        return;
    const QString value = line.mid(7).trimmed().remove('%');
    bool ok = false;
    const double percent = value.toDouble(&ok);
    if (!ok)
        return;

    const int clamped = std::clamp(static_cast<int>(percent), 0, 100);
    if (clamped <= job->reportedProgress)
        return;
    job->reportedProgress = clamped;
    dbSetState(job->videoId, State::Fetching, clamped, {});
    emit fetchProgress(job->videoId, clamped);
}

void YoutubeFetcher::onProcessErrorOccurred(QProcess::ProcessError error) {
    auto *process = qobject_cast<QProcess *>(sender());
    if (!process)
        return;
    auto *job = m_active.value(process->property("videoId").toString(), nullptr);
    if (!job)
        return;
    if (error == QProcess::FailedToStart) {
        setAvailable(false);
        finishJob(job, false, QStringLiteral("Could not run yt-dlp"));
    }
    // Every other error is followed by finished(), which does the cleanup.
}

void YoutubeFetcher::onProcessFinished(const int exitCode, QProcess::ExitStatus status) {
    auto *process = qobject_cast<QProcess *>(sender());
    if (!process)
        return;
    auto *job = m_active.value(process->property("videoId").toString(), nullptr);
    if (!job)
        return;

    if (status == QProcess::CrashExit) {
        finishJob(job, false, QStringLiteral("Download was interrupted"));
        return;
    }
    if (exitCode != 0) {
        const QString tail = job->stderrTail.trimmed();
        finishJob(job, false, tail.isEmpty() ? QStringLiteral("Download failed") : tail.section('\n', -1));
        return;
    }

    // Both the format selection and --remux-video ask for mp4, so anything else here
    // means the assumption behind the deterministic cache path no longer holds. Fail
    // loudly rather than renaming a mystery container into place.
    const QString part = partPathFor(job->videoId);
    const QString finalPath = cachePathFor(job->videoId);
    if (!QFileInfo::exists(part)) {
        m_logger->error("{} yt-dlp reported success but {} is missing", m_loggingPrefix, part);
        finishJob(job, false, QStringLiteral("Downloaded file was not where it was expected"));
        return;
    }

    QFile::remove(finalPath);
    if (!QFile::rename(part, finalPath)) {
        m_logger->error("{} Could not move {} into place at {}", m_loggingPrefix, part, finalPath);
        QFile::remove(part);
        finishJob(job, false, QStringLiteral("Could not save the downloaded file"));
        return;
    }

    updateSongDuration(job->songId, finalPath);
    m_logger->info("{} Fetched video {} to {}", m_loggingPrefix, job->videoId, finalPath);
    finishJob(job, true, {});
}

void YoutubeFetcher::finishJob(Job *job, const bool ok, const QString &error) {
    const QString videoId = job->videoId;
    const int songId = job->songId;

    if (job->process) {
        job->process->disconnect(this);
        job->process->deleteLater();
        job->process = nullptr;
    }
    m_active.remove(videoId);
    delete job;

    if (ok) {
        dbSetState(videoId, State::Ready, 100, {});
        // Runs after the new file is on disk and marked ready, so the cache it is
        // measuring includes it. The video just fetched is queued unsung, so the
        // sweeps cannot turn round and delete it.
        runCacheMaintenance();
    } else {
        QFile::remove(partPathFor(videoId));
        dbSetState(videoId, State::Failed, 0, error);
        m_logger->error("{} Fetch failed for {}: {}", m_loggingPrefix, videoId, error);
        maybeSelfUpdate(error);
    }

    emit fetchFinished(videoId, songId, ok, error);
    dispatchNext();
}

void YoutubeFetcher::sweepTimeouts() {
    const auto now = QDateTime::currentDateTime();
    QStringList expired;
    for (auto *job: m_active) {
        if (job->started.secsTo(now) > kJobTimeoutSecs)
            expired.append(job->videoId);
    }
    for (const auto &videoId: expired) {
        m_logger->warn("{} Fetch timed out after {}s: {}", m_loggingPrefix, kJobTimeoutSecs, videoId);
        if (auto *job = m_active.value(videoId, nullptr); job && job->process)
            job->process->kill();
    }
}

void YoutubeFetcher::maybeSelfUpdate(const QString &stderrTail) {
    const QString lowered = stderrTail.toLower();
    for (const auto &pattern: kVideoSpecificFailures) {
        if (lowered.contains(pattern))
            return;
    }
    if (m_updateProcess || !m_active.isEmpty())
        return;
    if (m_lastUpdateAttempt.isValid() && m_lastUpdateAttempt.secsTo(QDateTime::currentDateTime()) < kUpdateCooldownSecs)
        return;

    m_logger->info("{} Fetch failure looks like extractor breakage, updating yt-dlp", m_loggingPrefix);
    runUpdate();
}

void YoutubeFetcher::updateNow() {
    if (m_updateProcess) {
        return;
    }
    m_logger->info("{} Operator requested a yt-dlp update", m_loggingPrefix);
    runUpdate();
}

void YoutubeFetcher::runUpdate() {
    m_lastUpdateAttempt = QDateTime::currentDateTime();

    m_updateProcess = new QProcess(this);
    connect(m_updateProcess, &QProcess::finished, this, [this](const int exitCode, QProcess::ExitStatus) {
        const QString out = QString::fromUtf8(m_updateProcess->readAllStandardOutput()).trimmed();
        const QString err = QString::fromUtf8(m_updateProcess->readAllStandardError()).trimmed();
        m_updateProcess->deleteLater();
        m_updateProcess = nullptr;
        if (exitCode != 0) {
            m_logger->error("{} yt-dlp self-update failed with exit code {}", m_loggingPrefix, exitCode);
            // Overwhelmingly the cause on Windows: yt-dlp replaces its own executable,
            // which it cannot do from a directory the user has no write access to.
            emit updateFinished(false, err.isEmpty()
                                       ? tr("The update failed. If yt-dlp lives somewhere write-protected, "
                                            "move it to a folder you own and try again.")
                                       : err.section('\n', -1));
            return;
        }
        m_logger->info("{} yt-dlp self-update finished: {}", m_loggingPrefix, out);
        emit updateFinished(true, out.isEmpty() ? tr("yt-dlp is up to date.") : out.section('\n', -1));
        // Picks up the new version string, and re-enables the feature if the previous
        // binary was too broken to answer --version.
        refreshAvailability();
        requeueAfterUpdate();
    });
    connect(m_updateProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        m_logger->error("{} Could not run the yt-dlp self-update", m_loggingPrefix);
        if (m_updateProcess) {
            m_updateProcess->deleteLater();
            m_updateProcess = nullptr;
        }
        emit updateFinished(false, tr("Could not run yt-dlp. Check the path in settings."));
    });
    m_updateProcess->start(dlpPath(), {QStringLiteral("--update-to"), m_settings.youtubeDlpChannel()});
}

void YoutubeFetcher::requeueAfterUpdate() {
    QSqlQuery query;
    query.prepare("SELECT video_id FROM local_youtube_fetches WHERE state = 'failed' AND attempts < :max");
    query.bindValue(":max", kMaxAttempts);
    if (!query.exec())
        return;

    QStringList retry;
    while (query.next())
        retry.append(query.value(0).toString());

    for (const auto &videoId: retry) {
        if (m_active.contains(videoId) || m_queued.contains(videoId))
            continue;
        m_logger->info("{} Retrying {} after yt-dlp update", m_loggingPrefix, videoId);
        dbSetState(videoId, State::Pending, 0, {});
        m_queued.enqueue(videoId);
    }
    dispatchNext();
}

void YoutubeFetcher::resumePending() {
    QSqlQuery query;
    if (!query.exec("SELECT video_id FROM local_youtube_fetches WHERE state = 'pending' ORDER BY updated_at"))
        return;

    int resumed = 0;
    while (query.next()) {
        const QString videoId = query.value(0).toString();
        if (m_active.contains(videoId) || m_queued.contains(videoId))
            continue;
        m_queued.enqueue(videoId);
        resumed++;
    }
    if (resumed > 0)
        m_logger->info("{} Resuming {} pending fetch(es)", m_loggingPrefix, resumed);
    dispatchNext();
}

YoutubeFetcher::Status YoutubeFetcher::status(const QString &videoId) const {
    QSqlQuery query;
    query.prepare("SELECT state, progress, error FROM local_youtube_fetches WHERE video_id = :video_id");
    query.bindValue(":video_id", videoId);
    if (!query.exec() || !query.next())
        return {};

    Status status;
    status.known = true;
    status.state = stateFromString(query.value(0).toString());
    status.progress = query.value(1).toInt();
    status.error = query.value(2).toString();
    return status;
}

bool YoutubeFetcher::isReady(const QString &videoId) const {
    // The row and the file have to agree. A cache the KJ cleaned out by hand would
    // otherwise keep reporting songs as playable that are no longer there.
    if (status(videoId).state != State::Ready)
        return false;
    return QFileInfo::exists(cachePathFor(videoId));
}

QVector<YoutubeFetcher::CacheEntry> YoutubeFetcher::cacheEntries() const {
    QVector<CacheEntry> entries;
    QSqlQuery query;
    // Only 'ready' rows have a file. EXISTS rather than a join so a video queued by
    // two singers still yields one row.
    if (!query.exec("SELECT f.video_id, d.songid, d.artist, d.title, d.path, "
                    "COALESCE(d.plays, 0), d.lastplay, "
                    "EXISTS(SELECT 1 FROM queuesongs q WHERE q.song = d.songid AND q.played = 0) "
                    "FROM local_youtube_fetches f "
                    "INNER JOIN dbsongs d ON d.songid = f.songid "
                    "WHERE f.state = 'ready'")) {
        return entries;
    }

    while (query.next()) {
        CacheEntry entry;
        entry.videoId = query.value(0).toString();
        entry.songId = query.value(1).toInt();
        entry.artist = query.value(2).toString();
        entry.title = query.value(3).toString();
        entry.path = query.value(4).toString();
        entry.plays = query.value(5).toInt();
        entry.lastPlay = query.value(6).toDateTime();
        entry.queued = query.value(7).toBool();
        // A file the KJ deleted by hand reads as zero bytes and sorts to the front of
        // the eviction sweeps, which is where it belongs.
        entry.bytes = QFileInfo(entry.path).size();
        entries.append(entry);
    }

    std::sort(entries.begin(), entries.end(), [](const CacheEntry &a, const CacheEntry &b) {
        return a.lastPlay > b.lastPlay;
    });
    return entries;
}

qint64 YoutubeFetcher::cacheBytes() const {
    qint64 total = 0;
    for (const auto &entry: cacheEntries())
        total += entry.bytes;
    return total;
}

bool YoutubeFetcher::evict(const QString &videoId, QString *error) {
    const auto setError = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };

    if (m_active.contains(videoId))
        return setError(tr("That video is downloading right now."));

    QSqlQuery check;
    check.prepare("SELECT EXISTS(SELECT 1 FROM queuesongs q "
                  "INNER JOIN local_youtube_fetches f ON f.songid = q.song "
                  "WHERE f.video_id = :video_id AND q.played = 0)");
    check.bindValue(":video_id", videoId);
    if (check.exec() && check.next() && check.value(0).toBool())
        return setError(tr("That video is in a singer's queue."));

    const QString path = cachePathFor(videoId);
    if (!path.isEmpty() && QFileInfo::exists(path) && !QFile::remove(path)) {
        m_logger->error("{} Could not delete cached video {}", m_loggingPrefix, path);
        return setError(tr("The file could not be deleted. It may be in use."));
    }

    // Forget the download but keep the dbsongs row: it is referenced elsewhere and it
    // holds the play counts this policy runs on. Without a fetch row the song reads
    // as not playable, and a fresh request downloads it again.
    QSqlQuery remove;
    remove.prepare("DELETE FROM local_youtube_fetches WHERE video_id = :video_id");
    remove.bindValue(":video_id", videoId);
    remove.exec();
    m_logger->info("{} Evicted cached video {}", m_loggingPrefix, videoId);
    return true;
}

int YoutubeFetcher::runCacheMaintenance() {
    if (!m_settings.youtubeRequestsEnabled())
        return 0;

    auto entries = cacheEntries();
    qint64 total = 0;
    for (const auto &entry: entries)
        total += entry.bytes;

    const qint64 cap = static_cast<qint64>(m_settings.youtubeCacheMaxGb()) * 1024 * 1024 * 1024;
    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-m_settings.youtubeCacheKeepDays());
    int removed = 0;

    const auto drop = [&](const CacheEntry &entry) {
        if (entry.queued || m_active.contains(entry.videoId))
            return false;
        if (!evict(entry.videoId))
            return false;
        total -= entry.bytes;
        removed++;
        return true;
    };

    // Age sweep. A video sung once was a one-off request; one sung again has earned
    // its place and is only ever dropped by the size sweep below. An invalid lastPlay
    // means it was downloaded and never sung at all.
    QVector<CacheEntry> survivors;
    for (const auto &entry: entries) {
        const bool stale = !entry.lastPlay.isValid() || entry.lastPlay < cutoff;
        if (entry.plays <= 1 && stale && drop(entry))
            continue;
        survivors.append(entry);
    }

    // Size sweep. This one ignores plays and age - a cap that spares popular videos
    // is not a cap. Least recently played goes first.
    if (total > cap) {
        std::sort(survivors.begin(), survivors.end(), [](const CacheEntry &a, const CacheEntry &b) {
            return a.lastPlay < b.lastPlay;
        });
        for (const auto &entry: survivors) {
            if (total <= cap)
                break;
            drop(entry);
        }
    }

    if (removed > 0) {
        m_logger->info("{} Cache maintenance removed {} video(s), {} MB now in use", m_loggingPrefix, removed,
                       total / (1024 * 1024));
    }
    return removed;
}

int YoutubeFetcher::pendingCount() const {
    QSqlQuery query;
    if (query.exec("SELECT COUNT(1) FROM local_youtube_fetches WHERE state IN ('pending', 'fetching')") &&
        query.next()) {
        return query.value(0).toInt();
    }
    return 0;
}

void YoutubeFetcher::dbUpsert(const QString &videoId, const int songId, const QString &requestedBy) {
    QSqlQuery query;
    // A fresh request for a video that failed before starts it over from a clean slate:
    // the retry cap exists to stop the automatic post-update retries looping, not to
    // refuse a singer who asks again. A row that is already fetching is left alone -
    // resetting it would orphan the job that is running right now.
    query.prepare("INSERT INTO local_youtube_fetches (video_id, songid, state, progress, error, attempts, requested_by, updated_at) "
                  "VALUES (:video_id, :songid, 'pending', 0, NULL, 0, :requested_by, :updated_at) "
                  "ON CONFLICT(video_id) DO UPDATE SET songid = excluded.songid, requested_by = excluded.requested_by, "
                  "updated_at = excluded.updated_at, "
                  "state = CASE WHEN local_youtube_fetches.state = 'failed' THEN 'pending' "
                  "ELSE local_youtube_fetches.state END, "
                  "progress = CASE WHEN local_youtube_fetches.state = 'failed' THEN 0 "
                  "ELSE local_youtube_fetches.progress END, "
                  "error = CASE WHEN local_youtube_fetches.state = 'failed' THEN NULL "
                  "ELSE local_youtube_fetches.error END, "
                  "attempts = CASE WHEN local_youtube_fetches.state = 'failed' THEN 0 "
                  "ELSE local_youtube_fetches.attempts END");
    query.bindValue(":video_id", videoId);
    query.bindValue(":songid", songId);
    query.bindValue(":requested_by", requestedBy);
    query.bindValue(":updated_at", QDateTime::currentSecsSinceEpoch());
    if (!query.exec())
        m_logger->error("{} Could not record fetch row for {}: {}", m_loggingPrefix, videoId,
                        query.lastError().text());
}

void YoutubeFetcher::dbSetState(const QString &videoId, const State state, const int progress, const QString &error) {
    QSqlQuery query;
    query.prepare("UPDATE local_youtube_fetches SET state = :state, progress = :progress, error = :error, "
                  "updated_at = :updated_at WHERE video_id = :video_id");
    query.bindValue(":state", stateToString(state));
    query.bindValue(":progress", progress);
    query.bindValue(":error", error.isEmpty() ? QVariant() : QVariant(error));
    query.bindValue(":updated_at", QDateTime::currentSecsSinceEpoch());
    query.bindValue(":video_id", videoId);
    query.exec();
}

void YoutubeFetcher::dbIncrementAttempts(const QString &videoId) {
    QSqlQuery query;
    query.prepare("UPDATE local_youtube_fetches SET attempts = attempts + 1 WHERE video_id = :video_id");
    query.bindValue(":video_id", videoId);
    query.exec();
}

void YoutubeFetcher::updateSongDuration(const int songId, const QString &path) {
    TagReader reader;
    reader.taglibTags(path);
    const auto duration = static_cast<int>(reader.getDuration());
    if (duration <= 0) {
        m_logger->warn("{} Could not read a duration from {}, leaving the requested value in place",
                       m_loggingPrefix, path);
        return;
    }

    QSqlQuery query;
    query.prepare("UPDATE dbsongs SET duration = :duration WHERE songid = :songid");
    query.bindValue(":duration", duration);
    query.bindValue(":songid", songId);
    query.exec();
}
