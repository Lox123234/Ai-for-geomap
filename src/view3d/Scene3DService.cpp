#include "Scene3DService.h"

#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QThreadPool>
#include <QtMath>
#include <cmath>

using namespace view3d;

namespace {
const QLocale &ru()
{
    static const QLocale locale(QLocale::Russian);
    return locale;
}
QString count(quint64 n) { return ru().toString(qulonglong(n)); }
QString meters(double v, int decimals = 2) { return ru().toString(v, 'f', decimals) + QStringLiteral(" м"); }
} // namespace

Scene3DService::Scene3DService(QObject *parent) : QObject(parent)
{
    m_pool.setMaxThreadCount(2);
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
        if (QDir::cleanPath(QFileInfo(l.source).absoluteFilePath()).compare(wanted, Qt::CaseInsensitive) == 0) return l.id;
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
    l.info = QStringLiteral("Загрузка…");
    l.version = 1;
    // ODM's georeferenced cloud is LAZ (not read yet); the same points are in
    // odm_filterpoints/point_cloud.ply of the job, local to coords.txt.
    QString path2 = path;
    if (kind == QLatin1String("pointcloud") && path.endsWith(QLatin1String(".laz"), Qt::CaseInsensitive)) {
        const QString coords = odmCoordsFor(path);
        if (!coords.isEmpty()) {
            QDir job = QFileInfo(coords).absoluteDir();
            job.cdUp();
            const QString ply = job.filePath(QStringLiteral("odm_filterpoints/point_cloud.ply"));
            if (QFileInfo(ply).isFile()) {
                path2 = ply;
                l.loaded = ply;
            }
        }
    }
    m_layers << l;
    const QString id = l.id;
    const int budget = m_pointBudget;
    const Origin origin = m_origin;
    std::weak_ptr<bool> alive = m_alive;
    m_pool.start([this, alive, id, kind, path = path2, drape, budget, origin] {
        std::shared_ptr<const PointCloud> cloud;
        std::shared_ptr<const Mesh> mesh;
        QString error;
        if (kind == QLatin1String("pointcloud")) {
            auto c = std::make_shared<PointCloud>(readPointCloud(path, budget));
            error = c->error;
            cloud = c;
        } else if (kind == QLatin1String("mesh")) {
            const bool ply = path.endsWith(QLatin1String(".ply"), Qt::CaseInsensitive);
            auto m = std::make_shared<Mesh>(ply ? readMeshPly(path) : readObj(path));
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
        QMetaObject::invokeMethod(this, [this, alive, id, cloud, mesh, error] {
            if (alive.lock()) loaded(id, 1, cloud, mesh, error);
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
            if (!l.loaded.isEmpty())
                l.info += QStringLiteral(". Показан %1 того же задания (сжатый LAZ пока не читается)").arg(QFileInfo(l.loaded).fileName());
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
}

void Scene3DService::removeLayer(const QString &id)
{
    for (int i = 0; i < m_layers.size(); ++i)
        if (m_layers[i].id == id) {
            m_layers.removeAt(i);
            break;
        }
    if (m_layers.isEmpty()) {
        m_origin = {};
        m_cameraFitted = false;
    }
    ++m_revision;
    emit layersChanged();
    emit changed();
}

void Scene3DService::setLayerVisible(const QString &id, bool visible)
{
    for (Layer &l : m_layers)
        if (l.id == id && l.visible != visible) {
            l.visible = visible;
            ++m_revision;
            emit layersChanged();
            emit changed();
        }
}

void Scene3DService::clear()
{
    m_layers.clear();
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
        if (!l.visible) continue;
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
    s.camera = m_camera;
    s.pointSize = m_pointSize;
    s.heightColors = m_heightColors;
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
                                                      {QStringLiteral("terrain"), QStringLiteral("Рельеф (ЦММ)")}};
        list << QVariantMap{{"id", l.id}, {"name", l.name}, {"kind", l.kind}, {"kindTitle", kinds.value(l.kind)},
                            {"visible", l.visible}, {"loading", l.loading}, {"info", l.info}, {"error", l.error},
                            {"source", QDir::toNativeSeparators(l.source)}};
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
        if (!l.visible) continue;
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
        if (l.visible) total += l.cloud ? l.cloud->positions.size() : l.mesh ? l.mesh->positions.size() : 0;
    const qsizetype step = qMax<qsizetype>(1, total / 20000);
    for (const Layer &l : m_layers) {
        if (!l.visible || (!l.cloud && !l.mesh)) continue;
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
        if (!l.visible || l.loading || !l.error.isEmpty()) continue;
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
        if (!l.visible || l.loading || !l.error.isEmpty()) continue;
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

void Scene3DService::setColorMode(const QString &mode)
{
    const bool height = mode == QLatin1String("height");
    if (height == m_heightColors) return;
    m_heightColors = height;
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
