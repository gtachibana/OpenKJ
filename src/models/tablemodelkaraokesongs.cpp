#include "tablemodelkaraokesongs.h"

#include <QApplication>
#include <QSqlQuery>
#include <QSqlError>
#include <QPainter>
#include <QFileInfo>
#include <QDir>
#include <QDirIterator>
#include <QSvgRenderer>
#include <QMimeData>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

#include "gainlazyupdater.h"
#include "okjfmt.h"
#include "okjutil.h"

namespace {

    // Normalizes a stored field into the form the search needles are matched
    // against. The contains() guards keep QString's implicit sharing intact for
    // the common case, so songs without an '&' or apostrophe don't allocate.
    QString normalizeHaystack(const QString &src, bool ignoreApos) {
        QString s = src.toLower();
        if (s.contains('&'))
            s.replace('&', " and ");
        if (ignoreApos && s.contains('\''))
            s.remove('\'');
        return s;
    }

    // Normalizes the user's query into whitespace-separated needles.
    QString normalizeNeedles(const QString &raw, bool ignoreApos) {
        QString s = raw.toLower();
        s.replace(',', ' ');
        s.replace('&', " and ");
        if (ignoreApos)
            s.replace('\'', ' ');
        return s;
    }

}

TableModelKaraokeSongs::TableModelKaraokeSongs(QObject *parent)
        : QAbstractTableModel(parent) {
    m_logger = spdlog::get("logger");
    resizeIconsForFont(m_settings.applicationFont());
    connect(&searchTimer, &QTimer::timeout, this, &TableModelKaraokeSongs::searchExec);
}

QVariant TableModelKaraokeSongs::headerData(int section, Qt::Orientation orientation, int role) const {
    switch (role) {
        case Qt::FontRole:
            return m_headerFont;
        case Qt::DisplayRole:
            if (orientation == Qt::Horizontal)
                return getColumnName(section);
            break;
        case Qt::SizeHintRole:
            if (orientation == Qt::Horizontal)
                return getColumnSizeHint(section);
        default:
            return {};
    }
    return {};
}

QVariant TableModelKaraokeSongs::getColumnSizeHint(int section) const {
    switch (section) {
        case COL_ID:
            return QSize(m_itemFontMetrics.size(Qt::TextSingleLine, "_ID_").width(), m_itemHeight);

        case COL_DURATION:
            return QSize(m_itemFontMetrics.size(Qt::TextSingleLine, "_Time_").width(), m_itemHeight);
        case COL_PLAYS:
            return QSize(m_itemFontMetrics.size(Qt::TextSingleLine, "_Plays_").width(), m_itemHeight);
        case COL_LASTPLAY:
            return QSize(m_itemFontMetrics.size(Qt::TextSingleLine, "_10:00 10/00/00_PM").width(), m_itemHeight);
        case COL_SONGID:
            return QSize(m_itemFontMetrics.size(Qt::TextSingleLine, "XXXX0000000-01-00").width(), m_itemHeight);
        case COL_GAIN:
            return QSize(m_itemFontMetrics.size(Qt::TextSingleLine, "_-00.0 dB_").width(), m_itemHeight);
        case COL_ARTIST:
        case COL_TITLE:
        case COL_FILENAME:
        default:
            return {};
    }
}

QVariant TableModelKaraokeSongs::getColumnName(int section) {
    switch (section) {
        case COL_ID:
            return "ID";
        case COL_ARTIST:
            return "Artist";
        case COL_TITLE:
            return "Title";
        case COL_SONGID:
            return "SongID";
        case COL_FILENAME:
            return "Filename";
        case COL_DURATION:
            return "Time";
        case COL_PLAYS:
            return "Plays";
        case COL_LASTPLAY:
            return "Last Played";
        case COL_GAIN:
            return "Gain";
        default:
            return {};
    }
}

int TableModelKaraokeSongs::rowCount([[maybe_unused]]const QModelIndex &parent) const {
    return (int) m_filteredSongs.size();
}

int TableModelKaraokeSongs::columnCount([[maybe_unused]]const QModelIndex &parent) const {
    return COL_GAIN + 1;
}

QVariant TableModelKaraokeSongs::data(const QModelIndex &index, int role) const {
    if (!index.isValid())
        return {};
    switch (role) {
        case Qt::FontRole:
            return m_itemFont;
        case Qt::TextAlignmentRole:
            return getColumnTextAlignmentHint(index.column());
        case Qt::DecorationRole:
            return getColumnDecorationRole(index);
        case Qt::DisplayRole:
            return getItemDisplayData(index);
        case Qt::SizeHintRole:
            return m_itemFontMetrics.size(Qt::TextSingleLine, getItemDisplayData(index).toString());
        case Qt::UserRole: {
            QVariant retVal;
            retVal.setValue(m_filteredSongs.at(index.row()));
            return retVal;
        }
        default:
            return {};
    }
}

QVariant TableModelKaraokeSongs::getColumnDecorationRole(const QModelIndex &index) const {
    switch (index.column()) {
        case COL_SONGID:
            if (m_filteredSongs.at(index.row())->path.endsWith("cdg", Qt::CaseInsensitive))
                return m_iconCdg;
            else if (m_filteredSongs.at(index.row())->path.endsWith("zip", Qt::CaseInsensitive))
                return m_iconZip;
            else
                return m_iconVid;
        default:
            return {};
    }
}

QVariant TableModelKaraokeSongs::getColumnTextAlignmentHint(int column) {
    switch (column) {
        case COL_DURATION:
        case COL_PLAYS:
        case COL_LASTPLAY:
        case COL_GAIN:
            return QVariant::fromValue(Qt::Alignment(Qt::AlignRight | Qt::AlignVCenter));
        default:
            return QVariant::fromValue(Qt::Alignment(Qt::AlignLeft | Qt::AlignVCenter));
    }
}

QVariant TableModelKaraokeSongs::getItemDisplayData(const QModelIndex &index) const {
    switch (index.column()) {
        case COL_ID:
            return m_filteredSongs.at(index.row())->id;
        case COL_ARTIST:
            return m_filteredSongs.at(index.row())->artist;
        case COL_TITLE:
            return m_filteredSongs.at(index.row())->title;
        case COL_SONGID:
            return m_filteredSongs.at(index.row())->songid;
        case COL_FILENAME:
            return m_filteredSongs.at(index.row())->filename;
        case COL_DURATION:
            if (m_filteredSongs.at(index.row())->duration < 1)
                return {};
            return QTime(0, 0, 0, 0).addSecs(m_filteredSongs.at(index.row())->duration / 1000).toString(
                    "m:ss");
        case COL_PLAYS:
            return m_filteredSongs.at(index.row())->plays;
        case COL_LASTPLAY: {
            QLocale locale;
            return m_filteredSongs.at(index.row())->lastPlay.toString(
                    locale.dateTimeFormat(QLocale::ShortFormat));
        }
        case COL_GAIN:
            return gainDisplayText(m_filteredSongs.at(index.row())->gain);
        default:
            return {};
    }
}

// Blank rather than a placeholder for the not-yet-analyzed case: on a large library
// most of the column is empty for a long while, and a screen of em-dashes reads as
// noise. "n/a" is reserved for files analysis actually gave up on.
QString TableModelKaraokeSongs::gainDisplayText(const double gain) {
    if (std::isnan(gain))
        return {};
    if (std::fabs(gain) >= kGainSentinelThreshold)
        return "n/a";
    return QString::number(gain, 'f', 1) + " dB";
}

void TableModelKaraokeSongs::loadData() {
    emit layoutAboutToBeChanged();
    m_allSongs.clear();
    m_filteredSongs.clear();
    const bool ignoreApos = m_settings.ignoreAposInSearch();
    QSqlQuery query;
    // SQLite forward-only queries always report size() as -1, so the row count
    // has to come from a separate query. Worth it: without a reserve, loading a
    // 100k+ song library repeatedly reallocates both vectors.
    if (query.exec("SELECT COUNT(*) FROM dbsongs") && query.next()) {
        const auto rows = static_cast<size_t>(query.value(0).toLongLong());
        m_allSongs.reserve(rows);
        m_filteredSongs.reserve(rows);
    }
    query.exec("SELECT songid,artist,title,discid,duration,filename,path,searchstring,plays,lastplay,gain,bad FROM dbsongs");
    while (query.next()) {
        // Built once each. Artist, title and discid are all wanted twice - as they are
        // and lowercased - and asking the query for them a second time constructed a
        // second QString every time, three per row across a 100k-song load.
        const QString artist = query.value(1).toString();
        const QString title = query.value(2).toString();
        const QString discid = query.value(3).toString();
        const auto &song = m_allSongs.emplace_back(std::make_shared<okj::KaraokeSong>(okj::KaraokeSong{
                query.value(0).toInt(),
                artist,
                artist.toLower(),
                title,
                title.toLower(),
                discid,
                discid.toLower(),
                query.value(4).toInt(),
                query.value(5).toString(),
                query.value(6).toString(),
                query.value(7).toString().replace('&', " and ").toLower(),
                query.value(8).toInt(),
                query.value(9).toDateTime(),
                query.value(11).toBool(),
                (discid == "!!DROPPED!!"),
                // NULL means never analyzed, which the model shows as blank rather
                // than as a gain of 0 dB - those are very different things.
                query.value(10).isNull() ? std::numeric_limits<double>::quiet_NaN()
                                         : query.value(10).toDouble()
        }));
        setSearchHaystacks(*song, ignoreApos);
    }
    m_haystacksIgnoreApos = ignoreApos;
    rebuildPathHash();
    m_logger->info("{} Loaded {} karaoke songs from the db on disk", m_loggingPrefix, m_allSongs.size());
    invalidateSearchNarrowing();
    requestSearch();
    emit layoutChanged();
}

// Shares the song objects already loaded by another model instance instead of
// re-reading and re-building them from the database. Filter state stays independent.
void TableModelKaraokeSongs::loadDataFrom(const TableModelKaraokeSongs &source) {
    emit layoutAboutToBeChanged();
    m_allSongs = source.m_allSongs;
    m_filteredSongs.clear();
    // The songs are shared by pointer with the source model, so their haystacks
    // are already built - just inherit which apostrophe mode they were built for.
    m_haystacksIgnoreApos = source.m_haystacksIgnoreApos;
    rebuildPathHash();
    m_logger->info("{} Loaded {} karaoke songs shared from the primary model", m_loggingPrefix, m_allSongs.size());
    invalidateSearchNarrowing();
    requestSearch();
    emit layoutChanged();
}

void TableModelKaraokeSongs::rebuildPathHash() {
    m_songsByPath.clear();
    m_songsByPath.reserve(static_cast<int>(m_allSongs.size()));
    for (const auto &song : m_allSongs)
        m_songsByPath.insert(song->path, song);
}

void TableModelKaraokeSongs::search(const QString &searchString) {
    m_lastSearchRaw = searchString;
    m_lastSearch = normalizeNeedles(searchString, m_settings.ignoreAposInSearch());
    requestSearch();
}

void TableModelKaraokeSongs::requestSearch() {
    if (searchTimer.isActive())
        searchTimer.stop();
    searchTimer.start(100);
}

// Anything that reorders, adds to, or removes from m_allSongs - or changes
// which field is searched - breaks the assumption that m_filteredSongs is a
// superset of the next result, so the next search has to be a full scan.
void TableModelKaraokeSongs::invalidateSearchNarrowing() {
    m_canNarrowSearch = false;
}

void TableModelKaraokeSongs::setSearchHaystacks(okj::KaraokeSong &song, bool ignoreApos) {
    song.searchAll = normalizeHaystack(song.searchString, ignoreApos);
    song.searchArtist = normalizeHaystack(song.artistL, ignoreApos);
    song.searchTitle = normalizeHaystack(song.titleL, ignoreApos);
}

void TableModelKaraokeSongs::rebuildSearchHaystacks(bool ignoreApos) {
    for (const auto &song : m_allSongs)
        setSearchHaystacks(*song, ignoreApos);
    m_haystacksIgnoreApos = ignoreApos;
}

const QString &TableModelKaraokeSongs::searchHaystack(const okj::KaraokeSong &song) const {
    switch (m_searchType) {
        case SEARCH_TYPE_ARTIST:
            return song.searchArtist;
        case SEARCH_TYPE_TITLE:
            return song.searchTitle;
        case SEARCH_TYPE_ALL:
        default:
            return song.searchAll;
    }
}

void TableModelKaraokeSongs::searchExec() {
    searchTimer.stop();

    // The haystacks bake in the apostrophe setting, so pick up a change to it.
    if (const bool ignoreApos = m_settings.ignoreAposInSearch(); ignoreApos != m_haystacksIgnoreApos) {
        rebuildSearchHaystacks(ignoreApos);
        m_lastSearch = normalizeNeedles(m_lastSearchRaw, ignoreApos);
        invalidateSearchNarrowing();
    }

    const auto needles = m_lastSearch.split(' ', Qt::SkipEmptyParts);

    // When the query merely extends the previous one - the usual case while
    // typing - every song that matches it also matched the previous query, so
    // the previous results can be narrowed instead of rescanning the library.
    const bool narrow = m_canNarrowSearch && m_lastSearch.startsWith(m_lastExecutedSearch);
    const auto &candidates = narrow ? m_filteredSongs : m_allSongs;

    std::vector<std::shared_ptr<okj::KaraokeSong>> results;
    results.reserve(candidates.size());
    for (const auto &song : candidates) {
        if (song->dropped)
            continue;
        // Review mode inverts the filter rather than widening it: the point of the
        // list is to work through the marked songs, and a handful of them buried in
        // 100k good ones is not a list anybody can review.
        if (song->bad != m_showBadOnly)
            continue;
        const QString &haystack = searchHaystack(*song);
        bool match{true};
        for (const auto &needle : needles) {
            if (!haystack.contains(needle)) {
                match = false;
                break;
            }
        }
        if (match)
            results.emplace_back(song);
    }
    results.shrink_to_fit();

    emit layoutAboutToBeChanged();
    m_filteredSongs = std::move(results);
    emit layoutChanged();

    m_lastExecutedSearch = m_lastSearch;
    m_canNarrowSearch = true;
}

void TableModelKaraokeSongs::setSearchType(TableModelKaraokeSongs::SearchType type) {
    if (m_searchType == type)
        return;
    m_searchType = type;
    invalidateSearchNarrowing();
    requestSearch();
}

void TableModelKaraokeSongs::setShowBadOnly(const bool showBadOnly) {
    if (m_showBadOnly == showBadOnly)
        return;
    m_showBadOnly = showBadOnly;
    invalidateSearchNarrowing();
    requestSearch();
}

int TableModelKaraokeSongs::getIdForPath(const QString &path) {
    auto it = m_songsByPath.constFind(path);
    if (it == m_songsByPath.constEnd())
        return -1;
    return it.value()->id;
}

QString TableModelKaraokeSongs::getPath(const int songId) {
    auto it = std::find_if(m_allSongs.begin(), m_allSongs.end(), [&songId](const std::shared_ptr<okj::KaraokeSong> &song) {
        return (song->id == songId);
    });
    if (it == m_allSongs.end()) {
        m_logger->error("{} getPath called with unknown song id {}", m_loggingPrefix, songId);
        return {};
    }
    return it->get()->path;
}

void TableModelKaraokeSongs::updateSongHistory(const int songId) {
    auto it = find_if(m_allSongs.begin(), m_allSongs.end(), [&songId](const std::shared_ptr<okj::KaraokeSong> &song) {
        if (song->id == songId)
            return true;
        return false;
    });
    if (it != m_allSongs.end()) {
        it->get()->plays++;
        it->get()->lastPlay = QDateTime::currentDateTime();
    }

    auto it2 = find_if(m_filteredSongs.begin(), m_filteredSongs.end(),
                       [&songId](const std::shared_ptr<okj::KaraokeSong> &song) {
                           if (song->id == songId)
                               return true;
                           return false;
                       });
    if (it2 != m_filteredSongs.end()) {
        int row = (int) std::distance(m_filteredSongs.begin(), it2);
        emit dataChanged(this->index(row, COL_PLAYS), this->index(row, COL_LASTPLAY), QVector<int>(Qt::DisplayRole));
    }

    QSqlQuery query;
    query.prepare("UPDATE dbSongs set plays = plays + :incVal, lastplay = :curTs WHERE songid = :songid");
    query.bindValue(":curTs", QDateTime::currentDateTime());
    query.bindValue(":songid", songId);
    query.bindValue(":incVal", 1);
    query.exec();
}

okj::KaraokeSong &TableModelKaraokeSongs::getSong(const int songId) {
    auto it = std::find_if(m_allSongs.begin(), m_allSongs.end(), [&songId](const std::shared_ptr<okj::KaraokeSong> &song) {
        return (song->id == songId);
    });
    // A miss used to dereference end() and hand the caller whatever shared_ptr sat one
    // past the end of the vector - and callers copy the result, so it went on to
    // manipulate QString refcounts through that pointer. Song ids arrive here from the
    // network, so a miss is an ordinary event, not a programming error.
    if (it == m_allSongs.end()) {
        m_logger->error("{} getSong called with unknown song id {}", m_loggingPrefix, songId);
        return InvalidSong;
    }
    return **it;
}

void TableModelKaraokeSongs::resizeIconsForFont(const QFont &font) {
    m_logger->trace("{} resizeIconsForFont called with font: {} size: {}", m_loggingPrefix, font.toString().toStdString(), QFontMetrics(font).height());
    m_itemFont = m_settings.applicationFont();
    m_headerFont = m_settings.applicationFont();
    m_headerFont.setBold(true);
    m_itemFontMetrics = QFontMetrics(m_itemFont);
    m_itemHeight = m_itemFontMetrics.height() + 6;
    QString thm = (m_settings.theme() == 1) ? ":/theme/Icons/okjbreeze-dark/" : ":/theme/Icons/okjbreeze/";
    m_curFontHeight = QFontMetrics(font).height();
    m_iconVid = QImage(m_curFontHeight, m_curFontHeight, QImage::Format_ARGB32);
    m_iconZip = QImage(m_curFontHeight, m_curFontHeight, QImage::Format_ARGB32);
    m_iconCdg = QImage(m_curFontHeight, m_curFontHeight, QImage::Format_ARGB32);
    m_iconVid.fill(Qt::transparent);
    m_iconZip.fill(Qt::transparent);
    m_iconCdg.fill(Qt::transparent);
    QPainter painterVid(&m_iconVid);
    QPainter painterZip(&m_iconZip);
    QPainter painterCdg(&m_iconCdg);
    QSvgRenderer svgRndrVid(thm + "mimetypes/22/video-mp4.svg");
    QSvgRenderer svgRndrZip(thm + "mimetypes/22/application-zip.svg");
    QSvgRenderer svgRndrCdg(thm + "mimetypes/22/application-x-cda.svg");
    svgRndrVid.render(&painterVid);
    svgRndrZip.render(&painterZip);
    svgRndrCdg.render(&painterCdg);
}


QMimeData *TableModelKaraokeSongs::mimeData(const QModelIndexList &indexes) const {
    auto *mimeData = new QMimeData();
    // Guarded like TableModelRotation::mimeData(). Two of the four models checked and
    // two dereferenced element 0 of a list Qt does not promise to be non-empty.
    if (indexes.isEmpty())
        return mimeData;
    mimeData->setData("integer/songid",
                      data(indexes.at(0).sibling(indexes.at(0).row(), COL_ID), Qt::DisplayRole).toByteArray().data());
    return mimeData;
}

Qt::ItemFlags TableModelKaraokeSongs::flags(const QModelIndex &index) const {
    if (!index.isValid())
        return Qt::ItemIsEnabled;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
}


void TableModelKaraokeSongs::sort(int column, Qt::SortOrder order) {
    auto sortLambda = [&column](const std::shared_ptr<okj::KaraokeSong> &a, const std::shared_ptr<okj::KaraokeSong> &b) -> bool {
        switch (column) {
            case COL_ARTIST:
                if (a->artistL == b->artistL) {
                    if (a->titleL == b->titleL) {
                        return (a->songidL < b->songidL);
                    }
                    return (a->titleL < b->titleL);
                }
                return (a->artistL < b->artistL);
            case COL_TITLE:
                if (a->titleL == b->titleL) {
                    if (a->artistL == b->artistL) {
                        return (a->songidL < b->songidL);
                    }
                    return (a->artistL < b->artistL);
                }
                return (a->titleL < b->titleL);
            case COL_SONGID:
                return (a->songidL < b->songidL);
            case COL_FILENAME:
                return (a->filename.toLower() < b->filename.toLower());
            case COL_DURATION:
                return (a->duration < b->duration);
            case COL_PLAYS:
                return (a->plays < b->plays);
            case COL_LASTPLAY:
                return (a->lastPlay < b->lastPlay);

            default:
                return (a->id < b->id);
        }
    };

    QApplication::setOverrideCursor(Qt::BusyCursor);
    if (order == Qt::AscendingOrder) {
        std::sort(m_allSongs.begin(), m_allSongs.end(), sortLambda);
    } else {
        std::sort(m_allSongs.rbegin(), m_allSongs.rend(), sortLambda);
    }
    QApplication::restoreOverrideCursor();
    invalidateSearchNarrowing();
    requestSearch();
}

void TableModelKaraokeSongs::setSongDurations(const QVector<QPair<QString, int>> &durations) {
    for (const auto &[path, duration] : durations) {
        auto it = m_songsByPath.constFind(path);
        if (it != m_songsByPath.constEnd())
            it.value()->duration = duration;
    }
    if (!m_filteredSongs.empty())
        emit dataChanged(index(0, COL_DURATION), index(static_cast<int>(m_filteredSongs.size()) - 1, COL_DURATION),
                         QVector<int>{Qt::DisplayRole});
}

void TableModelKaraokeSongs::setSongGains(const QVector<QPair<QString, double>> &gains) {
    for (const auto &[path, gain] : gains) {
        auto it = m_songsByPath.constFind(path);
        if (it != m_songsByPath.constEnd())
            it.value()->gain = gain;
    }
    if (!m_filteredSongs.empty())
        emit dataChanged(index(0, COL_GAIN), index(static_cast<int>(m_filteredSongs.size()) - 1, COL_GAIN),
                         QVector<int>{Qt::DisplayRole});
}

void TableModelKaraokeSongs::markSongBad(QString path) {
    setSongBad(std::move(path), true);
}

void TableModelKaraokeSongs::unmarkSongBad(QString path) {
    setSongBad(std::move(path), false);
}

void TableModelKaraokeSongs::setSongBad(QString path, const bool bad) {
    QSqlQuery query;
    query.prepare("UPDATE dbsongs SET bad = :bad WHERE path == :path");
    query.bindValue(":bad", bad ? 1 : 0);
    query.bindValue(":path", path);
    query.exec();

    if (const auto it = m_songsByPath.constFind(path); it != m_songsByPath.constEnd())
        it.value()->bad = bad;

    // The song has to leave one list and join the other, and which of those is on
    // screen depends on the review toggle - so re-run the filter rather than trying
    // to patch m_filteredSongs for both cases.
    invalidateSearchNarrowing();
    requestSearch();
}

TableModelKaraokeSongs::DeleteStatus TableModelKaraokeSongs::removeBadSong(QString path) {
    bool isCdg = false;
    if (QFileInfo(path).suffix().toLower() == "cdg")
        isCdg = true;
    QString mediaFile;
    if (isCdg)
        mediaFile = findCdgAudioFile(path);
    QFile file(path);
    if (file.remove()) {
        QSqlQuery query;
        query.prepare("DELETE FROM dbsongs WHERE path == :path");
        query.bindValue(":path", path);
        query.exec();

        emit layoutAboutToBeChanged();
        auto newFilteredEnd = std::remove_if(m_filteredSongs.begin(), m_filteredSongs.end(),
                                             [&path](const std::shared_ptr<okj::KaraokeSong> &song) {
                                                 return (song->path == path);
                                             });
        m_filteredSongs.erase(newFilteredEnd, m_filteredSongs.end());

        emit layoutChanged();
        auto newAllSongsEnd = std::remove_if(m_allSongs.begin(), m_allSongs.end(),
                                             [&path](const std::shared_ptr<okj::KaraokeSong> &song) {
                                                 return (song->path == path);
                                             });
        m_allSongs.erase(newAllSongsEnd, m_allSongs.end());
        m_songsByPath.remove(path);
        invalidateSearchNarrowing();

        if (isCdg) {
            if (!QFile::remove(mediaFile)) {
                return DELETE_CDG_AUDIO_FAIL;
            }
        }
    } else {
        return DELETE_FAIL;
    }
    return DELETE_OK;
}

QString TableModelKaraokeSongs::findCdgAudioFile(const QString &path) {
    m_logger->debug("{} findMatchingAudioFile({}) called", m_loggingPrefix, path.toStdString());
    // This was a verbatim second copy of findMatchingAudioFile(): the same 41-element
    // case-permutation array, the same loop, and a comment pointing at the original
    // rather than calling it. Two copies of a search order is one too many for the
    // next person who has to add an extension to it.
    return findMatchingAudioFile(path);
}

int TableModelKaraokeSongs::addSong(okj::KaraokeSong song) {
    m_logger->debug("{} addSong() called", m_loggingPrefix);
    if (int songId = getIdForPath(song.path); songId > -1) {
        m_logger->debug("{} addSong() - Song at path already exists in the db:{}", m_loggingPrefix, song.path.toStdString());
        return songId;
    }
    QSqlQuery query;
    query.prepare(
            "INSERT INTO dbSongs (discid,artist,title,path,duration,filename,searchstring) VALUES(:songid, :artist, :title, :path, :duration, :filename, :searchString)");
    query.bindValue(":songid", song.songid);
    // Trimmed to keep browse-by-letter's prefix LIKE working - see MainWindow::dbInit() v109.
    query.bindValue(":artist", song.artist.trimmed());
    query.bindValue(":title", song.title.trimmed());
    query.bindValue(":path", song.path);
    query.bindValue(":duration", song.duration);
    query.bindValue(":filename", song.filename);
    query.bindValue(":searchString", song.searchString);
    query.exec();
    if (auto error = query.lastError(); error.type() != QSqlError::NoError) {
        m_logger->error("{} Error adding song to the database", m_loggingPrefix);
        m_logger->error("{} Database error: {}", m_loggingPrefix, error.text().toStdString());
        return -1;
    } else {
        int lastInsertId = query.lastInsertId().toInt();
        song.id = lastInsertId;
        auto newSong = m_allSongs.emplace_back(std::make_shared<okj::KaraokeSong>(song));
        setSearchHaystacks(*newSong, m_haystacksIgnoreApos);
        m_songsByPath.insert(newSong->path, newSong);
        invalidateSearchNarrowing();
        requestSearch();
        return lastInsertId;
    }
}
