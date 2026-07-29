#include "librarynamecleaner.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTextStream>
#include <algorithm>
#include <cstdlib>
#include <set>

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

int LibraryNameCleaner::execute(const std::vector<Proposal> &proposals) {
    m_errors.clear();
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

    if (!transaction.exec("COMMIT")) {
        m_errors.append(tr("Could not commit the changes: %1").arg(transaction.lastError().text()));
        transaction.exec("ROLLBACK");
        return 0;
    }

    QDir dataDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    m_journalPath = dataDir.absoluteFilePath(
            "namefix-" + QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss") + ".csv");
    QFile journal(m_journalPath);
    if (journal.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream stream(&journal);
        stream << "field,artist_scope,old_value,new_value,songs\n";
        for (const auto &proposal : ordered) {
            stream << ((proposal.field == Field::Title) ? "title" : "artist") << ','
                   << csvEscape(proposal.scopeArtist) << ','
                   << csvEscape(proposal.oldValue) << ','
                   << csvEscape(proposal.newValue) << ','
                   << proposal.songs << '\n';
        }
    } else {
        m_journalPath.clear();
        if (m_logger)
            m_logger->warn("{} Could not write the change journal", m_loggingPrefix);
    }

    if (m_logger)
        m_logger->info("{} Applied {} name changes across {} songs", m_loggingPrefix, ordered.size(),
                       songsChanged.size());
    return (int) songsChanged.size();
}
