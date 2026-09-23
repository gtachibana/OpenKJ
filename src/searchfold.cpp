#include "searchfold.h"

#include <array>
#include <utility>
#include <vector>
#include <QSqlQuery>

namespace okj {

QString foldForSearch(const QString &text) {
    // Most of a library is plain ASCII, and this runs over every song at load.
    bool ascii = true;
    for (const QChar c : text) {
        if (c.unicode() >= 0x80) {
            ascii = false;
            break;
        }
    }
    if (ascii)
        return text.toLower();

    // Compatibility decomposition splits "é" into "e" plus a combining accent, and also
    // flattens ligatures and full-width forms ("ﬁ" -> "fi", "Ａ" -> "A").
    const QString decomposed = text.normalized(QString::NormalizationForm_KD);
    QString out;
    out.reserve(decomposed.size());
    for (const QChar c : decomposed) {
        switch (c.category()) {
            case QChar::Mark_NonSpacing:
            case QChar::Mark_SpacingCombining:
            case QChar::Mark_Enclosing:
                continue;
            default:
                out.append(c);
        }
    }
    out = out.toLower();

    // Letters that are distinct characters rather than a base plus an accent, so
    // decomposition leaves them alone. Lowercased first, so only one form is needed.
    static const std::array<std::pair<QChar, QLatin1StringView>, 9> standalone{{
            {QChar(0x00DF), QLatin1StringView("ss")}, // ß
            {QChar(0x00E6), QLatin1StringView("ae")}, // æ
            {QChar(0x0153), QLatin1StringView("oe")}, // œ
            {QChar(0x00F8), QLatin1StringView("o")},  // ø
            {QChar(0x0142), QLatin1StringView("l")},  // ł
            {QChar(0x0111), QLatin1StringView("d")},  // đ
            {QChar(0x00F0), QLatin1StringView("d")},  // ð
            {QChar(0x00FE), QLatin1StringView("th")}, // þ
            {QChar(0x0131), QLatin1StringView("i")},  // dotless ı
    }};
    for (const auto &[from, to] : standalone) {
        if (out.contains(from))
            out.replace(from, to);
    }
    return out;
}

int refreshSearchFolds() {
    QSqlQuery select;
    // Answered from idx_dbsongs_fold, so an up-to-date library costs one index probe.
    if (!select.exec("SELECT songid, artist, title FROM dbsongs WHERE artistfold IS NULL"))
        return 0;

    struct Row {
        int id;
        QString artist;
        QString title;
    };
    std::vector<Row> rows;
    while (select.next())
        rows.push_back({select.value(0).toInt(), select.value(1).toString(), select.value(2).toString()});
    select.finish();
    if (rows.empty())
        return 0;

    QSqlQuery tx;
    tx.exec("BEGIN TRANSACTION");
    QSqlQuery update;
    update.prepare("UPDATE dbsongs SET artistfold = :artist, titlefold = :title WHERE songid = :id");
    for (const auto &row : rows) {
        update.bindValue(":artist", foldForSearch(row.artist));
        update.bindValue(":title", foldForSearch(row.title));
        update.bindValue(":id", row.id);
        update.exec();
    }
    tx.exec("COMMIT");
    return static_cast<int>(rows.size());
}

}
