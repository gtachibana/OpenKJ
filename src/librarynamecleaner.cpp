#include "librarynamecleaner.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTextStream>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <set>

#include "karaokefileinfo.h"
#include "karaokefilepatternresolver.h"
#include "okjutil.h"
#include "src/models/tablemodelkaraokesourcedirs.h"

namespace {

    // Below this, a name is too short for an edit-distance comparison to mean
    // anything - "Pink" and "Ping" are one apart and unrelated.
    constexpr int minKeyLengthForMisspelling = 5;

    // One edit, never two. Two edits reach far enough to join names that belong
    // to different people - "Grace Jones" is two from "Bruce Jones", "Alice
    // Cooper" two from "Alicia Cooper" - while every genuine typo worth catching
    // ("Lynard"/"Lynyrd", "Jackon"/"Jackson", "Fogelberg"/"Fogelburg") sits at
    // one. The extra reach costs more than it finds.
    constexpr int maxEditDistance = 1;

    // The rare spelling has to be this many times rarer than the common one
    // before a near miss is worth proposing.
    constexpr int misspellingRatio = 10;

    // ...and rare in absolute terms. A spelling used by a dozen songs is a
    // spelling somebody chose, not a slip.
    constexpr int misspellingMaxSongs = 3;

    // Blocking buckets this large are a common prefix like "john", not a set of
    // candidates. Comparing them all pairwise costs more than it finds.
    constexpr int maxBucketSize = 200;

    // How much the winner has to outweigh a variant before folding it in counts
    // as a safe bet rather than something to eyeball.
    constexpr int highConfidenceRatio = 5;

    // Windows refuses these outright, and a path much past this length needs
    // long-path support that cannot be assumed.
    const QString reservedChars{R"(<>:"/\|?*)"};
    constexpr int maxPathLength = 255;

    // Old DOS device names. Still special-cased by the filesystem, whatever the
    // extension.
    const std::array<QString, 22> reservedNames{
            "CON", "PRN", "AUX", "NUL",
            "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
            "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
    };

    QString csvEscape(const QString &field) {
        QString escaped = field;
        escaped.replace('"', "\"\"");
        return '"' + escaped + '"';
    }

    // Both blocking keys for a name. A typo rarely disturbs the first four
    // characters and the last four at once, so bucketing on each and comparing
    // within buckets finds the realistic cases without an N-squared sweep of the
    // whole library. "lynard skynyrd" and "lynyrd skynyrd" differ up front and
    // meet in the suffix bucket.
    QStringList blockingKeys(const QString &key) {
        QStringList keys{key.left(4)};
        const QString suffix = key.right(4);
        if (suffix != keys.first())
            keys << suffix;
        return keys;
    }

}

LibraryNameCleaner::LibraryNameCleaner(QObject *parent) : QObject(parent) {
    m_logger = spdlog::get("logger");
}

int LibraryNameCleaner::Plan::songsAffected() const {
    int total{0};
    for (const auto &proposal : proposals)
        total += proposal.songs;
    return total;
}

int LibraryNameCleaner::Plan::countOf(Field field) const {
    return (int) std::count_if(proposals.begin(), proposals.end(),
                               [field](const Proposal &p) { return p.field == field; });
}

int LibraryNameCleaner::Plan::countOf(Confidence confidence) const {
    return (int) std::count_if(proposals.begin(), proposals.end(),
                               [confidence](const Proposal &p) { return p.confidence == confidence; });
}

QString LibraryNameCleaner::reasonText(Reason reason) {
    switch (reason) {
        case Reason::Whitespace:
            return QObject::tr("Stray whitespace");
        case Reason::Misspelling:
            return QObject::tr("Looks misspelled");
        case Reason::Variant:
        default:
            return QObject::tr("Same name, spelled differently");
    }
}

QString LibraryNameCleaner::cleanWhitespace(const QString &value) {
    return value.simplified();
}

QString LibraryNameCleaner::normalizeKey(const QString &value) {
    // Decomposing first splits "é" into "e" plus a combining acute, so dropping
    // the marks below leaves the plain letter behind and the accented and
    // unaccented spellings land on the same key.
    const QString decomposed = value.normalized(QString::NormalizationForm_KD);
    QString key;
    key.reserve(decomposed.size());
    for (const QChar ch : decomposed) {
        const QChar::Category category = ch.category();
        if (category == QChar::Mark_NonSpacing || category == QChar::Mark_SpacingCombining
            || category == QChar::Mark_Enclosing)
            continue;
        if (ch.isLetterOrNumber())
            key.append(ch.toLower());
        else if (ch == '&')
            key.append("and");
        else if (ch.isSpace() || ch == '-' || ch == '_' || ch == '/' || ch == '+')
            key.append(' ');
        // Apostrophes, commas, periods and brackets are dropped outright, so
        // "Guns N' Roses" and "Guns N Roses" meet.
    }
    key = key.simplified();
    if (key.startsWith("the "))
        key = key.mid(4);
    return key;
}

QString LibraryNameCleaner::digitsOf(const QString &value) {
    QString digits;
    for (const QChar ch : value) {
        if (ch.isDigit())
            digits.append(ch);
    }
    return digits;
}

int LibraryNameCleaner::editDistance(const QString &a, const QString &b, int maxDistance) {
    const int lenA = a.size();
    const int lenB = b.size();
    const int over = maxDistance + 1;

    if (std::abs(lenA - lenB) > maxDistance)
        return over;
    if (lenA == 0)
        return std::min(lenB, over);
    if (lenB == 0)
        return std::min(lenA, over);

    // Three rows, because transposition needs to reach back two rows.
    std::vector<int> prev2(lenB + 1, 0);
    std::vector<int> prev(lenB + 1);
    std::vector<int> cur(lenB + 1);
    for (int j = 0; j <= lenB; j++)
        prev[j] = j;

    for (int i = 1; i <= lenA; i++) {
        cur[0] = i;
        int rowBest = cur[0];
        for (int j = 1; j <= lenB; j++) {
            const int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            int best = std::min(std::min(prev[j] + 1, cur[j - 1] + 1), prev[j - 1] + cost);
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1])
                best = std::min(best, prev2[j - 2] + 1);
            cur[j] = best;
            rowBest = std::min(rowBest, best);
        }
        // Every path through this row already costs too much; nothing downstream
        // can bring it back under the ceiling.
        if (rowBest > maxDistance)
            return over;
        std::swap(prev2, prev);
        std::swap(prev, cur);
    }
    return std::min(prev[lenB], over);
}

bool LibraryNameCleaner::preferAsCanonical(const Variant &candidate, const Variant &incumbent) {
    if (candidate.count != incumbent.count)
        return candidate.count > incumbent.count;

    // Nothing to go on but the shape of the string. Prefer ordinary mixed case
    // over shouting, then keep accents rather than drop them, and fall back to
    // plain ordering so the same library always yields the same proposal.
    const auto mixedCase = [](const QString &value) {
        bool hasUpper{false};
        bool hasLower{false};
        for (const QChar ch : value) {
            hasUpper = hasUpper || ch.isUpper();
            hasLower = hasLower || ch.isLower();
        }
        return hasUpper && hasLower;
    };
    if (mixedCase(candidate.value) != mixedCase(incumbent.value))
        return mixedCase(candidate.value);

    const auto hasAccents = [](const QString &value) {
        return std::any_of(value.begin(), value.end(), [](const QChar ch) { return ch.unicode() > 127; });
    };
    if (hasAccents(candidate.value) != hasAccents(incumbent.value))
        return hasAccents(candidate.value);

    return candidate.value < incumbent.value;
}

std::vector<LibraryNameCleaner::Cluster> LibraryNameCleaner::resolveClusters(const std::vector<Variant> &variants,
                                                                            Field field,
                                                                            const QString &scopeArtist,
                                                                            std::vector<Proposal> &proposals) const {
    QHash<QString, std::vector<int>> byKey;
    byKey.reserve((int) variants.size());
    for (int i = 0; i < (int) variants.size(); i++) {
        const QString key = normalizeKey(variants[i].value);
        if (key.isEmpty())
            continue;
        byKey[key].push_back(i);
    }

    std::vector<Cluster> clusters;
    clusters.reserve((size_t) byKey.size());
    for (auto it = byKey.constBegin(); it != byKey.constEnd(); ++it) {
        const std::vector<int> &members = it.value();

        int winner = members.front();
        int total{0};
        for (const int index : members) {
            total += variants[index].count;
            if (preferAsCanonical(variants[index], variants[winner]))
                winner = index;
        }

        // The winner's own spelling is cleaned up first, so every proposal in
        // this cluster targets the same final string and none of them chain.
        const QString canonical = cleanWhitespace(variants[winner].value);
        clusters.push_back({it.key(), canonical, total});

        for (const int index : members) {
            const Variant &variant = variants[index];
            if (variant.value == canonical)
                continue;

            Proposal proposal;
            proposal.field = field;
            proposal.scopeArtist = scopeArtist;
            proposal.oldValue = variant.value;
            proposal.newValue = canonical;
            proposal.songs = variant.count;
            proposal.keptSongs = variants[winner].count;
            proposal.reason = (cleanWhitespace(variant.value) == canonical) ? Reason::Whitespace : Reason::Variant;
            proposal.confidence = (proposal.reason == Reason::Whitespace
                                   || variants[winner].count >= variant.count * highConfidenceRatio)
                                  ? Confidence::High
                                  : Confidence::Medium;
            proposals.push_back(proposal);
        }
    }
    return clusters;
}

void LibraryNameCleaner::findMisspellings(const std::vector<Cluster> &clusters,
                                          std::vector<Proposal> &proposals) const {
    QHash<QString, std::vector<int>> buckets;
    for (int i = 0; i < (int) clusters.size(); i++) {
        if (clusters[i].key.size() < minKeyLengthForMisspelling)
            continue;
        for (const QString &blockingKey : blockingKeys(clusters[i].key))
            buckets[blockingKey].push_back(i);
    }

    std::set<std::pair<int, int>> compared;
    for (auto it = buckets.constBegin(); it != buckets.constEnd(); ++it) {
        const std::vector<int> &bucket = it.value();
        if (bucket.size() < 2 || (int) bucket.size() > maxBucketSize)
            continue;

        for (size_t a = 0; a < bucket.size(); a++) {
            for (size_t b = a + 1; b < bucket.size(); b++) {
                const int indexA = bucket[a];
                const int indexB = bucket[b];
                if (!compared.insert({std::min(indexA, indexB), std::max(indexA, indexB)}).second)
                    continue;

                const Cluster &first = clusters[indexA];
                const Cluster &second = clusters[indexB];

                // Without a lopsided count there is no way to tell which of the
                // two spellings is the mistake.
                const Cluster &common = (first.totalCount >= second.totalCount) ? first : second;
                const Cluster &rare = (first.totalCount >= second.totalCount) ? second : first;
                if (rare.totalCount > misspellingMaxSongs)
                    continue;
                if (common.totalCount < rare.totalCount * misspellingRatio)
                    continue;

                // "Vol 1" and "Vol 2" are one edit apart and both correct.
                if (digitsOf(first.key) != digitsOf(second.key))
                    continue;

                // A bare trailing "s" is far more often two bands than one typo -
                // Heart and Hearts, Ben Folds and Ben Fold. Leave those alone.
                const QString &shorter = (first.key.size() <= second.key.size()) ? first.key : second.key;
                const QString &longer = (first.key.size() <= second.key.size()) ? second.key : first.key;
                if (longer == shorter + "s")
                    continue;

                const int distance = editDistance(first.key, second.key, maxEditDistance);
                if (distance < 1 || distance > maxEditDistance)
                    continue;

                Proposal proposal;
                proposal.field = Field::Artist;
                proposal.reason = Reason::Misspelling;
                proposal.oldValue = rare.canonical;
                proposal.newValue = common.canonical;
                // Any spelling variants inside the rare cluster already have
                // their own proposal folding them into rare.canonical, and those
                // are applied first, so by the time this one runs the whole
                // cluster is sitting on the value it rewrites.
                proposal.songs = rare.totalCount;
                proposal.keptSongs = common.totalCount;
                // Never High, so a near miss is never ticked by default. The
                // variant pass only ever changes how a name is written; this one
                // decides that a name is wrong, and it decides that purely from
                // song counts. That judgement gets a human every time.
                proposal.confidence = Confidence::Medium;
                proposals.push_back(proposal);
            }
        }
    }
}

LibraryNameCleaner::Plan LibraryNameCleaner::plan() {
    m_errors.clear();
    Plan plan;

    emit stateChanged(tr("Reading artists..."));
    QApplication::processEvents();

    // COLLATE BINARY throughout. dbSongs.Artist and .Title are NOCASE columns,
    // and grouping or matching under that collation would quietly merge the case
    // variants this whole pass exists to find.
    QSqlQuery query;
    std::vector<Variant> artists;
    if (!query.exec("SELECT artist, COUNT(*) FROM dbSongs GROUP BY artist COLLATE BINARY")) {
        m_errors.append(tr("Could not read artists: %1").arg(query.lastError().text()));
        return plan;
    }
    while (query.next()) {
        Variant variant{query.value(0).toString(), query.value(1).toInt()};
        if (variant.value.isEmpty())
            continue;
        plan.scannedSongs += variant.count;
        artists.push_back(variant);
    }
    plan.distinctArtists = (int) artists.size();

    emit stateChanged(tr("Comparing %1 artists...").arg(plan.distinctArtists));
    QApplication::processEvents();

    const std::vector<Cluster> artistClusters = resolveClusters(artists, Field::Artist, QString(), plan.proposals);
    findMisspellings(artistClusters, plan.proposals);

    emit stateChanged(tr("Reading titles..."));
    QApplication::processEvents();

    // Titles are only ever compared against other titles by the same artist -
    // two artists covering the same song are not a naming mistake.
    QSqlQuery titleQuery;
    if (!titleQuery.exec("SELECT artist, title, COUNT(*) FROM dbSongs "
                         "GROUP BY artist COLLATE BINARY, title COLLATE BINARY "
                         "ORDER BY artist COLLATE BINARY")) {
        m_errors.append(tr("Could not read titles: %1").arg(titleQuery.lastError().text()));
        return plan;
    }

    QString currentArtist;
    bool haveArtist{false};
    std::vector<Variant> titles;
    const auto flushArtist = [&]() {
        if (titles.size() > 1)
            resolveClusters(titles, Field::Title, currentArtist, plan.proposals);
        titles.clear();
    };
    while (titleQuery.next()) {
        const QString artist = titleQuery.value(0).toString();
        if (!haveArtist || artist != currentArtist) {
            flushArtist();
            currentArtist = artist;
            haveArtist = true;
        }
        Variant variant{titleQuery.value(1).toString(), titleQuery.value(2).toInt()};
        if (variant.value.isEmpty())
            continue;
        titles.push_back(variant);
    }
    flushArtist();

    emit stateChanged(tr("Done."));

    if (m_logger)
        m_logger->info("{} Planned {} name changes across {} songs ({} distinct artists scanned)",
                       m_loggingPrefix, plan.proposals.size(), plan.songsAffected(), plan.distinctArtists);
    return plan;
}

QString LibraryNameCleaner::buildBaseName(int pattern, const QString &artist, const QString &title,
                                          const QString &discId) {
    // Same shapes the single-song editor writes (MainWindow::editSong), so a
    // song corrected here is named the way one corrected by hand would be.
    switch (pattern) {
        case SourceDir::SAT:
            return discId.isEmpty() ? QString() : discId + " - " + artist + " - " + title;
        case SourceDir::STA:
            return discId.isEmpty() ? QString() : discId + " - " + title + " - " + artist;
        case SourceDir::ATS:
            return discId.isEmpty() ? QString() : artist + " - " + title + " - " + discId;
        case SourceDir::TAS:
            return discId.isEmpty() ? QString() : title + " - " + artist + " - " + discId;
        case SourceDir::S_T_A:
            return discId.isEmpty() ? QString() : discId + "_" + title + "_" + artist;
        case SourceDir::AT:
            return artist + " - " + title;
        case SourceDir::TA:
            return title + " - " + artist;
        default:
            // CUSTOM matches an arbitrary regex and METADATA reads the tags, so
            // neither can be expressed as a filename.
            return {};
    }
}

bool LibraryNameCleaner::isLegalBaseName(const QString &baseName) {
    if (baseName.isEmpty())
        return false;
    for (const QChar ch : baseName) {
        if (ch.unicode() < 32 || reservedChars.contains(ch))
            return false;
    }
    if (baseName.endsWith('.') || baseName.endsWith(' ') || baseName.startsWith(' '))
        return false;
    const QString upper = baseName.toUpper();
    return std::none_of(reservedNames.begin(), reservedNames.end(),
                        [&upper](const QString &reserved) { return upper == reserved; });
}

LibraryNameCleaner::RenamePlan LibraryNameCleaner::planRenames(const std::vector<Proposal> &proposals) {
    m_errors.clear();
    RenamePlan plan;
    if (proposals.empty())
        return plan;

    emit stateChanged(tr("Working out which files can be renamed..."));
    QApplication::processEvents();

    // Indexed the same way execute() applies them, so the values worked out here
    // are the values the rows will actually end up holding.
    QHash<QString, QString> artistVariants;
    QHash<QString, QString> artistMisspellings;
    QHash<QString, QHash<QString, QString>> titlesByArtist;
    QSet<QString> interestingArtists;
    for (const auto &proposal : proposals) {
        if (proposal.field == Field::Title) {
            titlesByArtist[proposal.scopeArtist][proposal.oldValue] = proposal.newValue;
            interestingArtists.insert(proposal.scopeArtist);
        } else if (proposal.reason == Reason::Misspelling) {
            artistMisspellings[proposal.oldValue] = proposal.newValue;
            interestingArtists.insert(proposal.oldValue);
        } else {
            artistVariants[proposal.oldValue] = proposal.newValue;
            interestingArtists.insert(proposal.oldValue);
        }
    }

    QSqlQuery query;
    if (!query.exec("SELECT songid, path, artist, title, discid FROM dbSongs")) {
        m_errors.append(tr("Could not read the library: %1").arg(query.lastError().text()));
        return plan;
    }

    auto resolver = std::make_shared<KaraokeFilePatternResolver>();
    // Two songs can be corrected to the same name; the second one to ask for a
    // filename has to be turned away.
    QSet<QString> claimedPaths;

    while (query.next()) {
        const QString artist = query.value(2).toString();
        if (!interestingArtists.contains(artist))
            continue;

        const int songId = query.value(0).toInt();
        const QString path = query.value(1).toString();
        const QString title = query.value(3).toString();
        const QString discId = query.value(4).toString();

        QString finalTitle = title;
        if (const auto it = titlesByArtist.constFind(artist); it != titlesByArtist.constEnd())
            finalTitle = it->value(title, title);
        QString finalArtist = artistVariants.value(artist, artist);
        finalArtist = artistMisspellings.value(finalArtist, finalArtist);

        if (finalArtist == artist && finalTitle == title)
            continue;

        const auto &pattern = resolver->getPattern(path);
        if (pattern.pattern == SourceDir::CUSTOM || pattern.pattern == SourceDir::METADATA) {
            plan.keptAsIs.append(tr("%1 - this folder takes its names from tags or a custom pattern, "
                                    "not from the filename").arg(QFileInfo(path).fileName()));
            plan.keptAsIsByCause[tr("folder names songs from tags or a custom pattern")]++;
            continue;
        }

        const QString newBaseName = buildBaseName(pattern.pattern, finalArtist, finalTitle, discId);
        if (newBaseName.isEmpty()) {
            plan.keptAsIs.append(tr("%1 - this folder's naming pattern needs a song id and this song "
                                    "hasn't got one").arg(QFileInfo(path).fileName()));
            plan.keptAsIsByCause[tr("song has no song id, but the folder's pattern needs one")]++;
            continue;
        }
        if (!isLegalBaseName(newBaseName)) {
            plan.keptAsIs.append(tr("%1 - \"%2\" cannot be used as a filename")
                                         .arg(QFileInfo(path).fileName(), newBaseName));
            plan.keptAsIsByCause[tr("name holds characters a filename cannot")]++;
            continue;
        }

        const QFileInfo info(path);
        // QDir keeps Qt's '/' separator. dbSongs.path is populated from
        // QDirIterator, which always uses '/', and a path with native '\'
        // separators would compare unequal in DbUpdater's scan - making the song
        // look both missing and new on every later update.
        const QString newMain = QDir(info.absolutePath()).absoluteFilePath(newBaseName + "." + info.suffix());
        if (newMain == path)
            continue;
        if (newMain.length() > maxPathLength) {
            plan.keptAsIs.append(tr("%1 - the corrected filename would be too long").arg(info.fileName()));
            plan.keptAsIsByCause[tr("corrected filename would be too long")]++;
            continue;
        }

        // The parser is not the inverse of the namer: it folds underscores into
        // spaces and splits on " - ", handing the leftovers to a single field.
        // So rather than reason about which names survive that, the candidate is
        // put through the very parser the importer uses and dropped unless it
        // comes back identical. Nothing is stat'd here - for these patterns the
        // parse is pure string work on the basename.
        KaraokeFileInfo probe(nullptr, resolver);
        probe.setFile(newMain);
        if (probe.getArtist() != finalArtist || probe.getTitle() != finalTitle) {
            plan.keptAsIs.append(tr("%1 - renaming it would not survive a reimport, the filename reads "
                                    "back as \"%2 - %3\"")
                                         .arg(info.fileName(), probe.getArtist(), probe.getTitle()));
            plan.keptAsIsByCause[tr("filename would not read back as the corrected name")]++;
            continue;
        }

        if (QFile::exists(newMain) || claimedPaths.contains(newMain)) {
            plan.keptAsIs.append(tr("%1 - a file called \"%2\" is already there")
                                         .arg(info.fileName(), newBaseName + "." + info.suffix()));
            plan.keptAsIsByCause[tr("a file of that name is already there")]++;
            continue;
        }

        Rename rename;
        rename.songId = songId;
        rename.fromMain = path;
        rename.toMain = newMain;
        rename.newBaseName = newBaseName;
        rename.artist = finalArtist;
        rename.title = finalTitle;
        rename.discId = discId;

        // A loose CDG is half a song; its audio has to travel with it.
        if (info.suffix().compare("cdg", Qt::CaseInsensitive) == 0) {
            rename.fromAudio = findMatchingAudioFile(path);
            if (!rename.fromAudio.isEmpty()) {
                rename.toAudio = QDir(info.absolutePath())
                        .absoluteFilePath(newBaseName + "." + QFileInfo(rename.fromAudio).suffix());
                if (QFile::exists(rename.toAudio) || claimedPaths.contains(rename.toAudio)) {
                    plan.keptAsIs.append(tr("%1 - its audio file cannot be renamed, something of that "
                                            "name is already there").arg(info.fileName()));
                    plan.keptAsIsByCause[tr("a file of that name is already there")]++;
                    continue;
                }
            }
        }

        claimedPaths.insert(rename.toMain);
        if (!rename.toAudio.isEmpty())
            claimedPaths.insert(rename.toAudio);
        plan.renames.push_back(rename);
    }

    if (m_logger)
        m_logger->info("{} {} songs can be renamed on disk, {} kept as is", m_loggingPrefix, plan.renames.size(),
                       plan.keptAsIs.size());
    return plan;
}

bool LibraryNameCleaner::repointSong(const Rename &rename, QString &error) {
    QSqlQuery savepoint;
    savepoint.exec("SAVEPOINT namefix_song");

    // dbSongs is the anchor and also carries the columns derived from the
    // filename, but queueSongs denormalizes the path at insert and both history
    // tables key their own lookups on it. Miss any of them and the rows quietly
    // orphan.
    const QString searchString =
            rename.newBaseName + " " + rename.artist + " " + rename.title + " " + rename.discId;

    QSqlQuery songQuery;
    songQuery.prepare("UPDATE dbSongs SET path = :new, filename = :filename, searchstring = :searchstring "
                      "WHERE songid = :songid");
    songQuery.bindValue(":new", rename.toMain);
    // DbUpdater stores the basename without its extension; matching it keeps a
    // corrected row identical to one a fresh import would produce.
    songQuery.bindValue(":filename", rename.newBaseName);
    songQuery.bindValue(":searchstring", searchString);
    songQuery.bindValue(":songid", rename.songId);
    if (!songQuery.exec()) {
        error = songQuery.lastError().text();
        savepoint.exec("ROLLBACK TO namefix_song");
        savepoint.exec("RELEASE namefix_song");
        return false;
    }

    static const std::array<QString, 3> statements{
            QStringLiteral("UPDATE queuesongs SET path = :new WHERE path = :old"),
            QStringLiteral("UPDATE dbSongHistory SET filepath = :new WHERE filepath = :old"),
            QStringLiteral("UPDATE historySongs SET filepath = :new WHERE filepath = :old")
    };
    for (const auto &statement : statements) {
        QSqlQuery query;
        query.prepare(statement);
        query.bindValue(":new", rename.toMain);
        query.bindValue(":old", rename.fromMain);
        if (!query.exec()) {
            error = query.lastError().text();
            savepoint.exec("ROLLBACK TO namefix_song");
            savepoint.exec("RELEASE namefix_song");
            return false;
        }
    }

    savepoint.exec("RELEASE namefix_song");
    return true;
}

int LibraryNameCleaner::execute(const std::vector<Proposal> &proposals, const RenamePlan &renames) {
    m_errors.clear();
    m_warnings.clear();
    m_journalPath.clear();
    if (proposals.empty())
        return 0;

    // A title's predicate names the artist it sits under, so titles have to be
    // rewritten before any artist is renamed out from beneath them. Misspellings
    // go last because their predicate is the winning spelling of a cluster the
    // variant pass may still be assembling.
    std::vector<Proposal> ordered(proposals);
    std::stable_sort(ordered.begin(), ordered.end(), [](const Proposal &a, const Proposal &b) {
        const auto rank = [](const Proposal &p) {
            if (p.field == Field::Title)
                return 0;
            return (p.reason == Reason::Misspelling) ? 2 : 1;
        };
        return rank(a) < rank(b);
    });

    QSqlQuery transaction;
    transaction.exec("BEGIN TRANSACTION");

    QSqlQuery selectByArtist;
    selectByArtist.prepare("SELECT songid, path, artist, title, discid FROM dbSongs "
                           "WHERE artist = :old COLLATE BINARY");
    QSqlQuery selectByTitle;
    selectByTitle.prepare("SELECT songid, path, artist, title, discid FROM dbSongs "
                          "WHERE artist = :artist COLLATE BINARY AND title = :old COLLATE BINARY");
    QSqlQuery update;
    update.prepare("UPDATE dbSongs SET artist = :artist, title = :title, searchstring = :searchstring "
                   "WHERE songid = :songid");

    struct Row {
        int songId;
        QString path;
        QString artist;
        QString title;
        QString discId;
    };

    // A song can be touched twice - once for its title, once for its artist -
    // so the songs are counted rather than the writes.
    std::set<int> songsChanged;
    for (const auto &proposal : ordered) {
        QSqlQuery &select = (proposal.field == Field::Title) ? selectByTitle : selectByArtist;
        if (proposal.field == Field::Title)
            select.bindValue(":artist", proposal.scopeArtist);
        select.bindValue(":old", proposal.oldValue);
        if (!select.exec()) {
            m_errors.append(tr("Could not look up \"%1\": %2").arg(proposal.oldValue, select.lastError().text()));
            transaction.exec("ROLLBACK");
            return 0;
        }

        // The whole result set is buffered before anything is written, so the
        // updates below can't disturb a cursor that is still walking.
        std::vector<Row> rows;
        while (select.next()) {
            rows.push_back({select.value(0).toInt(), select.value(1).toString(), select.value(2).toString(),
                            select.value(3).toString(), select.value(4).toString()});
        }

        for (const auto &row : rows) {
            const QString artist = (proposal.field == Field::Artist) ? proposal.newValue : row.artist;
            const QString title = (proposal.field == Field::Title) ? proposal.newValue : row.title;
            // Same shape the importer and the single-song editor build, so a
            // corrected row stays findable by filename as well as by name.
            const QString searchString = QFileInfo(row.path).completeBaseName() + " " + artist + " " + title + " "
                                         + row.discId;

            update.bindValue(":artist", artist);
            update.bindValue(":title", title);
            update.bindValue(":searchstring", searchString);
            update.bindValue(":songid", row.songId);
            if (!update.exec()) {
                m_errors.append(tr("Could not update song %1: %2")
                                        .arg(QString::number(row.songId), update.lastError().text()));
                transaction.exec("ROLLBACK");
                return 0;
            }
            songsChanged.insert(row.songId);
        }
    }

    // Files come last, so a song whose files will not move still keeps the
    // correction in the database. A failure here is a warning, not a rollback.
    std::vector<Rename> renamed;
    const auto undoRenames = [&renamed]() {
        for (const auto &rename : renamed) {
            QFile::rename(rename.toMain, rename.fromMain);
            if (!rename.fromAudio.isEmpty())
                QFile::rename(rename.toAudio, rename.fromAudio);
        }
    };

    for (const auto &rename : renames.renames) {
        if (!QFile::rename(rename.fromMain, rename.toMain)) {
            m_warnings.append(tr("Could not rename %1, its name in OpenKJ was corrected anyway")
                                      .arg(QFileInfo(rename.fromMain).fileName()));
            continue;
        }
        if (!rename.fromAudio.isEmpty() && !QFile::rename(rename.fromAudio, rename.toAudio)) {
            // Never leave a CDG separated from its audio - put it back.
            QFile::rename(rename.toMain, rename.fromMain);
            m_warnings.append(tr("Could not rename the audio for %1, left the song's files alone")
                                      .arg(QFileInfo(rename.fromMain).fileName()));
            continue;
        }

        QString repointError;
        if (!repointSong(rename, repointError)) {
            // Never leave the database pointing at a path that no longer exists.
            QFile::rename(rename.toMain, rename.fromMain);
            if (!rename.fromAudio.isEmpty())
                QFile::rename(rename.toAudio, rename.fromAudio);
            m_warnings.append(tr("Could not repoint %1 (%2), left the song's files alone")
                                      .arg(QFileInfo(rename.fromMain).fileName(), repointError));
            continue;
        }

        renamed.push_back(rename);
        songsChanged.insert(rename.songId);
    }

    if (!transaction.exec("COMMIT")) {
        m_errors.append(tr("Could not commit the changes: %1").arg(transaction.lastError().text()));
        transaction.exec("ROLLBACK");
        // The rollback took the paths back to the old filenames, so the files
        // have to follow or the library points at names that no longer exist.
        undoRenames();
        m_warnings.clear();
        return 0;
    }

    QDir dataDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    m_journalPath = dataDir.absoluteFilePath(
            "namefix-" + QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss") + ".csv");
    QFile journal(m_journalPath);
    if (journal.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream stream(&journal);
        stream << "kind,field,artist_scope,old_value,new_value\n";
        for (const auto &proposal : ordered) {
            stream << "name," << ((proposal.field == Field::Title) ? "title" : "artist") << ','
                   << csvEscape(proposal.scopeArtist) << ','
                   << csvEscape(proposal.oldValue) << ','
                   << csvEscape(proposal.newValue) << '\n';
        }
        // Renames go in the same journal so undoing a run by hand means walking
        // one file, bottom to top.
        for (const auto &rename : renamed) {
            stream << "file,main,," << csvEscape(rename.fromMain) << ',' << csvEscape(rename.toMain) << '\n';
            if (!rename.fromAudio.isEmpty())
                stream << "file,audio,," << csvEscape(rename.fromAudio) << ',' << csvEscape(rename.toAudio)
                       << '\n';
        }
    } else {
        m_journalPath.clear();
        if (m_logger)
            m_logger->warn("{} Could not write the change journal", m_loggingPrefix);
    }

    if (m_logger)
        m_logger->info("{} Applied {} name changes across {} songs, renamed {} songs' files", m_loggingPrefix,
                       ordered.size(), songsChanged.size(), renamed.size());
    return (int) songsChanged.size();
}
