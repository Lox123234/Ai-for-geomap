#include <QtTest>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>
#include <QtEndian>
#include <cmath>
#include <functional>
#include <gdal.h>
#include <ogr_srs_api.h>
#include "Camera3D.h"
#include "Crs.h"
#include "GisEnvironment.h"
#include "LasEdit.h"
#include "Lidar.h"
#include "OdmTools.h"
#include "Scene3D.h"
#include "Scene3DService.h"
#include "WebTiles.h"

using namespace view3d;

namespace {

bool writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}

template<typename T> void put(QByteArray &a, T v)
{
    char b[sizeof(T)];
    qToLittleEndian(v, b);
    a.append(b, sizeof(T));
}

// Binary little-endian PLY: float x y z + uchar red green blue.
QByteArray binaryPly(const QVector<QVector3D> &points, const QVector<QColor> &colors)
{
    QByteArray a = "ply\nformat binary_little_endian 1.0\ncomment test\nelement vertex " + QByteArray::number(points.size())
                   + "\nproperty float x\nproperty float y\nproperty float z\nproperty uchar red\nproperty uchar green\n"
                     "property uchar blue\nend_header\n";
    for (int i = 0; i < points.size(); ++i) {
        put(a, points[i].x());
        put(a, points[i].y());
        put(a, points[i].z());
        a.append(char(colors[i].red())).append(char(colors[i].green())).append(char(colors[i].blue()));
    }
    return a;
}

// LAS 1.2, point format 2 (XYZ, RGB 16 bit), optionally with a CRS record (OGC WKT).
QByteArray las12(const QVector<QVector3D> &points, const QVector<QColor> &colors, double ox, double oy, double scale,
                 const QByteArray &wkt = {}, const QVector<quint8> &classes = {}, const QVector<quint16> &intensities = {})
{
    QByteArray vlr;
    if (!wkt.isEmpty()) {
        vlr = QByteArray(54, '\0');
        memcpy(vlr.data() + 2, "LASF_Projection", 15);
        qToLittleEndian(quint16(2112), vlr.data() + 18);
        qToLittleEndian(quint16(wkt.size() + 1), vlr.data() + 20);
        vlr += wkt + '\0';
    }
    QByteArray h(227, '\0');
    memcpy(h.data(), "LASF", 4);
    h[24] = 1;
    h[25] = 2;
    const auto set16 = [&h](int o, quint16 v) { qToLittleEndian(v, h.data() + o); };
    const auto set32 = [&h](int o, quint32 v) { qToLittleEndian(v, h.data() + o); };
    const auto setD = [&h](int o, double v) { qToLittleEndian(v, h.data() + o); };
    set16(94, 227);
    set32(96, quint32(227 + vlr.size()));
    set32(100, wkt.isEmpty() ? 0 : 1);
    h[104] = 2;
    set16(105, 26);
    set32(107, quint32(points.size()));
    setD(131, scale); setD(139, scale); setD(147, scale);
    setD(155, ox); setD(163, oy); setD(171, 0);
    double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;
    for (const QVector3D &p : points) {
        minX = qMin(minX, ox + p.x()); maxX = qMax(maxX, ox + p.x());
        minY = qMin(minY, oy + p.y()); maxY = qMax(maxY, oy + p.y());
    }
    setD(179, maxX); setD(187, minX); setD(195, maxY); setD(203, minY);
    QByteArray a = h + vlr;
    for (int i = 0; i < points.size(); ++i) {
        put(a, qint32(std::lround(points[i].x() / scale)));
        put(a, qint32(std::lround(points[i].y() / scale)));
        put(a, qint32(std::lround(points[i].z() / scale)));
        put(a, quint16(i < intensities.size() ? intensities[i] : 0));   // intensity
        a.append('\0');                                                 // return flags
        a.append(char(i < classes.size() ? classes[i] : 0));             // classification
        a.append(QByteArray(4, '\0'));                                  // scan angle, user data, source id
        put(a, quint16(colors[i].red() * 257));
        put(a, quint16(colors[i].green() * 257));
        put(a, quint16(colors[i].blue() * 257));
    }
    return a;
}

bool makeRaster(const QString &path, int w, int h, int bands, GDALDataType type, const double gt[6], int epsg,
                const std::function<double(int, int, int)> &value, double noData = std::nan(""))
{
    GDALDriverH driver = GDALGetDriverByName("GTiff");
    GDALDatasetH ds = GDALCreate(driver, QDir::toNativeSeparators(path).toUtf8().constData(), w, h, bands, type, nullptr);
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

// Pixel of a scene point in a viewport (origin top-left), as the view draws it.
QPointF project(const Camera &camera, const QVector3D &p, const QSizeF &viewport)
{
    const QMatrix4x4 m = camera.projection(float(viewport.width() / viewport.height()), 0.1f, 10000.0f) * camera.view();
    const QVector4D c = m * QVector4D(p, 1.0f);
    return QPointF((c.x() / c.w() + 1.0) * 0.5 * viewport.width(), (1.0 - c.y() / c.w()) * 0.5 * viewport.height());
}

bool waitLoaded(Scene3DService &scene, int timeoutMs = 120000)
{
    QElapsedTimer t;
    t.start();
    while (scene.loading() && t.elapsed() < timeoutMs) QTest::qWait(20);
    return !scene.loading();
}

} // namespace

class View3DTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QVERIFY2(GisEnvironment::initialize(QCoreApplication::applicationDirPath()), qPrintable(GisEnvironment::problems().join('\n')));
        QVERIFY(m_dir.isValid());
    }

    void plyBinaryWithColours()
    {
        const QString path = m_dir.filePath(QStringLiteral("cloud.ply"));
        const QVector<QVector3D> pts = {{1000.5f, 2000.25f, 10}, {1001.5f, 2001.25f, 12}, {1002.5f, 2000.25f, 14}};
        QVERIFY(writeFile(path, binaryPly(pts, {Qt::red, Qt::green, Qt::blue})));
        const PointCloud c = readPointCloud(path, 0);
        QVERIFY2(c.isValid(), qPrintable(c.error));
        QCOMPARE(c.positions.size(), 3);
        QCOMPARE(c.totalPoints, quint64(3));
        // No ODM offset: the origin is the first point rounded down, positions relative to it.
        QVERIFY(c.origin.valid);
        QCOMPARE(c.origin.x, 1000.0);
        QCOMPARE(c.origin.y, 2000.0);
        QCOMPARE(c.positions[1], QVector3D(1.5f, 1.25f, 12));
        QCOMPARE(c.colors[0], 0xffff0000u);
        QCOMPARE(c.colors[2], 0xff0000ffu);
        QCOMPARE(c.bounds.min, QVector3D(0.5f, 0.25f, 10));
        QCOMPARE(c.bounds.max, QVector3D(2.5f, 1.25f, 14));

        // A budget keeps every k-th point and reports the total.
        QVector<QVector3D> many;
        QVector<QColor> colors;
        for (int i = 0; i < 1000; ++i) { many << QVector3D(i, 0, 0); colors << Qt::white; }
        QVERIFY(writeFile(path, binaryPly(many, colors)));
        const PointCloud thin = readPointCloud(path, 100);
        QCOMPARE(thin.totalPoints, quint64(1000));
        QCOMPARE(thin.positions.size(), 100);
        QCOMPARE(thin.positions[1].x() - thin.positions[0].x(), 10.0f);
    }

    void plyAsciiDoubleAndOdmOffset()
    {
        // ODM: odm_filterpoints/point_cloud.ply is local to odm_georeferencing/coords.txt.
        const QString job = m_dir.filePath(QStringLiteral("job"));
        QVERIFY(writeFile(job + QStringLiteral("/odm_georeferencing/coords.txt"), "WGS84 UTM 15N\n576705 5188170\n1 2 3\n"));
        const QString path = job + QStringLiteral("/odm_filterpoints/point_cloud.ply");
        QVERIFY(writeFile(path, "ply\nformat ascii 1.0\nelement vertex 2\nproperty double x\nproperty double y\nproperty double z\n"
                                "property uchar diffuse_red\nproperty uchar diffuse_green\nproperty uchar diffuse_blue\n"
                                "element face 0\nproperty list uchar int vertex_indices\nend_header\n"
                                "-10.5 4.25 160.5 255 128 0\n3 -2 158 0 0 0\n"));
        Origin o;
        QString crs;
        QVERIFY(readOdmOffset(odmCoordsFor(path), &o, &crs));
        QCOMPARE(crs, QStringLiteral("WGS84 UTM 15N"));
        QCOMPARE(o.x, 576705.0);
        QCOMPARE(o.y, 5188170.0);
        const PointCloud c = readPointCloud(path, 0);
        QVERIFY2(c.isValid(), qPrintable(c.error));
        QCOMPARE(c.origin.x, 576705.0);
        QCOMPARE(c.positions[0], QVector3D(-10.5f, 4.25f, 160.5f));   // kept local
        QCOMPARE(c.colors[0], 0xffff8000u);
        QVERIFY(c.georeferenced);
        QCOMPARE(c.crs, QStringLiteral("WGS84 UTM 15N"));
    }

    void lasPointFormat2()
    {
        const QString path = m_dir.filePath(QStringLiteral("cloud.las"));
        const QVector<QVector3D> pts = {{0.5f, 1.25f, 150.5f}, {10, 20, 155}, {5, 5, 152.25f}};
        QVERIFY(writeFile(path, las12(pts, {Qt::red, Qt::green, QColor(10, 20, 30)}, 576000.0, 5188000.0, 0.01)));
        const PointCloud c = readPointCloud(path, 0);
        QVERIFY2(c.isValid(), qPrintable(c.error));
        QCOMPARE(c.totalPoints, quint64(3));
        QCOMPARE(c.origin.x, 576000.0);   // min X, rounded down
        QCOMPARE(c.origin.y, 5188001.0);  // min Y = 5188001.25
        QCOMPARE(c.positions[0], QVector3D(0.5f, 0.25f, 150.5f));
        QCOMPARE(c.positions[2], QVector3D(5, 4, 152.25f));
        QCOMPARE(c.colors[1], 0xff00ff00u);
        QCOMPARE(c.colors[2], 0xff0a141eu);
        QVERIFY(!c.georeferenced);   // no CRS record

        // With a CRS record: georeferenced, the CRS name read from the WKT.
        const QString geoPath = m_dir.filePath(QStringLiteral("cloud-utm.las"));
        QVERIFY(writeFile(geoPath, las12(pts, {Qt::red, Qt::green, Qt::blue}, 576000.0, 5188000.0, 0.01,
                                         "PROJCS[\"WGS 84 / UTM zone 15N\",GEOGCS[\"WGS 84\"]]")));
        const PointCloud g = readPointCloud(geoPath, 0);
        QVERIFY2(g.isValid(), qPrintable(g.error));
        QVERIFY(g.georeferenced);
        QCOMPARE(g.crs, QStringLiteral("WGS 84 / UTM zone 15N"));
        QCOMPARE(g.positions[2], QVector3D(5, 4, 152.25f));

        // LAZ is reported, not misread.
        const QString laz = m_dir.filePath(QStringLiteral("cloud.laz"));
        QVERIFY(QFile::copy(path, laz));
        const PointCloud z = readPointCloud(laz, 0);
        QVERIFY(!z.isValid());
        QVERIFY(z.error.contains(QStringLiteral("LAZ")));
        // Not a cloud at all.
        QVERIFY(writeFile(m_dir.filePath(QStringLiteral("bad.las")), "nothing"));
        QVERIFY(!readPointCloud(m_dir.filePath(QStringLiteral("bad.las")), 0).isValid());
        QVERIFY(!readPointCloud(m_dir.filePath(QStringLiteral("x.xyz")), 0).isValid());
    }

    void lasClassesAndIntensity()
    {
        // 100 points: classes 2 (ground) ×60, 5 (high vegetation) ×30, 6 (building) ×10; intensity 0..990.
        QVector<QVector3D> pts;
        QVector<QColor> colors;
        QVector<quint8> classes;
        QVector<quint16> intensity;
        for (int i = 0; i < 100; ++i) {
            pts << QVector3D(i, 0, 150);
            colors << Qt::white;
            classes << quint8(i < 60 ? 2 : i < 90 ? 5 : 6);
            intensity << quint16(i * 10);
        }
        const QString path = m_dir.filePath(QStringLiteral("classes.las"));
        QVERIFY(writeFile(path, las12(pts, colors, 576000, 5188000, 0.01, {}, classes, intensity)));
        const PointCloud c = readPointCloud(path, 0);
        QVERIFY2(c.isValid(), qPrintable(c.error));
        QCOMPARE(c.classes.size(), 100);
        QCOMPARE(int(c.classes[0]), 2);
        QCOMPARE(int(c.classes[95]), 6);
        QCOMPARE(c.intensity.size(), 100);
        QVERIFY(c.intensity[0] == 0 && c.intensity[99] == 255);   // 1st..99th percentile stretched
        QVERIFY(c.intensity[50] > 110 && c.intensity[50] < 145);
        QCOMPARE(classColor(2), 0xffa6763cu);
        QCOMPARE(className(6), QStringLiteral("здания"));
        // The scene describes the classes, largest first.
        Scene3DService scene;
        QVERIFY(scene.addPointCloud(path).isEmpty());
        QVERIFY(waitLoaded(scene));
        const QString info = scene.layersForQml().first().toMap().value("info").toString();
        QVERIFY2(info.contains(QStringLiteral("Классы: земля 60 %, высокая растительность 30 %, здания 10 %")), qPrintable(info));
        QVERIFY(info.contains(QStringLiteral("интенсивность")));
        scene.setColorMode(QStringLiteral("classes"));
        QCOMPARE(scene.colorMode(), QStringLiteral("classes"));
        QCOMPARE(scene.snapshot().colorMode, 2);
        scene.setColorMode(QStringLiteral("intensity"));
        QCOMPARE(scene.snapshot().colorMode, 3);
        // An ODM LAZ with an LAS beside it: the LAS is shown (georeferenced, classes).
        const QString job = m_dir.filePath(QStringLiteral("lasjob/odm_georeferencing"));
        QVERIFY(QDir().mkpath(job));
        QVERIFY(QFile::copy(path, job + QStringLiteral("/odm_georeferenced_model.las")));
        QVERIFY(writeFile(job + QStringLiteral("/odm_georeferenced_model.laz"), "LASF compressed"));
        QVERIFY(writeFile(job + QStringLiteral("/coords.txt"), "WGS84 UTM 15N\n576000 5188000\n"));
        Scene3DService other;
        QVERIFY(other.addPointCloud(job + QStringLiteral("/odm_georeferenced_model.laz")).isEmpty());
        QVERIFY(waitLoaded(other));
        const QString otherInfo = other.layersForQml().first().toMap().value("info").toString();
        QVERIFY2(otherInfo.contains(QStringLiteral("odm_georeferenced_model.las")), qPrintable(otherInfo));
        QVERIFY(otherInfo.contains(QStringLiteral("Классы")));
    }

    // Editing of a LAS cloud: 20 × 20 m of ground at 100 m, a green "tree" 5 m high, a grey
    // "building" 6 m high and one point under the ground; EPSG:32615 in the file.
    void lasEditAndClassification()
    {
        QVector<QVector3D> pts;
        QVector<QColor> colors;
        for (int y = 0; y < 20; ++y)
            for (int x = 0; x < 20; ++x) {
                const bool tree = x >= 2 && x <= 6 && y >= 2 && y <= 6, building = x >= 12 && x <= 15 && y >= 12 && y <= 15;
                pts << QVector3D(x + 0.5f, y + 0.5f, tree ? 105.0f : building ? 106.0f : 100.0f);
                colors << (tree ? QColor(40, 150, 40) : QColor(130, 130, 130));
            }
        pts << QVector3D(10.5f, 10.5f, 97.0f);   // noise below the ground
        colors << QColor(128, 128, 128);
        const QByteArray wkt = Crs::fromEpsg(32615).wkt().toUtf8();
        const QString path = m_dir.filePath(QStringLiteral("edit.las"));
        QVERIFY(writeFile(path, las12(pts, colors, 576000, 5188000, 0.001, wkt)));
        QCOMPARE(las::fileCrs(path).epsg(), 32615);

        const QPolygonF building({QPointF(576011.8, 5188011.8), QPointF(576016.2, 5188011.8), QPointF(576016.2, 5188016.2), QPointF(576011.8, 5188016.2)});
        const QString deleted = m_dir.filePath(QStringLiteral("out/deleted.las"));
        las::Result r = las::edit(path, deleted, las::Operation::DeleteInside, {building}, Crs::fromEpsg(32615));
        QVERIFY2(r.ok(), qPrintable(r.error));
        QCOMPARE(r.total, 401);
        QCOMPARE(r.changed, 16);
        QCOMPARE(r.written, 385);
        PointCloud c = readPointCloud(deleted, 0);
        QVERIFY2(c.isValid(), qPrintable(c.error));
        QCOMPARE(c.totalPoints, quint64(385));
        QCOMPARE(las::fileCrs(deleted).epsg(), 32615);   // the CRS record is kept
        // The building is gone from the header bounds too: the highest point is now the tree.
        QFile f(deleted);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray header = f.read(227);
        QCOMPARE(qFromLittleEndian<double>(header.constData() + 211), 105.0);
        QCOMPARE(qFromLittleEndian<double>(header.constData() + 219), 97.0);
        QCOMPARE(qFromLittleEndian<quint32>(header.constData() + 107), 385u);

        // Keep inside (the same polygon given in WGS 84), classify inside.
        QVector<QPointF> geo(building.begin(), building.end());
        QVERIFY(CoordinateTransform(Crs::fromEpsg(32615), Crs::wgs84()).transform(geo));
        r = las::edit(path, m_dir.filePath(QStringLiteral("out/kept.las")), las::Operation::KeepInside, {QPolygonF(geo)}, Crs::wgs84());
        QVERIFY2(r.ok(), qPrintable(r.error));
        QCOMPARE(r.written, 16);
        r = las::edit(path, m_dir.filePath(QStringLiteral("out/classified-inside.las")), las::Operation::ClassifyInside, {building},
                      Crs::fromEpsg(32615), 6);
        QCOMPARE(r.changed, 16);
        QCOMPARE(r.classes.value(6), 16);
        QCOMPARE(r.written, 401);

        // Classification by the height above a flat DTM and the colour.
        const QString dtm = m_dir.filePath(QStringLiteral("flat-dtm.tif"));
        const double gt[6] = {576000, 1, 0, 5188020, 0, -1};
        QVERIFY(makeRaster(dtm, 20, 20, 1, GDT_Float32, gt, 32615, [](int, int, int) { return 100.0; }));
        r = las::classify(path, m_dir.filePath(QStringLiteral("out/classes.las")), dtm);
        QVERIFY2(r.ok(), qPrintable(r.error));
        QCOMPARE(r.classes.value(2), 359);   // ground
        QCOMPARE(r.classes.value(5), 25);    // high vegetation (the tree)
        QCOMPARE(r.classes.value(6), 16);    // building
        QCOMPARE(r.classes.value(7), 1);     // noise
        c = readPointCloud(r.path, 0);
        QCOMPARE(c.classes.size(), 401);
        // Refusals: compressed LAZ, no outline.
        QVERIFY(!las::edit(m_dir.filePath(QStringLiteral("x.laz")), m_dir.filePath(QStringLiteral("out/x.las")), las::Operation::DeleteInside,
                           {building}, Crs::fromEpsg(32615)).ok());
        QVERIFY(!las::edit(path, m_dir.filePath(QStringLiteral("out/y.las")), las::Operation::DeleteInside, {}, Crs::fromEpsg(32615)).ok());
    }

    void objWithMaterialsAndTexture()
    {
        const QString dir = m_dir.filePath(QStringLiteral("obj"));
        QImage tex(4, 2, QImage::Format_RGB32);
        tex.fill(Qt::red);
        for (int x = 0; x < 4; ++x) tex.setPixelColor(x, 1, Qt::blue);   // bottom row blue
        QDir().mkpath(dir);
        QVERIFY(tex.save(dir + QStringLiteral("/tex.png")));
        QVERIFY(writeFile(dir + QStringLiteral("/m.mtl"), "newmtl textured\nKd 1 1 1\nmap_Kd tex.png\nnewmtl plain\nKd 0.5 0.5 0.5\n"));
        // A textured quad, then a pentagon with negative indices in another material.
        QVERIFY(writeFile(dir + QStringLiteral("/m.obj"),
                          "mtllib m.mtl\n"
                          "v 0 0 1\nv 2 0 1\nv 2 2 1\nv 0 2 1\n"
                          "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
                          "vn 0 0 1\n"
                          "usemtl textured\n"
                          "f 1/1/1 2/2/1 3/3/1 4/4/1\n"
                          "v 10 0 0\nv 11 0 0\nv 12 1 0\nv 11 2 0\nv 10 1 0\n"
                          "usemtl plain\n"
                          "f -5 -4 -3 -2 -1\n"));
        const Mesh m = readObj(dir + QStringLiteral("/m.obj"));
        QVERIFY2(m.isValid(), qPrintable(m.error));
        QCOMPARE(m.parts.size(), 2);
        QCOMPARE(m.triangles, quint64(2 + 3));
        QCOMPARE(m.parts[0].indices.size(), 6);
        QCOMPARE(m.parts[1].indices.size(), 9);
        QVERIFY(!m.parts[0].texture.isNull());
        QVERIFY(m.parts[1].texture.isNull());
        QCOMPARE(m.parts[0].texture.size(), QSize(4, 2));
        // OBJ v grows upwards, image rows downwards: vt (0,0) is the image's bottom-left.
        const quint32 first = m.parts[0].indices[0];
        QCOMPARE(m.uvs[int(first)], QVector2D(0, 1));
        QCOMPARE(m.parts[0].texture.pixelColor(0, 1), QColor(Qt::blue));
        QCOMPARE(m.bounds.min, QVector3D(0, 0, 0));
        QCOMPARE(m.bounds.max, QVector3D(12, 2, 1));
        QVERIFY(!m.origin.valid);   // not an ODM model
        QVERIFY(!m.georeferenced);

        // Too large textures are scaled down with a note.
        const Mesh small = readObj(dir + QStringLiteral("/m.obj"), 2);
        QCOMPARE(small.parts[0].texture.width(), 2);
        QVERIFY(small.note.contains(QStringLiteral("уменьшена")));
        // Broken index.
        QVERIFY(writeFile(dir + QStringLiteral("/bad.obj"), "v 0 0 0\nf 1 2 3\n"));
        QVERIFY(!readObj(dir + QStringLiteral("/bad.obj")).isValid());
    }

    void plyMesh()
    {
        const QString path = m_dir.filePath(QStringLiteral("mesh.ply"));
        QVERIFY(writeFile(path, "ply\nformat ascii 1.0\nelement vertex 4\nproperty float x\nproperty float y\nproperty float z\n"
                                "element face 1\nproperty list uchar int vertex_indices\nend_header\n"
                                "100 200 5\n101 200 5\n101 201 6\n100 201 6\n4 0 1 2 3\n"));
        const Mesh m = readMeshPly(path);
        QVERIFY2(m.isValid(), qPrintable(m.error));
        QCOMPARE(m.triangles, quint64(2));
        QCOMPARE(m.normals.size(), 4);
        QVERIFY(m.normals[0].z() > 0.5f);
        QCOMPARE(m.origin.x, 100.0);
        QCOMPARE(m.positions[2], QVector3D(1, 1, 6));
    }

    void demTerrainAndDrape()
    {
        // 10 × 8 DEM, 2 m cells, UTM 15N; one nodata cell; heights rise to the east.
        const double gt[6] = {576000, 2, 0, 5188016, 0, -2};
        const QString dem = m_dir.filePath(QStringLiteral("dem.tif"));
        QVERIFY(makeRaster(dem, 10, 8, 1, GDT_Float32, gt, 32615,
                           [](int, int x, int y) { return (x == 3 && y == 4) ? -9999.0 : 150.0 + x; }, -9999.0));
        const Mesh m = demMesh(dem, 1024);
        QVERIFY2(m.isValid(), qPrintable(m.error));
        QCOMPARE(m.positions.size(), 79);
        // 9 × 7 quads = 126 triangles, the hole removes the triangles touching it (6).
        QCOMPARE(m.triangles, quint64(126 - 6));
        QCOMPARE(m.origin.x, 576000.0);
        QCOMPARE(m.origin.y, 5188000.0);   // bottom edge
        QVERIFY(m.georeferenced);
        QVERIFY2(m.crs.contains(QStringLiteral("UTM zone 15N")), qPrintable(m.crs));
        QCOMPARE(m.bounds.min.z(), 150.0f);
        QCOMPARE(m.bounds.max.z(), 159.0f);
        // First vertex: centre of the top-left cell.
        QCOMPARE(m.positions[0], QVector3D(1, 15, 150));
        QCOMPARE(m.colors.size(), m.positions.size());
        QVERIFY(m.uvs.isEmpty());
        // Normals lean west (heights rise to the east).
        QVERIFY(m.normals[0].x() < -0.1f);

        // A coarser grid averages cells.
        const Mesh coarse = demMesh(dem, 4);
        QVERIFY(coarse.positions.size() <= 16);

        // Relative to a given origin.
        const Mesh shifted = demMesh(dem, 1024, Origin{576010, 5188010, 0, true});
        QCOMPARE(shifted.positions[0], QVector3D(-9, 5, 150));

        // Orthophoto 4× finer over a larger area in the same CRS: drape cut to the DEM.
        const double ogt[6] = {575990, 0.5, 0, 5188026, 0, -0.5};
        const QString ortho = m_dir.filePath(QStringLiteral("ortho.tif"));
        QVERIFY(makeRaster(ortho, 120, 80, 3, GDT_Byte, ogt, 32615, [](int b, int x, int) { return b == 1 ? (x < 60 ? 255 : 0) : 0; }));
        QString note;
        const QImage drape = readDrape(ortho, dem, 4096, &note);
        QVERIFY2(!drape.isNull(), qPrintable(note));
        QCOMPARE(drape.size(), QSize(40, 32));
        // Ortho column 60 is x = 576020: the DEM's right edge — all red inside the DEM.
        QCOMPARE(QColor(drape.pixel(39, 10)).red(), 255);
        const Mesh draped = demMesh(dem, 1024, {}, drape);
        QCOMPARE(draped.uvs.size(), draped.positions.size());
        QVERIFY(!draped.parts[0].texture.isNull());

        // Another CRS: no drape, with a reason.
        const QString other = m_dir.filePath(QStringLiteral("ortho32632.tif"));
        QVERIFY(makeRaster(other, 120, 80, 3, GDT_Byte, ogt, 32632, [](int, int, int) { return 100; }));
        QVERIFY(readDrape(other, dem, 4096, &note).isNull());
        QVERIFY(note.contains(QStringLiteral("системе координат")));
    }

    void cameraGeometry()
    {
        Bounds b;
        b.add(QVector3D(-50, -50, 0));
        b.add(QVector3D(50, 50, 20));
        Camera cam;
        cam.aspect = 800.0f / 600.0f;
        cam.setPreset(Camera::Iso, b);
        QCOMPARE(cam.target, QVector3D(0, 0, 10));
        // The whole box is in view.
        for (const QVector3D &corner : {b.min, b.max, QVector3D(-50, 50, 0), QVector3D(50, -50, 20)}) {
            const QPointF px = project(cam, corner, QSizeF(800, 600));
            QVERIFY2(px.x() > 0 && px.x() < 800 && px.y() > 0 && px.y() < 600, qPrintable(QStringLiteral("%1 %2").arg(px.x()).arg(px.y())));
        }
        // The target projects to the centre, and the central ray points at it.
        const QPointF c = project(cam, cam.target, QSizeF(800, 600));
        QVERIFY(std::abs(c.x() - 400) < 0.01 && std::abs(c.y() - 300) < 0.01);
        QVector3D o, d;
        cam.ray(QPointF(400, 300), QSizeF(800, 600), &o, &d);
        QVERIFY((d - cam.forward()).length() < 1e-5f);
        // A ray through a projected point passes through it (perspective and orthographic).
        for (bool ortho : {false, true}) {
            cam.orthographic = ortho;
            const QVector3D p(30, -20, 5);
            const QPointF px = project(cam, p, QSizeF(800, 600));
            cam.ray(px, QSizeF(800, 600), &o, &d);
            const QVector3D v = p - o;
            QVERIFY2((v - d * QVector3D::dotProduct(v, d)).length() < 0.05f, ortho ? "ortho" : "perspective");
        }
        cam.orthographic = false;

        // Top: looking straight down with north up on screen.
        cam.setPreset(Camera::Top, b);
        QVERIFY(cam.forward().z() < -0.999f);
        QVERIFY(project(cam, QVector3D(0, 40, 10), QSizeF(800, 600)).y() < 300);
        QVERIFY(project(cam, QVector3D(40, 0, 10), QSizeF(800, 600)).x() > 400);
        // Front looks north: east is to the right.
        cam.setPreset(Camera::Front, b);
        QVERIFY(cam.forward().y() > 0.999f);
        QVERIFY(project(cam, QVector3D(40, 0, 10), QSizeF(800, 600)).x() > 400);

        // Orbit keeps the pitch within limits, zoom changes the distance, pan moves the target.
        cam.orbit(0, 10000);
        QVERIFY(cam.pitch <= 89.9f);
        const float dist = cam.distance;
        cam.zoom(2);
        QVERIFY(cam.distance < dist);
        const QVector3D target = cam.target;
        cam.pan(100, 0, 600);
        QVERIFY((cam.target - target).length() > 0);
    }

    void picking()
    {
        PointCloud cloud;
        cloud.positions = {{0, 0, 0}, {0, 0, 10}, {5, 5, 0}};
        // Straight down onto (0, 0): the upper point wins.
        Pick p = pickPoint(cloud, QVector3D(0.05f, 0, 100), QVector3D(0, 0, -1), 0.5f);
        QVERIFY(p.hit);
        QCOMPARE(p.position, QVector3D(0, 0, 10));
        QVERIFY(!pickPoint(cloud, QVector3D(2, 2, 100), QVector3D(0, 0, -1), 0.5f).hit);

        Mesh mesh;
        mesh.positions = {{0, 0, 2}, {10, 0, 2}, {10, 10, 2}, {0, 10, 2}};
        mesh.parts.append(Mesh::Part{QString(), {}, {0, 1, 2, 0, 2, 3}});
        p = pickMesh(mesh, QVector3D(3, 7, 50), QVector3D(0, 0, -1));
        QVERIFY(p.hit);
        QVERIFY((p.position - QVector3D(3, 7, 2)).length() < 1e-4f);
        QVERIFY(!pickMesh(mesh, QVector3D(30, 7, 50), QVector3D(0, 0, -1)).hit);
    }

    void sceneService()
    {
        Scene3DService scene;
        QCOMPARE(scene.status(), QStringLiteral("Сцена пуста"));
        // Errors before loading.
        QVERIFY(scene.addFile(m_dir.filePath(QStringLiteral("missing.ply"))).contains(QStringLiteral("не найден")));
        QVERIFY(writeFile(m_dir.filePath(QStringLiteral("x.txt")), "x"));
        QVERIFY(scene.addFile(m_dir.filePath(QStringLiteral("x.txt"))).contains(QStringLiteral("не поддерживается")));

        // A terrain and a point cloud 1 m above it, in the same CRS.
        const double gt[6] = {576000, 1, 0, 5188100, 0, -1};
        const QString dem = m_dir.filePath(QStringLiteral("flat.tif"));
        QVERIFY(makeRaster(dem, 100, 100, 1, GDT_Float32, gt, 32615, [](int, int, int) { return 150.0; }));
        QVector<QVector3D> pts;
        QVector<QColor> colors;
        for (int i = 0; i < 50; ++i) { pts << QVector3D(576020 + i, 5188050, 151); colors << Qt::white; }
        const QString cloudPath = m_dir.filePath(QStringLiteral("line.ply"));
        // Absolute UTM in float would lose precision: write doubles.
        QByteArray ply = "ply\nformat ascii 1.0\nelement vertex 50\nproperty double x\nproperty double y\nproperty double z\nend_header\n";
        for (const QVector3D &p : pts) ply += QByteArray::number(double(p.x()), 'f', 2) + ' ' + QByteArray::number(double(p.y()), 'f', 2) + " 151\n";
        QVERIFY(writeFile(cloudPath, ply));

        QSignalSpy errors(&scene, &Scene3DService::errorOccurred);
        QVERIFY(scene.addTerrain(dem).isEmpty());
        QVERIFY(waitLoaded(scene));
        QVERIFY(scene.addFile(cloudPath).isEmpty());
        QVERIFY(scene.addFile(cloudPath).contains(QStringLiteral("уже")));   // no duplicates
        QVERIFY(waitLoaded(scene));
        QCOMPARE(errors.count(), 0);
        QCOMPARE(scene.layerCount(), 2);
        const QVariantList layers = scene.layersForQml();
        QCOMPARE(layers[0].toMap()[QStringLiteral("kind")].toString(), QStringLiteral("terrain"));
        QVERIFY(layers[0].toMap()[QStringLiteral("info")].toString().contains(QStringLiteral("150")));
        QCOMPARE(layers[1].toMap()[QStringLiteral("info")].toString(), QStringLiteral("50 точек, без цвета. Без геопривязки: координаты файла"));
        QVERIFY(scene.status().contains(QStringLiteral("50 точек")));
        // Common origin: the first loaded layer's; the cloud is placed 1 m above the terrain.
        QCOMPARE(scene.origin().x, 576000.0);
        const Bounds b = scene.bounds();
        QVERIFY(std::abs(b.max.z() - 151.0f) < 1e-3f && std::abs(b.min.z() - 150.0f) < 1e-3f);

        // Pick from the top: the cloud point over the terrain, in absolute coordinates.
        scene.setPreset(QStringLiteral("top"));
        const QSizeF vp(800, 600);
        const QVector3D target(float(576030.0 - scene.origin().x), float(5188050.0 - scene.origin().y), 151.0f);
        QPointF px = project(scene.camera(), target, vp);
        QVERIFY(scene.pickAt(float(px.x()), float(px.y()), float(vp.width()), float(vp.height())));
        QVERIFY2(std::abs(scene.lastPickAbsolute(0) - 576030.0) < 0.6, qPrintable(scene.pickText()));
        QVERIFY(std::abs(scene.lastPickAbsolute(2) - 151.0) < 0.01);
        QVERIFY(scene.pickText().contains(QStringLiteral("576")));
        // Next to the line: the terrain.
        px = project(scene.camera(), QVector3D(target.x(), target.y() - 20, 150), vp);
        QVERIFY(scene.pickAt(float(px.x()), float(px.y()), float(vp.width()), float(vp.height())));
        QVERIFY(std::abs(scene.lastPickAbsolute(2) - 150.0) < 0.01);

        // Vertical exaggeration is visual: picks still return true heights.
        scene.setVerticalScale(5);
        scene.setPreset(QStringLiteral("iso"));
        const QVector3D onTerrain(70, 30, 150);
        px = project(scene.camera(), QVector3D(onTerrain.x(), onTerrain.y(), onTerrain.z() * 5), vp);
        QVERIFY(scene.pickAt(float(px.x()), float(px.y()), float(vp.width()), float(vp.height())));
        QVERIFY2(std::abs(scene.lastPickAbsolute(2) - 150.0) < 0.05, qPrintable(scene.pickText()));
        QVERIFY2((scene.lastPick() - onTerrain).toVector2D().length() < 0.5f, qPrintable(scene.pickText()));
        // Back to 1×: the camera still looks at the data (its target follows the stretch).
        const float targetZ = scene.camera().target.z();
        scene.setVerticalScale(1);
        QVERIFY(std::abs(scene.camera().target.z() - targetZ / 5) < 0.01f);

        // Measure: two terrain points 30 m apart horizontally.
        scene.setTool(QStringLiteral("measure"));
        scene.setPreset(QStringLiteral("top"));
        for (const QVector3D &p : {QVector3D(10, 10, 150), QVector3D(40, 10, 150)}) {
            px = project(scene.camera(), p, vp);
            QVERIFY(scene.pickAt(float(px.x()), float(px.y()), float(vp.width()), float(vp.height())));
        }
        QVERIFY2(scene.measureText().startsWith(QStringLiteral("Расстояние 30,0")), qPrintable(scene.measureText()));
        QVERIFY(scene.measureText().contains(QStringLiteral("уклон 0,0°")));
        // The plain PLY has no CRS: values in file units, no metres.
        QVERIFY(!scene.georeferenced());
        QVERIFY(scene.measureText().contains(QStringLiteral("единицах файла")));
        QVERIFY(!scene.measureText().contains(QStringLiteral(" м ")));
        // Only the terrain (GeoTIFF with a CRS): metres.
        scene.setLayerVisible(scene.layerIdForSource(cloudPath), false);
        QVERIFY(scene.georeferenced());
        for (const QVector3D &p : {QVector3D(10, 10, 150), QVector3D(40, 10, 150)}) {
            px = project(scene.camera(), p, vp);
            QVERIFY(scene.pickAt(float(px.x()), float(px.y()), float(vp.width()), float(vp.height())));
        }
        QVERIFY2(scene.measureText().startsWith(QStringLiteral("Расстояние 30,00 м")), qPrintable(scene.measureText()));
        scene.setLayerVisible(scene.layerIdForSource(cloudPath), true);
        scene.setTool(QStringLiteral("orbit"));
        QVERIFY(scene.measureText().isEmpty());

        // Hidden layers are not picked and not counted; removal and clearing.
        const QString cloudId = scene.layerIdForSource(cloudPath);
        QVERIFY(!cloudId.isEmpty());
        scene.setLayerVisible(cloudId, false);
        QVERIFY(!scene.status().contains(QStringLiteral("точек")));
        const Scene3DService::Snapshot snap = scene.snapshot();
        QCOMPARE(snap.layers.size(), 2);
        QVERIFY(snap.bounds.valid);
        scene.removeLayer(cloudId);
        QCOMPARE(scene.layerCount(), 1);
        scene.clear();
        QCOMPARE(scene.layerCount(), 0);
        QVERIFY(!scene.origin().valid);

        // A broken file is reported, not shown.
        QVERIFY(writeFile(m_dir.filePath(QStringLiteral("broken.ply")), "ply\nformat ascii 1.0\nelement vertex 3\nproperty float q\nend_header\n1\n"));
        QVERIFY(scene.addFile(m_dir.filePath(QStringLiteral("broken.ply"))).isEmpty());
        QVERIFY(waitLoaded(scene));
        QCOMPARE(errors.count(), 1);
        QVERIFY(!scene.layersForQml()[0].toMap()[QStringLiteral("error")].toString().isEmpty());
    }

    // GEOMAP_3D_JOB=<ODM job folder>: the results of a real reconstruction in the 3D scene.
    // The map's basemap under the scene and the photos of the job at their poses. A fake ODM job
    // (coords.txt, reconstruction.json with a nadir and a tilted shot, photos, point cloud) and a
    // tile server of local files whose pixels say where they are: red = tile x, green = tile y,
    // blue = the column inside the tile. Checked pixels of the basemap in the scene must come
    // from the tile and the column of their own geographic position.
    void basemapAndCamerasInTheScene()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const double pi = 3.14159265358979323846;
        const double E = 500000, N = 5600000;   // UTM 32N
        const QString job = tmp.filePath(QStringLiteral("Проект/jobs/j1"));
        QVERIFY(writeFile(job + QStringLiteral("/odm_georeferencing/coords.txt"), "WGS84 UTM 32N\n500000 5600000\n"));
        QVector<QVector3D> points;
        QVector<QColor> colours;
        for (int i = 0; i < 2000; ++i) {   // a field of 100 × 80 m at 200–203 m, local to coords.txt
            points << QVector3D(-50 + (i % 50) * 2.0f, -40 + (i / 50) * 2.0f, 200 + (i % 7) * 0.5f);
            colours << QColor(60, 160, 60);
        }
        QVERIFY(writeFile(job + QStringLiteral("/odm_filterpoints/point_cloud.ply"), binaryPly(points, colours)));
        // Photos 400 × 300: the top-left quarter yellow, the rest red (IMG_1) or blue (IMG_2).
        QVERIFY(QDir().mkpath(job + QStringLiteral("/images")));
        for (int k = 1; k <= 2; ++k) {
            QImage photo(400, 300, QImage::Format_RGB32);
            photo.fill(k == 1 ? QColor(220, 30, 30) : QColor(30, 30, 220));
            QPainter(&photo).fillRect(0, 0, 200, 150, QColor(240, 220, 20));
            QVERIFY(photo.save(job + QStringLiteral("/images/IMG_%1.JPG").arg(k), "JPG", 95));
        }
        // IMG_1 looks straight down from (-20, 0, 300) with the top of the image to the north,
        // IMG_2 from (20, 0, 300) tilted 30° to the north. OpenSfM: X_camera = R·X + t, R as an
        // axis-angle vector (here a rotation about x).
        auto shot = [](double angle, double cx, double cy, double cz) {
            const double ca = std::cos(angle), sa = std::sin(angle);
            const double rc[3] = {cx, ca * cy - sa * cz, sa * cy + ca * cz};
            return QJsonObject{{"camera", "cam"}, {"rotation", QJsonArray{angle, 0.0, 0.0}},
                               {"translation", QJsonArray{-rc[0], -rc[1], -rc[2]}}};
        };
        const QJsonObject shots{{"IMG_1.JPG", shot(pi, -20, 0, 300)}, {"IMG_2.JPG", shot(pi - pi / 6, 20, 0, 300)}};
        const QJsonObject cameras{{"cam", QJsonObject{{"projection_type", "perspective"}, {"width", 400}, {"height", 300},
                                                      {"focal", 0.9}, {"k1", 0.0}, {"k2", 0.0}}}};
        QVERIFY(writeFile(job + QStringLiteral("/opensfm/reconstruction.json"),
                          QJsonDocument(QJsonArray{QJsonObject{{"cameras", cameras}, {"shots", shots}}}).toJson()));

        Scene3DService scene;
        scene.setViewAspect(800.0f / 600.0f);
        QVERIFY(scene.addPointCloud(job + QStringLiteral("/odm_filterpoints/point_cloud.ply")).isEmpty());
        QVERIFY(waitLoaded(scene));
        QTRY_COMPARE_WITH_TIMEOUT(scene.cameras().size(), 2, 30000);
        QCOMPARE(scene.origin().x, E);
        QCOMPARE(scene.layerCount(), 2);   // the cloud and the photos
        const Scene3DService::CameraMark *nadir = nullptr, *tilted = nullptr;
        for (const Scene3DService::CameraMark &c : scene.cameras()) (c.name == QLatin1String("IMG_1.JPG") ? nadir : tilted) = &c;
        QVERIFY(nadir && tilted);
        QVERIFY((nadir->centre - QVector3D(-20, 0, 300)).length() < 0.01f);
        QVERIFY((tilted->centre - QVector3D(20, 0, 300)).length() < 0.01f);
        // The nadir photo lies flat under its camera, its top-left corner (pixel 0, 0) to the north-west.
        const QVector3D middle = (nadir->corners[0] + nadir->corners[1] + nadir->corners[2] + nadir->corners[3]) / 4;
        QVERIFY(std::abs(middle.x() - nadir->centre.x()) < 0.01f && std::abs(middle.y() - nadir->centre.y()) < 0.01f);
        QVERIFY(middle.z() < nadir->centre.z() - 1);
        for (int k = 0; k < 4; ++k) QVERIFY(std::abs(nadir->corners[k].z() - middle.z()) < 0.01f);
        QVERIFY(nadir->corners[0].x() < middle.x() && nadir->corners[0].y() > middle.y());
        QVERIFY(nadir->corners[2].x() > middle.x() && nadir->corners[2].y() < middle.y());
        // Sized by the spacing of the cameras (40 m): neighbours do not cover each other.
        const float width = (nadir->corners[1] - nadir->corners[0]).length();
        QVERIFY2(width > 20 && width < 40, qPrintable(QString::number(width)));
        QVERIFY(std::abs(width / (nadir->corners[3] - nadir->corners[0]).length() - 4.0f / 3.0f) < 0.01f);   // the photo's shape
        // The tilted photo is ahead of its camera, to the north and down.
        const QVector3D ahead = (tilted->corners[0] + tilted->corners[1] + tilted->corners[2] + tilted->corners[3]) / 4 - tilted->centre;
        QVERIFY(ahead.y() > 0.4f * ahead.length() && ahead.z() < -0.7f * ahead.length());
        // The thumbnail on its rectangle as the camera saw it: yellow towards the top-left corner.
        const Scene3DService::Layer *photos = nullptr;
        for (const Scene3DService::Layer &l : scene.layers()) if (l.kind == QLatin1String("cameras")) photos = &l;
        QVERIFY(photos && photos->overlay && photos->mesh);
        const Mesh &cm = *photos->mesh;
        QVERIFY(!cm.parts.isEmpty() && !cm.parts[0].texture.isNull());
        int tl = -1, br = -1;
        for (int i = 0; i < cm.positions.size(); ++i) {
            if (tl < 0 && (cm.positions[i] - nadir->corners[0]).length() < 1e-3f) tl = i;
            if (br < 0 && (cm.positions[i] - nadir->corners[2]).length() < 1e-3f) br = i;
        }
        QVERIFY(tl >= 0 && br >= 0);
        const QImage &atlas = cm.parts[0].texture;
        auto texel = [&](float t) {
            const QVector2D uv = cm.uvs[tl] * (1 - t) + cm.uvs[br] * t;
            return atlas.pixelColor(qBound(0, int(uv.x() * atlas.width()), atlas.width() - 1), qBound(0, int(uv.y() * atlas.height()), atlas.height() - 1));
        };
        const QColor yellow = texel(0.25f), red = texel(0.75f);
        QVERIFY2(yellow.red() > 180 && yellow.green() > 160 && yellow.blue() < 100, qPrintable(yellow.name()));
        QVERIFY2(red.red() > 170 && red.green() < 100 && red.blue() < 100, qPrintable(red.name()));
        // A click on the photo names it; with the photos hidden the click reaches the ground.
        scene.setPreset(QStringLiteral("top"));
        const QPointF at = project(scene.camera(), middle, QSizeF(800, 600));
        QVERIFY(scene.pickAt(float(at.x()), float(at.y()), 800, 600));
        QVERIFY2(scene.pickText().startsWith(QStringLiteral("Снимок IMG_1.JPG")), qPrintable(scene.pickText()));
        // The ground right under the photo: covered by it, reached with the photos hidden.
        const QPointF below = project(scene.camera(), QVector3D(-20, 0, 200), QSizeF(800, 600));
        QVERIFY(scene.pickAt(float(below.x()), float(below.y()), 800, 600));
        QVERIFY2(scene.pickText().startsWith(QStringLiteral("Снимок IMG_1.JPG")), qPrintable(scene.pickText()));
        scene.setShowCameras(false);
        QVERIFY(!scene.showCameras());
        QVERIFY(scene.pickAt(float(below.x()), float(below.y()), 800, 600));
        QVERIFY2(scene.pickText().startsWith(QStringLiteral("Точка")), qPrintable(scene.pickText()));
        QVERIFY(std::abs(scene.lastPick().x() - (-20)) < 1.5f && std::abs(scene.lastPick().y()) < 1.5f);
        scene.setShowCameras(true);

        // Tiles around the place: zoom 15 (the context) and 20 (the finest of a custom server: the detail).
        const QString tiles = tmp.filePath(QStringLiteral("tiles"));
        CoordinateTransform toWgs(Crs::fromEpsg(32632), Crs::wgs84());
        QVERIFY(toWgs.isValid());
        auto worldPixel = [&](double e, double n, int z, double *gx, double *gy) {
            double x = e, y = n;
            if (!toWgs.transform(x, y)) return false;   // longitude, latitude
            const double lat = y * pi / 180, size = 256.0 * double(1 << z);
            *gx = (x + 180.0) / 360.0 * size;
            *gy = (1.0 - std::log(std::tan(lat) + 1.0 / std::cos(lat)) / pi) / 2.0 * size;
            return true;
        };
        int written = 0;
        for (const auto &[z, half] : {std::pair<int, double>{15, 1800.0}, std::pair<int, double>{20, 110.0}}) {
            double x0, y0, x1, y1;
            QVERIFY(worldPixel(E - half, N + half, z, &x0, &y0));
            QVERIFY(worldPixel(E + half, N - half, z, &x1, &y1));
            for (int ty = int(y0 / 256); ty <= int(y1 / 256); ++ty)
                for (int tx = int(x0 / 256); tx <= int(x1 / 256); ++tx) {
                    QImage t(256, 256, QImage::Format_RGB32);
                    for (int py = 0; py < 256; ++py)
                        for (int px = 0; px < 256; ++px) t.setPixel(px, py, qRgb(tx & 255, ty & 255, px));
                    const QString path = QStringLiteral("%1/%2/%3/%4.png").arg(tiles).arg(z).arg(tx).arg(ty);
                    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
                    QVERIFY(t.save(path));
                    ++written;
                }
        }
        qInfo("Tiles written: %d", written);
        const QString id = QStringLiteral("custom:") + QUrl::fromLocalFile(tiles).toString() + QStringLiteral("/{z}/{x}/{y}.png");
        QSignalSpy basemapChanged(&scene, &Scene3DService::basemapChanged);
        scene.setBasemap(id);
        QCOMPARE(scene.basemap(), id);
        // Drawn again as the tiles arrive; finally every pixel of the detail is there.
        const Scene3DService::Layer *ground = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT([&] {
            ground = nullptr;
            for (const Scene3DService::Layer &l : scene.layers()) if (l.kind == QLatin1String("basemap") && l.mesh) ground = &l;
            if (!ground || ground->mesh->parts.size() != 2 || ground->mesh->parts[0].texture.isNull()) return false;
            const QImage &d = ground->mesh->parts[0].texture;
            for (int y = 0; y < d.height(); y += 37)
                for (int x = 0; x < d.width(); x += 37) if (d.pixelColor(x, y).alpha() < 255) return false;
            return webtiles::TileStore::instance()->pending() == 0;
        }(), 60000);
        QVERIFY(ground->overlay && ground->error.isEmpty());
        QVERIFY(!scene.basemapAttribution().isEmpty());
        QVERIFY(basemapChanged.count() > 0);
        // On the ground: just under the data, around the data and the cameras.
        const Mesh &bm = *ground->mesh;
        const Bounds data = scene.bounds();
        QVERIFY(bm.bounds.max.z() < data.min.z() && bm.bounds.max.z() > data.min.z() - 2);
        QVERIFY(bm.bounds.min.x() < data.min.x() - 1000 && bm.bounds.max.y() > data.max.y() + 1000);   // the wider context
        QVERIFY(scene.bounds().max.z() < 210);   // the basemap and the cameras are not data: the view fits the data
        // The detail texture: vertices 0 (north-west) and 2 (south-east) span it.
        const QImage detail = bm.parts[0].texture;
        const QVector3D nw = bm.positions[0], se = bm.positions[2];
        QVERIFY(nw.x() < data.min.x() && nw.y() > data.max.y() && se.x() > data.max.x() && se.y() < data.min.y());
        int checked = 0, right = 0;
        for (int j = 1; j < 8; ++j)
            for (int i = 1; i < 8; ++i) {
                const double u = i / 8.0, v = j / 8.0;
                const double e = E + nw.x() + (se.x() - nw.x()) * u, n = N + nw.y() + (se.y() - nw.y()) * v;
                double gx, gy;
                QVERIFY(worldPixel(e, n, 20, &gx, &gy));
                const double column = gx - 256.0 * std::floor(gx / 256);
                if (column < 3 || column > 252) continue;   // the edges of tiles blend
                const QColor c = detail.pixelColor(qMin(detail.width() - 1, int(u * detail.width())), qMin(detail.height() - 1, int(v * detail.height())));
                ++checked;
                const bool ok = c.red() == (int(gx / 256) & 255) && c.green() == (int(gy / 256) & 255) && std::abs(c.blue() - column) <= 3;
                if (ok) ++right;
                else qInfo("u %.3f v %.3f: rgb %d %d %d, expected %d %d %.1f", u, v, c.red(), c.green(), c.blue(), int(gx / 256) & 255,
                           int(gy / 256) & 255, column);
            }
        QVERIFY(checked > 30);
        QVERIFY2(right == checked, qPrintable(QStringLiteral("%1 of %2").arg(right).arg(checked)));
        // The context ring has its own texture of the wider area.
        QVERIFY(!bm.parts[1].texture.isNull() && bm.parts[1].indices.size() == 24);
        // Off: the basemap leaves the scene.
        scene.setBasemap(QString());
        for (const Scene3DService::Layer &l : scene.layers()) QVERIFY(l.kind != QLatin1String("basemap"));
        QVERIFY(scene.basemapAttribution().isEmpty());
        // A built-in server is used only after its terms were accepted.
        QSettings().remove(QStringLiteral("basemap/accepted/osm"));
        scene.setBasemap(QStringLiteral("osm"));
        QVERIFY(scene.basemap().isEmpty());
        // Removing the data removes the overlays.
        const QString cloudId = scene.layers().first().id;
        scene.removeLayer(cloudId);
        QCOMPARE(scene.layerCount(), 0);
        QVERIFY(scene.cameras().isEmpty());
    }

    void odmJobResults()
    {
        const QString job = qEnvironmentVariable("GEOMAP_3D_JOB");
        if (job.isEmpty()) QSKIP("GEOMAP_3D_JOB is not set");
        const QString cloudPath = job + QStringLiteral("/odm_filterpoints/point_cloud.ply");
        const QString objPath = job + QStringLiteral("/odm_texturing/odm_textured_model_geo.obj");
        const QString dsmPath = job + QStringLiteral("/odm_dem/dsm.tif");
        const QString orthoPath = job + QStringLiteral("/odm_orthophoto/odm_orthophoto.tif");
        QElapsedTimer t;
        t.start();
        const PointCloud cloud = readPointCloud(cloudPath, 5000000);
        QVERIFY2(cloud.isValid(), qPrintable(cloud.error));
        qInfo("Point cloud: %llu points, %d shown, %s colours, %lld ms", cloud.totalPoints, int(cloud.positions.size()),
              cloud.colors.isEmpty() ? "no" : "with", t.restart());
        QVERIFY(cloud.origin.valid);
        const Mesh model = readObj(objPath);
        QVERIFY2(model.isValid(), qPrintable(model.error));
        int textures = 0;
        for (const Mesh::Part &p : model.parts) textures += p.texture.isNull() ? 0 : 1;
        qInfo("Model: %d vertices, %llu triangles, %d parts, %d textures, %lld ms %s", int(model.positions.size()), model.triangles,
              int(model.parts.size()), textures, t.restart(), qPrintable(model.note));
        QVERIFY(textures > 0);
        QCOMPARE(model.origin.x, cloud.origin.x);
        QString note;
        const QImage drape = readDrape(orthoPath, dsmPath, 4096, &note);
        QVERIFY2(!drape.isNull(), qPrintable(note));
        const Mesh terrain = demMesh(dsmPath, 1024, cloud.origin, drape);
        QVERIFY2(terrain.isValid(), qPrintable(terrain.error));
        qInfo("DSM terrain: %d vertices, %llu triangles, drape %dx%d, %lld ms", int(terrain.positions.size()), terrain.triangles,
              drape.width(), drape.height(), t.restart());
        // The three describe the same place: overlapping extents and close heights.
        const auto overlap = [](const Bounds &a, const Bounds &b) {
            const float x = qMin(a.max.x(), b.max.x()) - qMax(a.min.x(), b.min.x());
            const float y = qMin(a.max.y(), b.max.y()) - qMax(a.min.y(), b.min.y());
            return qMax(0.0f, x) * qMax(0.0f, y) / qMax(1e-6f, (a.size().x() * a.size().y()));
        };
        qInfo("Cloud  X %.1f..%.1f Y %.1f..%.1f Z %.1f..%.1f", cloud.bounds.min.x(), cloud.bounds.max.x(), cloud.bounds.min.y(),
              cloud.bounds.max.y(), cloud.bounds.min.z(), cloud.bounds.max.z());
        qInfo("Model  X %.1f..%.1f Y %.1f..%.1f Z %.1f..%.1f", model.bounds.min.x(), model.bounds.max.x(), model.bounds.min.y(),
              model.bounds.max.y(), model.bounds.min.z(), model.bounds.max.z());
        qInfo("DSM    X %.1f..%.1f Y %.1f..%.1f Z %.1f..%.1f", terrain.bounds.min.x(), terrain.bounds.max.x(), terrain.bounds.min.y(),
              terrain.bounds.max.y(), terrain.bounds.min.z(), terrain.bounds.max.z());
        QVERIFY(overlap(model.bounds, cloud.bounds) > 0.8f);
        QVERIFY(overlap(terrain.bounds, cloud.bounds) > 0.8f);
        // Model vertex heights near the DSM: pick the DSM below model vertices.
        int checked = 0, close = 0;
        for (int i = 0; i < model.positions.size(); i += qMax(1, int(model.positions.size() / 500))) {
            const QVector3D v = model.positions[i];
            const Pick p = pickMesh(terrain, QVector3D(v.x(), v.y(), 10000), QVector3D(0, 0, -1));
            if (!p.hit) continue;
            ++checked;
            if (std::abs(p.position.z() - v.z()) < 1.5f) ++close;
        }
        qInfo("Model vertices within 1.5 m of the DSM: %d of %d", close, checked);
        QVERIFY(checked > 100);
        QVERIFY(close > checked * 0.8);

        // The service: all three together.
        Scene3DService scene;
        QVERIFY(scene.addPointCloud(cloudPath).isEmpty());
        QVERIFY(waitLoaded(scene));
        QVERIFY(scene.addModel(objPath).isEmpty());
        QVERIFY(scene.addTerrain(dsmPath, orthoPath).isEmpty());
        QVERIFY(waitLoaded(scene));
        int data = 0;
        for (const QVariant &l : scene.layersForQml()) {
            const QVariantMap m = l.toMap();
            QVERIFY2(m[QStringLiteral("error")].toString().isEmpty(), qPrintable(m[QStringLiteral("error")].toString()));
            qInfo("%s: %s", qPrintable(m[QStringLiteral("kindTitle")].toString()), qPrintable(m[QStringLiteral("info")].toString()));
            data += m[QStringLiteral("overlay")].toBool() ? 0 : 1;
        }
        QCOMPARE(data, 3);
        // The photos of the job hang above the data at their reconstructed positions.
        QTRY_VERIFY_WITH_TIMEOUT(!scene.cameras().isEmpty(), 60000);
        qInfo("Cameras: %d", int(scene.cameras().size()));
        const Bounds dataBounds = scene.bounds();
        for (const Scene3DService::CameraMark &c : scene.cameras()) {
            QVERIFY(c.centre.x() > dataBounds.min.x() - 200 && c.centre.x() < dataBounds.max.x() + 200);
            QVERIFY(c.centre.z() > dataBounds.center().z());   // above the ground
        }
        scene.setShowCameras(false);   // the point under the cursor, not a photo
        scene.setPreset(QStringLiteral("top"));
        QVERIFY(scene.pickAt(400, 300, 800, 600));
        qInfo("%s", qPrintable(scene.pickText()));
        QVERIFY(scene.georeferenced());
        QVERIFY(scene.pickText().endsWith(QStringLiteral(" м")));
        QVERIFY(std::abs(scene.lastPickAbsolute(0) - (cloud.origin.x + cloud.bounds.center().x())) < 60);
    }

    // LiDAR through PDAL of the bundled ODM (GEOMAP_ODM_HOME, else skipped): what a LAS holds, a
    // DSM with the roof of a building and a DTM with the ground under it (SMRF), the LAZ of the
    // same cloud unpacked for the 3D view — in folders with Cyrillic names.
    void lidarCloudToSurfaces()
    {
        QString error;
        if (odmtools::root(&error).isEmpty() || odmtools::program(QStringLiteral("pdal")).isEmpty())
            QSKIP(qPrintable(QStringLiteral("PDAL из OpenDroneMap не найден: %1").arg(error)));
        // 40 × 40 m of ground rising 5 cm per metre eastwards, a 10 × 10 m building 8 m high in
        // the middle; a point every 0.5 m.
        QVector<QVector3D> pts;
        QVector<QColor> colors;
        for (int j = 0; j <= 80; ++j)
            for (int i = 0; i <= 80; ++i) {
                const float x = i * 0.5f, y = j * 0.5f;
                const bool roof = x >= 15 && x <= 25 && y >= 15 && y <= 25;
                pts << QVector3D(x, y, 100 + 0.05f * x + (roof ? 8 : 0));
                colors << (roof ? QColor(200, 60, 50) : QColor(90, 140, 70));
            }
        const QString folder = m_dir.filePath(QStringLiteral("Облака съёмки"));
        const QString las = QDir(folder).filePath(QStringLiteral("участок.las"));
        QByteArray bytes = las12(pts, colors, 576000.0, 5188000.0, 0.01, Crs::fromEpsg(32615).wkt().toUtf8());
        // No WKT bit in the global encoding: PDAL ignores the record, GeoMap AI reads it itself.
        QVERIFY(writeFile(las, bytes));

        const lidar::Info info = lidar::inspect(las);
        QVERIFY2(info.ok(), qPrintable(info.error));
        QCOMPARE(info.points, quint64(pts.size()));
        QVERIFY(std::abs(info.extent.left() - 576000) < 0.01 && std::abs(info.extent.right() - 576040) < 0.01);
        QVERIFY2(std::abs(info.maxZ - 109.25) < 0.01, qPrintable(QStringLiteral("z %1 – %2").arg(info.minZ).arg(info.maxZ)));   // roof at the east edge: 100 + 0.05 · 25 + 8
        QVERIFY2(Crs::fromWkt(info.crsWkt).isValid(), qPrintable(info.crsWkt));
        QVERIFY(info.hasColor);
        QVERIFY(!info.compressed);
        QCOMPARE(info.classes, QVector<int>{0});
        QCOMPARE(lidar::suggestedResolution(info), 1.0);   // √(2 · 1600 m² / 6561) ≈ 0,7 m

        auto valueAt = [](const QString &tif, double x, double y) {
            GDALDatasetH ds = GDALOpen(QDir::toNativeSeparators(tif).toUtf8().constData(), GA_ReadOnly);
            if (!ds) return -1.0;
            double gt[6];
            GDALGetGeoTransform(ds, gt);
            float v = -1;
            GDALRasterIO(GDALGetRasterBand(ds, 1), GF_Read, int((x - gt[0]) / gt[1]), int((y - gt[3]) / gt[5]), 1, 1, &v, 1, 1, GDT_Float32, 0, 0);
            GDALClose(ds);
            return double(v);
        };
        const QString out = m_dir.filePath(QStringLiteral("Проект/lidar"));
        const lidar::Surface dsm = lidar::buildSurface(info, QStringLiteral("dsm"), 1.0, QDir(out).filePath(QStringLiteral("участок_dsm.tif")));
        QVERIFY2(dsm.ok(), qPrintable(dsm.error));
        QCOMPARE(dsm.width, 41);
        const double roof = valueAt(dsm.path, 576020.2, 5188020.2), field = valueAt(dsm.path, 576005.2, 5188005.2);
        QVERIFY2(std::abs(roof - 109) < 0.25 && std::abs(field - 100.3) < 0.2, qPrintable(QStringLiteral("%1 %2").arg(roof).arg(field)));

        const lidar::Surface dtm = lidar::buildSurface(info, QStringLiteral("dtm"), 1.0, QDir(out).filePath(QStringLiteral("участок_dtm.tif")));
        QVERIFY2(dtm.ok(), qPrintable(dtm.error));
        QVERIFY(!dtm.groundFromFile);
        const double under = valueAt(dtm.path, 576020.2, 5188020.2);
        QVERIFY2(std::abs(under - 101) < 0.5, qPrintable(QString::number(under)));   // the ground under the roof

        // The same cloud as LAZ: unpacked once into the cache and read like the LAS.
        const QString laz = QDir(folder).filePath(QStringLiteral("участок.laz"));
        QFile::copy(las, m_dir.filePath(QStringLiteral("a.las")));
        const odmtools::Result packed = odmtools::pdal({QStringLiteral("translate"), QDir::toNativeSeparators(m_dir.filePath(QStringLiteral("a.las"))),
                                                        QDir::toNativeSeparators(m_dir.filePath(QStringLiteral("a.laz"))),
                                                        QStringLiteral("--readers.las.default_srs=EPSG:32615")});
        QVERIFY2(packed.ok(), qPrintable(packed.error));
        QVERIFY(QFile::rename(m_dir.filePath(QStringLiteral("a.laz")), laz));
        const lidar::Info zinfo = lidar::inspect(laz);
        QVERIFY2(zinfo.ok() && zinfo.compressed, qPrintable(zinfo.error));
        QString unpackError;
        const QString unpacked = lidar::uncompressed(laz, &unpackError);
        QVERIFY2(!unpacked.isEmpty(), qPrintable(unpackError));
        QCOMPARE(lidar::uncompressed(laz, &unpackError), unpacked);   // cached
        const PointCloud c = readPointCloud(unpacked, 0);
        QVERIFY2(c.isValid(), qPrintable(c.error));
        QCOMPARE(c.totalPoints, quint64(pts.size()));
        QVERIFY(c.georeferenced);
    }

private:
    QTemporaryDir m_dir;
};

QTEST_MAIN(View3DTest)
#include "test_view3d.moc"
