#ifndef LIBRARYREORGANIZER_H
#define LIBRARYREORGANIZER_H

#include <QMap>
#include <QObject>
#include <QStringList>
#include <memory>
#include <vector>
#include <spdlog/spdlog.h>
#include <spdlog/async_logger.h>

#include "okjfmt.h"

// Distributes the karaoke files sitting loose in a source directory into
// single-level subdirectories and repoints the database at their new locations.
//
// This exists because a single directory holding 100k+ files is slow to
// enumerate (every file added to a watched directory triggers a full rescan of
// it) and, on NTFS, slow to create files in once 8.3 name generation starts
// hitting collisions. Bucketing costs nothing at playback time - the database
// holds absolute paths and DbUpdater already walks subdirectories.
class LibraryReorganizer : public QObject
{
Q_OBJECT

public:
    // How a song is assigned to a subdirectory.
    enum class Scheme {
        ArtistInitial,    // first letter of dbSongs.artist - usually the more even spread
        FilenameInitial   // first letter of the filename - works for files not in the db
    };

    // One song's worth of files. A loose CDG carries its audio companion along;
    // zips and video files move on their own.
    struct SongMove {
        QString fromMain;   // the file holding the dbSongs row (.cdg / .zip / video)
        QString toMain;
        QString fromAudio;  // companion audio for a loose .cdg, empty otherwise
        QString toAudio;
    };

    struct Plan {
        QString sourceDir;
        Scheme scheme{Scheme::ArtistInitial};
        std::vector<SongMove> moves;
        QStringList skipped;             // human-readable reason per skipped file
        QMap<QString, int> distribution; // bucket name -> song count

        [[nodiscard]] bool isEmpty() const { return moves.empty(); }
        [[nodiscard]] size_t fileCount() const;
        [[nodiscard]] int largestBucket() const;
    };

    explicit LibraryReorganizer(QObject *parent = nullptr);

    // Walks sourceDir's top level and works out where everything should go.
    // Touches nothing on disk.
    [[nodiscard]] Plan plan(const QString &sourceDir, Scheme scheme);

    // Moves the files and rewrites the database paths. Returns false if any
    // song failed to move; errors() explains which.
    bool execute(const Plan &plan);

    [[nodiscard]] const QStringList &errors() const { return m_errors; }

    // CSV of every completed move, written as execute() runs so a partial run
    // can be traced or undone by hand.
    [[nodiscard]] const QString &journalPath() const { return m_journalPath; }

    static QString schemeName(Scheme scheme);

signals:
    void progressMessage(const QString &msg);
    void stateChanged(QString state);
    void progressChanged(int progress, int max);

private:
    std::string m_loggingPrefix{"[LibraryReorganizer]"};
    std::shared_ptr<spdlog::logger> m_logger;
    QStringList m_errors;
    QString m_journalPath;

    // dbSongs.path -> artist, for the ArtistInitial scheme.
    QMap<QString, QString> loadArtistsByPath(const QString &sourceDir) const;

    static QString bucketFor(const QString &text);
    static bool isKaraokeFile(const QString &suffixLower);
    static bool isAudioFile(const QString &suffixLower);

    // Rewrites every table that stores this path. dbSongs is the anchor, but
    // three other tables keep their own independent copies. Wrapped in a
    // savepoint so a failure part-way leaves none of them rewritten.
    static bool repointDatabase(const QString &oldPath, const QString &newPath, QString &error);
};

#endif // LIBRARYREORGANIZER_H
