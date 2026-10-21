#include <QtTest>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>
#include "ImageQuality.h"
#include "ImportJob.h"
#include "ThumbnailCache.h"
#include "TestImages.h"

namespace {
bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
bool saveImage(const QString &path, int width, int height, const char *format = nullptr)
{
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(QColor(40, 120, 200));
    return image.save(path, format);
}
// Runs a job to completion on its worker thread and returns all of its results.
QVector<Photo> runJob(ImportJob &job)
{
    QSignalSpy finished(&job, &ImportJob::finished);
    job.start();
    if (!finished.wait(60000)) return {};
    return job.takeResults();
}
QString canonical(const QString &path) { return QFileInfo(path).canonicalFilePath(); }
} // namespace

class ImportTest : public QObject
{
    Q_OBJECT
private slots:
    void importsAndReportsEveryFile()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString project = tmp.filePath(QStringLiteral("Проект"));
        QVERIFY(QDir().mkpath(project + "/photos"));
        const QString src = tmp.filePath(QStringLiteral("Снимки"));
        QVERIFY(QDir().mkpath(src));
        QVERIFY(TestImages::writeJpegWithGps(src + QStringLiteral("/с GPS.jpg")));
        QVERIFY(saveImage(src + "/plain.png", 40, 30));
        QVERIFY(saveImage(src + "/ortho.tif", 50, 20, "TIFF"));
        QVERIFY(saveImage(src + "/known.jpg", 10, 10));
        QVERIFY(writeFile(src + "/broken.jpg", "not a jpeg"));
        QVERIFY(writeFile(src + "/notes.txt", "text"));

        ImportJob::Request request;
        request.projectDir = QDir(project).canonicalPath();
        request.files = {src + QStringLiteral("/с GPS.jpg"), src + "/plain.png", src + "/ortho.tif",
                         src + "/known.jpg", src + "/broken.jpg", src + "/notes.txt",
                         src + "/missing.jpg", src + "/plain.png"};   // the last one twice
        request.knownSources = {canonical(src + "/known.jpg")};
        ImportJob job(request);
        const QVector<Photo> photos = runJob(job);

        QCOMPARE(photos.size(), 3);
        const ImportJob::Progress p = job.progress();
        QCOMPARE(p.total, 7);                 // notes.txt is not queued
        QCOMPARE(p.processed, 7);
        QCOMPARE(p.imported, 3);
        QCOMPARE(p.duplicates, 2);            // known.jpg and the repeated plain.png
        QCOMPARE(p.unreadable, 2);            // broken.jpg and missing.jpg
        QCOMPARE(p.unsupported, 1);
        QCOMPARE(p.copyFailed, 0);
        QVERIFY(!p.scanning);
        QVERIFY(job.failure().isEmpty());
        QCOMPARE(job.issues().size(), 5);

        QCOMPARE(photos[0].name, QStringLiteral("с GPS.jpg"));
        QVERIFY(photos[0].hasGps);
        QCOMPARE(photos[2].width, 50);
        for (const Photo &photo : photos) {
            QVERIFY(photo.file.startsWith("photos/"));
            QVERIFY(QFileInfo::exists(QDir(project).filePath(photo.file)));
            QVERIFY(QFileInfo::exists(ThumbnailCache::thumbnailPath(project, photo.file)));
        }
        QVERIFY(job.takeResults().isEmpty());
    }

    void folderScanSkipsProjectAndHonoursLimit()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString root = tmp.filePath("flight");
        QVERIFY(QDir().mkpath(root + "/sub"));
        QVERIFY(QDir().mkpath(root + "/project/photos"));
        for (int i = 0; i < 4; ++i) QVERIFY(saveImage(QStringLiteral("%1/a%2.jpg").arg(root).arg(i), 8, 8));
        for (int i = 0; i < 4; ++i) QVERIFY(saveImage(QStringLiteral("%1/sub/b%2.png").arg(root).arg(i), 8, 8));
        for (int i = 0; i < 3; ++i) QVERIFY(saveImage(QStringLiteral("%1/project/photos/c%2.jpg").arg(root).arg(i), 8, 8));

        ImportJob::Request all;
        all.folder = QDir(root).canonicalPath();
        all.projectDir = QDir(root + "/project").canonicalPath();
        ImportJob job(all);
        QCOMPARE(runJob(job).size(), 8);      // the project's own photos are never scanned
        QVERIFY(!job.progress().truncated);

        ImportJob::Request limited = all;
        limited.maxFiles = 5;
        ImportJob second(limited);
        QCOMPARE(runJob(second).size(), 5);
        QVERIFY(second.progress().truncated);
        QCOMPARE(second.progress().total, 5);
    }

    void cancelStopsBetweenFiles()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString project = tmp.filePath("project");
        QVERIFY(QDir().mkpath(project + "/photos"));
        ImportJob::Request request;
        request.projectDir = QDir(project).canonicalPath();
        for (int i = 0; i < 60; ++i) {
            const QString file = tmp.filePath(QStringLiteral("src%1.jpg").arg(i));
            QVERIFY(saveImage(file, 1200, 900));
            request.files.append(file);
        }
        ImportJob job(request);
        QSignalSpy finished(&job, &ImportJob::finished);
        connect(&job, &ImportJob::resultsAvailable, &job, [&job] { job.cancel(); });
        job.start();
        QVERIFY(finished.wait(60000));
        const int imported = job.progress().imported;
        QVERIFY2(imported >= 1 && imported < 60, qPrintable(QString::number(imported)));
        QCOMPARE(job.takeResults().size(), imported);
        QCOMPARE(int(QDir(project + "/photos").entryList(QDir::Files).size()), imported);
    }

    void thumbnailIsBoundedAndCached()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString project = tmp.path();
        QVERIFY(QDir().mkpath(project + "/photos"));
        QVERIFY(saveImage(project + "/photos/wide.jpg", 1600, 900));

        QString error;
        const QImage thumb = ThumbnailCache::load(project, "photos/wide.jpg", &error);
        QVERIFY2(!thumb.isNull(), qUtf8Printable(error));
        QCOMPARE(thumb.width(), ThumbnailCache::MaxSide);
        QCOMPARE(thumb.height(), 180);
        const QString cached = ThumbnailCache::thumbnailPath(project, "photos/wide.jpg");
        QCOMPARE(cached, QDir(project).filePath("cache/thumbs/wide_jpg.jpg"));
        QVERIFY(QFileInfo::exists(cached));

        // A valid, newer cache file is used as is (marker image instead of the real preview).
        QImage marker(7, 5, QImage::Format_RGB32);
        marker.fill(Qt::red);
        QVERIFY(marker.save(cached, "JPG"));
        QCOMPARE(ThumbnailCache::load(project, "photos/wide.jpg").size(), QSize(7, 5));

        // A corrupt cache file is rebuilt.
        QVERIFY(writeFile(cached, "garbage"));
        QCOMPARE(ThumbnailCache::load(project, "photos/wide.jpg").size(), QSize(320, 180));

        // A photo newer than its preview is rebuilt as well.
        QVERIFY(marker.save(cached, "JPG"));
        QFile photo(project + "/photos/wide.jpg");
        QVERIFY(photo.open(QIODevice::ReadWrite));
        QVERIFY(photo.setFileTime(QDateTime::currentDateTime().addSecs(60), QFileDevice::FileModificationTime));
        photo.close();
        QCOMPARE(ThumbnailCache::load(project, "photos/wide.jpg").size(), QSize(320, 180));

        // Small images are not enlarged.
        QVERIFY(saveImage(project + "/photos/small.png", 100, 40));
        QCOMPARE(ThumbnailCache::load(project, "photos/small.png").size(), QSize(100, 40));
    }

    void thumbnailFollowsExifOrientation()
    {
        // Orientation 6 (rotate 90° clockwise): a 1600x900 landscape becomes portrait.
        QByteArray tiff(64, '\0');
        tiff[0] = 'I'; tiff[1] = 'I';
        TestImages::put16(tiff, 2, 42);
        TestImages::put32(tiff, 4, 8);
        TestImages::put16(tiff, 8, 1);
        TestImages::entry(tiff, 10, 0x0112, 3, 1, 6);
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QVERIFY(QDir().mkpath(tmp.filePath("photos")));
        QVERIFY(TestImages::writeJpeg(tmp.filePath("photos/rotated.jpg"), TestImages::app1Segment(tiff), 1600, 900));
        QCOMPARE(ThumbnailCache::load(tmp.path(), "photos/rotated.jpg").size(), QSize(180, 320));
    }

    void thumbnailRejectsUnsafePaths_data()
    {
        QTest::addColumn<QString>("file");
        QTest::newRow("parent") << "../outside.jpg";
        QTest::newRow("traversal") << "photos/../geomap.json";
        QTest::newRow("subfolder") << "photos/sub/x.jpg";
        QTest::newRow("backslash") << "photos\\x.jpg";
        QTest::newRow("drive") << "photos/C:x.jpg";
        QTest::newRow("absolute") << "C:/Windows/win.ini";
        QTest::newRow("empty name") << "photos/";
    }
    void thumbnailRejectsUnsafePaths()
    {
        QFETCH(QString, file);
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QString error;
        QVERIFY(ThumbnailCache::thumbnailPath(tmp.path(), file).isEmpty());
        QVERIFY(ThumbnailCache::load(tmp.path(), file, &error).isNull());
        QVERIFY(!error.isEmpty());
        QVERIFY(!QFileInfo::exists(tmp.filePath("cache")));
    }

    void thumbnailOfMissingOrBrokenPhoto()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QVERIFY(QDir().mkpath(tmp.filePath("photos")));
        QString error;
        QVERIFY(ThumbnailCache::load(tmp.path(), "photos/none.jpg", &error).isNull());
        QVERIFY(!error.isEmpty());
        QVERIFY(writeFile(tmp.filePath("photos/broken.jpg"), "not a jpeg"));
        error.clear();
        QVERIFY(ThumbnailCache::load(tmp.path(), "photos/broken.jpg", &error).isNull());
        QVERIFY(!error.isEmpty());
    }

    // Sharpness (imagequality): hard edges keep it, a motion blur takes it down, contrast does not
    // matter; the quality is against the sharpest photos of a set.
    void sharpnessOfPhotos()
    {
        const QImage sharp = TestImages::texturedScene(1200, 900);
        const double s = imagequality::sharpness(sharp);
        const double b5 = imagequality::sharpness(TestImages::motionBlur(sharp, 5));
        const double b9 = imagequality::sharpness(TestImages::motionBlur(sharp, 9));
        QImage dim = sharp;   // the same scene at half the contrast
        for (int y = 0; y < dim.height(); ++y) {
            QRgb *line = reinterpret_cast<QRgb *>(dim.scanLine(y));
            for (int x = 0; x < dim.width(); ++x)
                line[x] = qRgb(64 + qRed(line[x]) / 2, 64 + qGreen(line[x]) / 2, 64 + qBlue(line[x]) / 2);
        }
        const double d = imagequality::sharpness(dim);
        qInfo("sharp %.3f, half contrast %.3f, motion blur 5 px %.3f, 9 px %.3f", s, d, b5, b9);
        QVERIFY(s > 0.8);
        QVERIFY(qAbs(d - s) < 0.03);
        QVERIFY(b9 < b5 && b5 < s);
        QCOMPARE(imagequality::quality(s, s), 1.0);
        QVERIFY(imagequality::quality(b9, s) < imagequality::Threshold);
        QCOMPARE(imagequality::quality(s + 0.1, s), 1.0);   // capped
        QCOMPARE(imagequality::quality(s - 1, s), 0.0);
        QVERIFY(qIsNaN(imagequality::reference({})));
        QVERIFY(qIsNaN(imagequality::quality(0.8, qQNaN())));
        // The 90th percentile of a set: one blurred photo among sharp ones does not move it.
        QVector<double> set(9, s);
        set << b9;
        QVERIFY(qAbs(imagequality::reference(set) - s) < 1e-9);
        QVERIFY(qIsNaN(imagequality::sharpness(QImage(40, 30, QImage::Format_RGB32))));   // too small

        // From files: a large JPEG is read at half size; a broken file is an error.
        QTemporaryDir tmp;
        const QString big = tmp.filePath(QStringLiteral("big.jpg"));
        QVERIFY(TestImages::texturedScene(4400, 3300).save(big, "JPG", 95));
        QElapsedTimer timer;
        timer.start();
        const imagequality::Result r = imagequality::measure(big);
        QVERIFY2(r.ok(), qPrintable(r.error));
        qInfo("4400 × 3300 JPEG: %.3f in %lld ms", r.sharpness, timer.elapsed());
        QVERIFY(r.sharpness > 0.8);
        QVERIFY(writeFile(tmp.filePath(QStringLiteral("broken.jpg")), "not a jpeg"));
        QVERIFY(!imagequality::measure(tmp.filePath(QStringLiteral("broken.jpg"))).ok());
    }

    // GEOMAP_ODM_DATASET=<folder of real photos of one survey>: no sharp photo comes out blurred;
    // copies with 21 px of motion blur (a slow shutter on a fast drone) all do.
    void sharpnessOfRealPhotos()
    {
        const QString folder = qEnvironmentVariable("GEOMAP_ODM_DATASET");
        if (folder.isEmpty()) QSKIP("GEOMAP_ODM_DATASET is not set");
        const QStringList files = QDir(folder).entryList({QStringLiteral("*.jpg"), QStringLiteral("*.jpeg")}, QDir::Files, QDir::Name);
        QVERIFY(files.size() >= 3);
        QVector<double> values;
        QElapsedTimer timer;
        timer.start();
        for (const QString &f : files) {
            const imagequality::Result r = imagequality::measure(QDir(folder).filePath(f));
            QVERIFY2(r.ok(), qPrintable(f + QStringLiteral(": ") + r.error));
            values << r.sharpness;
        }
        const double ref = imagequality::reference(values);
        qInfo("%lld photos, %lld ms each; reference %.3f", qint64(files.size()), timer.elapsed() / files.size(), ref);
        for (int i = 0; i < files.size(); ++i) {
            const double q = imagequality::quality(values[i], ref);
            QVERIFY2(q >= 0.8, qPrintable(QStringLiteral("%1: sharpness %2, quality %3").arg(files[i]).arg(values[i]).arg(q)));
        }
        QTemporaryDir tmp;
        for (int i = 0; i < files.size(); i += qMax(1, int(files.size()) / 3)) {
            const QImage photo(QDir(folder).filePath(files[i]));
            QVERIFY(!photo.isNull());
            double q[2];
            const int lengths[2] = {9, 21};
            for (int k = 0; k < 2; ++k) {
                const QString copy = tmp.filePath(QStringLiteral("blur%1.jpg").arg(lengths[k]));
                QVERIFY(TestImages::motionBlur(photo, lengths[k]).save(copy, "JPG", 92));
                const imagequality::Result r = imagequality::measure(copy);
                QVERIFY2(r.ok(), qPrintable(r.error));
                q[k] = imagequality::quality(r.sharpness, ref);
            }
            qInfo("%s: quality %.2f, motion blur 9 px %.2f, 21 px %.2f", qPrintable(files[i]),
                  imagequality::quality(values[i], ref), q[0], q[1]);
            QVERIFY(q[0] < imagequality::quality(values[i], ref) - 0.2);
            QVERIFY2(q[1] < imagequality::Threshold, qPrintable(files[i]));
        }
    }
};

QTEST_GUILESS_MAIN(ImportTest)
#include "test_import.moc"
