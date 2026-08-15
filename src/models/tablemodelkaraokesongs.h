#ifndef TABLEMODELKARAOKESONGS_H
#define TABLEMODELKARAOKESONGS_H

#include <QAbstractTableModel>
#include <QDateTime>
#include <QHash>
#include <QImage>
#include <QPair>
#include <QVector>
#include <memory>
#include <QTimer>
#include "settings.h"
#include <spdlog/spdlog.h>
#include <spdlog/async_logger.h>
#include "okjtypes.h"



class TableModelKaraokeSongs : public QAbstractTableModel {
Q_OBJECT

public:
    enum ModelCols {
        COL_ID=0,
        COL_ARTIST,
        COL_TITLE,
        COL_SONGID,
        COL_FILENAME,
        COL_DURATION,
        COL_PLAYS,
        COL_LASTPLAY,
        // Kept last so the saved column widths of existing installs still line up.
        COL_GAIN
    };
    enum SearchType {
        SEARCH_TYPE_ALL=1,
        SEARCH_TYPE_ARTIST,
        SEARCH_TYPE_TITLE
    };
    enum DeleteStatus {
        DELETE_OK,
        DELETE_FAIL,
        DELETE_CDG_AUDIO_FAIL
    };

    explicit TableModelKaraokeSongs(QObject *parent = nullptr);
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    [[nodiscard]] int rowCount(const QModelIndex &parent) const override;
    [[nodiscard]] int columnCount(const QModelIndex &parent) const override;
    [[nodiscard]] QMimeData *mimeData(const QModelIndexList &indexes) const override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex &index) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index, int role) const override;
    void loadData();
    void loadDataFrom(const TableModelKaraokeSongs &source);
    void sort(int column, Qt::SortOrder order) override;
    void search(const QString &searchString);
    void setSearchType(SearchType type);
    // Swaps the library view over to the songs marked bad, which are hidden from
    // every other list in the app - this is the only place they can be seen.
    void setShowBadOnly(bool showBadOnly);
    [[nodiscard]] bool showBadOnly() const { return m_showBadOnly; }
    // Returned by getSong() for a song id the library does not hold, the way
    // TableModelRotation hands back InvalidSinger. Callers must check isValid():
    // this is reached with ids that came off the network.
    okj::KaraokeSong InvalidSong{.id = -1};
    int getIdForPath(const QString &path);
    // Empty when the id is unknown.
    QString getPath(int songId);
    void updateSongHistory(int songId);
    okj::KaraokeSong &getSong(int songId);
    void markSongBad(QString path);
    void unmarkSongBad(QString path);
    DeleteStatus removeBadSong(QString path);
    QString findCdgAudioFile(const QString& path);
    int addSong(okj::KaraokeSong song);


private:
    std::string m_loggingPrefix{"[KaraokeSongsModel]"};
    std::shared_ptr<spdlog::logger> m_logger;
    std::vector<std::shared_ptr<okj::KaraokeSong>> m_filteredSongs;
    std::vector< std::shared_ptr<okj::KaraokeSong> > m_allSongs;
    QHash<QString, std::shared_ptr<okj::KaraokeSong>> m_songsByPath;
    QString m_lastSearch;
    QString m_lastSearchRaw;
    QString m_lastExecutedSearch;
    bool m_canNarrowSearch{false};
    bool m_haystacksIgnoreApos{false};
    int m_curFontHeight{0};
    QImage m_iconCdg;
    QImage m_iconZip;
    QImage m_iconVid;
    SearchType m_searchType{SearchType::SEARCH_TYPE_ALL};
    bool m_showBadOnly{false};
    Settings m_settings;
    QFont m_itemFont;
    int m_itemHeight{20};
    QFont m_headerFont;
    QFontMetrics m_itemFontMetrics{m_settings.applicationFont()};
    QTimer searchTimer{this};

    void searchExec();
    void requestSearch();
    void invalidateSearchNarrowing();
    void rebuildSearchHaystacks(bool ignoreApos);
    static void setSearchHaystacks(okj::KaraokeSong &song, bool ignoreApos);
    [[nodiscard]] const QString &searchHaystack(const okj::KaraokeSong &song) const;
    void rebuildPathHash();
    void setSongBad(QString path, bool bad);
    static QVariant getColumnName(int section) ;
    [[nodiscard]] QVariant getColumnSizeHint(int section) const;
    [[nodiscard]] QVariant getItemDisplayData(const QModelIndex &index) const;
    [[nodiscard]] static QVariant getColumnTextAlignmentHint(int column) ;
    [[nodiscard]] static QString gainDisplayText(double gain);
    [[nodiscard]] QVariant getColumnDecorationRole(const QModelIndex &index) const;

public slots:
    void setSongDurations(const QVector<QPair<QString, int>> &durations);
    void setSongGains(const QVector<QPair<QString, double>> &gains);
    void resizeIconsForFont(const QFont &font);

};

#endif // TABLEMODELKARAOKESONGS_H
