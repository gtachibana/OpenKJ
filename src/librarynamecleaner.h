#ifndef LIBRARYNAMECLEANER_H
#define LIBRARYNAMECLEANER_H

#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <memory>
#include <vector>
#include <spdlog/spdlog.h>
#include <spdlog/async_logger.h>

#include "librarymerger.h"
#include "okjfmt.h"

// Finds artist and title strings in dbSongs that look like variants of each
// other - "Beyonce" sitting alongside "Beyoncé", "Lynard Skynyrd" alongside
// "Lynyrd Skynyrd" - and proposes folding the rare spelling into the common one.
//
// Nothing here consults an external service. The library is its own authority:
// when one spelling covers 180 songs and a near-identical one covers 3, the rare
// one is almost certainly the typo. That count asymmetry is the entire signal,
// which is why every proposal replaces an existing library string with another
// existing library string. No spelling is ever invented, the one exception being
// stray whitespace, which is only ever removed.
//
// Corrections are written to dbSongs only - files on disk keep their names. A
// routine rescan imports by path and skips rows it already has
// (DbUpdater::process), so the edits survive one; clearing and rebuilding the
// database from scratch discards them.
class LibraryNameCleaner : public QObject
{
Q_OBJECT

public:
    enum class Field {
        Artist,
        Title
    };

    enum class Reason {
        Whitespace,   // leading, trailing or doubled spaces
        Variant,      // identical once case, accents and punctuation are ignored
        Misspelling   // one edit away from a far more common spelling
    };

    enum class Confidence {
        High,
        Medium
    };

    // One proposed rename. Applying it rewrites every dbSongs row whose field
    // holds oldValue exactly - and, for titles, whose artist is scopeArtist,
    // since the same title under a different artist is a different song.
    struct Proposal {
        Field field{Field::Artist};
        Reason reason{Reason::Variant};
        Confidence confidence{Confidence::Medium};
        QString scopeArtist;  // titles only: the artist this title sits under
        QString oldValue;
        QString newValue;
        int songs{0};         // rows carrying oldValue
        int keptSongs{0};     // rows already on the winning spelling
    };

    struct Plan {
        std::vector<Proposal> proposals;
        int scannedSongs{0};
        int distinctArtists{0};

        [[nodiscard]] bool isEmpty() const { return proposals.empty(); }
        [[nodiscard]] int songsAffected() const;
        [[nodiscard]] int countOf(Field field) const;
        [[nodiscard]] int countOf(Confidence confidence) const;
    };

    // One song's files, renamed to match its corrected name. A loose CDG carries
    // its audio companion along; zips and video files move on their own.
    struct Rename {
        int songId{0};
        QString fromMain;     // the file holding the dbSongs row
        QString toMain;
        QString fromAudio;    // companion audio for a loose CDG, empty otherwise
        QString toAudio;
        QString newBaseName;  // dbSongs.filename, extension stripped
        QString artist;       // the corrected values this filename encodes
        QString title;
        QString discId;
    };

    struct RenamePlan {
        std::vector<Rename> renames;
        // Songs whose corrected filename is already taken. Rather than being
        // left alone, the two sets are collapsed into one - see LibraryMerger.
        std::vector<LibraryMerger::Conflict> merges;
        // One line per song left alone, saying why. These are corrected in the
        // database anyway - only their files are untouched.
        QStringList keptAsIs;
        // The same thing counted by cause, so the question of whether any of it
        // matters can be answered from numbers rather than by scrolling a list.
        QMap<QString, int> keptAsIsByCause;

        [[nodiscard]] bool isEmpty() const { return renames.empty() && merges.empty(); }
        // How many of the merges bin the spare set, and how many set it aside
        // for a look later. The two read very differently in a confirm dialog.
        [[nodiscard]] int countOf(LibraryMerger::Disposal disposal) const;
    };

    // Works out which of the songs these proposals touch can have their files
    // renamed to match. Reads the database and stats the disk; changes nothing.
    //
    // A rename is only planned when the new filename provably reads back as the
    // corrected name. The parser is not the inverse of the namer - it folds
    // underscores into spaces and splits on " - ", so a name carrying either can
    // come back different or truncated - so every candidate is run through the
    // real import parser and dropped unless it round-trips exactly.
    [[nodiscard]] RenamePlan planRenames(const std::vector<Proposal> &proposals);

    // Applies the given proposals in a single transaction, rolling the lot back
    // if any statement fails. Returns the number of distinct songs rewritten -
    // a song whose artist and title are both corrected counts once. errors()
    // explains a zero.
    //
    // Files listed in renames are renamed as the transaction runs. A song whose
    // files will not move keeps its correction in the database regardless; that
    // is a warning, not a failure.
    int execute(const std::vector<Proposal> &proposals, const RenamePlan &renames);
    int execute(const std::vector<Proposal> &proposals) { return execute(proposals, RenamePlan{}); }

    explicit LibraryNameCleaner(QObject *parent = nullptr);

    // Reads dbSongs and works out what to propose. Writes nothing.
    [[nodiscard]] Plan plan();

    // Fatal problems - the transaction was rolled back and nothing changed.
    [[nodiscard]] const QStringList &errors() const { return m_errors; }

    // Songs whose files could not be renamed. The run still succeeded and their
    // names are corrected in the database.
    [[nodiscard]] const QStringList &warnings() const { return m_warnings; }

    // CSV of every applied change, written once the transaction commits so a run
    // can be traced or reversed by hand.
    [[nodiscard]] const QString &journalPath() const { return m_journalPath; }

    static QString reasonText(Reason reason);

    // Case, accents, punctuation and a leading "the" all folded away. Used only
    // to decide which strings are candidates for each other - never stored.
    static QString normalizeKey(const QString &value);

    // Collapses internal runs of whitespace and trims the ends. The one
    // transformation here that can produce a string not already in the library.
    static QString cleanWhitespace(const QString &value);

    // Optimal string alignment distance - Levenshtein plus transposition of
    // adjacent characters, so a swapped pair ("Beonyce") costs one edit rather
    // than two. Gives up once the distance exceeds maxDistance, returning
    // maxDistance + 1.
    static int editDistance(const QString &a, const QString &b, int maxDistance);

signals:
    void stateChanged(const QString &state);

private:
    std::string m_loggingPrefix{"[LibraryNameCleaner]"};
    std::shared_ptr<spdlog::logger> m_logger;
    QStringList m_errors;
    QStringList m_warnings;
    QString m_journalPath;

    // A distinct string in one of the two columns, with the number of songs
    // spelling it that way.
    struct Variant {
        QString value;
        int count{0};
    };

    // A group of variants that normalize to the same key, resolved down to the
    // one spelling the library mostly agrees on.
    struct Cluster {
        QString key;
        QString canonical;  // the winning spelling, whitespace already cleaned
        int totalCount{0};
    };

    // Groups variants by normalized key, appends a proposal for every member
    // that isn't already the winner, and returns the resolved clusters.
    std::vector<Cluster> resolveClusters(const std::vector<Variant> &variants,
                                         Field field,
                                         const QString &scopeArtist,
                                         std::vector<Proposal> &proposals) const;

    // Pairs up clusters whose keys are a character or two apart and wildly
    // lopsided in song count, and proposes folding the rare one in.
    void findMisspellings(const std::vector<Cluster> &clusters, std::vector<Proposal> &proposals) const;

    // Of two spellings with equal song counts, which one to keep.
    static bool preferAsCanonical(const Variant &candidate, const Variant &incumbent);

    // The digits of a string in order, "Vol 2" -> "2". Two names whose digits
    // differ are a numbering difference, not a typo.
    static QString digitsOf(const QString &value);

    // The filename a source directory's naming pattern calls for, extension
    // excluded. Empty when the pattern cannot express these values - a pattern
    // that encodes a song id, for a song that hasn't got one.
    static QString buildBaseName(int pattern, const QString &artist, const QString &title, const QString &discId);

    // Whether Windows will accept this as a filename at all. Reserved
    // characters, trailing dots and spaces, and the old device names.
    static bool isLegalBaseName(const QString &baseName);

    // Repoints every table holding this song's path and rewrites the columns
    // derived from its filename. Wrapped in a savepoint.
    static bool repointSong(const Rename &rename, QString &error);
};

#endif // LIBRARYNAMECLEANER_H
