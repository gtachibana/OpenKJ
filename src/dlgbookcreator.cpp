#include "dlgbookcreator.h"
#include "ui_dlgbookcreator.h"
#include <QFileDialog>
#include <QMessageBox>
#include <QPainter>
#include <QPdfWriter>
#include <QProgressDialog>
#include <QSqlQuery>
#include <QStandardPaths>


DlgBookCreator::DlgBookCreator(QWidget *parent) :
        QDialog(parent),
        ui(new Ui::DlgBookCreator) {
    m_logger = spdlog::get("logger");
    ui->setupUi(this);
    loadSettings();
    setupConnections();
}

void DlgBookCreator::loadSettings() {
    m_settings.restoreWindowState(this);
    ui->cbxColumns->addItem("2", 2);
    ui->cbxColumns->addItem("3", 3);
    ui->cbxPageSize->addItem(tr("Letter"), QPageSize::Letter);
    ui->cbxPageSize->addItem(tr("Legal"), QPageSize::Legal);
    ui->cbxPageSize->addItem(tr("A4"), QPageSize::A4);
    ui->fontCbxArtist->setCurrentFont(m_settings.bookCreatorArtistFont());
    ui->fontCbxTitle->setCurrentFont(m_settings.bookCreatorTitleFont());
    ui->fontCbxHeader->setCurrentFont(m_settings.bookCreatorHeaderFont());
    ui->fontCbxFooter->setCurrentFont(m_settings.bookCreatorFooterFont());
    ui->spinBoxSizeArtist->setValue(m_settings.bookCreatorArtistFont().pointSize());
    ui->spinBoxSizeTitle->setValue(m_settings.bookCreatorTitleFont().pointSize());
    ui->spinBoxSizeHeader->setValue(m_settings.bookCreatorHeaderFont().pointSize());
    ui->spinBoxSizeFooter->setValue(m_settings.bookCreatorFooterFont().pointSize());
    ui->checkBoxBoldArtist->setChecked(m_settings.bookCreatorArtistFont().bold());
    ui->checkBoxBoldTitle->setChecked(m_settings.bookCreatorTitleFont().bold());
    ui->checkBoxBoldHeader->setChecked(m_settings.bookCreatorHeaderFont().bold());
    ui->checkBoxBoldFooter->setChecked(m_settings.bookCreatorFooterFont().bold());
    ui->doubleSpinBoxLeft->setValue(m_settings.bookCreatorMarginLft());
    ui->doubleSpinBoxRight->setValue(m_settings.bookCreatorMarginRt());
    ui->doubleSpinBoxTop->setValue(m_settings.bookCreatorMarginTop());
    ui->doubleSpinBoxBottom->setValue(m_settings.bookCreatorMarginBtm());
    ui->cbxColumns->setCurrentIndex(m_settings.bookCreatorCols());
    ui->cbxPageSize->setCurrentIndex(m_settings.bookCreatorPageSize());
    ui->lineEditHeaderText->setText(m_settings.bookCreatorHeaderText());
    ui->lineEditFooterText->setText(m_settings.bookCreatorFooterText());
    ui->cbxPageNumbering->setChecked(m_settings.bookCreatorPageNumbering());
}

void DlgBookCreator::setupConnections() const {
    connect(ui->doubleSpinBoxLeft, qOverload<double>(&QDoubleSpinBox::valueChanged), &m_settings,
            &Settings::setBookCreatorMarginLft);
    connect(ui->doubleSpinBoxRight, qOverload<double>(&QDoubleSpinBox::valueChanged), &m_settings,
            &Settings::setBookCreatorMarginRt);
    connect(ui->doubleSpinBoxTop, qOverload<double>(&QDoubleSpinBox::valueChanged), &m_settings,
            &Settings::setBookCreatorMarginTop);
    connect(ui->doubleSpinBoxBottom, qOverload<double>(&QDoubleSpinBox::valueChanged), &m_settings,
            &Settings::setBookCreatorMarginBtm);
    connect(ui->fontCbxFooter, &QFontComboBox::currentFontChanged, this, &DlgBookCreator::saveFontSettings);
    connect(ui->spinBoxSizeFooter, qOverload<int>(&QSpinBox::valueChanged), this, &DlgBookCreator::saveFontSettings);
    connect(ui->checkBoxBoldFooter, qOverload<bool>(&QCheckBox::clicked), this, &DlgBookCreator::saveFontSettings);
    connect(ui->lineEditFooterText, &QLineEdit::textChanged, &m_settings, &Settings::setBookCreatorFooterText);
    connect(ui->fontCbxHeader, &QFontComboBox::currentFontChanged, this, &DlgBookCreator::saveFontSettings);
    connect(ui->spinBoxSizeHeader, qOverload<int>(&QSpinBox::valueChanged), this, &DlgBookCreator::saveFontSettings);
    connect(ui->checkBoxBoldHeader, qOverload<bool>(&QCheckBox::clicked), this, &DlgBookCreator::saveFontSettings);
    connect(ui->lineEditHeaderText, &QLineEdit::textChanged, &m_settings, &Settings::setBookCreatorHeaderText);
    connect(ui->fontCbxArtist, &QFontComboBox::currentFontChanged, this, &DlgBookCreator::saveFontSettings);
    connect(ui->spinBoxSizeArtist, qOverload<int>(&QSpinBox::valueChanged), this, &DlgBookCreator::saveFontSettings);
    connect(ui->checkBoxBoldArtist, qOverload<bool>(&QCheckBox::clicked), this, &DlgBookCreator::saveFontSettings);
    connect(ui->fontCbxTitle, &QFontComboBox::currentFontChanged, this, &DlgBookCreator::saveFontSettings);
    connect(ui->spinBoxSizeTitle, qOverload<int>(&QSpinBox::valueChanged), this, &DlgBookCreator::saveFontSettings);
    connect(ui->checkBoxBoldTitle, qOverload<bool>(&QCheckBox::clicked), this, &DlgBookCreator::saveFontSettings);
    connect(ui->doubleSpinBoxBottom, qOverload<double>(&QDoubleSpinBox::valueChanged), &m_settings,
            &Settings::setBookCreatorMarginBtm);
    connect(ui->doubleSpinBoxLeft, qOverload<double>(&QDoubleSpinBox::valueChanged), &m_settings,
            &Settings::setBookCreatorMarginLft);
    connect(ui->doubleSpinBoxRight, qOverload<double>(&QDoubleSpinBox::valueChanged), &m_settings,
            &Settings::setBookCreatorMarginRt);
    connect(ui->doubleSpinBoxTop, qOverload<double>(&QDoubleSpinBox::valueChanged), &m_settings,
            &Settings::setBookCreatorMarginTop);
    connect(ui->cbxPageNumbering, qOverload<bool>(&QCheckBox::clicked), &m_settings,
            &Settings::setBookCreatorPageNumbering);
    connect(ui->buttonBox, &QDialogButtonBox::clicked, this, &DlgBookCreator::close);
    connect(ui->btnGenerate, &QPushButton::clicked, this, &DlgBookCreator::btnGenerateClicked);
    connect(ui->cbxColumns, qOverload<int>(&QComboBox::currentIndexChanged), &m_settings,
            &Settings::setBookCreatorCols);
    connect(ui->cbxPageSize, qOverload<int>(&QComboBox::currentIndexChanged), &m_settings,
            &Settings::setBookCreatorPageSize);
}

DlgBookCreator::~DlgBookCreator() = default;

void DlgBookCreator::saveFontSettings() {
    QFont tFont(ui->fontCbxTitle->currentFont());
    tFont.setPointSize(ui->spinBoxSizeTitle->value());
    tFont.setBold(ui->checkBoxBoldTitle->isChecked());
    m_settings.setBookCreatorTitleFont(tFont);

    QFont aFont(ui->fontCbxArtist->currentFont());
    aFont.setPointSize(ui->spinBoxSizeArtist->value());
    aFont.setBold(ui->checkBoxBoldArtist->isChecked());
    m_settings.setBookCreatorArtistFont(aFont);

    QFont hFont(ui->fontCbxHeader->currentFont());
    hFont.setPointSize(ui->spinBoxSizeHeader->value());
    hFont.setBold(ui->checkBoxBoldHeader->isChecked());
    m_settings.setBookCreatorHeaderFont(hFont);

    QFont fFont(ui->fontCbxFooter->currentFont());
    fFont.setPointSize(ui->spinBoxSizeFooter->value());
    fFont.setBold(ui->checkBoxBoldFooter->isChecked());
    m_settings.setBookCreatorFooterFont(fFont);
}

// One pass over the library rather than one query per artist. Ordered the same way the
// per-artist queries were, so the book comes out identically; the artist rows are the
// changes of artist in the result rather than a separate query each. On a 100k-song
// library the old shape was around ten thousand round trips to sqlite.
QStringList DlgBookCreator::getEntries() {
    QSqlQuery query;
    QStringList entries;
    QString sql = "SELECT DISTINCT artist, title FROM dbsongs "
                  "WHERE discid != '!!DROPPED!!' AND discid != '!!YOUTUBE!!' AND bad = 0 "
                  "ORDER BY artist, title";
    query.exec(sql);
    QString lastArtist;
    bool haveArtist{false};
    while (query.next()) {
        const QString artist = query.value(0).toString();
        if (!haveArtist || artist != lastArtist) {
            entries.append("-" + artist);
            lastArtist = artist;
            haveArtist = true;
        }
        entries.append("+" + query.value(1).toString());
    }
    return entries;
}

void DlgBookCreator::writePdf(const QString &filename, int nCols) {
    QProgressDialog progress(this);
    progress.setWindowModality(Qt::WindowModal);
    // Cancellable. Generating a book from a large library takes minutes, and with the
    // cancel button explicitly removed the only way out was killing the app.
    progress.setLabelText("Gathering artist data");
    progress.setValue(0);
    progress.setMaximum(0);
    progress.show();
    m_logger->info("{} Beginning pdf book generation",m_loggingPrefix);
    QFont aFont = m_settings.bookCreatorArtistFont();
    QFont tFont = m_settings.bookCreatorTitleFont();
    QFont hFont = m_settings.bookCreatorHeaderFont();
    QFont fFont = m_settings.bookCreatorFooterFont();
    QPdfWriter pdf(filename);
    pdf.setPageSize(QPageSize(static_cast<QPageSize::PageSizeId>(ui->cbxPageSize->currentData().toInt())));
    pdf.setPageMargins(
            QMarginsF(ui->doubleSpinBoxLeft->value(), ui->doubleSpinBoxTop->value(), ui->doubleSpinBoxRight->value(),
                      ui->doubleSpinBoxBottom->value()), QPageLayout::Inch);
    QPainter painter(&pdf);
    m_logger->info("{} Getting song data",m_loggingPrefix);
    progress.setLabelText("Parsing song data");
    QApplication::processEvents();
    QStringList entries = getEntries();
    m_logger->info("{} Got {} entries",m_loggingPrefix, entries.size());
    QPen pen;
    pen.setColor(QColor(0, 0, 0));
    pen.setWidth(4);
    painter.setPen(pen);
    painter.setFont(tFont);
    int lineOffset;
    int lineOffset2 = 0;
    QRect txtRect;
    int fontHeight = painter.fontMetrics().height();
    QString lastArtist;
    lineOffset = painter.viewport().width() / 2;
    if (nCols == 3) {
        lineOffset = painter.viewport().width() / 3;
        lineOffset2 = lineOffset + lineOffset;
    }
    int curDrawPos;
    int pages = 0;
    m_logger->info("{} Writing data to pdf",m_loggingPrefix);
    progress.setLabelText("Writing data to PDF");
    progress.setValue(0);
    const int totalEntries = static_cast<int>(entries.size());
    progress.setMaximum(totalEntries);
    bool cancelled{false};
    m_logger->info("{} Generating pages",m_loggingPrefix);
    while (!entries.isEmpty()) {
        QApplication::processEvents();
        if (progress.wasCanceled()) {
            cancelled = true;
            break;
        }
        // Every column below can legitimately consume nothing - the "last line and it's
        // an artist, push it to the next column" branch advances the draw position
        // without taking the entry. If a page/font combination leaves every column doing
        // that, the entry list never shrinks and the loop writes pages forever.
        const int entriesAtPageStart = static_cast<int>(entries.size());
        pages++;
        m_logger->debug("{} Generating page {}",m_loggingPrefix, pages);
        int topOffset = 40;
        int headerOffset = 0;
        int bottomOffset = 0;
        if (ui->lineEditHeaderText->text() != "") {
            painter.setFont(hFont);
            painter.drawText(0, 0, painter.viewport().width(), painter.fontMetrics().height(), Qt::AlignCenter,
                             ui->lineEditHeaderText->text());
            headerOffset = painter.fontMetrics().height() + 50;
        }
        if (ui->lineEditFooterText->text() != "" || m_settings.bookCreatorPageNumbering()) {
            bottomOffset = 45;
            painter.setFont(fFont);
            int fFontHeight = painter.fontMetrics().height();
            if (m_settings.bookCreatorFooterText() != "")
                painter.drawText(0, painter.viewport().height() - fFontHeight, painter.viewport().width(),
                                 painter.fontMetrics().height(), Qt::AlignCenter, m_settings.bookCreatorFooterText());
            if (m_settings.bookCreatorPageNumbering()) {
                QString pageStr = tr("Page ") + QString::number(pages);
                txtRect = painter.fontMetrics().boundingRect(pageStr);
                painter.drawText(painter.viewport().width() - txtRect.width() - 20,
                                 painter.viewport().height() - txtRect.height(), txtRect.width(), txtRect.height(),
                                 Qt::AlignRight, pageStr);
            }
            bottomOffset = fFontHeight + bottomOffset;
        }
        painter.drawLine(0, headerOffset, 0, painter.viewport().height() - bottomOffset);
        painter.drawLine(painter.viewport().width(), headerOffset, painter.viewport().width(),
                         painter.viewport().height() - bottomOffset);
        painter.drawLine(0, headerOffset, painter.viewport().width(), headerOffset);
        painter.drawLine(0, painter.viewport().height() - bottomOffset, painter.viewport().width(),
                         painter.viewport().height() - bottomOffset);
        if (nCols == 2)
            painter.drawLine(lineOffset, headerOffset, lineOffset, painter.viewport().height() - bottomOffset);
        if (nCols == 3) {
            painter.drawLine(lineOffset, headerOffset, lineOffset, painter.viewport().height() - bottomOffset);
            painter.drawLine(lineOffset2, headerOffset, lineOffset2, painter.viewport().height() - bottomOffset);
        }
        curDrawPos = topOffset + headerOffset;
        while ((curDrawPos + fontHeight) <= (painter.viewport().height() - bottomOffset)) {
            QApplication::processEvents();
            if (entries.isEmpty())
                break;
            QString entry;
            if ((curDrawPos == (topOffset + headerOffset)) && (entries.at(0).at(0) == QString("+"))) {
                // We're at the top and it's not an artist entry, re-display artist
                entry = "-" + lastArtist + tr(" (cont'd)");
            } else if ((curDrawPos + (2 * fontHeight) >= (painter.viewport().height() - bottomOffset)) &&
                       (entries.at(0).at(0) == QString("-"))) {
                // We're on the last line and it's an artist, skip it to the next col/page
                curDrawPos = curDrawPos + fontHeight;
                continue;
            } else
                entry = entries.takeFirst();
            if (entry.at(0) == QString("-")) {
                painter.setFont(aFont);
                txtRect = painter.fontMetrics().boundingRect(entry);
                entry.remove(0, 1);
                painter.drawText(200, curDrawPos, txtRect.width(), txtRect.height(), Qt::AlignLeft, entry);
                lastArtist = entry;
            } else if (entry.at(0) == QString("+")) {
                painter.setFont(tFont);
                txtRect = painter.fontMetrics().boundingRect(entry);
                entry.remove(0, 1);
                painter.drawText(400, curDrawPos, txtRect.width(), txtRect.height(), Qt::AlignLeft, entry);

            }
            curDrawPos = curDrawPos + fontHeight;
        }
        curDrawPos = topOffset + headerOffset;
        while ((curDrawPos + fontHeight) <= (painter.viewport().height() - bottomOffset)) {
            QApplication::processEvents();
            if (entries.isEmpty())
                break;
            QString entry;
            if ((curDrawPos == (topOffset + headerOffset)) && (entries.at(0).at(0) == QString("+"))) {
                // We're at the top and it's not an artist entry, re-display artist
                entry = "-" + lastArtist + tr(" (cont'd)");
            } else if ((curDrawPos + (2 * fontHeight) >= (painter.viewport().height() - bottomOffset)) &&
                       (entries.at(0).at(0) == QString("-"))) {
                // We're on the last line and it's an artist, skip it to the next col/page
                curDrawPos = curDrawPos + fontHeight;
                continue;
            } else
                entry = entries.takeFirst();
            if (entry.at(0) == QString("-")) {
                painter.setFont(aFont);
                txtRect = painter.fontMetrics().boundingRect(entry);
                entry.remove(0, 1);
                painter.drawText(lineOffset + 200, curDrawPos, txtRect.width(), txtRect.height(), Qt::AlignLeft, entry);
                lastArtist = entry;
            } else if (entry.at(0) == QString("+")) {
                painter.setFont(tFont);
                txtRect = painter.fontMetrics().boundingRect(entry);
                entry.remove(0, 1);
                painter.drawText(lineOffset + 400, curDrawPos, txtRect.width(), txtRect.height(), Qt::AlignLeft, entry);
            }
            curDrawPos = curDrawPos + fontHeight;
        }
        if (nCols == 3) {
            curDrawPos = topOffset + headerOffset;
            while ((curDrawPos + fontHeight) <= (painter.viewport().height() - bottomOffset)) {
                QApplication::processEvents();
                if (entries.isEmpty())
                    break;
                QString entry;
                if ((curDrawPos == (topOffset + headerOffset)) && (entries.at(0).at(0) == QString("+"))) {
                    // We're at the top and it's not an artist entry, re-display artist
                    entry = "-" + lastArtist + tr(" (cont'd)");
                } else if ((curDrawPos + (2 * fontHeight) >= (painter.viewport().height() - bottomOffset)) &&
                           (entries.at(0).at(0) == QString("-"))) {
                    // We're on the last line and it's an artist, skip it to the next col/page
                    curDrawPos = curDrawPos + fontHeight;
                    continue;
                } else
                    entry = entries.takeFirst();
                if (entry.at(0) == QString("-")) {
                    painter.setFont(aFont);
                    txtRect = painter.fontMetrics().boundingRect(entry);
                    entry.remove(0, 1);
                    painter.drawText(lineOffset2 + 200, curDrawPos, txtRect.width(), txtRect.height(), Qt::AlignLeft,
                                     entry);
                    lastArtist = entry;
                } else if (entry.at(0) == QString("+")) {
                    painter.setFont(tFont);
                    txtRect = painter.fontMetrics().boundingRect(entry);
                    entry.remove(0, 1);
                    painter.drawText(lineOffset2 + 400, curDrawPos, txtRect.width(), txtRect.height(), Qt::AlignLeft,
                                     entry);
                }
                curDrawPos = curDrawPos + fontHeight;
            }
        }
        if (static_cast<int>(entries.size()) == entriesAtPageStart) {
            m_logger->error("{} Page {} consumed no entries - stopping rather than writing pages forever. "
                            "The page size and font size are probably leaving no usable room in a column.",
                            m_loggingPrefix, pages);
            break;
        }
        if (!entries.isEmpty()) {
            pdf.newPage();
            pdf.setPageMargins(QMarginsF(ui->doubleSpinBoxLeft->value(), ui->doubleSpinBoxTop->value(),
                                         ui->doubleSpinBoxRight->value(), ui->doubleSpinBoxBottom->value()),
                               QPageLayout::Inch);
        }
        // Entries actually written, not pages. The maximum here is the entry count -
        // 100k+ on a library this size - while this advanced once per page, so the bar
        // never visibly left zero.
        progress.setValue(totalEntries - static_cast<int>(entries.size()));
    }
    m_logger->info("{} Done writing data to pdf",m_loggingPrefix);
    m_logger->info("{} Finalizing pdf",m_loggingPrefix);
    progress.setLabelText("Finalizing PDF");
    progress.setMaximum(0);
    progress.setValue(0);
    // Finalized either way: a cancelled run has already written pages, and leaving the
    // painter open would abandon the file half written.
    painter.end();
    progress.close();
    QMessageBox msgBox(this);
    if (cancelled) {
        msgBox.setText(tr("Songbook generation cancelled. The PDF holds the %1 pages written so far.").arg(pages));
        m_logger->info("{} Songbook generation cancelled after {} pages",m_loggingPrefix, pages);
    } else {
        msgBox.setText("Songbook PDF generation complete");
        m_logger->info("{} Songbook generation complete",m_loggingPrefix);
    }
    msgBox.exec();
}


void DlgBookCreator::btnGenerateClicked() {
    QString defFn = "Songbook.pdf";
#ifdef Q_OS_LINUX
    QString defaultFilePath =
            QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QDir::separator() + defFn;
    QString saveFilePath = QFileDialog::getSaveFileName(this, "Select songbook filename", defaultFilePath,
                                                        "PDF Files (*.pdf)", nullptr, QFileDialog::DontUseNativeDialog);
#else
    QString defaultFilePath =
            QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QDir::separator() + defFn;
    QString saveFilePath = QFileDialog::getSaveFileName(this, "Select songbook filename", defaultFilePath,
                                                        "PDF Files (*.pdf)", nullptr);
#endif
    if (saveFilePath != "") {
        QApplication::processEvents();
        writePdf(saveFilePath, ui->cbxColumns->currentData().toInt());
    }
}

void DlgBookCreator::resizeEvent(QResizeEvent *event) {
    m_settings.saveWindowState(this);
    QDialog::resizeEvent(event);
}

