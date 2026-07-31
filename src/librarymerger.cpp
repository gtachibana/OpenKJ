#include "librarymerger.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QSqlError>
#include <QSqlQuery>
#include <algorithm>
#include <array>
#include <utility>
#include <vector>

#include "okjutil.h"
#include "settings.h"
#include "src/models/tablemodelkaraokesourcedirs.h"

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace {

    // Volume and file index on Windows, device and inode elsewhere. Two files
    // are the same file exactly when both halves match.
    using FileIdentity = std::array<quint64, 2>;

#ifdef Q_OS_WIN
    bool fileIdentity(const QString &path, FileIdentity &id) {
        // Opened asking for no access at all, which is enough to query identity
        // and, unlike a read handle, does not fail on a track something else is
        // busy playing. Sharing everything for the same reason.
        const HANDLE handle = CreateFileW(reinterpret_cast<const wchar_t *>(path.utf16()), 0,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                          OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return false;
        BY_HANDLE_FILE_INFORMATION info{};
        const bool ok = GetFileInformationByHandle(handle, &info);
        CloseHandle(handle);
        if (!ok)
            return false;
        id[0] = info.dwVolumeSerialNumber;
        id[1] = (static_cast<quint64>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
        return true;
    }
#else
    bool fileIdentity(const QString &path, FileIdentity &id) {
        struct stat info{};
        if (::stat(path.toLocal8Bit().constData(), &info) != 0)
            return false;
        id[0] = static_cast<quint64>(info.st_dev);
        id[1] = static_cast<quint64>(info.st_ino);
        return true;
    }
#endif

    // Big enough that comparing a video is not a syscall per kilobyte, small
    // enough that two of them sit comfortably in cache.
    constexpr qint64 compareChunkSize = 256 * 1024;

    // Where the losing sets go when nothing has been configured. A sibling of
    // the source directory rather than a child of it, so the importer does not
    // find the files again on its next pass and put back what was just removed.
    const QString defaultReviewDirName{QStringLiteral("OpenKJ Review")};

    // A file's companion audio, for a loose CDG. Empty for zips and videos,
    // which carry their audio inside.
    QString audioFor(const QString &mainPath) {
        if (QFileInfo(mainPath).suffix().compare("cdg", Qt::CaseInsensitive) != 0)
            return {};
        return findMatchingAudioFile(mainPath);
    }

    qint64 sizeOf(const QString &path) {
        return path.isEmpty() ? -1 : QFileInfo(path).size();
    }

    // A basename free for every one of these extensions at once, so a CDG and
    // its audio can be moved as a pair without the two being handed different
    // names. Steps " (2)", " (3)"... until one is clear.
    QString freeBaseName(const QDir &dir, const QString &wanted, const QStringList &suffixes) {
        const auto taken = [&](const QString &base) {
            return std::any_of(suffixes.begin(), suffixes.end(), [&](const QString &suffix) {
                return dir.exists(base + "." + suffix);
            });
        };
        if (!taken(wanted))
            return wanted;
        for (int n = 2; n < 1000; n++) {
            const QString candidate = wanted + QString(" (%1)").arg(n);
            if (!taken(candidate))
                return candidate;
        }
        return {};
    }

}

LibraryMerger::LibraryMerger(std::shared_ptr<spdlog::logger> logger) :
        m_logger(std::move(logger)) {
}

bool LibraryMerger::sameFileOnDisk(const QString &a, const QString &b) {
    FileIdentity idA{};
    FileIdentity idB{};
    if (!fileIdentity(a, idA) || !fileIdentity(b, idB))
        return false;
    return idA == idB;
}

bool LibraryMerger::filesIdentical(const QString &a, const QString &b) {
    const QFileInfo infoA(a);
    const QFileInfo infoB(b);
    if (!infoA.isFile() || !infoB.isFile())
        return false;
    // Two karaoke tracks that differ at all almost always differ in length, so
    // this settles nearly every comparison without opening anything.
    if (infoA.size() != infoB.size())
        return false;

    QFile fileA(a);
    QFile fileB(b);
    if (!fileA.open(QIODevice::ReadOnly) || !fileB.open(QIODevice::ReadOnly))
        return false;
    while (!fileA.atEnd()) {
        if (fileA.read(compareChunkSize) != fileB.read(compareChunkSize))
            return false;
    }
    // Equal sizes mean both streams run out together; if they have not, one of
    // the reads failed part way and the answer is not trustworthy.
    return fileB.atEnd();
}

QString LibraryMerger::resolveReviewDir(const QString &songPath) {
    Settings settings;
    const QString configured = settings.libraryReviewDir();
    if (!configured.isEmpty())
        return configured;

    // Nothing configured, so put it beside the source directory the song came
    // from. Same volume, which keeps moving a set a rename rather than a copy.
    TableModelKaraokeSourceDirs sourceDirs;
    const SourceDir sourceDir = sourceDirs.getDirByPath(songPath);
    const QString anchor = (sourceDir.getIndex() == -1) ? QFileInfo(songPath).absolutePath()
                                                        : sourceDir.getPath();
    QDir parent(anchor);
    // A source directory at the root of a volume has no parent to sit beside;
    // fall back to a child of it in that one case.
    if (parent.cdUp())
        return parent.absoluteFilePath(defaultReviewDirName);
    return QDir(anchor).absoluteFilePath(defaultReviewDirName);
}

bool LibraryMerger::inspect(int songIdWantingName, const QString &mainWantingName,
                            const QString &occupantMain, Conflict &conflict, QString &error) {
    const QFileInfo wantingInfo(mainWantingName);
    const QFileInfo occupantInfo(occupantMain);

    if (!occupantInfo.isFile()) {
        error = QObject::tr("nothing is actually holding that name");
        return false;
    }
    if (!wantingInfo.isFile()) {
        error = QObject::tr("the song's own file is missing");
        return false;
    }
    // A rename that changes nothing but capitalisation is the file arriving at
    // itself. Callers filter that out before getting here; this is the backstop,
    // and it matters - were one of these to get through, the file would be taken
    // for a duplicate of itself, compare identical to itself, and be recycled.
    if (sameFileOnDisk(mainWantingName, occupantMain)) {
        error = QObject::tr("the wanted name belongs to the same file");
        return false;
    }

    // Whoever the database says owns the occupied name decides which way round
    // this goes. A file nothing has imported is the one that gives way.
    int occupantSongId{0};
    QSqlQuery query;
    query.prepare("SELECT songid FROM dbSongs WHERE path = :path");
    query.bindValue(":path", occupantMain);
    if (!query.exec()) {
        error = query.lastError().text();
        return false;
    }
    if (query.next())
        occupantSongId = query.value(0).toInt();

    // Finding nothing is what authorises disposing of the file outright, so it
    // is worth being sure of. The path can be spelled differently to the way the
    // database holds it - a song part way through a change of capitalisation is
    // reached by its new name while its row still carries the old one - and
    // taking that for an unimported stray would bin a song out of the library.
    //
    // Only reached when the exact lookup misses, and every candidate is confirmed
    // by file identity, so a case-insensitive match cannot claim a file that
    // merely resembles this one.
    QString occupantPath = occupantMain;
    if (occupantSongId == 0) {
        QSqlQuery loose;
        loose.prepare("SELECT songid, path FROM dbSongs WHERE path = :path COLLATE NOCASE");
        loose.bindValue(":path", occupantMain);
        if (!loose.exec()) {
            error = loose.lastError().text();
            return false;
        }
        while (loose.next()) {
            if (sameFileOnDisk(loose.value(1).toString(), occupantMain)) {
                occupantSongId = loose.value(0).toInt();
                // The spelling the database holds, not the one we arrived by, so
                // that repointing lines up with every other row naming this song.
                occupantPath = loose.value(1).toString();
                break;
            }
        }
    }

    conflict = Conflict{};
    if (occupantSongId == 0) {
        conflict.loserSongId = 0;
        conflict.loserMain = occupantMain;
        conflict.winnerSongId = songIdWantingName;
        conflict.winnerMain = mainWantingName;
    } else {
        conflict.loserSongId = songIdWantingName;
        conflict.loserMain = mainWantingName;
        conflict.winnerSongId = occupantSongId;
        conflict.winnerMain = occupantPath;
    }
    conflict.loserAudio = audioFor(conflict.loserMain);
    conflict.winnerAudio = audioFor(conflict.winnerMain);

    // The graphics are what say whether these are the same recording. For a
    // loose pair that is the CDG; for a zip or a video the file carries both and
    // is compared whole.
    const bool identical = filesIdentical(conflict.loserMain, conflict.winnerMain);
    conflict.disposal = identical ? Disposal::Recycled : Disposal::SetAside;

    if (identical) {
        // Interchangeable graphics, so the audio is a free choice and the bigger
        // file is the better bet - a higher bitrate rip, or one that has not
        // been clipped short.
        const qint64 loserAudio = sizeOf(conflict.loserAudio);
        const qint64 winnerAudio = sizeOf(conflict.winnerAudio);
        conflict.promoteLoserAudio = (loserAudio > winnerAudio);
    }

    return true;
}

QString LibraryMerger::describe(const Conflict &conflict) {
    const QString loser = QFileInfo(conflict.loserMain).fileName();
    const QString winner = QFileInfo(conflict.winnerMain).fileName();
    if (conflict.disposal == Disposal::Recycled) {
        if (conflict.promoteLoserAudio) {
            return QObject::tr("%1 matches %2 - kept %2 and its bigger audio file, recycled the rest")
                    .arg(loser, winner);
        }
        return QObject::tr("%1 matches %2 - kept %2, recycled %1").arg(loser, winner);
    }
    return QObject::tr("%1 and %2 are different recordings - kept %2, moved %1 to the review folder")
            .arg(loser, winner);
}

bool LibraryMerger::moveAside(const QString &path, const QString &reviewDir, const QString &baseName,
                              QString &movedTo, QString &error) {
    QDir dir(reviewDir);
    if (!dir.exists() && !QDir().mkpath(reviewDir)) {
        error = QObject::tr("could not create the review folder %1").arg(reviewDir);
        return false;
    }
    const QString target = dir.absoluteFilePath(baseName + "." + QFileInfo(path).suffix());
    // QFile::rename copies and deletes when the two are on different volumes, so
    // a review folder somewhere else still works - it is just slower.
    if (!QFile::rename(path, target)) {
        error = QObject::tr("could not move %1 to %2").arg(QFileInfo(path).fileName(), reviewDir);
        return false;
    }
    movedTo = target;
    return true;
}

bool LibraryMerger::discard(const QString &path, const QString &reviewDir, QString &movedTo, QString &error) {
    // The member overload rather than the static one: on success it retargets
    // the QFile at where the file landed, which is what wants recording, and it
    // has not been through the deprecation the static form's out-parameter has.
    QFile file(path);
    if (file.moveToTrash()) {
        movedTo = file.fileName().isEmpty() ? QObject::tr("(Recycle Bin)") : file.fileName();
        return true;
    }
    // Network shares and some removable volumes have no Recycle Bin. Rather
    // than fall through to an unrecoverable delete, the file goes where the
    // differing-graphics case sends things.
    if (m_logger)
        m_logger->warn("{} Recycle Bin refused {}, moving it to the review folder instead", m_loggingPrefix,
                       path.toStdString());
    const QDir dir(reviewDir);
    const QString base = freeBaseName(dir, QFileInfo(path).completeBaseName(),
                                      QStringList{QFileInfo(path).suffix()});
    if (base.isEmpty()) {
        error = QObject::tr("could not find a free name in the review folder for %1")
                        .arg(QFileInfo(path).fileName());
        return false;
    }
    return moveAside(path, reviewDir, base, movedTo, error);
}

bool LibraryMerger::foldRow(const Conflict &conflict, QString &error) {
    // An orphan has no row to fold - nothing has imported it - so the whole
    // database half is a no-op and only its file has to go.
    if (conflict.losingAnOrphan())
        return true;

    QSqlQuery savepoint;
    savepoint.exec("SAVEPOINT merge_song");
    const auto fail = [&](const QString &text) {
        error = text;
        savepoint.exec("ROLLBACK TO merge_song");
        savepoint.exec("RELEASE merge_song");
        return false;
    };

    // queueSongs keys on the row id and denormalizes the path; the two history
    // tables key on the path alone; regulars and favorites key on the row id.
    // The losing row is about to disappear, so every one of them has to be moved
    // over to the keeper or it orphans.
    static const std::array<QString, 3> byId{
            QStringLiteral("UPDATE queueSongs SET song = :win, path = :winPath WHERE song = :lose"),
            QStringLiteral("UPDATE regularSongs SET songid = :win WHERE songid = :lose"),
            // The primary key is (user, song), so a singer who has favorited
            // both copies would collide. OR IGNORE leaves that row behind for
            // the delete below rather than failing the statement.
            QStringLiteral("UPDATE OR IGNORE local_user_favorites SET song_id = :win WHERE song_id = :lose")
    };
    for (const auto &statement : byId) {
        QSqlQuery query;
        query.prepare(statement);
        query.bindValue(":win", conflict.winnerSongId);
        query.bindValue(":lose", conflict.loserSongId);
        if (statement.contains(":winPath"))
            query.bindValue(":winPath", conflict.winnerMain);
        if (!query.exec())
            return fail(query.lastError().text());
    }

    QSqlQuery favorites;
    favorites.prepare("DELETE FROM local_user_favorites WHERE song_id = :lose");
    favorites.bindValue(":lose", conflict.loserSongId);
    if (!favorites.exec())
        return fail(favorites.lastError().text());

    static const std::array<QString, 2> byPath{
            QStringLiteral("UPDATE dbSongHistory SET filepath = :winPath WHERE filepath = :losePath"),
            QStringLiteral("UPDATE historySongs SET filepath = :winPath WHERE filepath = :losePath")
    };
    for (const auto &statement : byPath) {
        QSqlQuery query;
        query.prepare(statement);
        query.bindValue(":winPath", conflict.winnerMain);
        query.bindValue(":losePath", conflict.loserMain);
        if (!query.exec())
            return fail(query.lastError().text());
    }

    QSqlQuery drop;
    drop.prepare("DELETE FROM dbSongs WHERE songid = :lose");
    drop.bindValue(":lose", conflict.loserSongId);
    if (!drop.exec())
        return fail(drop.lastError().text());

    savepoint.exec("RELEASE merge_song");
    return true;
}

bool LibraryMerger::disposeFiles(const Conflict &conflict, const QString &reviewDir, QString &error) {
    // Moves already made, newest last, so a failure part way through can put
    // back whatever is still putable.
    std::vector<std::pair<QString, QString>> moved;
    const auto undo = [&moved]() {
        for (auto it = moved.rbegin(); it != moved.rend(); ++it)
            QFile::rename(it->second, it->first);
    };

    if (conflict.disposal == Disposal::SetAside) {
        // Two different recordings, so nothing is thrown away and the losing
        // pair travels together under one name.
        const QDir dir(reviewDir);
        QStringList suffixes{QFileInfo(conflict.loserMain).suffix()};
        if (!conflict.loserAudio.isEmpty())
            suffixes << QFileInfo(conflict.loserAudio).suffix();
        if (!dir.exists() && !QDir().mkpath(reviewDir)) {
            error = QObject::tr("could not create the review folder %1").arg(reviewDir);
            return false;
        }
        const QString base = freeBaseName(dir, QFileInfo(conflict.loserMain).completeBaseName(), suffixes);
        if (base.isEmpty()) {
            error = QObject::tr("could not find a free name in the review folder for %1")
                            .arg(QFileInfo(conflict.loserMain).fileName());
            return false;
        }

        QString movedTo;
        if (!moveAside(conflict.loserMain, reviewDir, base, movedTo, error)) {
            return false;
        }
        moved.emplace_back(conflict.loserMain, movedTo);
        if (!conflict.loserAudio.isEmpty()) {
            if (!moveAside(conflict.loserAudio, reviewDir, base, movedTo, error)) {
                // Never leave a CDG parted from its audio.
                undo();
                return false;
            }
            moved.emplace_back(conflict.loserAudio, movedTo);
        }
        if (m_logger)
            m_logger->info("{} {}", m_loggingPrefix, describe(conflict).toStdString());
        return true;
    }

    // Identical graphics. The winner's copy stands in for the loser's exactly,
    // so the loser's goes first: at no point after this is there a song without
    // its graphics.
    QString discarded;
    if (!discard(conflict.loserMain, reviewDir, discarded, error))
        return false;

    if (conflict.loserAudio.isEmpty()) {
        if (m_logger)
            m_logger->info("{} {}", m_loggingPrefix, describe(conflict).toStdString());
        return true;
    }

    if (!conflict.promoteLoserAudio) {
        if (!discard(conflict.loserAudio, reviewDir, discarded, error))
            return false;
        if (m_logger)
            m_logger->info("{} {}", m_loggingPrefix, describe(conflict).toStdString());
        return true;
    }

    // The loser's audio is the better of the two and is taking the winner's
    // place. The one being replaced steps aside under a temporary name rather
    // than being binned outright, so a failure half way leaves the song with
    // audio rather than none - the Recycle Bin would not give it back.
    QString parked;
    if (!conflict.winnerAudio.isEmpty()) {
        parked = conflict.winnerAudio + ".okjmerge";
        if (QFile::exists(parked))
            QFile::remove(parked);
        if (!QFile::rename(conflict.winnerAudio, parked)) {
            error = QObject::tr("could not set aside %1").arg(QFileInfo(conflict.winnerAudio).fileName());
            return false;
        }
    }

    // The promoted file has to answer to the winner's basename or
    // findMatchingAudioFile will never pair the two up again. Its own extension
    // is kept, since that is what the bytes are.
    const QString target = QDir(QFileInfo(conflict.winnerMain).absolutePath())
                                   .absoluteFilePath(QFileInfo(conflict.winnerMain).completeBaseName() + "."
                                                     + QFileInfo(conflict.loserAudio).suffix());
    if (!QFile::rename(conflict.loserAudio, target)) {
        error = QObject::tr("could not move %1 into place").arg(QFileInfo(conflict.loserAudio).fileName());
        if (!parked.isEmpty())
            QFile::rename(parked, conflict.winnerAudio);
        return false;
    }
    if (!parked.isEmpty() && !discard(parked, reviewDir, discarded, error)) {
        // The good audio is already in place, so this is untidiness rather than
        // damage - say so and let the caller log it.
        return false;
    }

    if (m_logger)
        m_logger->info("{} {}", m_loggingPrefix, describe(conflict).toStdString());
    return true;
}
