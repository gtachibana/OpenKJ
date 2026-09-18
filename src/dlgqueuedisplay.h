/*
 * Copyright (c) 2013-2021 Thomas Isaac Lightburn
 *
 *
 * This file is part of OpenKJ.
 *
 * OpenKJ is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef OPENKJ_DLGQUEUEDISPLAY_H
#define OPENKJ_DLGQUEUEDISPLAY_H

#include <QDialog>
#include <QImage>
#include <QString>
#include <QVector>
#include <memory>
#include <spdlog/async_logger.h>
#include "settings.h"

class QPainter;
class TableModelRotation;

// The audience-facing view of the rotation: who is singing, who is coming up, and how
// to get into the list. Meant for a TV or a projector on a second screen, so it is
// painted rather than laid out in widgets - the type is sized from the window, and the
// list shows as many singers as fit at a size that reads from across a room.
//
// Deliberately read-only and input-free. It is pointed at a room, not at the KJ, and
// anything clickable on it would be clicked by whoever walks past the TV.
class DlgQueueDisplay : public QDialog {
Q_OBJECT

public:
    explicit DlgQueueDisplay(TableModelRotation &rotationModel, QWidget *parent = nullptr);

public slots:
    // What MainWindow has on its own now-playing labels. It has to come from there:
    // the rotation knows a singer's *next* song, and between the song starting and the
    // queue entry being marked played there is nothing in the database that says which
    // one is actually on.
    void setNowPlaying(const QString &singer, const QString &artist, const QString &title, bool playing);
    // Cheap - it only schedules a repaint. Everything is read out of the rotation model
    // at paint time, so there is no copy here to fall out of step with the real one.
    void refresh();

protected:
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    struct UpNextEntry {
        QString name;
        QString song;
        QString note;
    };

    TableModelRotation &m_rotationModel;
    Settings m_settings;
    std::shared_ptr<spdlog::logger> m_logger;
    std::string m_loggingPrefix{"[DlgQueueDisplay]"};

    QString m_nowSinger;
    QString m_nowSong;
    bool m_playing{false};

    // Regenerated only when the address or the panel size changes - encoding is cheap
    // but this repaints on a one second tick for the length of a show.
    QImage m_qrImage;
    QString m_qrSourceUrl;

    [[nodiscard]] QVector<UpNextEntry> upNextEntries(int limit) const;
    [[nodiscard]] const QImage &qrImage(const QString &url);
    int paintHeader(QPainter &painter, const QRect &area);
    void paintUpNext(QPainter &painter, const QRect &area);
    void paintRequestPanel(QPainter &painter, const QRect &area, const QString &url);
    [[nodiscard]] QFont scaledFont(double heightFraction, bool bold = false) const;
};

#endif //OPENKJ_DLGQUEUEDISPLAY_H
