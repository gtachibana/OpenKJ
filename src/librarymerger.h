#ifndef LIBRARYMERGER_H
#define LIBRARYMERGER_H

#include <QString>
#include <QStringList>
#include <memory>
#include <spdlog/spdlog.h>
#include <spdlog/async_logger.h>

#include "okjfmt.h"

// Settles the case where renaming a song's files would land on a name another
// set of files already holds.
//
// Two files wanting one name means the library holds the same song twice under
// two spellings, so the pair is collapsed to one. Which set survives is decided
// by the database, not by the disk: the set already carrying the wanted name
// keeps it, and the other is disposed of. The exception is a file nothing has
// imported - an orphan sitting on the name is the one that goes, and the song
// that provoked the collision gets the name it asked for.
//
// How the losing set is disposed of turns on whether the two are actually the
// same recording, which is decided by comparing the graphics byte for byte - the
// CDG for a loose pair, the file itself for a zip or a video. Identical graphics
// mean the sets are interchangeable, so the spare goes to the Recycle Bin and
// the bigger of the two audio files is kept. Differing graphics mean they are
// two different cuts that merely share an artist and title, so nothing is thrown
// away: the losing set is moved to a review folder to be looked at later.
//
// Nothing here is destructive in the unrecoverable sense. The Recycle Bin holds
// what it is given, the review folder is a plain directory, and every disposal
// is written to the caller's journal.
class LibraryMerger
{
public:
    // What happens to the losing set of files.
    enum class Disposal {
        Recycled,  // graphics matched byte for byte, so the spare went to the Recycle Bin
        SetAside   // graphics differed, so the set was moved to the review folder
    };

    // One collision, already inspected and resolved on paper.
    struct Conflict {
        // The set being disposed of.
        int loserSongId{0};    // its dbSongs row, 0 when nothing has imported it
        QString loserMain;
        QString loserAudio;    // its audio, for a loose CDG - empty otherwise

        // The set that keeps the name.
        int winnerSongId{0};   // 0 when the keeper is an unimported file
        QString winnerMain;    // where it is *now*; a later rename may move it again
        QString winnerAudio;

        Disposal disposal{Disposal::Recycled};

        // Identical graphics and the loser's audio is the bigger of the two, so
        // the keeper's audio is replaced with it rather than the loser's being
        // binned. Only ever set alongside Disposal::Recycled - when the graphics
        // differ the two audio files are not interchangeable.
        bool promoteLoserAudio{false};

        // The keeper is a file no dbSongs row points at, so the collision is
        // settled by disposing of *it*. Nothing needs repointing and the rename
        // that provoked this still goes ahead.
        [[nodiscard]] bool losingAnOrphan() const { return loserSongId == 0; }
    };

    explicit LibraryMerger(std::shared_ptr<spdlog::logger> logger = nullptr);

    // Decides how a collision should be settled. Reads bytes off the disk and
    // consults dbSongs; changes nothing.
    //
    // mainWantingName is the song asking for the name - it has a dbSongs row by
    // definition. occupantMain is whatever already holds that name. Returns
    // false, with the reason in error, when the collision cannot be settled at
    // all: the two paths resolve to the same file, or the graphics could not be
    // read to compare them.
    [[nodiscard]] bool inspect(int songIdWantingName, const QString &mainWantingName,
                               const QString &occupantMain, Conflict &conflict, QString &error);

    // Settling a conflict is deliberately split in two, and the halves run on
    // opposite sides of the caller's COMMIT.
    //
    // The database half first, inside the transaction: it can be rolled back,
    // and a conflict that cannot be folded should not cost anybody their files.
    // The disk half only once that commit has landed, because the Recycle Bin
    // does not give anything back on request - if the transaction were to roll
    // back afterwards, the rows would return pointing at files that are gone.
    //
    // Deferring it that way trades one failure for a milder one: if disposal
    // fails after the commit, the losing files are still sitting there with no
    // row of their own, and the next library scan simply imports them again.
    // Visible and fixable, rather than silent and broken.
    [[nodiscard]] bool foldRow(const Conflict &conflict, QString &error);

    // reviewDir is consulted for Disposal::SetAside, and for the fallback when
    // the Recycle Bin refuses a file - a network share, or a volume without one.
    // It is created if it does not exist.
    [[nodiscard]] bool disposeFiles(const Conflict &conflict, const QString &reviewDir, QString &error);

    // A line describing what apply() did, for the confirm dialog and the journal.
    [[nodiscard]] static QString describe(const Conflict &conflict);

    // The folder the losing sets go to when their graphics differ. Returns the
    // configured folder when one is set, otherwise a sibling of the source
    // directory holding songPath - same volume, so moving a set is a rename
    // rather than a multi-gigabyte copy.
    [[nodiscard]] static QString resolveReviewDir(const QString &songPath);

    // Whether two files hold the same bytes. Compares sizes first, then streams
    // both in step and gives up at the first difference.
    [[nodiscard]] static bool filesIdentical(const QString &a, const QString &b);

    // Whether two paths name one and the same file on disk.
    //
    // A rename that changes nothing but capitalisation - which is what fixing a
    // shouted artist name amounts to - looks exactly like a collision: Windows
    // reports the target as already existing, because it does not tell "BEYONCE"
    // from "Beyonce". On a case-sensitive filesystem those are two files and the
    // collision is real. Nothing about the strings can tell those two situations
    // apart, and neither can canonical paths without relying on unstated
    // behaviour, so the filesystem is asked outright: same volume and same file
    // index (or device and inode) means one file.
    //
    // False if either path cannot be opened, which keeps an unreadable file from
    // being mistaken for a match.
    [[nodiscard]] static bool sameFileOnDisk(const QString &a, const QString &b);

private:
    std::string m_loggingPrefix{"[LibraryMerger]"};
    std::shared_ptr<spdlog::logger> m_logger;

    // Sends a file to the Recycle Bin, falling back to the review folder when
    // the volume has not got one. Records what it did in movedTo.
    bool discard(const QString &path, const QString &reviewDir, QString &movedTo, QString &error);

    // Moves a file into the review folder under baseName + its own extension,
    // stepping the name aside if something of that name is already there.
    static bool moveAside(const QString &path, const QString &reviewDir, const QString &baseName,
                          QString &movedTo, QString &error);
};

#endif // LIBRARYMERGER_H
