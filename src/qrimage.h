#ifndef QRIMAGE_H
#define QRIMAGE_H

#include <QImage>
#include <QString>

// A QR code for text, one pixel per module, black on white with the four-module quiet
// zone the spec asks for already around it. Null if the text is too long to encode.
// Scale it up with Qt::FastTransformation and a whole-number factor: a smoothly scaled
// QR code is a blurry QR code.
QImage makeQrImage(const QString &text);

#endif // QRIMAGE_H
