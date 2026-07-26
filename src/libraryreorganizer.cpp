#include "libraryreorganizer.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTextStream>
#include <algorithm>
#include <array>

#include "okjutil.h"

namespace {

    // Kept in sync with DbUpdater::karaoke_file_extensions / audio_file_extensions.
    // These are the files that get a dbSongs row.
    const std::array<QString, 9> karaokeExtensions{
            "avi", "cdg", "m4v", "mkv", "mp4", "mpeg", "mpg", "wmv", "zip"
    };

    // These never get a row of their own; they only exist as the audio half of
    // a loose CDG pair.
    const std::array<QString, 5> audioExtensions{
            "flac", "mov", "mp3", "ogg", "wav"
    };

    // How many songs to move before committing their path updates. Bounds how
    // much has to be replayed from the journal if the run dies partway.
    constexpr int commitBatchSize = 500;

    QString csvEscape(const QString &field) {
        QString escaped = field;
        escaped.replace('"', "\"\"");
        return '"' + escaped + '"';
    }

}

LibraryReorganizer::LibraryReorganizer(QObject *parent) : QObject(parent) {
    m_logger = spdlog::get("logger");
}

size_t LibraryReorganizer::Plan::fileCount() const {
    size_t count{0};
    for (const auto &move : moves)
        count += move.fromAudio.isEmpty() ? 1 : 2;
    return count;
}

int LibraryReorganizer::Plan::largestBucket() const {
    int largest{0};
    for (const auto &count : distribution)
        largest = std::max(largest, count);
    return largest;
}

QString LibraryReorganizer::schemeName(Scheme scheme) {
    switch (scheme) {
        case Scheme::FilenameInitial:
            return QObject::tr("filename initial");
        case Scheme::ArtistInitial:
        default:
            return QObject::tr("artist initial");
    }
}

bool LibraryReorganizer::isKaraokeFile(const QString &suffixLower) {
    return std::find(karaokeExtensions.begin(), karaokeExtensions.end(), suffixLower) != karaokeExtensions.end();
}

bool LibraryReorganizer::isAudioFile(const QString &suffixLower) {
    return std::find(audioExtensions.begin(), audioExtensions.end(), suffixLower) != audioExtensions.end();
}

QString LibraryReorganizer::bucketFor(const QString &text) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return QStringLiteral("#");
    const QChar first = trimmed.at(0).toUpper();
    if (first.isDigit())
        return QStringLiteral("0-9");
    if (first >= QChar('A') && first <= QChar('Z'))
        return QString(first);
    return QStringLiteral("#");
}

QMap<QString, QString> LibraryReorganizer::loadArtistsByPath(const QString &sourceDir) const {
    QMap<QString, QString> artists;
    QSqlQuery query;
    query.prepare("SELECT path, artist FROM dbsongs WHERE path LIKE :prefix");
    query.bindValue(":prefix", QDir(sourceDir).absolutePath() + "%");
    query.exec();
    while (query.next())
        artists.insert(query.value(0).toString(), query.value(1).toString());
    return artists;
}

LibraryReorganizer::Plan LibraryReorganizer::plan(const QString &sourceDir, Scheme scheme) {
    Plan result;
    result.sourceDir = QDir(sourceDir).absolutePath();
    result.scheme = scheme;

    emit stateChanged(tr("Reading song database..."));
    QApplication::processEvents();
    const auto artistsByPath = loadArtistsByPath(result.sourceDir);

    emit stateChanged(tr("Scanning %1...").arg(result.sourceDir));
    QApplication::processEvents();

    // Only the top level - anything already in a subdirectory has been bucketed
    // by a previous run, or is organized some other way we shouldn't disturb.
    QDir dir(result.sourceDir);
    const auto entries = dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot);

    int examined{0};
    for (const auto &entry : entries) {
        examined++;
        if (examined % 500 == 0) {
            emit progressChanged(examined, static_cast<int>(entries.size()));
            QApplication::processEvents();
        }

        const QString suffix = entry.suffix().toLower();
        if (isAudioFile(suffix))
            continue; // moved as the companion of its CDG; a stray one is left alone
        if (!isKaraokeFile(suffix))
            continue;

        const QString mainPath = entry.absoluteFilePath();

        SongMove move;
        move.fromMain = mainPath;

        if (suffix == "cdg") {
            // Use playback's own matching rule so we move exactly the file that
            // will be looked for at play time.
            move.fromAudio = findMatchingAudioFile(mainPath);
            if (move.fromAudio.isEmpty()) {
                result.skipped.append(tr("%1 - no matching audio file").arg(entry.fileName()));
                continue;
            }
        }

        QString bucketKey;
        if (scheme == Scheme::ArtistInitial) {
            bucketKey = artistsByPath.value(mainPath);
            if (bucketKey.trimmed().isEmpty())
                bucketKey = entry.completeBaseName(); // not in the db, or artist never parsed
        } else {
            bucketKey = entry.completeBaseName();
        }

        const QString bucket = bucketFor(bucketKey);
        // Build with QDir so the result keeps Qt's '/' separator. dbSongs.path
        // is populated from QDirIterator, which always uses '/' - a path with
        // native '\' separators would compare unequal in DbUpdater's scan and
        // make the song look both missing and new on every subsequent update.
        const QString targetDir = QDir(result.sourceDir).absoluteFilePath(bucket);
        move.toMain = QDir(targetDir).absoluteFilePath(entry.fileName());

        if (QFile::exists(move.toMain)) {
            result.skipped.append(tr("%1 - a file of that name already exists in %2").arg(entry.fileName(), bucket));
            continue;
        }
        if (!move.fromAudio.isEmpty()) {
            move.toAudio = QDir(targetDir).absoluteFilePath(QFileInfo(move.fromAudio).fileName());
            if (QFile::exists(move.toAudio)) {
                result.skipped.append(
                        tr("%1 - its audio file already exists in %2").arg(entry.fileName(), bucket));
                continue;
            }
        }

        result.distribution[bucket]++;
        result.moves.push_back(move);
    }

    emit stateChanged(tr("Scan complete."));
    m_logger->info("{} Planned {} moves in {} ({} skipped)", m_loggingPrefix, result.moves.size(),
                   result.sourceDir.toStdString(), result.skipped.size());
    return result;
}

bool LibraryReorganizer::repointDatabase(const QString &oldPath, const QString &newPath, QString &error) {
    // dbSongs is the anchor, but queueSongs denormalizes the path at insert and
    // both history tables key their own lookups on it. Miss any of them and the
    // rows quietly orphan.
    static const std::array<QString, 4> statements{
            QStringLiteral("UPDATE dbsongs SET path = :new WHERE path = :old"),
            QStringLiteral("UPDATE queuesongs SET path = :new WHERE path = :old"),
            QStringLiteral("UPDATE dbSongHistory SET filepath = :new WHERE filepath = :old"),
            QStringLiteral("UPDATE historySongs SET filepath = :new WHERE filepath = :old")
    };

    QSqlQuery savepoint;
    savepoint.exec("SAVEPOINT song");

    for (const auto &statement : statements) {
        QSqlQuery query;
        query.prepare(statement);
        query.bindValue(":new", newPath);
        query.bindValue(":old", oldPath);
        if (!query.exec()) {
            error = query.lastError().text();
            savepoint.exec("ROLLBACK TO song");
            savepoint.exec("RELEASE song");
            return false;
        }
    }

    savepoint.exec("RELEASE song");
    return true;
}

bool LibraryReorganizer::execute(const Plan &plan) {
    m_errors.clear();

    if (plan.isEmpty())
        return true;

    QDir dataDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    m_journalPath = dataDir.absoluteFilePath(
            "reorg-" + QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss") + ".csv");
    QFile journal(m_journalPath);
    const bool journalOpen = journal.open(QIODevice::WriteOnly | QIODevice::Text);
    QTextStream journalStream(&journal);
    if (journalOpen)
        journalStream << "old_path,new_path\n";
    else
        m_logger->warn("{} Could not open move journal for writing: {}", m_loggingPrefix, m_journalPath.toStdString());

    emit stateChanged(tr("Moving files..."));
    QApplication::processEvents();

    QSqlQuery transaction;
    transaction.exec("BEGIN TRANSACTION");

    int moved{0};
    int sinceCommit{0};
    for (const auto &move : plan.moves) {
        const QString targetDir = QFileInfo(move.toMain).absolutePath();
        if (!QDir().mkpath(targetDir)) {
            m_errors.append(tr("Could not create directory: %1").arg(targetDir));
            break;
        }

        if (!QFile::rename(move.fromMain, move.toMain)) {
            m_errors.append(tr("Could not move %1").arg(move.fromMain));
            continue;
        }
        if (!move.fromAudio.isEmpty() && !QFile::rename(move.fromAudio, move.toAudio)) {
            // Never leave a CDG separated from its audio - put it back.
            QFile::rename(move.toMain, move.fromMain);
            m_errors.append(tr("Could not move %1, left the song where it was").arg(move.fromAudio));
            continue;
        }

        QString dbError;
        if (!repointDatabase(move.fromMain, move.toMain, dbError)) {
            // Never leave the database pointing at a path that no longer exists.
            QFile::rename(move.toMain, move.fromMain);
            if (!move.fromAudio.isEmpty())
                QFile::rename(move.toAudio, move.fromAudio);
            m_errors.append(tr("Database update failed for %1 (%2), left the song where it was")
                                    .arg(move.fromMain, dbError));
            continue;
        }

        if (journalOpen) {
            journalStream << csvEscape(move.fromMain) << ',' << csvEscape(move.toMain) << '\n';
            if (!move.fromAudio.isEmpty())
                journalStream << csvEscape(move.fromAudio) << ',' << csvEscape(move.toAudio) << '\n';
        }

        moved++;
        sinceCommit++;
        if (sinceCommit >= commitBatchSize) {
            transaction.exec("COMMIT");
            if (journalOpen)
                journalStream.flush();
            transaction.exec("BEGIN TRANSACTION");
            sinceCommit = 0;
        }

        if (moved % 100 == 0) {
            emit progressChanged(moved, static_cast<int>(plan.moves.size()));
            emit progressMessage(tr("Moved %1 of %2 songs...").arg(moved).arg(plan.moves.size()));
            QApplication::processEvents();
        }
    }

    transaction.exec("COMMIT");
    if (journalOpen) {
        journalStream.flush();
        journal.close();
    }

    emit progressChanged(static_cast<int>(plan.moves.size()), static_cast<int>(plan.moves.size()));
    emit stateChanged(tr("Move complete."));
    m_logger->info("{} Moved {} of {} songs, {} errors", m_loggingPrefix, moved, plan.moves.size(), m_errors.size());

    return m_errors.isEmpty();
}
