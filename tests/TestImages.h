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

// JPEG APP1 segment with an XMP packet.
inline QByteArray xmpSegment(const QByteArray &xml)
{
    const QByteArray app1 = QByteArray("http://ns.adobe.com/xap/1.0/\0", 29) + xml;
    QByteArray marker = QByteArray::fromHex("ffe1");
    const quint16 size = quint16(app1.size() + 2);
    marker.append(char(size >> 8)); marker.append(char(size & 0xff));
    return marker + app1;
}

// The XMP of a DJI Phantom 4 RTK photo: flight data as attributes, RTK fixed.
inline QByteArray djiRtkXmp()
{
    return QByteArray(
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
        "<rdf:Description rdf:about=\"DJI Meta Data\" xmlns:drone-dji=\"http://www.dji.com/drone-dji/1.0/\"\n"
        " drone-dji:AbsoluteAltitude=\"+120.53\"\n drone-dji:RelativeAltitude=\"+80.20\"\n"
        " drone-dji:GimbalRollDegree=\"+0.00\"\n drone-dji:GimbalYawDegree=\"-87.30\"\n drone-dji:GimbalPitchDegree=\"-89.90\"\n"
        " drone-dji:FlightRollDegree=\"+1.40\"\n drone-dji:FlightYawDegree=\"-88.10\"\n drone-dji:FlightPitchDegree=\"-3.20\"\n"
        " drone-dji:RtkFlag=\"50\"\n drone-dji:RtkStdLon=\"0.01262\"\n drone-dji:RtkStdLat=\"0.01445\"\n drone-dji:RtkStdHgt=\"0.02531\"/>\n"
        "</rdf:RDF></x:xmpmeta>");
}

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

// A scene with hard edges, like roofs and fields from above: 400 rectangles of random colours on
// a grey ground (fixed seed).
inline QImage texturedScene(int width, int height, quint32 seed = 1)
{
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(QColor(110, 120, 100));
    auto next = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    for (int i = 0; i < 400; ++i) {
        const int w = 8 + int(next() % quint32(width / 10)), h = 8 + int(next() % quint32(height / 10));
        const int x0 = int(next() % quint32(width)), y0 = int(next() % quint32(height));
        const int g = int(next() % 256);
        const QRgb c = qRgb(g, qBound(0, g + int(next() % 40) - 20, 255), qBound(0, g - 15, 255));
        for (int y = y0; y < qMin(height, y0 + h); ++y) {
            QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
            for (int x = x0; x < qMin(width, x0 + w); ++x) line[x] = c;
        }
    }
    return image;
}

// Horizontal motion blur over `length` pixels: a moving average along the rows.
inline QImage motionBlur(const QImage &source, int length)
{
    const QImage in = source.convertToFormat(QImage::Format_RGB32);
    QImage out(in.size(), QImage::Format_RGB32);
    const int r = length / 2, w = in.width();
    for (int y = 0; y < in.height(); ++y) {
        const QRgb *src = reinterpret_cast<const QRgb *>(in.constScanLine(y));
        QRgb *dst = reinterpret_cast<QRgb *>(out.scanLine(y));
        int sum[3] = {0, 0, 0};
        for (int k = -r; k <= r; ++k) {
            const QRgb p = src[qBound(0, k, w - 1)];
            sum[0] += qRed(p); sum[1] += qGreen(p); sum[2] += qBlue(p);
        }
        for (int x = 0; x < w; ++x) {
            dst[x] = qRgb(sum[0] / (2 * r + 1), sum[1] / (2 * r + 1), sum[2] / (2 * r + 1));
            const QRgb add = src[qMin(w - 1, x + r + 1)], drop = src[qMax(0, x - r)];
            sum[0] += qRed(add) - qRed(drop); sum[1] += qGreen(add) - qGreen(drop); sum[2] += qBlue(add) - qBlue(drop);
        }
    }
    return out;
}

} // namespace TestImages
