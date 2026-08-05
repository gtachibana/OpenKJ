#include "dlgnamecleanup.h"
#include "ui_dlgnamecleanup.h"

#include <algorithm>

#include <QApplication>
#include <QFileDialog>
#include <QFont>
#include <QHeaderView>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollBar>
#include <QTableWidgetItem>

#include "settings.h"

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
    Settings settings;
    ui->lineEditReviewDir->setText(settings.libraryReviewDir());
    connect(&m_cleaner, &LibraryNameCleaner::stateChanged, this, [this](const QString &state) {
        ui->lblSummary->setText(state);
    });
    analyze();
}

DlgNameCleanup::~DlgNameCleanup() {
    delete ui;
}

void DlgNameCleanup::analyze() {
    // A fresh scan groups the library from scratch, so overrides keyed on the
    // old grouping mean nothing against it.
    m_retargets.clear();
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
        fieldItem->setFlags((fieldItem->flags() | Qt::ItemIsUserCheckable) & ~Qt::ItemIsEditable);
        // Only the safe bets start ticked. Everything else is a judgement call
        // and should be looked at before it is applied.
        fieldItem->setCheckState(
                (proposal.confidence == LibraryNameCleaner::Confidence::High) ? Qt::Checked : Qt::Unchecked);
        // The table gets sorted, so rows carry their index rather than relying
        // on position.
        fieldItem->setData(Qt::UserRole, row);
        ui->tableWidget->setItem(row, COL_FIELD, fieldItem);

        // Everything but the winning spelling is a statement of what the library
        // holds, so only that one column is worth typing into.
        const auto readOnly = [](QTableWidgetItem *item) {
            item->setFlags(item->flags() & ~Qt::ItemIsEditable);
            return item;
        };
        // Names are capped to a readable width below, so anything long is only
        // fully legible in the tooltip.
        const auto withTooltip = [&readOnly](const QString &value) {
            auto *item = new QTableWidgetItem(value);
            item->setToolTip(value);
            return readOnly(item);
        };
        ui->tableWidget->setItem(row, COL_ARTIST, withTooltip(proposal.scopeArtist));
        ui->tableWidget->setItem(row, COL_CURRENT, withTooltip(proposal.oldValue));

        const QString target = targetValue(proposal);
        auto *proposedItem = new QTableWidgetItem(target);
        proposedItem->setToolTip(tr("Type a different spelling to make it the winner for this whole group, "
                                    "including \"%1\".").arg(proposal.newValue));
        if (target != proposal.newValue) {
            // An overruled winner is worth spotting at a glance - it is the one
            // thing on screen the analysis did not choose.
            QFont font = proposedItem->font();
            font.setBold(true);
            proposedItem->setFont(font);
        }
        ui->tableWidget->setItem(row, COL_PROPOSED, proposedItem);

        auto *songsItem = new QTableWidgetItem;
        songsItem->setData(Qt::DisplayRole, proposal.songs);
        songsItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        songsItem->setToolTip(tr("%1 song(s) use this spelling, %2 already use \"%3\".")
                                      .arg(QString::number(proposal.songs), QString::number(proposal.keptSongs),
                                           proposal.newValue));
        ui->tableWidget->setItem(row, COL_SONGS, readOnly(songsItem));

        ui->tableWidget->setItem(row, COL_CONFIDENCE,
                                 readOnly(new QTableWidgetItem(confidenceText(proposal.confidence))));
        ui->tableWidget->setItem(row, COL_REASON,
                                 readOnly(new QTableWidgetItem(LibraryNameCleaner::reasonText(proposal.reason))));
    }

    ui->tableWidget->setSortingEnabled(true);
    sizeColumns();
    m_populating = false;
}

void DlgNameCleanup::sizeColumns() {
    auto *table = ui->tableWidget;
    auto *header = table->horizontalHeader();
    table->resizeColumnsToContents();

    // Why is the last column and stretches, so all it needs is for the others
    // not to crowd it out. Sized to their contents, one long artist or title
    // takes the whole width and pushes Why off the right-hand edge - the column
    // that says whether a row can be trusted, and so the last thing that should
    // need scrolling to. The columns holding a value from a short fixed set get
    // what they need; the three free-text ones share what is left of the width.
    // resizeColumnsToContents() has just set every column to its content hint, so the
    // section size is that hint. Asking the view for it directly is not an option:
    // QTableView narrows QAbstractItemView::sizeHintForColumn() to protected.
    const int reason = std::max(header->sectionSize(COL_REASON), header->sectionSizeHint(COL_REASON));
    const int fixed = header->sectionSize(COL_FIELD) + header->sectionSize(COL_SONGS)
                      + header->sectionSize(COL_CONFIDENCE) + reason;
    const int cap = std::max((table->viewport()->width() - fixed) / 3,
                             table->fontMetrics().averageCharWidth() * 12);
    for (const int column : {COL_ARTIST, COL_CURRENT, COL_PROPOSED})
        header->resizeSection(column, std::min(header->sectionSize(column), cap));
}

void DlgNameCleanup::showEvent(QShowEvent *event) {
    QDialog::showEvent(event);
    // The first pass ran against a viewport that had no real width yet.
    sizeColumns();
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

QString DlgNameCleanup::clusterKey(const LibraryNameCleaner::Proposal &proposal) {
    // A separator no artist or title can hold, so two different groups cannot
    // collide by concatenation.
    static const QChar sep(u'\x1f');
    return QString::number(static_cast<int>(proposal.field)) + sep + proposal.scopeArtist + sep
           + proposal.newValue;
}

QString DlgNameCleanup::targetValue(const LibraryNameCleaner::Proposal &proposal) const {
    return m_retargets.value(clusterKey(proposal), proposal.newValue);
}

std::vector<LibraryNameCleaner::Proposal> DlgNameCleanup::checkedProposals() const {
    std::vector<LibraryNameCleaner::Proposal> checked;
    // Groups whose winner has been overruled, and one pristine proposal from
    // each so the old winner can be described afterwards.
    QHash<QString, LibraryNameCleaner::Proposal> overruled;

    for (int row = 0; row < ui->tableWidget->rowCount(); row++) {
        const auto *item = ui->tableWidget->item(row, COL_FIELD);
        if (item == nullptr || item->checkState() != Qt::Checked)
            continue;

        const auto &pristine = m_plan.proposals[item->data(Qt::UserRole).toInt()];
        const QString key = clusterKey(pristine);
        const QString target = targetValue(pristine);
        if (target != pristine.newValue) {
            // Any row of the group names the old winner, but they can disagree
            // on how many songs already spell it that way - a misspelling row
            // counts a whole cluster where a variant row counts one spelling.
            // The larger figure is the one describing the group.
            const auto existing = overruled.constFind(key);
            if (existing == overruled.constEnd() || pristine.keptSongs > existing->keptSongs)
                overruled.insert(key, pristine);
        }

        // A row whose spelling is the one that was picked has nothing left to
        // do - it is already right.
        if (target == pristine.oldValue)
            continue;

        LibraryNameCleaner::Proposal proposal = pristine;
        proposal.newValue = target;
        checked.push_back(proposal);
    }

    // The spelling the group used to agree on is now just another variant, so it
    // needs a row of its own. Without this the majority keeps the old name and
    // picking a different winner would move only the rare spellings onto it.
    for (auto it = overruled.constBegin(); it != overruled.constEnd(); ++it) {
        const LibraryNameCleaner::Proposal &pristine = it.value();
        LibraryNameCleaner::Proposal formerWinner = pristine;
        formerWinner.oldValue = pristine.newValue;
        formerWinner.newValue = m_retargets.value(it.key());
        formerWinner.songs = pristine.keptSongs;
        formerWinner.keptSongs = 0;
        // Not a typo the library disagreed about - a choice that was made here.
        formerWinner.reason = LibraryNameCleaner::Reason::Variant;
        formerWinner.confidence = LibraryNameCleaner::Confidence::High;
        if (formerWinner.oldValue != formerWinner.newValue)
            checked.push_back(formerWinner);
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

void DlgNameCleanup::onItemChanged(QTableWidgetItem *item) {
    if (m_populating || item == nullptr)
        return;
    if (item->column() != COL_PROPOSED) {
        updateSummary();
        return;
    }

    const auto *fieldItem = ui->tableWidget->item(item->row(), COL_FIELD);
    if (fieldItem == nullptr)
        return;
    const auto &pristine = m_plan.proposals[fieldItem->data(Qt::UserRole).toInt()];

    const QString typed = item->text().trimmed();
    if (typed.isEmpty() || typed == targetValue(pristine)) {
        // Nothing was really changed, but the cell may now hold stray spaces.
        m_populating = true;
        item->setText(targetValue(pristine));
        m_populating = false;
        return;
    }

    if (typed == pristine.newValue)
        m_retargets.remove(clusterKey(pristine));
    else
        m_retargets.insert(clusterKey(pristine), typed);

    // Every other row aiming at the same spelling has just changed too, so the
    // table is rebuilt rather than the one cell patched. Rebuilding would reset
    // the ticks to what the analysis suggested, so they are carried over - and
    // the group that was just given a winner is ticked outright, since choosing
    // one is only meaningful if it gets applied.
    const QString editedKey = clusterKey(pristine);
    QHash<int, bool> ticked;
    for (int row = 0; row < ui->tableWidget->rowCount(); row++) {
        const auto *field = ui->tableWidget->item(row, COL_FIELD);
        if (field == nullptr)
            continue;
        const int index = field->data(Qt::UserRole).toInt();
        ticked.insert(index, field->checkState() == Qt::Checked
                                     || clusterKey(m_plan.proposals[index]) == editedKey);
    }

    const int scrollPosition = ui->tableWidget->verticalScrollBar()->value();
    populateTable();
    m_populating = true;
    for (int row = 0; row < ui->tableWidget->rowCount(); row++) {
        auto *field = ui->tableWidget->item(row, COL_FIELD);
        if (field == nullptr)
            continue;
        field->setCheckState(ticked.value(field->data(Qt::UserRole).toInt(), false) ? Qt::Checked
                                                                                   : Qt::Unchecked);
    }
    m_populating = false;
    applyFilter();
    ui->tableWidget->verticalScrollBar()->setValue(scrollPosition);
    updateSummary();
}

void DlgNameCleanup::on_btnBrowseReviewDir_clicked() {
    const QString chosen = QFileDialog::getExistingDirectory(this, tr("Folder for duplicates needing review"),
                                                             ui->lineEditReviewDir->text());
    if (!chosen.isEmpty())
        ui->lineEditReviewDir->setText(chosen);
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

    LibraryNameCleaner::RenamePlan renames;
    if (ui->cbxRenameFiles->isChecked()) {
        // Saved before planning, because working out where a duplicate would go
        // is part of the plan the confirmation describes.
        Settings settings;
        settings.setLibraryReviewDir(ui->lineEditReviewDir->text().trimmed());
        QApplication::setOverrideCursor(Qt::WaitCursor);
        renames = m_cleaner.planRenames(checked);
        QApplication::restoreOverrideCursor();
        if (!m_cleaner.errors().isEmpty()) {
            QMessageBox::warning(this, tr("Could not check the files"), m_cleaner.errors().join("\n"));
            return;
        }
    }

    QMessageBox confirm(this);
    confirm.setText(tr("Apply %1 name changes?").arg(checked.size()));

    QString detail = tr("%1 song entries will be rewritten in the database.\n\n").arg(songs);
    if (!ui->cbxRenameFiles->isChecked()) {
        detail += tr("Files on disk are not touched and nothing is deleted. Every change is written to a "
                     "CSV in the OpenKJ data folder so it can be traced or undone by hand.\n\n"
                     "Note that clearing and rebuilding the database rebuilds these names from the "
                     "filenames again, discarding the corrections.");
    } else {
        detail += tr("%1 songs will also have their files renamed to match, so the fix survives a "
                     "database rebuild. Nothing is deleted, and every rename is written to a CSV in the "
                     "OpenKJ data folder.\n\n")
                          .arg(renames.renames.size());

        const int recycled = renames.countOf(LibraryMerger::Disposal::Recycled);
        const int setAside = renames.countOf(LibraryMerger::Disposal::SetAside);
        if (recycled > 0 || setAside > 0) {
            detail += tr("%1 songs turn out to be duplicates - their corrected name is one another file "
                         "already has - and will be merged into the copy that holds it. Queue entries, "
                         "history and favourites follow the copy that is kept.\n\n")
                              .arg(renames.merges.size());
            if (recycled > 0) {
                detail += tr("%1 of them are the same recording byte for byte, so the spare goes to the "
                             "Recycle Bin. Where the two audio files differ in size the bigger one is "
                             "kept.\n\n")
                                  .arg(recycled);
            }
            if (setAside > 0) {
                detail += tr("%1 share a name but not their graphics, so they are different recordings. "
                             "Nothing of theirs is binned - the spare set is moved to %2 for you to look "
                             "at later.\n\n")
                                  .arg(QString::number(setAside),
                                       ui->lineEditReviewDir->text().trimmed().isEmpty()
                                               ? tr("a folder beside each source directory")
                                               : ui->lineEditReviewDir->text().trimmed());
            }
        }

        if (!renames.keptAsIs.isEmpty()) {
            detail += tr("%1 songs keep the filenames they have - see the details. They are still "
                         "corrected in OpenKJ; only a rebuild would lose them.\n\n")
                              .arg(renames.keptAsIs.size());
        }
        detail += tr("Do this between shows - renaming a file that is queued up or playing is asking "
                     "for trouble. Back up your library before the first run.");
    }
    confirm.setInformativeText(detail);

    if (!renames.keptAsIs.isEmpty() || !renames.merges.empty()) {
        QStringList details;
        if (!renames.merges.empty()) {
            details << tr("Duplicates to be merged:");
            for (const auto &conflict : renames.merges)
                details << LibraryMerger::describe(conflict);
            details << "";
        }
        if (!renames.keptAsIs.isEmpty()) {
            // Counts first, so the question of whether any of this matters can
            // be answered without reading a few hundred lines.
            for (auto it = renames.keptAsIsByCause.constBegin(); it != renames.keptAsIsByCause.constEnd(); ++it)
                details << QString("%1: %2").arg(QString::number(it.value()), it.key());
            details << "" << tr("Song by song:") << renames.keptAsIs;
        }
        confirm.setDetailedText(details.join("\n"));
    }

    confirm.setIcon(QMessageBox::Question);
    confirm.addButton(QMessageBox::Cancel);
    QPushButton *goButton = confirm.addButton(tr("Apply"), QMessageBox::AcceptRole);
    confirm.exec();
    if (confirm.clickedButton() != goButton)
        return;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    const int changed = m_cleaner.execute(checked, renames);
    QApplication::restoreOverrideCursor();

    if (!m_cleaner.errors().isEmpty()) {
        QMessageBox::warning(this, tr("Nothing was changed"),
                             tr("The changes were rolled back and the library is as it was.\n\n%1")
                                     .arg(m_cleaner.errors().join("\n")));
        return;
    }

    m_appliedChanges = true;
    QMessageBox done(this);
    done.setIcon(m_cleaner.warnings().isEmpty() ? QMessageBox::Information : QMessageBox::Warning);
    done.setText(tr("Updated %1 songs.").arg(changed));
    QString outcome;
    if (!m_cleaner.warnings().isEmpty()) {
        outcome = tr("%1 songs could not have their files renamed and were corrected in OpenKJ only.\n\n")
                          .arg(m_cleaner.warnings().size());
        done.setDetailedText(m_cleaner.warnings().join("\n"));
    }
    if (!m_cleaner.journalPath().isEmpty())
        outcome += tr("A record of the changes was written to:\n%1").arg(m_cleaner.journalPath());
    done.setInformativeText(outcome);
    done.exec();

    // Re-run so anything the applied changes newly exposed shows up, and so the
    // list no longer offers what has already been done.
    analyze();
}

void DlgNameCleanup::on_btnClose_clicked() {
    accept();
}
