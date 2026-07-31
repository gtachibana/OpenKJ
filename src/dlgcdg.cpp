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

#include "dlgcdg.h"
#include "ui_dlgcdg.h"
#include <QScreen>
#include <QSvgRenderer>
#include <QPainter>
#include <QDir>
#include <QImageReader>
#include <QWindow>
#include <QRandomGenerator>
#include <algorithm>
#include <cmath>
#include <iterator>
#ifdef Q_OS_WIN
#include <windows.h>
#endif


VideoDisplay *DlgCdg::getVideoDisplay()
{
    return ui->videoDisplayKar;
}

VideoDisplay *DlgCdg::getVideoDisplayBm()
{
    return ui->videoDisplayBm;
}

DlgCdg::DlgCdg(MediaBackend &KaraokeBackend, MediaBackend &BreakBackend, QWidget *parent, Qt::WindowFlags f) :
    QDialog(parent, f), ui(new Ui::DlgCdg), m_kmb(KaraokeBackend), m_bmb(BreakBackend)
{
    ui->setupUi(this);
    m_tWidget = std::make_unique<TransparentWidget>(this);
    m_tWidget->setObjectName("DurationTimer");
    m_tWidget->show();
    m_tWidget->move(m_settings.durationPosition());
    m_pitchCue = std::make_unique<PitchCueWidget>(this);
    m_pitchCue->setObjectName("PitchCue");
    m_pitchCue->hide();
    m_cheers = std::make_unique<CheerWidget>(this);
    m_cheers->setObjectName("Cheers");
    m_cheers->hide();
    ui->videoDisplayKar->setFillOnPaint(false);
    ui->widgetAlert->setAutoFillBackground(true);
    ui->fsToggleWidget->hide();
    ui->widgetAlert->hide();
    ui->widgetAlert->setAttribute(Qt::WA_TransparentForMouseEvents);
    ui->widgetAlert->setMouseTracking(true);
    ui->scroll->setVisible(m_settings.tickerEnabled());
    ui->scroll->setTickerEnabled(m_settings.tickerEnabled());
    m_tWidget->setVisible(m_settings.cdgRemainEnabled());
    tickerFontChanged();
    ui->scroll->setSpeed(m_settings.tickerSpeed());
    m_tWidget->setTextFont(m_settings.cdgRemainFont());
    QPalette palette = ui->scroll->palette();
    palette.setColor(ui->scroll->foregroundRole(), m_settings.tickerTextColor());
    ui->scroll->setPalette(palette);
    palette = this->palette();
    palette.setColor(QPalette::Window, m_settings.tickerBgColor());
    setPalette(palette);
    m_lastSize.setWidth(300);
    m_lastSize.setHeight(216);
    m_fullScreen = m_settings.cdgWindowFullscreen();
    m_tWidget->setTextColor(m_settings.cdgRemainTextColor());
    m_tWidget->setBackgroundColor(m_settings.cdgRemainBgColor());
    applyBackgroundImageMode();
    showAlert(false);
    alertFontChanged(m_settings.karaokeAAAlertFont());
    alertBgColorChanged(m_settings.alertBgColor());
    alertTxtColorChanged(m_settings.alertTxtColor());
    cdgOffsetsChanged();
    connect(ui->videoDisplayKar, &VideoDisplay::mouseMoveEvent, this, &DlgCdg::mouseMove);
    connect(ui->btnToggleFullscreen, &QPushButton::clicked, this, &DlgCdg::btnToggleFullscreenClicked);
    connect(&m_timerSlideShow, &QTimer::timeout, this, &DlgCdg::timerSlideShowTimeout);
    connect(&m_timer1s, &QTimer::timeout, this, &DlgCdg::timer1sTimeout);
    connect(&m_timerAlertCountdown, &QTimer::timeout, this, &DlgCdg::timerCountdownTimeout);
    connect(&m_timerButtonShow, &QTimer::timeout, [&] () { ui->fsToggleWidget->hide(); });
    m_timerButtonShow.setInterval(1000);
    m_timerAlertCountdown.setInterval(1000);
    m_timer1s.start(1000);
    if (m_settings.bgMode() == Settings::BG_MODE_SLIDESHOW)
        m_timerSlideShow.start(static_cast<int>(m_settings.slideShowInterval() * 1000));
    ui->videoDisplayBm->hide();
    if (!m_settings.showCdgWindow())
        hide();
    else
        show();
}

DlgCdg::~DlgCdg() = default;

void DlgCdg::setTickerText(const QString &text)
{
    ui->scroll->setText(text);
}

void DlgCdg::stopTicker()
{
    ui->scroll->stop();
}

void DlgCdg::tickerFontChanged()
{
    ui->scroll->setFont(m_settings.tickerFont());
    ui->scroll->setMinimumHeight(QFontMetrics(m_settings.tickerFont()).height());
    ui->scroll->setMaximumHeight(QFontMetrics(m_settings.tickerFont()).height());
    ui->scroll->refresh();
}

void DlgCdg::tickerSpeedChanged()
{
    ui->scroll->setSpeed(m_settings.tickerSpeed());
}

void DlgCdg::tickerTextColorChanged()
{
    auto palette = ui->scroll->palette();
    palette.setColor(ui->scroll->foregroundRole(), m_settings.tickerTextColor());
    ui->scroll->setPalette(palette);
    ui->scroll->refresh();
}

void DlgCdg::tickerBgColorChanged()
{
    auto palette = this->palette();
    palette.setColor(QPalette::Window, m_settings.tickerBgColor());
    this->setPalette(palette);
    ui->scroll->refresh();
    //ui->scroll->refreshTickerSettings();
}

void DlgCdg::tickerEnableChanged()
{
    ui->scroll->setVisible(m_settings.tickerEnabled());
    ui->scroll->setTickerEnabled(m_settings.tickerEnabled());
    ui->scroll->setText(ui->scroll->getCurrentText(), true);
}

void DlgCdg::mouseDoubleClickEvent([[maybe_unused]]QMouseEvent *e)
{
    m_fullScreen = !m_fullScreen;
    if (m_fullScreen)
        showFullScreen();
    else
        showNormal();
    cdgOffsetsChanged();
    m_settings.setCdgWindowFullscreen(m_fullScreen);
    m_settings.saveWindowState(this);
    auto *currentScreen = screen();
    if (!currentScreen) {
        currentScreen = QGuiApplication::primaryScreen();
    }
    if (currentScreen) {
        m_settings.setCdgWindowFullscreenMonitor(QGuiApplication::screens().indexOf(currentScreen));
    }
}

QFileInfoList DlgCdg::getSlideShowImages()
{
    QFileInfoList images;
    QDir srcDir(m_settings.bgSlideShowDir());
    auto files = srcDir.entryInfoList(QDir::Files, QDir::Name | QDir::IgnoreCase);
    for (const auto & file : files)
    {
        if (QImageReader::imageFormat(file.absoluteFilePath()) != "")
            images << file;
    }
    return images;
}

void DlgCdg::showAlert(bool show)
{
    if ((show) && (m_settings.karaokeAAAlertEnabled()))
    {
        ui->videoDisplayKar->hide();
        ui->videoDisplayBm->hide();
        ui->widgetAlert->show();
    }
    else
    {
        ui->widgetAlert->hide();
        if (m_bmb.hasActiveVideo() && !m_kmb.hasActiveVideo()) {
            ui->videoDisplayBm->show();
            ui->videoDisplayKar->hide();
        }
        else {
            ui->videoDisplayBm->hide();
            ui->videoDisplayKar->show();
        }
    }
}

void DlgCdg::setNextSinger(const QString &name)
{
    ui->lblNextSinger->setText(name);
}

void DlgCdg::setNextSong(const QString &song)
{
    ui->lblNextSong->setText(song);
}

void DlgCdg::setCountdownSecs(int seconds)
{
    m_countdownPos = seconds;
    ui->lblSeconds->setText(tr("Starts in: ") + QString::number(seconds) + tr(" seconds"));
    m_timerAlertCountdown.stop();
    m_timerAlertCountdown.start();
}

void DlgCdg::timerCountdownTimeout()
{
    if (m_countdownPos > 0)
        m_countdownPos--;
    ui->lblSeconds->setText(tr("Starts in: ") + QString::number(m_countdownPos) + tr(" seconds"));
    ui->lblSeconds->repaint();
    ui->widgetAlert->repaint();
}

void DlgCdg::showPitchCue(const int semitones, const bool up)
{
    if (!m_settings.cdgPitchCueEnabled())
        return;

    // Sized and placed on every cue rather than in a resizeEvent - the badge is only
    // on screen for a couple of seconds, and this keeps DlgCdg's geometry handling
    // (which fullscreen transitions already make delicate) untouched.
    QFont font = m_settings.cdgRemainFont();
    font.setPointSize(std::max(font.pointSize(), 24));
    font.setBold(true);
    m_pitchCue->setTextFont(font);

    const int margin = std::max(12, width() / 40);
    m_pitchCue->move(width() - m_pitchCue->width() - margin, margin);
    m_pitchCue->raise();
    m_pitchCue->showCue(semitones, up);
}

void DlgCdg::showCheers(const QString &reaction, const int count, const int songTotal)
{
    if (!m_settings.cheersEnabled())
        return;

    // Resized on every batch rather than from a resizeEvent, for the same reason the
    // key cue is: it keeps this out of DlgCdg's geometry handling, which fullscreen
    // transitions already make delicate. Batches arrive every 250ms while the room is
    // cheering, so a window that changed size mid-ovation corrects itself immediately.
    // Drawn over everything, the between-songs alert screen included. That screen is
    // opaque, so anything behind it is simply not there - and the ovation as a song
    // ends is the moment this feature exists for, which is exactly when that screen
    // is up. The overlay is transparent apart from the glyphs themselves, so the
    // alert's text stays readable underneath.
    m_cheers->setGeometry(rect());
    m_cheers->addCheers(reaction, count, songTotal);
}

void DlgCdg::alertBgColorChanged(const QColor &color)
{
    auto palette = ui->widgetAlert->palette();
    palette.setColor(ui->widgetAlert->backgroundRole(), color);
    ui->widgetAlert->setPalette(palette);
}

void DlgCdg::alertTxtColorChanged(const QColor &color)
{
    auto palette = ui->widgetAlert->palette();
    palette.setColor(ui->widgetAlert->foregroundRole(), color);
    ui->widgetAlert->setPalette(palette);
}

void DlgCdg::applyBackgroundImageMode()
{
    if (m_settings.bgMode() == Settings::BgMode::BG_MODE_IMAGE && QFile::exists(m_settings.cdgDisplayBackgroundImage()))
    {
        m_timerSlideShow.stop();
        ui->videoDisplayKar->setBackground(QPixmap(m_settings.cdgDisplayBackgroundImage()));
    }
    else if (m_settings.bgMode() == Settings::BgMode::BG_MODE_SLIDESHOW && QDir(m_settings.bgSlideShowDir()).exists())
    {
        m_timerSlideShow.start();
        slideShowMoveNext();
    }
    else
    {
        m_timerSlideShow.stop();
        ui->videoDisplayKar->useDefaultBackground();
    }
}

void DlgCdg::timerSlideShowTimeout()
{
    if (!ui->videoDisplayKar->hasActiveVideo() && !ui->videoDisplayBm->hasActiveVideo())
    {
        slideShowMoveNext();
    }
}

void DlgCdg::slideShowMoveNext()
{
    m_curSlideshowPos++;
    auto images = getSlideShowImages();
    if (images.empty())
    {
        ui->videoDisplayKar->useDefaultBackground();
        return;
    }
    if (m_curSlideshowPos >= images.size())
        m_curSlideshowPos = 0;
    if (images.at(m_curSlideshowPos).fileName().endsWith("svg", Qt::CaseInsensitive))
    {
        QPixmap bgImage(QSize(1920,1080));
        QPainter painter(&bgImage);
        QSvgRenderer renderer(images.at(m_curSlideshowPos).absoluteFilePath());
        renderer.render(&painter);
        ui->videoDisplayKar->setBackground(bgImage);
    }
    else
        ui->videoDisplayKar->setBackground(images.at(m_curSlideshowPos).absoluteFilePath());
}

void DlgCdg::alertFontChanged(const QFont &font)
{
    QFont f = font;
    f.setPointSize(50);
    ui->label->setFont(f);
    ui->label_2->setFont(f);
    ui->lblNextSinger->setFont(f);
    ui->lblNextSong->setFont(f);
    ui->lblSeconds->setFont(f);
}

void DlgCdg::mouseMove([[maybe_unused]]QMouseEvent *event)
{
    if (m_fullScreen)
        ui->btnToggleFullscreen->setText(tr("Make Windowed"));
    else
        ui->btnToggleFullscreen->setText(tr("Make Fullscreen"));
    ui->fsToggleWidget->show();
    m_timerButtonShow.start();
}

void DlgCdg::timer1sTimeout()
{
    if (m_settings.cdgRemainEnabled())
    {
        if (m_kmb.state() == MediaBackend::PlayingState && !m_tWidget->isVisible())
        {
            m_tWidget->show();
        }
        else if (m_kmb.state() != MediaBackend::PlayingState && m_tWidget->isVisible())
        {
            m_tWidget->hide();
        }
        if (m_kmb.state() == MediaBackend::PlayingState)
        {
            m_tWidget->setString(" " + MediaBackend::msToMMSS(m_kmb.duration() - m_kmb.position()) + " ");
        }
    }
}


void DlgCdg::btnToggleFullscreenClicked()
{
    m_fullScreen = !m_fullScreen;
    if (m_fullScreen)
    {
        showFullScreen();
#ifdef Q_OS_WIN
        SetWindowPos(reinterpret_cast<HWND>(winId()), HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#endif
    }
    else
    {
        showNormal();
#ifdef Q_OS_WIN
        SetWindowPos(reinterpret_cast<HWND>(winId()), HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#endif
    }
    m_settings.setCdgWindowFullscreen(m_fullScreen);
    m_settings.saveWindowState(this);
    auto *currentScreen = screen();
    if (!currentScreen) {
        currentScreen = QGuiApplication::primaryScreen();
    }
    if (currentScreen) {
        m_settings.setCdgWindowFullscreenMonitor(QGuiApplication::screens().indexOf(currentScreen));
    }
    cdgOffsetsChanged();
}

void DlgCdg::cdgOffsetsChanged()
{
    if (isFullScreen())
        this->layout()->setContentsMargins(m_settings.cdgOffsetLeft(),m_settings.cdgOffsetTop(),m_settings.cdgOffsetRight(),m_settings.cdgOffsetBottom());
    else
        this->layout()->setContentsMargins(0,0,0,0);
}


void DlgCdg::closeEvent([[maybe_unused]]QCloseEvent *event)
{
    this->hide();
    m_settings.setShowCdgWindow(false);
    event->ignore();
    emit visibilityChanged(false);
}

void DlgCdg::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    // Two guards against infinite re-entrance:
    //
    // 1. m_fullscreenTransitionActive — blocks synchronous re-entrant showEvent() calls
    //    that arrive via SendMessage(WM_SHOWWINDOW) during showFullScreen().
    //
    // 2. isFullScreen() check — blocks asynchronous re-entrant showEvent() calls that
    //    arrive via PostMessage(WM_SHOWWINDOW) after showFullScreen() has returned and
    //    m_fullscreenTransitionActive has already been cleared. By the time showFullScreen()
    //    calls setWindowState(), Qt's window state is updated immediately, so isFullScreen()
    //    returns true for any subsequent showEvent() triggered by that same transition.
    if (m_fullscreenTransitionActive)
        return;
    m_settings.setShowCdgWindow(true);
    if (m_settings.cdgWindowFullscreen() && !isFullScreen())
    {
        // Do NOT call restoreGeometry() here — it would briefly show the window at its
        // saved windowed position before going fullscreen, causing visible jumping.
        // Defer showFullScreen() by one event-loop tick so we are fully outside this
        // showEvent() call frame before changing window state.
        m_fullscreenTransitionActive = true;
        QTimer::singleShot(0, this, [this] () {
            ui->btnToggleFullscreen->setText("Make Windowed");
            // Move to the saved monitor before going fullscreen so the window
            // lands on the correct screen (e.g. TV output) rather than the primary.
            auto screens = QGuiApplication::screens();
            int monitorIdx = m_settings.cdgWindowFullScreenMonitor();
            if (monitorIdx >= 0 && monitorIdx < screens.count()) {
                if (auto *wh = windowHandle())
                    wh->setScreen(screens.at(monitorIdx));
            }
            this->showFullScreen();
#ifdef Q_OS_WIN
            SetWindowPos(reinterpret_cast<HWND>(this->winId()), HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#endif
            m_fullscreenTransitionActive = false;
            cdgOffsetsChanged();
        });
    }
    else if (!m_settings.cdgWindowFullscreen())
    {
        m_settings.restoreWindowState(this);
        ui->btnToggleFullscreen->setText("Make Fullscreen");
    }
    emit visibilityChanged(true);
}

void DlgCdg::hideEvent(QHideEvent *event)
{
    m_settings.saveWindowState(this);
    QWidget::hideEvent(event);
}

void DlgCdg::setSlideshowInterval(int secs) {
    m_timerSlideShow.setInterval(secs * 1000);
}


TransparentWidget::~TransparentWidget()
{
    m_settings.saveWindowState(this);
}

void TransparentWidget::mouseMoveEvent(QMouseEvent *event)
{
    this->move(event->globalPosition().toPoint() + m_startPoint);
    m_settings.setDurationPosition(this->pos());
}

void TransparentWidget::moveEvent(QMoveEvent *event)
{
    QWidget::moveEvent(event);
    //settings.saveWindowState(this);
}

void TransparentWidget::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        m_startPoint = frameGeometry().topLeft() - event->globalPosition().toPoint();
    }
}

void TransparentWidget::setString(const QString &string) const {
    m_label->setText(string);
}

void TransparentWidget::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.fillRect (this->rect(), QColor(0, 0, 0, 0x20)); /* set transparent color*/
}

TransparentWidget::TransparentWidget(QWidget *parent)
        : QWidget(parent)
{
    setWindowFlags(Qt::FramelessWindowHint);
    auto layout = new QHBoxLayout(this);
    setLayout(layout);
    layout->setSpacing(0);
    layout->setContentsMargins(0,0,0,0);
    setContentsMargins(0,0,0,0);
    m_label = std::make_unique<QLabel>(this);
    layout->addWidget(m_label.get());
    m_label->setContentsMargins(0,0,0,0);
    m_label->setSizePolicy(QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding));
    m_label->setText("00:00");
    m_label->setAutoFillBackground(true);
    m_label->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
}

void TransparentWidget::setTextColor(const QColor &color) const {
    auto palette = m_label->palette();
    palette.setColor(QPalette::WindowText, color);
    m_label->setPalette(palette);
}

void TransparentWidget::setBackgroundColor(const QColor &color) const {
    auto palette = m_label->palette();
    palette.setColor(QPalette::Window, color);
    m_label->setPalette(palette);
}

void TransparentWidget::setTextFont(const QFont &font) {
    m_label->setFont(font);
    m_label->setAlignment(Qt::AlignVCenter | Qt::AlignHCenter);
#if (QT_VERSION >= QT_VERSION_CHECK(5,11,0))
    setFixedSize(QFontMetrics(font).horizontalAdvance("_____"), QFontMetrics(font).height());
    m_label->setFixedSize(QFontMetrics(font).horizontalAdvance("_____"), QFontMetrics(font).height());
#else
    setFixedSize(QFontMetrics(font).width("_____"),QFontMetrics(font).tightBoundingRect("0123456789:").height());
    m_label->setFixedSize(QFontMetrics(font).width("_____"),QFontMetrics(font).tightBoundingRect("0123456789:").height());
#endif
}

void TransparentWidget::resetPosition() {
    move(0,0);
}

PitchCueWidget::PitchCueWidget(QWidget *parent)
        : QWidget(parent)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    m_font = font();
    m_fadeTimer.setInterval(c_tickMs);
    connect(&m_fadeTimer, &QTimer::timeout, this, &PitchCueWidget::onFadeTick);
}

void PitchCueWidget::setTextFont(const QFont &font)
{
    m_font = font;
    const QFontMetrics metrics(m_font);
    // Widest string the badge can hold ("-12" plus the arrow), so a cue never
    // reflows or clips as the singer walks the key up and down.
    const int textWidth = metrics.horizontalAdvance(QString(c_arrowUp) + " -12");
    setFixedSize(textWidth + metrics.height(), metrics.height() + metrics.height() / 2);
}

void PitchCueWidget::showCue(const int semitones, const bool up)
{
    m_text = QString("%1 %2%3")
                     .arg(up ? c_arrowUp : c_arrowDown)
                     .arg(semitones > 0 ? QStringLiteral("+") : QStringLiteral(""))
                     .arg(semitones);
    m_elapsedMs = 0;
    m_opacity = 1.0;
    show();
    raise();
    update();
    m_fadeTimer.start();
}

void PitchCueWidget::onFadeTick()
{
    m_elapsedMs += c_tickMs;
    if (m_elapsedMs <= c_holdMs) {
        return;
    }
    const int fadeElapsed = m_elapsedMs - c_holdMs;
    if (fadeElapsed >= c_fadeMs) {
        m_fadeTimer.stop();
        m_opacity = 0.0;
        hide();
        return;
    }
    m_opacity = 1.0 - (static_cast<qreal>(fadeElapsed) / c_fadeMs);
    update();
}

CheerWidget::CheerWidget(QWidget *parent)
        : QWidget(parent)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);

    // Named families rather than whatever the widget inherited: the glyphs are
    // emoji, and the UI font on none of the three platforms carries them. Qt walks
    // this list and uses the first one present, so listing all three keeps a single
    // build working everywhere.
    m_glyphFont.setFamilies({"Segoe UI Emoji", "Apple Color Emoji", "Noto Color Emoji"});
    m_counterFont = font();
    m_counterFont.setBold(true);

    m_animTimer.setInterval(c_tickMs);
    connect(&m_animTimer, &QTimer::timeout, this, &CheerWidget::onTick);
}

// Built from code points for the same reason PitchCueWidget's arrows are: it keeps
// the glyphs independent of how a compiler decides to read this file's encoding.
QString CheerWidget::glyphFor(const QString &reaction)
{
    char32_t codePoint = 0x1F44F; // CLAPPING HANDS SIGN, and the fallback
    if (reaction == QLatin1String("fire"))
        codePoint = 0x1F525;
    else if (reaction == QLatin1String("heart"))
        codePoint = 0x1F496; // SPARKLING HEART - one code point, no variation selector
    else if (reaction == QLatin1String("star"))
        codePoint = 0x2B50;
    else if (reaction == QLatin1String("party"))
        codePoint = 0x1F389;
    return QString::fromUcs4(&codePoint, 1);
}

void CheerWidget::addCheers(const QString &reaction, const int count, const int songTotal)
{
    m_total = songTotal;

    const QString glyph = glyphFor(reaction);
    auto *rng = QRandomGenerator::global();
    const int spawn = std::min({count, c_maxPerBatch, c_maxParticles - static_cast<int>(m_particles.size())});
    for (int i = 0; i < spawn; ++i) {
        Particle particle;
        particle.glyph = glyph;
        // Kept off the extreme edges so a wide glyph never half-hangs off the screen.
        particle.x = 0.08 + rng->generateDouble() * 0.84;
        particle.speed = 0.010 + rng->generateDouble() * 0.010;
        particle.drift = 0.02 + rng->generateDouble() * 0.05;
        particle.phase = rng->generateDouble() * 6.283;
        particle.scale = 0.75 + rng->generateDouble() * 0.5;
        // Spread the batch down past the bottom edge so a burst rises as a stream
        // rather than as one rank of emoji moving in lockstep.
        particle.progress = -(rng->generateDouble() * 0.25);
        m_particles.append(particle);
    }

    show();
    raise();
    if (!m_animTimer.isActive())
        m_animTimer.start();
    update();
}

void CheerWidget::onTick()
{
    for (auto it = m_particles.begin(); it != m_particles.end();) {
        it->progress += it->speed;
        it = (it->progress >= 1.0) ? m_particles.erase(it) : std::next(it);
    }

    if (m_particles.isEmpty()) {
        m_animTimer.stop();
        hide();
        return;
    }
    update();
}

void CheerWidget::paintEvent(QPaintEvent *)
{
    if (m_particles.isEmpty())
        return;

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);

    const int baseSize = std::max(16, height() / 14);
    for (const Particle &particle : m_particles) {
        if (particle.progress < 0.0)
            continue;

        // Fade in off the bottom edge and back out at the top, so nothing pops into
        // or out of existence mid-screen.
        qreal opacity = 1.0;
        if (particle.progress < 0.12)
            opacity = particle.progress / 0.12;
        else if (particle.progress > 0.75)
            opacity = 1.0 - ((particle.progress - 0.75) / 0.25);
        painter.setOpacity(std::clamp(opacity, 0.0, 1.0));

        QFont glyphFont = m_glyphFont;
        glyphFont.setPixelSize(std::max(8, static_cast<int>(baseSize * particle.scale)));
        painter.setFont(glyphFont);

        const qreal sway = particle.drift * std::sin(particle.phase + particle.progress * 9.0);
        const int x = static_cast<int>((particle.x + sway) * width());
        // Travels from just below the bottom edge to just above the top.
        const int y = static_cast<int>((1.10 - particle.progress * 1.20) * height());

        const int box = glyphFont.pixelSize() * 2;
        painter.drawText(QRect(x - box / 2, y - box / 2, box, box), Qt::AlignCenter, particle.glyph);
    }

    if (m_total <= 0)
        return;

    // The tally, low and left - the duration timer sits top-left by default and the
    // key cue takes the top-right corner.
    painter.setOpacity(0.85);
    QFont counterFont = m_counterFont;
    counterFont.setPixelSize(std::max(14, height() / 22));
    painter.setFont(counterFont);

    // Glyph and number are drawn in separate passes with their own fonts. Setting one
    // font for the whole string would leave either the digits to an emoji font or the
    // emoji to Qt's fallback, and this way neither is left to chance.
    QFont badgeGlyphFont = m_glyphFont;
    badgeGlyphFont.setPixelSize(counterFont.pixelSize());

    const QString glyph = glyphFor("clap");
    const QString count = QString::number(m_total);
    const QFontMetrics glyphMetrics(badgeGlyphFont);
    const QFontMetrics countMetrics(counterFont);
    const int padding = countMetrics.height() / 2;
    const int glyphWidth = glyphMetrics.horizontalAdvance(glyph);
    const int countWidth = countMetrics.horizontalAdvance(count);

    const int margin = std::max(12, width() / 40);
    QRect box(0, 0, glyphWidth + countWidth + padding * 3, countMetrics.height() * 3 / 2);
    box.moveBottomLeft(QPoint(margin, height() - margin));

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 0xb0));
    painter.drawRoundedRect(box, box.height() / 4.0, box.height() / 4.0);

    painter.setPen(QColor(0xff, 0xff, 0xff));
    painter.setFont(badgeGlyphFont);
    painter.drawText(QRect(box.left() + padding, box.top(), glyphWidth, box.height()),
                     Qt::AlignCenter, glyph);
    painter.setFont(counterFont);
    painter.drawText(QRect(box.left() + padding * 2 + glyphWidth, box.top(), countWidth, box.height()),
                     Qt::AlignCenter, count);
}

void PitchCueWidget::paintEvent(QPaintEvent *)
{
    if (m_text.isEmpty())
        return;

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setOpacity(m_opacity);

    const qreal radius = height() / 4.0;
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 0xb0));
    painter.drawRoundedRect(rect(), radius, radius);

    painter.setFont(m_font);
    painter.setPen(QColor(0xff, 0xff, 0xff));
    painter.drawText(rect(), Qt::AlignCenter, m_text);
}
