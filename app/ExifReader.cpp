#include "ExifReader.h"
#include <QFile>
#include <QImageReader>
#include <QByteArray>
#include <QSet>
#include <QtEndian>
#include <cmath>
#include <cstring>

namespace {
class TiffView {
public:
    explicit TiffView(QByteArray bytes) : b(std::move(bytes))
    {
        if (b.size() >= 8) {
            le = b[0] == 'I' && b[1] == 'I';
            ok = (le || (b[0] == 'M' && b[1] == 'M')) && u16(2) == 42;
        }
    }
    bool good() const { return ok; }
    bool inRange(quint64 offset, quint64 length) const
    {
        return offset <= quint64(b.size()) && length <= quint64(b.size()) - offset;
    }
    quint16 u16(quint64 pos) const
    {
        if (!inRange(pos, 2)) return 0;
        const auto *p = reinterpret_cast<const uchar*>(b.constData()) + pos;
        return le ? qFromLittleEndian<quint16>(p) : qFromBigEndian<quint16>(p);
    }
    quint32 u32(quint64 pos) const
    {
        if (!inRange(pos, 4)) return 0;
        const auto *p = reinterpret_cast<const uchar*>(b.constData()) + pos;
        return le ? qFromLittleEndian<quint32>(p) : qFromBigEndian<quint32>(p);
    }
    QString ascii(quint64 offset, quint32 count) const
    {
        if (count > 16384 || !inRange(offset, count)) return {};
        const QByteArray str = b.mid(qsizetype(offset), qsizetype(count));
        return QString::fromLatin1(str.constData(), str.indexOf('\0') >= 0 ? str.indexOf('\0') : str.size()).trimmed();
    }
    double rational(quint64 pos) const
    {
        const quint32 num = u32(pos), den = u32(pos + 4);
        return inRange(pos, 8) && den != 0 ? double(num) / double(den) : NAN;
    }
    void parse(ImageMetadata &meta)
    {
        if (!ok) return;
        QSet<quint32> visited;
        readIfd(u32(4), meta, 0, visited);
    }
private:
    QByteArray b;
    bool le = false;
    bool ok = false;
    void readIfd(quint32 offset, ImageMetadata &meta, int kind, QSet<quint32> &visited)
    {
        if (visited.contains(offset) || visited.size() > 12 || !inRange(offset, 2)) return;
        visited.insert(offset);
        const quint16 entries = u16(offset);
        if (entries > 512 || !inRange(quint64(offset) + 2, quint64(entries) * 12)) return;
        QString latRef, lonRef;
        double lat = NAN, lon = NAN;
        for (int i = 0; i < entries; ++i) {
            const quint64 pos = quint64(offset) + 2 + quint64(i) * 12;
            const quint16 tag = u16(pos), type = u16(pos + 2);
            const quint32 count = u32(pos + 4), ptr = u32(pos + 8);
            const quint64 units = type == 2 || type == 1 || type == 7 ? 1
                : type == 3 ? 2 : type == 4 || type == 9 ? 4 : type == 5 || type == 10 ? 8 : 0;
            if (!units || count > 1'000'000 || quint64(count) * units > 4'000'000) continue;
            const quint64 bytes = quint64(count) * units;
            const quint64 val = bytes <= 4 ? pos + 8 : ptr;
            if (!inRange(val, bytes)) continue;
            if (kind == 0 && tag == 0x8769 && type == 4) readIfd(ptr, meta, 1, visited);
            else if (kind == 0 && tag == 0x8825 && type == 4) readIfd(ptr, meta, 2, visited);
            else if (kind == 0 && tag == 0x010F && type == 2) meta.cameraMake = ascii(val, count);
            else if (kind == 0 && tag == 0x0110 && type == 2) meta.cameraModel = ascii(val, count);
            else if ((kind == 0 && tag == 0x0132 || kind == 1 && tag == 0x9003) && type == 2) {
                if (kind == 1 || meta.dateTime.isEmpty()) meta.dateTime = ascii(val, count);
            }
            else if (kind == 2 && tag == 1 && type == 2) latRef = ascii(val, count);
            else if (kind == 2 && tag == 3 && type == 2) lonRef = ascii(val, count);
            else if (kind == 2 && (tag == 2 || tag == 4) && type == 5 && count == 3) {
                const double deg = rational(val), min = rational(val + 8), sec = rational(val + 16);
                if (std::isfinite(deg) && std::isfinite(min) && std::isfinite(sec) && min < 60 && sec < 60) {
                    const double result = deg + min / 60 + sec / 3600;
                    if (tag == 2) lat = result;
                    else lon = result;
                }
            }
        }
        if (kind == 2 && std::isfinite(lat) && std::isfinite(lon)
            && lat <= 90 && lon <= 180
            && (latRef == "N" || latRef == "S") && (lonRef == "E" || lonRef == "W")) {
            meta.hasGps = true;
            meta.latitude = latRef == "S" ? -lat : lat;
            meta.longitude = lonRef == "W" ? -lon : lon;
        }
        // Walk linked IFDs only for main directories, not EXIF/GPS directories.
        const quint64 next = quint64(offset) + 2 + quint64(entries) * 12;
        if (kind == 0 && inRange(next, 4) && u32(next) != 0)
            readIfd(u32(next), meta, 3, visited);
    }
};

QByteArray extractExif(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    if (file.peek(2) == QByteArray::fromHex("ffd8")) {
        file.read(2);
        // JPEG metadata is before image data. Limit search to 8 MiB.
        while (file.pos() < 8 * 1024 * 1024 && !file.atEnd()) {
            const QByteArray lead = file.read(1);
            if (lead.size() != 1) break;
            if (uchar(lead[0]) != 0xff) break;
            QByteArray markerBytes;
            do { markerBytes = file.read(1); } while (markerBytes.size() == 1 && uchar(markerBytes[0]) == 0xff);
            if (markerBytes.isEmpty()) break;
            const quint8 marker = quint8(markerBytes[0]);
            if (marker == 0xda || marker == 0xd9) break; // scan or end
            if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;
            const QByteArray len = file.read(2);
            if (len.size() != 2) break;
            const int n = (quint8(len[0]) << 8) | quint8(len[1]);
            if (n < 2) break;
            if (marker == 0xe1) {
                const QByteArray chunk = file.read(n - 2);
                if (chunk.startsWith(QByteArray("Exif\0\0", 6))) return chunk.mid(6);
            } else if (!file.seek(file.pos() + n - 2)) break;
        }
        return {};
    }
    const QByteArray begin = file.peek(4);
    if (begin == QByteArray::fromHex("49492a00") || begin == QByteArray::fromHex("4d4d002a"))
        return file.read(8 * 1024 * 1024);
    return {};
}
} // namespace

ImageMetadata ExifReader::read(const QString &path)
{
    ImageMetadata meta;
    QImageReader reader(path);
    reader.setAutoTransform(true);
    if (!reader.canRead()) return meta;
    const QSize size = reader.size();
    meta.valid = size.isValid();
    meta.width = size.width();
    meta.height = size.height();
    TiffView exif(extractExif(path));
    if (exif.good()) exif.parse(meta);
    return meta;
}
