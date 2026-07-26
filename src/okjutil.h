#ifndef OKJUTIL_H
#define OKJUTIL_H
#include <QString>
#include <QFileInfo>
#include <QDirIterator>
#include <QRegularExpression>

// Given a cdg file path, tries to find a matching supported audio file
// Returns an empty QString if no match is found
// Optimized for finding most common file extensions first
inline QString findMatchingAudioFile(const QString &cdgFilePath) {
    std::array<QString, 41> audioExtensions{
        "mp3",
        "MP3",
        "wav",
        "WAV",
        "ogg",
        "OGG",
        "mov",
        "MOV",
        "flac",
        "FLAC",
        "Mp3","mP3",
        "Wav","wAv","waV","WAv","wAV","WaV",
        "Ogg","oGg","ogG","OGg","oGG","OgG",
        "Mov","mOv","moV","MOv","mOV","MoV",
        "Flac","fLac","flAc","flaC","FLac","FLAc",
        "flAC","fLAC","FlaC", "FLaC", "FlAC"
    };

    QFileInfo cdgInfo(cdgFilePath);
    for (const auto &ext : audioExtensions) {
        QString testPath = cdgInfo.absolutePath() + QDir::separator() + cdgInfo.completeBaseName() + '.' + ext;
        if (QFile::exists(testPath))
            return testPath;
    }
    return QString();
}

// GStreamer is handed file paths encoded with QString::toLocal8Bit() (see
// MediaBackend::play), which silently mangles any character the local 8-bit
// codec can't represent. Returns true when a path survives that round trip and
// can therefore be played straight from its original location instead of being
// copied somewhere with a safe name first.
inline bool pathIsGstSafe(const QString &path) {
    return QString::fromLocal8Bit(path.toLocal8Bit()) == path;
}

// Strips any "[...]" bracketed segments (e.g. "[Karaoke Version]") out of a
// song artist/title string, collapsing the resulting whitespace.
inline QString stripBracketedText(const QString &text) {
    static const QRegularExpression bracketRe(R"(\[[^\]]*\])");
    QString result = text;
    result.remove(bracketRe);
    return result.simplified();
}



#endif // OKJUTIL_H
