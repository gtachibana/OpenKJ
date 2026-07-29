#include "dlgnamecleanup.h"
#include "ui_dlgnamecleanup.h"

#include <QApplication>
#include <QHeaderView>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidgetItem>

namespace {

    enum FilterMode {
        FILTER_ALL = 0,
        FILTER_ARTISTS,
        FILTER_TITLES,
        FILTER_HIGH_CONFIDENCE
    };

    QString fieldText(LibraryNameCleaner::Field field) {
        return (field == LibraryNameCleaner::Field::Title) ? QObject::tr("Title") : QObject::tr("Artist");
    }

    QString confidenceText(LibraryNameCleaner::Confidence confidence) {
        return (confidence == LibraryNameCleaner::Confidence::High) ? QObject::tr("High") : QObject::tr("Medium");
    }

}

DlgNameCleanup::DlgNameCleanup(QWidget *parent) :
        QDialog(parent),
        ui(new Ui::DlgNameCleanup) {
    ui->setupUi(this);
    ui->tableWidget->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    ui->tableWidget->horizontalHeader()->setStretchLastSection(true);
    ui->tableWidget->verticalHeader()->setVisible(false);
    ui->tableWidget->setSortingEnabled(true);
    connect(ui->tableWidget, &QTableWidget::itemChanged, this, &DlgNameCleanup::onItemChanged);
    connect(&m_cleaner, &LibraryNameCleaner::stateChanged, this, [this](const QString &state) {
        ui->lblSummary->setText(state);
    });
    analyze();
}

DlgNameCleanup::~DlgNameCleanup() {
    delete ui;
}

void DlgNameCleanup::analyze() {
    QApplication::setOverrideCursor(Qt::WaitCursor);
    m_plan = m_cleaner.plan();
    QApplication::restoreOverrideCursor();

    if (!m_cleaner.errors().isEmpty()) {
        QMessageBox::warning(this, tr("Could not scan the library"), m_cleaner.errors().join("\n"));
    }

    populateTable();
    applyFilter();
    updateSummary();
}

void DlgNameCleanup::populateTable() {
    m_populating = true;
    ui->tableWidget->setSortingEnabled(false);
    ui->tableWidget->clearContents();
    ui->tableWidget->setRowCount((int) m_plan.proposals.size());

    for (int row = 0; row < (int) m_plan.proposals.size(); row++) {
        const auto &proposal = m_plan.proposals[row];

        auto *fieldItem = new QTableWidgetItem(fieldText(proposal.field));
        fieldItem->setFlags(fieldItem->flags() | Qt::ItemIsUserCheckable);
        // Only the safe bets start ticked. Everything else is a judgement call
        // and should be looked at before it is applied.
        fieldItem->setCheckState(
                (proposal.confidence == LibraryNameCleaner::Confidence::High) ? Qt::Checked : Qt::Unchecked);
        // The table gets sorted, so rows carry their index rather than relying
        // on position.
        fieldItem->setData(Qt::UserRole, row);
        ui->tableWidget->setItem(row, COL_FIELD, fieldItem);

        ui->tableWidget->setItem(row, COL_ARTIST, new QTableWidgetItem(proposal.scopeArtist));
        ui->tableWidget->setItem(row, COL_CURRENT, new QTableWidgetItem(proposal.oldValue));
        ui->tableWidget->setItem(row, COL_PROPOSED, new QTableWidgetItem(proposal.newValue));

        auto *songsItem = new QTableWidgetItem;
        songsItem->setData(Qt::DisplayRole, proposal.songs);
        songsItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        songsItem->setToolTip(tr("%1 song(s) use this spelling, %2 already use the proposed one.")
                                      .arg(proposal.songs)
                                      .arg(proposal.keptSongs));
        ui->tableWidget->setItem(row, COL_SONGS, songsItem);

        ui->tableWidget->setItem(row, COL_CONFIDENCE, new QTableWidgetItem(confidenceText(proposal.confidence)));
        ui->tableWidget->setItem(row, COL_REASON,
                                 new QTableWidgetItem(LibraryNameCleaner::reasonText(proposal.reason)));
    }

    ui->tableWidget->setSortingEnabled(true);
    ui->tableWidget->resizeColumnsToContents();
    m_populating = false;
}

void DlgNameCleanup::applyFilter() {
    const int mode = ui->cbxFilter->currentIndex();
    for (int row = 0; row < ui->tableWidget->rowCount(); row++) {
        const auto *item = ui->tableWidget->item(row, COL_FIELD);
        if (item == nullptr)
            continue;
        const auto &proposal = m_plan.proposals[item->data(Qt::UserRole).toInt()];

        bool visible{true};
        switch (mode) {
            case FILTER_ARTISTS:
                visible = (proposal.field == LibraryNameCleaner::Field::Artist);
                break;
            case FILTER_TITLES:
                visible = (proposal.field == LibraryNameCleaner::Field::Title);
                break;
            case FILTER_HIGH_CONFIDENCE:
                visible = (proposal.confidence == LibraryNameCleaner::Confidence::High);
                break;
            case FILTER_ALL:
            default:
                break;
        }
        ui->tableWidget->setRowHidden(row, !visible);
    }
}

std::vector<LibraryNameCleaner::Proposal> DlgNameCleanup::checkedProposals() const {
    std::vector<LibraryNameCleaner::Proposal> checked;
    for (int row = 0; row < ui->tableWidget->rowCount(); row++) {
        const auto *item = ui->tableWidget->item(row, COL_FIELD);
        if (item == nullptr || item->checkState() != Qt::Checked)
            continue;
        checked.push_back(m_plan.proposals[item->data(Qt::UserRole).toInt()]);
    }
    return checked;
}

void DlgNameCleanup::updateSummary() {
    if (m_plan.isEmpty()) {
        ui->lblSummary->setText(tr("Nothing to fix. Scanned %1 songs by %2 artists and found no spelling variants.")
                                        .arg(m_plan.scannedSongs)
                                        .arg(m_plan.distinctArtists));
        ui->btnApply->setEnabled(false);
        return;
    }

    const auto checked = checkedProposals();
    int songs{0};
    for (const auto &proposal : checked)
        songs += proposal.songs;

    // The count is of everything ticked, filtered out or not, so a hidden row
    // can never be applied unnoticed. Song entries rather than songs, since one
    // song can be counted by both an artist fix and a title fix.
    ui->lblSummary->setText(tr("%1 possible fixes across %2 songs by %3 artists "
                               "(%4 artist, %5 title, %6 high confidence). "
                               "%7 ticked, covering %8 song entries.")
                                    .arg(m_plan.proposals.size())
                                    .arg(m_plan.scannedSongs)
                                    .arg(m_plan.distinctArtists)
                                    .arg(m_plan.countOf(LibraryNameCleaner::Field::Artist))
                                    .arg(m_plan.countOf(LibraryNameCleaner::Field::Title))
                                    .arg(m_plan.countOf(LibraryNameCleaner::Confidence::High))
                                    .arg(checked.size())
                                    .arg(songs));
    ui->btnApply->setEnabled(!checked.empty());
}

void DlgNameCleanup::onItemChanged() {
    if (m_populating)
        return;
    updateSummary();
}

void DlgNameCleanup::on_cbxFilter_currentIndexChanged(int index) {
    Q_UNUSED(index)
    applyFilter();
}

void DlgNameCleanup::on_btnCheckVisible_clicked() {
    m_populating = true;
    for (int row = 0; row < ui->tableWidget->rowCount(); row++) {
        if (!ui->tableWidget->isRowHidden(row))
            ui->tableWidget->item(row, COL_FIELD)->setCheckState(Qt::Checked);
    }
    m_populating = false;
    updateSummary();
}

void DlgNameCleanup::on_btnUncheckVisible_clicked() {
    m_populating = true;
    for (int row = 0; row < ui->tableWidget->rowCount(); row++) {
        if (!ui->tableWidget->isRowHidden(row))
            ui->tableWidget->item(row, COL_FIELD)->setCheckState(Qt::Unchecked);
    }
    m_populating = false;
    updateSummary();
}

void DlgNameCleanup::on_btnApply_clicked() {
    const auto checked = checkedProposals();
    if (checked.empty()) {
        QMessageBox::information(this, tr("Nothing selected"), tr("Tick the changes you want applied first."));
        return;
    }

    int songs{0};
    for (const auto &proposal : checked)
        songs += proposal.songs;

    QMessageBox confirm(this);
    confirm.setText(tr("Apply %1 name changes?").arg(checked.size()));
    confirm.setInformativeText(tr("%1 song entries will be rewritten in the database.\n\n"
                                  "Files on disk are not touched and nothing is deleted. Every change is "
                                  "written to a CSV in the OpenKJ data folder so it can be traced or undone "
                                  "by hand.\n\n"
                                  "Note that clearing and rebuilding the database rebuilds these names from "
                                  "the filenames again, discarding the corrections.")
                                       .arg(songs));
    confirm.setIcon(QMessageBox::Question);
    confirm.addButton(QMessageBox::Cancel);
    QPushButton *goButton = confirm.addButton(tr("Apply"), QMessageBox::AcceptRole);
    confirm.exec();
    if (confirm.clickedButton() != goButton)
        return;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    const int changed = m_cleaner.execute(checked);
    QApplication::restoreOverrideCursor();

    if (!m_cleaner.errors().isEmpty()) {
        QMessageBox::warning(this, tr("Nothing was changed"),
                             tr("The changes were rolled back and the library is as it was.\n\n%1")
                                     .arg(m_cleaner.errors().join("\n")));
        return;
    }

    m_appliedChanges = true;
    QMessageBox done(this);
    done.setIcon(QMessageBox::Information);
    done.setText(tr("Updated %1 songs.").arg(changed));
    if (!m_cleaner.journalPath().isEmpty())
        done.setInformativeText(tr("A record of the changes was written to:\n%1").arg(m_cleaner.journalPath()));
    done.exec();

    // Re-run so anything the applied changes newly exposed shows up, and so the
    // list no longer offers what has already been done.
    analyze();
}

void DlgNameCleanup::on_btnClose_clicked() {
    accept();
}
