#ifndef DLGNAMECLEANUP_H
#define DLGNAMECLEANUP_H

#include <QDialog>
#include <QHash>
#include <QString>
#include <vector>

#include "librarynamecleaner.h"

class QTableWidgetItem;

namespace Ui {
    class DlgNameCleanup;
}

// Review screen for LibraryNameCleaner. Runs the analysis on open, lists what it
// found, and applies only the rows the KJ ticks. High confidence rows start
// ticked; everything else has to be looked at first.
class DlgNameCleanup : public QDialog
{
Q_OBJECT

public:
    explicit DlgNameCleanup(QWidget *parent = nullptr);
    ~DlgNameCleanup() override;

    // True if anything was written, so the caller knows to reload the library.
    [[nodiscard]] bool appliedChanges() const { return m_appliedChanges; }

protected:
    void showEvent(QShowEvent *event) override;

private slots:
    void on_btnApply_clicked();
    void on_btnClose_clicked();
    void on_btnCheckVisible_clicked();
    void on_btnUncheckVisible_clicked();
    void on_btnBrowseReviewDir_clicked();
    void on_cbxFilter_currentIndexChanged(int index);
    void onItemChanged(QTableWidgetItem *item);

private:
    Ui::DlgNameCleanup *ui;
    LibraryNameCleaner m_cleaner;
    LibraryNameCleaner::Plan m_plan;
    bool m_appliedChanges{false};
    // Set while the table is being rebuilt, so the check-state signal doesn't
    // recount the summary once per row.
    bool m_populating{false};

    // Winning spellings the KJ has overruled, keyed by cluster.
    //
    // The analysis picks a winner by song count, which answers "which of these
    // is the typo" but not "which of these is right" - the library agreeing on a
    // spelling 180 times over does not make it the one wanted. Typing into a
    // Proposed cell replaces the winner for the whole group, so the majority
    // spelling becomes just another variant and gets renamed along with the
    // rest. Editing a single row instead would be a no-op whenever the value
    // typed is that row's own current spelling, which is precisely the case
    // worth having.
    QHash<QString, QString> m_retargets;

    // Identifies the group a proposal belongs to: every proposal that is aiming
    // at the same spelling, for the same field, under the same artist. Built
    // from the pristine analysis, never from a retargeted copy.
    [[nodiscard]] static QString clusterKey(const LibraryNameCleaner::Proposal &proposal);

    // The spelling this proposal is currently aimed at, the KJ's override
    // included.
    [[nodiscard]] QString targetValue(const LibraryNameCleaner::Proposal &proposal) const;

    enum Columns {
        COL_FIELD = 0,
        COL_ARTIST,
        COL_CURRENT,
        COL_PROPOSED,
        COL_SONGS,
        COL_CONFIDENCE,
        COL_REASON
    };

    void analyze();
    void populateTable();
    void sizeColumns();
    void applyFilter();
    void updateSummary();
    [[nodiscard]] std::vector<LibraryNameCleaner::Proposal> checkedProposals() const;
};

#endif // DLGNAMECLEANUP_H
