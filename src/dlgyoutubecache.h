#ifndef DLGYOUTUBECACHE_H
#define DLGYOUTUBECACHE_H

#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QTableWidget>

#include "mediabackend.h"
#include "videodisplay.h"
#include "youtubefetcher.h"

class Settings;

// Lists the videos downloaded for YouTube requests and lets the KJ watch one and
// remove them.
//
// Built in code rather than from a .ui file, matching how the Local Mode and YouTube
// settings groups are put together in this fork.
class DlgYoutubeCache : public QDialog {
Q_OBJECT

public:
    explicit DlgYoutubeCache(YoutubeFetcher &fetcher, Settings &settings, QWidget *parent = nullptr);
    ~DlgYoutubeCache() override;

private:
    YoutubeFetcher &m_fetcher;
    Settings &m_settings;

    QTableWidget *m_table{nullptr};
    QLabel *m_labelSummary{nullptr};
    QPushButton *m_buttonDelete{nullptr};
    QPushButton *m_buttonSweep{nullptr};

    // Its own pipeline, so previewing never disturbs whatever the show is playing.
    // VideoPreview is the same type the library preview dialog uses, which means it
    // tolerates media with no audio track rather than treating that as an error.
    MediaBackend m_preview{this, "YTCACHEPREVIEW", MediaBackend::VideoPreview};
    VideoDisplay *m_videoDisplay{nullptr};
    QLabel *m_labelNowPreviewing{nullptr};
    QLabel *m_labelPosition{nullptr};
    QSlider *m_sliderPosition{nullptr};
    QSlider *m_sliderVolume{nullptr};
    QPushButton *m_buttonPlayPause{nullptr};
    QPushButton *m_buttonStop{nullptr};
    // The row whose file is loaded in the preview pipeline. Held so a delete or a
    // sweep can tear the preview down first - on Windows the open handle would
    // otherwise stop the file being removed.
    QString m_previewVideoId;
    // True while the KJ is dragging the seek bar, so position updates arriving from
    // the pipeline don't yank the handle back out from under them.
    bool m_seeking{false};

    enum Column {
        ColArtist = 0,
        ColTitle,
        ColSize,
        ColPlays,
        ColLastPlayed,
        ColStatus,
        ColCount
    };

    // The cache path stashed on each row alongside the video id, so previewing does
    // not have to go back to the fetcher for it.
    static constexpr int PathRole = Qt::UserRole + 1;

    void refresh();
    void deleteSelected();
    // Applies the same age and size policy that runs on startup and after a fetch,
    // so the button does exactly what the automatic sweeps do - no separate rules the
    // KJ has to learn.
    void sweepNow();

    QWidget *buildPreviewPane();
    void previewRow(int row);
    void togglePlayPause();
    // Stops playback and waits for the pipeline to let go of the file. Cheap when
    // nothing is loaded, so callers about to touch the disk can just call it.
    void stopPreview();
    void updateTransport();
    // What the pane says when nothing is loaded, which is also how the KJ finds out
    // the preview exists at all.
    [[nodiscard]] static QString idlePreviewText();

    [[nodiscard]] static QString formatBytes(qint64 bytes);
};

#endif // DLGYOUTUBECACHE_H
