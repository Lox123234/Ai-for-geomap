#include <QtTest>
#include <QDir>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <cmath>
#include <gdal.h>
#include <ogr_srs_api.h>
#include "GisEnvironment.h"
#include "MockEngine.h"
#include "PhotoModel.h"
#include "ProcessingService.h"
#include "ProcessingTypes.h"
#include "ResultExport.h"
#include "ResultExporter.h"

// Export of processing results: rasters re-encoded, reprojected and resampled (GeoTIFF/COG),
// models with their MTL and textures, unique names, and the background service on a job.
namespace {
bool makeRgb(const QString &path, int w, int h, const double gt[6], int epsg)
{
    GDALDatasetH ds = GDALCreate(GDALGetDriverByName("GTiff"), QDir::toNativeSeparators(path).toUtf8().constData(), w, h, 3, GDT_Byte, nullptr);
    if (!ds) return false;
    double t[6];
    std::copy(gt, gt + 6, t);
    GDALSetGeoTransform(ds, t);
    OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
    OSRImportFromEPSG(srs, epsg);
    GDALSetSpatialRef(ds, srs);
    OSRDestroySpatialReference(srs);
    std::vector<unsigned char> row(w);
    for (int b = 1; b <= 3; ++b)
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) row[x] = static_cast<unsigned char>((x + y * b) % 256);
            GDALRasterIO(GDALGetRasterBand(ds, b), GF_Write, 0, y, w, 1, row.data(), w, 1, GDT_Byte, 0, 0);
        }
    GDALClose(ds);
    return true;
}
bool writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}
struct Opened
{
    GDALDatasetH ds;
    explicit Opened(const QString &p) : ds(GDALOpen(QDir::toNativeSeparators(p).toUtf8().constData(), GA_ReadOnly)) {}
    ~Opened() { if (ds) GDALClose(ds); }
};
} // namespace

class ExportTest : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QVERIFY2(GisEnvironment::initialize(QCoreApplication::applicationDirPath()), qPrintable(GisEnvironment::problems().join('\n')));
        QVERIFY(m_dir.isValid());
        const double gt[6] = {576000, 0.5, 0, 5188100, 0, -0.5};   // 100 × 80 m in UTM 15N, 0.5 m cells
        QVERIFY(makeRgb(m_dir.filePath("ortho.tif"), 200, 160, gt, 32615));
    }

    void rasterKeepsReprojectsAndResamples()
    {
        using namespace exporting;
        // Same CRS, re-encoded: identical grid.
        Result r = exportRaster(m_dir.filePath("ortho.tif"), m_dir.filePath("out/same.tif"), {});
        QVERIFY2(r.ok(), qPrintable(r.error));
        {
            Opened o(r.path);
            QVERIFY(o.ds);
            QCOMPARE(GDALGetRasterXSize(o.ds), 200);
            QCOMPARE(GDALGetRasterCount(o.ds), 3);
        }
        // WGS 84: geographic coordinates around (-92.0°, 46.8°).
        RasterOptions geo;
        geo.crs = Crs::fromEpsg(4326);
        r = exportRaster(m_dir.filePath("ortho.tif"), m_dir.filePath("out/wgs84.tif"), geo);
        QVERIFY2(r.ok(), qPrintable(r.error));
        {
            Opened o(r.path);
            double gt[6];
            QCOMPARE(GDALGetGeoTransform(o.ds, gt), CE_None);
            QVERIFY2(std::abs(gt[0] + 92.0) < 0.05 && std::abs(gt[3] - 46.84) < 0.05, qPrintable(QStringLiteral("%1 %2").arg(gt[0]).arg(gt[3])));
            QVERIFY(QString::fromUtf8(GDALGetProjectionRef(o.ds)).contains(QStringLiteral("WGS 84")));
        }
        // Cell size 2 m: a quarter of the columns.
        RasterOptions coarse;
        coarse.resolution = 2.0;
        r = exportRaster(m_dir.filePath("ortho.tif"), m_dir.filePath("out/2m.tif"), coarse);
        QVERIFY2(r.ok(), qPrintable(r.error));
        {
            Opened o(r.path);
            QCOMPARE(GDALGetRasterXSize(o.ds), 50);
            QCOMPARE(GDALGetRasterYSize(o.ds), 40);
        }
        // Cloud Optimized GeoTIFF.
        RasterOptions cog;
        cog.cog = true;
        r = exportRaster(m_dir.filePath("ortho.tif"), m_dir.filePath("out/cog.tif"), cog);
        QVERIFY2(r.ok(), qPrintable(r.error));
        {
            Opened o(r.path);
            QCOMPARE(QString::fromLatin1(GDALGetMetadataItem(o.ds, "LAYOUT", "IMAGE_STRUCTURE")), QStringLiteral("COG"));
        }
        // Missing source: an error, no file.
        r = exportRaster(m_dir.filePath("missing.tif"), m_dir.filePath("out/x.tif"), {});
        QVERIFY(!r.ok());
        QVERIFY(!QFileInfo::exists(m_dir.filePath("out/x.tif")));
        // Cancelled before it starts writing: nothing left behind.
        std::atomic_bool stop{true};
        r = exportRaster(m_dir.filePath("ortho.tif"), m_dir.filePath("out/cancelled.tif"), {}, &stop);
        QVERIFY(!r.ok());
        QVERIFY(!QFileInfo::exists(m_dir.filePath("out/cancelled.tif")));
    }

    void modelWithMaterialsAndTextures()
    {
        const QString src = m_dir.filePath("model");
        QVERIFY(writeFile(src + "/m.obj", "mtllib m.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nusemtl a\nf 1/1 2/1 3/1\n"));
        QVERIFY(writeFile(src + "/m.mtl", "newmtl a\nmap_Kd tex/a.png\nnewmtl b\nmap_Kd b.png\n"));
        QVERIFY(writeFile(src + "/tex/a.png", "png-a"));
        QVERIFY(writeFile(src + "/b.png", "png-b"));
        QVERIFY(writeFile(src + "/unrelated.txt", "x"));
        const exporting::Result r = exporting::exportObj(src + "/m.obj", m_dir.filePath("out/model"));
        QVERIFY2(r.ok(), qPrintable(r.error));
        QCOMPARE(r.files.size(), 4);
        QVERIFY(QFileInfo::exists(m_dir.filePath("out/model/tex/a.png")));
        QVERIFY(QFileInfo::exists(m_dir.filePath("out/model/b.png")));
        QVERIFY(!QFileInfo::exists(m_dir.filePath("out/model/unrelated.txt")));
        // A texture outside the model folder is refused, and a missing one reported.
        QVERIFY(writeFile(src + "/bad.mtl", "newmtl a\nmap_Kd ../secret.png\n"));
        QVERIFY(writeFile(m_dir.filePath("secret.png"), "s"));
        QVERIFY(writeFile(src + "/bad.obj", "mtllib bad.mtl\nv 0 0 0\n"));
        QVERIFY(!exporting::exportObj(src + "/bad.obj", m_dir.filePath("out/bad")).ok());
        QVERIFY(writeFile(src + "/gone.mtl", "newmtl a\nmap_Kd nothere.png\n"));
        QVERIFY(writeFile(src + "/gone.obj", "mtllib gone.mtl\nv 0 0 0\n"));
        QVERIFY(exporting::exportObj(src + "/gone.obj", m_dir.filePath("out/gone")).error.contains(QStringLiteral("nothere.png")));
        // Unique names.
        QCOMPARE(QFileInfo(exporting::uniqueTarget(m_dir.filePath("out"), "same.tif")).fileName(), QStringLiteral("same-2.tif"));
        QCOMPARE(QFileInfo(exporting::uniqueTarget(m_dir.filePath("out"), "new.tif")).fileName(), QStringLiteral("new.tif"));
    }

    void serviceExportsAJob()
    {
        // A project with one completed job: orthophoto, model and report.
        const QString project = m_dir.filePath("project");
        ProcessingJob job;
        job.id = QStringLiteral("20261015-120000");
        job.engine = QStringLiteral("mock");
        job.profile = QStringLiteral("balanced");
        job.state = JobState::Completed;
        job.createdAt = QDateTime::currentDateTimeUtc();
        job.outputs << Artifact{QStringLiteral("orthophoto"), QStringLiteral("odm_orthophoto/odm_orthophoto.tif"), 1, QString()}
                    << Artifact{QStringLiteral("mesh"), QStringLiteral("odm_texturing/m.obj"), 1, QString()}
                    << Artifact{QStringLiteral("report"), QStringLiteral("odm_report/report.pdf"), 1, QString()};
        const QDir jobDir(project + "/jobs/" + job.id);
        QVERIFY(QDir().mkpath(jobDir.filePath("odm_orthophoto")));
        QVERIFY(QFile::copy(m_dir.filePath("ortho.tif"), jobDir.filePath("odm_orthophoto/odm_orthophoto.tif")));
        QVERIFY(writeFile(jobDir.filePath("odm_texturing/m.obj"), "mtllib m.mtl\nv 0 0 0\n"));
        QVERIFY(writeFile(jobDir.filePath("odm_texturing/m.mtl"), "newmtl a\nmap_Kd t.png\n"));
        QVERIFY(writeFile(jobDir.filePath("odm_texturing/t.png"), "t"));
        QVERIFY(writeFile(jobDir.filePath("odm_report/report.pdf"), "%PDF-1.4"));
        QVERIFY(writeFile(jobDir.filePath("job.json"), QJsonDocument(job.toJson()).toJson()));

        auto record = std::make_shared<MockEngine::Record>();
        ProcessingService processing(new MockEngine(record));
        PhotoModel photos;
        processing.setProject(project, &photos);
        ResultExporter exporter(&processing);
        QCOMPARE(exporter.latestJobWithResults(), job.id);
        const QVariantList results = exporter.results(job.id);
        QCOMPARE(results.size(), 3);
        QCOMPARE(results.first().toMap().value("title").toString(), QStringLiteral("Ортофотоплан"));
        QVERIFY(results.first().toMap().value("raster").toBool());

        const QString out = m_dir.filePath("export");
        // Refusals: unknown file, folder inside the job, unknown CRS.
        QVERIFY(!exporter.start(job.id, {QStringLiteral("odm_dem/dsm.tif")}, QUrl::fromLocalFile(out)).isEmpty());
        QVERIFY(!exporter.start(job.id, {job.outputs.first().file}, QUrl::fromLocalFile(jobDir.filePath("x"))).isEmpty());
        QVERIFY(!exporter.start(job.id, {job.outputs.first().file}, QUrl::fromLocalFile(out), {{"crs", "EPSG:999999"}}).isEmpty());

        QSignalSpy finished(&exporter, &ResultExporter::finished);
        const QStringList all{job.outputs[0].file, job.outputs[1].file, job.outputs[2].file};
        QCOMPARE(exporter.start(job.id, all, QUrl::fromLocalFile(out), {{"crs", "4326"}, {"cog", true}}), QString());
        QVERIFY(exporter.busy());
        QVERIFY(finished.wait(60000));
        QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(1).toString()));
        QCOMPARE(finished.first().at(2).toStringList().size(), 3);
        QVERIFY(QFileInfo::exists(out + "/20261015-120000-odm_orthophoto-epsg4326-cog.tif"));
        QVERIFY(QFileInfo::exists(out + "/20261015-120000-odm_texturing/m.obj"));
        QVERIFY(QFileInfo::exists(out + "/20261015-120000-odm_texturing/t.png"));
        QVERIFY(QFileInfo::exists(out + "/20261015-120000-report.pdf"));
        // Again: nothing overwritten.
        finished.clear();
        QCOMPARE(exporter.start(job.id, {job.outputs[2].file}, QUrl::fromLocalFile(out)), QString());
        QVERIFY(finished.wait(60000));
        QVERIFY(QFileInfo::exists(out + "/20261015-120000-report-2.pdf"));
    }

private:
    QTemporaryDir m_dir;
};

QTEST_MAIN(ExportTest)
#include "test_export.moc"
