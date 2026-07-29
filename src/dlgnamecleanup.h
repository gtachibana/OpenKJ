#ifndef DLGNAMECLEANUP_H
#define DLGNAMECLEANUP_H

#include <QDialog>
#include <vector>

#include "librarynamecleaner.h"

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

private slots:
    void on_btnApply_clicked();
    void on_btnClose_clicked();
    void on_btnCheckVisible_clicked();
    void on_btnUncheckVisible_clicked();
    void on_cbxFilter_currentIndexChanged(int index);
    void onItemChanged();

private:
    Ui::DlgNameCleanup *ui;
    LibraryNameCleaner m_cleaner;
    LibraryNameCleaner::Plan m_plan;
    bool m_appliedChanges{false};
    // Set while the table is being rebuilt, so the check-state signal doesn't
    // recount the summary once per row.
    bool m_populating{false};

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
    void applyFilter();
    void updateSummary();
    [[nodiscard]] std::vector<LibraryNameCleaner::Proposal> checkedProposals() const;
};

#endif // DLGNAMECLEANUP_H
