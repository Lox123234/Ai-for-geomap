#include <QtTest>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>
#include <cmath>
#include <functional>
#include <gdal.h>
#include <ogr_api.h>
#include <ogr_srs_api.h>
#include "Crs.h"
#include "Geodesy.h"
#include "GisEnvironment.h"
#include "MapService.h"
#include "PhotoModel.h"
#include "RasterSource.h"
#include "VectorSource.h"

namespace {
constexpr double Pi = 3.14159265358979323846;

// Writes a tiled GeoTIFF with GDAL. epsg 0 = no CRS in the file.
bool makeRaster(const QString &path, int w, int h, int bands, GDALDataType type, const double gt[6], int epsg,
                const std::function<double(int, int, int)> &value, double noData = std::nan(""))
{
    GDALDriverH driver = GDALGetDriverByName("GTiff");
    const char *options[] = {"TILED=YES", "COMPRESS=DEFLATE", nullptr};
    GDALDatasetH ds = GDALCreate(driver, QDir::toNativeSeparators(path).toUtf8().constData(), w, h, bands, type,
                                 const_cast<char **>(options));
    if (!ds) return false;
    double transform[6];
    std::copy(gt, gt + 6, transform);
    GDALSetGeoTransform(ds, transform);
    if (epsg) {
        OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
        OSRImportFromEPSG(srs, epsg);
        GDALSetSpatialRef(ds, srs);
        OSRDestroySpatialReference(srs);
    }
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

bool writeText(const QString &path, const QByteArray &text)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(text) == text.size();
}

// Area of the ellipsoidal quadrangle between two parallels and two meridians (authalic formula).
double quadrangleArea(double a, double f, double lat1, double lat2, double dLonDeg)
{
    const double e2 = f * (2 - f), e = std::sqrt(e2);
    auto q = [&](double phi) {
        const double s = std::sin(phi);
        return (1 - e2) * (s / (1 - e2 * s * s) - (1 / (2 * e)) * std::log((1 - e * s) / (1 + e * s)));
    };
    return a * a * (dLonDeg * Pi / 180) / 2 * (q(lat2 * Pi / 180) - q(lat1 * Pi / 180));
}
} // namespace

class GisTest : public QObject
{
    Q_OBJECT
    QTemporaryDir m_settings;
private slots:
    void initTestCase()
    {
        QVERIFY(m_settings.isValid());
        QCoreApplication::setOrganizationName(QStringLiteral("GeoMapAI-Tests"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settings.path());
        QVERIFY2(GisEnvironment::initialize(QCoreApplication::applicationDirPath()),
                 qPrintable(GisEnvironment::problems().join('\n')));
        QVERIFY(QFileInfo::exists(QDir(GisEnvironment::projDataDirectory()).filePath(QStringLiteral("proj.db"))));
        qInfo("GDAL %s, PROJ %s, proj.db in %s", qPrintable(GisEnvironment::gdalVersion()), qPrintable(GisEnvironment::projVersion()),
              qPrintable(GisEnvironment::projDataDirectory()));
    }

    void crsFromEpsgAndUserInput()
    {
        QString error;
        const Crs utm = Crs::fromEpsg(32615, &error);
        QVERIFY2(utm.isValid(), qPrintable(error));
        QCOMPARE(utm.epsg(), 32615);
        QVERIFY(utm.isProjected());
        QCOMPARE(utm.name(), QStringLiteral("WGS 84 / UTM zone 15N"));
        QCOMPARE(utm.unitName(), QStringLiteral("metre"));
        QCOMPARE(utm.metersPerUnit(), 1.0);
        QCOMPARE(utm.semiMajor(), 6378137.0);
        QVERIFY(qAbs(utm.inverseFlattening() - 298.257223563) < 1e-9);
        QCOMPARE(utm.label(), QStringLiteral("EPSG:32615 · WGS 84 / UTM zone 15N"));
        QCOMPARE(utm.geographic().epsg(), 4326);

        const Crs wgs = Crs::fromUserInput(QStringLiteral("EPSG:4326"), &error);
        QVERIFY(wgs.isGeographic());
        QVERIFY(wgs.isSame(Crs::wgs84()));
        QCOMPARE(Crs::fromUserInput(QStringLiteral("3857")).epsg(), 3857);
        QVERIFY(Crs::fromWkt(utm.wkt()).isSame(utm));
        QCOMPARE(Crs::fromWkt(utm.wkt()).epsg(), 32615);

        QVERIFY(!Crs::fromEpsg(999999, &error).isValid());
        QVERIFY(error.contains(QStringLiteral("999999")));
        QVERIFY(!Crs::fromUserInput(QStringLiteral("не система координат"), &error).isValid());
        QVERIFY(!Crs::fromUserInput(QStringLiteral("C:/Windows/win.ini"), &error).isValid());   // no file access
        QVERIFY(!Crs().isValid());
        QCOMPARE(Crs().label(), QStringLiteral("CRS не задан"));
    }

    // Control values that do not depend on PROJ: UTM is defined with k0 = 0.9996 on the central
    // meridian, the WGS 84 meridian arc to 45° is 4 984 944.378 m; Web Mercator x(180°) = pi * a.
    void transformsMatchIndependentControlValues()
    {
        const Crs wgs = Crs::wgs84(), utm = Crs::fromEpsg(32615), merc = Crs::fromEpsg(3857);
        CoordinateTransform toUtm(wgs, utm), toWgs(utm, wgs), toMerc(wgs, merc);
        QVERIFY(toUtm.isValid() && toWgs.isValid() && toMerc.isValid());
        double x = -93, y = 0;
        QVERIFY(toUtm.transform(x, y));
        QVERIFY(qAbs(x - 500000.0) < 1e-6);
        QVERIFY(qAbs(y) < 1e-6);
        x = -93; y = 45;
        QVERIFY(toUtm.transform(x, y));
        QVERIFY2(qAbs(x - 500000.0) < 1e-4 && qAbs(y - 4982950.400) < 0.01, qPrintable(QStringLiteral("%1 %2").arg(x, 0, 'f', 4).arg(y, 0, 'f', 4)));
        QVERIFY(toWgs.transform(x, y));
        QVERIFY(qAbs(x + 93) < 1e-9 && qAbs(y - 45) < 1e-9);
        x = 180; y = 0;
        QVERIFY(toMerc.transform(x, y));
        QVERIFY(qAbs(x - Pi * 6378137.0) < 1e-6);
        // Axis order is always x = longitude.
        QVector<QPointF> pts{QPointF(-93, 45), QPointF(-92, 46)};
        QVERIFY(toUtm.transform(pts));
        QVERIFY(pts[0].x() > 400000 && pts[0].y() > 4000000);
        CoordinateTransform invalid(Crs(), utm);
        QVERIFY(!invalid.isValid());
    }

    void geodesicMeasurementsMatchControlValues()
    {
        // One degree along the equator: a * pi / 180 exactly.
        QVERIFY(qAbs(Geodesy::distance(QPointF(0, 0), QPointF(1, 0)) - 6378137.0 * Pi / 180) < 1e-6);
        // One degree of latitude from the equator on WGS 84: 110 574.389 m.
        QVERIFY(qAbs(Geodesy::distance(QPointF(0, 0), QPointF(0, 1)) - 110574.389) < 0.01);
        // GeographicLib documentation, GeodSolve -i: JFK (40.6, -73.8) to LHR (51.6, -0.5).
        QVERIFY(qAbs(Geodesy::distance(QPointF(-73.8, 40.6), QPointF(-0.5, 51.6)) - 5551759.400319) < 0.001);
        // A polyline is the sum of its geodesic segments.
        const double l = Geodesy::length({QPointF(0, 0), QPointF(1, 0), QPointF(1, 1)});
        QVERIFY(qAbs(l - (6378137.0 * Pi / 180 + Geodesy::distance(QPointF(1, 0), QPointF(1, 1)))) < 1e-6);
        // 1° x 1° at the equator against the authalic quadrangle formula; the top edge is a
        // geodesic, not a parallel, which changes the area by about 3e-5.
        const double expected = quadrangleArea(6378137.0, 1 / 298.257223563, 0, 1, 1);
        const Geodesy::Polygon p = Geodesy::polygon({QPointF(0, 0), QPointF(1, 0), QPointF(1, 1), QPointF(0, 1)});
        QVERIFY2(qAbs(p.area / expected - 1) < 1e-4, qPrintable(QStringLiteral("%1 vs %2").arg(p.area, 0, 'f', 1).arg(expected, 0, 'f', 1)));
        // Open and closed rings and both orientations give the same positive area.
        const Geodesy::Polygon closed = Geodesy::polygon({QPointF(0, 0), QPointF(0, 1), QPointF(1, 1), QPointF(1, 0), QPointF(0, 0)});
        QVERIFY(qAbs(closed.area - p.area) < 1e-3);
        // Not the naive "degrees times degrees": that would be off by orders of magnitude.
        QVERIFY(p.area > 1.2e10 && p.area < 1.24e10);
        QCOMPARE(Geodesy::polygon({QPointF(0, 0), QPointF(1, 1)}).area, 0.0);
    }

    void rasterTilesAndValuesFollowTheGeotransform()
    {
        QTemporaryDir tmp;
        const QString path = tmp.filePath(QStringLiteral("ортофото.tif"));
        // 1000 x 800, 10 cm pixels; left half red, right half blue.
        const double gt[6] = {576600, 0.1, 0, 5188300, 0, -0.1};
        QVERIFY(makeRaster(path, 1000, 800, 3, GDT_Byte, gt, 32615, [](int b, int x, int) {
            return x < 500 ? (b == 1 ? 255 : 0) : (b == 3 ? 255 : 0);
        }));
        const RasterInfo info = RasterInfo::read(path);
        QVERIFY2(info.valid, qPrintable(info.error));
        QCOMPARE(info.width, 1000);
        QCOMPARE(info.bands, 3);
        QCOMPARE(info.crs.epsg(), 32615);
        QCOMPARE(info.extent, QRectF(576600, 5188220, 100, 80));
        QVERIFY(info.summary().contains(QStringLiteral("10,0 см/пикс")));
        const RasterStyle style = RasterStyle::automatic(info, {});
        QCOMPARE(style.mode, RasterStyle::Rgb);

        const Crs utm = Crs::fromEpsg(32615);
        RasterSource source(path, {}, utm);
        QVERIFY2(source.isValid(), qPrintable(source.error()));
        source.setStyle(style);
        QCOMPARE(source.mapExtent(), QRectF(576600, 5188220, 100, 80));
        QCOMPARE(source.maxLevel(), 2);
        const QImage tile = source.renderTile(0, 0, 0);
        QCOMPARE(tile.size(), QSize(256, 256));
        QCOMPARE(QColor(tile.pixel(10, 10)), QColor(255, 0, 0));
        const QImage right = source.renderTile(0, 3, 0);   // x 768..999: blue
        QCOMPARE(right.size(), QSize(232, 256));
        QCOMPARE(QColor(right.pixel(10, 10)), QColor(0, 0, 255));
        const QImage coarse = source.renderTile(2, 0, 0);  // whole raster at 1/4
        QCOMPARE(coarse.size(), QSize(250, 200));
        QVERIFY(source.renderTile(0, 9, 9).isNull());
        QCOMPARE(source.valueAt(QPointF(576600.05, 5188299.95), 1), 255.0);
        QCOMPARE(source.valueAt(QPointF(576699.95, 5188299.95), 1), 0.0);
        QVERIFY(std::isnan(source.valueAt(QPointF(576500, 5188300), 1)));
        const QImage region = source.renderRegion(QRectF(576600, 5188220, 100, 80), QSize(200, 160));
        QCOMPARE(QColor(region.pixel(20, 80)), QColor(255, 0, 0));
        QCOMPARE(QColor(region.pixel(180, 80)), QColor(0, 0, 255));
    }

    // Acceptance of stage C: a GeoTIFF with a known CRS lines up with reference vector geometry
    // whose coordinates come from an independent definition (UTM on the central meridian).
    void geoTiffMatchesReferenceGeometry()
    {
        QTemporaryDir tmp;
        const QString raster = tmp.filePath(QStringLiteral("reference.tif"));
        // Upper-left corner exactly at (lon -93°, lat 45°): E 500 000, N 4 982 950.400.
        const double gt[6] = {500000, 1, 0, 4982950.400, 0, -1};
        QVERIFY(makeRaster(raster, 200, 200, 1, GDT_Float32, gt, 32615, [](int, int x, int y) { return 100 + x * 0.1 + y * 0.01; }));
        const QString vector = tmp.filePath(QStringLiteral("reference.geojson"));
        QVERIFY(writeText(vector, R"({"type":"FeatureCollection","features":[
            {"type":"Feature","properties":{"name":"corner"},"geometry":{"type":"Point","coordinates":[-93.0,45.0]}}]})"));

        // In the raster's CRS: the reference point is the upper-left corner, within 1 cm.
        const Crs utm = Crs::fromEpsg(32615);
        const VectorData points = VectorData::load(vector, {}, {}, utm);
        QVERIFY2(points.valid, qPrintable(points.error));
        QVERIFY(points.crsNote.contains(QStringLiteral("RFC 7946")));
        const QPointF p = points.features.first().parts.first().first();
        RasterSource source(raster, {}, utm);
        QVERIFY(source.isValid());
        const QRectF e = source.mapExtent();
        QVERIFY2(qAbs(p.x() - e.left()) < 0.01 && qAbs(p.y() - e.bottom()) < 0.01,
                 qPrintable(QStringLiteral("%1 %2 vs %3 %4").arg(p.x(), 0, 'f', 3).arg(p.y(), 0, 'f', 3).arg(e.left(), 0, 'f', 3).arg(e.bottom(), 0, 'f', 3)));

        // In WGS 84 the raster is warped: its north-west corner is at (-93°, 45°) within 1e-6°.
        RasterSource warped(raster, {}, Crs::wgs84());
        QVERIFY2(warped.isValid(), qPrintable(warped.error()));
        const QRectF w = warped.mapExtent();
        QVERIFY2(qAbs(w.left() + 93.0) < 1e-5 && qAbs(w.bottom() - 45.0) < 1e-5,
                 qPrintable(QStringLiteral("%1 %2").arg(w.left(), 0, 'f', 8).arg(w.bottom(), 0, 'f', 8)));
        QVERIFY(qAbs(warped.valueAt(QPointF(-92.9999, 44.9999), 1) - 100) < 2);   // near the corner
        QVERIFY(std::isnan(warped.valueAt(QPointF(-93.01, 45.01), 1)));          // outside: no data
    }

    void elevationStyleAndNoData()
    {
        QTemporaryDir tmp;
        const QString path = tmp.filePath(QStringLiteral("dsm.tif"));
        const double gt[6] = {576600, 0.5, 0, 5188300, 0, -0.5};
        QVERIFY(makeRaster(path, 300, 300, 1, GDT_Float32, gt, 32615, [](int, int x, int y) {
            return x < 10 ? -9999.0 : 150.0 + x * 0.05 + y * 0.02;
        }, -9999));
        const RasterInfo info = RasterInfo::read(path);
        QCOMPARE(info.dataType, QStringLiteral("Float32"));
        QCOMPARE(info.noData.value(0), -9999.0);
        const RasterStyle style = RasterStyle::automatic(info, {qMakePair(150.0, 170.0)});
        QCOMPARE(style.mode, RasterStyle::SingleBand);
        QCOMPARE(style.ramp, RasterStyle::Terrain);
        QVERIFY(style.hillshade);
        RasterSource source(path, {}, Crs::fromEpsg(32615));
        source.setStyle(style);
        const QImage tile = source.renderTile(0, 0, 0);
        QCOMPARE(qAlpha(tile.pixel(2, 50)), 0);          // nodata: transparent
        QCOMPARE(qAlpha(tile.pixel(100, 50)), 255);
        QVERIFY(std::isnan(source.valueAt(QPointF(576601, 5188290), 1)));
        QVERIFY(qAbs(source.valueAt(QPointF(576650.1, 5188290.1), 1) - (150 + 100 * 0.05 + 19 * 0.02)) < 1e-3);
        // Styles survive the project file.
        RasterStyle copy = RasterStyle::fromVariant(style.toVariant(), RasterStyle());
        QCOMPARE(copy.mode, style.mode);
        QCOMPARE(copy.ramp, style.ramp);
        QCOMPARE(copy.max, style.max);
        QCOMPARE(RasterStyle::rampColor(RasterStyle::Gray, 1.0), QColor(255, 255, 255));
    }

    void rasterWithoutCrsNeedsOne()
    {
        QTemporaryDir tmp;
        const QString path = tmp.filePath(QStringLiteral("nocrs.tif"));
        const double gt[6] = {576600, 1, 0, 5188300, 0, -1};
        QVERIFY(makeRaster(path, 50, 50, 1, GDT_Byte, gt, 0, [](int, int, int) { return 7; }));
        const RasterInfo info = RasterInfo::read(path);
        QVERIFY(info.hasGeoTransform);
        QVERIFY(!info.crs.isValid());
        QVERIFY(info.summary().contains(QStringLiteral("CRS не задан")));
        RasterSource none(path, {}, Crs::fromEpsg(32615));
        QVERIFY(!none.isValid());
        QVERIFY(none.error().contains(QStringLiteral("систем")));
        RasterSource assigned(path, Crs::fromEpsg(32615), Crs::fromEpsg(32615));
        QVERIFY(assigned.isValid());
        QCOMPARE(assigned.valueAt(QPointF(576610.5, 5188290.5), 1), 7.0);
    }

    void pyramidsAreBuiltBesideNotIntoTheFile()
    {
        QTemporaryDir tmp;
        const QString path = tmp.filePath(QStringLiteral("big.tif"));
        const double gt[6] = {576600, 0.05, 0, 5188300, 0, -0.05};
        QVERIFY(makeRaster(path, 5000, 4200, 1, GDT_Byte, gt, 32615, [](int, int x, int y) { return (x / 100 + y / 100) % 2 ? 200 : 40; }));
        const RasterInfo info = RasterInfo::read(path);
        QVERIFY(PyramidBuilder::needed(info));
        const QDateTime modified = QFileInfo(path).lastModified();
        const qint64 size = QFileInfo(path).size();
        const QString vrt = PyramidBuilder::cachePathFor(path, tmp.filePath(QStringLiteral("cache")));
        QString error;
        // Cancel first: nothing is left behind.
        QVERIFY(!PyramidBuilder::build(path, vrt, [](double) { return false; }, &error));
        QVERIFY(error.contains(QStringLiteral("отменено")) || !error.isEmpty());
        QVERIFY(!PyramidBuilder::isComplete(vrt));
        QVector<double> progress;
        QVERIFY2(PyramidBuilder::build(path, vrt, [&progress](double d) { progress << d; return true; }, &error), qPrintable(error));
        QVERIFY(PyramidBuilder::isComplete(vrt));
        QVERIFY(!progress.isEmpty());
        QCOMPARE(QFileInfo(path).lastModified(), modified);   // the source file is untouched
        QCOMPARE(QFileInfo(path).size(), size);
        QVERIFY(!QFileInfo::exists(path + QStringLiteral(".ovr")));
        const RasterInfo cached = RasterInfo::read(vrt);
        QVERIFY(cached.overviews >= 4);
        QVERIFY(!PyramidBuilder::needed(cached));
        RasterSource source(path, {}, Crs::fromEpsg(32615));
        source.setStyle(RasterStyle::automatic(info, {qMakePair(0.0, 255.0)}));
        source.setPyramidPath(vrt);
        QCOMPARE(source.renderPath(), vrt);
        QVERIFY(!source.renderTile(source.maxLevel(), 0, 0).isNull());
    }

    void vectorsLoadAndRoundTrip()
    {
        QTemporaryDir tmp;
        const QString path = tmp.filePath(QStringLiteral("объекты.geojson"));
        QVERIFY(writeText(path, R"({"type":"FeatureCollection","features":[
          {"type":"Feature","properties":{"name":"т"},"geometry":{"type":"Point","coordinates":[-93.0,45.0]}},
          {"type":"Feature","properties":{"name":"л"},"geometry":{"type":"LineString","coordinates":[[-93.0,45.0],[-92.99,45.0]]}},
          {"type":"Feature","properties":{"name":"п"},"geometry":{"type":"Polygon","coordinates":[[[-93.0,45.0],[-92.99,45.0],[-92.99,45.01],[-93.0,45.0]]]}},
          {"type":"Feature","properties":{"name":"пусто"},"geometry":null}]})"));
        QCOMPARE(VectorData::layerNames(path).size(), 1);
        Crs crs;
        QRectF extent;
        QString error;
        QVERIFY(VectorData::probe(path, {}, &crs, &extent, &error));
        QCOMPARE(crs.epsg(), 4326);
        QVERIFY(qAbs(extent.left() + 93) < 1e-9);
        const Crs utm = Crs::fromEpsg(32615);
        const VectorData data = VectorData::load(path, {}, {}, utm);
        QVERIFY2(data.valid, qPrintable(data.error));
        QCOMPARE(data.features.size(), 3);
        QCOMPARE(data.points, 1);
        QCOMPARE(data.lines, 1);
        QCOMPARE(data.polygons, 1);
        QCOMPARE(data.skipped, 1);
        QCOMPARE(data.features[0].attributes.value("name").toString(), QStringLiteral("т"));
        QVERIFY(qAbs(data.features[0].parts.first().first().x() - 500000) < 1e-4);
        QVERIFY(data.summary().contains(QStringLiteral("полигонов: 1")));

        for (const QString &name : {QStringLiteral("out.gpkg"), QStringLiteral("out.kml"), QStringLiteral("out.geojson")}) {
            const QString out = tmp.filePath(QStringLiteral("экспорт/") + name);
            QDir().mkpath(QFileInfo(out).absolutePath());
            QVERIFY2(writeVectorFile(out, QStringLiteral("drawings"), data.features, utm, &error), qPrintable(name + ": " + error));
            const VectorData back = VectorData::load(out, {}, {}, utm);
            QVERIFY2(back.valid, qPrintable(name + ": " + back.error));
            QCOMPARE(back.features.size(), 3);
            const QPointF a = data.features[1].parts.first().last(), b = back.features[1].parts.first().last();
            QVERIFY2(qAbs(a.x() - b.x()) < 0.01 && qAbs(a.y() - b.y()) < 0.01, qPrintable(name));
            if (name.endsWith(QLatin1String("gpkg"))) QCOMPARE(back.sourceCrs.epsg(), 32615);   // GeoPackage keeps the map CRS
            else QCOMPARE(back.sourceCrs.epsg(), 4326);                                        // GeoJSON/KML are WGS 84
        }
        QVERIFY(!writeVectorFile(tmp.filePath(QStringLiteral("x.shp")), QStringLiteral("x"), data.features, utm, &error));
        // A GeoPackage layer without CRS is not guessed.
        QVERIFY(!VectorData::load(tmp.filePath(QStringLiteral("missing.gpkg")), {}, {}, utm).valid);
    }

    // Background preparation (statistics, pyramids) ends with a clean status, also when the
    // layer is added from the processing view right after another one.
    void mapServiceFinishesRasterPreparation()
    {
        QTemporaryDir tmp;
        const QString project = tmp.filePath(QStringLiteral("p"));
        QVERIFY(QDir().mkpath(project));
        const QString dem = tmp.filePath(QStringLiteral("dsm.tif"));
        const QString wide = tmp.filePath(QStringLiteral("wide.tif"));
        const double gt[6] = {576600, 0.1, 0, 5188300, 0, -0.1};
        QVERIFY(makeRaster(dem, 600, 500, 1, GDT_Float32, gt, 32615, [](int, int x, int y) { return 150.0 + x * 0.01 + y * 0.02; }, -9999));
        QVERIFY(makeRaster(wide, 5000, 300, 1, GDT_Byte, gt, 32615, [](int, int x, int) { return x % 256; }));

        MapService service;
        service.setViewSize(QSizeF(800, 600));
        service.setProject(project, nullptr);
        QCOMPARE(service.addLayerFromPath(dem), QString());
        QCOMPARE(service.addLayerFromPath(wide), QString());
        auto settled = [&](const QString &path) {
            for (const MapLayer &l : service.layers())
                if (l.source == path) return !l.loading && l.status.isEmpty();
            return false;
        };
        auto describe = [&] {
            QStringList s;
            for (const MapLayer &l : service.layers()) s << QStringLiteral("%1: loading=%2 status=%3").arg(l.source).arg(l.loading).arg(l.status);
            return s.join(QStringLiteral("; "));
        };
        QTRY_VERIFY2_WITH_TIMEOUT(settled(dem), qPrintable(describe()), 15000);
        QTRY_VERIFY2_WITH_TIMEOUT(settled(wide), qPrintable(describe()), 30000);
        for (const MapLayer &l : service.layers())
            if (l.source == dem) {
                QVERIFY(qAbs(l.style.min - 150.0) < 0.5);   // from the statistics, nodata excluded
                QVERIFY(l.style.max > 160.0);
            }
    }

    void mapServiceMeasuresDrawsAndPersists()
    {
        QTemporaryDir tmp;
        const QString project = tmp.filePath(QStringLiteral("Проект карты"));
        QVERIFY(QDir().mkpath(project));
        PhotoModel photos;
        QVector<Photo> list;
        for (int i = 0; i < 3; ++i) {
            Photo p;
            p.id = QString::number(i);
            p.name = QStringLiteral("DJI_%1.JPG").arg(i);
            p.hasGps = true;
            p.longitude = -92.0 + i * 0.001;   // Brighton-like: UTM zone 15
            p.latitude = 46.84;
            list << p;
        }
        photos.resetPhotos(list, project);

        auto service = std::make_unique<MapService>();
        service->setViewSize(QSizeF(1000, 800));
        service->setProject(project, &photos);
        // Without other data the photos choose the CRS: their UTM zone.
        QCOMPARE(service->crs().epsg(), 32615);
        QVERIFY2(service->crsNote().contains(QStringLiteral("UTM-зона 15N")), qPrintable(service->crsNote()));
        QCOMPARE(service->layerCount(), 2);   // photos and drawings

        const QString raster = tmp.filePath(QStringLiteral("ortho.tif"));
        const double gt[6] = {499900, 0.5, 0, 5188400, 0, -0.5};
        QVERIFY(makeRaster(raster, 400, 400, 3, GDT_Byte, gt, 32615, [](int b, int, int) { return b == 2 ? 200 : 30; }));
        QCOMPARE(service->addLayerFromPath(raster), QString());
        QCOMPARE(service->layerCount(), 3);
        QVERIFY(service->addLayerFromPath(raster).contains(QStringLiteral("уже")));
        QVERIFY(!service->addLayerFromPath(tmp.filePath(QStringLiteral("нет.tif"))).isEmpty());
        const QImage image = service->renderMap(QSize(500, 400), false);
        QCOMPARE(QColor(image.pixel(250, 200)), QColor(30, 200, 30));   // the raster is centred

        // Geodesic measurement in UTM: on the central meridian ground = grid / k0 (0.9996).
        const QVector<QPointF> square{QPointF(500000, 5188000), QPointF(500100, 5188000), QPointF(500100, 5188100), QPointF(500000, 5188100)};
        const MapService::Measure m = service->measure(square, true);
        QVERIFY2(qAbs(m.area - 10000 / (0.9996 * 0.9996)) < 0.05, qPrintable(QString::number(m.area, 'f', 4)));
        QCOMPARE(m.planarArea, 10000.0);
        QVERIFY(qAbs(m.perimeter - 400 / 0.9996) < 0.01);
        const MapService::Measure line = service->measure({QPointF(500000, 5188000), QPointF(500000, 5189000)}, false);
        QVERIFY(qAbs(line.length - 1000 / 0.9996) < 0.01);
        QVERIFY(MapService::formatArea(12345.6).startsWith(QStringLiteral("1,2346 га")));
        QVERIFY(MapService::formatLength(1234.5).contains(QStringLiteral("м")));

        // Drawing with the pointer: polygon, undo, redo, select and delete.
        service->setView(QPointF(500000, 5188000), 1.0);
        service->setTool(QStringLiteral("polygon"));
        for (const QPointF &s : {QPointF(500, 400), QPointF(600, 400), QPointF(600, 300)}) {
            service->pointerPressed(s, Qt::LeftButton, Qt::NoModifier);
            service->pointerReleased(s);
        }
        QVERIFY(service->measurementText().contains(QStringLiteral("площадь")));
        service->finishSketch();
        QCOMPARE(service->drawingCount(), 1);
        QVERIFY(service->canUndo());
        service->undo();
        QCOMPARE(service->drawingCount(), 0);
        service->redo();
        QCOMPARE(service->drawingCount(), 1);
        service->setTool(QStringLiteral("point"));
        service->pointerPressed(QPointF(100, 100), Qt::LeftButton, Qt::NoModifier);
        service->pointerReleased(QPointF(100, 100));
        QCOMPARE(service->drawingCount(), 2);
        service->setTool(QStringLiteral("edit"));
        service->pointerPressed(QPointF(580, 380), Qt::LeftButton, Qt::NoModifier);   // inside the polygon
        service->pointerReleased(QPointF(580, 380));
        QVERIFY(service->hasSelection());
        // Drag a vertex, then undo the move.
        service->pointerPressed(QPointF(600, 300), Qt::LeftButton, Qt::NoModifier);
        service->pointerDragged(QPointF(650, 250));
        service->pointerReleased(QPointF(650, 250));
        QVERIFY(service->undoText().contains(QStringLiteral("вершины")));
        service->deleteSelected();
        QCOMPARE(service->drawingCount(), 1);
        service->undo();
        QCOMPARE(service->drawingCount(), 2);

        // Export of drawings and of the map.
        const QString exported = tmp.filePath(QStringLiteral("объекты.gpkg"));
        QCOMPARE(service->exportDrawings(QUrl::fromLocalFile(exported)), QString());
        const VectorData back = VectorData::load(exported, {}, {}, Crs::fromEpsg(32615));
        QCOMPARE(back.features.size(), 2);
        bool hasArea = false;
        for (const VectorFeature &f : back.features) hasArea = hasArea || f.attributes.contains(QStringLiteral("area_m2"));
        QVERIFY(hasArea);
        {   // measurements are numeric fields, not text
            GDALDatasetH ds = GDALOpenEx(exported.toUtf8().constData(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
            QVERIFY(ds);
            OGRFeatureDefnH defn = OGR_L_GetLayerDefn(GDALDatasetGetLayer(ds, 0));
            const int area = OGR_FD_GetFieldIndex(defn, "area_m2");
            QVERIFY(area >= 0);
            QCOMPARE(OGR_Fld_GetType(OGR_FD_GetFieldDefn(defn, area)), OFTReal);
            QCOMPARE(OGR_Fld_GetType(OGR_FD_GetFieldDefn(defn, OGR_FD_GetFieldIndex(defn, "name"))), OFTString);
            GDALClose(ds);
        }
        QSignalSpy exportDone(service.get(), &MapService::exportFinished);
        service->exportMap(QUrl::fromLocalFile(tmp.filePath(QStringLiteral("карта.pdf"))));
        QVERIFY(exportDone.wait(30000));
        QVERIFY(exportDone.first().first().toBool());
        QVERIFY(QFileInfo(tmp.filePath(QStringLiteral("карта.pdf"))).size() > 10000);
        service->exportMap(QUrl::fromLocalFile(tmp.filePath(QStringLiteral("карта.png"))));
        QVERIFY(exportDone.wait(30000));
        QCOMPARE(QImage(tmp.filePath(QStringLiteral("карта.png"))).width(), 2000);
        // For a look at the layout: GEOMAP_TEST_OUTPUT=<folder> keeps copies of both exports.
        const QString keep = qEnvironmentVariable("GEOMAP_TEST_OUTPUT");
        if (!keep.isEmpty() && QDir().mkpath(keep))
            for (const QString &name : {QStringLiteral("карта.pdf"), QStringLiteral("карта.png")}) {
                QFile::remove(QDir(keep).filePath(name));
                QFile::copy(tmp.filePath(name), QDir(keep).filePath(name));
            }

        // Persistence: closing the project writes map.json and the drawings; reopening restores them.
        service->setLayerOpacity(service->layers()[0].id, 0.5);
        service->setProject(QString(), nullptr);
        QVERIFY(QFileInfo::exists(QDir(project).filePath(QStringLiteral("map.json"))));
        QVERIFY(QFileInfo::exists(QDir(project).filePath(QStringLiteral("vectors/drawings.geojson"))));
        auto reopened = std::make_unique<MapService>();
        reopened->setViewSize(QSizeF(1000, 800));
        reopened->setProject(project, &photos);
        QCOMPARE(reopened->crs().epsg(), 32615);
        QCOMPARE(reopened->layerCount(), 3);
        QCOMPARE(reopened->drawingCount(), 2);
        QCOMPARE(reopened->layers()[0].opacity, 0.5);
        QCOMPARE(reopened->center(), QPointF(500000, 5188000));
        const QJsonObject state = QJsonDocument::fromJson([&] {
            QFile f(QDir(project).filePath(QStringLiteral("map.json")));
            f.open(QIODevice::ReadOnly);
            return f.readAll();
        }()).object();
        QCOMPARE(state.value("crsEpsg").toInt(), 32615);
        // A raster outside the project is stored with its absolute path, one inside relatively.
        QVERIFY(QDir::isAbsolutePath(state.value("layers").toArray().first().toObject().value("source").toString()));

        // Changing the CRS of the map reprojects everything; drawings survive through WGS 84.
        QCOMPARE(reopened->setMapCrs(QStringLiteral("EPSG:3857")), QString());
        QCOMPARE(reopened->crs().epsg(), 3857);
        QCOMPARE(reopened->drawingCount(), 2);
        QVERIFY(!reopened->setMapCrs(QStringLiteral("EPSG:0")).isEmpty());
    }
};

// Map export draws text (fonts need QGuiApplication); Linux CI runs it offscreen.
QTEST_MAIN(GisTest)
#include "test_gis.moc"
