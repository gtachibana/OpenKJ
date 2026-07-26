#ifndef GAINLAZYUPDATER_H
#define GAINLAZYUPDATER_H

#include <QObject>
#include <QPair>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <QVector>
#include <spdlog/spdlog.h>
#include <spdlog/async_logger.h>
#include <spdlog/fmt/ostr.h>

#include "okjfmt.h"

// dbsongs.gain holds a ReplayGain track gain in dB. NULL means the file has never been
// analyzed; this sentinel means analysis ran and failed, so the file isn't retried on
// every launch. Real gains are only ever a handful of dB either way, so anything at or
// beyond kGainSentinelThreshold is a sentinel rather than a usable value.
inline constexpr double kGainAnalysisFailed = 999.0;
inline constexpr double kGainSentinelThreshold = 100.0;

// Reads the stored gain for a karaoke file, in dB. Returns 0 (no adjustment) when the
// song has not been analyzed yet, analysis failed, or the path isn't in the database.
double storedGainDb(const QString &path);

// Analyzes one file per invocation rather than a whole list, so the controller can
// interleave newly prioritized songs and suspend cleanly when playback starts.
class LazyGainUpdateWorker : public QObject
{
    Q_OBJECT
public slots:
    void analyzeFile(const QString &path);

signals:
    void gotGain(const QString &path, double gainDb);
};

class LazyGainUpdateController : public QObject
{
    Q_OBJECT
    QThread workerThread;
    QQueue<QString> m_pending;
    QSet<QString> m_pendingSet;
    bool m_busy{false};
    bool m_playbackActive{false};
    bool m_stopped{false};
    QTimer m_flushTimer;
    QVector<QPair<QString, double>> m_pendingUpdates;
    std::string m_loggingPrefix{"[LazyGainController]"};
    std::shared_ptr<spdlog::logger> m_logger;

    // How many library songs to hold in memory at once. A first run on a 100k+ song
    // library would otherwise pull every path into the queue up front.
    static constexpr int kSeedBatchSize = 5000;

    void enqueue(const QStringList &paths, bool front);
    void seedQueue();
    void dispatchNext();
    void flushPendingUpdates();

public:
    explicit LazyGainUpdateController(QObject *parent = nullptr);
    ~LazyGainUpdateController() override;

    // Seeds the work queue from the database - songs currently in the queue first, then
    // the rest of the library most-played first - and starts analyzing if idle.
    void getGains();
    void stopWork();

public slots:
    // Analysis fully decodes the file, so it only runs while karaoke playback is idle.
    // Anything else risks starving the playback pipeline in the middle of a show.
    void setPlaybackActive(bool active);
    // Moves songs to the front of the queue so a song about to be sung gets a gain
    // before it plays, instead of waiting for the background sweep to reach it.
    void prioritize(const QStringList &paths);
    void updateDbGain(const QString &path, double gainDb);

signals:
    void analyze(const QString &path);
};

#endif // GAINLAZYUPDATER_H
