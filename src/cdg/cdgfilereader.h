#ifndef CDGFILEREADER_H
#define CDGFILEREADER_H

#include <QString>
#include "cdgimageframe.h"
#include "libCDG.h"
#include <spdlog/logger.h>

class CdgFileReader
{
public:
    explicit CdgFileReader(const QString &filename);

    /**
     * @brief Read first/next frame from the data stream.
     * @note  Replaces currentFrame() with the next frame with visible changes and
     * sets currentFrameDurationMS() and currentFramePositionMS() as well.
     * @return true if here are more frames to be read. false if EOF.
     */
    bool moveToNextFrame();

    // By reference: the only caller wants .data() to hand to gst_buffer_fill, and
    // returning by value copied all 56KB of the frame for it, up to 60 times a second.
    const std::array<uchar, cdg::CDG_IMAGE_SIZE> &currentFrame() const { return m_current_image_data; }
    [[nodiscard]] int currentFrameDurationMS() const;
    [[nodiscard]] int currentFramePositionMS() const;

    /**
     * @brief Set currentFrame() to the frame that should be displayed at a given point in time.
     * Note: If the position given is less than the position of currentFrame, the file is "rewinded" and read from the start.
     * @param positionMS The position in milliseconds.
     * @return true is positionMS is within file range.
     */
    bool seek(int positionMS);

    /**
     * @brief Duration of the entire file in milliseconds
     * @return
     */
    int getTotalDurationMS();

    /**
     * Returns the position of the very last frame.
     * This can be less than the total duration, beceause: "total duration = position + duration of final frame".
     *
     * @return The value is not known until all data is read. If so, -1 is returned.
     */
    int positionOfFinalFrameMS();

#ifdef QT_DEBUG
    [[maybe_unused]] void saveNextImgToFile();
    [[maybe_unused]] void saveCurrentImgToFile();
#endif

private:
    std::shared_ptr<spdlog::logger> logger;
    std::string m_loggingPrefix{"[CDGFileReader]"};
    void rewind();
    bool readAndProcessNextPackage();
    inline bool isEOF();

    inline static int getDurationOfPackagesInMS(int numberOfPackages);

    QByteArray m_cdgData;
    int m_cdgDataPos{0};

    std::array<uchar, cdg::CDG_IMAGE_SIZE> m_current_image_data{0};
    int m_current_image_pgk_idx{0};

    CdgImageFrame m_next_image;
    int m_next_image_pgk_idx{0};

    /**
     * Index of the last read package that caused a visible image change.
     * -1 means no frame has been seen yet, matching what rewind() sets.
     */
    int m_last_image_change_pgk_idx{-1};
};

#endif // CDGFILEREADER_H
