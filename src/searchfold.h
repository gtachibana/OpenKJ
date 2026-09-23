#ifndef SEARCHFOLD_H
#define SEARCHFOLD_H

#include <QString>

namespace okj {

// Lowercases and strips accents, so that a search typed on a keyboard without them -
// "beyonce", "motorhead" - still finds "Beyoncé" and "Motörhead". Applied to both the
// text being searched and the query, so the two only ever meet in folded form.
QString foldForSearch(const QString &text);

// Fills dbsongs.artistfold/titlefold on every row that lacks them. New rows arrive with
// both NULL, and a trigger (see MainWindow::dbInit() v111) clears them whenever artist
// or title is changed, so anything that writes a song's names - whatever it is - only
// leaves work for this, never a stale value. Cheap when there is nothing to do: the
// pending rows are found through idx_dbsongs_fold rather than a scan.
int refreshSearchFolds();

}

#endif // SEARCHFOLD_H
