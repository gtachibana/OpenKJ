#include "gainlazyupdater.h"

#include <cmath>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QVariant>
#include <gst/gst.h>

#include "mzarchive.h"
#include "okjutil.h"

namespace {

// Analysis has to decode the whole track, so a file that stalls partway through would
// otherwise hold the worker forever. Nothing in a karaoke library legitimately takes
// this long to decode at full speed.
constexpr int kAnalysisTimeoutMs = 300000;

// Links decodebin's audio pad into the analysis chain. Karaoke video files carry a video
// stream too; leaving its pad unlinked lets decodebin drop it instead of stalling.
void gainPadAdded(GstElement *, GstPad *pad, gpointer userData)
{
    auto *converter = static_cast<GstElement *>(userData);
    GstPad *sinkPad = gst_element_get_static_pad(converter, "sink");
    if (!sinkPad)
        return;
    if (!gst_pad_is_linked(sinkPad))
    {
        GstCaps *caps = gst_pad_get_current_caps(pad);
        if (caps)
        {
            if (gst_caps_get_size(caps) > 0)
            {
                const gchar *name = gst_structure_get_name(gst_caps_get_structure(caps, 0));
                if (g_str_has_prefix(name, "audio/"))
                    gst_pad_link(pad, sinkPad);
            }
            gst_caps_unref(caps);
        }
    }
    gst_object_unref(sinkPad);
}

// Resolves the karaoke file the database knows about to the audio file that actually
// needs decoding. Zips are extracted into tmpDir, which the caller keeps alive.
QString audioFileToAnalyze(const QString &path, QTemporaryDir &tmpDir)
{
    if (path.endsWith(".zip", Qt::CaseInsensitive))
    {
        if (!tmpDir.isValid())
            return {};
        MzArchive archive(path);
        if (!archive.checkAudio())
            return {};
        const QString destFile = "gain" + archive.audioExtension();
        if (!archive.extractAudio(tmpDir.path(), destFile))
            return {};
        return tmpDir.path() + QDir::separator() + destFile;
    }
    if (path.endsWith(".cdg", Qt::CaseInsensitive))
        return findMatchingAudioFile(path);
    return path;
}

} // namespace

double storedGainDb(const QString &path)
{
    QSqlQuery query;
    query.prepare("SELECT gain FROM dbsongs WHERE path = :path");
    query.bindValue(":path", path);
    query.exec();
    if (!query.next())
        return 0.0;
    const QVariant gain = query.value(0);
    if (gain.isNull())
        return 0.0;
    const double gainDb = gain.toDouble();
    if (std::fabs(gainDb) >= kGainSentinelThreshold)
        return 0.0;
    return gainDb;
}

void LazyGainUpdateWorker::analyzeFile(const QString &path)
{
    const std::string loggingPrefix{"[LazyGainThread]"};
    auto logger = spdlog::get("logger");

    QTemporaryDir tmpDir;
    const QString audioPath = audioFileToAnalyze(path, tmpDir);
    if (audioPath.isEmpty() || !QFile::exists(audioPath))
    {
        logger->warn("{} No analyzable audio for {}", loggingPrefix, path);
        emit gotGain(path, kGainAnalysisFailed);
        return;
    }

    if (!gst_is_initialized())
        gst_init(nullptr, nullptr);

    GstElement *pipeline = gst_pipeline_new("gainAnalysisPipeline");
    GstElement *decoder = gst_element_factory_make("uridecodebin", "gainDecoder");
    GstElement *converter = gst_element_factory_make("audioconvert", "gainAConv");
    GstElement *resampler = gst_element_factory_make("audioresample", "gainAResample");
    GstElement *analysis = gst_element_factory_make("rganalysis", "gainAnalysis");
    GstElement *sink = gst_element_factory_make("fakesink", "gainSink");

    if (!pipeline || !decoder || !converter || !resampler || !analysis || !sink)
    {
        logger->warn("{} Unable to build the gain analysis pipeline - is the GStreamer replaygain plugin present?",
                     loggingPrefix);
        // Nothing has been added to the bin yet, so every element still owns its own ref.
        for (GstElement *element: {pipeline, decoder, converter, resampler, analysis, sink})
        {
            if (element)
                gst_object_unref(element);
        }
        emit gotGain(path, kGainAnalysisFailed);
        return;
    }

    gchar *uri = gst_filename_to_uri(audioPath.toLocal8Bit(), nullptr);
    g_object_set(decoder, "uri", uri, nullptr);
    g_free(uri);
    // Karaoke video files carry a video stream that nothing here consumes. Restricting
    // the decoder to audio keeps it from exposing - and waiting on - a video pad.
    GstCaps *audioCaps = gst_caps_from_string("audio/x-raw(ANY)");
    g_object_set(decoder, "caps", audioCaps, "expose-all-streams", FALSE, nullptr);
    gst_caps_unref(audioCaps);
    // num-tracks 1 makes rganalysis emit the track gain at end of stream instead of
    // holding out for an album's worth of tracks. sync off lets it run flat out rather
    // than at playback speed.
    g_object_set(analysis, "num-tracks", 1, nullptr);
    g_object_set(sink, "sync", FALSE, nullptr);

    gst_bin_add_many(GST_BIN(pipeline), decoder, converter, resampler, analysis, sink, nullptr);
    gst_element_link_many(converter, resampler, analysis, sink, nullptr);
    g_signal_connect(decoder, "pad-added", G_CALLBACK(gainPadAdded), converter);

    double gainDb = kGainAnalysisFailed;
    bool gotTag = false;
    GstBus *bus = gst_element_get_bus(pipeline);
    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE)
    {
        QElapsedTimer elapsed;
        elapsed.start();
        bool done = false;
        while (!done)
        {
            if (QThread::currentThread()->isInterruptionRequested())
            {
                logger->info("{} Analysis interrupt requested", loggingPrefix);
                gotTag = false;
                break;
            }
            if (elapsed.elapsed() > kAnalysisTimeoutMs)
            {
                logger->warn("{} Timed out analyzing {}", loggingPrefix, path);
                gotTag = false;
                break;
            }
            GstMessage *msg = gst_bus_timed_pop_filtered(
                    bus, 200 * GST_MSECOND,
                    static_cast<GstMessageType>(GST_MESSAGE_TAG | GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
            if (!msg)
                continue;
            switch (GST_MESSAGE_TYPE(msg))
            {
                case GST_MESSAGE_TAG:
                {
                    GstTagList *tags = nullptr;
                    gst_message_parse_tag(msg, &tags);
                    if (tags)
                    {
                        gdouble value;
                        if (gst_tag_list_get_double(tags, GST_TAG_TRACK_GAIN, &value))
                        {
                            gainDb = value;
                            gotTag = true;
                        }
                        gst_tag_list_unref(tags);
                    }
                    break;
                }
                case GST_MESSAGE_ERROR:
                {
                    GError *err = nullptr;
                    gst_message_parse_error(msg, &err, nullptr);
                    logger->warn("{} Error analyzing {}: {}", loggingPrefix, path,
                                 err && err->message ? err->message : "unknown error");
                    if (err)
                        g_error_free(err);
                    done = true;
                    break;
                }
                case GST_MESSAGE_EOS:
                default:
                    done = true;
                    break;
            }
            gst_message_unref(msg);
        }
    }
    else
    {
        logger->warn("{} Unable to start the gain analysis pipeline for {}", loggingPrefix, path);
    }

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(bus);
    gst_object_unref(pipeline);

    if (gotTag && std::fabs(gainDb) < kGainSentinelThreshold)
    {
        logger->trace("{} Got gain: {} dB for file: {}", loggingPrefix, gainDb, path);
        emit gotGain(path, gainDb);
    }
    else
    {
        emit gotGain(path, kGainAnalysisFailed);
    }
}

LazyGainUpdateController::LazyGainUpdateController(QObject *parent) : QObject(parent)
{
    m_logger = spdlog::get("logger");
    auto *worker = new LazyGainUpdateWorker;
    workerThread.setObjectName("GainUpdater");
    worker->moveToThread(&workerThread);
    connect(&workerThread, &QThread::finished, worker, &QObject::deleteLater);
    connect(this, &LazyGainUpdateController::analyze, worker, &LazyGainUpdateWorker::analyzeFile);
    connect(worker, &LazyGainUpdateWorker::gotGain, this, &LazyGainUpdateController::updateDbGain);
    m_flushTimer.setInterval(1000);
    connect(&m_flushTimer, &QTimer::timeout, this, &LazyGainUpdateController::flushPendingUpdates);
    workerThread.start();
    workerThread.setPriority(QThread::IdlePriority);
}

LazyGainUpdateController::~LazyGainUpdateController()
{
    workerThread.requestInterruption();
    workerThread.quit();
    workerThread.wait();
    flushPendingUpdates();
}

void LazyGainUpdateController::getGains()
{
    m_stopped = false;
    seedQueue();
}

// Pulls the next batch of unanalyzed songs out of the database. Batched rather than read
// in one go because a first run on a large library would otherwise hold every path in
// memory at once; seedQueue() is called again once the batch drains.
void LazyGainUpdateController::seedQueue()
{
    if (m_stopped)
        return;
    m_logger->info("{} Finding songs with no loudness data", m_loggingPrefix);

    // Songs already waiting in the rotation go first - those are the ones whose gain the
    // KJ will actually hear tonight.
    QStringList queued;
    QSqlQuery queuedQuery;
    queuedQuery.exec("SELECT DISTINCT dbsongs.path FROM queueSongs, dbsongs "
                     "WHERE queueSongs.song = dbsongs.songid AND queueSongs.played = 0 AND dbsongs.gain IS NULL");
    while (queuedQuery.next())
        queued.append(queuedQuery.value(0).toString());

    // Then the rest of the library, most-played first, so a partial sweep still covers
    // the songs most likely to come up.
    QStringList rest;
    QSqlQuery restQuery;
    restQuery.exec(QString("SELECT path FROM dbsongs WHERE gain IS NULL ORDER BY plays DESC, artist, title LIMIT %1")
                           .arg(kSeedBatchSize));
    while (restQuery.next())
        rest.append(restQuery.value(0).toString());

    enqueue(queued, true);
    enqueue(rest, false);
    m_logger->info("{} Done, queued {} songs for loudness analysis ({} of them waiting in the rotation)",
                   m_loggingPrefix, m_pending.size(), queued.size());
    dispatchNext();
}

// Leaves the worker thread's interruption flag alone: that flag can only be cleared by
// restarting the thread, so setting it here would permanently break a controller that
// gets stopped and started again (toggling the setting off and back on). Aborting an
// in-flight analysis is only needed at teardown, which the destructor handles.
void LazyGainUpdateController::stopWork()
{
    m_stopped = true;
    m_pending.clear();
    m_pendingSet.clear();
}

void LazyGainUpdateController::setPlaybackActive(bool active)
{
    if (m_playbackActive == active)
        return;
    m_playbackActive = active;
    if (active)
        m_logger->debug("{} Playback started, suspending loudness analysis", m_loggingPrefix);
    else
        dispatchNext();
}

void LazyGainUpdateController::prioritize(const QStringList &paths)
{
    if (m_stopped || paths.isEmpty())
        return;
    // Callers pass only songs that still need a gain, so everything here goes to the
    // front whether or not the current batch happened to contain it. On a large library
    // most requested songs won't be in the batch at all, and those are exactly the ones
    // that need jumping the line.
    for (auto it = paths.crbegin(); it != paths.crend(); ++it)
    {
        if (*it == m_inFlight)
            continue;
        m_pending.removeOne(*it);
        m_pendingSet.insert(*it);
        m_pending.prepend(*it);
    }
    dispatchNext();
}

// Prepending walks the list backwards so the caller's order survives into the queue.
void LazyGainUpdateController::enqueue(const QStringList &paths, bool front)
{
    if (front)
    {
        for (auto it = paths.crbegin(); it != paths.crend(); ++it)
        {
            if (m_pendingSet.contains(*it) || *it == m_inFlight)
                continue;
            m_pendingSet.insert(*it);
            m_pending.prepend(*it);
        }
        return;
    }
    for (const auto &path: paths)
    {
        if (m_pendingSet.contains(path) || path == m_inFlight)
            continue;
        m_pendingSet.insert(path);
        m_pending.append(path);
    }
}

void LazyGainUpdateController::dispatchNext()
{
    if (m_busy || m_stopped || m_playbackActive || m_pending.isEmpty())
        return;
    m_busy = true;
    m_inFlight = m_pending.dequeue();
    emit analyze(m_inFlight);
}

void LazyGainUpdateController::updateDbGain(const QString &path, double gainDb)
{
    m_busy = false;
    m_inFlight.clear();
    m_pendingSet.remove(path);
    m_pendingUpdates.append({path, gainDb});
    if (m_pendingUpdates.size() >= 50)
        flushPendingUpdates();
    else if (!m_flushTimer.isActive())
        m_flushTimer.start();
    dispatchNext();
    // Batch drained - go back to the database for the next one. Analyzed files come back
    // with a non-null gain (a sentinel if analysis failed), so the pool shrinks every
    // pass and this terminates instead of re-queuing the same songs.
    if (!m_busy && !m_stopped && m_pending.isEmpty())
    {
        flushPendingUpdates();
        seedQueue();
    }
}

// Batches writes the same way the duration updater does, so a long sweep doesn't hit the
// database once per song from the UI thread.
void LazyGainUpdateController::flushPendingUpdates()
{
    m_flushTimer.stop();
    if (m_pendingUpdates.isEmpty())
        return;
    const auto batch = std::move(m_pendingUpdates);
    m_pendingUpdates = {};
    QSqlQuery query;
    query.exec("BEGIN TRANSACTION");
    query.prepare("UPDATE dbsongs SET gain = :gain WHERE path = :path");
    for (const auto &update: batch)
    {
        query.bindValue(":path", update.first);
        query.bindValue(":gain", update.second);
        query.exec();
    }
    query.exec("COMMIT");
    m_logger->debug("{} Flushed {} gain updates to the database", m_loggingPrefix, batch.size());
}
