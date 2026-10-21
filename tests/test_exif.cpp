#include <QtTest>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <cmath>
#include "../app/ExifReader.h"
#include "TestImages.h"

using namespace TestImages;

namespace {
bool writeBytes(const QString &fileName, const QByteArray &bytes)
{
    QFile file(fileName);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
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
    void readsCameraAndDate()
    {
        QByteArray t(160, '\0');
        t[0] = 'I'; t[1] = 'I'; put16(t, 2, 42); put32(t, 4, 8);
        put16(t, 8, 4);
        entry(t, 10, 0x010F, 2, 4, 'D' | ('J' << 8) | ('I' << 16));  // Make "DJI\0" stored inline
        entry(t, 22, 0x0110, 2, 7, 100);          // Model at offset 100
        entry(t, 34, 0x0132, 2, 20, 110);         // DateTime (file change date)
        entry(t, 46, 0x8769, 4, 1, 62);           // EXIF IFD
        put16(t, 62, 1);
        entry(t, 64, 0x9003, 2, 20, 130);         // DateTimeOriginal wins over DateTime
        t.replace(100, 7, QByteArray("FC3411\0", 7));
        t.replace(110, 20, QByteArray("2026:09:20 10:15:00\0", 20));
        t.replace(130, 20, QByteArray("2026:09:21 11:00:00\0", 20));
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString path = tmp.filePath("camera.jpg");
        QVERIFY(writeJpeg(path, app1Segment(t)));
        const ImageMetadata meta = ExifReader::read(path);
        QVERIFY(meta.valid);
        QCOMPARE(meta.cameraMake, QStringLiteral("DJI"));
        QCOMPARE(meta.cameraModel, QStringLiteral("FC3411"));
        QCOMPARE(meta.dateTime, QStringLiteral("2026:09:21 11:00:00"));
        QVERIFY(!meta.hasGps);
    }
    // DJI flight data in XMP: heights, gimbal and drone attitude, the RTK solution and its σ.
    void readsDjiXmp()
    {
        QTemporaryDir tmp;
        const QString path = tmp.filePath(QStringLiteral("DJI_0001.JPG"));
        QVERIFY(writeJpeg(path, withGpsExif() + xmpSegment(djiRtkXmp()), 64, 48));
        const ImageMetadata m = ExifReader::read(path);
        QVERIFY(m.valid && m.hasGps);
        auto near = [](double a, double b) { return std::abs(a - b) < 1e-9; };
        QVERIFY(near(m.relativeAltitude, 80.2));
        QVERIFY(near(m.absoluteAltitude, 120.53));
        QVERIFY(near(m.gimbalYaw, -87.3) && near(m.yaw, -87.3));
        QVERIFY(near(m.pitch, -89.9));
        QVERIFY(near(m.gimbalRoll, 0));
        QVERIFY(near(m.flightYaw, -88.1) && near(m.flightPitch, -3.2) && near(m.flightRoll, 1.4));
        QCOMPARE(m.rtkFlag, 50);
        QVERIFY(near(m.accuracyXy, 0.01445));   // the larger of latitude and longitude
        QVERIFY(near(m.accuracyZ, 0.02531));
    }
    // Elements instead of attributes; the accuracy of other cameras (Camera:GPS…Accuracy);
    // without the gimbal's yaw the drone's heading is the photo's.
    void readsXmpElementsAndCameraAccuracy()
    {
        QTemporaryDir tmp;
        const QString path = tmp.filePath(QStringLiteral("IMG_0001.JPG"));
        const QByteArray xml(
            "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
            "<rdf:Description xmlns:Camera=\"http://pix4d.com/camera/1.0/\" xmlns:drone-dji=\"http://www.dji.com/drone-dji/1.0/\">"
            "<drone-dji:FlightYawDegree> 45.5 </drone-dji:FlightYawDegree>"
            "<Camera:GPSXYAccuracy>1.5</Camera:GPSXYAccuracy><Camera:GPSZAccuracy>3</Camera:GPSZAccuracy>"
            "</rdf:Description></rdf:RDF></x:xmpmeta>");
        QVERIFY(writeJpeg(path, xmpSegment(xml)));
        const ImageMetadata m = ExifReader::read(path);
        QVERIFY(m.valid && !m.hasGps);
        QVERIFY(std::abs(m.yaw - 45.5) < 1e-9 && std::isnan(m.gimbalYaw));
        QVERIFY(std::isnan(m.pitch) && std::isnan(m.relativeAltitude));
        QCOMPARE(m.rtkFlag, -1);
        QVERIFY(std::abs(m.accuracyXy - 1.5) < 1e-9 && std::abs(m.accuracyZ - 3) < 1e-9);
        // No XMP: nothing invented.
        const QString plain = tmp.filePath(QStringLiteral("plain.jpg"));
        QVERIFY(writeJpegWithGps(plain));
        const ImageMetadata p = ExifReader::read(plain);
        QCOMPARE(p.rtkFlag, -1);
        QVERIFY(std::isnan(p.accuracyXy) && std::isnan(p.flightYaw) && std::isnan(p.absoluteAltitude));
    }
    void readsCyrillicPath()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString dir = tmp.filePath(QStringLiteral("Съёмка БПЛА"));
        QVERIFY(QDir().mkpath(dir));
        const QString fileName = dir + QStringLiteral("/Снимок №1.jpg");
        QVERIFY(writeJpegWithGps(fileName));
        const ImageMetadata meta = ExifReader::read(fileName);
        QVERIFY(meta.valid);
        QVERIFY(meta.hasGps);
        QVERIFY(std::abs(meta.latitude - (43 + 14.0 / 60)) < 0.000001);
    }
    void readsTiffWithoutGps()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString path = tmp.filePath(QStringLiteral("ортофото.tiff"));
        QImage input(24, 18, QImage::Format_RGB32);
        input.fill(Qt::green);
        QVERIFY(input.save(path, "TIFF"));
        const ImageMetadata meta = ExifReader::read(path);
        QVERIFY(meta.valid);
        QCOMPARE(meta.width, 24);
        QCOMPARE(meta.height, 18);
        QVERIFY(!meta.hasGps);
    }
    void rejectsCorruptFiles_data()
    {
        QTest::addColumn<QString>("fileName");
        QTest::addColumn<QByteArray>("bytes");
        QByteArray garbage(4096, '\0');
        for (int i = 0; i < garbage.size(); ++i) garbage[i] = char((i * 131 + 7) & 0xff);
        QTest::newRow("garbage jpg") << "garbage.jpg" << garbage;
        QTest::newRow("empty png") << "empty.png" << QByteArray();
        QTest::newRow("text tif") << "text.tif" << QByteArray("not an image at all");
        QTest::newRow("jpeg signature only") << "header.jpg" << (QByteArray::fromHex("ffd8ffe1") + garbage);
        QTest::newRow("tiff signature only") << "header.tiff" << (QByteArray::fromHex("49492a00") + garbage);
    }
    void rejectsCorruptFiles()
    {
        QFETCH(QString, fileName);
        QFETCH(QByteArray, bytes);
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString path = tmp.filePath(fileName);
        QVERIFY(writeBytes(path, bytes));
        const ImageMetadata meta = ExifReader::read(path);
        QVERIFY(!meta.valid);
        QVERIFY(!meta.hasGps);
    }
    void survivesMalformedExif_data()
    {
        QTest::addColumn<QByteArray>("tiff");
        QTest::addColumn<bool>("expectGps");
        QByteArray t = gpsTiff();
        QTest::newRow("valid") << t << true;
        t = gpsTiff(); put32(t, 22, 8);                 // main IFD links to itself
        QTest::newRow("ifd loop") << t << true;
        t = gpsTiff(); put32(t, 18, 5000);              // GPS IFD beyond the block
        QTest::newRow("gps offset out of range") << t << false;
        t = gpsTiff(); put16(t, 8, 0xffff);             // absurd entry count
        QTest::newRow("huge entry count") << t << false;
        t = gpsTiff(); entry(t, 40, 2, 5, 0xffffffffu, 80);
        QTest::newRow("huge value count") << t << false;
        t = gpsTiff(); put32(t, 84, 0);                 // degrees denominator 0
        QTest::newRow("zero denominator") << t << false;
        t = gpsTiff(); rationals(t, 80, 95, 0, 0);      // latitude 95°
        QTest::newRow("latitude out of range") << t << false;
        t = gpsTiff(); rationals(t, 80, 43, 75, 0);     // 75 minutes
        QTest::newRow("minutes out of range") << t << false;
        t = gpsTiff(); entry(t, 28, 1, 2, 2, 'X');      // invalid hemisphere
        QTest::newRow("bad latitude ref") << t << false;
        t = gpsTiff(); put16(t, 2, 7);                  // not a TIFF header
        QTest::newRow("bad tiff magic") << t << false;
        QTest::newRow("truncated tiff") << gpsTiff().left(30) << false;
    }
    void survivesMalformedExif()
    {
        QFETCH(QByteArray, tiff);
        QFETCH(bool, expectGps);
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString path = tmp.filePath("malformed.jpg");
        QVERIFY(writeJpeg(path, app1Segment(tiff)));
        const ImageMetadata meta = ExifReader::read(path);
        QVERIFY(meta.valid);                            // pixels are intact
        QCOMPARE(meta.width, 16);
        QCOMPARE(meta.height, 12);
        QCOMPARE(meta.hasGps, expectGps);
    }
    void survivesTruncatedJpeg()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString path = tmp.filePath("truncated.jpg");
        QVERIFY(writeJpegWithGps(path, 64, 48));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.resize(file.size() / 2));
        file.close();
        const ImageMetadata meta = ExifReader::read(path);  // must not crash or hang
        QVERIFY(!meta.valid || (meta.width == 64 && meta.height == 48));
    }
};

QTEST_GUILESS_MAIN(ExifReaderTest)
#include "test_exif.moc"
