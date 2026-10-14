#include "GeoTiffInfo.h"
#include <QByteArray>
#include <QFile>
#include <QLocale>
#include <QStringList>
#include <QVector>
#include <QtEndian>
#include <cstring>

namespace {
constexpr quint16 TagWidth = 256, TagHeight = 257, TagSamples = 277;
constexpr quint16 TagPixelScale = 33550, TagTiepoint = 33922, TagGeoKeys = 34735;
constexpr quint64 MaxValueBytes = 1024 * 1024;

int typeSize(quint16 type)
{
    switch (type) {
    case 1: case 2: case 6: case 7: return 1;
    case 3: case 8: return 2;
    case 4: case 9: case 11: return 4;
    case 5: case 10: case 12: case 16: case 17: case 18: return 8;
    default: return 0;
    }
}

class Reader
{
public:
    explicit Reader(QFile &file) : m_file(file) {}
    bool little = true;
    bool big = false;

    bool bytes(quint64 pos, quint64 n, QByteArray &out)
    {
        if (n > MaxValueBytes || pos > quint64(m_file.size()) || n > quint64(m_file.size()) - pos) return false;
        if (!m_file.seek(qint64(pos))) return false;
        out = m_file.read(qint64(n));
        return quint64(out.size()) == n;
    }
    quint16 u16(const char *p) const
    {
        const auto *u = reinterpret_cast<const uchar *>(p);
        return little ? qFromLittleEndian<quint16>(u) : qFromBigEndian<quint16>(u);
    }
    quint32 u32(const char *p) const
    {
        const auto *u = reinterpret_cast<const uchar *>(p);
        return little ? qFromLittleEndian<quint32>(u) : qFromBigEndian<quint32>(u);
    }
    quint64 u64(const char *p) const
    {
        const auto *u = reinterpret_cast<const uchar *>(p);
        return little ? qFromLittleEndian<quint64>(u) : qFromBigEndian<quint64>(u);
    }
    double f64(const char *p) const
    {
        const quint64 v = u64(p);
        double d;
        std::memcpy(&d, &v, sizeof d);
        return d;
    }
    // Unsigned integer value number `i` of an entry (SHORT, LONG or LONG8).
    quint64 integer(quint16 type, const QByteArray &data, int i) const
    {
        const int size = typeSize(type);
        if (size == 0 || (i + 1) * size > data.size()) return 0;
        const char *p = data.constData() + i * size;
        return size == 2 ? u16(p) : size == 4 ? u32(p) : u64(p);
    }

private:
    QFile &m_file;
};

struct Entry { quint16 tag = 0; quint16 type = 0; quint64 count = 0; QByteArray value; };
} // namespace

GeoTiffInfo GeoTiffInfo::read(const QString &path)
{
    GeoTiffInfo info;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        info.error = file.errorString();
        return info;
    }
    Reader r(file);
    QByteArray header;
    if (!r.bytes(0, 16, header) && !r.bytes(0, 8, header)) {
        info.error = QStringLiteral("File too small for a TIFF header");
        return info;
    }
    if (header.startsWith("II")) r.little = true;
    else if (header.startsWith("MM")) r.little = false;
    else {
        info.error = QStringLiteral("Not a TIFF file");
        return info;
    }
    const quint16 magic = r.u16(header.constData() + 2);
    quint64 ifd = 0;
    if (magic == 42) {
        ifd = r.u32(header.constData() + 4);
    } else if (magic == 43 && header.size() >= 16 && r.u16(header.constData() + 4) == 8) {
        r.big = true;
        ifd = r.u64(header.constData() + 8);
    } else {
        info.error = QStringLiteral("Unsupported TIFF variant");
        return info;
    }
    info.bigTiff = r.big;

    // First IFD: entry count, then 12-byte (TIFF) or 20-byte (BigTIFF) entries.
    const int countSize = r.big ? 8 : 2, entrySize = r.big ? 20 : 12, inlineSize = r.big ? 8 : 4;
    QByteArray raw;
    if (!r.bytes(ifd, quint64(countSize), raw)) {
        info.error = QStringLiteral("IFD offset outside the file");
        return info;
    }
    const quint64 entries = r.big ? r.u64(raw.constData()) : r.u16(raw.constData());
    if (entries == 0 || entries > 4096 || !r.bytes(ifd + quint64(countSize), entries * quint64(entrySize), raw)) {
        info.error = QStringLiteral("Corrupt IFD");
        return info;
    }
    QVector<Entry> list;
    for (quint64 i = 0; i < entries; ++i) {
        const char *p = raw.constData() + i * quint64(entrySize);
        Entry e;
        e.tag = r.u16(p);
        e.type = r.u16(p + 2);
        e.count = r.big ? r.u64(p + 4) : r.u32(p + 4);
        const int size = typeSize(e.type);
        if (size == 0 || e.count == 0 || e.count > MaxValueBytes / quint64(size)) continue;
        const quint64 total = e.count * quint64(size);
        const char *valueField = p + (r.big ? 12 : 8);
        if (total <= quint64(inlineSize)) {
            e.value = QByteArray(valueField, int(total));
        } else {
            const quint64 offset = r.big ? r.u64(valueField) : r.u32(valueField);
            if (!r.bytes(offset, total, e.value)) continue;
        }
        list.append(e);
    }

    auto find = [&list](quint16 tag) -> const Entry * {
        for (const Entry &e : list) if (e.tag == tag) return &e;
        return nullptr;
    };
    if (const Entry *e = find(TagWidth)) info.width = int(r.integer(e->type, e->value, 0));
    if (const Entry *e = find(TagHeight)) info.height = int(r.integer(e->type, e->value, 0));
    if (const Entry *e = find(TagSamples)) info.bands = int(r.integer(e->type, e->value, 0));
    else info.bands = 1;
    info.valid = info.width > 0 && info.height > 0;

    const Entry *scale = find(TagPixelScale);
    const Entry *tie = find(TagTiepoint);
    if (scale && tie && scale->type == 12 && tie->type == 12 && scale->value.size() >= 16 && tie->value.size() >= 48) {
        info.pixelSizeX = r.f64(scale->value.constData());
        info.pixelSizeY = r.f64(scale->value.constData() + 8);
        const double i = r.f64(tie->value.constData()), j = r.f64(tie->value.constData() + 8);
        const double x = r.f64(tie->value.constData() + 24), y = r.f64(tie->value.constData() + 32);
        info.originX = x - i * info.pixelSizeX;
        info.originY = y + j * info.pixelSizeY;
        info.georeferenced = info.pixelSizeX > 0 && info.pixelSizeY > 0;
    }
    // GeoKeyDirectory: 4-short header, then (KeyID, TIFFTagLocation, Count, Value) per key.
    if (const Entry *keys = find(TagGeoKeys); keys && keys->type == 3 && keys->value.size() >= 8) {
        const int shorts = keys->value.size() / 2;
        const int numberOfKeys = int(r.integer(3, keys->value, 3));
        int projected = 0, geographic = 0, model = 0;
        for (int k = 0; k < numberOfKeys && 4 + k * 4 + 3 < shorts; ++k) {
            const int base = 4 + k * 4;
            const quint64 id = r.integer(3, keys->value, base);
            const quint64 location = r.integer(3, keys->value, base + 1);
            const quint64 value = r.integer(3, keys->value, base + 3);
            if (location != 0) continue;   // values stored in other tags are not needed here
            if (id == 1024) model = int(value);
            else if (id == 3072) projected = int(value);
            else if (id == 2048) geographic = int(value);
        }
        // 32767 means "user-defined": no EPSG code to report.
        if (projected > 0 && projected != 32767) { info.epsg = projected; info.projected = true; }
        else if (geographic > 0 && geographic != 32767) info.epsg = geographic;
        if (model == 1) info.projected = true;
    }
    if (!info.valid) info.error = QStringLiteral("Missing image size");
    return info;
}

QString GeoTiffInfo::describe() const
{
    if (!valid) return QStringLiteral("не читается: %1").arg(error);
    QStringList parts;
    parts << (epsg ? QStringLiteral("EPSG:%1").arg(epsg) : QStringLiteral("CRS не определён"));
    if (georeferenced) {
        const QLocale ru(QLocale::Russian);
        if (projected) parts << QStringLiteral("%1 см/пикс").arg(ru.toString(pixelSizeX * 100.0, 'f', 1));
        else parts << QStringLiteral("%1°/пикс").arg(ru.toString(pixelSizeX, 'g', 3));
    } else {
        parts << QStringLiteral("без геопривязки");
    }
    parts << QStringLiteral("%1×%2").arg(width).arg(height);
    parts << QStringLiteral("каналов: %1").arg(bands);
    return parts.join(QStringLiteral(", "));
}
