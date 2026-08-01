#ifndef DLGYOUTUBECACHE_H
#define DLGYOUTUBECACHE_H

#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>

#include "youtubefetcher.h"

class Settings;

// Lists the videos downloaded for YouTube requests and lets the KJ remove them.
//
// Built in code rather than from a .ui file, matching how the Local Mode and YouTube
// settings groups are put together in this fork.
class DlgYoutubeCache : public QDialog {
Q_OBJECT

public:
    explicit DlgYoutubeCache(YoutubeFetcher &fetcher, Settings &settings, QWidget *parent = nullptr);

private:
    YoutubeFetcher &m_fetcher;
    Settings &m_settings;

    QTableWidget *m_table{nullptr};
    QLabel *m_labelSummary{nullptr};
    QPushButton *m_buttonDelete{nullptr};
    QPushButton *m_buttonSweep{nullptr};

    enum Column {
        ColArtist = 0,
        ColTitle,
        ColSize,
        ColPlays,
        ColLastPlayed,
        ColStatus,
        ColCount
    };

    void refresh();
    void deleteSelected();
    // Applies the same age and size policy that runs on startup and after a fetch,
    // so the button does exactly what the automatic sweeps do - no separate rules the
    // KJ has to learn.
    void sweepNow();
    [[nodiscard]] static QString formatBytes(qint64 bytes);
};

#endif // DLGYOUTUBECACHE_H
