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

#include "dlgqueuedisplay.h"

#include <QCloseEvent>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QRegularExpression>
#include <algorithm>
#include <stdexcept>
#include <spdlog/spdlog.h>

#include "3rdparty/qrcodegen/qrcodegen.hpp"
#include "models/tablemodelrotation.h"

namespace {
    // A dark ground so the window can sit on a TV all night without being the
    // brightest thing in the room, with the same yellow the rotation table uses for
    // the current singer so the two screens agree about who is on.
    const QColor kBackground{16, 16, 22};
    const QColor kPanel{28, 28, 38};
    const QColor kNowPlaying{255, 214, 0};
    const QColor kPrimaryText{240, 240, 245};
    const QColor kSecondaryText{165, 165, 180};
    const QColor kDimText{120, 120, 135};

    // Enough of a quiet zone for a phone camera to lock on; the spec asks for four
    // modules and this is what the panel's white card is sized around.
    constexpr int kQrQuietModules = 4;

    QString elide(const QFontMetrics &metrics, const QString &text, const int width) {
        return metrics.elidedText(text, Qt::ElideRight, width);
    }
}

DlgQueueDisplay::DlgQueueDisplay(TableModelRotation &rotationModel, QWidget *parent)
        : QDialog(parent), m_rotationModel(rotationModel) {
    m_logger = spdlog::get("logger");
    setWindowTitle("OpenKJ - Queue Display");
    // Not modal and not always-on-top: it lives on the other screen, and the KJ has to
    // be able to put a settings dialog in front of it on the machine's own display.
    setWindowFlags(windowFlags() | Qt::Window);
    setModal(false);
    setMinimumSize(480, 320);
    resize(1280, 720);
    m_settings.restoreWindowState(this);

    // The request site address is not connected to anything: every Settings instance
    // owns its own QSettings, so a change signal from the settings dialog would never
    // arrive here. It is read on each repaint instead, which the KJ's once-a-second
    // tick already drives.
    connect(&m_rotationModel, &TableModelRotation::rotationModified, this, &DlgQueueDisplay::refresh);
}

void DlgQueueDisplay::setNowPlaying(const QString &singer, const QString &artist, const QString &title,
                                    const bool playing) {
    QString song;
    if (!artist.isEmpty() && !title.isEmpty())
        song = artist + " - " + title;
    else
        song = artist + title;

    if (singer == m_nowSinger && song == m_nowSong && playing == m_playing)
        return;

    m_nowSinger = singer;
    m_nowSong = song;
    m_playing = playing;
    update();
}

void DlgQueueDisplay::refresh() {
    update();
}

QFont DlgQueueDisplay::scaledFont(const double heightFraction, const bool bold) const {
    QFont font = m_settings.applicationFont();
    // Point sizes are meaningless here - the window is whatever size the TV is - so
    // everything is sized off the window height and stays in proportion when the KJ
    // drags it to a different screen.
    font.setPixelSize(std::max(9, static_cast<int>(height() * heightFraction)));
    font.setBold(bold);
    return font;
}

QVector<DlgQueueDisplay::UpNextEntry> DlgQueueDisplay::upNextEntries(const int limit) const {
    QVector<UpNextEntry> entries;
    const auto singerCount = static_cast<int>(m_rotationModel.singerCount());
    if (singerCount == 0 || limit <= 0)
        return entries;

    // Start after whoever is up. With no current singer - between songs, or under
    // "current singer on top", where the finished singer has already been moved to the
    // bottom - the top of the rotation is the next turn.
    const int currentId = m_rotationModel.currentSinger();
    const auto &current = m_rotationModel.getSinger(currentId);
    const int start = current.isValid() ? current.position + 1 : 0;

    for (int offset = 0; offset < singerCount && entries.size() < limit; offset++) {
        const auto &singer = m_rotationModel.getSingerAtPosition((start + offset) % singerCount);
        if (!singer.isValid() || singer.id == currentId)
            continue;
        // Nothing queued means this singer is not actually up: findNextPlayableSinger
        // walks past an empty queue, and the model sinks those singers below everyone
        // with songs. Listing them would promise the room a turn that never comes - and
        // a rotation nobody cleared since a previous show is all names and no songs, so
        // the screen would be advertising people who aren't in the building. The video
        // ticker's "up next" leaves them out for the same reason.
        if (singer.numSongsUnsung() < 1)
            continue;

        UpNextEntry entry;
        entry.name = singer.name;
        entry.song = singer.nextSongArtistTitle();

        // The two states that decide whether this singer's turn actually happens. The
        // room is the audience for both of them: somebody who marked themselves away
        // and wandered back learns from the screen that OpenKJ still thinks they are
        // gone, and somebody whose video is downloading can see why they are waiting.
        if (singer.paused)
            entry.note = "stepped away";
        else if (const QString reason = singer.nextSongUnplayableReason(); !reason.isEmpty())
            entry.note = reason;

        entries.append(entry);
    }
    return entries;
}

const QImage &DlgQueueDisplay::qrImage(const QString &url) {
    if (url == m_qrSourceUrl && !m_qrImage.isNull())
        return m_qrImage;

    m_qrSourceUrl = url;
    m_qrImage = QImage();
    try {
        // MEDIUM rather than LOW: the code is photographed off a screen from across a
        // bar, at an angle, through a phone camera that is doing its best.
        const auto qr = qrcodegen::QrCode::encodeText(url.toUtf8().constData(), qrcodegen::QrCode::Ecc::MEDIUM);
        const int modules = qr.getSize();
        const int side = modules + (kQrQuietModules * 2);
        QImage image(side, side, QImage::Format_RGB32);
        image.fill(Qt::white);
        for (int y = 0; y < modules; y++) {
            for (int x = 0; x < modules; x++) {
                if (qr.getModule(x, y))
                    image.setPixel(x + kQrQuietModules, y + kQrQuietModules, qRgb(0, 0, 0));
            }
        }
        m_qrImage = image;
    } catch (const std::exception &e) {
        // Only reachable if the address is longer than a QR code can hold, which means
        // it is not an address anyone is going to type either. Drawn as text-only.
        m_logger->warn("{} Could not encode request site URL as a QR code: {}", m_loggingPrefix, e.what());
    }
    return m_qrImage;
}

int DlgQueueDisplay::paintHeader(QPainter &painter, const QRect &area) {
    const QFont labelFont = scaledFont(0.030);
    const QFont singerFont = scaledFont(0.085, true);
    const QFont songFont = scaledFont(0.045);

    int y = area.top();

    painter.setFont(labelFont);
    painter.setPen(kDimText);
    // Between songs these fields still hold whoever just finished - MainWindow only
    // overwrites them when the next song starts - so say that rather than leaving the
    // room to read a finished turn as the current one.
    const QString label = m_playing ? "NOW SINGING" : "LAST UP";
    painter.drawText(QRect(area.left(), y, area.width(), QFontMetrics(labelFont).height()),
                     Qt::AlignLeft | Qt::AlignVCenter, label);
    y += QFontMetrics(labelFont).height();

    painter.setFont(singerFont);
    painter.setPen(kNowPlaying);
    const QFontMetrics singerMetrics(singerFont);
    const QString singer = m_nowSinger.isEmpty() ? QStringLiteral("- nobody up yet -") : m_nowSinger;
    painter.drawText(QRect(area.left(), y, area.width(), singerMetrics.height()),
                     Qt::AlignLeft | Qt::AlignVCenter, elide(singerMetrics, singer, area.width()));
    y += singerMetrics.height();

    if (!m_nowSong.isEmpty()) {
        painter.setFont(songFont);
        painter.setPen(kSecondaryText);
        const QFontMetrics songMetrics(songFont);
        painter.drawText(QRect(area.left(), y, area.width(), songMetrics.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, elide(songMetrics, m_nowSong, area.width()));
        y += songMetrics.height();
    }

    return y;
}

void DlgQueueDisplay::paintUpNext(QPainter &painter, const QRect &area) {
    const QFont labelFont = scaledFont(0.030);
    const QFont nameFont = scaledFont(0.050, true);
    const QFont songFont = scaledFont(0.034);

    const QFontMetrics labelMetrics(labelFont);
    const QFontMetrics nameMetrics(nameFont);
    const QFontMetrics songMetrics(songFont);

    painter.setFont(labelFont);
    painter.setPen(kDimText);
    painter.drawText(QRect(area.left(), area.top(), area.width(), labelMetrics.height()),
                     Qt::AlignLeft | Qt::AlignVCenter, "UP NEXT");

    const int listTop = area.top() + (labelMetrics.height() * 3 / 2);
    const int rowHeight = nameMetrics.height() + songMetrics.height() + (songMetrics.height() / 3);
    const int rows = (area.bottom() - listTop) / std::max(1, rowHeight);
    const auto entries = upNextEntries(rows);

    if (entries.isEmpty()) {
        painter.setFont(songFont);
        painter.setPen(kDimText);
        painter.drawText(QRect(area.left(), listTop, area.width(), nameMetrics.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, "Nobody else has a song queued yet");
        return;
    }

    // Wide enough for two digits at this size; a rotation deep enough to need three is
    // deeper than the list can show anyway.
    const int numberWidth = nameMetrics.horizontalAdvance("00  ");
    int y = listTop;
    for (int i = 0; i < entries.size(); i++) {
        const auto &entry = entries.at(i);

        painter.setFont(nameFont);
        painter.setPen(kDimText);
        painter.drawText(QRect(area.left(), y, numberWidth, nameMetrics.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, QString::number(i + 1));

        painter.setPen(entry.note.isEmpty() ? kPrimaryText : kSecondaryText);
        const int textLeft = area.left() + numberWidth;
        const int textWidth = area.right() - textLeft;
        painter.drawText(QRect(textLeft, y, textWidth, nameMetrics.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, elide(nameMetrics, entry.name, textWidth));
        y += nameMetrics.height();

        QString second = entry.song;
        if (!entry.note.isEmpty())
            second = second.isEmpty() ? entry.note : second + "  (" + entry.note + ")";
        if (!second.isEmpty()) {
            painter.setFont(songFont);
            painter.setPen(kDimText);
            painter.drawText(QRect(textLeft, y, textWidth, songMetrics.height()),
                             Qt::AlignLeft | Qt::AlignVCenter, elide(songMetrics, second, textWidth));
        }
        y += songMetrics.height() + (songMetrics.height() / 3);
    }
}

void DlgQueueDisplay::paintRequestPanel(QPainter &painter, const QRect &area, const QString &url) {
    const QFont labelFont = scaledFont(0.034, true);
    const QFont urlFont = scaledFont(0.026);
    const QFontMetrics labelMetrics(labelFont);
    const QFontMetrics urlMetrics(urlFont);

    painter.setPen(Qt::NoPen);
    painter.setBrush(kPanel);
    painter.drawRoundedRect(area, area.width() / 20.0, area.width() / 20.0);

    const int pad = area.width() / 12;
    const int textBlock = labelMetrics.height() + urlMetrics.height() + pad;
    const int codeSide = std::min(area.width() - (pad * 2), area.height() - (pad * 2) - textBlock);

    const QImage &code = qrImage(url);
    int y = area.top() + pad;
    if (!code.isNull() && codeSide > 0) {
        // Nearest-neighbour and an integer module size, so every module lands on whole
        // pixels. A smoothly scaled QR code is a blurry QR code.
        const int scale = std::max(1, codeSide / code.width());
        const int side = scale * code.width();
        const QRect target(area.left() + ((area.width() - side) / 2), y, side, side);
        painter.drawImage(target, code.scaled(side, side, Qt::KeepAspectRatio, Qt::FastTransformation));
        y = target.bottom() + (pad / 2);
    }

    painter.setFont(labelFont);
    painter.setPen(kPrimaryText);
    painter.drawText(QRect(area.left() + pad, y, area.width() - (pad * 2), labelMetrics.height()),
                     Qt::AlignHCenter | Qt::AlignVCenter, "Scan to request a song");
    y += labelMetrics.height();

    painter.setFont(urlFont);
    painter.setPen(kSecondaryText);
    // The address is spelled out under the code for anyone whose camera won't focus,
    // with the scheme dropped - nobody types "https://" into a phone.
    static const QRegularExpression scheme("^https?://");
    QString display = url;
    display.remove(scheme);
    painter.drawText(QRect(area.left() + pad, y, area.width() - (pad * 2), urlMetrics.height()),
                     Qt::AlignHCenter | Qt::AlignVCenter, elide(urlMetrics, display, area.width() - (pad * 2)));
}

void DlgQueueDisplay::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), kBackground);

    const int margin = std::max(12, static_cast<int>(height() * 0.04));
    const QRect content = rect().adjusted(margin, margin, -margin, -margin);
    if (content.width() < 100 || content.height() < 100)
        return;

    const int headerBottom = paintHeader(painter, content);
    const int dividerY = headerBottom + (margin / 2);
    painter.setPen(QPen(kPanel, std::max(1, height() / 300)));
    painter.drawLine(content.left(), dividerY, content.right(), dividerY);

    QRect below(content.left(), dividerY + margin, content.width(), content.bottom() - dividerY - margin);
    if (below.height() < 60)
        return;

    const QString url = m_settings.requestSiteUrl();
    if (!url.isEmpty()) {
        // The panel takes a third of the width, capped so it doesn't become a billboard
        // on an ultrawide screen and leave the rotation squeezed into a column.
        const int panelWidth = std::min(below.width() / 3, static_cast<int>(below.height() * 0.9));
        const QRect panel(below.right() - panelWidth, below.top(), panelWidth, below.height());
        paintRequestPanel(painter, panel, url);
        below.setRight(panel.left() - margin);
    }

    paintUpNext(painter, below);
}

void DlgQueueDisplay::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_F11) {
        isFullScreen() ? showNormal() : showFullScreen();
        return;
    }
    if (event->key() == Qt::Key_Escape && isFullScreen()) {
        // Escape leaves fullscreen rather than closing, so the KJ can't lose the window
        // off a second screen with one keypress. QDialog would otherwise reject() here.
        showNormal();
        return;
    }
    QDialog::keyPressEvent(event);
}

void DlgQueueDisplay::mouseDoubleClickEvent(QMouseEvent *event) {
    Q_UNUSED(event)
    isFullScreen() ? showNormal() : showFullScreen();
}

void DlgQueueDisplay::closeEvent(QCloseEvent *event) {
    if (!isFullScreen())
        m_settings.saveWindowState(this);
    QDialog::closeEvent(event);
}
