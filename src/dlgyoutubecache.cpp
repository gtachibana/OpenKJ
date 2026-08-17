#include "dlgyoutubecache.h"

#include <QFile>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLocale>
#include <QMessageBox>
#include <QSplitter>
#include <QVBoxLayout>

#include "settings.h"

DlgYoutubeCache::DlgYoutubeCache(YoutubeFetcher &fetcher, Settings &settings, QWidget *parent)
        : QDialog(parent), m_fetcher(fetcher), m_settings(settings) {
    setWindowTitle(tr("Downloaded YouTube videos"));
    resize(1040, 520);

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

    // The list keeps the room it had before the pane arrived; the preview takes the
    // extra width, and the KJ can drag it away again if they only came here to delete.
    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(m_table);
    splitter->addWidget(buildPreviewPane());
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    splitter->setChildrenCollapsible(false);
    layout->addWidget(splitter, 1);

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
        updateTransport();
    });
    // Deliberately double-click and not selection: arrowing down the list would
    // otherwise start a video out of the venue's speakers at every step.
    connect(m_table, &QTableWidget::cellDoubleClicked, this, [this](const int row, int) {
        previewRow(row);
    });

    m_preview.setUseSilenceDetection(false);
    m_preview.setVideoOutputWidgets({m_videoDisplay});
    m_preview.setEnforceAspectRatio(m_settings.enforceAspectRatio());
    m_preview.setVolume(m_sliderVolume->value());

    connect(&m_preview, &MediaBackend::durationChanged, this, [this](const qint64 duration) {
        m_sliderPosition->setMaximum(static_cast<int>(duration));
        updateTransport();
    });
    connect(&m_preview, &MediaBackend::positionChanged, this, [this](const qint64 position) {
        if (!m_seeking)
            m_sliderPosition->setValue(static_cast<int>(position));
        updateTransport();
    });
    // EndOfMediaState arrives with the pipeline already back at NULL, so there is
    // nothing to stop here - the transport just needs to fall back to "Play".
    connect(&m_preview, &MediaBackend::stateChanged, this, [this](MediaBackend::State) {
        updateTransport();
    });

    connect(m_sliderPosition, &QSlider::sliderPressed, this, [this]() { m_seeking = true; });
    connect(m_sliderPosition, &QSlider::sliderReleased, this, [this]() {
        m_seeking = false;
        m_preview.setPosition(m_sliderPosition->value());
    });
    connect(m_sliderVolume, &QSlider::valueChanged, this, [this](const int value) {
        m_preview.setVolume(value);
    });
    connect(m_buttonPlayPause, &QPushButton::clicked, this, &DlgYoutubeCache::togglePlayPause);
    connect(m_buttonStop, &QPushButton::clicked, this, [this]() {
        stopPreview();
        updateTransport();
    });

    refresh();
}

DlgYoutubeCache::~DlgYoutubeCache() {
    stopPreview();
}

QWidget *DlgYoutubeCache::buildPreviewPane() {
    auto *pane = new QGroupBox(tr("Preview"), this);
    auto *paneLayout = new QVBoxLayout(pane);

    m_labelNowPreviewing = new QLabel(idlePreviewText(), pane);
    m_labelNowPreviewing->setWordWrap(true);
    paneLayout->addWidget(m_labelNowPreviewing);

    m_videoDisplay = new VideoDisplay(pane);
    m_videoDisplay->setMinimumSize(320, 180);
    m_videoDisplay->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    paneLayout->addWidget(m_videoDisplay, 1);

    auto *seekRow = new QHBoxLayout;
    m_sliderPosition = new QSlider(Qt::Horizontal, pane);
    m_sliderPosition->setRange(0, 0);
    seekRow->addWidget(m_sliderPosition, 1);
    m_labelPosition = new QLabel("0:00 / 0:00", pane);
    seekRow->addWidget(m_labelPosition);
    paneLayout->addLayout(seekRow);

    auto *controlRow = new QHBoxLayout;
    m_buttonPlayPause = new QPushButton(tr("Play"), pane);
    m_buttonStop = new QPushButton(tr("Stop"), pane);
    controlRow->addWidget(m_buttonPlayPause);
    controlRow->addWidget(m_buttonStop);
    controlRow->addStretch(1);
    controlRow->addWidget(new QLabel(tr("Volume"), pane));
    m_sliderVolume = new QSlider(Qt::Horizontal, pane);
    m_sliderVolume->setRange(0, 100);
    m_sliderVolume->setValue(50);
    m_sliderVolume->setMaximumWidth(120);
    // Preview audio leaves by the break music output, same as the library preview
    // dialog, so on a single-output rig the room hears it.
    m_sliderVolume->setToolTip(tr("Preview audio plays out of the break music output device."));
    controlRow->addWidget(m_sliderVolume);
    paneLayout->addLayout(controlRow);

    return pane;
}

QString DlgYoutubeCache::idlePreviewText() {
    return tr("Double-click a video to play it here.");
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
        artist->setData(PathRole, entry.path);
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
    updateTransport();
}

void DlgYoutubeCache::previewRow(const int row) {
    auto *item = m_table->item(row, ColArtist);
    if (!item)
        return;
    const auto path = item->data(PathRole).toString();
    const auto title = QString("%1 - %2").arg(item->text(),
                                              m_table->item(row, ColTitle)
                                              ? m_table->item(row, ColTitle)->text() : QString());

    stopPreview();

    if (path.isEmpty() || !QFile::exists(path)) {
        m_labelNowPreviewing->setText(tr("%1\nThe downloaded file is missing.").arg(title));
        updateTransport();
        return;
    }

    m_previewVideoId = item->data(Qt::UserRole).toString();
    m_labelNowPreviewing->setText(title);
    m_sliderPosition->setRange(0, 0);
    m_preview.setMedia(path);
    m_preview.setVolume(m_sliderVolume->value());
    m_preview.play();
    updateTransport();
}

void DlgYoutubeCache::togglePlayPause() {
    switch (m_preview.state()) {
        case MediaBackend::PlayingState:
            m_preview.pause();
            break;
        case MediaBackend::PausedState:
            m_preview.play();
            break;
        default: {
            // Nothing loaded, so the button doubles as "play what's highlighted",
            // which is what a KJ who reached for it rather than double-clicking meant.
            const auto selection = m_table->selectionModel()->selectedRows(ColArtist);
            if (!selection.isEmpty())
                previewRow(selection.first().row());
            break;
        }
    }
    updateTransport();
}

void DlgYoutubeCache::stopPreview() {
    m_previewVideoId.clear();
    m_seeking = false;
    // Unconditional, and not guarded on state(): end of stream already reports as
    // stopped while the pipeline still holds the file open, and on Windows that open
    // handle is enough to make a delete or a sweep fail. rawStop() drives the pipeline
    // to NULL synchronously and is harmless when it is already there.
    m_preview.rawStop();
    m_sliderPosition->setRange(0, 0);
}

void DlgYoutubeCache::updateTransport() {
    const auto state = m_preview.state();
    const bool loaded = state == MediaBackend::PlayingState || state == MediaBackend::PausedState;

    m_buttonPlayPause->setText(state == MediaBackend::PlayingState ? tr("Pause") : tr("Play"));
    m_buttonPlayPause->setEnabled(loaded || !m_table->selectionModel()->selectedRows().isEmpty());
    m_buttonStop->setEnabled(loaded);
    m_sliderPosition->setEnabled(loaded);

    if (!loaded) {
        m_labelPosition->setText("0:00 / 0:00");
        return;
    }
    m_labelPosition->setText(QString("%1 / %2").arg(MediaBackend::msToMMSS(m_preview.position()),
                                                    MediaBackend::msToMMSS(m_preview.duration())));
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

    // A file being previewed is still open, and Windows will not let it be removed.
    if (videoIds.contains(m_previewVideoId)) {
        stopPreview();
        m_labelNowPreviewing->setText(idlePreviewText());
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
    // The sweep picks its own victims, and the one on screen could be among them.
    stopPreview();
    m_labelNowPreviewing->setText(idlePreviewText());

    const int removed = m_fetcher.runCacheMaintenance();
    refresh();
    QMessageBox::information(this, tr("Cache cleaned up"),
                             removed > 0
                             ? tr("Removed %n video(s).", nullptr, removed)
                             : tr("Nothing needed removing."));
}
