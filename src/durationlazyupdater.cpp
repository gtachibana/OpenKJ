#include "durationlazyupdater.h"

#include <QSqlQuery>
#include <QVariant>
#include "mzarchive.h"
#include "karaokefileinfo.h"


void LazyDurationUpdateWorker::getDurations(const QStringList &files) {
    if (files.isEmpty())
        return;
    std::string m_loggingPrefix{"[LazyDurationThread]"};
    std::shared_ptr<spdlog::logger> logger;
    logger = spdlog::get("logger");
    logger->info("{} Starting scan", m_loggingPrefix);
    MzArchive archive;
    KaraokeFileInfo parser;
    for (const auto &path : files)
    {
        unsigned int duration = 0;
        if (path.endsWith(".zip", Qt::CaseInsensitive))
        {
            archive.setArchiveFile(path);
            duration = archive.getSongDuration();
        }
        if (duration == 0)
        {
            parser.setFile(path);
            duration = parser.getDuration();
        }
        if (duration == 0)
            logger->warn("{} Unable to get duration for file {}. - File is likely corrupted or invalid", m_loggingPrefix, path);
        else
            logger->trace("{} Got duration: {} for file: {}", m_loggingPrefix, duration, path);
        emit gotDuration(path, duration);
        if (QThread::currentThread()->isInterruptionRequested()) {
            logger->info("{} Scan interrupt requested", m_loggingPrefix);
            break;
        }
    }
    logger->info("{} Scan complete", m_loggingPrefix);
}

LazyDurationUpdateController::LazyDurationUpdateController(QObject *parent) : QObject(parent) {
    m_logger = spdlog::get("logger");
    auto *worker = new LazyDurationUpdateWorker;
    workerThread.setObjectName("DurationUpdater");
    worker->moveToThread(&workerThread);
    connect(&workerThread, &QThread::finished, worker, &QObject::deleteLater);
    connect(this, &LazyDurationUpdateController::operate, worker, &LazyDurationUpdateWorker::getDurations);
    connect(worker, &LazyDurationUpdateWorker::gotDuration, this, &LazyDurationUpdateController::updateDbDuration);
    m_flushTimer.setInterval(1000);
    connect(&m_flushTimer, &QTimer::timeout, this, &LazyDurationUpdateController::flushPendingUpdates);
    workerThread.start();
    workerThread.setPriority(QThread::IdlePriority);
}

LazyDurationUpdateController::~LazyDurationUpdateController() {
    workerThread.quit();
    workerThread.wait();
    flushPendingUpdates();
}

void LazyDurationUpdateController::getSongsRequiringUpdate()
{
    m_logger->info("{} Finding songs with missing durations", m_loggingPrefix);
    files.clear();
    QSqlQuery query;
    // Duration sentinels: -2 = never attempted (lazy add), -1 or 0 = attempted but
    // detection failed. Only pick up never-attempted files so unreadable ones aren't
    // re-scanned on every launch.
    query.exec("SELECT path FROM dbsongs WHERE duration = -2 ORDER BY artist, title");
    files.reserve(query.size());
    while (query.next())
    {
        files.append(query.value(0).toString());
    }
    m_logger->info("{} Done, found {} songs with missing durations", m_loggingPrefix, files.size());
}

void LazyDurationUpdateController::stopWork()
{
    workerThread.requestInterruption();
}

void LazyDurationUpdateController::updateDbDuration(const QString& file, int duration)
{
    // 0 means detection failed; store -1 so the file isn't retried on every launch.
    m_pendingUpdates.append({file, (duration == 0) ? -1 : duration});
    if (m_pendingUpdates.size() >= 500)
        flushPendingUpdates();
    else if (!m_flushTimer.isActive())
        m_flushTimer.start();
}

// Writes buffered duration results to the db in a single transaction and pushes
// them to the model in one batch, instead of one write and one O(n) model scan
// per song, which flooded the UI thread on large libraries.
void LazyDurationUpdateController::flushPendingUpdates()
{
    m_flushTimer.stop();
    if (m_pendingUpdates.isEmpty())
        return;
    const auto batch = std::move(m_pendingUpdates);
    m_pendingUpdates = {};
    QSqlQuery query;
    query.exec("BEGIN TRANSACTION");
    query.prepare("UPDATE dbsongs SET duration = :duration WHERE path = :path");
    for (const auto &update : batch) {
        query.bindValue(":path", update.first);
        query.bindValue(":duration", update.second);
        query.exec();
    }
    query.exec("COMMIT");
    m_logger->debug("{} Flushed {} duration updates to the database", m_loggingPrefix, batch.size());
    emit gotDurations(batch);
}

void LazyDurationUpdateController::getDurations()
{
    getSongsRequiringUpdate();
    emit operate(files);
}
