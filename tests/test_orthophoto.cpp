#include <QtTest>
#include <QDir>
#include <QElapsedTimer>
#include <QImage>
#include <QTemporaryDir>
#include <cmath>
#include <gdal.h>
#include <ogr_srs_api.h>
#include "GisEnvironment.h"
#include "TrueOrthophoto.h"

// True orthophoto on a synthetic scene with a known answer (ground, an 8 m building, a conical
// tree, nine photos with lens distortion rendered from the surface) and, with
// GEOMAP_ORTHO_JOB=<ODM job folder>, on real reconstruction results.
namespace {
constexpr double OffsetX = 500000, OffsetY = 5000000, Ground = 100;

double surface(double x, double y)   // local frame, metres
{
    if (std::fabs(x) <= 5 && std::fabs(y) <= 5) return Ground + 8;                // building
    const double r = std::hypot(x - 12, y - 8);
    if (r < 4) return Ground + 6 * (1 - r / 4);                                   // tree
    return Ground;
}

QRgb texture(double x, double y)
{
    if (std::fabs(x) <= 5 && std::fabs(y) <= 5)                                   // roof: stripes
        return int(std::floor(x / 0.5)) % 2 ? qRgb(210, 40, 30) : qRgb(240, 235, 225);
    const double r = std::hypot(x - 12, y - 8);
    if (r < 4) return int(r / 0.5) % 2 ? qRgb(40, 140, 50) : qRgb(120, 200, 80);   // crown rings
    const bool checker = (int(std::floor(x)) + int(std::floor(y))) % 2 == 0;
    const bool stripe = int(std::floor(x / 0.2)) % 2 == 0;
    const int k = stripe ? 20 : 0;
    return checker ? qRgb(180 - k, 170 - k, 140 - k) : qRgb(90 + k, 110 + k, 70 + k);
}

ortho::Shot nadirShot(double cx, double cy, double cz, const QString &image)
{
    ortho::Shot s;
    s.image = image;
    s.camera.width = 1200;
    s.camera.height = 800;
    s.camera.fx = s.camera.fy = 0.9;
    s.camera.k1 = -0.02;
    const double R[9] = {1, 0, 0, 0, -1, 0, 0, 0, -1};   // looking down, image up = north
    std::copy(R, R + 9, s.R);
    const double C[3] = {cx, cy, cz};
    for (int i = 0; i < 3; ++i) s.t[i] = -(R[i * 3] * C[0] + R[i * 3 + 1] * C[1] + R[i * 3 + 2] * C[2]);
    std::copy(C, C + 3, s.centre);
    return s;
}

// The photo of a shot: surface points every 1.5 cm splatted with a z-buffer.
QImage render(const ortho::Shot &s)
{
    QImage img(s.camera.width, s.camera.height, QImage::Format_RGB32);
    img.fill(Qt::black);
    std::vector<float> zb(size_t(img.width()) * img.height(), 1e30f);
    const double step = 0.015;
    for (double y = -20; y < 20; y += step)
        for (double x = -20; x < 20; x += step) {
            const double P[3] = {x, y, surface(x, y)};
            double u, v, d;
            if (!s.project(P, &u, &v, &d)) continue;
            const int px = int(std::lround(u)), py = int(std::lround(v));
            if (px < 0 || py < 0 || px >= img.width() || py >= img.height()) continue;
            float &z = zb[size_t(py) * img.width() + px];
            if (d < z) {
                z = float(d);
                img.setPixel(px, py, texture(x, y));
            }
        }
    return img;
}

bool writeDsm(const QString &path, double cell)
{
    const int n = int(std::lround(40 / cell));
    GDALDatasetH ds = GDALCreate(GDALGetDriverByName("GTiff"), QDir::toNativeSeparators(path).toUtf8().constData(), n, n, 1, GDT_Float32, nullptr);
    if (!ds) return false;
    double gt[6] = {OffsetX - 20, cell, 0, OffsetY + 20, 0, -cell};
    GDALSetGeoTransform(ds, gt);
    OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
    OSRImportFromEPSG(srs, 32633);
    GDALSetSpatialRef(ds, srs);
    OSRDestroySpatialReference(srs);
    GDALSetRasterNoDataValue(GDALGetRasterBand(ds, 1), -9999);
    std::vector<float> line(n);
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) line[i] = float(surface(-20 + (i + 0.5) * cell, 20 - (j + 0.5) * cell));
        GDALRasterIO(GDALGetRasterBand(ds, 1), GF_Write, 0, j, n, 1, line.data(), n, 1, GDT_Float32, 0, 0);
    }
    GDALClose(ds);
    return true;
}
} // namespace

class OrthophotoTest : public QObject
{
    Q_OBJECT
    QTemporaryDir m_dir;

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        QVERIFY2(GisEnvironment::initialize(QCoreApplication::applicationDirPath()), qPrintable(GisEnvironment::problems().join('\n')));
    }

    void syntheticScene()
    {
        ortho::Scene scene;
        scene.offsetX = OffsetX;
        scene.offsetY = OffsetY;
        QElapsedTimer timer;
        timer.start();
        for (int j = -1; j <= 1; ++j)
            for (int i = -1; i <= 1; ++i) {
                const QString path = m_dir.filePath(QStringLiteral("photo_%1_%2.jpg").arg(i + 1).arg(j + 1));
                ortho::Shot s = nadirShot(i * 10.0, j * 10.0, Ground + 40, path);
                QVERIFY(render(s).save(path, "JPG", 95));
                scene.shots << s;
            }
        qInfo("9 photos rendered in %lld ms", timer.elapsed());
        const QString dsm = m_dir.filePath(QStringLiteral("dsm.tif"));
        QVERIFY(writeDsm(dsm, 0.05));
        const QString out = m_dir.filePath(QStringLiteral("true_ortho.tif"));
        const ortho::Result r = ortho::build(scene, dsm, out);
        QVERIFY2(r.ok(), qPrintable(r.error));
        qInfo("True orthophoto %dx%d at %.3f m (GSD %.3f m), %d objects, coverage %.3f, %.1f s", r.width, r.height, r.resolution,
              r.groundSampleDistance, r.objects, r.coverage, r.seconds);
        QVERIFY2(qAbs(r.groundSampleDistance - 40 / 1080.0) < 0.002, qPrintable(QString::number(r.groundSampleDistance)));
        QCOMPARE(r.objects, 2);   // the building and the tree
        QVERIFY(r.coverage > 0.98);

        // Compare with the texture of the surface at every output pixel.
        GDALDatasetH ds = GDALOpen(QDir::toNativeSeparators(out).toUtf8().constData(), GA_ReadOnly);
        QVERIFY(ds);
        double gt[6];
        GDALGetGeoTransform(ds, gt);
        QCOMPARE(gt[0], OffsetX - 20);
        QCOMPARE(gt[3], OffsetY + 20);
        const int w = GDALGetRasterXSize(ds), h = GDALGetRasterYSize(ds);
        std::vector<uchar> rgba(size_t(w) * h * 4);
        int bands[4] = {1, 2, 3, 4};
        QCOMPARE(GDALDatasetRasterIO(ds, GF_Read, 0, 0, w, h, rgba.data(), w, h, GDT_Byte, 4, bands, 4, w * 4, 1), CE_None);
        GDALClose(ds);
        double sum = 0, roofSum = 0, nearSum = 0;
        qint64 count = 0, wrong = 0, roof = 0, nearCount = 0, nearWrong = 0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const uchar *p = &rgba[(size_t(y) * w + x) * 4];
                if (!p[3]) continue;
                const double X = gt[0] + (x + 0.5) * gt[1] - OffsetX, Y = gt[3] + (y + 0.5) * gt[5] - OffsetY;
                // Pixels on texture edges are mixtures: not counted.
                bool edge = false;
                const QRgb e = texture(X, Y);
                for (double dx : {-0.03, 0.03})
                    for (double dy : {-0.03, 0.03}) edge |= texture(X + dx, Y + dy) != e;
                if (edge) continue;
                const double err = (std::abs(p[0] - qRed(e)) + std::abs(p[1] - qGreen(e)) + std::abs(p[2] - qBlue(e))) / 3.0;
                sum += err;
                ++count;
                wrong += err > 50;
                if (std::fabs(X) <= 5 && std::fabs(Y) <= 5) { roofSum += err; ++roof; }
                // The ground right next to the building: hidden in some photos, must not get the roof or a wall.
                const double ring = qMax(std::fabs(X), std::fabs(Y));
                if (ring > 5.2 && ring < 7) { nearSum += err; ++nearCount; nearWrong += err > 50; }
            }
        qInfo("Mean |error| %.2f (roof %.2f, ground next to the building %.2f); wrong pixels %.3f %% (next to the building %.3f %%)",
              sum / count, roofSum / roof, nearSum / nearCount, 100.0 * wrong / count, 100.0 * nearWrong / nearCount);
        QVERIFY2(sum / count < 12, qPrintable(QString::number(sum / count)));
        QVERIFY2(roofSum / roof < 12, qPrintable(QString::number(roofSum / roof)));
        QVERIFY2(double(wrong) / count < 0.01, qPrintable(QString::number(double(wrong) / count)));
        QVERIFY2(double(nearWrong) / nearCount < 0.02, qPrintable(QString::number(double(nearWrong) / nearCount)));
    }

    void realJob()
    {
        const QString job = qEnvironmentVariable("GEOMAP_ORTHO_JOB");
        if (job.isEmpty()) QSKIP("GEOMAP_ORTHO_JOB is not set");
        ortho::Scene scene;
        QString error;
        QVERIFY2(ortho::readOdmJob(job, &scene, &error), qPrintable(error));
        const QString outDir = qEnvironmentVariable("GEOMAP_TEST_OUTPUT", m_dir.path());
        const QString out = QDir(outDir).filePath(QStringLiteral("true_orthophoto.tif"));
        QFile::remove(out);
        ortho::Options options;
        options.resolution = qEnvironmentVariable("GEOMAP_ORTHO_RESOLUTION").toDouble();
        const QString dtm = QDir(job).filePath(QStringLiteral("odm_dem/dtm.tif"));
        if (QFileInfo::exists(dtm)) options.groundPath = dtm;
        const ortho::Result r = ortho::build(scene, QDir(job).filePath(QStringLiteral("odm_dem/dsm.tif")), out, options);
        QVERIFY2(r.ok(), qPrintable(r.error));
        QStringList gains;
        for (double g : r.gains) gains << QString::number(g, 'f', 3);
        qInfo("%s: %d photos, %dx%d at %.3f m (GSD %.3f m), %d objects, coverage %.4f (hidden %.4f), %.1f s; gains %s", qPrintable(out), r.photos,
              r.width, r.height, r.resolution, r.groundSampleDistance, r.objects, r.coverage, r.hidden, r.seconds, qPrintable(gains.join(' ')));
        QVERIFY(r.coverage > 0.95);
    }
};

QTEST_MAIN(OrthophotoTest)
#include "test_orthophoto.moc"
