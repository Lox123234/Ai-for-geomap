#include <QtTest>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QOffscreenSurface>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickGraphicsDevice>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QtEndian>
#include <QUrl>
#include <QtQml/qqmlextensionplugin.h>
#include <rhi/qrhi.h>
#include <memory>
#include "CommandRegistry.h"
#include "GisEnvironment.h"
#include "MapService.h"
#include "ProjectTreeModel.h"
#include "Scene3DService.h"
#include "View3DItem.h"
#include "WebTiles.h"

Q_IMPORT_QML_PLUGIN(GeoMapAIPlugin)

// The 3D view drawn by the GPU without a window on the screen: Qt Quick renders through
// QQuickRenderControl into a texture of our own QRhi (Direct3D 11 on Windows, otherwise
// OpenGL) and the pixels are read back. Checks colours, texture orientation and coverage;
// GEOMAP_TEST_OUTPUT=<folder> keeps the images, GEOMAP_3D_JOB=<ODM job> renders its results.
namespace {

bool writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}

class OffscreenView
{
public:
    ~OffscreenView()
    {
        // The window before its render control, both before the resources and the QRhi.
        m_window.reset();
        m_control.reset();
        m_pass.reset();
        m_target.reset();
        m_depth.reset();
        m_texture.reset();
        m_rhi.reset();
    }

    bool init(const QSize &size)
    {
        m_size = size;
#ifdef Q_OS_WIN
        QRhiD3D11InitParams params;
        m_rhi.reset(QRhi::create(QRhi::D3D11, &params));
#else
        m_surface.reset(QRhiGles2InitParams::newFallbackSurface());
        QRhiGles2InitParams params;
        params.fallbackSurface = m_surface.get();
        m_rhi.reset(QRhi::create(QRhi::OpenGLES2, &params));
#endif
        if (!m_rhi) return false;
        m_control = std::make_unique<QQuickRenderControl>();
        m_window = std::make_unique<QQuickWindow>(m_control.get());
        m_window->setGraphicsDevice(QQuickGraphicsDevice::fromRhi(m_rhi.get()));
        if (!m_control->initialize()) return false;
        m_texture.reset(m_rhi->newTexture(QRhiTexture::RGBA8, size, 1, QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
        if (!m_texture->create()) return false;
        m_depth.reset(m_rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, size, 1));
        if (!m_depth->create()) return false;
        QRhiTextureRenderTargetDescription desc{QRhiColorAttachment(m_texture.get())};
        desc.setDepthStencilBuffer(m_depth.get());
        m_target.reset(m_rhi->newTextureRenderTarget(desc));
        m_pass.reset(m_target->newCompatibleRenderPassDescriptor());
        m_target->setRenderPassDescriptor(m_pass.get());
        if (!m_target->create()) return false;
        m_window->setRenderTarget(QQuickRenderTarget::fromRhiRenderTarget(m_target.get()));
        m_window->setGeometry(0, 0, size.width(), size.height());
        m_window->contentItem()->setSize(size);
        return true;
    }

    View3DItem *load(QQmlEngine &engine)
    {
        return qobject_cast<View3DItem *>(loadQml(engine, "import QtQuick\nimport GeoMapAI.View3D\nView3DItem {}\n"));
    }

    QQuickItem *loadQml(QQmlEngine &engine, const QByteArray &qml)
    {
        QQmlComponent component(&engine);
        component.setData(qml, QUrl());
        auto *item = qobject_cast<QQuickItem *>(component.create());
        if (!item) {
            qWarning() << component.errorString();
            return nullptr;
        }
        item->setParentItem(m_window->contentItem());
        item->setSize(m_size);
        return item;
    }

    QImage render()
    {
        QImage result;
        // Two frames: the first one creates the item's texture and pipelines.
        for (int frame = 0; frame < 2; ++frame) {
            m_control->polishItems();
            m_control->beginFrame();
            m_control->sync();
            m_control->render();
            QRhiReadbackResult readback;
            bool done = false;
            readback.completed = [&done] { done = true; };
            QRhiResourceUpdateBatch *batch = m_rhi->nextResourceUpdateBatch();
            batch->readBackTexture(m_texture.get(), &readback);
            m_control->commandBuffer()->resourceUpdate(batch);
            m_control->endFrame();   // offscreen frames are synchronous
            if (!done) continue;
            const QImage wrapped(reinterpret_cast<const uchar *>(readback.data.constData()), readback.pixelSize.width(),
                                 readback.pixelSize.height(), QImage::Format_RGBA8888_Premultiplied);
            result = m_rhi->isYUpInFramebuffer() ? wrapped.mirrored() : wrapped.copy();
        }
        return result;
    }

    QString backend() const { return m_rhi ? QString::fromLatin1(m_rhi->backendName()) + QStringLiteral(" · ") + m_rhi->driverInfo().deviceName : QString(); }

private:
    QSize m_size;
    std::unique_ptr<QOffscreenSurface> m_surface;
    std::unique_ptr<QRhi> m_rhi;
    std::unique_ptr<QQuickRenderControl> m_control;
    std::unique_ptr<QQuickWindow> m_window;
    std::unique_ptr<QRhiTexture> m_texture;
    std::unique_ptr<QRhiRenderBuffer> m_depth;
    std::unique_ptr<QRhiTextureRenderTarget> m_target;
    std::unique_ptr<QRhiRenderPassDescriptor> m_pass;
};

bool waitLoaded(Scene3DService &scene)
{
    QElapsedTimer t;
    t.start();
    while (scene.loading() && t.elapsed() < 180000) QTest::qWait(20);
    return !scene.loading();
}

// Share of pixels that differ from the background (#171B22).
double coverage(const QImage &image)
{
    qint64 n = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            const QColor c = image.pixelColor(x, y);
            n += (std::abs(c.red() - 23) + std::abs(c.green() - 27) + std::abs(c.blue() - 34)) > 12;
        }
    return double(n) / (qint64(image.width()) * image.height());
}

void keep(const QImage &image, const QString &name)
{
    const QString dir = qEnvironmentVariable("GEOMAP_TEST_OUTPUT");
    if (!dir.isEmpty() && QDir().mkpath(dir)) image.save(QDir(dir).filePath(name));
}

bool near(const QColor &c, const QColor &want, int tolerance = 40)
{
    return std::abs(c.red() - want.red()) <= tolerance && std::abs(c.green() - want.green()) <= tolerance
        && std::abs(c.blue() - want.blue()) <= tolerance;
}

// Whether any pixel of the 9 × 9 block around (x, y) has the colour (points leave gaps).
bool anyNear(const QImage &image, int x, int y, const QColor &want, int tolerance = 30)
{
    for (int dy = -4; dy <= 4; ++dy)
        for (int dx = -4; dx <= 4; ++dx)
            if (near(image.pixelColor(x + dx, y + dy), want, tolerance)) return true;
    return false;
}
int maxLightness(const QImage &image, int x, int y)
{
    int m = 0;
    for (int dy = -4; dy <= 4; ++dy)
        for (int dx = -4; dx <= 4; ++dx) m = qMax(m, image.pixelColor(x + dx, y + dy).lightness());
    return m;
}

} // namespace

class View3DRenderTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        GisEnvironment::initialize(QCoreApplication::applicationDirPath());
        QQuickStyle::setStyle(QStringLiteral("Basic"));   // as the application
        qmlRegisterType<View3DItem>("GeoMapAI.View3D", 1, 0, "View3DItem");
        QVERIFY2(QFile::exists(QStringLiteral(":/qt/qml/GeoMapAI/shaders/points.vert.qsb")), "shaders are not in the resources");
        QVERIFY(QFile::exists(QStringLiteral(":/qt/qml/GeoMapAI/shaders/mesh.frag.qsb")));
        if (!m_view.init(QSize(800, 600))) QSKIP("No GPU rendering here (QRhi could not be created)");
        qInfo("Rendering with %s", qPrintable(m_view.backend()));
        m_item = m_view.load(m_engine);
        QVERIFY(m_item);
        m_item->setScene(&m_scene);
        QVERIFY(m_dir.isValid());
    }

    void emptySceneIsBackground()
    {
        const QImage image = m_view.render();
        QVERIFY(!image.isNull());
        QCOMPARE(image.size(), QSize(800, 600));
        QTRY_VERIFY(m_item->gpuReady());
        QVERIFY2(coverage(image) < 0.001, qPrintable(QString::number(coverage(image))));
    }

    // A red point grid seen from the top fills the middle of the view with red.
    void pointCloudFromTheTop()
    {
        QByteArray ply = "ply\nformat ascii 1.0\nelement vertex 1681\nproperty float x\nproperty float y\nproperty float z\n"
                         "property uchar red\nproperty uchar green\nproperty uchar blue\nend_header\n";
        for (int y = 0; y <= 40; ++y)
            for (int x = 0; x <= 40; ++x)
                ply += QByteArray::number(x * 0.5) + ' ' + QByteArray::number(y * 0.5) + " 10 230 40 40\n";
        const QString path = m_dir.filePath(QStringLiteral("grid.ply"));
        QVERIFY(writeFile(path, ply));
        QVERIFY(m_scene.addPointCloud(path).isEmpty());
        QVERIFY(waitLoaded(m_scene));
        m_scene.setPreset(QStringLiteral("top"));
        m_scene.setPointSize(8);
        QImage image = m_view.render();
        keep(image, QStringLiteral("render-01-points-top.png"));
        QVERIFY2(near(image.pixelColor(400, 300), QColor(230, 40, 40)), qPrintable(image.pixelColor(400, 300).name()));
        const double c = coverage(image);
        qInfo("points: %.1f %% of the view", c * 100);
        QVERIFY(c > 0.1);   // round 8 px points on a 0.5 m grid
        // Colour by height: the uniform height maps to the low end of the ramp (blue).
        m_scene.setColorMode(QStringLiteral("height"));
        image = m_view.render();
        keep(image, QStringLiteral("render-02-points-height.png"));
        QVERIFY2(near(image.pixelColor(400, 300), QColor(44, 107, 191)), qPrintable(image.pixelColor(400, 300).name()));
        m_scene.setColorMode(QStringLiteral("rgb"));
        m_scene.clear();
    }

    // Classes and intensity of a LAS cloud: ground brown on the west half, buildings red on the east.
    void classesAndIntensity()
    {
        QByteArray h(227, '\0');
        memcpy(h.data(), "LASF", 4);
        h[24] = 1; h[25] = 2;
        const int n = 41 * 41;
        const auto set16 = [&h](int o, quint16 v) { qToLittleEndian(v, h.data() + o); };
        const auto set32 = [&h](int o, quint32 v) { qToLittleEndian(v, h.data() + o); };
        const auto setD = [&h](int o, double v) { qToLittleEndian(v, h.data() + o); };
        set16(94, 227); set32(96, 227); h[104] = 0; set16(105, 20); set32(107, n);
        setD(131, 0.01); setD(139, 0.01); setD(147, 0.01); setD(155, 0); setD(163, 0); setD(171, 0);
        setD(179, 20); setD(187, 0); setD(195, 20); setD(203, 0);
        QByteArray a = h;
        for (int y = 0; y <= 40; ++y)
            for (int x = 0; x <= 40; ++x) {
                char rec[20] = {};
                qToLittleEndian(qint32(x * 50), rec);
                qToLittleEndian(qint32(y * 50), rec + 4);
                qToLittleEndian(qint32(1000), rec + 8);
                qToLittleEndian(quint16(x < 20 ? 100 : 5000), rec + 12);   // dark west, bright east
                rec[15] = char(x < 20 ? 2 : 6);
                a.append(rec, 20);
            }
        QVERIFY(writeFile(m_dir.filePath(QStringLiteral("classes.las")), a));
        QVERIFY(m_scene.addPointCloud(m_dir.filePath(QStringLiteral("classes.las"))).isEmpty());
        QVERIFY(waitLoaded(m_scene));
        m_scene.setPreset(QStringLiteral("top"));
        m_scene.setPointSize(10);
        m_scene.setColorMode(QStringLiteral("classes"));
        QImage image = m_view.render();
        keep(image, QStringLiteral("render-04-classes.png"));
        QVERIFY2(anyNear(image, 250, 300, QColor(166, 118, 60)), qPrintable(image.pixelColor(250, 300).name()));
        QVERIFY2(anyNear(image, 550, 300, QColor(225, 90, 70)), qPrintable(image.pixelColor(550, 300).name()));
        QVERIFY(!anyNear(image, 250, 300, QColor(225, 90, 70)));
        m_scene.setColorMode(QStringLiteral("intensity"));
        image = m_view.render();
        keep(image, QStringLiteral("render-05-intensity.png"));
        QVERIFY2(maxLightness(image, 250, 300) < 60, qPrintable(QString::number(maxLightness(image, 250, 300))));
        QVERIFY2(maxLightness(image, 550, 300) > 200, qPrintable(QString::number(maxLightness(image, 550, 300))));
    }

    // A textured quad seen from the top: the image appears with north up and east right.
    void textureOrientation()
    {
        QImage tex(64, 64, QImage::Format_RGB32);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x)
                tex.setPixelColor(x, y, y < 32 ? (x < 32 ? QColor(220, 30, 30) : QColor(30, 200, 30))
                                               : (x < 32 ? QColor(30, 60, 220) : QColor(230, 220, 40)));
        QVERIFY(tex.save(m_dir.filePath(QStringLiteral("quad.png"))));
        QVERIFY(writeFile(m_dir.filePath(QStringLiteral("quad.mtl")), "newmtl t\nmap_Kd quad.png\n"));
        QVERIFY(writeFile(m_dir.filePath(QStringLiteral("quad.obj")),
                          "mtllib quad.mtl\nv 0 0 0\nv 10 0 0\nv 10 10 0\nv 0 10 0\n"
                          "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nusemtl t\nf 1/1 2/2 3/3 4/4\n"));
        QVERIFY(m_scene.addModel(m_dir.filePath(QStringLiteral("quad.obj"))).isEmpty());
        QVERIFY(waitLoaded(m_scene));
        m_scene.setPreset(QStringLiteral("top"));
        const QImage image = m_view.render();
        keep(image, QStringLiteral("render-03-texture-top.png"));
        // Quadrants of the quad around the centre (the quad spans about 300 px here).
        const QColor nw = image.pixelColor(370, 270), ne = image.pixelColor(430, 270);
        const QColor sw = image.pixelColor(370, 330), se = image.pixelColor(430, 330);
        QVERIFY2(near(nw, QColor(220, 30, 30)), qPrintable(nw.name()));
        QVERIFY2(near(ne, QColor(30, 200, 30)), qPrintable(ne.name()));
        QVERIFY2(near(sw, QColor(30, 60, 220)), qPrintable(sw.name()));
        QVERIFY2(near(se, QColor(230, 220, 40)), qPrintable(se.name()));
        // Hidden layers are not drawn.
        m_scene.setLayerVisible(m_scene.layers().first().id, false);
        QVERIFY(coverage(m_view.render()) < 0.001);
        m_scene.clear();
    }

    // GEOMAP_3D_JOB: the point cloud, the textured model and the DSM with the orthophoto.
    void odmJobViews()
    {
        const QString job = qEnvironmentVariable("GEOMAP_3D_JOB");
        if (job.isEmpty()) QSKIP("GEOMAP_3D_JOB is not set");
        struct Case { QString name, kind, path, drape; };
        QList<Case> cases = {
            {QStringLiteral("cloud"), QStringLiteral("pointcloud"), job + QStringLiteral("/odm_georeferencing/odm_georeferenced_model.laz"), {}},
            {QStringLiteral("model"), QStringLiteral("mesh"), job + QStringLiteral("/odm_texturing/odm_textured_model_geo.obj"), {}},
            {QStringLiteral("dsm"), QStringLiteral("terrain"), job + QStringLiteral("/odm_dem/dsm.tif"), job + QStringLiteral("/odm_orthophoto/odm_orthophoto.tif")},
            {QStringLiteral("dtm"), QStringLiteral("terrain"), job + QStringLiteral("/odm_dem/dtm.tif"), {}},
        };
        // The surface and the true orthophoto of GeoMap AI (in the job, or GEOMAP_3D_TRUE=<folder with both>).
        const QString trueDir = qEnvironmentVariable("GEOMAP_3D_TRUE", job + QStringLiteral("/geomap"));
        if (QFileInfo::exists(trueDir + QStringLiteral("/true_surface.tif")))
            cases << Case{QStringLiteral("true"), QStringLiteral("terrain"), trueDir + QStringLiteral("/true_surface.tif"),
                          trueDir + QStringLiteral("/true_orthophoto.tif")};
        for (const Case &c : cases) {
            m_scene.clear();
            QElapsedTimer t;
            t.start();
            const QString error = c.kind == QLatin1String("pointcloud") ? m_scene.addPointCloud(c.path)
                                : c.kind == QLatin1String("mesh") ? m_scene.addModel(c.path) : m_scene.addTerrain(c.path, c.drape);
            QVERIFY2(error.isEmpty(), qPrintable(error));
            QVERIFY(waitLoaded(m_scene));
            const QVariantMap layer = m_scene.layersForQml().first().toMap();
            QVERIFY2(layer.value("error").toString().isEmpty(), qPrintable(layer.value("error").toString()));
            const qint64 loadMs = t.restart();
            for (const QString &preset : {QStringLiteral("iso"), QStringLiteral("top")}) {
                m_scene.setPreset(preset);
                const QImage image = m_view.render();
                const QString name = QStringLiteral("brighton-%1-%2.png").arg(c.name, preset);
                keep(image, name);
                const double cov = coverage(image);
                qInfo("%s: load %lld ms, frame %lld ms, %.0f %% covered; %s", qPrintable(name), loadMs, t.restart(), cov * 100,
                      qPrintable(layer.value("info").toString()));
                // Clouds leave gaps between points and the corners of their extent empty.
                QVERIFY2(cov > (c.kind == QLatin1String("pointcloud") ? 0.06 : 0.15), qPrintable(name));
            }
        }
        // Everything together, colours by height and 3× exaggeration.
        m_scene.clear();
        QVERIFY(m_scene.addPointCloud(cases[0].path).isEmpty());
        QVERIFY(waitLoaded(m_scene));
        QVERIFY(m_scene.addModel(cases[1].path).isEmpty());
        QVERIFY(m_scene.addTerrain(cases[3].path).isEmpty());
        QVERIFY(waitLoaded(m_scene));
        m_scene.setVerticalScale(3);
        m_scene.setPreset(QStringLiteral("iso"));
        keep(m_view.render(), QStringLiteral("brighton-all-exaggerated.png"));
        m_scene.setColorMode(QStringLiteral("height"));
        m_scene.setVerticalScale(1);
        m_scene.orbit(200, -40);
        const QImage image = m_view.render();
        keep(image, QStringLiteral("brighton-all-height.png"));
        QVERIFY(coverage(image) > 0.08);   // a low oblique view of the whole scene
        m_scene.setColorMode(QStringLiteral("rgb"));
    }

    // GEOMAP_3D_JOB: the whole 3D workspace of the window (context bar, overlays, axes, the
    // layer panel) over the GPU view, as the user sees it.
    void workspaceWithOdmResults()
    {
        const QString job = qEnvironmentVariable("GEOMAP_3D_JOB");
        if (job.isEmpty()) QSKIP("GEOMAP_3D_JOB is not set");
        OffscreenView view;
        QVERIFY(view.init(QSize(1360, 760)));
        QQmlEngine engine;
        CommandRegistry commands;
        ProjectTreeModel tree(nullptr, nullptr, nullptr);
        Scene3DService scene;
        MapService gis;   // the basemap choice of the 3D bar is the map's
        engine.rootContext()->setContextProperty("scene3d", &scene);
        engine.rootContext()->setContextProperty("gis", &gis);
        engine.rootContext()->setContextProperty("commands", &commands);
        engine.rootContext()->setContextProperty("projectTree", &tree);
        // The window's command functions, reduced to what the 3D workspace asks.
        QQuickItem *root = view.loadQml(engine, R"(
import QtQuick
import QtQuick.Layouts
import GeoMapAI
Rectangle {
    color: Theme.bg
    QtObject {
        id: host
        function commandChecked(id) { return id === "tool.rotate" || (id === "view.axes" && scene3d.showAxes) }
        function commandEnabled(id) { return true }
        function runCommand(id) { return false }
        function open3D(kind, path) {}
        function openBasemap(id) {}
        function showError(text) {}
    }
    RowLayout {
        anchors.fill: parent
        spacing: 0
        View3DWorkspace { Layout.fillWidth: true; Layout.fillHeight: true; host: host }
        Rectangle { Layout.preferredWidth: 1; Layout.fillHeight: true; color: Theme.line }
        Rectangle {
            Layout.preferredWidth: 320; Layout.fillHeight: true; color: Theme.panel
            View3DPanel { anchors.fill: parent; host: host }
        }
    }
}
)");
        QVERIFY(root);
        QVERIFY(scene.addPointCloud(job + QStringLiteral("/odm_georeferencing/odm_georeferenced_model.laz")).isEmpty());
        QVERIFY(waitLoaded(scene));
        QVERIFY(scene.addTerrain(job + QStringLiteral("/odm_dem/dsm.tif"), job + QStringLiteral("/odm_orthophoto/odm_orthophoto.tif")).isEmpty());
        QVERIFY(waitLoaded(scene));
        auto layerOf = [&scene](const QString &kind) {
            for (const Scene3DService::Layer &l : scene.layers()) if (l.kind == kind) return l.id;
            return QString();
        };
        // The photos of the job over the point cloud, where they were taken.
        QTRY_VERIFY_WITH_TIMEOUT(!scene.cameras().isEmpty(), 60000);
        scene.setLayerVisible(layerOf(QStringLiteral("terrain")), false);
        scene.setPreset(QStringLiteral("iso"));
        scene.zoom(-2);
        QImage image = view.render();
        keep(image, QStringLiteral("brighton-workspace-cameras.png"));
        QVERIFY(coverage(image) > 0.2);
        // GEOMAP_3D_TILES=<XYZ tiles folder, e.g. the job's orthophoto cut by gdal2tiles --xyz>:
        // a basemap of the same place under the scene; the points must lie on their own image.
        const QString tiles = qEnvironmentVariable("GEOMAP_3D_TILES");
        if (!tiles.isEmpty()) {
            scene.setBasemap(QStringLiteral("custom:") + QUrl::fromLocalFile(tiles).toString() + QStringLiteral("/{z}/{x}/{y}.png"));
            QTRY_VERIFY_WITH_TIMEOUT([&] {
                for (const Scene3DService::Layer &l : scene.layers())
                    if (l.kind == QLatin1String("basemap") && l.mesh && !l.mesh->parts.isEmpty() && !l.mesh->parts[0].texture.isNull())
                        return webtiles::TileStore::instance()->pending() == 0 && !scene.basemapAttribution().isEmpty();
                return false;
            }(), 60000);
            QTest::qWait(1500);   // the last redraw after the last tiles
            for (const Scene3DService::Layer &l : scene.layers())
                if (l.kind == QLatin1String("basemap") && l.mesh) {
                    const QImage &d = l.mesh->parts[0].texture;
                    int opaque = 0;
                    for (int y = 0; y < d.height(); y += 4)
                        for (int x = 0; x < d.width(); x += 4) opaque += d.pixelColor(x, y).alpha() > 0;
                    keep(d, QStringLiteral("brighton-basemap-detail.png"));
                    qInfo("basemap detail %dx%d, %.1f %% drawn; plane z %.1f..%.1f, x %.1f..%.1f; data z %.1f; positions %d",
                          d.width(), d.height(), 100.0 * opaque / qMax(1, (d.width() / 4) * (d.height() / 4)), l.mesh->bounds.min.z(),
                          l.mesh->bounds.max.z(), l.mesh->bounds.min.x(), l.mesh->bounds.max.x(), scene.bounds().min.z(),
                          int(l.mesh->positions.size()));
                }
            image = view.render();
            keep(image, QStringLiteral("brighton-workspace-basemap.png"));
            scene.setLayerVisible(layerOf(QStringLiteral("pointcloud")), false);
            scene.setShowCameras(false);
            image = view.render();
            keep(image, QStringLiteral("brighton-workspace-basemap-only.png"));
            scene.setLayerVisible(layerOf(QStringLiteral("pointcloud")), true);
            scene.setShowCameras(true);
            scene.setBasemap(QString());
        }
        scene.setShowCameras(false);
        scene.setPreset(QStringLiteral("iso"));
        scene.orbit(-60, 20);
        QVERIFY(scene.pickAt(520, 400, 1040, 722));
        image = view.render();
        keep(image, QStringLiteral("brighton-workspace-cloud.png"));
        QVERIFY(coverage(image) > 0.3);
        scene.setLayerVisible(layerOf(QStringLiteral("pointcloud")), false);
        scene.setLayerVisible(layerOf(QStringLiteral("terrain")), true);
        scene.setTool(QStringLiteral("measure"));
        QVERIFY(scene.pickAt(400, 420, 1040, 722));
        QVERIFY(scene.pickAt(700, 360, 1040, 722));
        qInfo("%s | %s", qPrintable(scene.pickText()), qPrintable(scene.measureText()));
        image = view.render();
        keep(image, QStringLiteral("brighton-workspace-dsm.png"));
        delete root;
    }

    void cleanup()
    {
        m_scene.clear();
        m_scene.setColorMode(QStringLiteral("rgb"));
        m_scene.setVerticalScale(1);
        m_scene.setPointSize(2);
    }

    void cleanupTestCase()
    {
        delete m_item;
        m_item = nullptr;
    }

private:
    QQmlEngine m_engine;
    Scene3DService m_scene;
    OffscreenView m_view;
    View3DItem *m_item = nullptr;
    QTemporaryDir m_dir;
};

QTEST_MAIN(View3DRenderTest)
#include "test_view3d_render.moc"
