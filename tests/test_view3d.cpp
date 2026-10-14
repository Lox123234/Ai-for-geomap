#include <QtTest>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtEndian>
#include <cmath>
#include <functional>
#include <gdal.h>
#include <ogr_srs_api.h>
#include "Camera3D.h"
#include "GisEnvironment.h"
#include "Scene3D.h"
#include "Scene3DService.h"

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
                 const QByteArray &wkt = {})
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
        a.append(QByteArray(8, '\0'));   // intensity, flags, class, angle, user data, source id
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
        QCOMPARE(scene.layerCount(), 3);
        for (const QVariant &l : scene.layersForQml()) {
            const QVariantMap m = l.toMap();
            QVERIFY2(m[QStringLiteral("error")].toString().isEmpty(), qPrintable(m[QStringLiteral("error")].toString()));
            qInfo("%s: %s", qPrintable(m[QStringLiteral("kindTitle")].toString()), qPrintable(m[QStringLiteral("info")].toString()));
        }
        scene.setPreset(QStringLiteral("top"));
        QVERIFY(scene.pickAt(400, 300, 800, 600));
        qInfo("%s", qPrintable(scene.pickText()));
        QVERIFY(scene.georeferenced());
        QVERIFY(scene.pickText().endsWith(QStringLiteral(" м")));
        QVERIFY(std::abs(scene.lastPickAbsolute(0) - (cloud.origin.x + cloud.bounds.center().x())) < 60);
    }

private:
    QTemporaryDir m_dir;
};

QTEST_MAIN(View3DTest)
#include "test_view3d.moc"
