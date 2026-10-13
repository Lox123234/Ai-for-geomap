#include <QtTest>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <cmath>
#include "../app/ExifReader.h"

namespace {
void put16(QByteArray &b, int pos, quint16 n)
{
    b[pos] = char(n & 0xff);
    b[pos + 1] = char((n >> 8) & 0xff);
}
void put32(QByteArray &b, int pos, quint32 n)
{
    for (int i = 0; i < 4; ++i) b[pos + i] = char((n >> (8 * i)) & 0xff);
}
void entry(QByteArray &b, int pos, quint16 tag, quint16 type, quint32 count, quint32 value)
{
    put16(b, pos, tag);
    put16(b, pos + 2, type);
    put32(b, pos + 4, count);
    put32(b, pos + 8, value);
}
void rationals(QByteArray &b, int pos, quint32 deg, quint32 min, quint32 sec)
{
    put32(b, pos, deg); put32(b, pos + 4, 1);
    put32(b, pos + 8, min); put32(b, pos + 12, 1);
    put32(b, pos + 16, sec); put32(b, pos + 20, 1);
}
QByteArray withGpsExif()
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
    const QByteArray app1 = QByteArray("Exif\0\0", 6) + t;
    QByteArray marker = QByteArray::fromHex("ffe1");
    const quint16 size = quint16(app1.size() + 2);
    marker.append(char(size >> 8)); marker.append(char(size & 0xff));
    return marker + app1;
}
} // namespace

class ExifReaderTest : public QObject
{
    Q_OBJECT
private slots:
    void readsGpsInJpeg()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString fileName = tmp.filePath("location.jpg");
        QImage input(16, 12, QImage::Format_RGB32);
        input.fill(Qt::cyan);
        QVERIFY(input.save(fileName, "JPG"));
        QFile file(fileName);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QByteArray bytes = file.readAll(); file.close();
        QVERIFY(bytes.startsWith(QByteArray::fromHex("ffd8")));
        bytes.insert(2, withGpsExif());
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write(bytes), qint64(bytes.size())); file.close();
        const ImageMetadata meta = ExifReader::read(fileName);
        QVERIFY(meta.valid);
        QVERIFY(meta.hasGps);
        QCOMPARE(meta.width, 16);
        QCOMPARE(meta.height, 12);
        QVERIFY(std::abs(meta.latitude - (43 + 14.0 / 60)) < 0.000001);
        QVERIFY(std::abs(meta.longitude - (76 + 56.0 / 60)) < 0.000001);
    }
    void readsSimplePngWithoutGps()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString path = tmp.filePath("plain.png");
        QImage input(20, 10, QImage::Format_RGB32);
        input.fill(Qt::red);
        QVERIFY(input.save(path));
        const ImageMetadata meta = ExifReader::read(path);
        QVERIFY(meta.valid);
        QCOMPARE(meta.width, 20);
        QCOMPARE(meta.height, 10);
        QVERIFY(!meta.hasGps);
    }
    void rejectsMissingFile()
    {
        const ImageMetadata meta = ExifReader::read("/no-such-image-90ea81.jpg");
        QVERIFY(!meta.valid);
        QVERIFY(!meta.hasGps);
    }
};

QTEST_GUILESS_MAIN(ExifReaderTest)
#include "test_exif.moc"
