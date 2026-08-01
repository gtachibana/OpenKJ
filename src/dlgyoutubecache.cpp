#include "dlgyoutubecache.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLocale>
#include <QMessageBox>
#include <QVBoxLayout>

#include "settings.h"

DlgYoutubeCache::DlgYoutubeCache(YoutubeFetcher &fetcher, Settings &settings, QWidget *parent)
        : QDialog(parent), m_fetcher(fetcher), m_settings(settings) {
    setWindowTitle(tr("Downloaded YouTube videos"));
    resize(720, 420);

    auto *layout = new QVBoxLayout(this);

    m_labelSummary = new QLabel(this);
    m_labelSummary->setWordWrap(true);
    layout->addWidget(m_labelSummary);

    m_table = new QTableWidget(0, ColCount, this);
    m_table->setHorizontalHeaderLabels({tr("Artist"), tr("Title"), tr("Size"), tr("Plays"),
                                        tr("Last sung"), tr("Status")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(false);
    m_table->horizontalHeader()->setSectionResizeMode(ColTitle, QHeaderView::Stretch);
    m_table->setSortingEnabled(false);
    layout->addWidget(m_table, 1);

    auto *buttonRow = new QHBoxLayout;
    m_buttonDelete = new QPushButton(tr("Delete selected"), this);
    m_buttonSweep = new QPushButton(tr("Remove unused now"), this);
    m_buttonSweep->setToolTip(tr("Applies the same rules that run automatically: drop videos sung at most\n"
                                 "once and untouched for the configured number of days, then, if the cache\n"
                                 "is still over its size limit, the least recently sung."));
    buttonRow->addWidget(m_buttonDelete);
    buttonRow->addWidget(m_buttonSweep);
    buttonRow->addStretch(1);
    auto *closeButton = new QPushButton(tr("Close"), this);
    buttonRow->addWidget(closeButton);
    layout->addLayout(buttonRow);

    connect(m_buttonDelete, &QPushButton::clicked, this, &DlgYoutubeCache::deleteSelected);
    connect(m_buttonSweep, &QPushButton::clicked, this, &DlgYoutubeCache::sweepNow);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this]() {
        m_buttonDelete->setEnabled(!m_table->selectedItems().isEmpty());
    });

    refresh();
}

QString DlgYoutubeCache::formatBytes(const qint64 bytes) {
    return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}

void DlgYoutubeCache::refresh() {
    const auto entries = m_fetcher.cacheEntries();
    m_table->setRowCount(static_cast<int>(entries.size()));

    qint64 total = 0;
    for (int row = 0; row < entries.size(); ++row) {
        const auto &entry = entries.at(row);
        total += entry.bytes;

        auto *artist = new QTableWidgetItem(entry.artist);
        // The video id rides along on the row so a deletion doesn't depend on the
        // table's ordering matching the fetcher's.
        artist->setData(Qt::UserRole, entry.videoId);
        m_table->setItem(row, ColArtist, artist);
        m_table->setItem(row, ColTitle, new QTableWidgetItem(entry.title));
        m_table->setItem(row, ColSize, new QTableWidgetItem(formatBytes(entry.bytes)));
        m_table->setItem(row, ColPlays, new QTableWidgetItem(QString::number(entry.plays)));
        m_table->setItem(row, ColLastPlayed, new QTableWidgetItem(
                entry.lastPlay.isValid() ? QLocale().toString(entry.lastPlay, QLocale::ShortFormat)
                                         : tr("Never")));
        m_table->setItem(row, ColStatus, new QTableWidgetItem(
                entry.queued ? tr("In a queue") : QString()));
    }
    m_table->resizeColumnsToContents();
    m_table->horizontalHeader()->setSectionResizeMode(ColTitle, QHeaderView::Stretch);
    m_buttonDelete->setEnabled(false);

    const qint64 cap = static_cast<qint64>(m_settings.youtubeCacheMaxGb()) * 1024 * 1024 * 1024;
    m_labelSummary->setText(tr("%n video(s) downloaded, using %1 of a %2 limit.", nullptr,
                               static_cast<int>(entries.size()))
                                    .arg(formatBytes(total), formatBytes(cap)));
}

void DlgYoutubeCache::deleteSelected() {
    QStringList videoIds;
    const auto selection = m_table->selectionModel()->selectedRows(ColArtist);
    for (const auto &index: selection) {
        if (auto *item = m_table->item(index.row(), ColArtist))
            videoIds.append(item->data(Qt::UserRole).toString());
    }
    if (videoIds.isEmpty())
        return;

    if (QMessageBox::question(this, tr("Delete downloaded videos"),
                              tr("Delete %n downloaded video(s)? They will be downloaded again if "
                                 "someone requests them.", nullptr, static_cast<int>(videoIds.size())),
                              QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    QStringList refused;
    for (const auto &videoId: videoIds) {
        QString error;
        if (!m_fetcher.evict(videoId, &error))
            refused.append(error);
    }
    refresh();

    if (!refused.isEmpty()) {
        refused.removeDuplicates();
        QMessageBox::information(this, tr("Some videos were kept"),
                                 tr("Not everything selected could be removed:\n\n%1")
                                         .arg(refused.join('\n')));
    }
}

void DlgYoutubeCache::sweepNow() {
    const int removed = m_fetcher.runCacheMaintenance();
    refresh();
    QMessageBox::information(this, tr("Cache cleaned up"),
                             removed > 0
                             ? tr("Removed %n video(s).", nullptr, removed)
                             : tr("Nothing needed removing."));
}
