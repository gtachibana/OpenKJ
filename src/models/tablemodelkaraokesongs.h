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
        COL_LASTPLAY
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
    int getIdForPath(const QString &path);
    QString getPath(int songId);
    void updateSongHistory(int songId);
    okj::KaraokeSong &getSong(int songId);
    void markSongBad(QString path);
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
    int m_curFontHeight{0};
    QImage m_iconCdg;
    QImage m_iconZip;
    QImage m_iconVid;
    SearchType m_searchType{SearchType::SEARCH_TYPE_ALL};
    Settings m_settings;
    QFont m_itemFont;
    int m_itemHeight{20};
    QFont m_headerFont;
    QFontMetrics m_itemFontMetrics{m_settings.applicationFont()};
    QTimer searchTimer{this};

    void searchExec();
    void rebuildPathHash();
    static QVariant getColumnName(int section) ;
    [[nodiscard]] QVariant getColumnSizeHint(int section) const;
    [[nodiscard]] QVariant getItemDisplayData(const QModelIndex &index) const;
    [[nodiscard]] static QVariant getColumnTextAlignmentHint(int column) ;
    [[nodiscard]] QVariant getColumnDecorationRole(const QModelIndex &index) const;

public slots:
    void setSongDurations(const QVector<QPair<QString, int>> &durations);
    void resizeIconsForFont(const QFont &font);

};

#endif // TABLEMODELKARAOKESONGS_H
