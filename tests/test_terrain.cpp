#include <QtTest>
#include <QDir>
#include <QTemporaryDir>
#include <cmath>
#include <functional>
#include <gdal.h>
#include <ogr_api.h>
#include <ogr_srs_api.h>
#include "Crs.h"
#include "Geodesy.h"
#include "GisEnvironment.h"
#include "TerrainAnalysis.h"

// Terrain analysis on synthetic surfaces with known answers: planes (slope, aspect, contours,
// profiles) and a box on flat ground (volumes), in UTM 15N with 1 m cells.
namespace {
constexpr double X0 = 576000, Y0 = 5188100;   // top-left corner

bool makeDem(const QString &path, int w, int h, const std::function<double(int, int)> &z, int epsg = 32615, double cell = 1.0,
             double noData = std::nan(""), double x0 = X0, double y0 = Y0)
{
    GDALDatasetH ds = GDALCreate(GDALGetDriverByName("GTiff"), QDir::toNativeSeparators(path).toUtf8().constData(), w, h, 1,
                                 GDT_Float32, nullptr);
    if (!ds) return false;
    double gt[6] = {x0, cell, 0, y0, 0, -cell};
    GDALSetGeoTransform(ds, gt);
    OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
    OSRImportFromEPSG(srs, epsg);
    GDALSetSpatialRef(ds, srs);
    OSRDestroySpatialReference(srs);
    GDALRasterBandH band = GDALGetRasterBand(ds, 1);
    if (!std::isnan(noData)) GDALSetRasterNoDataValue(band, noData);
    QVector<float> row(w);
    for (int r = 0; r < h; ++r) {
        for (int c = 0; c < w; ++c) row[c] = float(z(c, r));
        if (GDALRasterIO(band, GF_Write, 0, r, w, 1, row.data(), w, 1, GDT_Float32, 0, 0) != CE_None) {
            GDALClose(ds);
            return false;
        }
    }
    GDALClose(ds);
    return true;
}

double pixel(const QString &path, int c, int r)
{
    GDALDatasetH ds = GDALOpen(QDir::toNativeSeparators(path).toUtf8().constData(), GA_ReadOnly);
    float v = 0;
    if (ds) {
        GDALRasterIO(GDALGetRasterBand(ds, 1), GF_Read, c, r, 1, 1, &v, 1, 1, GDT_Float32, 0, 0);
        GDALClose(ds);
    }
    return v;
}

Crs utm() { return Crs::fromEpsg(32615); }
} // namespace

class TerrainTest : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QVERIFY2(GisEnvironment::initialize(QCoreApplication::applicationDirPath()), qPrintable(GisEnvironment::problems().join('\n')));
        QVERIFY(m_dir.isValid());
        // Rises 0.1 m per metre to the east; rises 0.2 m per metre to the north.
        QVERIFY(makeDem(m_dir.filePath("east.tif"), 100, 80, [](int c, int) { return 100.0 + 0.1 * (c + 0.5); }));
        QVERIFY(makeDem(m_dir.filePath("north.tif"), 100, 80, [](int, int r) { return 100.0 + 0.2 * (80 - r - 0.5); }));
    }

    void slopeOfPlanes()
    {
        const terrain::RasterResult deg = terrain::slope(m_dir.filePath("east.tif"), m_dir.filePath("out/east-slope.tif"));
        QVERIFY2(deg.ok(), qPrintable(deg.error));
        const double expected = qRadiansToDegrees(std::atan(0.1));   // 5.7106°
        QVERIFY(deg.stats.valid);
        QVERIFY2(std::fabs(deg.stats.min - expected) < 0.01 && std::fabs(deg.stats.max - expected) < 0.01,
                 qPrintable(QStringLiteral("%1..%2").arg(deg.stats.min).arg(deg.stats.max)));
        QCOMPARE(pixel(deg.path, 0, 0), -9999.0);   // the outer ring has no neighbourhood: nodata, not a guess
        QVERIFY(std::fabs(pixel(deg.path, 1, 1) - expected) < 0.01);
        const terrain::RasterResult pct = terrain::slope(m_dir.filePath("east.tif"), m_dir.filePath("out/east-slope-pct.tif"), true);
        QVERIFY2(pct.ok(), qPrintable(pct.error));
        QVERIFY(std::fabs(pct.stats.mean - 10.0) < 0.01);
        const terrain::RasterResult north = terrain::slope(m_dir.filePath("north.tif"), m_dir.filePath("out/north-slope.tif"));
        QVERIFY(std::fabs(north.stats.mean - qRadiansToDegrees(std::atan(0.2))) < 0.01);
        // Errors are reported, not thrown.
        QVERIFY(!terrain::slope(m_dir.filePath("missing.tif"), m_dir.filePath("out/x.tif")).ok());
    }

    void aspectOfPlanes()
    {
        // A surface rising to the east faces west (270°); rising to the north, it faces south (180°).
        const terrain::RasterResult west = terrain::aspect(m_dir.filePath("east.tif"), m_dir.filePath("out/east-aspect.tif"));
        QVERIFY2(west.ok(), qPrintable(west.error));
        QVERIFY2(std::fabs(west.stats.mean - 270.0) < 0.01, qPrintable(QString::number(west.stats.mean)));
        const terrain::RasterResult south = terrain::aspect(m_dir.filePath("north.tif"), m_dir.filePath("out/north-aspect.tif"));
        QVERIFY(std::fabs(south.stats.mean - 180.0) < 0.01);
    }

    void contoursOfAPlane()
    {
        // 100.05 .. 109.95 m: levels 101 .. 109 every metre, each a straight north-south line.
        const terrain::ContourResult c = terrain::contours(m_dir.filePath("east.tif"), m_dir.filePath("out/contours.gpkg"), 1.0, 0.0);
        QVERIFY2(c.ok(), qPrintable(c.error));
        QCOMPARE(c.lines, 9);
        QCOMPARE(c.minLevel, 101.0);
        QCOMPARE(c.maxLevel, 109.0);
        // The 105 m line lies 50 m east of the west edge.
        GDALDatasetH ds = GDALOpenEx(QDir::toNativeSeparators(c.path).toUtf8().constData(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
        QVERIFY(ds);
        OGRLayerH layer = GDALDatasetGetLayerByName(ds, "contours");
        QVERIFY(layer);
        QVERIFY(OGR_L_GetSpatialRef(layer));
        bool found = false;
        while (OGRFeatureH f = OGR_L_GetNextFeature(layer)) {
            if (OGR_F_GetFieldAsDouble(f, OGR_F_GetFieldIndex(f, "elev")) == 105.0) {
                OGRGeometryH g = OGR_F_GetGeometryRef(f);
                for (int i = 0; i < OGR_G_GetPointCount(g); ++i) QVERIFY(std::fabs(OGR_G_GetX(g, i) - (X0 + 50.0)) < 0.01);
                found = OGR_G_GetPointCount(g) >= 2;
            }
            OGR_F_Destroy(f);
        }
        GDALClose(ds);
        QVERIFY(found);
        // Base shifts the levels; a non-positive interval and flooding levels are refused.
        const terrain::ContourResult half = terrain::contours(m_dir.filePath("east.tif"), m_dir.filePath("out/c2.gpkg"), 2.0, 0.5);
        QCOMPARE(half.minLevel, 100.5);
        QVERIFY(!terrain::contours(m_dir.filePath("east.tif"), m_dir.filePath("out/c3.gpkg"), 0, 0).ok());
        QVERIFY(!terrain::contours(m_dir.filePath("east.tif"), m_dir.filePath("out/c4.gpkg"), 0.0001, 0).ok());
        // Suggested intervals: round numbers, 10–25 lines.
        QCOMPARE(terrain::suggestedInterval(100, 112), 1.0);
        QCOMPARE(terrain::suggestedInterval(0, 300), 20.0);
        QCOMPARE(terrain::suggestedInterval(0, 37), 2.5);
        QCOMPARE(terrain::suggestedInterval(5, 5), 1.0);
    }

    void profileAlongAPlane()
    {
        // West to east 80 m at a constant northing: heights 101.05 → 109.05 linearly.
        const QVector<QPointF> line = {{X0 + 10.5, Y0 - 40.5}, {X0 + 50.5, Y0 - 40.5}, {X0 + 90.5, Y0 - 40.5}};
        const terrain::Profile p = terrain::profile(m_dir.filePath("east.tif"), line, utm(), 161);
        QVERIFY2(p.ok(), qPrintable(p.error));
        QCOMPARE(p.points.size(), 161);
        // Geodesic length: 80 m of grid divided by the UTM scale factor there (~0.99962).
        QVERIFY2(std::fabs(p.length - 80.0 / 0.99962) < 0.02, qPrintable(QString::number(p.length, 'f', 4)));
        QCOMPARE(p.points.first().distance, 0.0);
        QVERIFY(std::fabs(p.points.last().distance - p.length) < 1e-6);
        QVERIFY(std::fabs(p.points.first().z - 101.05) < 1e-3);
        QVERIFY(std::fabs(p.points.last().z - 109.05) < 1e-3);
        QVERIFY(std::fabs(p.points[80].z - 105.05) < 1e-3);
        QVERIFY(std::fabs(p.ascent - 8.0) < 1e-3);
        QVERIFY(p.descent < 1e-6);
        QVERIFY(std::fabs(p.maxSlope - qRadiansToDegrees(std::atan(8.0 / p.length))) < 0.01);
        QCOMPARE(p.noData, 0);
        // The same line given in WGS 84: the same heights.
        QVector<QPointF> geo = line;
        QVERIFY(CoordinateTransform(utm(), Crs::wgs84()).transform(geo));
        const terrain::Profile g = terrain::profile(m_dir.filePath("east.tif"), geo, Crs::wgs84(), 161);
        QVERIFY2(g.ok(), qPrintable(g.error));
        QVERIFY(std::fabs(g.points[80].z - 105.05) < 0.01);
        QVERIFY(std::fabs(g.length - p.length) < 0.01);
        // Partly outside the DEM: no heights invented there.
        const terrain::Profile out = terrain::profile(m_dir.filePath("east.tif"), {{X0 - 50.5, Y0 - 40.5}, {X0 + 50.5, Y0 - 40.5}}, utm(), 101);
        QVERIFY(out.ok());
        QVERIFY(out.noData >= 45 && out.noData <= 55);
        QVERIFY(!std::isfinite(out.points.first().z));
        QVERIFY(!terrain::profile(m_dir.filePath("east.tif"), {{X0 + 1, Y0 - 1}}, utm()).ok());
    }

    void volumesOfABox()
    {
        // 60 × 60 m at 100 m with a 20 × 20 m box 5 m high in the middle (cells 20..39).
        const auto box = [](int c, int r) { return (c >= 20 && c < 40 && r >= 20 && r < 40) ? 105.0 : 100.0; };
        QVERIFY(makeDem(m_dir.filePath("box.tif"), 60, 60, box));
        QVERIFY(makeDem(m_dir.filePath("flat.tif"), 60, 60, [](int, int) { return 100.0; }));
        // Polygon 40 × 40 m around the box (cells 10..49).
        const QVector<QPointF> ring = {{X0 + 10, Y0 - 10}, {X0 + 50, Y0 - 10}, {X0 + 50, Y0 - 50}, {X0 + 10, Y0 - 50}};
        terrain::Volume v = terrain::volume(m_dir.filePath("box.tif"), ring, utm(), terrain::Base::Level, 100.0);
        QVERIFY2(v.ok(), qPrintable(v.error));
        QCOMPARE(v.cells, qint64(1600));
        QVERIFY(std::fabs(v.area - 1600) < 1e-6);
        QVERIFY(std::fabs(v.above - 2000) < 1e-6);
        QVERIFY(v.below < 1e-9);
        QCOMPARE(v.baseLevel, 100.0);
        // Level 102: 400 cells 3 m above, 1200 cells 2 m below.
        v = terrain::volume(m_dir.filePath("box.tif"), ring, utm(), terrain::Base::Level, 102.0);
        QVERIFY(std::fabs(v.above - 1200) < 1e-6 && std::fabs(v.below - 2400) < 1e-6);
        QVERIFY(std::fabs(v.net() + 1200) < 1e-6);
        // Bases from the boundary (all at 100 m) and from another surface.
        for (terrain::Base base : {terrain::Base::BoundaryMean, terrain::Base::BoundaryMin, terrain::Base::BestFit}) {
            v = terrain::volume(m_dir.filePath("box.tif"), ring, utm(), base);
            QVERIFY2(v.ok() && std::fabs(v.above - 2000) < 0.01 && v.below < 0.01, qPrintable(QString::number(v.above)));
        }
        v = terrain::volume(m_dir.filePath("box.tif"), ring, utm(), terrain::Base::Surface, 0, m_dir.filePath("flat.tif"));
        QVERIFY(v.ok() && std::fabs(v.above - 2000) < 1e-6);
        // A stockpile on a slope: the best-fit plane removes the slope, a horizontal base does not.
        QVERIFY(makeDem(m_dir.filePath("slope-box.tif"), 60, 60,
                        [box](int c, int r) { return box(c, r) + 0.1 * (c + 0.5); }));
        v = terrain::volume(m_dir.filePath("slope-box.tif"), ring, utm(), terrain::Base::BestFit);
        QVERIFY2(std::fabs(v.above - 2000) < 1.0 && v.below < 1.0, qPrintable(QStringLiteral("%1 / %2").arg(v.above).arg(v.below)));
        // The polygon in WGS 84 gives the same cells.
        QVector<QPointF> geo = ring;
        QVERIFY(CoordinateTransform(utm(), Crs::wgs84()).transform(geo));
        v = terrain::volume(m_dir.filePath("box.tif"), geo, Crs::wgs84(), terrain::Base::Level, 100.0);
        QVERIFY(v.ok());
        QCOMPARE(v.cells, qint64(1600));
        // Nodata cells are counted, not guessed.
        QVERIFY(makeDem(m_dir.filePath("holes.tif"), 60, 60, [box](int c, int r) { return (c == 15 && r < 30) ? -9999.0 : box(c, r); },
                        32615, 1.0, -9999.0));
        v = terrain::volume(m_dir.filePath("holes.tif"), ring, utm(), terrain::Base::Level, 100.0);
        QCOMPARE(v.noData, qint64(20));
        QCOMPARE(v.cells, qint64(1580));
        // Geographic DEMs and polygons outside are refused with a reason.
        QVERIFY(makeDem(m_dir.filePath("geo.tif"), 10, 10, [](int, int) { return 1.0; }, 4326, 0.001));
        QVERIFY(!terrain::volume(m_dir.filePath("geo.tif"), geo, Crs::wgs84(), terrain::Base::Level, 0).ok());
        QVERIFY(!terrain::volume(m_dir.filePath("box.tif"), {{0, 0}, {1, 0}, {1, 1}}, utm(), terrain::Base::Level, 0).ok());
    }

    void differenceOfModels()
    {
        // Box minus flat ground: 0 everywhere, 5 m on the box (400 of 3600 cells).
        const terrain::RasterResult d = terrain::difference(m_dir.filePath("box.tif"), m_dir.filePath("flat.tif"), m_dir.filePath("out/diff.tif"));
        QVERIFY2(d.ok(), qPrintable(d.error));
        QCOMPARE(d.stats.min, 0.0);
        QCOMPARE(d.stats.max, 5.0);
        QVERIFY(std::fabs(d.stats.mean - 5.0 * 400 / 3600) < 1e-6);
        QCOMPARE(pixel(d.path, 30, 30), 5.0);
        // B on another grid (2 m cells, shifted, larger): resampled onto A's grid.
        QVERIFY(makeDem(m_dir.filePath("coarse.tif"), 40, 40, [](int, int) { return 99.0; }, 32615, 2.0, std::nan(""), X0 - 7.3, Y0 + 5.1));
        const terrain::RasterResult c = terrain::difference(m_dir.filePath("flat.tif"), m_dir.filePath("coarse.tif"), m_dir.filePath("out/diff2.tif"));
        QVERIFY2(c.ok(), qPrintable(c.error));
        QVERIFY(std::fabs(c.stats.min - 1.0) < 1e-6 && std::fabs(c.stats.max - 1.0) < 1e-6);
        // Where B has no data: nodata in the result.
        QVERIFY(makeDem(m_dir.filePath("part.tif"), 30, 60, [](int, int) { return 98.0; }));
        const terrain::RasterResult p = terrain::difference(m_dir.filePath("flat.tif"), m_dir.filePath("part.tif"), m_dir.filePath("out/diff3.tif"));
        QVERIFY2(p.ok(), qPrintable(p.error));
        QCOMPARE(pixel(p.path, 10, 10), 2.0);
        QCOMPARE(pixel(p.path, 50, 10), -9999.0);
        // No overlap at all.
        QVERIFY(makeDem(m_dir.filePath("far.tif"), 10, 10, [](int, int) { return 1.0; }, 32615, 1.0, std::nan(""), X0 + 5000, Y0));
        QVERIFY(!terrain::difference(m_dir.filePath("flat.tif"), m_dir.filePath("far.tif"), m_dir.filePath("out/diff4.tif")).ok());
    }

    // Cross sections of a plane rising 0.1 m per metre to the east, along a north–south axis.
    void crossSectionsOfAPlane()
    {
        const QVector<QPointF> axis{{X0 + 50.5, Y0 - 10}, {X0 + 50.5, Y0 - 70}};   // heading south
        QVector<QVector<QPointF>> lines;
        const QVector<terrain::Profile> s = terrain::crossSections(m_dir.filePath("east.tif"), axis, utm(), 10, 20, 81, &lines);
        QCOMPARE(s.size(), 7);   // stations 0, 10, …, 60 m
        QCOMPARE(lines.size(), 7);
        for (int i = 0; i < s.size(); ++i) {
            QVERIFY2(s[i].ok(), qPrintable(s[i].error));
            // Left of a southward axis is east: from 107.05 m down to 103.05 m.
            QVERIFY(qAbs(s[i].points.first().z - (100 + 0.1 * 70.5)) < 1e-3);
            QVERIFY(qAbs(s[i].points.last().z - (100 + 0.1 * 30.5)) < 1e-3);
            QVERIFY(qAbs(s[i].descent - 4.0) < 1e-3);
            QVERIFY(qAbs(s[i].length - 40) < 0.1);
            QVERIFY(qAbs(lines[i].first().y() - (Y0 - 10 - 10 * i)) < 1e-6);
        }
        QVERIFY(!terrain::crossSections(m_dir.filePath("east.tif"), {{-93, 46.8}, {-93, 46.7}}, Crs::wgs84(), 10, 20).first().ok());
        QVERIFY(terrain::crossSections(m_dir.filePath("east.tif"), axis, utm(), 0, 20).isEmpty());
    }

    // Viewshed over flat ground with a 10 m wall: from the west, nothing behind the wall is seen.
    void viewshedBehindAWall()
    {
        QVERIFY(makeDem(m_dir.filePath("wall.tif"), 200, 100, [](int c, int) { return c >= 100 && c < 102 ? 110.0 : 100.0; }));
        const terrain::Viewshed v = terrain::viewshed(m_dir.filePath("wall.tif"), {X0 + 50.5, Y0 - 50.5}, utm(), 1.7, 0, 1000,
                                                      m_dir.filePath("out/viewshed.tif"));
        QVERIFY2(v.ok(), qPrintable(v.error));
        QCOMPARE(v.observerZ, 100.0);
        QVERIFY2(qAbs(v.area - 20000) < 1, qPrintable(QString::number(v.area)));
        QVERIFY2(qAbs(v.visibleArea - 10200) < 300, qPrintable(QString::number(v.visibleArea)));   // the west half and the wall
        GDALDatasetH ds = GDALOpen(QDir::toNativeSeparators(v.path).toUtf8().constData(), GA_ReadOnly);
        QVERIFY(ds);
        uchar west = 9, east = 9;
        GDALRasterIO(GDALGetRasterBand(ds, 1), GF_Read, 20, 50, 1, 1, &west, 1, 1, GDT_Byte, 0, 0);
        GDALRasterIO(GDALGetRasterBand(ds, 1), GF_Read, 150, 50, 1, 1, &east, 1, 1, GDT_Byte, 0, 0);
        GDALClose(ds);
        QCOMPARE(int(west), 1);
        QCOMPARE(int(east), 0);
        // A small radius: only the area within it counts.
        const terrain::Viewshed near = terrain::viewshed(m_dir.filePath("wall.tif"), {X0 + 50.5, Y0 - 50.5}, utm(), 1.7, 0, 20,
                                                         m_dir.filePath("out/viewshed-20.tif"));
        QVERIFY(near.ok());
        QVERIFY2(qAbs(near.area - 3.14159 * 400) < 60, qPrintable(QString::number(near.area)));
        QVERIFY(qAbs(near.visibleArea - near.area) < 1);
        QVERIFY(!terrain::viewshed(m_dir.filePath("wall.tif"), {X0 - 500, Y0}, utm(), 1.7, 0, 100, m_dir.filePath("out/v3.tif")).ok());
    }

    // Raster calculator: vegetation indices and expressions on bands with known values.
    void calculatorAndIndices()
    {
        // 100 × 10, RGBA: green vegetation in the west half, grey soil in the east; column 0 transparent.
        const QString path = m_dir.filePath("rgba.tif");
        GDALDatasetH ds = GDALCreate(GDALGetDriverByName("GTiff"), QDir::toNativeSeparators(path).toUtf8().constData(), 100, 10, 4, GDT_Byte, nullptr);
        QVERIFY(ds);
        double gt[6] = {X0, 1, 0, Y0, 0, -1};
        GDALSetGeoTransform(ds, gt);
        GDALSetRasterColorInterpretation(GDALGetRasterBand(ds, 4), GCI_AlphaBand);
        for (int b = 1; b <= 4; ++b) {
            QVector<uchar> v(1000);
            for (int r = 0; r < 10; ++r)
                for (int c = 0; c < 100; ++c) {
                    const bool west = c < 50;
                    const int values[2][4] = {{120, 100, 80, 255}, {50, 150, 50, 255}};
                    v[r * 100 + c] = uchar(b == 4 ? (c == 0 ? 0 : 255) : values[west][b - 1]);
                }
            GDALRasterIO(GDALGetRasterBand(ds, b), GF_Write, 0, 0, 100, 10, v.data(), 100, 10, GDT_Byte, 0, 0);
        }
        GDALClose(ds);
        double above = 0;
        const terrain::RasterResult exg = terrain::calculate(path, terrain::indexExpression(QStringLiteral("exg")), m_dir.filePath("out/exg.tif"),
                                                             terrain::indexThreshold(QStringLiteral("exg")), &above);
        QVERIFY2(exg.ok(), qPrintable(exg.error));
        QVERIFY(qAbs(pixel(exg.path, 10, 5) - 0.8) < 1e-6);    // (300 − 100) / 250
        QVERIFY(qAbs(pixel(exg.path, 80, 5) - 0.0) < 1e-6);    // (200 − 200) / 300
        QCOMPARE(pixel(exg.path, 0, 5), -9999.0);              // transparent
        QVERIFY2(qAbs(above - 49.0 / 99.0) < 1e-6, qPrintable(QString::number(above)));
        QVERIFY(qAbs(exg.stats.max - 0.8) < 1e-6 && qAbs(exg.stats.min) < 1e-6);
        // NDVI-like expression with functions; division by zero is nodata.
        const terrain::RasterResult e = terrain::calculate(path, QStringLiteral("max(B1, B2) - sqrt(abs(-B3)) * 2"), m_dir.filePath("out/e.tif"));
        QVERIFY2(e.ok(), qPrintable(e.error));
        QVERIFY(qAbs(pixel(e.path, 10, 5) - (150 - std::sqrt(50.0) * 2)) < 1e-3);
        const terrain::RasterResult zero = terrain::calculate(path, QStringLiteral("B1/(B2-B2)"), m_dir.filePath("out/zero.tif"));
        QVERIFY(zero.ok());
        QVERIFY(!zero.stats.valid);
        QCOMPARE(pixel(zero.path, 10, 5), -9999.0);
        // Errors are explained before anything is computed.
        QVERIFY(terrain::checkExpression(QStringLiteral("B5"), 4).contains(QStringLiteral("B5")));
        QVERIFY(!terrain::checkExpression(QStringLiteral("(B1+"), 4).isEmpty());
        QVERIFY(!terrain::checkExpression(QStringLiteral("B1 B2"), 4).isEmpty());
        QVERIFY(!terrain::checkExpression(QStringLiteral("max(B1)"), 4).isEmpty());
        QVERIFY(terrain::checkExpression(QStringLiteral("-(B1 - 2.5) * min(B2, 3)"), 4).isEmpty());
        QCOMPARE(terrain::indexExpression(QStringLiteral("ndvi"), 3, 2, 1, 5), QStringLiteral("(B5-B3)/(B5+B3)"));
        QVERIFY(!terrain::calculate(path, QStringLiteral("B9"), m_dir.filePath("out/b9.tif")).ok());
    }

private:
    QTemporaryDir m_dir;
};

QTEST_MAIN(TerrainTest)
#include "test_terrain.moc"
