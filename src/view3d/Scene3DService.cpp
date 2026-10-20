#include "Scene3DService.h"

#include "Crs.h"
#include "Lidar.h"
#include "TrueOrthophoto.h"
#include "WebTiles.h"

#include <QDir>
#include <QHash>
#include <QFileInfo>
#include <QImageReader>
#include <QLocale>
#include <QPainter>
#include <QSettings>
#include <QThreadPool>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <limits>

using namespace view3d;

namespace {
const QLocale &ru()
{
    static const QLocale locale(QLocale::Russian);
    return locale;
}
QString count(quint64 n) { return ru().toString(qulonglong(n)); }
QString meters(double v, int decimals = 2) { return ru().toString(v, 'f', decimals) + QStringLiteral(" м"); }

const QString BasemapId = QStringLiteral("basemap");
const QString CamerasId = QStringLiteral("cameras");

// The ODM job folder of a file inside it (beside odm_georeferencing/coords.txt), or "".
QString jobFolderOf(const QString &file)
{
    const QString coords = odmCoordsFor(file);
    if (coords.isEmpty()) return {};
    QDir job = QFileInfo(coords).absoluteDir();
    job.cdUp();
    return job.absolutePath();
}

void addVertex(Mesh &m, const QVector3D &p, const QVector2D &uv, quint32 colour)
{
    m.positions << p;
    m.uvs << uv;
    m.normals << QVector3D(0, 0, 1);
    m.colors << colour;
}

// The basemap as a ground plane at height z: the area of the data in detail, around it a wider
// context of the same map (a ring, so the two never overlap). Rectangles in the scene CRS
// (x left→right, y from top() = south to bottom() = north); images with north up.
Mesh basemapMesh(const QRectF &inner, const QRectF &outer, float z, const Origin &origin, const QImage &detail, const QImage &context)
{
    Mesh m;
    m.origin = origin;
    m.georeferenced = true;
    const quint32 ground = 0xff1d222bu;   // where tiles are missing: about the colour of the background
    auto add = [&](double x, double y, const QRectF &image) {
        const QVector2D uv(float((x - image.left()) / image.width()), float((image.bottom() - y) / image.height()));
        addVertex(m, QVector3D(float(x - origin.x), float(y - origin.y), z), uv, ground);
        return quint32(m.positions.size() - 1);
    };
    Mesh::Part d;
    d.material = QStringLiteral("detail");
    if (!detail.isNull()) d.texture = detail.convertToFormat(QImage::Format_RGBA8888);
    const quint32 nw = add(inner.left(), inner.bottom(), inner), ne = add(inner.right(), inner.bottom(), inner),
                  se = add(inner.right(), inner.top(), inner), sw = add(inner.left(), inner.top(), inner);
    d.indices << nw << ne << se << nw << se << sw;
    Mesh::Part r;
    r.material = QStringLiteral("context");
    if (!context.isNull()) r.texture = context.convertToFormat(QImage::Format_RGBA8888);
    const quint32 onw = add(outer.left(), outer.bottom(), outer), one = add(outer.right(), outer.bottom(), outer),
                  ose = add(outer.right(), outer.top(), outer), osw = add(outer.left(), outer.top(), outer);
    const quint32 inw = add(inner.left(), inner.bottom(), outer), ine = add(inner.right(), inner.bottom(), outer),
                  ise = add(inner.right(), inner.top(), outer), isw = add(inner.left(), inner.top(), outer);
    auto quad = [&r](quint32 a, quint32 b, quint32 c, quint32 e) { r.indices << a << b << c << a << c << e; };
    quad(onw, one, ine, inw);
    quad(one, ose, ise, ine);
    quad(ose, osw, isw, ise);
    quad(osw, onw, inw, isw);
    m.parts << d << r;
    m.triangles = 10;
    for (const QVector3D &p : m.positions) m.bounds.add(p);
    return m;
}

// A line as two crossed thin quads (visible from above and from the side).
void addLine(Mesh &m, Mesh::Part &part, const QVector3D &a, const QVector3D &b, float width, quint32 colour)
{
    const QVector3D d = (b - a).normalized();
    QVector3D w1 = QVector3D::crossProduct(d, QVector3D(0, 0, 1));
    if (w1.lengthSquared() < 1e-6f) w1 = QVector3D::crossProduct(d, QVector3D(1, 0, 0));
    w1.normalize();
    const QVector3D w2 = QVector3D::crossProduct(d, w1).normalized();
    for (const QVector3D &w : {w1, w2}) {
        const QVector3D h = w * (width / 2);
        const quint32 i = quint32(m.positions.size());
        for (const QVector3D &p : {a - h, a + h, b + h, b - h}) addVertex(m, p, {}, colour);
        part.indices << i << i + 1 << i + 2 << i << i + 2 << i + 3;
    }
}

// The photos of a reconstruction as cameras: each photo (thumbnail) on its rectangle in front of
// the camera, as the camera saw it, and the frustum edges to the centre of projection. The
// rectangles are sized by the spacing of the cameras so that neighbours do not cover each other.
Mesh cameraMesh(const ortho::Scene &scene, const Origin &origin, QVector<Scene3DService::CameraMark> *marks)
{
    Mesh m;
    m.origin = origin;
    m.georeferenced = true;
    const int n = int(scene.shots.size());
    QVector<QVector3D> centres(n);
    for (int i = 0; i < n; ++i) {
        const ortho::Shot &s = scene.shots[i];
        centres[i] = QVector3D(float(s.centre[0] + scene.offsetX - origin.x), float(s.centre[1] + scene.offsetY - origin.y),
                               float(s.centre[2] - origin.z));
    }
    std::vector<float> nearest;
    for (int i = 0; i < n; ++i) {
        float best = std::numeric_limits<float>::max();
        for (int j = 0; j < n; ++j)
            if (j != i) best = qMin(best, (centres[i] - centres[j]).length());
        if (best > 0.01f && best < std::numeric_limits<float>::max()) nearest.push_back(best);
    }
    float spacing = 10.0f;
    if (!nearest.empty()) {
        std::nth_element(nearest.begin(), nearest.begin() + nearest.size() / 2, nearest.end());
        spacing = nearest[nearest.size() / 2];
    }
    const float halfWidth = 0.42f * spacing, lineWidth = qMax(0.02f, 0.045f * halfWidth);
    // Thumbnails in one atlas (one texture for all the photos).
    const int cols = qMax(1, int(std::ceil(std::sqrt(double(n)))));
    const int rows = qMax(1, (n + cols - 1) / cols);
    const int cellW = qBound(32, 4096 / cols, 256), cellH = cellW * 3 / 4;
    QImage atlas(cols * cellW, rows * cellH, QImage::Format_RGBA8888);
    atlas.fill(QColor(58, 142, 230));   // a photo that cannot be read: the colour of the frames
    QPainter painter(&atlas);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    Mesh::Part photos, frames;
    photos.material = QStringLiteral("photos");
    frames.material = QStringLiteral("frames");
    const quint32 blue = 0xff3a8ee6u;
    for (int i = 0; i < n; ++i) {
        const ortho::Shot &s = scene.shots[i];
        const ortho::Camera &c = s.camera;
        const int col = i % cols, row = i / cols;
        QImageReader reader(s.image);
        reader.setScaledSize(QSize(cellW, cellH));
        const QImage thumb = reader.read();
        if (!thumb.isNull()) painter.drawImage(QRect(col * cellW, row * cellH, cellW, cellH), thumb);
        // Corners of the photo at the depth that gives the rectangle its width.
        const double size = qMax(c.width, c.height);
        const double depth = halfWidth * 2.0 * c.fx * size / qMax(1, c.width);
        auto corner = [&](double px, double py) {
            const double xn = ((px - c.width / 2.0) / size - c.cx) / c.fx, yn = ((py - c.height / 2.0) / size - c.cy) / c.fy;
            const QVector3D dir(float(s.R[0] * xn + s.R[3] * yn + s.R[6]), float(s.R[1] * xn + s.R[4] * yn + s.R[7]),
                                float(s.R[2] * xn + s.R[5] * yn + s.R[8]));
            return centres[i] + dir * float(depth);
        };
        Scene3DService::CameraMark mark;
        mark.name = s.name.isEmpty() ? QFileInfo(s.image).fileName() : s.name;
        mark.centre = centres[i];
        mark.corners[0] = corner(0, 0);
        mark.corners[1] = corner(c.width, 0);
        mark.corners[2] = corner(c.width, c.height);
        mark.corners[3] = corner(0, c.height);
        const float u0 = (col * cellW + 0.5f) / atlas.width(), u1 = ((col + 1) * cellW - 0.5f) / atlas.width();
        const float v0 = (row * cellH + 0.5f) / atlas.height(), v1 = ((row + 1) * cellH - 0.5f) / atlas.height();
        const quint32 first = quint32(m.positions.size());
        addVertex(m, mark.corners[0], QVector2D(u0, v0), 0xffffffffu);
        addVertex(m, mark.corners[1], QVector2D(u1, v0), 0xffffffffu);
        addVertex(m, mark.corners[2], QVector2D(u1, v1), 0xffffffffu);
        addVertex(m, mark.corners[3], QVector2D(u0, v1), 0xffffffffu);
        photos.indices << first << first + 1 << first + 2 << first << first + 2 << first + 3;
        for (int k = 0; k < 4; ++k) {
            addLine(m, frames, mark.corners[k], mark.corners[(k + 1) % 4], lineWidth, blue);
            addLine(m, frames, mark.centre, mark.corners[k], lineWidth, blue);
        }
        if (marks) *marks << mark;
    }
    painter.end();
    photos.texture = atlas;
    m.parts << photos << frames;
    m.triangles = quint64(photos.indices.size() + frames.indices.size()) / 3;
    for (const QVector3D &p : m.positions) m.bounds.add(p);
    return m;
}
} // namespace

Scene3DService::Scene3DService(QObject *parent) : QObject(parent)
{
    m_pool.setMaxThreadCount(2);
    m_basemapTimer.setSingleShot(true);
    connect(&m_basemapTimer, &QTimer::timeout, this, &Scene3DService::buildBasemap);
    // New tiles of the basemap (downloaded for this view or the map): drawn again, coalesced.
    // Also while the first drawing is still in progress: its tiles may come before it ends.
    // Once every tile of the area is there, tiles of the map elsewhere do not redraw it.
    connect(webtiles::TileStore::instance(), &webtiles::TileStore::arrived, this, [this](const QString &provider) {
        if (!m_basemap.isEmpty() && provider == m_basemap && (!m_basemapComplete || m_basemapBusy)) scheduleBasemap(true);
    });
}

Scene3DService::~Scene3DService()
{
    *m_alive = false;
    m_pool.clear();
    m_pool.waitForDone();
}

QString Scene3DService::addPointCloud(const QString &path) { return add(QStringLiteral("pointcloud"), path, {}); }
QString Scene3DService::addModel(const QString &path) { return add(QStringLiteral("mesh"), path, {}); }
QString Scene3DService::addTerrain(const QString &demPath, const QString &drapePath) { return add(QStringLiteral("terrain"), demPath, drapePath); }

QString Scene3DService::addFile(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("las") || suffix == QLatin1String("laz")) return addPointCloud(path);
    if (suffix == QLatin1String("obj")) return addModel(path);
    if (suffix == QLatin1String("tif") || suffix == QLatin1String("tiff") || suffix == QLatin1String("vrt")) return addTerrain(path);
    if (suffix == QLatin1String("ply")) {
        // A PLY with faces is a model, without them a point cloud.
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) {
            const QByteArray header = f.read(4096);
            const int face = header.indexOf("element face ");
            if (face >= 0 && header.mid(face + 13, 2) != "0\n" && header.mid(face + 13, 2) != "0\r") return addModel(path);
        }
        return addPointCloud(path);
    }
    return QStringLiteral("Формат не поддерживается: %1. 3D-просмотр открывает облака PLY/LAS, модели OBJ/PLY и модели высот GeoTIFF.").arg(suffix);
}

QString Scene3DService::addUrl(const QUrl &url, const QString &kind)
{
    const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
    if (kind == QLatin1String("pointcloud")) return addPointCloud(path);
    if (kind == QLatin1String("mesh")) return addModel(path);
    if (kind == QLatin1String("terrain")) return addTerrain(path);
    return addFile(path);
}

QString Scene3DService::layerIdForSource(const QString &path) const
{
    const QString wanted = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    for (const Layer &l : m_layers)
        if (!l.overlay && QDir::cleanPath(QFileInfo(l.source).absoluteFilePath()).compare(wanted, Qt::CaseInsensitive) == 0) return l.id;
    return {};
}

QString Scene3DService::add(const QString &kind, const QString &path, const QString &drape)
{
    if (!QFileInfo(path).isFile()) return QStringLiteral("Файл не найден: %1").arg(QDir::toNativeSeparators(path));
    if (!layerIdForSource(path).isEmpty()) return QStringLiteral("«%1» уже в 3D-сцене.").arg(QFileInfo(path).fileName());
    Layer l;
    l.id = QStringLiteral("l%1").arg(m_nextId++);
    l.kind = kind;
    l.source = QFileInfo(path).absoluteFilePath();
    l.drape = drape;
    l.name = QFileInfo(path).completeBaseName();
    l.job = jobFolderOf(path);
    l.info = QStringLiteral("Загрузка…");
    l.version = 1;
    // LAZ: an uncompressed LAS beside it (ODM --pc-las: the same points with classes and
    // intensity), else the LAZ unpacked by PDAL of the bundled ODM; without ODM, the job's
    // odm_filterpoints/point_cloud.ply.
    QString path2 = path, fallback;
    if (kind == QLatin1String("pointcloud") && path.endsWith(QLatin1String(".laz"), Qt::CaseInsensitive)) {
        const QString las = path.left(path.size() - 4) + QStringLiteral(".las");
        const QString coords = odmCoordsFor(path);
        if (QFileInfo(las).isFile()) {
            path2 = las;
            l.loaded = las;
        } else if (!coords.isEmpty()) {
            QDir job = QFileInfo(coords).absoluteDir();
            job.cdUp();
            const QString ply = job.filePath(QStringLiteral("odm_filterpoints/point_cloud.ply"));
            if (QFileInfo(ply).isFile()) fallback = ply;
        }
    }
    m_layers << l;
    const QString id = l.id;
    const int budget = m_pointBudget;
    const Origin origin = m_origin;
    std::weak_ptr<bool> alive = m_alive;
    m_pool.start([this, alive, id, kind, path2, fallback, drape, budget, origin, job = l.job] {
        std::shared_ptr<const PointCloud> cloud;
        std::shared_ptr<const Mesh> mesh;
        QString error, path = path2, shown, lazError;
        if (kind == QLatin1String("pointcloud") && path.endsWith(QLatin1String(".laz"), Qt::CaseInsensitive)) {
            const QString las = lidar::uncompressed(path, &lazError, [alive] { return !alive.lock(); });
            if (!las.isEmpty()) path = las;
            else if (!fallback.isEmpty()) path = shown = fallback;
        }
        // ODM's local coordinates: the CRS of the job's results (for the basemap).
        auto jobCrs = [&job](bool georeferenced, QString &wkt) {
            if (georeferenced && wkt.isEmpty() && !job.isEmpty()) wkt = ortho::resultCrsWkt(job);
        };
        if (kind == QLatin1String("pointcloud")) {
            auto c = std::make_shared<PointCloud>(readPointCloud(path, budget));
            jobCrs(c->georeferenced, c->crsWkt);
            error = !c->error.isEmpty() && !lazError.isEmpty() ? QStringLiteral("LAZ не распакован: ") + lazError : c->error;
            cloud = c;
        } else if (kind == QLatin1String("mesh")) {
            const bool ply = path.endsWith(QLatin1String(".ply"), Qt::CaseInsensitive);
            auto m = std::make_shared<Mesh>(ply ? readMeshPly(path) : readObj(path));
            jobCrs(m->georeferenced, m->crsWkt);
            error = m->error;
            mesh = m;
        } else {
            QString note;
            const QImage image = drape.isEmpty() ? QImage() : readDrape(drape, path, 4096, &note);
            // DEM coordinates relative to the scene origin when there is one.
            auto m = std::make_shared<Mesh>(demMesh(path, 1024, origin, image));
            m->note = note;
            error = m->error;
            mesh = m;
        }
        if (!alive.lock()) return;
        QMetaObject::invokeMethod(this, [this, alive, id, cloud, mesh, error, shown] {
            if (!alive.lock()) return;
            for (Layer &l : m_layers)
                if (l.id == id && !shown.isEmpty()) l.loaded = shown;
            loaded(id, 1, cloud, mesh, error);
        }, Qt::QueuedConnection);
    });
    ++m_revision;
    emit layersChanged();
    emit changed();
    return {};
}

void Scene3DService::loaded(const QString &id, int, std::shared_ptr<const PointCloud> cloud, std::shared_ptr<const Mesh> mesh,
                            const QString &error)
{
    for (Layer &l : m_layers) {
        if (l.id != id) continue;
        l.loading = false;
        l.error = error;
        l.cloud = cloud;
        l.mesh = mesh;
        ++l.version;
        const Origin layerOrigin = cloud ? cloud->origin : mesh ? mesh->origin : Origin{};
        if (error.isEmpty() && !m_origin.valid && layerOrigin.valid) m_origin = layerOrigin;
        if (!error.isEmpty()) {
            l.info = error;
            emit errorOccurred(QStringLiteral("%1: %2").arg(l.name, error));
        } else if (cloud) {
            l.info = cloud->totalPoints > quint64(cloud->positions.size())
                ? QStringLiteral("%1 точек, показано %2 (предел памяти)").arg(count(cloud->totalPoints), count(cloud->positions.size()))
                : QStringLiteral("%1 точек").arg(count(cloud->positions.size()));
            if (cloud->colors.isEmpty()) l.info += QStringLiteral(", без цвета");
            if (!cloud->classes.isEmpty()) {
                // The classes present, largest first: "земля 46 %, высокая растительность 30 %, …".
                QHash<int, qsizetype> counts;
                for (quint8 c : cloud->classes) ++counts[c];
                QList<QPair<qsizetype, int>> order;
                for (auto it = counts.cbegin(); it != counts.cend(); ++it) order << qMakePair(it.value(), it.key());
                std::sort(order.begin(), order.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
                QStringList parts;
                for (int k = 0; k < qMin(4, int(order.size())); ++k)
                    parts << QStringLiteral("%1 %2 %").arg(className(order[k].second)).arg(qRound(100.0 * order[k].first / cloud->classes.size()));
                l.info += QStringLiteral(". Классы: ") + parts.join(QStringLiteral(", "));
            }
            if (!cloud->intensity.isEmpty()) l.info += QStringLiteral(", есть интенсивность");
            if (!l.loaded.isEmpty())
                l.info += QStringLiteral(". Показан %1 того же задания").arg(QFileInfo(l.loaded).fileName());
            l.info += cloud->georeferenced ? (cloud->crs.isEmpty() ? QString() : QStringLiteral(". CRS: ") + cloud->crs)
                                           : QStringLiteral(". Без геопривязки: координаты файла");
        } else if (mesh) {
            int textures = 0;
            for (const Mesh::Part &p : mesh->parts) textures += p.texture.isNull() ? 0 : 1;
            l.info = QStringLiteral("%1 треугольников, %2 вершин").arg(count(mesh->triangles), count(mesh->positions.size()));
            if (textures) l.info += QStringLiteral(", текстур: %1").arg(textures);
            if (l.kind == QLatin1String("terrain"))
                l.info = QStringLiteral("высоты %1 – %2, %3").arg(meters(mesh->bounds.min.z() + mesh->origin.z, 1),
                                                                   meters(mesh->bounds.max.z() + mesh->origin.z, 1), l.info);
            if (!mesh->note.isEmpty()) l.info += QStringLiteral(". ") + mesh->note.trimmed();
            l.info += mesh->georeferenced ? (mesh->crs.isEmpty() ? QString() : QStringLiteral(". CRS: ") + mesh->crs)
                                          : QStringLiteral(". Без геопривязки: координаты файла");
        }
        break;
    }
    if (!m_cameraFitted && bounds().valid) {
        m_cameraFitted = true;
        applyPreset(Camera::Iso);
    }
    ++m_revision;
    emit layersChanged();
    emit changed();
    dataChanged();
}

void Scene3DService::removeLayer(const QString &id)
{
    if (const Layer *o = overlay(id)) {   // the basemap and the cameras are hidden, not removed
        if (o->visible) setLayerVisible(id, false);
        return;
    }
    for (int i = 0; i < m_layers.size(); ++i)
        if (m_layers[i].id == id) {
            m_layers.removeAt(i);
            break;
        }
    bool data = false;
    for (const Layer &l : m_layers) data = data || !l.overlay;
    if (!data) {
        removeOverlays();
        m_origin = {};
        m_cameraFitted = false;
    }
    ++m_revision;
    emit layersChanged();
    emit changed();
    if (data) dataChanged();
}

void Scene3DService::setLayerVisible(const QString &id, bool visible)
{
    for (Layer &l : m_layers)
        if (l.id == id && l.visible != visible) {
            l.visible = visible;
            if (id == CamerasId && m_showCameras != visible) {
                m_showCameras = visible;
                emit settingsChanged();
            }
            ++m_revision;
            emit layersChanged();
            emit changed();
            if (id == BasemapId) emit basemapChanged();   // attribution only while it is shown
        }
}

void Scene3DService::clear()
{
    m_layers.clear();
    removeOverlays();
    m_origin = {};
    m_cameraFitted = false;
    m_measure.clear();
    m_hasPick = false;
    m_pickText.clear();
    m_measureText.clear();
    ++m_revision;
    emit layersChanged();
    emit pickChanged();
    emit changed();
}

QVector3D Scene3DService::layerOffset(const Layer &l) const
{
    const Origin o = l.cloud ? l.cloud->origin : l.mesh ? l.mesh->origin : Origin{};
    if (!o.valid || !m_origin.valid) return {};
    return QVector3D(float(o.x - m_origin.x), float(o.y - m_origin.y), float(o.z - m_origin.z));
}

Bounds Scene3DService::bounds() const
{
    Bounds b;
    for (const Layer &l : m_layers) {
        if (!l.visible || l.overlay) continue;
        const Bounds lb = l.cloud ? l.cloud->bounds : l.mesh ? l.mesh->bounds : Bounds{};
        if (!lb.valid) continue;
        const QVector3D off = layerOffset(l);
        b.add(lb.min + off);
        b.add(lb.max + off);
    }
    return b;
}

Scene3DService::Snapshot Scene3DService::snapshot() const
{
    Snapshot s;
    s.layers = m_layers;
    for (Layer &l : s.layers) {
        // The renderer needs only the offset of each layer; keep it in `info`-free copies small.
        l.info.clear();
    }
    s.origin = m_origin;
    s.bounds = bounds();
    s.extent = s.bounds;
    for (const Layer &l : m_layers)
        if (l.visible && l.overlay && l.mesh && l.mesh->bounds.valid) {
            s.extent.add(l.mesh->bounds.min + layerOffset(l));
            s.extent.add(l.mesh->bounds.max + layerOffset(l));
        }
    s.camera = m_camera;
    s.pointSize = m_pointSize;
    s.colorMode = m_colorMode;
    s.verticalScale = m_verticalScale;
    s.lighting = m_lighting;
    s.revision = m_revision;
    return s;
}

QVariantList Scene3DService::layersForQml() const
{
    QVariantList list;
    for (const Layer &l : m_layers) {
        static const QHash<QString, QString> kinds = {{QStringLiteral("pointcloud"), QStringLiteral("Облако точек")},
                                                      {QStringLiteral("mesh"), QStringLiteral("3D-модель")},
                                                      {QStringLiteral("terrain"), QStringLiteral("Рельеф (ЦММ)")},
                                                      {BasemapId, QStringLiteral("Подложка карты под сценой")},
                                                      {CamerasId, QStringLiteral("Снимки на местах съёмки")}};
        list << QVariantMap{{"id", l.id}, {"name", l.name}, {"kind", l.kind}, {"kindTitle", kinds.value(l.kind)},
                            {"overlay", l.overlay}, {"visible", l.visible}, {"loading", l.loading}, {"info", l.info}, {"error", l.error},
                            {"source", l.overlay ? QString() : QDir::toNativeSeparators(l.source)}};
    }
    return list;
}

bool Scene3DService::loading() const
{
    for (const Layer &l : m_layers) if (l.loading) return true;
    return false;
}

QString Scene3DService::status() const
{
    if (m_layers.isEmpty()) return QStringLiteral("Сцена пуста");
    if (loading()) return QStringLiteral("Загрузка…");
    quint64 points = 0, triangles = 0;
    for (const Layer &l : m_layers) {
        if (!l.visible || l.overlay) continue;
        if (l.cloud) points += quint64(l.cloud->positions.size());
        if (l.mesh) triangles += l.mesh->triangles;
    }
    QStringList parts;
    if (points) parts << QStringLiteral("%1 точек").arg(count(points));
    if (triangles) parts << QStringLiteral("%1 треугольников").arg(count(triangles));
    return parts.isEmpty() ? QStringLiteral("Нет видимых слоёв") : parts.join(QStringLiteral(" · "));
}

// ------------------------------------------------------------------ camera

void Scene3DService::orbit(float dx, float dy)
{
    m_camera.orbit(dx, dy);
    emit viewChanged();
    emit changed();
}

void Scene3DService::pan(float dx, float dy, float viewportHeight)
{
    m_camera.pan(dx, dy, viewportHeight);
    emit viewChanged();
    emit changed();
}

void Scene3DService::zoom(float steps)
{
    m_camera.zoom(steps);
    emit viewChanged();
    emit changed();
}

void Scene3DService::setViewAspect(float aspect)
{
    if (aspect > 0.01f && std::isfinite(aspect)) m_camera.aspect = aspect;
}

Bounds Scene3DService::drawnBounds(QVector<QVector3D> *samples) const
{
    Bounds b = bounds();
    if (b.valid) {
        b.min.setZ(b.min.z() * m_verticalScale);
        b.max.setZ(b.max.z() * m_verticalScale);
    }
    if (!samples) return b;
    qsizetype total = 0;
    for (const Layer &l : m_layers)
        if (l.visible && !l.overlay) total += l.cloud ? l.cloud->positions.size() : l.mesh ? l.mesh->positions.size() : 0;
    const qsizetype step = qMax<qsizetype>(1, total / 20000);
    for (const Layer &l : m_layers) {
        if (!l.visible || l.overlay || (!l.cloud && !l.mesh)) continue;
        const QVector<QVector3D> &positions = l.cloud ? l.cloud->positions : l.mesh->positions;
        const QVector3D off = layerOffset(l);
        for (qsizetype i = 0; i < positions.size(); i += step) {
            const QVector3D p = positions[i] + off;
            samples->append(QVector3D(p.x(), p.y(), p.z() * m_verticalScale));
        }
    }
    return b;
}

void Scene3DService::applyPreset(Camera::Preset preset)
{
    QVector<QVector3D> samples;
    const Bounds b = drawnBounds(&samples);
    m_camera.setPreset(preset, b, samples);
    emit viewChanged();
    emit changed();
}

void Scene3DService::fitAll()
{
    QVector<QVector3D> samples;
    const Bounds b = drawnBounds(&samples);
    m_camera.fit(b, samples);
    emit viewChanged();
    emit changed();
}

void Scene3DService::setPreset(const QString &preset)
{
    static const QHash<QString, Camera::Preset> presets = {
        {QStringLiteral("top"), Camera::Top}, {QStringLiteral("bottom"), Camera::Bottom}, {QStringLiteral("front"), Camera::Front},
        {QStringLiteral("back"), Camera::Back}, {QStringLiteral("left"), Camera::Left}, {QStringLiteral("right"), Camera::Right},
        {QStringLiteral("iso"), Camera::Iso}};
    applyPreset(presets.value(preset, Camera::Iso));
}

bool Scene3DService::georeferenced() const
{
    bool any = false;
    for (const Layer &l : m_layers) {
        if (!l.visible || l.overlay || l.loading || !l.error.isEmpty()) continue;
        const bool geo = l.cloud ? l.cloud->georeferenced : l.mesh ? l.mesh->georeferenced : false;
        if (!geo) return false;
        any = true;
    }
    return any;
}

double Scene3DService::lastPickAbsolute(int axis) const
{
    const double o = axis == 0 ? m_origin.x : axis == 1 ? m_origin.y : m_origin.z;
    return o + double(axis == 0 ? m_lastPick.x() : axis == 1 ? m_lastPick.y() : m_lastPick.z());
}

bool Scene3DService::pickAt(float x, float y, float width, float height)
{
    QVector3D origin, direction;
    m_camera.ray(QPointF(x, y), QSizeF(width, height), &origin, &direction);
    // The view stretches heights by the vertical scale; pick in data coordinates.
    const float s = qMax(0.01f, m_verticalScale);
    origin.setZ(origin.z() / s);
    direction.setZ(direction.z() / s);
    direction.normalize();
    // Tolerance: a few pixels at the target distance.
    const float worldPerPixel = m_camera.orthoHeight() / qMax(1.0f, height);
    Pick best;
    for (const Layer &l : m_layers) {
        if (!l.visible || l.overlay || l.loading || !l.error.isEmpty()) continue;
        const QVector3D off = layerOffset(l);
        const QVector3D o = origin - off;
        Pick p;
        if (l.cloud) {
            const int stride = qMax(1, int(l.cloud->positions.size() / 2000000));
            p = pickPoint(*l.cloud, o, direction, worldPerPixel * 6.0f, stride);
        } else if (l.mesh) {
            p = pickMesh(*l.mesh, o, direction);
        }
        if (p.hit && (!best.hit || p.distance < best.distance)) {
            best = p;
            best.position += off;
        }
    }
    // A photo in front of the data: its name and where it was taken.
    const Layer *cams = overlay(CamerasId);
    if (cams && cams->visible && cams->mesh && !m_cameras.isEmpty()) {
        const QVector3D off = layerOffset(*cams);
        const Pick p = pickMesh(*cams->mesh, origin - off, direction);
        if (p.hit && (!best.hit || p.distance < best.distance)) {
            int nearest = 0;
            float distance = std::numeric_limits<float>::max();
            for (int i = 0; i < m_cameras.size(); ++i) {
                const CameraMark &c = m_cameras[i];
                const QVector3D middle = (c.corners[0] + c.corners[1] + c.corners[2] + c.corners[3]) / 4;
                const float d = qMin((p.position - middle).length(), (p.position - c.centre).length());
                if (d < distance) { distance = d; nearest = i; }
            }
            const CameraMark &c = m_cameras[nearest];
            m_pickText = QStringLiteral("Снимок %1 · центр съёмки X %2  Y %3  Z %4").arg(c.name,
                ru().toString(m_origin.x + c.centre.x() + off.x(), 'f', 2), ru().toString(m_origin.y + c.centre.y() + off.y(), 'f', 2),
                meters(m_origin.z + c.centre.z() + off.z()));
            emit pickChanged();
            emit changed();
            return true;
        }
    }
    if (!best.hit) {
        m_pickText = QStringLiteral("Нет точки под курсором");
        emit pickChanged();
        return false;
    }
    m_lastPick = best.position;
    m_hasPick = true;
    // Without georeferencing (and so without a known scale) values are in file units, not metres.
    const bool geo = georeferenced();
    const auto length = [geo](double v) { return geo ? meters(v) : ru().toString(v, 'f', 2); };
    m_pickText = QStringLiteral("%1 X %2  Y %3  Z %4").arg(geo ? QStringLiteral("Точка:") : QStringLiteral("Точка (координаты файла):"),
                                                         ru().toString(lastPickAbsolute(0), 'f', 2), ru().toString(lastPickAbsolute(1), 'f', 2),
                                                         length(lastPickAbsolute(2)));
    if (m_tool == QLatin1String("measure")) {
        if (m_measure.size() >= 2) m_measure.clear();
        m_measure << best.position;
        if (m_measure.size() == 1) {
            m_measureText = QStringLiteral("Выберите вторую точку");
        } else {
            const QVector3D d = m_measure[1] - m_measure[0];
            const double horizontal = std::hypot(double(d.x()), double(d.y()));
            const double slope = horizontal > 1e-9 ? qRadiansToDegrees(std::atan2(std::fabs(double(d.z())), horizontal)) : 90.0;
            m_measureText = QStringLiteral("Расстояние %1 · по горизонтали %2 · перепад %3 · уклон %4°")
                                .arg(length(d.length()), length(horizontal), length(double(d.z())), ru().toString(slope, 'f', 1));
            if (!geo) m_measureText += QStringLiteral(" — в единицах файла: без геопривязки и масштаба метрические значения не выдаются");
        }
    }
    emit pickChanged();
    emit changed();
    return true;
}

void Scene3DService::clearMeasurement()
{
    m_measure.clear();
    m_measureText.clear();
    emit pickChanged();
    emit changed();
}

// ------------------------------------------------------------------ settings

void Scene3DService::setPointBudget(int points)
{
    points = qBound(100000, points, 50000000);
    if (points == m_pointBudget) return;
    m_pointBudget = points;
    emit settingsChanged();
}

void Scene3DService::setPointSize(float size)
{
    size = qBound(1.0f, size, 12.0f);
    if (qFuzzyCompare(size, m_pointSize)) return;
    m_pointSize = size;
    emit settingsChanged();
    emit changed();
}

QString Scene3DService::colorMode() const
{
    static const char *names[] = {"rgb", "height", "classes", "intensity"};
    return QString::fromLatin1(names[qBound(0, m_colorMode, 3)]);
}

void Scene3DService::setColorMode(const QString &mode)
{
    const int m = mode == QLatin1String("height") ? 1 : mode == QLatin1String("classes") ? 2 : mode == QLatin1String("intensity") ? 3 : 0;
    if (m == m_colorMode) return;
    m_colorMode = m;
    emit settingsChanged();
    emit changed();
}
void Scene3DService::setVerticalScale(float scale)
{
    scale = qBound(0.1f, scale, 20.0f);
    if (qFuzzyCompare(scale, m_verticalScale)) return;
    // The camera looks at the same data point: its height is stretched with the scene.
    m_camera.target.setZ(m_camera.target.z() * scale / m_verticalScale);
    m_verticalScale = scale;
    emit viewChanged();
    emit settingsChanged();
    emit changed();
}

void Scene3DService::setLighting(bool on)
{
    if (on == m_lighting) return;
    m_lighting = on;
    emit settingsChanged();
    emit changed();
}

void Scene3DService::setShowAxes(bool on)
{
    if (on == m_showAxes) return;
    m_showAxes = on;
    emit settingsChanged();
}

void Scene3DService::setOrthographic(bool on)
{
    if (on == m_camera.orthographic) return;
    m_camera.orthographic = on;
    emit viewChanged();
    emit changed();
}

void Scene3DService::setTool(const QString &tool)
{
    if (tool == m_tool) return;
    m_tool = tool;
    if (tool != QLatin1String("measure")) clearMeasurement();
    emit toolChanged();
}

// ------------------------------------------------------------------ basemap and cameras

void Scene3DService::setBasemap(const QString &providerId)
{
    QString id = providerId;
    // As on the map: a built-in server only after its terms were accepted.
    if (!id.isEmpty() && !id.startsWith(QLatin1String("custom:"))
        && !QSettings().value(QStringLiteral("basemap/accepted/") + id).toBool())
        id.clear();
    if (!id.isEmpty() && !webtiles::provider(id).isValid()) id.clear();
    if (id == m_basemap) return;
    m_basemap = id;
    m_basemapKey.clear();
    m_basemapComplete = false;
    ++m_basemapGeneration;
    for (int i = 0; i < m_layers.size(); ++i)
        if (m_layers[i].overlay && m_layers[i].id == BasemapId) {
            m_layers.removeAt(i);
            ++m_revision;
            emit layersChanged();
            emit changed();
            break;
        }
    if (!id.isEmpty()) scheduleBasemap(false);
    emit basemapChanged();
}

QString Scene3DService::basemapAttribution() const
{
    for (const Layer &l : m_layers)
        if (l.overlay && l.id == BasemapId && l.visible && l.error.isEmpty()) return webtiles::provider(m_basemap).attribution;
    return {};
}

void Scene3DService::setShowCameras(bool on)
{
    if (on == m_showCameras) return;
    m_showCameras = on;
    if (overlay(CamerasId)) setLayerVisible(CamerasId, on);
    emit settingsChanged();
}

Scene3DService::Layer *Scene3DService::overlay(const QString &id)
{
    for (Layer &l : m_layers)
        if (l.overlay && l.id == id) return &l;
    return nullptr;
}

void Scene3DService::setOverlay(Layer layer)
{
    layer.overlay = true;
    layer.loading = false;
    bool replaced = false;
    for (Layer &l : m_layers)
        if (l.overlay && l.id == layer.id) {
            layer.version = l.version + 1;
            layer.visible = l.visible;   // as the user left it
            l = layer;
            replaced = true;
            break;
        }
    if (!replaced) {
        layer.version = 1;
        m_layers << layer;
    }
    ++m_revision;
    emit layersChanged();
    emit changed();
    if (layer.id == BasemapId) emit basemapChanged();
}

void Scene3DService::removeOverlays()
{
    const bool basemap = overlay(BasemapId) != nullptr;
    m_layers.erase(std::remove_if(m_layers.begin(), m_layers.end(), [](const Layer &l) { return l.overlay; }), m_layers.end());
    m_cameras.clear();
    m_camerasJob.clear();
    m_basemapKey.clear();
    m_basemapComplete = false;
    ++m_basemapGeneration;   // drawings in progress are dropped
    if (basemap) emit basemapChanged();
}

void Scene3DService::dataChanged()
{
    bool data = false;
    for (const Layer &l : m_layers) data = data || !l.overlay;
    if (!data) {
        removeOverlays();
        return;
    }
    // The photos of the job the first georeferenced layer comes from.
    if (m_camerasJob.isEmpty() && m_origin.valid)
        for (const Layer &l : m_layers) {
            if (l.overlay || l.loading || !l.error.isEmpty() || l.job.isEmpty()) continue;
            if (!(l.cloud ? l.cloud->georeferenced : l.mesh && l.mesh->georeferenced)) continue;
            loadCameras(l.job);
            break;
        }
    if (!m_basemap.isEmpty()) scheduleBasemap(false);
}

QString Scene3DService::sceneCrsWkt() const
{
    // The first definition PROJ understands (a LAS record may be incomplete); for files of an
    // ODM job, the CRS of the job's results.
    auto usable = [](const QString &wkt) {
        if (wkt.isEmpty()) return false;
        const Crs crs = Crs::fromWkt(wkt);
        return crs.isValid() && crs.metersPerUnit() > 0;   // a projected CRS: the scene is in metres
    };
    for (const Layer &l : m_layers) {
        if (l.overlay || l.loading || !l.error.isEmpty()) continue;
        const QString own = l.cloud ? l.cloud->crsWkt : l.mesh ? l.mesh->crsWkt : QString();
        if (usable(own)) return own;
        if (!l.job.isEmpty()) {
            const QString job = ortho::resultCrsWkt(l.job);
            if (usable(job)) return job;
        }
    }
    return {};
}

void Scene3DService::loadCameras(const QString &job)
{
    m_camerasJob = job;
    const Origin origin = m_origin;
    std::weak_ptr<bool> alive = m_alive;
    m_pool.start([this, alive, job, origin] {
        ortho::Scene scene;
        QString error;
        std::shared_ptr<const Mesh> mesh;
        QVector<CameraMark> marks;
        // Poses even without the photos (then the rectangles are plain).
        if (ortho::readOdmJob(job, &scene, &error, {}, false)) mesh = std::make_shared<const Mesh>(cameraMesh(scene, origin, &marks));
        if (!alive.lock()) return;
        QMetaObject::invokeMethod(this, [this, alive, job, mesh, marks] {
            if (!alive.lock() || job != m_camerasJob || !mesh || marks.isEmpty()) return;
            m_cameras = marks;
            Layer l;
            l.id = CamerasId;
            l.kind = CamerasId;
            l.name = QStringLiteral("Снимки");
            l.visible = m_showCameras;
            l.info = QStringLiteral("Снимков: %1 — на позициях и с наклоном из реконструкции задания; щелчок по снимку — его имя")
                         .arg(marks.size());
            l.mesh = mesh;
            setOverlay(l);
            if (!m_basemap.isEmpty()) scheduleBasemap(false);   // the area includes the cameras
        }, Qt::QueuedConnection);
    });
}

void Scene3DService::scheduleBasemap(bool force)
{
    m_basemapForce = m_basemapForce || force;
    // New tiles arrive one by one: drawn again at most every 0.7 s.
    if (!m_basemapTimer.isActive()) m_basemapTimer.start(force ? 700 : 0);
}

void Scene3DService::buildBasemap()
{
    const bool force = m_basemapForce;
    m_basemapForce = false;
    if (m_basemap.isEmpty() || !m_origin.valid) return;
    if (m_basemapBusy) {
        m_basemapAgain = true;
        m_basemapForce = m_basemapForce || force;
        return;
    }
    // The area of the data (every loaded layer, shown or not) and of the cameras.
    Bounds data;
    for (const Layer &l : m_layers) {
        if (l.overlay || l.loading || !l.error.isEmpty()) continue;
        const Bounds lb = l.cloud ? l.cloud->bounds : l.mesh ? l.mesh->bounds : Bounds{};
        if (!lb.valid) continue;
        data.add(lb.min + layerOffset(l));
        data.add(lb.max + layerOffset(l));
    }
    if (!data.valid) return;
    const webtiles::Provider provider = webtiles::provider(m_basemap);
    Layer l;
    l.id = BasemapId;
    l.kind = BasemapId;
    l.name = QStringLiteral("Подложка · ") + provider.title;
    const QString wkt = sceneCrsWkt();
    if (wkt.isEmpty()) {
        l.error = l.info = QStringLiteral("У данных сцены нет системы координат: подложку не к чему привязать.");
        setOverlay(l);
        return;
    }
    double x0 = data.min.x(), y0 = data.min.y(), x1 = data.max.x(), y1 = data.max.y();
    if (Layer *cams = overlay(CamerasId)) {
        const QVector3D off = layerOffset(*cams);
        for (const CameraMark &c : m_cameras) {
            x0 = qMin(x0, double(c.centre.x() + off.x()));
            x1 = qMax(x1, double(c.centre.x() + off.x()));
            y0 = qMin(y0, double(c.centre.y() + off.y()));
            y1 = qMax(y1, double(c.centre.y() + off.y()));
        }
    }
    x0 += m_origin.x; x1 += m_origin.x; y0 += m_origin.y; y1 += m_origin.y;
    // In detail around the data, and a wider context (at least 3 km across) to see where it is.
    const double size = qMax(20.0, qMax(x1 - x0, y1 - y0));
    const double margin = qMax(30.0, 0.3 * size);
    const QRectF inner(x0 - margin, y0 - margin, x1 - x0 + 2 * margin, y1 - y0 + 2 * margin);
    const double half = qMax(1500.0, 1.5 * qMax(inner.width(), inner.height()));
    const QRectF outer(inner.center().x() - half, inner.center().y() - half, 2 * half, 2 * half);
    // On the ground: just under the lowest data (robust bounds), so the data stand on it.
    const float z = data.min.z() - qMax(0.3f, 0.002f * data.radius());
    const QString key = QStringLiteral("%1|%2|%3|%4|%5|%6").arg(m_basemap).arg(inner.x(), 0, 'f', 0).arg(inner.y(), 0, 'f', 0)
                            .arg(inner.width(), 0, 'f', 0).arg(inner.height(), 0, 'f', 0).arg(double(z), 0, 'f', 1);
    if (!force && key == m_basemapKey) return;
    if (key != m_basemapKey) m_basemapComplete = false;
    m_basemapKey = key;
    m_basemapBusy = true;
    const int generation = m_basemapGeneration;
    const Origin origin = m_origin;
    const Crs crs = Crs::fromWkt(wkt);
    const double longer = qMax(inner.width(), inner.height());
    const QSize detailSize(qMax(64, int(2048 * inner.width() / longer)), qMax(64, int(2048 * inner.height() / longer)));
    std::weak_ptr<bool> alive = m_alive;
    m_pool.start([this, alive, generation, provider, crs, inner, outer, z, origin, detailSize, name = l.name] {
        webtiles::WebRasterSource source(provider, crs, inner.center());
        const QString error = source.error();
        QImage detail, context;
        bool complete = false;
        if (error.isEmpty()) {
            bool detailComplete = false, contextComplete = false;   // tiles not yet here are queued for download
            detail = source.renderRegion(inner, detailSize, &detailComplete);
            context = source.renderRegion(outer, QSize(1024, 1024), &contextComplete);
            complete = detailComplete && contextComplete;
        }
        qint64 drawn = 0, samples = 0;
        for (int y = 0; y < detail.height(); y += 8) {
            const QRgb *line = reinterpret_cast<const QRgb *>(detail.constScanLine(y));
            for (int x = 0; x < detail.width(); x += 8, ++samples) drawn += qAlpha(line[x]) > 0 ? 1 : 0;
        }
        const double share = samples ? double(drawn) / double(samples) : 0.0;
        auto mesh = std::make_shared<const Mesh>(basemapMesh(inner, outer, z, origin, detail, context));
        if (!alive.lock()) return;
        QMetaObject::invokeMethod(this, [this, alive, generation, mesh, error, share, outer, name, complete] {
            if (!alive.lock()) return;
            m_basemapBusy = false;
            if (generation == m_basemapGeneration && !m_basemap.isEmpty()) {
                m_basemapComplete = complete;
                Layer l;
                l.id = BasemapId;
                l.kind = BasemapId;
                l.name = name;
                l.error = error;
                l.mesh = error.isEmpty() ? mesh : nullptr;
                webtiles::TileStore *store = webtiles::TileStore::instance();
                l.info = error.isEmpty() ? QStringLiteral("На уровне земли под данными, %1 × %1 км вокруг съёмки")
                                               .arg(ru().toString(outer.width() / 1000.0, 'f', 1))
                                         : error;
                if (error.isEmpty() && share < 0.98) {
                    // The server's own short reason (HTTP 404, no connection), not a path.
                    const QString reason = store->lastError();
                    l.info += store->pending() > 0 ? QStringLiteral(" · загрузка тайлов…")
                              : reason.isEmpty() ? QString()
                              : reason.size() <= 40 ? QStringLiteral(" · часть тайлов не получена (%1)").arg(reason)
                                                    : QStringLiteral(" · часть тайлов не получена");
                }
                setOverlay(l);
            }
            if (m_basemapAgain) {
                m_basemapAgain = false;
                scheduleBasemap(false);
            }
        }, Qt::QueuedConnection);
    });
}