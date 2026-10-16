// Real reconstruction with the built-in OpenDroneMap through ProcessingService + OdmEngine, on a
// folder of overlapping drone photos (GEOMAP_ODM_DATASET), and a check of everything it produces:
// orthophoto, DSM, DTM, point cloud, textured model, report, and the results on the map.
// GEOMAP_ODM_HOME points at the ODM folder when it is not next to the test (engines\odm).
// Registered with ctest only when configured with -DGEOMAP_ODM_DATASET=<folder>; minutes to an hour.
// The project is created and filled as in the application (ProjectManager); GEOMAP_ODM_KEEP=1
// keeps it, and it opens in GeoMap AI. GEOMAP_ODM_FULL=0 skips the long full-profile run. Qt Test
// stops a function after 5 minutes by default: run it with QTEST_FUNCTION_TIMEOUT (ctest sets 6 hours).
#include <QtTest>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QProcess>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <gdal.h>
#include <memory>
#include "ChunkService.h"
#include "GisEnvironment.h"
#include "MapService.h"
#include "OdmEngine.h"
#include "Overlap.h"
#include "PhotoModel.h"
#include "ProcessingService.h"
#include "ProjectManager.h"
#include "RasterSource.h"

namespace {
// Processes whose executable lies in the ODM folder (Windows), e.g. after a cancel.
int odmProcesses(const QString &root)
{
#ifdef Q_OS_WIN
    QProcess ps;
    const QString filter = QDir::toNativeSeparators(root).replace(QLatin1String("'"), QLatin1String("''"));
    ps.start(QStringLiteral("powershell"), {QStringLiteral("-NoProfile"), QStringLiteral("-Command"),
             QStringLiteral("@(Get-Process | Where-Object { $_.Path -like '%1*' }).Count").arg(filter)});
    ps.waitForFinished(60000);
    return QString::fromLatin1(ps.readAllStandardOutput()).trimmed().toInt();
#else
    Q_UNUSED(root)
    return 0;
#endif
}

struct LasHeader
{
    bool valid = false;
    int major = 0, minor = 0;
    quint64 points = 0;
    double minX = 0, maxX = 0, minY = 0, maxY = 0, minZ = 0, maxZ = 0;
};

// Public header block of LAS 1.x / LAZ (the header is not compressed).
LasHeader readLas(const QString &path)
{
    LasHeader h;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return h;
    const QByteArray b = f.read(375);
    if (b.size() < 227 || !b.startsWith("LASF")) return h;
    const auto u8 = [&b](int o) { return int(quint8(b[o])); };
    const auto f64 = [&b](int o) { return qFromLittleEndian<double>(b.constData() + o); };
    h.major = u8(24);
    h.minor = u8(25);
    h.points = qFromLittleEndian<quint32>(b.constData() + 107);
    if (h.minor >= 4 && b.size() >= 255) h.points = qMax(h.points, qFromLittleEndian<quint64>(b.constData() + 247));
    h.maxX = f64(179); h.minX = f64(187);
    h.maxY = f64(195); h.minY = f64(203);
    h.maxZ = f64(211); h.minZ = f64(219);
    h.valid = true;
    return h;
}

struct ObjSummary
{
    int vertices = 0, texcoords = 0, faces = 0;
    QStringList materials, textures, missingTextures;
};

ObjSummary readObj(const QString &path)
{
    ObjSummary s;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return s;
    const QDir dir = QFileInfo(path).absoluteDir();
    while (!f.atEnd()) {
        const QByteArray line = f.readLine();
        if (line.startsWith("v ")) ++s.vertices;
        else if (line.startsWith("vt ")) ++s.texcoords;
        else if (line.startsWith("f ")) ++s.faces;
        else if (line.startsWith("mtllib ")) s.materials << QString::fromUtf8(line.mid(7).trimmed());
    }
    for (const QString &mtl : s.materials) {
        QFile m(dir.filePath(mtl));
        if (!m.open(QIODevice::ReadOnly)) continue;
        while (!m.atEnd()) {
            const QByteArray line = m.readLine().trimmed();
            if (!line.startsWith("map_Kd ")) continue;
            const QString texture = QString::fromUtf8(line.mid(7).trimmed());
            s.textures << texture;
            if (!QFileInfo::exists(dir.filePath(texture))) s.missingTextures << texture;
        }
    }
    return s;
}

// Elevation at a map coordinate (nearest cell), NaN outside or on nodata.
struct Dem
{
    GDALDatasetH ds = nullptr;
    double gt[6] = {};
    int width = 0, height = 0;
    int hasNoData = 0;
    double noData = 0;
    std::vector<float> values;

    bool open(const QString &path)
    {
        ds = GDALOpen(path.toUtf8().constData(), GA_ReadOnly);
        if (!ds || GDALGetGeoTransform(ds, gt) != CE_None) return false;
        width = GDALGetRasterXSize(ds);
        height = GDALGetRasterYSize(ds);
        GDALRasterBandH band = GDALGetRasterBand(ds, 1);
        noData = GDALGetRasterNoDataValue(band, &hasNoData);
        values.resize(size_t(width) * height);
        return GDALRasterIO(band, GF_Read, 0, 0, width, height, values.data(), width, height, GDT_Float32, 0, 0) == CE_None;
    }
    ~Dem() { if (ds) GDALClose(ds); }
    double at(double x, double y) const
    {
        const int col = int(std::floor((x - gt[0]) / gt[1]));
        const int row = int(std::floor((y - gt[3]) / gt[5]));
        if (col < 0 || row < 0 || col >= width || row >= height) return std::nan("");
        const float v = values[size_t(row) * width + col];
        if (!std::isfinite(v) || (hasNoData && std::fabs(v - noData) < 1e-3)) return std::nan("");
        return v;
    }
};
} // namespace

class OdmIntegrationTest : public QObject
{
    Q_OBJECT
private:
    QTemporaryDir m_tmp{QDir::temp().filePath(QStringLiteral("GeoMap ODM тест-XXXXXX"))};
    QTemporaryDir m_settings;
    QString m_project;
    std::unique_ptr<ProjectManager> m_projects;
    std::unique_ptr<ProcessingService> m_service;
    OdmEngine *m_engine = nullptr;

    QString runToEnd(const QString &profile, const QVariantMap &options, int timeoutHours, QStringList *stages)
    {
        QSignalSpy finished(m_service.get(), &ProcessingService::jobFinished);
        auto connection = connect(m_service.get(), &ProcessingService::jobUpdated, this, [this, stages](const QString &id) {
            const QString stage = m_service->job(id).stage;
            if (!stage.isEmpty() && !stages->contains(stage)) stages->append(stage);
        });
        QElapsedTimer timer;
        timer.start();
        const QString id = m_service->startJob(profile, {}, options);
        if (id.isEmpty()) return {};
        const bool done = finished.wait(timeoutHours * 3600 * 1000);
        disconnect(connection);
        const ProcessingJob job = m_service->job(id);
        qInfo("Job %s (%s): %s in %lld s; stages: %s", qUtf8Printable(id), qUtf8Printable(profile), qUtf8Printable(jobStateText(job.state)),
              timer.elapsed() / 1000, qUtf8Printable(stages->join(QStringLiteral(", "))));
        for (const Artifact &a : job.outputs)
            qInfo("  %-10s %s  %lld bytes  %s", qUtf8Printable(a.kind), qUtf8Printable(a.file), a.bytes, qUtf8Printable(a.details));
        return done ? id : QString();
    }
    QString output(const QString &id, const QString &kind) const
    {
        for (const Artifact &a : m_service->job(id).outputs)
            if (a.kind == kind) return QDir(m_service->jobDirectory(id)).filePath(a.file);
        return {};
    }

private slots:
    void initTestCase()
    {
        const QString dataset = qEnvironmentVariable("GEOMAP_ODM_DATASET");
        if (dataset.isEmpty()) QSKIP("GEOMAP_ODM_DATASET is not set");
        QVERIFY(QFileInfo(dataset).isDir());
        QVERIFY(m_tmp.isValid());
        m_tmp.setAutoRemove(qEnvironmentVariableIsEmpty("GEOMAP_ODM_KEEP"));
        // The recent-projects list of the test, not the user's.
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settings.path());
        GisEnvironment::initialize(QCoreApplication::applicationDirPath());

        m_projects = std::make_unique<ProjectManager>();
        QVERIFY(m_projects->createProject(QUrl::fromLocalFile(m_tmp.path()), QStringLiteral("Проект Brighton")));
        m_project = m_projects->projectPath();
        m_projects->importFolder(QUrl::fromLocalFile(QFileInfo(dataset).canonicalFilePath()));
        QTRY_VERIFY_WITH_TIMEOUT(!m_projects->busy(), 600000);
        QVERIFY(m_projects->photoCount() >= 2);
        QVERIFY(m_projects->saveProject());
        qInfo("Imported %d photos into %s", m_projects->photoCount(), qUtf8Printable(QDir::toNativeSeparators(m_project)));

        m_engine = new OdmEngine;
        m_service = std::make_unique<ProcessingService>(m_engine);
        m_service->setProject(m_project, m_projects->photoModel());
        m_service->checkEnvironment();
        QTRY_VERIFY_WITH_TIMEOUT(m_service->environmentChecked() && !m_service->checkingEnvironment(), 120000);
        QVERIFY2(m_service->engineAvailable(), qUtf8Printable(m_service->environmentProblems().join('\n')));
        QCOMPARE(m_service->backend(), QStringLiteral("native"));
        qInfo("%s; ODM in %s", qUtf8Printable(m_service->environmentSummary()), qUtf8Printable(QDir::toNativeSeparators(m_engine->nativeRoot())));
    }

    void cancelEndsEveryOdmProcess()
    {
        const QString id = m_service->startJob(QStringLiteral("fast"));
        QVERIFY(!id.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(m_service->job(id).state == JobState::Running && !m_service->job(id).stage.isEmpty(), 300000);
        QTest::qWait(5000);
        const int before = odmProcesses(m_engine->nativeRoot());
        qInfo("ODM processes before cancel: %d", before);
        m_service->cancelJob(id);
        QTRY_COMPARE_WITH_TIMEOUT(m_service->job(id).state, JobState::Cancelled, 120000);
        QTRY_COMPARE_WITH_TIMEOUT(odmProcesses(m_engine->nativeRoot()), 0, 30000);
        QVERIFY(m_service->deleteJob(id));
    }

    void fastProfileProducesAGeoreferencedOrthophoto()
    {
        // The engine reads the photos through links; their bytes must not change.
        QHash<QString, QByteArray> before;
        for (const Photo &p : m_projects->photoModel()->photos()) {
            QFile f(QDir(m_project).filePath(p.file));
            QVERIFY(f.open(QIODevice::ReadOnly));
            before.insert(p.file, QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256));
        }
        QStringList stages;
        const QString id = runToEnd(QStringLiteral("fast"), {}, 2, &stages);
        QVERIFY(!id.isEmpty());
        const ProcessingJob job = m_service->job(id);
        QVERIFY2(job.state == JobState::Completed, qUtf8Printable(job.message));
        QCOMPARE(job.exitCode, 0);
        QCOMPARE(job.backend, QStringLiteral("native"));
        QVERIFY(stages.contains(QStringLiteral("opensfm")));
        const QString ortho = output(id, QStringLiteral("orthophoto"));
        QVERIFY(!ortho.isEmpty());
        const RasterInfo info = RasterInfo::read(ortho);
        QVERIFY2(info.valid && info.crs.isProjected(), qUtf8Printable(info.error));
        QVERIFY(QFileInfo(QDir(m_service->jobDirectory(id)).filePath(QStringLiteral("log.txt"))).size() > 1000);
        for (auto it = before.cbegin(); it != before.cend(); ++it) {
            QFile f(QDir(m_project).filePath(it.key()));
            QVERIFY(f.open(QIODevice::ReadOnly));
            QCOMPARE(QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256), it.value());
        }
        const QVariantMap quality = m_engine->qualityReport(m_service->jobDirectory(id));
        QVERIFY(quality.value(QStringLiteral("alignedImages")).toInt() > 0);
        qInfo("Quality: %d of %d images aligned, reprojection %.2f px, GSD %.2f cm",
              quality.value(QStringLiteral("alignedImages")).toInt(), quality.value(QStringLiteral("totalImages")).toInt(),
              quality.value(QStringLiteral("reprojectionPx")).toDouble(), quality.value(QStringLiteral("gsdCm")).toDouble());
    }

    // Dense cloud, textured model, DSM, DTM (with ground classification), orthophoto and report,
    // each read back and checked against the others.
    void fullProfileProducesConsistentResults()
    {
        if (qEnvironmentVariable("GEOMAP_ODM_FULL") == QLatin1String("0")) QSKIP("GEOMAP_ODM_FULL=0");
        QStringList stages;
        const QVariantMap options{{QStringLiteral("dtm"), true}, {QStringLiteral("pc-classify"), true}};
        const QString id = runToEnd(QStringLiteral("balanced"), options, 6, &stages);
        QVERIFY(!id.isEmpty());
        const ProcessingJob job = m_service->job(id);
        QVERIFY2(job.state == JobState::Completed, qUtf8Printable(job.message));
        for (const QString &stage : {QStringLiteral("openmvs"), QStringLiteral("odm_meshing"), QStringLiteral("mvs_texturing"),
                                     QStringLiteral("odm_dem"), QStringLiteral("odm_orthophoto")})
            QVERIFY2(stages.contains(stage), qUtf8Printable(stage));

        // Rasters: orthophoto RGB(A), DSM and DTM one Float32 band, all in the same projected CRS.
        const RasterInfo ortho = RasterInfo::read(output(id, QStringLiteral("orthophoto")));
        const RasterInfo dsm = RasterInfo::read(output(id, QStringLiteral("dsm")));
        const RasterInfo dtm = RasterInfo::read(output(id, QStringLiteral("dtm")));
        QVERIFY2(ortho.valid && dsm.valid && dtm.valid, "orthophoto, DSM and DTM must be readable");
        QVERIFY(ortho.bands >= 3);
        QCOMPARE(dsm.bands, 1);
        QCOMPARE(dtm.bands, 1);
        QCOMPARE(dsm.dataType, QStringLiteral("Float32"));
        QVERIFY(ortho.crs.isProjected());
        QVERIFY(dsm.crs.isSame(ortho.crs) && dtm.crs.isSame(ortho.crs));
        QVERIFY(dsm.extent.intersects(ortho.extent));
        qInfo("Orthophoto: %s\nDSM: %s\nDTM: %s", qUtf8Printable(ortho.summary()), qUtf8Printable(dsm.summary()), qUtf8Printable(dtm.summary()));

        // The terrain model lies under the surface model: sample both on a grid.
        Dem surface, terrain;
        QVERIFY(surface.open(output(id, QStringLiteral("dsm"))));
        QVERIFY(terrain.open(output(id, QStringLiteral("dtm"))));
        QVector<double> diff;
        const QRectF area = dsm.extent.intersected(dtm.extent);
        for (int i = 1; i < 60; ++i)
            for (int k = 1; k < 60; ++k) {
                const double x = area.left() + area.width() * i / 60.0, y = area.top() + area.height() * k / 60.0;
                const double s = surface.at(x, y), t = terrain.at(x, y);
                if (std::isfinite(s) && std::isfinite(t)) diff << s - t;
            }
        QVERIFY2(diff.size() > 500, qPrintable(QString::number(diff.size())));
        std::sort(diff.begin(), diff.end());
        const double median = diff[diff.size() / 2];
        const auto below = std::count_if(diff.cbegin(), diff.cend(), [](double d) { return d >= -0.5; });
        qInfo("DSM - DTM on %d cells: median %.2f m, 95th percentile %.2f m, DTM above DSM by > 0.5 m: %.1f %%",
              int(diff.size()), median, diff[int(diff.size() * 0.95)], 100.0 * (diff.size() - below) / diff.size());
        QVERIFY(median >= 0.0);
        QVERIFY(below >= 0.95 * diff.size());

        // Point cloud: LAS/LAZ header, points, bounds within the DSM.
        const LasHeader las = readLas(output(id, QStringLiteral("pointcloud")));
        QVERIFY(las.valid);
        qInfo("Point cloud: LAS %d.%d, %llu points, X %.1f..%.1f, Y %.1f..%.1f, Z %.1f..%.1f", las.major, las.minor,
              static_cast<unsigned long long>(las.points), las.minX, las.maxX, las.minY, las.maxY, las.minZ, las.maxZ);
        QVERIFY(las.points > 10000);
        const QRectF cloud(las.minX, las.minY, las.maxX - las.minX, las.maxY - las.minY);
        const QRectF overlap = cloud.intersected(dsm.extent);
        QVERIFY(overlap.width() * overlap.height() > 0.6 * cloud.width() * cloud.height());
        double zMin = 1e9, zMax = -1e9;
        for (float v : surface.values)
            if (std::isfinite(v) && !(surface.hasNoData && std::fabs(v - surface.noData) < 1e-3)) { zMin = qMin(zMin, double(v)); zMax = qMax(zMax, double(v)); }
        QVERIFY2(las.minZ < zMax && las.maxZ > zMin, "cloud heights overlap the DSM heights");

        // Textured model: geometry, texture coordinates, materials and their image files.
        const ObjSummary obj = readObj(output(id, QStringLiteral("mesh")));
        qInfo("Model: %d vertices, %d texture coordinates, %d faces, %d textures", obj.vertices, obj.texcoords, obj.faces, int(obj.textures.size()));
        QVERIFY(obj.vertices > 1000 && obj.faces > 1000 && obj.texcoords > 0);
        QVERIFY(!obj.textures.isEmpty());
        QVERIFY2(obj.missingTextures.isEmpty(), qUtf8Printable(obj.missingTextures.join(", ")));

        // Report.
        QFile report(output(id, QStringLiteral("report")));
        QVERIFY(report.open(QIODevice::ReadOnly));
        QVERIFY(report.read(4) == "%PDF");

        // On the map: the photos choose the UTM zone; the results land in it without reprojection.
        MapService map;
        map.setViewSize(QSizeF(1200, 900));
        map.setProject(m_project, m_projects->photoModel());
        QVERIFY(map.crs().isSame(ortho.crs));
        for (const QString &kind : {QStringLiteral("orthophoto"), QStringLiteral("dsm"), QStringLiteral("dtm")})
            QCOMPARE(map.addLayerFile(output(id, kind)), QString());
        QTRY_VERIFY_WITH_TIMEOUT([&] {
            for (const MapLayer &l : map.layers()) if (l.type == MapLayer::Raster && (l.loading || !l.status.isEmpty())) return false;
            return true;
        }(), 120000);
        map.zoomToAll();
        const QImage image = map.renderMap(QSize(1200, 900), false);
        int painted = 0;
        for (int y = 0; y < image.height(); y += 10)
            for (int x = 0; x < image.width(); x += 10) painted += QColor(image.pixel(x, y)) != QColor(16, 21, 28);
        QVERIFY2(painted > 0.2 * (image.width() / 10) * (image.height() / 10), qPrintable(QString::number(painted)));
        const QString keep = qEnvironmentVariable("GEOMAP_TEST_OUTPUT");
        if (!keep.isEmpty() && QDir().mkpath(keep)) image.save(QDir(keep).filePath(QStringLiteral("odm-results-on-map.png")));
    }

    // The "Высокое качество" profile as the user starts it (ultra features, high point cloud, DSM,
    // DTM, 2 cm orthophoto), to the end — with the automatic repeat of a crashed stage if one
    // happens — then the true orthophoto with its surface and the overlap map of the job.
    void highQualityProfileCompletes()
    {
        if (qEnvironmentVariable("GEOMAP_ODM_FULL") == QLatin1String("0")) QSKIP("GEOMAP_ODM_FULL=0");
        QStringList stages;
        const QString id = runToEnd(QStringLiteral("high"), {}, 8, &stages);
        QVERIFY(!id.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!isActive(m_service->job(id).state), 8 * 3600 * 1000);   // after automatic repeats
        const ProcessingJob job = m_service->job(id);
        const QString log = QString::fromUtf8([&] {
            QFile f(QDir(m_service->jobDirectory(id)).filePath(QStringLiteral("log.txt")));
            return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
        }());
        qInfo("High quality: %s; automatic repeats in the log: %d", qUtf8Printable(job.message),
              int(log.count(QStringLiteral("автоматически (попытка"))));
        QVERIFY2(job.state == JobState::Completed, qUtf8Printable(job.message));
        for (const char *kind : {"orthophoto", "dsm", "dtm", "pointcloud", "mesh", "report"})
            QVERIFY2(!output(id, QString::fromLatin1(kind)).isEmpty(), kind);
        // The true orthophoto starts by itself after a job with a DSM.
        QTRY_VERIFY_WITH_TIMEOUT(!m_service->trueOrthoBusy() && QFileInfo::exists(QDir(m_service->jobDirectory(id)).filePath(QStringLiteral("geomap/true_orthophoto.tif"))), 1800000);
        const QString dir = m_service->jobDirectory(id);
        QVERIFY(QFileInfo::exists(QDir(dir).filePath(QStringLiteral("geomap/true_surface.tif"))));
        const RasterInfo trueOrtho = RasterInfo::read(QDir(dir).filePath(QStringLiteral("geomap/true_orthophoto.tif")));
        QVERIFY2(trueOrtho.valid && trueOrtho.bands == 4 && trueOrtho.crs.isProjected(), qUtf8Printable(trueOrtho.error));
        qInfo("True orthophoto: %s; status: %s", qUtf8Printable(trueOrtho.summary()), qUtf8Printable(m_service->trueOrthoStatus()));
        const overlap::Result o = overlap::fromJob(dir, QDir(m_tmp.path()).filePath(QStringLiteral("overlap.tif")));
        QVERIFY2(o.ok(), qUtf8Printable(o.error));
        qInfo("Overlap: median %.0f photos, >= 3: %.1f %%, >= 5: %.1f %%, >= 9: %.1f %%", o.median, o.share3 * 100, o.share5 * 100, o.share9 * 100);
        QVERIFY(o.median >= 3);
    }

    // Chunks on real photos: the survey split in two overlapping halves by the photo positions,
    // each half processed on its own (fast profile), the two orthophotos merged into one mosaic
    // that covers more than either half.
    void chunksSplitProcessAndMerge()
    {
        ChunkService chunks(m_projects.get(), m_service.get());
        QCOMPARE(chunks.count(), 1);
        const QVariantMap split = chunks.split(chunks.active(), {{"mode", "grid"}, {"rows", 1}, {"cols", 2}, {"overlap", 30}, {"minPhotos", 5}});
        QVERIFY2(split.value("ok").toBool(), qUtf8Printable(split.value("error").toString()));
        const QStringList parts = split.value("created").toStringList();
        QCOMPARE(parts.size(), 2);
        qInfo("Split: %s; %s", qUtf8Printable(chunks.chunk(parts[0])->name + QStringLiteral(" %1 photos, ").arg(chunks.chunk(parts[0])->photos.size())
                                              + chunks.chunk(parts[1])->name + QStringLiteral(" %1 photos").arg(chunks.chunk(parts[1])->photos.size())),
              qUtf8Printable(split.value("notes").toStringList().join(QStringLiteral("; "))));
        QStringList stages;
        QVector<double> coverage;
        for (const QString &part : parts) {
            QVERIFY(chunks.setActive(part));
            const QString id = runToEnd(QStringLiteral("fast"), {}, 2, &stages);
            QVERIFY(!id.isEmpty());
            const ProcessingJob job = m_service->job(id);
            QVERIFY2(job.state == JobState::Completed, qUtf8Printable(job.message));
            QCOMPARE(job.chunk, part);
            QCOMPARE(job.images.size(), chunks.chunk(part)->photos.size());
        }
        QSignalSpy finished(&chunks, &ChunkService::mergeFinished);
        QCOMPARE(chunks.mergeResults(parts, QStringLiteral("orthophoto")), QString());
        QVERIFY(finished.wait(900000));
        QVERIFY2(finished.first().at(0).toBool(), qUtf8Printable(finished.first().at(2).toString()));
        const QString mosaicPath = finished.first().at(1).toString();
        qInfo("%s", qUtf8Printable(finished.first().at(2).toString()));
        // Area with data (alpha > 0), m²: the mosaic covers the union of the halves.
        auto area = [](const QString &path) {
            GDALDatasetH ds = GDALOpen(QDir::toNativeSeparators(path).toUtf8().constData(), GA_ReadOnly);
            if (!ds) return 0.0;
            double gt[6];
            GDALGetGeoTransform(ds, gt);
            const int w = GDALGetRasterXSize(ds), h = GDALGetRasterYSize(ds), step = 4;
            std::vector<unsigned char> line(static_cast<size_t>(w));
            qint64 count = 0;
            GDALRasterBandH alpha = GDALGetRasterBand(ds, GDALGetRasterCount(ds));
            for (int y = 0; y < h; y += step) {
                GDALRasterIO(alpha, GF_Read, 0, y, w, 1, line.data(), w, 1, GDT_Byte, 0, 0);
                for (int x = 0; x < w; x += step) count += line[size_t(x)] > 0 ? 1 : 0;
            }
            GDALClose(ds);
            return double(count) * step * step * std::fabs(gt[1] * gt[5]);
        };
        double largest = 0;
        for (const QVariant &v : chunks.resultsOf(parts, QStringLiteral("orthophoto"))) {
            const double a = area(v.toMap().value("path").toString());
            qInfo("  %s: %.0f m² with data", qUtf8Printable(v.toMap().value("name").toString()), a);
            largest = std::max(largest, a);
        }
        const double merged = area(mosaicPath);
        qInfo("  mosaic: %.0f m² with data", merged);
        QVERIFY(merged > largest * 1.05);
        const QString keep = qEnvironmentVariable("GEOMAP_TEST_OUTPUT");
        if (!keep.isEmpty()) QFile::copy(mosaicPath, QDir(keep).filePath(QStringLiteral("chunks-mosaic.tif")));
    }
};

QTEST_MAIN(OdmIntegrationTest)
#include "test_odm_integration.moc"
