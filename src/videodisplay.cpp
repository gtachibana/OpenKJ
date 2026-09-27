#include "videodisplay.h"
#include <QPainter>
#include <QPaintEvent>
#include <QSvgRenderer>
#include <QRegularExpression>
#include <algorithm>
#include "qrimage.h"

VideoDisplay::VideoDisplay(QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    auto palette = this->palette();
    palette.setColor(QPalette::Window, Qt::black);
    setPalette(palette);
    setMouseTracking(true);
}


void VideoDisplay::setBackground(const QPixmap &pixmap)
{
    m_useDefaultBg = false;
    m_currentBg = pixmap;
    update();
}

void VideoDisplay::useDefaultBackground()
{
    m_useDefaultBg = true;
    update();
}

void VideoDisplay::setRequestUrl(const QString &url)
{
    if (url == m_requestUrl)
        return;
    m_requestUrl = url;
    // Encoded once here rather than on every paint.
    m_requestQr = url.isEmpty() ? QImage() : makeQrImage(url);
    update();
}

void VideoDisplay::setHasActiveVideo(const bool &value)
{
    if (m_hasActiveVideo != value)
    {
        if (value)
        {
            // Switching to having video.
            // Make sure the background is painted black initially to prevent
            // any old buffer from "bleeding though" when in HW mode.
            m_repaintBackgroundOnce = true;
        }
        m_hasActiveVideo = value;
        update();
    }
}

void VideoDisplay::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);

    if (m_hasActiveVideo)
    {
        // playing - fix black bars in hw mode (event is not called in sw mode when playing).
        // (perhaps simplify all this by always using fillOnPaint. Performance should be measured though.)
        if (m_fillOnPaint || m_repaintBackgroundOnce)
        {
            painter.fillRect(event->rect(), Qt::black);
            m_repaintBackgroundOnce = false;
        }
    }
    else
    {
        // stopped - draw background image
        painter.fillRect(event->rect(), Qt::black);
        if (m_useDefaultBg)
        {
            QSvgRenderer renderer(QString(":icons/Icons/okjlogo.svg"));
#if (QT_VERSION >= QT_VERSION_CHECK(5,15,0))
            renderer.setAspectRatioMode(Qt::KeepAspectRatio);
#endif
            renderer.render(&painter);
        }
        else
        {
            painter.drawPixmap(rect(), m_currentBg, m_currentBg.rect());
        }
        if (!m_requestUrl.isEmpty())
            paintRequestPanel(painter);
    }
}

void VideoDisplay::paintRequestPanel(QPainter &painter)
{
    // Tucked into the bottom-right corner, sized off the window height so it reads the
    // same on a small monitor and across the room on a projector.
    const int h = height();
    const int margin = std::max(8, h / 30);
    const int pad = std::max(6, h / 60);

    QFont labelFont = font();
    labelFont.setBold(true);
    labelFont.setPixelSize(std::max(10, h / 28));
    QFont urlFont = font();
    urlFont.setPixelSize(std::max(9, h / 34));
    const QFontMetrics labelMetrics(labelFont);
    const QFontMetrics urlMetrics(urlFont);
    const QString label = tr("Scan to request a song");
    // Spelled out under the code for anyone whose camera won't focus, with the scheme
    // dropped - nobody types "https://" into a phone.
    static const QRegularExpression scheme("^https?://");
    QString address = m_requestUrl;
    address.remove(scheme);

    // Nearest-neighbour and a whole number of pixels per module, so every module
    // lands on whole pixels.
    int codeSide = 0;
    if (!m_requestQr.isNull()) {
        const int scale = std::max(1, (h * 3 / 10) / m_requestQr.width());
        codeSide = scale * m_requestQr.width();
    }

    const int maxTextWidth = (width() / 2) - (pad * 2);
    const int neededTextWidth = std::min(maxTextWidth, std::max(labelMetrics.horizontalAdvance(label),
                                                                urlMetrics.horizontalAdvance(address)));
    const int panelWidth = std::max(codeSide, neededTextWidth) + (pad * 2);
    const int panelHeight = codeSide + (codeSide > 0 ? pad : 0) + labelMetrics.height() + urlMetrics.height() + (pad * 2);
    const QRect panel(width() - margin - panelWidth, h - margin - panelHeight, panelWidth, panelHeight);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 190));
    painter.drawRoundedRect(panel, pad, pad);

    int y = panel.top() + pad;
    if (codeSide > 0) {
        const QRect target(panel.left() + ((panel.width() - codeSide) / 2), y, codeSide, codeSide);
        painter.drawImage(target, m_requestQr.scaled(codeSide, codeSide, Qt::KeepAspectRatio, Qt::FastTransformation));
        y = target.bottom() + 1 + pad;
    }

    const int textLeft = panel.left() + pad;
    const int textWidth = panel.width() - (pad * 2);
    painter.setFont(labelFont);
    painter.setPen(QColor(240, 240, 245));
    painter.drawText(QRect(textLeft, y, textWidth, labelMetrics.height()), Qt::AlignHCenter | Qt::AlignVCenter,
                     labelMetrics.elidedText(label, Qt::ElideRight, textWidth));
    y += labelMetrics.height();
    painter.setFont(urlFont);
    painter.setPen(QColor(200, 200, 210));
    painter.drawText(QRect(textLeft, y, textWidth, urlMetrics.height()), Qt::AlignHCenter | Qt::AlignVCenter,
                     urlMetrics.elidedText(address, Qt::ElideRight, textWidth));
    painter.restore();
}
