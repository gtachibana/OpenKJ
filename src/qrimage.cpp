#include "qrimage.h"
#include <stdexcept>
#include <spdlog/spdlog.h>
#include "3rdparty/qrcodegen/qrcodegen.hpp"

QImage makeQrImage(const QString &text) {
    // Enough of a quiet zone for a phone camera to lock on.
    constexpr int quietModules = 4;
    try {
        // MEDIUM rather than LOW: the code is photographed off a screen from across a
        // bar, at an angle, through a phone camera that is doing its best.
        const auto qr = qrcodegen::QrCode::encodeText(text.toUtf8().constData(), qrcodegen::QrCode::Ecc::MEDIUM);
        const int modules = qr.getSize();
        const int side = modules + (quietModules * 2);
        QImage image(side, side, QImage::Format_RGB32);
        image.fill(Qt::white);
        for (int y = 0; y < modules; y++) {
            for (int x = 0; x < modules; x++) {
                if (qr.getModule(x, y))
                    image.setPixel(x + quietModules, y + quietModules, qRgb(0, 0, 0));
            }
        }
        return image;
    } catch (const std::exception &e) {
        // Only reachable if the address is longer than a QR code can hold, which means
        // it is not an address anyone is going to type either.
        if (auto logger = spdlog::get("logger"))
            logger->warn("[QrImage] Could not encode {} as a QR code: {}", text.toStdString(), e.what());
        return {};
    }
}
