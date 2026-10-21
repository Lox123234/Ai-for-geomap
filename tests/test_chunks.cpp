#include <QtTest>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <cmath>
#include <functional>
#include <cpl_string.h>
#include <gdal.h>
#include <ogr_srs_api.h>
#include "ChunkService.h"
#include "Crs.h"
#include "GisEnvironment.h"
#include "MockEngine.h"
#include "Mosaic.h"
#include "PhotoMasks.h"
#include "PhotoModel.h"
#include "PhotoQualityService.h"
#include "ProcessingService.h"
#include "ProcessingTypes.h"
#include "ProjectManager.h"
#include "TestImages.h"

namespace {

struct Shot
{
    QString id;
    double lat = 0, lon = 0;
    QString takenAt;
    bool gps = true;
};

// A project folder written directly: photos/<id>.jpg and geomap.json with their positions and times.
bool makeProject(const QString &dir, const QVector<Shot> &shots)
{
    if (!QDir().mkpath(dir + QStringLiteral("/photos"))) return false;
    QJsonArray photos;
    for (const Shot &s : shots) {
        const QString file = QStringLiteral("photos/%1.jpg").arg(s.id);
        QImage image(16, 12, QImage::Format_RGB32);
        image.fill(Qt::darkGreen);
        if (!image.save(dir + QLatin1Char('/') + file, "JPG")) return false;
        QJsonObject o{{"id", s.id}, {"file", file}, {"name", s.id + QStringLiteral(".JPG")}, {"sourcePath", QString()},
                      {"cameraMake", "Test"}, {"cameraModel", "Cam"}, {"takenAt", s.takenAt}, {"bytes", 100},
                      {"width", 16}, {"height", 12}, {"hasGps", s.gps}};
        if (s.gps) {
            o.insert("latitude", s.lat);
            o.insert("longitude", s.lon);
        }
        photos.append(o);
    }
    QFile f(dir + QStringLiteral("/geomap.json"));
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QJsonDocument(QJsonObject{{"schemaVersion", 1}, {"name", "Блоки"}, {"createdAt", "2026-10-16T10:00:00Z"},
                                      {"photos", photos}}).toJson());
    return true;
}

// A 4 × 4 grid of shots 50 m apart around (50° N, 10° E), north row first.
QVector<Shot> gridShots()
{
    QVector<Shot> shots;
    const double kx = 111320.0 * std::cos(50.0 * M_PI / 180.0), ky = 110574.0;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            Shot s;
            s.id = QStringLiteral("p%1%2").arg(r).arg(c);
            s.lon = 10.0 + (c * 50.0) / kx;
            s.lat = 50.0 - (r * 50.0) / ky;
            s.takenAt = QStringLiteral("2026:10:16 10:%1:00").arg(r * 4 + c, 2, 10, QLatin1Char('0'));
            shots << s;
        }
    return shots;
}

bool writeRaster(const QString &path, int w, int h, int bands, GDALDataType type, const double gt[6], int epsg,
                 const std::function<double(int, int, int)> &value, double noData = std::nan(""), bool alpha = false)
{
    char **options = nullptr;
    if (alpha) {
        options = CSLSetNameValue(options, "PHOTOMETRIC", "RGB");
        options = CSLSetNameValue(options, "ALPHA", "YES");
    }
    GDALDatasetH ds = GDALCreate(GDALGetDriverByName("GTiff"), QDir::toNativeSeparators(path).toUtf8().constData(), w, h, bands, type, options);
    CSLDestroy(options);
    if (!ds) return false;
    double transform[6];
    std::copy(gt, gt + 6, transform);
    GDALSetGeoTransform(ds, transform);
    OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
    OSRImportFromEPSG(srs, epsg);
    GDALSetSpatialRef(ds, srs);
    OSRDestroySpatialReference(srs);
    QVector<double> line(w);
    for (int b = 1; b <= bands; ++b) {
        GDALRasterBandH band = GDALGetRasterBand(ds, b);
        if (!std::isnan(noData)) GDALSetRasterNoDataValue(band, noData);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) line[x] = value(b, x, y);
            if (GDALRasterIO(band, GF_Write, 0, y, w, 1, line.data(), w, 1, GDT_Float64, 0, 0) != CE_None) {
                GDALClose(ds);
                return false;
            }
        }
    }
    GDALClose(ds);
    return true;
}

// A solid RGBA orthophoto in UTM 32N.
bool orthophoto(const QString &path, double x0, double y1, int w, int h, const QColor &colour)
{
    const double gt[6] = {x0, 1, 0, y1, 0, -1};
    return writeRaster(path, w, h, 4, GDT_Byte, gt, 32632,
                       [&](int b, int, int) { return b == 1 ? colour.red() : b == 2 ? colour.green() : b == 3 ? colour.blue() : 255; },
                       std::nan(""), true);
}

struct Pixel
{
    double v[4] = {0, 0, 0, 0};
    bool ok = false;
};

// Pixel of a raster at map coordinates (of the raster's CRS).
Pixel sample(const QString &path, double x, double y)
{
    Pixel p;
    GDALDatasetH ds = GDALOpen(QDir::toNativeSeparators(path).toUtf8().constData(), GA_ReadOnly);
    if (!ds) return p;
    double gt[6];
    GDALGetGeoTransform(ds, gt);
    const int px = int(std::floor((x - gt[0]) / gt[1])), py = int(std::floor((y - gt[3]) / gt[5]));
    if (px >= 0 && py >= 0 && px < GDALGetRasterXSize(ds) && py < GDALGetRasterYSize(ds)) {
        for (int b = 0; b < std::min(4, GDALGetRasterCount(ds)); ++b)
            GDALRasterIO(GDALGetRasterBand(ds, b + 1), GF_Read, px, py, 1, 1, &p.v[b], 1, 1, GDT_Float64, 0, 0);
        p.ok = true;
    }
    GDALClose(ds);
    return p;
}

QPointF lonLat(double e, double n)
{
    double x = e, y = n;
    CoordinateTransform(Crs::fromEpsg(32632), Crs::wgs84()).transform(x, y);
    return {x, y};
}

} // namespace

class ChunkTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QVERIFY2(GisEnvironment::initialize(QCoreApplication::applicationDirPath()), qPrintable(GisEnvironment::problems().join('\n')));
    }

    // A project before chunks has one chunk with every photo, and no chunks.json until chunks are used.
    void oneChunkUntilChunksAreUsed()
    {
        QTemporaryDir tmp;
        const QString dir = tmp.filePath(QStringLiteral("Проект"));
        QVERIFY(makeProject(dir, gridShots()));
        {
            ProjectManager projects;
            ChunkService chunks(&projects, nullptr);
            QVERIFY(projects.openProject(QUrl::fromLocalFile(dir)));
            QCOMPARE(chunks.count(), 1);
            QCOMPARE(chunks.activeName(), QStringLiteral("Блок 1"));
            QCOMPARE(chunks.activePhotoCount(), 16);
            QCOMPARE(chunks.photoModel()->rowCount(), 16);
            QVERIFY(!QFileInfo::exists(dir + QStringLiteral("/chunks.json")));
            // A new chunk is empty and active; the photo grid follows the active chunk.
            const QString c2 = chunks.create();
            QCOMPARE(chunks.active(), c2);
            QCOMPARE(chunks.activeName(), QStringLiteral("Блок 2"));
            QCOMPARE(chunks.photoModel()->rowCount(), 0);
            QVERIFY(QFileInfo::exists(dir + QStringLiteral("/chunks.json")));
            QVERIFY(chunks.setActive(QStringLiteral("c1")));
            QCOMPARE(chunks.photoModel()->rowCount(), 16);
            // The grid's rows map to the project's rows (selection).
            const QModelIndex fifth = chunks.photoModel()->index(4, 0);
            QCOMPARE(fifth.data(PhotoModel::RowRole).toInt(), 4);
            QVERIFY(chunks.setActive(c2));
        }
        ProjectManager projects;
        ChunkService chunks(&projects, nullptr);
        QVERIFY(projects.openProject(QUrl::fromLocalFile(dir)));
        QCOMPARE(chunks.count(), 2);
        QCOMPARE(chunks.active(), QStringLiteral("c2"));
        QCOMPARE(chunks.chunk(QStringLiteral("c1"))->photos.size(), 16);
    }

    // Imported photos join the active chunk; removed ones leave every chunk.
    void photosFollowTheProject()
    {
        QTemporaryDir tmp;
        const QString dir = tmp.filePath(QStringLiteral("p"));
        QVERIFY(makeProject(dir, gridShots().mid(0, 3)));
        ProjectManager projects;
        ChunkService chunks(&projects, nullptr);
        QVERIFY(projects.openProject(QUrl::fromLocalFile(dir)));
        const QString c2 = chunks.create(QStringLiteral("Новые"));
        const QString file = tmp.filePath(QStringLiteral("new.jpg"));
        QVERIFY(TestImages::writeJpegWithGps(file));
        projects.importFiles({QUrl::fromLocalFile(file)});
        QTRY_VERIFY_WITH_TIMEOUT(!projects.busy() && projects.photoCount() == 4, 20000);
        QTRY_COMPARE(chunks.chunk(c2)->photos.size(), 1);
        QCOMPARE(chunks.chunk(QStringLiteral("c1"))->photos.size(), 3);
        QCOMPARE(chunks.photoModel()->rowCount(), 1);
        // Removing the photo from the project removes it from its chunk.
        projects.selectPhoto(chunks.photoModel()->index(0, 0).data(PhotoModel::RowRole).toInt());
        QVERIFY(projects.removeSelectedPhoto());
        QTRY_COMPARE(chunks.chunk(c2)->photos.size(), 0);
        QCOMPARE(projects.photoCount(), 3);
    }

    void duplicateRenameRemove()
    {
        QTemporaryDir tmp;
        const QString dir = tmp.filePath(QStringLiteral("p"));
        QVERIFY(makeProject(dir, gridShots()));
        ProjectManager projects;
        ChunkService chunks(&projects, nullptr);
        QVERIFY(projects.openProject(QUrl::fromLocalFile(dir)));
        QCOMPARE(chunks.remove(QStringLiteral("c1")), QStringLiteral("Последний блок проекта удалить нельзя."));
        const QString copy = chunks.duplicate(QStringLiteral("c1"));
        QCOMPARE(chunks.chunk(copy)->name, QStringLiteral("Блок 1 (копия)"));
        QCOMPARE(chunks.chunk(copy)->photos, chunks.chunk(QStringLiteral("c1"))->photos);
        QVERIFY(!chunks.rename(copy, QStringLiteral("блок 1")).isEmpty());   // names are unique, case aside
        QVERIFY(!chunks.rename(copy, QStringLiteral("  ")).isEmpty());
        QCOMPARE(chunks.rename(copy, QStringLiteral("Север")), QString());
        QCOMPARE(chunks.nameOf(copy), QStringLiteral("Север"));
        // A photo can leave a chunk only if another chunk keeps it.
        const QString p = QStringLiteral("p00");
        QCOMPARE(chunks.removePhoto(copy, p), QString());
        QVERIFY(!chunks.removePhoto(QStringLiteral("c1"), p).isEmpty());
        QCOMPARE(chunks.addPhoto(copy, p), QString());
        QCOMPARE(chunks.chunk(copy)->photos.first(), p);   // in the project's order
        // A chunk with photos of its own: they stay in the project, in the first chunk.
        const QString empty = chunks.create(QStringLiteral("Пустой"));
        QCOMPARE(chunks.addPhoto(empty, p), QString());
        QCOMPARE(chunks.removePhoto(QStringLiteral("c1"), p), QString());
        QCOMPARE(chunks.removePhoto(copy, p), QString());
        QCOMPARE(chunks.chunk(QStringLiteral("c1"))->photos.size(), 15);
        QCOMPARE(chunks.remove(empty), QString());
        QCOMPARE(chunks.chunk(QStringLiteral("c1"))->photos.size(), 16);
        QCOMPARE(chunks.active(), QStringLiteral("c1"));
        QCOMPARE(chunks.count(), 2);
    }

    // A grid over the photo positions, with and without overlap; the cells are kept for the mosaic.
    void splitByGrid()
    {
        QTemporaryDir tmp;
        const QString dir = tmp.filePath(QStringLiteral("p"));
        QVector<Shot> shots = gridShots();
        Shot noGps;
        noGps.id = QStringLiteral("nogps");
        noGps.gps = false;
        shots << noGps;
        QVERIFY(makeProject(dir, shots));
        ProjectManager projects;
        ChunkService chunks(&projects, nullptr);
        QVERIFY(projects.openProject(QUrl::fromLocalFile(dir)));
        QVariantMap options{{"mode", "grid"}, {"rows", 2}, {"cols", 2}, {"overlap", 0}};
        QVariantMap preview = chunks.splitPreview(QStringLiteral("c1"), options);
        QVERIFY2(preview.value("ok").toBool(), qPrintable(preview.value("error").toString()));
        QVERIFY2(preview.value("parts").toList().size() == 4, qPrintable(preview.value("notes").toStringList().join(" | ")
                                                                        + QStringLiteral(" parts %1").arg(preview.value("parts").toList().size())));
        QCOMPARE(preview.value("noPosition").toInt(), 1);
        for (const QVariant &part : preview.value("parts").toList()) QCOMPARE(part.toMap().value("photos").toInt(), 4);
        // 40 % of a 75 m cell = 30 m: the neighbouring column and row (50 m away) join each part.
        options["overlap"] = 40;
        const QVariantMap result = chunks.split(QStringLiteral("c1"), options);
        QVERIFY2(result.value("ok").toBool(), qPrintable(result.value("error").toString()));
        const QStringList created = result.value("created").toStringList();
        QCOMPARE(created.size(), 4);
        QCOMPARE(chunks.count(), 5);
        QCOMPARE(chunks.active(), QStringLiteral("c1"));   // the source stays, as it was
        QCOMPARE(chunks.chunk(QStringLiteral("c1"))->photos.size(), 17);
        const ChunkService::Chunk *nw = chunks.chunk(created.first());
        QCOMPARE(nw->name, QStringLiteral("Блок 1 · ряд 1, столбец 1"));
        QCOMPARE(nw->photos.size(), 9);
        for (const char *id : {"p00", "p01", "p02", "p10", "p11", "p12", "p20", "p21", "p22"}) QVERIFY2(nw->photos.contains(QLatin1String(id)), id);
        QVERIFY(!nw->photos.contains(QStringLiteral("p03")) && !nw->photos.contains(QStringLiteral("nogps")));
        // The cell (without the overlap) covers its own quadrant: p00 and p11 inside, p22 outside.
        QCOMPARE(nw->cell.size(), 4);
        const QRectF cell = QPolygonF(nw->cell).boundingRect();
        for (const Shot &s : gridShots()) {
            const bool inside = cell.adjusted(-1e-9, -1e-9, 1e-9, 1e-9).contains(QPointF(s.lon, s.lat));
            const bool own = s.id == QLatin1String("p00") || s.id == QLatin1String("p01") || s.id == QLatin1String("p10") || s.id == QLatin1String("p11");
            QVERIFY2(inside == own, qPrintable(s.id));
        }
        // Persisted with the cells.
        ProjectManager again;
        ChunkService reread(&again, nullptr);
        projects.closeProject();
        QVERIFY(again.openProject(QUrl::fromLocalFile(dir)));
        QCOMPARE(reread.count(), 5);
        QCOMPARE(reread.chunk(created.first())->cell.size(), 4);
        // Too few photos per part: skipped, and nothing to split.
        const QVariantMap sparse = reread.splitPreview(QStringLiteral("c1"), {{"mode", "grid"}, {"rows", 4}, {"cols", 4}, {"overlap", 0}, {"minPhotos", 2}});
        QVERIFY(!sparse.value("ok").toBool());
        QCOMPARE(sparse.value("skipped").toInt(), 16);
    }

    // Flights: pauses between consecutive shots.
    void splitByFlights()
    {
        QTemporaryDir tmp;
        const QString dir = tmp.filePath(QStringLiteral("p"));
        QVector<Shot> shots;
        const QStringList times = {"10:00:00", "10:00:05", "10:00:10", "10:12:00", "10:12:05", "10:12:10", "10:12:15", "10:30:00", "10:30:05"};
        for (int i = 0; i < times.size(); ++i)
            shots << Shot{QStringLiteral("f%1").arg(i), 50.0, 10.0 + i * 1e-4, QStringLiteral("2026:10:16 ") + times[i], true};
        QVERIFY(makeProject(dir, shots));
        ProjectManager projects;
        ChunkService chunks(&projects, nullptr);
        QVERIFY(projects.openProject(QUrl::fromLocalFile(dir)));
        QVariantMap r = chunks.split(QStringLiteral("c1"), {{"mode", "flights"}, {"gapMinutes", 5}, {"minPhotos", 2}});
        QVERIFY2(r.value("ok").toBool(), qPrintable(r.value("error").toString()));
        const QStringList created = r.value("created").toStringList();
        QCOMPARE(created.size(), 3);
        QCOMPARE(chunks.chunk(created[0])->photos.size(), 3);
        QCOMPARE(chunks.chunk(created[1])->photos.size(), 4);
        QCOMPARE(chunks.chunk(created[2])->photos.size(), 2);
        QCOMPARE(chunks.chunk(created[1])->name, QStringLiteral("Блок 1 · полёт 2"));
        // Merged back: every photo once, in the project's order.
        const QString merged = chunks.mergePhotos(created, QStringLiteral("Все полёты"));
        QCOMPARE(chunks.chunk(merged)->photos, chunks.chunk(QStringLiteral("c1"))->photos);
        QCOMPARE(chunks.active(), merged);
        // One flight only with a long pause allowed.
        r = chunks.splitPreview(QStringLiteral("c1"), {{"mode", "flights"}, {"gapMinutes", 60}});
        QVERIFY(!r.value("ok").toBool() && r.value("error").toString().contains(QStringLiteral("одним полётом")));
    }

    // Jobs process the active chunk's photos and belong to it; jobs from before chunks join the first one.
    void processingFollowsTheActiveChunk()
    {
        QTemporaryDir tmp;
        const QString dir = tmp.filePath(QStringLiteral("p"));
        QVERIFY(makeProject(dir, gridShots()));
        ProjectManager projects;
        auto record = std::make_shared<MockEngine::Record>();
        auto *engine = new MockEngine(record);
        ProcessingService processing(engine);
        processing.setDiskReserveBytes(0);
        QObject::connect(&projects, &ProjectManager::projectDirectoryChanged, &processing,
                         [&](const QString &d) { processing.setProject(d, projects.photoModel()); });
        QVERIFY(projects.openProject(QUrl::fromLocalFile(dir)));
        processing.checkEnvironment();
        QTRY_VERIFY(processing.engineAvailable());
        // A job before chunks existed: no chunk.
        const QString old = processing.startJob(QStringLiteral("fast"));
        QVERIFY(!old.isEmpty());
        QTRY_COMPARE(record->started.size(), 1);
        QVERIFY(processing.job(old).chunk.isEmpty());
        QCOMPARE(processing.job(old).images.size(), 16);
        engine->finish(JobOutcome::Failed, 1, QStringLiteral("x"));
        QTRY_VERIFY(!processing.active());

        ChunkService chunks(&projects, &processing);
        const QVariantMap r = chunks.split(QStringLiteral("c1"), {{"mode", "grid"}, {"rows", 1}, {"cols", 2}, {"overlap", 0}});
        QVERIFY(r.value("ok").toBool());
        QCOMPARE(processing.job(old).chunk, QStringLiteral("c1"));   // adopted when chunks were first written
        {
            QFile f(dir + QStringLiteral("/jobs/") + old + QStringLiteral("/job.json"));
            QVERIFY(f.open(QIODevice::ReadOnly));
            QCOMPARE(QJsonDocument::fromJson(f.readAll()).object().value("chunk").toString(), QStringLiteral("c1"));
        }
        const QString west = r.value("created").toStringList().first();
        QVERIFY(chunks.setActive(west));
        const QVariantMap check = processing.preflight(QStringLiteral("fast"));
        QCOMPARE(check.value("images").toInt(), 8);
        const QString id = processing.startJob(QStringLiteral("fast"));
        QVERIFY(!id.isEmpty());
        QCOMPARE(processing.job(id).chunk, west);
        QCOMPARE(processing.job(id).images.size(), 8);
        QTRY_COMPARE(record->started.size(), 2);
        QCOMPARE(record->lastImages.size(), 8);
        engine->finish(JobOutcome::Failed, 1, QStringLiteral("x"));
        QTRY_VERIFY(!processing.active());
        QVERIFY(processing.jobInChunk(processing.job(id)));
        QVERIFY(!processing.jobInChunk(processing.job(old)));
        int jobs = 0;
        for (const QVariant &c : chunks.chunksForQml()) if (c.toMap().value("id") == west) jobs = c.toMap().value("jobs").toInt();
        QCOMPARE(jobs, 1);
    }

    // Two orthophotos overlapping 50 m: the seam in the middle of the overlap (or between the cells
    // of a split), blended over a metre on each side; empty where neither has data.
    void mosaicOfOrthophotos()
    {
        QTemporaryDir tmp;
        const QString a = tmp.filePath(QStringLiteral("a.tif")), b = tmp.filePath(QStringLiteral("b.tif"));
        const double E = 500000, N = 5600000;
        QVERIFY(orthophoto(a, E, N + 100, 200, 100, QColor(220, 20, 20)));         // x 0..200, y 0..100
        QVERIFY(orthophoto(b, E + 150, N + 120, 200, 100, QColor(20, 20, 220)));   // x 150..350, y 20..120
        const QString out = tmp.filePath(QStringLiteral("mosaics/ortho.tif"));
        mosaic::Result r = mosaic::build({{a, QStringLiteral("A"), {}}, {b, QStringLiteral("B"), {}}}, out);
        QVERIFY2(r.ok(), qPrintable(r.error));
        QCOMPARE(r.width, 350);
        QCOMPARE(r.height, 120);
        QCOMPARE(r.bands, 4);
        QVERIFY(!QFileInfo::exists(tmp.filePath(QStringLiteral("mosaics/ortho.partial.tif"))));
        // Centres at x 100 (A) and 250 (B): the seam at 175 (bisector).
        auto at = [&](double x, double y) { return sample(out, E + x, N + y); };
        Pixel p = at(100, 50);
        QVERIFY(p.ok && p.v[0] > 200 && p.v[2] < 40 && p.v[3] == 255);
        p = at(170, 50);   // inside both, nearer A
        QVERIFY2(p.v[0] > 200, qPrintable(QString::number(p.v[0])));
        p = at(180, 50);   // nearer B
        QVERIFY2(p.v[2] > 200, qPrintable(QString::number(p.v[2])));
        // B's centre is also 20 m north of A's: at y 50 the bisector crosses x 176.3.
        p = at(176.3, 50);   // by the seam: both
        QVERIFY2(p.v[0] > 50 && p.v[2] > 50, qPrintable(QStringLiteral("%1 %2").arg(p.v[0]).arg(p.v[2])));
        p = at(160, 10);   // only A has data here (B starts at y 20)
        QVERIFY(p.v[0] > 200 && p.v[3] == 255);
        p = at(300, 5);    // neither
        QCOMPARE(p.v[3], 0.0);
        // Chunk cells move the seam: A's cell x 0..160, B's 160..350 → centres 80 and 255, seam at 167.5.
        const QVector<QPointF> cellA = {lonLat(E, N + 120), lonLat(E + 160, N + 120), lonLat(E + 160, N), lonLat(E, N)};
        const QVector<QPointF> cellB = {lonLat(E + 160, N + 120), lonLat(E + 350, N + 120), lonLat(E + 350, N), lonLat(E + 160, N)};
        r = mosaic::build({{a, QStringLiteral("A"), cellA}, {b, QStringLiteral("B"), cellB}}, out);
        QVERIFY2(r.ok(), qPrintable(r.error));
        QVERIFY(at(165, 50).v[0] > 200);
        QVERIFY(at(171, 50).v[2] > 200);
        // Different kinds do not mix.
        const QString dem = tmp.filePath(QStringLiteral("dem.tif"));
        const double gt[6] = {E, 1, 0, N + 100, 0, -1};
        QVERIFY(writeRaster(dem, 50, 50, 1, GDT_Float32, gt, 32632, [](int, int, int) { return 100.0; }, -9999));
        r = mosaic::build({{a, QStringLiteral("A"), {}}, {dem, QStringLiteral("DEM"), {}}}, out);
        QVERIFY(!r.ok() && r.error.contains(QStringLiteral("другого вида")));
    }

    // Heights: nodata of the nearer model is filled by the other; another CRS is reprojected.
    void mosaicOfHeights()
    {
        QTemporaryDir tmp;
        const QString a = tmp.filePath(QStringLiteral("a.tif")), b = tmp.filePath(QStringLiteral("b.tif"));
        const double E = 500000, N = 5600000;
        const double gtA[6] = {E, 0.5, 0, N + 100, 0, -0.5};   // x 0..100, y 0..100, 0.5 m
        QVERIFY(writeRaster(a, 200, 200, 1, GDT_Float32, gtA, 32632, [](int, int x, int y) {
            return (x >= 120 && x < 140 && y >= 80 && y < 120) ? -9999.0 : 10.0;   // a hole at x 60..70 m
        }, -9999));
        // B in Web Mercator over x 50..150 m: 20 m everywhere.
        double bx0 = E + 50, by1 = N + 100, bx1 = E + 150, by0 = N;
        CoordinateTransform toMerc(Crs::fromEpsg(32632), Crs::fromEpsg(3857));
        QVERIFY(toMerc.transform(bx0, by1));
        QVERIFY(toMerc.transform(bx1, by0));
        const int bw = 160, bh = 160;
        const double gtB[6] = {bx0, (bx1 - bx0) / bw, 0, by1, 0, -(by1 - by0) / bh};
        QVERIFY(writeRaster(b, bw, bh, 1, GDT_Float32, gtB, 3857, [](int, int, int) { return 20.0; }, -9999));
        const QString out = tmp.filePath(QStringLiteral("dem.tif"));
        const mosaic::Result r = mosaic::build({{a, QStringLiteral("A"), {}}, {b, QStringLiteral("B"), {}}}, out);
        QVERIFY2(r.ok(), qPrintable(r.error));
        QCOMPARE(r.bands, 1);
        QVERIFY(!r.notes.isEmpty() && r.notes.first().contains(QStringLiteral("перепроецирован")));
        QVERIFY(std::abs(r.resolution - 0.5) < 1e-9);
        auto h = [&](double x, double y) { return sample(out, E + x, N + y).v[0]; };
        QVERIFY(std::abs(h(20, 50) - 10.0) < 1e-3);
        QVERIFY2(std::abs(h(65, 50) - 20.0) < 1e-3, qPrintable(QString::number(h(65, 50))));   // A's hole, nearer A's centre: from B
        QVERIFY(std::abs(h(55, 30) - 10.0) < 1e-3);   // nearer A, A has data
        QVERIFY(std::abs(h(140, 50) - 20.0) < 1e-3);
        GDALDatasetH ds = GDALOpen(QDir::toNativeSeparators(out).toUtf8().constData(), GA_ReadOnly);
        QVERIFY(ds);
        int has = 0;
        QCOMPARE(GDALGetRasterNoDataValue(GDALGetRasterBand(ds, 1), &has), -9999.0);
        QVERIFY(has);
        GDALClose(ds);
    }

    // The chunks' newest orthophotos merged from the project: the jobs' outputs, the cells of the split.
    void mergeResultsOfChunks()
    {
        QTemporaryDir tmp;
        const QString dir = tmp.filePath(QStringLiteral("p"));
        QVERIFY(makeProject(dir, gridShots()));
        ProjectManager projects;
        auto record = std::make_shared<MockEngine::Record>();
        auto *engine = new MockEngine(record);
        ProcessingService processing(engine);
        processing.setDiskReserveBytes(0);
        processing.setAutoTrueOrthophoto(false);
        QObject::connect(&projects, &ProjectManager::projectDirectoryChanged, &processing,
                         [&](const QString &d) { processing.setProject(d, projects.photoModel()); });
        ChunkService chunks(&projects, &processing);
        QVERIFY(projects.openProject(QUrl::fromLocalFile(dir)));
        processing.checkEnvironment();
        QTRY_VERIFY(processing.engineAvailable());
        const QStringList parts = chunks.split(QStringLiteral("c1"), {{"mode", "grid"}, {"rows", 1}, {"cols", 2}, {"overlap", 10}})
                                      .value("created").toStringList();
        QCOMPARE(parts.size(), 2);
        QVERIFY(chunks.mergeResults(parts, QStringLiteral("orthophoto")).contains(QStringLiteral("Нет готового результата")));
        // The photos' place in UTM 32N: the north-west shot (10° E, 50° N); the grid goes 150 m east and south.
        double E = 10.0, N = 50.0;
        QVERIFY(CoordinateTransform(Crs::wgs84(), Crs::fromEpsg(32632)).transform(E, N));
        // West x −60..120 m (extent centre 30), east 30..180 (centre 105): by extents the seam would be
        // at 67.5 m, by the split's cells (0..75, 75..150) at 75.
        for (int k = 0; k < 2; ++k) {
            QVERIFY(chunks.setActive(parts[k]));
            const QString id = processing.startJob(QStringLiteral("fast"));
            QVERIFY(!id.isEmpty());
            QTRY_COMPARE(engine->running(), id);
            QVERIFY(QDir().mkpath(processing.jobDirectory(id) + QStringLiteral("/out")));
            QVERIFY(orthophoto(processing.jobDirectory(id) + QStringLiteral("/out/ortho.tif"), k == 0 ? E - 60 : E + 30, N + 30,
                               k == 0 ? 180 : 150, 210, k == 0 ? QColor(200, 10, 10) : QColor(10, 10, 200)));
            engine->finish(JobOutcome::Succeeded, 0, QString());
            QTRY_VERIFY(!processing.active());
            QCOMPARE(processing.job(id).state, JobState::Completed);
        }
        const QVariantList results = chunks.resultsOf(parts, QStringLiteral("orthophoto"));
        QCOMPARE(results.size(), 2);
        for (const QVariant &v : results) QVERIFY(!v.toMap().value("path").toString().isEmpty());
        QSignalSpy finished(&chunks, &ChunkService::mergeFinished);
        QCOMPARE(chunks.mergeResults(parts, QStringLiteral("orthophoto")), QString());
        QVERIFY(chunks.merging());
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 60000);
        QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(2).toString()));
        const QString path = finished.first().at(1).toString();
        QVERIFY(path.startsWith(dir + QStringLiteral("/mosaics/orthophoto-")));
        QVERIFY(!chunks.merging());
        QVERIFY(sample(path, E + 20, N - 60).v[0] > 150);
        QVERIFY(sample(path, E + 160, N - 60).v[2] > 150);
        // The seam follows the cells: 71 m is still the west chunk's.
        const Pixel p71 = sample(path, E + 71, N - 60), p79 = sample(path, E + 79, N - 60);
        QVERIFY2(p71.v[0] > 150, qPrintable(QStringLiteral("%1 %2").arg(p71.v[0]).arg(p71.v[2])));
        QVERIFY2(p79.v[2] > 150, qPrintable(QStringLiteral("%1 %2").arg(p79.v[0]).arg(p79.v[2])));
    }

    // Switched-off photos stay in the chunk and its photo list but not in its processing; kept in
    // chunks.json, by a split and by a merge.
    void switchedOffPhotos()
    {
        QTemporaryDir tmp;
        const QString dir = tmp.filePath(QStringLiteral("p"));
        QVERIFY(makeProject(dir, gridShots()));
        ProjectManager projects;
        auto record = std::make_shared<MockEngine::Record>();
        auto *engine = new MockEngine(record);
        ProcessingService processing(engine);
        processing.setDiskReserveBytes(0);
        QObject::connect(&projects, &ProjectManager::projectDirectoryChanged, &processing,
                         [&](const QString &d) { processing.setProject(d, projects.photoModel()); });
        ChunkService chunks(&projects, &processing);
        QVERIFY(projects.openProject(QUrl::fromLocalFile(dir)));
        processing.checkEnvironment();
        QTRY_VERIFY(processing.engineAvailable());

        QCOMPARE(chunks.setPhotosEnabled({QStringLiteral("p00"), QStringLiteral("p33"), QStringLiteral("none")}, false), QString());
        QCOMPARE(chunks.activeDisabledCount(), 2);
        QVERIFY(!chunks.photoEnabled(QStringLiteral("p00")) && chunks.photoEnabled(QStringLiteral("p01")));
        QCOMPARE(chunks.photoModel()->rowCount(), 16);   // still listed
        QCOMPARE(chunks.photoModel()->roleNames().value(ChunkPhotoModel::EnabledRole), QByteArray("photoEnabled"));
        QVERIFY(!chunks.photoModel()->index(0, 0).data(ChunkPhotoModel::EnabledRole).toBool());
        QVERIFY(chunks.photoModel()->index(1, 0).data(ChunkPhotoModel::EnabledRole).toBool());
        const QVariantMap check = processing.preflight(QStringLiteral("fast"));
        QCOMPARE(check.value("images").toInt(), 14);
        QVERIFY(check.value("notes").toStringList().join('|').contains(QStringLiteral("Отключено снимков: 2")));
        const QString id = processing.startJob(QStringLiteral("fast"));
        QVERIFY(!id.isEmpty());
        QCOMPARE(processing.job(id).images.size(), 14);
        QVERIFY(!processing.job(id).images.contains(QStringLiteral("photos/p00.jpg")));
        QTRY_COMPARE(record->started.size(), 1);
        engine->finish(JobOutcome::Failed, 1, QStringLiteral("x"));
        QTRY_VERIFY(!processing.active());

        // A split keeps them off in the parts; a merge where they were off everywhere.
        const QStringList parts = chunks.split(QStringLiteral("c1"), {{"mode", "grid"}, {"rows", 1}, {"cols", 2}, {"overlap", 0}})
                                      .value("created").toStringList();
        QCOMPARE(parts.size(), 2);
        QCOMPARE(chunks.chunk(parts[0])->disabled, QStringList{QStringLiteral("p00")});
        QCOMPARE(chunks.chunk(parts[1])->disabled, QStringList{QStringLiteral("p33")});
        const QString merged = chunks.mergePhotos(parts);
        QCOMPARE(chunks.active(), merged);
        QCOMPARE(chunks.activeDisabledCount(), 2);
        QCOMPARE(chunks.setPhotosEnabled({QStringLiteral("p00")}, true), QString());
        QCOMPARE(chunks.activeDisabledCount(), 1);
        QCOMPARE(processing.preflight(QStringLiteral("fast")).value("images").toInt(), 15);
        // Leaving a chunk, the photo is no longer listed as off there.
        QCOMPARE(chunks.removePhoto(merged, QStringLiteral("p33")), QString());
        QCOMPARE(chunks.activeDisabledCount(), 0);

        ProjectManager again;
        ChunkService reread(&again, nullptr);
        projects.closeProject();
        QVERIFY(again.openProject(QUrl::fromLocalFile(dir)));
        QCOMPARE(reread.chunk(QStringLiteral("c1"))->disabled, (QStringList{QStringLiteral("p00"), QStringLiteral("p33")}));
        QCOMPARE(reread.chunk(parts[1])->disabled, QStringList{QStringLiteral("p33")});
        QVERIFY(reread.chunk(merged)->disabled.isEmpty());
    }

    // Masks by a rectangle, and the excluded areas of one photo added to every photo of the chunk
    // of the same size (a part of the drone in every frame).
    void masksByRectangleAndOnEveryPhoto()
    {
        QTemporaryDir tmp;
        const QString dir = tmp.filePath(QStringLiteral("p"));
        const QVector<Shot> shots = gridShots().mid(0, 4);
        QVERIFY(makeProject(dir, shots));
        for (int i = 0; i < shots.size(); ++i) {   // 160 × 120; the last one upright, 120 × 160
            QImage photo(i == 3 ? 120 : 160, i == 3 ? 160 : 120, QImage::Format_RGB32);
            photo.fill(QColor(90, 120, 80));
            QVERIFY(photo.save(QStringLiteral("%1/photos/%2.jpg").arg(dir, shots[i].id), "JPG"));
        }
        ProjectManager projects;
        ChunkService chunks(&projects, nullptr);
        PhotoMasks masks(nullptr);
        QObject::connect(&projects, &ProjectManager::projectDirectoryChanged, &masks, &PhotoMasks::setProject);
        QVERIFY(projects.openProject(QUrl::fromLocalFile(dir)));
        const QStringList files = chunks.activePhotoFiles();
        QCOMPARE(files.size(), 4);
        QCOMPARE(files.first(), QStringLiteral("photos/p00.jpg"));
        auto maskAt = [&](const QString &file, int x, int y) {
            return QImage(QDir(dir).filePath(PhotoMasks::maskFile(file))).convertToFormat(QImage::Format_Grayscale8).pixelColor(x, y).red();
        };

        // The bottom right quarter out, then its corner back.
        QCOMPARE(masks.maskRect(files[0], 1.0, 1.0, 0.5, 0.5), QString());
        QVERIFY(masks.hasMask(files[0]));
        QCOMPARE(maskAt(files[0], 100, 80), 0);
        QCOMPARE(maskAt(files[0], 79, 59), 255);
        QCOMPARE(maskAt(files[0], 159, 119), 0);
        QCOMPARE(masks.maskRect(files[0], 0.75, 0.75, 1.0, 1.0, true), QString());
        QCOMPARE(maskAt(files[0], 150, 110), 255);
        QCOMPARE(maskAt(files[0], 100, 80), 0);
        QVERIFY(!masks.maskRect(files[0], 0.2, 0.2, 0.2, 0.6).isEmpty());   // no area

        // On every photo of the same size.
        QSignalSpy finished(&masks, &PhotoMasks::finished);
        QCOMPARE(masks.copyToPhotos(files[0], files), QString());
        QVERIFY(masks.busy());
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 30000);
        QVERIFY(finished.first().at(0).toBool());
        QCOMPARE(finished.first().at(1).toString(), QStringLiteral("Маска добавлена к 2 сн. · другой размер: 1"));
        QVERIFY(!masks.busy());
        for (int i : {1, 2}) {
            QVERIFY(masks.hasMask(files[i]));
            QCOMPARE(maskAt(files[i], 100, 80), 0);
            QCOMPARE(maskAt(files[i], 150, 110), 255);
            QCOMPARE(maskAt(files[i], 10, 10), 255);
        }
        QVERIFY(!masks.hasMask(files[3]));
        QVERIFY(!masks.copyToPhotos(files[3], files).isEmpty());   // it has no mask

        // Everything given back: no mask at all.
        QCOMPARE(masks.maskRect(files[1], 0, 0, 1, 1, true), QString());
        QVERIFY(!masks.hasMask(files[1]));
        QCOMPARE(masks.maskCount(), 2);
    }

    // Обработка → Оценка качества снимков on real pictures: two of six blurred, found, switched off,
    // kept in quality.json; a changed file is measured again; the estimate can be stopped.
    void qualityOfThePhotos()
    {
        QTemporaryDir tmp;
        const QString dir = tmp.filePath(QStringLiteral("p"));
        const QVector<Shot> shots = gridShots().mid(0, 6);
        QVERIFY(makeProject(dir, shots));
        auto write = [&](int i, int blur) {
            QImage scene = TestImages::texturedScene(800, 600, quint32(i + 1));
            if (blur > 0) scene = TestImages::motionBlur(scene, blur);
            return scene.save(QStringLiteral("%1/photos/%2.jpg").arg(dir, shots[i].id), "JPG", 92);
        };
        for (int i = 0; i < shots.size(); ++i) QVERIFY(write(i, i == 1 || i == 4 ? 15 : 0));
        const QString b1 = shots[1].id, b2 = shots[4].id, sharp = shots[0].id;

        ProjectManager projects;
        ChunkService chunks(&projects, nullptr);
        PhotoQualityService quality(&projects, &chunks);
        QVERIFY(projects.openProject(QUrl::fromLocalFile(dir)));
        QCOMPARE(quality.measured(), 0);
        QCOMPARE(quality.qualityOf(sharp), -1.0);
        QSignalSpy finished(&quality, &PhotoQualityService::finished);
        QCOMPARE(quality.estimate(), QString());
        QVERIFY(quality.busy());
        QTRY_VERIFY_WITH_TIMEOUT(!quality.busy(), 30000);
        QCOMPARE(quality.done(), 6);
        QCOMPARE(quality.measured(), 6);
        QCOMPARE(quality.blurred(), 2);
        QCOMPARE(finished.count(), 1);
        QVERIFY(finished.first().at(0).toBool());
        QCOMPARE(finished.first().at(1).toString(), QStringLiteral("Оценено снимков: 6 · нечётких: 2"));
        const QVariantList rows = quality.photos();   // worst first
        QCOMPARE(rows.size(), 6);
        const QStringList worst{rows[0].toMap().value("id").toString(), rows[1].toMap().value("id").toString()};
        QVERIFY2(worst.contains(b1) && worst.contains(b2), qPrintable(worst.join(' ')));
        QVERIFY(rows[0].toMap().value("blurred").toBool() && !rows[2].toMap().value("blurred").toBool());
        QVERIFY(quality.qualityOf(sharp) >= 0.8);
        QVERIFY(quality.qualityOf(b1) < 0.5);
        QCOMPARE(quality.estimate(), QStringLiteral("Все снимки блока уже оценены"));

        // A photo replaced by a blurred one: only it is measured again.
        QVERIFY(write(0, 15));
        QCOMPARE(quality.estimate(), QString());
        QCOMPARE(quality.total(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(!quality.busy(), 30000);
        QCOMPARE(quality.blurred(), 3);

        // Off in the chunk, and on again.
        QCOMPARE(quality.blurredEnabled(), 3);
        QCOMPARE(quality.disableBlurred(), QString());
        QCOMPARE(chunks.activeDisabledCount(), 3);
        QVERIFY(!chunks.photoEnabled(b1));
        QCOMPARE(quality.blurredEnabled(), 0);
        QCOMPARE(quality.blurred(), 3);
        QVERIFY(!quality.photos().first().toMap().value("enabled").toBool());
        QCOMPARE(quality.enableAll(), QString());
        QCOMPARE(chunks.activeDisabledCount(), 0);
        quality.setThreshold(0.123);
        QCOMPARE(quality.threshold(), 0.12);

        // Stopped right away: the photos not started are skipped, the values so far are kept.
        finished.clear();
        QCOMPARE(quality.estimate(true), QString());
        quality.cancel();
        QTRY_VERIFY_WITH_TIMEOUT(!quality.busy(), 30000);
        QCOMPARE(quality.done(), 6);
        QCOMPARE(finished.count(), 1);
        QVERIFY2(finished.first().at(1).toString().startsWith(QStringLiteral("Оценка остановлена")), qPrintable(finished.first().at(1).toString()));
        QCOMPARE(quality.measured(), 6);

        // Back with the project.
        const double q = quality.qualityOf(b2);
        ProjectManager again;
        ChunkService chunks2(&again, nullptr);
        PhotoQualityService reread(&again, &chunks2);
        projects.closeProject();
        QCOMPARE(quality.measured(), 0);
        QVERIFY(again.openProject(QUrl::fromLocalFile(dir)));
        QCOMPARE(reread.measured(), 6);
        QCOMPARE(reread.threshold(), 0.12);
        QVERIFY(qAbs(reread.qualityOf(b2) - q) < 1e-9);
    }
};

QTEST_MAIN(ChunkTest)
#include "test_chunks.moc"
