#pragma once

#include <QByteArray>
#include <QFile>
#include <QImage>
#include <QString>

// Synthetic images shared by the test executables.
namespace TestImages {

inline void put16(QByteArray &b, int pos, quint16 n)
{
    b[pos] = char(n & 0xff);
    b[pos + 1] = char((n >> 8) & 0xff);
}
inline void put32(QByteArray &b, int pos, quint32 n)
{
    for (int i = 0; i < 4; ++i) b[pos + i] = char((n >> (8 * i)) & 0xff);
}
inline void entry(QByteArray &b, int pos, quint16 tag, quint16 type, quint32 count, quint32 value)
{
    put16(b, pos, tag);
    put16(b, pos + 2, type);
    put32(b, pos + 4, count);
    put32(b, pos + 8, value);
}
inline void rationals(QByteArray &b, int pos, quint32 deg, quint32 min, quint32 sec)
{
    put32(b, pos, deg); put32(b, pos + 4, 1);
    put32(b, pos + 8, min); put32(b, pos + 12, 1);
    put32(b, pos + 16, sec); put32(b, pos + 20, 1);
}
// Little-endian TIFF block with a GPS IFD: 43°14'N, 76°56'E.
inline QByteArray gpsTiff()
{
    QByteArray t(128, '\0');
    t[0] = 'I'; t[1] = 'I'; put16(t, 2, 42); put32(t, 4, 8);
    put16(t, 8, 1);                           // one entry in main IFD
    entry(t, 10, 0x8825, 4, 1, 26);           // GPS IFD at offset 26
    put32(t, 22, 0);
    put16(t, 26, 4);                          // GPS: 4 entries
    entry(t, 28, 1, 2, 2, 'N');               // GPSLatitudeRef, inline N\0
    entry(t, 40, 2, 5, 3, 80);                // GPSLatitude
    entry(t, 52, 3, 2, 2, 'E');               // GPSLongitudeRef
    entry(t, 64, 4, 5, 3, 104);              // GPSLongitude
    put32(t, 76, 0);
    rationals(t, 80, 43, 14, 0);
    rationals(t, 104, 76, 56, 0);
    return t;
}
// JPEG APP1 segment (marker + length + "Exif\0\0" + TIFF block).
inline QByteArray app1Segment(const QByteArray &tiff)
{
    const QByteArray app1 = QByteArray("Exif\0\0", 6) + tiff;
    QByteArray marker = QByteArray::fromHex("ffe1");
    const quint16 size = quint16(app1.size() + 2);
    marker.append(char(size >> 8)); marker.append(char(size & 0xff));
    return marker + app1;
}
inline QByteArray withGpsExif() { return app1Segment(gpsTiff()); }

// Writes a real JPEG and inserts `segment` right after the SOI marker.
inline bool writeJpeg(const QString &fileName, const QByteArray &segment, int width = 16, int height = 12)
{
    QImage input(width, height, QImage::Format_RGB32);
    input.fill(Qt::cyan);
    if (!input.save(fileName, "JPG")) return false;
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) return false;
    QByteArray bytes = file.readAll(); file.close();
    if (!bytes.startsWith(QByteArray::fromHex("ffd8"))) return false;
    bytes.insert(2, segment);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return file.write(bytes) == bytes.size();
}
inline bool writeJpegWithGps(const QString &fileName, int width = 16, int height = 12)
{
    return writeJpeg(fileName, withGpsExif(), width, height);
}

} // namespace TestImages
