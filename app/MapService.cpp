#include "MapService.h"
#include "Geodesy.h"
#include "GisEnvironment.h"
#include "MapTiles.h"
#include "PhotoModel.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QPdfWriter>
#include <QSaveFile>
#include <QThread>
#include <QThreadPool>
#include <QUndoCommand>
#include <QUuid>
#include <cmath>
#include <gdal.h>

namespace {
const QString PhotosId = QStringLiteral("photos");
const QString DrawingsId = QStringLiteral("drawings");
const QString StateFile = QStringLiteral("map.json");
const QString DrawingsFile = QStringLiteral("vectors/drawings.geojson");
const QColor Background(16, 21, 28);
const QColor DrawingColor(84, 214, 255);
const QColor SketchColor(255, 214, 102);

const QLocale &ru()
{
    static const QLocale locale(QLocale::Russian);
    return locale;
}

// Measurements stored with features: numbers rounded to 3 decimals (mm, mm²).
double toMillimetres(double value)
{
    return std::round(value * 1000.0) / 1000.0;
}

// Russian names of the common CRS units; others as PROJ names them.
QString unitLabel(const Crs &crs)
{
    const QString unit = crs.unitName();
    if (unit == QLatin1String("metre") || unit == QLatin1String("meter")) return QStringLiteral("метр");
    if (unit == QLatin1String("degree")) return QStringLiteral("градус");
    if (unit == QLatin1String("US survey foot")) return QStringLiteral("фут США");
    if (unit == QLatin1String("foot")) return QStringLiteral("фут");
    return unit;
}

// Deterministic CRS for data in degrees: the UTM zone (WGS 84) of a point. Polar regions and
// the special zones of Norway/Svalbard are not handled; the user can choose another CRS.
Crs utmFor(const QPointF &lonLat, QString *note)
{
    const int zone = qBound(1, int(std::floor((lonLat.x() + 180.0) / 6.0)) + 1, 60);
    const int epsg = (lonLat.y() >= 0 ? 32600 : 32700) + zone;
    if (note) *note = QStringLiteral("UTM-зона %1%2 по положению данных").arg(zone).arg(QLatin1Char(lonLat.y() >= 0 ? 'N' : 'S'));
    return Crs::fromEpsg(epsg);
}

// Everything a frame needs, copied so that exports can paint on a worker thread.
struct Frame
{
    QVector<MapLayer> layers;
    Crs crs;
    QPointF center;
    double resolution = 1;
    QSizeF size;
    int selectedPhoto = -1;
    int selectedFeature = -1;
    QPointF toScreen(const QPointF &m) const
    {
        return QPointF((m.x() - center.x()) / resolution + size.width() / 2.0,
                       size.height() / 2.0 - (m.y() - center.y()) / resolution);
    }
    QRectF mapRect() const
    {
        const double w = size.width() * resolution, h = size.height() * resolution;
        return QRectF(center.x() - w / 2, center.y() - h / 2, w, h);
    }
};

QPainterPath pathOf(const Frame &f, const VectorFeature &feature)
{
    QPainterPath path;
    path.setFillRule(Qt::OddEvenFill);
    for (const QPolygonF &part : feature.parts) {
        if (part.isEmpty()) continue;
        QPolygonF screen;
        screen.reserve(part.size());
        for (const QPointF &p : part) screen << f.toScreen(p);
        if (feature.kind == VectorFeature::Polygon) {
            path.addPolygon(screen);
            path.closeSubpath();
        } else {
            path.addPolygon(screen);
        }
    }
    return path;
}

void paintVector(QPainter &p, const Frame &f, const VectorData &data, const QColor &color, bool handlesFor, int selected)
{
    const QRectF view = f.mapRect();
    QColor fill = color;
    fill.setAlphaF(0.22f);
    for (int i = 0; i < data.features.size(); ++i) {
        const VectorFeature &feature = data.features[i];
        if (feature.kind != VectorFeature::Point && feature.bounds.isValid() && !feature.bounds.intersects(view)) continue;
        const bool isSelected = handlesFor && i == selected;
        QPen pen(isSelected ? QColor(255, 255, 255) : color, isSelected ? 3 : 2);
        pen.setCosmetic(true);
        p.setPen(pen);
        if (feature.kind == VectorFeature::Point) {
            p.setBrush(color);
            for (const QPolygonF &part : feature.parts)
                for (const QPointF &pt : part) p.drawEllipse(f.toScreen(pt), isSelected ? 6 : 4.5, isSelected ? 6 : 4.5);
        } else {
            p.setBrush(feature.kind == VectorFeature::Polygon ? QBrush(fill) : Qt::NoBrush);
            p.drawPath(pathOf(f, feature));
        }
        if (isSelected) {
            p.setBrush(QColor(255, 255, 255));
            p.setPen(QPen(QColor(20, 30, 40), 1));
            for (const QPolygonF &part : feature.parts)
                for (const QPointF &pt : part) {
                    const QPointF s = f.toScreen(pt);
                    p.drawRect(QRectF(s.x() - 4, s.y() - 4, 8, 8));
                }
        }
    }
}

using MissingTiles = QVector<QPair<TileKey, std::shared_ptr<RasterSource>>>;

void paintRasterTiles(QPainter &p, const Frame &f, const MapLayer &l, TileCache *tiles, QSet<TileKey> &wanted,
                      MissingTiles &missing)
{
    const RasterSource &r = *l.raster;
    const double *gt = r.gridTransform();
    const double gridRes = std::fabs(gt[1]);
    int level = gridRes > 0 ? int(std::floor(std::log2(f.resolution / gridRes))) : 0;
    level = qBound(0, level, r.maxLevel());
    const qint64 span = qint64(RasterSource::TileSize) << level;
    const QRectF view = f.mapRect();
    const double c0 = (view.left() - gt[0]) / gt[1], c1 = (view.right() - gt[0]) / gt[1];
    const double r0 = (view.bottom() - gt[3]) / gt[5], r1 = (view.top() - gt[3]) / gt[5];
    const int tx0 = qMax(0, int(std::floor(c0 / span))), tx1 = qMin(int((r.gridWidth() - 1) / span), int(std::floor(c1 / span)));
    const int ty0 = qMax(0, int(std::floor(r0 / span))), ty1 = qMin(int((r.gridHeight() - 1) / span), int(std::floor(r1 / span)));
    if (tx1 - tx0 > 64 || ty1 - ty0 > 64) return;   // view far larger than the raster at this level
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    auto screenRect = [&](qint64 x0, qint64 y0, qint64 x1, qint64 y1) {
        const QPointF a = f.toScreen(QPointF(gt[0] + x0 * gt[1], gt[3] + y0 * gt[5]));
        const QPointF b = f.toScreen(QPointF(gt[0] + x1 * gt[1], gt[3] + y1 * gt[5]));
        return QRectF(a, b).normalized();
    };
    for (int ty = ty0; ty <= ty1; ++ty) {
        for (int tx = tx0; tx <= tx1; ++tx) {
            const qint64 x0 = tx * span, y0 = ty * span;
            const qint64 x1 = qMin<qint64>(x0 + span, r.gridWidth()), y1 = qMin<qint64>(y0 + span, r.gridHeight());
            const QRectF target = screenRect(x0, y0, x1, y1);
            const TileKey key{quintptr(l.raster.get()), l.version, level, tx, ty};
            wanted.insert(key);
            const QImage image = tiles->tile(key);
            if (!image.isNull()) {
                if (image.width() > 1) p.drawImage(target, image);
                continue;
            }
            missing << qMakePair(key, l.raster);
            // Until the tile arrives, show the matching part of a coarser cached tile.
            for (int up = 1; up <= 5 && level + up <= r.maxLevel(); ++up) {
                const int pl = level + up;
                const qint64 pspan = qint64(RasterSource::TileSize) << pl;
                const int ptx = int(x0 / pspan), pty = int(y0 / pspan);
                const QImage parent = tiles->tile(TileKey{quintptr(l.raster.get()), l.version, pl, ptx, pty});
                if (parent.isNull() || parent.width() <= 1) continue;
                const double scale = double(qint64(1) << pl);
                const QRectF src((x0 - ptx * pspan) / scale, (y0 - pty * pspan) / scale, (x1 - x0) / scale, (y1 - y0) / scale);
                p.drawImage(target, parent, src);
                break;
            }
        }
    }
}

// Paints every visible layer bottom to top. With `tiles` the rasters come from the tile
// cache (interactive); without, each raster is read for the whole view (export).
void paintFrame(QPainter &p, const Frame &f, TileCache *tiles, QSet<TileKey> *wanted, MissingTiles *missing)
{
    p.fillRect(QRectF(QPointF(0, 0), f.size), Background);
    if (!f.crs.isValid()) return;
    p.setRenderHint(QPainter::Antialiasing, true);
    for (const MapLayer &l : f.layers) {
        if (!l.visible || l.opacity <= 0) continue;
        p.save();
        p.setOpacity(l.opacity);
        if (l.type == MapLayer::Raster && l.raster && l.raster->isValid()) {
            if (tiles) {
                paintRasterTiles(p, f, l, tiles, *wanted, *missing);
            } else {
                p.drawImage(QRectF(QPointF(0, 0), f.size), l.raster->renderRegion(f.mapRect(), f.size.toSize()));
            }
        } else if (l.type == MapLayer::Vector && l.vector.valid) {
            paintVector(p, f, l.vector, l.color, false, -1);
        } else if (l.type == MapLayer::Photos && l.vector.valid) {
            paintVector(p, f, l.vector, l.color, false, -1);
            if (f.selectedPhoto >= 0) {
                for (const VectorFeature &feature : l.vector.features) {
                    if (feature.id != f.selectedPhoto) continue;
                    p.setPen(QPen(QColor(20, 20, 20), 2));
                    p.setBrush(QColor(255, 236, 140));
                    p.drawEllipse(f.toScreen(feature.parts.first().first()), 8, 8);
                }
            }
        } else if (l.type == MapLayer::Drawings && l.vector.valid) {
            paintVector(p, f, l.vector, l.color, true, f.selectedFeature);
        }
        p.restore();
    }
}

double shoelace(const QVector<QPointF> &ring)
{
    double a = 0;
    for (int i = 0; i < ring.size(); ++i) {
        const QPointF &p = ring[i], &q = ring[(i + 1) % ring.size()];
        a += p.x() * q.y() - q.x() * p.y();
    }
    return std::fabs(a) / 2;
}

double planarLength(const QVector<QPointF> &line, bool closed)
{
    double s = 0;
    for (int i = 1; i < line.size(); ++i) s += std::hypot(line[i].x() - line[i - 1].x(), line[i].y() - line[i - 1].y());
    if (closed && line.size() > 2) s += std::hypot(line.first().x() - line.last().x(), line.first().y() - line.last().y());
    return s;
}

QString number(double v, int decimals) { return ru().toString(v, 'f', decimals); }
} // namespace

// ------------------------------------------------------------- undo commands

class DrawingCommand final : public QUndoCommand
{
public:
    enum Kind { Add, Remove, MoveVertex };
    DrawingCommand(MapService *service, Kind kind, const QString &text) : QUndoCommand(text), m_service(service), m_kind(kind) {}
    VectorFeature feature;   // Add/Remove
    int index = -1;          // Remove: position
    qint64 featureId = -1;   // MoveVertex
    int part = 0, vertex = 0;
    QPointF from, to;
    void redo() override { apply(true); }
    void undo() override { apply(false); }

private:
    void apply(bool forward)
    {
        VectorData *d = m_service->drawings();
        if (!d) return;
        if (m_kind == MoveVertex) {
            for (VectorFeature &f : d->features) {
                if (f.id != featureId || part >= f.parts.size() || vertex >= f.parts[part].size()) continue;
                f.parts[part][vertex] = forward ? to : from;
                f.bounds = QRectF();
                for (const QPolygonF &pp : f.parts) f.bounds = f.bounds.isNull() ? pp.boundingRect() : f.bounds.united(pp.boundingRect());
            }
        } else if ((m_kind == Add) == forward) {
            d->features.insert(m_kind == Add ? d->features.size() : qBound(0, index, int(d->features.size())), feature);
        } else {
            for (int i = 0; i < d->features.size(); ++i) {
                if (d->features[i].id != feature.id) continue;
                d->features.removeAt(i);
                break;
            }
            m_service->m_selectedFeature = -1;
        }
        m_service->applyDrawingChange();
    }
    MapService *m_service;
    Kind m_kind;
};

// ------------------------------------------------------------ LayerListModel

LayerListModel::LayerListModel(MapService *service) : QAbstractListModel(service), m_service(service) {}

int LayerListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_service->layerCount();
}

QVariant LayerListModel::data(const QModelIndex &index, int role) const
{
    const int n = m_service->layerCount();
    if (!index.isValid() || index.row() >= n) return {};
    const MapLayer &l = m_service->layers()[n - 1 - index.row()];   // top first
    static const char *types[] = {"raster", "vector", "photos", "drawings"};
    switch (role) {
    case IdRole: return l.id;
    case NameRole: return l.name;
    case TypeRole: return QString::fromLatin1(types[l.type]);
    case VisibleRole: return l.visible;
    case OpacityRole: return l.opacity;
    case StatusRole: return l.status;
    case CrsRole: {
        const Crs c = l.type == MapLayer::Raster ? (l.assignedCrs.isValid() ? l.assignedCrs : l.rasterInfo.crs) : l.vector.sourceCrs;
        return c.isValid() ? c.label() : QStringLiteral("CRS не задан");
    }
    case SummaryRole:
        if (l.type == MapLayer::Raster) return l.rasterInfo.valid ? l.rasterInfo.summary() : QString();
        return l.vector.valid ? l.vector.summary() : QString();
    case NeedsCrsRole: return l.needsCrs;
    case LoadingRole: return l.loading;
    case ColorRole: return l.color;
    case LegendRole: {
        QVariantList stops;
        if (l.type == MapLayer::Raster && l.style.mode == RasterStyle::SingleBand)
            for (const auto &s : RasterStyle::rampStops(l.style.ramp)) stops << QVariantMap{{"position", s.first}, {"color", s.second}};
        return stops;
    }
    case StyleRole: return l.style.toVariant();
    case BandsRole: return l.rasterInfo.bands;
    default: return {};
    }
}

QHash<int, QByteArray> LayerListModel::roleNames() const
{
    return {{IdRole, "layerId"}, {NameRole, "name"}, {TypeRole, "layerType"}, {VisibleRole, "layerVisible"},
            {OpacityRole, "layerOpacity"}, {StatusRole, "status"}, {CrsRole, "crs"}, {SummaryRole, "summary"},
            {NeedsCrsRole, "needsCrs"}, {LoadingRole, "loading"}, {ColorRole, "layerColor"}, {LegendRole, "legend"},
            {StyleRole, "rasterStyle"}, {BandsRole, "bands"}};
}

void LayerListModel::reset()
{
    beginResetModel();
    endResetModel();
}

void LayerListModel::layerChanged(int layerIndex)
{
    const int row = m_service->layerCount() - 1 - layerIndex;
    if (row >= 0) emit dataChanged(index(row), index(row));
}

// -------------------------------------------------------------- MapService

MapService::MapService(QObject *parent)
    : QObject(parent), m_model(new LayerListModel(this)), m_tiles(new TileCache(this))
{
    connect(m_tiles, &TileCache::tileReady, this, &MapService::repaint);
    connect(&m_undo, &QUndoStack::indexChanged, this, &MapService::undoChanged);
    connect(&m_undo, &QUndoStack::cleanChanged, this, &MapService::undoChanged);
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(500);
    connect(&m_saveTimer, &QTimer::timeout, this, [this] {
        saveState();
        saveDrawings();
    });
}

MapService::~MapService()
{
    if (m_saveTimer.isActive()) {
        saveState();
        saveDrawings();
    }
    for (MapLayer &l : m_layers)
        if (l.cancel) *l.cancel = true;
    QThreadPool::globalInstance()->waitForDone();
}

QString MapService::gisVersion() const
{
    return QStringLiteral("GDAL %1 · PROJ %2").arg(GisEnvironment::gdalVersion(), GisEnvironment::projVersion());
}

QStringList MapService::problems() const
{
    return GisEnvironment::problems();
}

int MapService::indexOf(const QString &id) const
{
    for (int i = 0; i < m_layers.size(); ++i) if (m_layers[i].id == id) return i;
    return -1;
}

MapLayer *MapService::layer(const QString &id)
{
    const int i = indexOf(id);
    return i >= 0 ? &m_layers[i] : nullptr;
}

int MapService::drawingsIndex() const
{
    return indexOf(DrawingsId);
}

VectorData *MapService::drawings()
{
    const int i = drawingsIndex();
    return i >= 0 ? &m_layers[i].vector : nullptr;
}

int MapService::drawingCount() const
{
    const int i = drawingsIndex();
    return i >= 0 ? int(m_layers[i].vector.features.size()) : 0;
}

QString MapService::cacheDirectory() const
{
    return QDir(m_project).filePath(QStringLiteral("cache/maps"));
}

QString MapService::relativeSource(const QString &path) const
{
    const QString rel = QDir(m_project).relativeFilePath(path);
    return rel.startsWith(QLatin1String("..")) || QDir::isAbsolutePath(rel) ? QDir::cleanPath(path) : rel;
}

QString MapService::resolveSource(const QString &path) const
{
    return QDir::isAbsolutePath(path) ? path : QDir(m_project).filePath(path);
}

QString MapService::displayPath(const QUrl &url) const
{
    return QDir::toNativeSeparators(url.toLocalFile());
}

// ---------------------------------------------------------- project switching

void MapService::setProject(const QString &directory, const PhotoModel *photos)
{
    if (!m_project.isEmpty()) {
        m_saveTimer.stop();
        saveState();
        saveDrawings();
    }
    for (MapLayer &l : m_layers)
        if (l.cancel) *l.cancel = true;
    m_layers.clear();
    m_tiles->clear();
    m_undo.clear();
    m_sketch.clear();
    m_selectedFeature = -1;
    m_selectedPhoto = -1;
    m_selectedLayer.clear();
    m_crs = {};
    m_crsNote.clear();
    m_measurementText.clear();
    m_measurementDetails.clear();
    m_project = directory.isEmpty() ? QString() : QDir(directory).absolutePath();
    m_photos = photos;
    if (!m_project.isEmpty()) loadState();
    m_model->reset();
    updateScale();
    emit projectChanged();
    emit crsChanged();
    emit layersChanged();
    emit drawingsChanged();
    emit measurementChanged();
    emit selectionChanged();
    emit repaint();
}

void MapService::loadState()
{
    m_loadingState = true;
    QFile file(QDir(m_project).filePath(StateFile));
    QJsonObject state;
    if (file.open(QIODevice::ReadOnly)) state = QJsonDocument::fromJson(file.readAll()).object();
    if (state.value("schemaVersion").toInt() == 1) {
        const QString wkt = state.value("crs").toString();
        if (!wkt.isEmpty()) m_crs = Crs::fromWkt(wkt);
        m_crsNote = state.value("crsNote").toString();
        const QJsonObject view = state.value("view").toObject();
        m_center = QPointF(view.value("x").toDouble(), view.value("y").toDouble());
        m_resolution = view.value("resolution").toDouble(1.0);
        if (m_resolution <= 0) m_resolution = 1.0;
        for (const QJsonValue &v : state.value("layers").toArray()) {
            const QJsonObject o = v.toObject();
            MapLayer l;
            l.id = o.value("id").toString();
            const QString type = o.value("type").toString();
            l.type = type == QLatin1String("vector") ? MapLayer::Vector : type == QLatin1String("photos") ? MapLayer::Photos
                   : type == QLatin1String("drawings") ? MapLayer::Drawings : MapLayer::Raster;
            l.name = o.value("name").toString();
            l.source = o.value("source").toString().isEmpty() ? QString() : resolveSource(o.value("source").toString());
            l.vectorLayer = o.value("layer").toString();
            l.visible = o.value("visible").toBool(true);
            l.opacity = qBound(0.0, o.value("opacity").toDouble(1.0), 1.0);
            if (o.contains("color")) l.color = QColor(o.value("color").toString());
            if (!o.value("crs").toString().isEmpty()) l.assignedCrs = Crs::fromWkt(o.value("crs").toString());
            if (o.contains("style")) l.style = RasterStyle::fromVariant(o.value("style").toObject().toVariantMap(), l.style);
            l.status = o.contains("style") ? QStringLiteral("style") : QString();   // marker: keep the saved style
            if (l.id.isEmpty()) continue;
            m_layers << l;
        }
    }
    if (indexOf(PhotosId) < 0) {
        MapLayer photos;
        photos.id = PhotosId;
        photos.type = MapLayer::Photos;
        photos.color = QColor(255, 159, 67);
        m_layers << photos;
    }
    if (indexOf(DrawingsId) < 0) {
        MapLayer d;
        d.id = DrawingsId;
        d.type = MapLayer::Drawings;
        d.color = DrawingColor;
        m_layers << d;
    }
    for (MapLayer &l : m_layers) {
        if (l.type == MapLayer::Photos) l.name = QStringLiteral("Точки съёмки");
        if (l.type == MapLayer::Drawings) l.name = QStringLiteral("Рисунки и измерения");
    }
    // Without a saved CRS the photos decide (UTM zone of their GPS positions).
    rebuildPhotos();
    for (int i = 0; i < m_layers.size(); ++i) {
        if (m_layers[i].type == MapLayer::Raster) prepareRaster(i);
        else if (m_layers[i].type == MapLayer::Vector) prepareVector(i);
    }
    loadDrawings();
    if (!state.contains("view")) zoomToAll();
    m_loadingState = false;
}

void MapService::scheduleSave()
{
    if (!m_project.isEmpty() && !m_loadingState) m_saveTimer.start();
}

void MapService::saveState() const
{
    if (m_project.isEmpty()) return;
    QJsonArray layers;
    for (const MapLayer &l : m_layers) {
        static const char *types[] = {"raster", "vector", "photos", "drawings"};
        QJsonObject o{{"id", l.id}, {"type", QString::fromLatin1(types[l.type])}, {"name", l.name},
                      {"visible", l.visible}, {"opacity", l.opacity}, {"color", l.color.name()}};
        if (!l.source.isEmpty()) o.insert("source", relativeSource(l.source));
        if (!l.vectorLayer.isEmpty()) o.insert("layer", l.vectorLayer);
        if (l.assignedCrs.isValid()) o.insert("crs", l.assignedCrs.wkt());
        if (l.type == MapLayer::Raster && l.rasterInfo.valid) o.insert("style", QJsonObject::fromVariantMap(l.style.toVariant()));
        layers.append(o);
    }
    const QJsonObject state{{"schemaVersion", 1}, {"crs", m_crs.isValid() ? m_crs.wkt() : QString()},
                            {"crsEpsg", m_crs.epsg()}, {"crsNote", m_crsNote},
                            {"view", QJsonObject{{"x", m_center.x()}, {"y", m_center.y()}, {"resolution", m_resolution}}},
                            {"layers", layers}};
    QSaveFile file(QDir(m_project).filePath(StateFile));
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(state).toJson(QJsonDocument::Indented));
        file.commit();
    }
}

void MapService::saveDrawings() const
{
    if (m_project.isEmpty() || !m_crs.isValid()) return;
    const int i = drawingsIndex();
    if (i < 0) return;
    const QString path = QDir(m_project).filePath(DrawingsFile);
    QVector<VectorFeature> features = m_layers[i].vector.features;
    if (features.isEmpty() && !QFileInfo::exists(path)) return;
    // Measurements go with the features into the file (and its exports).
    for (VectorFeature &f : features) {
        if (f.parts.isEmpty()) continue;
        const QVector<QPointF> pts(f.parts.first().begin(), f.parts.first().end());
        if (f.kind == VectorFeature::Line) f.attributes.insert(QStringLiteral("length_m"), toMillimetres(measure(pts, false).length));
        if (f.kind == VectorFeature::Polygon) {
            const Measure m = measure(pts, true);
            f.attributes.insert(QStringLiteral("area_m2"), toMillimetres(m.area));
            f.attributes.insert(QStringLiteral("perimeter_m"), toMillimetres(m.perimeter));
        }
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    QString error;
    writeVectorFile(path, QStringLiteral("drawings"), features, m_crs, &error);
}

void MapService::loadDrawings()
{
    const int i = drawingsIndex();
    if (i < 0) return;
    MapLayer &l = m_layers[i];
    l.vector = {};
    l.vector.valid = m_crs.isValid();
    l.vector.sourceCrs = Crs::wgs84();
    const QString path = QDir(m_project).filePath(DrawingsFile);
    if (!m_crs.isValid() || !QFileInfo::exists(path)) return;
    VectorData data = VectorData::load(path, {}, {}, m_crs);
    if (!data.valid) {
        l.status = data.error;
        return;
    }
    for (VectorFeature &f : data.features) {
        f.id = m_nextFeatureId++;
        f.attributes.remove(QStringLiteral("length_m"));
        f.attributes.remove(QStringLiteral("area_m2"));
        f.attributes.remove(QStringLiteral("perimeter_m"));
    }
    data.crsNote.clear();   // the project's own file: WGS 84 is how it is stored, not a guess
    l.vector = data;
}

// ------------------------------------------------------------------- CRS

void MapService::setCrs(const Crs &crs, const QString &note)
{
    m_crs = crs;
    m_crsNote = note;
    emit crsChanged();
}

void MapService::chooseCrsFor(const MapLayer &l)
{
    if (m_crs.isValid()) return;
    Crs candidate;
    QRectF extent;
    if (l.type == MapLayer::Raster) {
        candidate = l.assignedCrs.isValid() ? l.assignedCrs : l.rasterInfo.crs;
        extent = l.rasterInfo.extent;
    } else {
        QString error;
        VectorData::probe(l.source, l.vectorLayer, &candidate, &extent, &error);
        if (l.assignedCrs.isValid()) candidate = l.assignedCrs;
    }
    if (!candidate.isValid()) return;
    if (candidate.isProjected()) {
        setCrs(candidate, QStringLiteral("по слою «%1»").arg(l.name));
        return;
    }
    QPointF c = extent.center();
    CoordinateTransform toWgs(candidate, Crs::wgs84());
    toWgs.transform(c);
    QString note;
    const Crs utm = utmFor(c, &note);
    setCrs(utm, note + QStringLiteral(" (слой «%1» в градусах)").arg(l.name));
}

QString MapService::describeCrs(const QString &text) const
{
    QString error;
    const Crs crs = Crs::fromUserInput(text, &error);
    if (!crs.isValid()) return QStringLiteral("✕ ") + error;
    QString kind = crs.isProjected() ? QStringLiteral("проекция, единицы: %1").arg(unitLabel(crs))
                                     : QStringLiteral("географическая, единицы: %1").arg(unitLabel(crs));
    return crs.label() + QStringLiteral(" — ") + kind;
}

QString MapService::setMapCrs(const QString &text)
{
    QString error;
    const Crs crs = Crs::fromUserInput(text, &error);
    if (!crs.isValid()) return error;
    QPointF center = m_center;
    double resolutionMeters = m_resolution * (m_crs.isProjected() ? m_crs.metersPerUnit() : 111320.0);
    if (m_crs.isValid()) {
        CoordinateTransform t(m_crs, crs);
        t.transform(center);
    }
    setCrs(crs, QStringLiteral("пользователем"));   // shown as "Выбрана пользователем"
    m_center = center;
    m_resolution = crs.isProjected() ? resolutionMeters / qMax(1e-9, crs.metersPerUnit()) : resolutionMeters / 111320.0;
    reprojectAll();
    scheduleSave();
    return {};
}

void MapService::reprojectAll()
{
    m_tiles->clear();
    // Drawings are kept through WGS 84 so that they survive any change of the map CRS.
    const int d = drawingsIndex();
    QVector<VectorFeature> drawn = d >= 0 ? m_layers[d].vector.features : QVector<VectorFeature>();
    rebuildPhotos();
    for (int i = 0; i < m_layers.size(); ++i) {
        if (m_layers[i].type == MapLayer::Raster) prepareRaster(i);
        else if (m_layers[i].type == MapLayer::Vector) prepareVector(i);
    }
    loadDrawings();
    m_undo.clear();
    m_model->reset();
    updateScale();
    emit layersChanged();
    emit repaint();
    Q_UNUSED(drawn)
}

QString MapService::assignLayerCrs(const QString &id, const QString &text)
{
    MapLayer *l = layer(id);
    if (!l) return QStringLiteral("Слой не найден.");
    QString error;
    const Crs crs = Crs::fromUserInput(text, &error);
    if (!crs.isValid()) return error;
    l->assignedCrs = crs;
    l->needsCrs = false;
    l->status.clear();
    const int i = indexOf(id);
    if (l->type == MapLayer::Raster) prepareRaster(i);
    else prepareVector(i);
    scheduleSave();
    return {};
}

// --------------------------------------------------------------- layers

QString MapService::addLayerFromUrl(const QUrl &url)
{
    return addLayerFromPath(url.toLocalFile());
}

QString MapService::addLayerFromPath(const QString &path, bool zoom)
{
    if (m_project.isEmpty()) return QStringLiteral("Откройте проект.");
    const QFileInfo fi(path);
    if (!fi.isFile()) return QStringLiteral("Файл не найден: %1").arg(QDir::toNativeSeparators(path));
    for (const MapLayer &l : m_layers)
        if (!l.source.isEmpty() && QFileInfo(l.source) == fi) return QStringLiteral("Этот файл уже на карте.");
    // Raster first (GeoTIFF/COG/JPEG with world file), then vector layers.
    GisEnvironment::clearError();
    GDALDatasetH probe = GDALOpenEx(QDir::toNativeSeparators(path).toUtf8().constData(),
                                    GDAL_OF_RASTER | GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
    if (!probe) return QStringLiteral("Формат не распознан: %1").arg(GisEnvironment::lastError());
    const bool raster = GDALGetRasterCount(probe) > 0;
    GDALClose(probe);
    const int insertAt = qMax(0, indexOf(PhotosId));   // below photos and drawings
    QStringList added;
    if (raster) {
        MapLayer l;
        l.id = QUuid::createUuid().toString(QUuid::Id128).left(12);
        l.type = MapLayer::Raster;
        l.name = fi.completeBaseName();
        l.source = fi.absoluteFilePath();
        m_layers.insert(insertAt, l);
        prepareRaster(insertAt);
        added << l.id;
    } else {
        QString error;
        const QStringList names = VectorData::layerNames(path, &error);
        if (names.isEmpty()) return error;
        static const QColor palette[] = {QColor(255, 196, 61), QColor(126, 211, 33), QColor(244, 114, 182), QColor(167, 139, 250)};
        int n = 0;
        for (const QString &name : names) {
            MapLayer l;
            l.id = QUuid::createUuid().toString(QUuid::Id128).left(12);
            l.type = MapLayer::Vector;
            l.name = names.size() > 1 ? fi.completeBaseName() + QStringLiteral(" · ") + name : fi.completeBaseName();
            l.source = fi.absoluteFilePath();
            l.vectorLayer = name;
            l.color = palette[(indexOf(PhotosId) + n++) % 4];
            m_layers.insert(insertAt, l);
            prepareVector(insertAt);
            added << l.id;
        }
    }
    m_model->reset();
    m_selectedLayer = added.last();
    emit layersChanged();
    emit selectionChanged();
    if (zoom) zoomToLayer(added.first());
    scheduleSave();
    emit repaint();
    return {};
}

void MapService::prepareRaster(int index)
{
    MapLayer &l = m_layers[index];
    if (l.cancel) *l.cancel = true;
    l.cancel = std::make_shared<std::atomic_bool>(false);
    const bool keepStyle = l.status == QLatin1String("style");
    l.raster.reset();
    l.needsCrs = false;
    l.status.clear();
    l.rasterInfo = RasterInfo::read(l.source);
    if (!l.rasterInfo.valid) {
        l.status = l.rasterInfo.error;
        return;
    }
    if (!l.rasterInfo.hasGeoTransform) {
        l.status = QStringLiteral("Нет геопривязки: растр нельзя поместить на карту.");
        return;
    }
    const Crs layerCrs = l.assignedCrs.isValid() ? l.assignedCrs : l.rasterInfo.crs;
    if (!layerCrs.isValid()) {
        if (!keepStyle) l.style = RasterStyle::automatic(l.rasterInfo, {});   // what the properties panel shows
        l.needsCrs = true;
        l.status = QStringLiteral("Система координат не задана. Укажите CRS — без неё слой не показывается.");
        return;
    }
    chooseCrsFor(l);
    if (!m_crs.isValid()) return;
    auto source = std::make_shared<RasterSource>(l.source, l.assignedCrs, m_crs);
    if (!source->isValid()) {
        l.status = source->error();
        return;
    }
    l.raster = source;
    // Style: saved, or automatic from band statistics (computed in the background for non-byte data).
    const bool needsStats = !keepStyle && !(l.rasterInfo.dataType == QLatin1String("Byte") && l.rasterInfo.bands >= 3);
    if (!needsStats) {
        if (!keepStyle) l.style = RasterStyle::automatic(l.rasterInfo, {});
        source->setStyle(l.style);
    }
    const bool pyramids = PyramidBuilder::needed(l.rasterInfo);
    const QString vrt = PyramidBuilder::cachePathFor(l.source, cacheDirectory());
    if (pyramids && PyramidBuilder::isComplete(vrt)) source->setPyramidPath(vrt);
    if (!needsStats && !(pyramids && !PyramidBuilder::isComplete(vrt))) return;

    l.loading = true;
    l.status = needsStats ? QStringLiteral("Расчёт статистики…") : QStringLiteral("Построение пирамид…");
    const QString id = l.id, path = l.source;
    const RasterInfo info = l.rasterInfo;
    auto cancel = l.cancel;
    const bool buildPyramids = pyramids && !PyramidBuilder::isComplete(vrt);
    QThreadPool::globalInstance()->start([this, id, path, info, cancel, needsStats, buildPyramids, vrt, source] {
        QVector<QPair<double, double>> ranges;
        if (needsStats) {
            GDALDatasetH ds = GDALOpenEx(QDir::toNativeSeparators(path).toUtf8().constData(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                                         nullptr, nullptr, nullptr);
            for (int b = 1; ds && b <= qMin(3, info.bands) && !*cancel; ++b) {
                double mm[2] = {0, 255};
                GDALComputeRasterMinMax(GDALGetRasterBand(ds, b), TRUE, mm);   // approximate: overviews/sampling
                ranges << qMakePair(mm[0], mm[1]);
            }
            if (ds) GDALClose(ds);
            QMetaObject::invokeMethod(this, [this, id, info, ranges, cancel] {
                MapLayer *l = layer(id);
                if (!l || *cancel || !l->raster) return;
                l->style = RasterStyle::automatic(info, ranges);
                l->raster->setStyle(l->style);
                ++l->version;
                changed(indexOf(id));
                scheduleSave();
            }, Qt::QueuedConnection);
        }
        if (buildPyramids && !*cancel) {
            QString error;
            QElapsedTimer last;
            last.start();
            const bool ok = PyramidBuilder::build(path, vrt, [this, id, cancel, &last](double done) {
                if (last.elapsed() > 500) {
                    last.restart();
                    QMetaObject::invokeMethod(this, [this, id, done] {
                        if (MapLayer *l = layer(id)) {
                            l->status = QStringLiteral("Построение пирамид: %1 %").arg(int(done * 100));
                            changed(indexOf(id));
                        }
                    }, Qt::QueuedConnection);
                }
                return !*cancel;
            }, &error);
            QMetaObject::invokeMethod(this, [this, id, ok, error, vrt, cancel] {
                MapLayer *l = layer(id);
                if (!l || *cancel || !l->raster) return;
                if (ok) {
                    l->raster->setPyramidPath(vrt);
                    ++l->version;
                    l->status.clear();
                } else {
                    l->status = error;
                }
                l->loading = false;
                changed(indexOf(id));
            }, Qt::QueuedConnection);
        } else {
            QMetaObject::invokeMethod(this, [this, id, cancel] {
                MapLayer *l = layer(id);
                if (!l || *cancel) return;
                l->loading = false;
                if (l->status.startsWith(QStringLiteral("Расчёт"))) l->status.clear();
                changed(indexOf(id));
            }, Qt::QueuedConnection);
        }
    });
}

void MapService::prepareVector(int index)
{
    MapLayer &l = m_layers[index];
    if (l.cancel) *l.cancel = true;
    l.cancel = std::make_shared<std::atomic_bool>(false);
    l.needsCrs = false;
    l.status.clear();
    l.vector = {};
    Crs crs;
    QRectF extent;
    QString error;
    if (!VectorData::probe(l.source, l.vectorLayer, &crs, &extent, &error)) {
        l.status = error;
        return;
    }
    if (!crs.isValid() && !l.assignedCrs.isValid()) {
        l.needsCrs = true;
        l.status = QStringLiteral("Система координат не задана. Укажите CRS — без неё слой не показывается.");
        return;
    }
    chooseCrsFor(l);
    l.loading = true;
    l.status = QStringLiteral("Загрузка объектов…");
    const QString id = l.id, path = l.source, name = l.vectorLayer;
    const Crs assigned = l.assignedCrs, mapCrs = m_crs;
    auto cancel = l.cancel;
    QThreadPool::globalInstance()->start([this, id, path, name, assigned, mapCrs, cancel] {
        VectorData data = VectorData::load(path, name, assigned, mapCrs);
        QMetaObject::invokeMethod(this, [this, id, data, cancel] {
            MapLayer *l = layer(id);
            if (!l || *cancel) return;
            l->vector = data;
            l->loading = false;
            l->status = data.valid ? QString() : data.error;
            changed(indexOf(id));
        }, Qt::QueuedConnection);
    });
}

void MapService::photosChanged()
{
    rebuildPhotos();
    const int i = indexOf(PhotosId);
    if (i >= 0) changed(i);
}

void MapService::rebuildPhotos()
{
    const int i = indexOf(PhotosId);
    if (i < 0) return;
    MapLayer &l = m_layers[i];
    l.vector = {};
    l.status.clear();
    QVector<VectorFeature> features;
    QVector<QPointF> lonLat;
    if (m_photos) {
        const QVector<Photo> &photos = m_photos->photos();
        for (int k = 0; k < photos.size(); ++k) {
            if (!photos[k].hasGps) continue;
            VectorFeature f;
            f.id = k;
            f.kind = VectorFeature::Point;
            f.attributes.insert(QStringLiteral("name"), photos[k].name);
            lonLat << QPointF(photos[k].longitude, photos[k].latitude);
            features << f;
        }
    }
    if (features.isEmpty()) {
        l.status = QStringLiteral("У снимков проекта нет GPS-координат.");
        return;
    }
    if (!m_crs.isValid()) {
        QPointF c(0, 0);
        for (const QPointF &p : lonLat) c += p;
        c /= double(lonLat.size());
        QString note;
        const Crs utm = utmFor(c, &note);
        setCrs(utm, note + QStringLiteral(" (точки съёмки)"));
    }
    CoordinateTransform toMap(Crs::wgs84(), m_crs);
    toMap.transform(lonLat);
    for (int k = 0; k < features.size(); ++k) {
        features[k].parts << QPolygonF({lonLat[k]});
        l.vector.extent = k == 0 ? QRectF(lonLat[k], QSizeF(0, 0)) : l.vector.extent.united(QRectF(lonLat[k], QSizeF(0, 0)));
    }
    l.vector.features = features;
    l.vector.points = int(features.size());
    l.vector.sourceCrs = Crs::wgs84();
    l.vector.crsNote = QStringLiteral("GPS из EXIF снимков");
    l.vector.valid = true;
}

void MapService::setSelectedPhoto(int index)
{
    m_selectedPhoto = index;
    emit repaint();
}

void MapService::changed(int index)
{
    if (index < 0) return;
    m_model->layerChanged(index);
    emit layersChanged();
    emit repaint();
}

void MapService::removeLayer(const QString &id)
{
    const int i = indexOf(id);
    if (i < 0 || id == PhotosId || id == DrawingsId) return;
    if (m_layers[i].cancel) *m_layers[i].cancel = true;
    m_layers.removeAt(i);
    m_model->reset();
    if (m_selectedLayer == id) m_selectedLayer.clear();
    emit layersChanged();
    emit selectionChanged();
    scheduleSave();
    emit repaint();
}

void MapService::moveLayer(const QString &id, int delta)
{
    const int i = indexOf(id);
    const int j = i + delta;
    if (i < 0 || j < 0 || j >= m_layers.size()) return;
    m_layers.move(i, j);
    m_model->reset();
    emit layersChanged();
    scheduleSave();
    emit repaint();
}

void MapService::setLayerVisible(const QString &id, bool visible)
{
    if (MapLayer *l = layer(id); l && l->visible != visible) {
        l->visible = visible;
        changed(indexOf(id));
        scheduleSave();
    }
}

void MapService::setLayerOpacity(const QString &id, double opacity)
{
    if (MapLayer *l = layer(id)) {
        l->opacity = qBound(0.0, opacity, 1.0);
        changed(indexOf(id));
        scheduleSave();
    }
}

void MapService::setLayerColor(const QString &id, const QColor &color)
{
    if (MapLayer *l = layer(id)) {
        l->color = color;
        changed(indexOf(id));
        scheduleSave();
    }
}

void MapService::setLayerStyle(const QString &id, const QVariantMap &style)
{
    MapLayer *l = layer(id);
    if (!l || l->type != MapLayer::Raster || !l->raster) return;
    RasterStyle s = RasterStyle::fromVariant(style, l->style);
    const int bands = l->rasterInfo.bands;
    auto valid = [bands](int b) { return b >= 1 && b <= bands; };
    if (!valid(s.red) || !valid(s.green) || !valid(s.blue) || !valid(s.band)) return;
    l->style = s;
    l->raster->setStyle(s);
    ++l->version;
    changed(indexOf(id));
    scheduleSave();
}

QVariantMap MapService::layerDetails(const QString &id) const
{
    const int i = indexOf(id);
    if (i < 0) return {};
    const MapLayer &l = m_layers[i];
    QVariantMap d{{"id", l.id}, {"name", l.name}, {"source", QDir::toNativeSeparators(l.source)},
                  {"status", l.status}, {"needsCrs", l.needsCrs}, {"mapCrs", m_crs.label()},
                  {"opacity", l.opacity}, {"visible", l.visible}, {"removable", l.type == MapLayer::Raster || l.type == MapLayer::Vector}};
    if (l.type == MapLayer::Raster) {
        d.insert("type", "raster");
        d.insert("summary", l.rasterInfo.valid ? l.rasterInfo.summary() : QString());
        d.insert("sourceCrs", l.assignedCrs.isValid() ? l.assignedCrs.label() + QStringLiteral(" (указана пользователем)")
                                                      : l.rasterInfo.crs.label());
        d.insert("style", l.style.toVariant());
        d.insert("bands", l.rasterInfo.bands);
        d.insert("dataType", l.rasterInfo.dataType);
        d.insert("reprojected", l.raster && !(l.assignedCrs.isValid() ? l.assignedCrs : l.rasterInfo.crs).isSame(m_crs));
    } else {
        d.insert("type", l.type == MapLayer::Vector ? "vector" : l.type == MapLayer::Photos ? "photos" : "drawings");
        d.insert("summary", l.vector.valid ? l.vector.summary() : QString());
        d.insert("sourceCrs", l.vector.sourceCrs.label());
        d.insert("fields", l.vector.fields);
        d.insert("color", l.color);
    }
    return d;
}

void MapService::selectLayer(const QString &id)
{
    if (m_selectedLayer == id) return;
    m_selectedLayer = id;
    emit selectionChanged();
}

// ----------------------------------------------------------------- view

void MapService::setViewSize(const QSizeF &size)
{
    if (size == m_viewSize || size.isEmpty()) return;
    m_viewSize = size;
    updateScale();
    emit repaint();
}

void MapService::setMetersPerScreenPixel(double meters)
{
    if (meters > 0) {
        m_metersPerScreenPixel = meters;
        updateScale();
    }
}

QPointF MapService::toScreen(const QPointF &m) const
{
    return QPointF((m.x() - m_center.x()) / m_resolution + m_viewSize.width() / 2.0,
                   m_viewSize.height() / 2.0 - (m.y() - m_center.y()) / m_resolution);
}

QPointF MapService::toMap(const QPointF &s) const
{
    return QPointF(m_center.x() + (s.x() - m_viewSize.width() / 2.0) * m_resolution,
                   m_center.y() - (s.y() - m_viewSize.height() / 2.0) * m_resolution);
}

void MapService::panBy(const QPointF &d)
{
    m_center -= QPointF(d.x() * m_resolution, -d.y() * m_resolution);
    updateScale();
    scheduleSave();
    emit repaint();
}

void MapService::zoomAt(const QPointF &screen, double factor)
{
    const QPointF anchor = toMap(screen);
    const double r = qBound(1e-9, m_resolution / factor, 1e9);
    m_center = anchor + (m_center - anchor) * (r / m_resolution);
    m_resolution = r;
    updateScale();
    scheduleSave();
    emit repaint();
}

void MapService::setView(const QPointF &center, double resolution)
{
    m_center = center;
    m_resolution = qMax(1e-9, resolution);
    updateScale();
    scheduleSave();
    emit repaint();
}

namespace {
// Extent of a layer in the map CRS; false when it has nothing to show.
bool layerExtent(const MapLayer &l, QRectF *extent)
{
    if (l.type == MapLayer::Raster) {
        if (!l.raster || !l.raster->isValid()) return false;
        *extent = l.raster->mapExtent();
        return true;
    }
    if (!l.vector.valid || l.vector.features.isEmpty()) return false;
    *extent = l.vector.extent;
    return true;
}
} // namespace

void MapService::zoomToLayer(const QString &id)
{
    const int i = indexOf(id);
    QRectF extent;
    if (i < 0 || !layerExtent(m_layers[i], &extent)) return;
    if (extent.width() < 1e-9 && extent.height() < 1e-9) extent.adjust(-50, -50, 50, 50);   // one point
    const double r = qMax(extent.width() / qMax(1.0, m_viewSize.width() - 40), extent.height() / qMax(1.0, m_viewSize.height() - 40));
    setView(extent.center(), r > 0 ? r : 1);
}

void MapService::zoomToAll()
{
    QRectF all;
    bool any = false;
    for (const MapLayer &l : m_layers) {
        QRectF e;
        if (!layerExtent(l, &e)) continue;
        all = any ? all.united(e) : e;
        any = true;
    }
    if (!any) return;
    if (all.width() < 1e-9 && all.height() < 1e-9) all.adjust(-50, -50, 50, 50);
    const double r = qMax(all.width() / qMax(1.0, m_viewSize.width() - 40), all.height() / qMax(1.0, m_viewSize.height() - 40));
    setView(all.center(), r > 0 ? r : 1);
}
void MapService::updateScale()
{
    double mpp = 0;
    if (m_crs.isValid() && m_resolution > 0) {
        // Ground metres per screen pixel at the view centre, geodesically.
        const Crs geo = m_crs.geographic();
        QPointF a = m_center, b = m_center + QPointF(100 * m_resolution, 0);
        CoordinateTransform t(m_crs, geo);
        if (t.transform(a) && t.transform(b)) mpp = Geodesy::distance(a, b, Geodesy::ellipsoidOf(m_crs)) / 100.0;
    }
    if (mpp <= 0 || !std::isfinite(mpp)) {
        m_scaleBarMeters = m_scaleBarPixels = 0;
        m_scaleText.clear();
    } else {
        const double target = 120 * mpp;
        const double base = std::pow(10.0, std::floor(std::log10(target)));
        double nice = base;
        for (double k : {1.0, 2.0, 5.0, 10.0}) if (k * base <= target) nice = k * base;
        m_scaleBarMeters = nice;
        m_scaleBarPixels = nice / mpp;
        const double ratio = mpp / m_metersPerScreenPixel;
        m_scaleText = QStringLiteral("1 : %1").arg(ru().toString(qRound64(ratio)));
    }
    emit viewChanged();
}

// --------------------------------------------------------------- painting

void MapService::paint(QPainter *painter, const QSizeF &size, bool interactive)
{
    Q_UNUSED(interactive)
    Frame f;
    f.layers = m_layers;
    f.crs = m_crs;
    f.center = m_center;
    f.resolution = m_resolution;
    f.size = size;
    f.selectedPhoto = m_selectedPhoto;
    f.selectedFeature = m_selectedFeature;
    QSet<TileKey> wanted;
    MissingTiles missing;
    paintFrame(*painter, f, m_tiles, &wanted, &missing);
    m_tiles->setWanted(wanted);
    for (const auto &m : missing) m_tiles->request(m.first, m.second);

    if (!m_crs.isValid()) {
        painter->setPen(QColor(141, 154, 170));
        painter->drawText(QRectF(QPointF(0, 0), size), Qt::AlignCenter,
                          QStringLiteral("Добавьте геопривязанный слой (GeoTIFF, GeoJSON, KML, GeoPackage)\n"
                                         "или снимки с GPS — карта примет их систему координат."));
        return;
    }
    // Sketch of the measuring/drawing tool.
    if (!m_sketch.isEmpty()) {
        QVector<QPointF> pts = m_sketch;
        if (m_hasHover && !m_sketchDone) pts << m_hover;
        QPolygonF screen;
        for (const QPointF &p : pts) screen << toScreen(p);
        const bool area = m_tool == QLatin1String("area") || m_tool == QLatin1String("polygon");
        QPen pen(SketchColor, 2, Qt::DashLine);
        pen.setCosmetic(true);
        painter->setPen(pen);
        QColor fill = SketchColor;
        fill.setAlphaF(0.18f);
        painter->setBrush(area ? QBrush(fill) : Qt::NoBrush);
        if (area && screen.size() > 2) painter->drawPolygon(screen);
        else painter->drawPolyline(screen);
        painter->setBrush(SketchColor);
        painter->setPen(QPen(QColor(20, 24, 30), 1));
        for (const QPointF &p : m_sketch) painter->drawEllipse(toScreen(p), 4, 4);
    }
}

// ----------------------------------------------------------------- input

void MapService::pointerMoved(const QPointF &screen)
{
    m_lastScreen = screen;
    if (!m_crs.isValid()) return;
    const QPointF map = toMap(screen);
    m_hover = map;
    m_hasHover = true;
    // Coordinates in the map CRS and WGS 84.
    if (m_crs.isProjected())
        m_cursorText = QStringLiteral("X %1  Y %2 %3").arg(number(map.x(), 2), number(map.y(), 2),
                                                           unitLabel(m_crs) == QStringLiteral("метр") ? QStringLiteral("м") : unitLabel(m_crs));
    else
        m_cursorText = QStringLiteral("%1°  %2°").arg(number(map.y(), 6), number(map.x(), 6));
    QPointF wgs = map;
    CoordinateTransform toWgs(m_crs, Crs::wgs84());
    if (toWgs.transform(wgs))
        m_cursorWgs84 = QStringLiteral("%1° %2, %3° %4").arg(number(std::fabs(wgs.y()), 6), wgs.y() >= 0 ? QStringLiteral("с. ш.") : QStringLiteral("ю. ш."),
                                                              number(std::fabs(wgs.x()), 6), wgs.x() >= 0 ? QStringLiteral("в. д.") : QStringLiteral("з. д."));
    else
        m_cursorWgs84.clear();
    // Value of the topmost visible single-band raster (elevation), at most 20 times a second.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastValueQuery > 50) {
        m_lastValueQuery = now;
        m_cursorValue.clear();
        for (int i = int(m_layers.size()) - 1; i >= 0; --i) {
            const MapLayer &l = m_layers[i];
            if (!l.visible || l.type != MapLayer::Raster || !l.raster || l.style.mode != RasterStyle::SingleBand) continue;
            const double v = l.raster->valueAt(map, l.style.band);
            if (std::isfinite(v)) m_cursorValue = QStringLiteral("%1: %2").arg(l.name, number(v, 2));
            break;
        }
    }
    emit cursorChanged();
    if (m_dragFeature < 0 && !m_panning && !m_sketch.isEmpty() && !m_sketchDone) {
        updateMeasurement();
        emit repaint();
    }
}

void MapService::pointerPressed(const QPointF &screen, Qt::MouseButton button, Qt::KeyboardModifiers)
{
    m_pressScreen = m_lastScreen = screen;
    m_panning = false;
    m_dragFeature = m_dragVertex = -1;
    if (button == Qt::MiddleButton) {
        m_panning = true;
        return;
    }
    if (button == Qt::RightButton) {
        finishSketch();
        return;
    }
    if (m_tool == QLatin1String("edit") && m_crs.isValid()) {
        if (m_selectedFeature >= 0) {
            const int v = hitVertex(m_selectedFeature, screen);
            if (v >= 0) {
                m_dragFeature = m_selectedFeature;
                m_dragVertex = v;
                const VectorData *d = drawings();
                int idx = v;
                for (const QPolygonF &part : d->features[m_dragFeature].parts) {
                    if (idx < part.size()) { m_dragFrom = part[idx]; break; }
                    idx -= int(part.size());
                }
                return;
            }
        }
        const int hit = hitFeature(screen);
        if (hit != m_selectedFeature) {
            m_selectedFeature = hit;
            updateMeasurement();
            emit drawingsChanged();
            emit repaint();
        }
    }
}

void MapService::pointerDragged(const QPointF &screen)
{
    const QPointF delta = screen - m_lastScreen;
    if (m_dragFeature >= 0) {
        VectorData *d = drawings();
        if (d && m_dragFeature < d->features.size()) {
            int idx = m_dragVertex;
            for (QPolygonF &part : d->features[m_dragFeature].parts) {
                if (idx < part.size()) { part[idx] = toMap(screen); break; }
                idx -= int(part.size());
            }
            updateMeasurement();
            emit repaint();
        }
        m_lastScreen = screen;
        return;
    }
    if (!m_panning && (screen - m_pressScreen).manhattanLength() > 4) m_panning = true;
    if (m_panning) panBy(delta);
    m_lastScreen = screen;
}

void MapService::pointerReleased(const QPointF &screen)
{
    if (m_ignoreRelease) {
        m_ignoreRelease = false;
        m_panning = false;
        return;
    }
    if (m_dragFeature >= 0) {
        VectorData *d = drawings();
        if (d && m_dragFeature < d->features.size()) {
            int part = 0, vertex = m_dragVertex;
            for (const QPolygonF &p : d->features[m_dragFeature].parts) {
                if (vertex < p.size()) break;
                vertex -= int(p.size());
                ++part;
            }
            auto *cmd = new DrawingCommand(this, DrawingCommand::MoveVertex, QStringLiteral("Перемещение вершины"));
            cmd->featureId = d->features[m_dragFeature].id;
            cmd->part = part;
            cmd->vertex = vertex;
            cmd->from = m_dragFrom;
            cmd->to = toMap(screen);
            m_undo.push(cmd);
        }
        m_dragFeature = m_dragVertex = -1;
        return;
    }
    if (m_panning) {
        m_panning = false;
        return;
    }
    if (!m_crs.isValid()) return;
    const QPointF map = toMap(screen);
    if (m_tool == QLatin1String("length") || m_tool == QLatin1String("area") || m_tool == QLatin1String("line")
        || m_tool == QLatin1String("polygon")) {
        if (m_sketchDone) {
            m_sketch.clear();
            m_sketchDone = false;
        }
        m_sketch << map;
        updateMeasurement();
        emit repaint();
    } else if (m_tool == QLatin1String("point")) {
        VectorFeature f;
        f.kind = VectorFeature::Point;
        f.id = m_nextFeatureId++;
        f.parts << QPolygonF({map});
        f.attributes.insert(QStringLiteral("name"), QStringLiteral("Точка %1").arg(f.id));
        auto *cmd = new DrawingCommand(this, DrawingCommand::Add, QStringLiteral("Добавление точки"));
        cmd->feature = f;
        m_undo.push(cmd);
    }
}

void MapService::pointerDoubleClicked(const QPointF &)
{
    // The first click of the pair already added a vertex; the second must not add another.
    m_ignoreRelease = true;
    finishSketch();
}

void MapService::finishSketch()
{
    if (m_sketch.isEmpty()) return;
    const bool line = m_tool == QLatin1String("line"), polygon = m_tool == QLatin1String("polygon");
    if (line || polygon) {
        QVector<QPointF> pts = m_sketch;
        for (int i = int(pts.size()) - 1; i > 0; --i)   // double click leaves duplicates
            if ((toScreen(pts[i]) - toScreen(pts[i - 1])).manhattanLength() < 3) pts.removeAt(i);
        if (pts.size() >= (polygon ? 3 : 2)) {
            VectorFeature f;
            f.kind = polygon ? VectorFeature::Polygon : VectorFeature::Line;
            f.id = m_nextFeatureId++;
            f.parts << QPolygonF(pts);
            f.bounds = f.parts.first().boundingRect();
            f.attributes.insert(QStringLiteral("name"), (polygon ? QStringLiteral("Полигон %1") : QStringLiteral("Линия %1")).arg(f.id));
            auto *cmd = new DrawingCommand(this, DrawingCommand::Add, polygon ? QStringLiteral("Добавление полигона")
                                                                              : QStringLiteral("Добавление линии"));
            cmd->feature = f;
            m_undo.push(cmd);
        }
        m_sketch.clear();
        m_sketchDone = false;
    } else {
        m_sketchDone = true;   // the measurement stays on screen until the next click
    }
    updateMeasurement();
    emit repaint();
}

bool MapService::keyPressed(int key)
{
    if (key == Qt::Key_Escape) {
        m_sketch.clear();
        m_sketchDone = false;
        if (m_selectedFeature >= 0) {
            m_selectedFeature = -1;
            emit drawingsChanged();
        }
        updateMeasurement();
        emit repaint();
        return true;
    }
    if (key == Qt::Key_Backspace && !m_sketch.isEmpty() && !m_sketchDone) {
        m_sketch.removeLast();
        updateMeasurement();
        emit repaint();
        return true;
    }
    if (key == Qt::Key_Return || key == Qt::Key_Enter) {
        finishSketch();
        return true;
    }
    if (key == Qt::Key_Delete && m_selectedFeature >= 0) {
        deleteSelected();
        return true;
    }
    return false;
}

void MapService::setTool(const QString &tool)
{
    if (tool == m_tool) return;
    m_tool = tool;
    m_sketch.clear();
    m_sketchDone = false;
    if (tool != QLatin1String("edit") && m_selectedFeature >= 0) {
        m_selectedFeature = -1;
        emit drawingsChanged();
    }
    updateMeasurement();
    emit toolChanged();
    emit repaint();
}

void MapService::deleteSelected()
{
    VectorData *d = drawings();
    if (!d || m_selectedFeature < 0 || m_selectedFeature >= d->features.size()) return;
    auto *cmd = new DrawingCommand(this, DrawingCommand::Remove, QStringLiteral("Удаление объекта"));
    cmd->feature = d->features[m_selectedFeature];
    cmd->index = m_selectedFeature;
    m_undo.push(cmd);
}

void MapService::clearMeasurement()
{
    m_sketch.clear();
    m_sketchDone = false;
    updateMeasurement();
    emit repaint();
}

void MapService::applyDrawingChange()
{
    const int i = drawingsIndex();
    if (i >= 0) {
        MapLayer &l = m_layers[i];
        l.vector.valid = true;
        l.vector.points = l.vector.lines = l.vector.polygons = 0;
        l.vector.extent = QRectF();
        for (const VectorFeature &f : l.vector.features) {
            if (f.kind == VectorFeature::Point) ++l.vector.points;
            else if (f.kind == VectorFeature::Line) ++l.vector.lines;
            else ++l.vector.polygons;
            const QRectF b = f.kind == VectorFeature::Point ? QRectF(f.parts.first().first(), QSizeF(0, 0)) : f.bounds;
            l.vector.extent = l.vector.extent.isNull() ? b : l.vector.extent.united(b);
        }
        m_model->layerChanged(i);
    }
    if (m_selectedFeature >= drawingCount()) m_selectedFeature = -1;
    updateMeasurement();
    scheduleSave();
    emit drawingsChanged();
    emit repaint();
}

int MapService::hitFeature(const QPointF &screen) const
{
    const int i = drawingsIndex();
    if (i < 0) return -1;
    const VectorData &d = m_layers[i].vector;
    const double tolerance = 6;
    for (int k = int(d.features.size()) - 1; k >= 0; --k) {
        const VectorFeature &f = d.features[k];
        for (const QPolygonF &part : f.parts) {
            QPolygonF s;
            for (const QPointF &p : part) s << toScreen(p);
            if (f.kind == VectorFeature::Polygon && s.containsPoint(screen, Qt::OddEvenFill)) return k;
            for (int j = 0; j < s.size(); ++j) {
                if ((s[j] - screen).manhattanLength() <= tolerance * 1.5) return k;
                if (j == 0) continue;
                // Distance from the segment.
                const QPointF a = s[j - 1], b = s[j], ab = b - a;
                const double len2 = QPointF::dotProduct(ab, ab);
                const double t = len2 > 0 ? qBound(0.0, QPointF::dotProduct(screen - a, ab) / len2, 1.0) : 0;
                const QPointF c = a + ab * t;
                if (std::hypot(screen.x() - c.x(), screen.y() - c.y()) <= tolerance) return k;
            }
        }
    }
    return -1;
}

int MapService::hitVertex(int feature, const QPointF &screen) const
{
    const int i = drawingsIndex();
    if (i < 0 || feature < 0 || feature >= m_layers[i].vector.features.size()) return -1;
    int index = 0;
    for (const QPolygonF &part : m_layers[i].vector.features[feature].parts)
        for (const QPointF &p : part) {
            const QPointF s = toScreen(p);
            if (std::fabs(s.x() - screen.x()) <= 6 && std::fabs(s.y() - screen.y()) <= 6) return index;
            ++index;
        }
    return -1;
}

// ------------------------------------------------------------ measurement

MapService::Measure MapService::measure(const QVector<QPointF> &mapPoints, bool closed) const
{
    Measure m;
    if (!m_crs.isValid() || mapPoints.size() < 2) return m;
    QVector<QPointF> geo = mapPoints;
    CoordinateTransform toGeo(m_crs, m_crs.geographic());
    if (!toGeo.transform(geo)) return m;
    const Geodesy::Ellipsoid e = Geodesy::ellipsoidOf(m_crs);
    if (closed && geo.size() >= 3) {
        const Geodesy::Polygon p = Geodesy::polygon(geo, e);
        m.area = p.area;
        m.perimeter = p.perimeter;
    }
    m.length = Geodesy::length(geo, e);
    if (m_crs.isProjected()) {
        const double k = m_crs.metersPerUnit();
        m.planarLength = planarLength(mapPoints, closed) * k;
        if (closed && mapPoints.size() >= 3) m.planarArea = shoelace(mapPoints) * k * k;
    }
    return m;
}

QString MapService::formatLength(double m)
{
    return m >= 10000 ? QStringLiteral("%1 км").arg(number(m / 1000, 3)) : QStringLiteral("%1 м").arg(number(m, m < 100 ? 2 : 1));
}

QString MapService::formatArea(double a)
{
    if (a >= 1e6) return QStringLiteral("%1 км² (%2 га)").arg(number(a / 1e6, 4), number(a / 1e4, 2));
    if (a >= 1e4) return QStringLiteral("%1 га (%2 м²)").arg(number(a / 1e4, 4), number(a, 1));
    return QStringLiteral("%1 м²").arg(number(a, 2));
}

void MapService::updateMeasurement()
{
    QVector<QPointF> pts;
    bool closed = false;
    QString what;
    if (!m_sketch.isEmpty()) {
        pts = m_sketch;
        if (m_hasHover && !m_sketchDone) pts << m_hover;
        closed = m_tool == QLatin1String("area") || m_tool == QLatin1String("polygon");
    } else if (m_selectedFeature >= 0 && m_selectedFeature < drawingCount()) {
        const VectorFeature &f = m_layers[drawingsIndex()].vector.features[m_selectedFeature];
        if (!f.parts.isEmpty()) pts = QVector<QPointF>(f.parts.first().begin(), f.parts.first().end());
        closed = f.kind == VectorFeature::Polygon;
        what = f.attributes.value(QStringLiteral("name")).toString() + QStringLiteral(": ");
    }
    const QString ellipsoid = m_crs.isValid() ? m_crs.geographic().name() : QString();
    if (pts.size() < 2) {
        m_measurementText = m_tool == QLatin1String("length") ? QStringLiteral("Щёлкайте по карте, чтобы измерить длину")
                          : m_tool == QLatin1String("area") ? QStringLiteral("Щёлкайте по карте, чтобы измерить площадь")
                          : QString();
        m_measurementDetails.clear();
    } else {
        const Measure m = measure(pts, closed);
        if (closed && pts.size() >= 3) {
            m_measurementText = what + QStringLiteral("площадь %1, периметр %2").arg(formatArea(m.area), formatLength(m.perimeter));
            m_measurementDetails = QStringLiteral("Геодезически на эллипсоиде (%1). По плоскости %2: %3. Вершин: %4.")
                                       .arg(ellipsoid, m_crs.label(), m_crs.isProjected() ? formatArea(m.planarArea) : QStringLiteral("—"))
                                       .arg(pts.size());
        } else {
            m_measurementText = what + QStringLiteral("длина %1").arg(formatLength(m.length));
            m_measurementDetails = QStringLiteral("Геодезически на эллипсоиде (%1). По плоскости %2: %3. Вершин: %4.")
                                       .arg(ellipsoid, m_crs.label(), m_crs.isProjected() ? formatLength(m.planarLength) : QStringLiteral("—"))
                                       .arg(pts.size());
        }
    }
    emit measurementChanged();
}

// ----------------------------------------------------------------- export

QString MapService::exportDrawings(const QUrl &url)
{
    const int i = drawingsIndex();
    if (i < 0 || m_layers[i].vector.features.isEmpty()) return QStringLiteral("Нет нарисованных объектов.");
    QVector<VectorFeature> features = m_layers[i].vector.features;
    for (VectorFeature &f : features) {
        const QVector<QPointF> pts(f.parts.first().begin(), f.parts.first().end());
        if (f.kind == VectorFeature::Line) f.attributes.insert(QStringLiteral("length_m"), toMillimetres(measure(pts, false).length));
        if (f.kind == VectorFeature::Polygon) {
            const Measure m = measure(pts, true);
            f.attributes.insert(QStringLiteral("area_m2"), toMillimetres(m.area));
            f.attributes.insert(QStringLiteral("perimeter_m"), toMillimetres(m.perimeter));
        }
    }
    QString error;
    if (!writeVectorFile(url.toLocalFile(), QStringLiteral("geomap_drawings"), features, m_crs, &error)) return error;
    return {};
}

namespace {
// What an export needs, copied on the GUI thread so that it can be painted on a worker.
struct ExportState
{
    QVector<MapLayer> layers;
    Crs crs;
    QPointF center;
    double resolution = 1;
    QSizeF viewSize;
    double scaleBarPixels = 0, scaleBarMeters = 0;
};

// A4 landscape PDF: 10 mm margins, 200 dpi; the image fills the printable area exactly, so the
// printed scale is known.
constexpr double PdfDpi = 200;
constexpr double PdfMarginMm = 10;
const QSize PdfImageSize(qRound((297 - 2 * PdfMarginMm) / 25.4 * PdfDpi), qRound((210 - 2 * PdfMarginMm) / 25.4 * PdfDpi));

// paperMetersPerPixel: size of one image pixel on paper, or 0 when the output has no physical
// size (PNG); then only the scale bar is drawn, without a "1 : N" ratio.
QImage renderState(const ExportState &s, const QSize &size, bool decorations, double paperMetersPerPixel = 0)
{
    const QVector<MapLayer> &m_layers = s.layers;
    const Crs &m_crs = s.crs;
    const QSizeF &m_viewSize = s.viewSize;
    const double m_scaleBarPixels = s.scaleBarPixels, m_scaleBarMeters = s.scaleBarMeters;
    Frame f;
    f.layers = s.layers;
    f.crs = s.crs;
    f.center = s.center;
    f.resolution = s.resolution * (double(s.viewSize.width()) / qMax(1, size.width()));
    f.size = size;
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    QPainter p(&image);
    paintFrame(p, f, nullptr, nullptr, nullptr);
    if (!decorations) return image;
    // Legend, scale bar, CRS, sources.
    p.setRenderHint(QPainter::Antialiasing, true);
    QFont font = p.font();
    font.setPixelSize(qMax(12, size.height() / 60));
    p.setFont(font);
    const int line = QFontMetrics(font).height();
    QStringList legend;
    for (int i = int(m_layers.size()) - 1; i >= 0; --i)
        if (m_layers[i].visible && (m_layers[i].raster || (m_layers[i].vector.valid && !m_layers[i].vector.features.isEmpty())))
            legend << QString::number(i);
    const int boxW = size.width() / 3, boxH = (legend.size() + 5) * line + 16;
    const QRect box(size.width() - boxW - 12, 12, boxW, boxH);
    p.setPen(QColor(60, 75, 95));
    p.setBrush(QColor(16, 21, 28, 225));
    p.drawRoundedRect(box, 6, 6);
    int y = box.top() + 8 + line;
    p.setPen(QColor(236, 242, 250));
    p.drawText(box.left() + 10, y, QStringLiteral("Легенда"));
    for (const QString &k : legend) {
        const MapLayer &l = m_layers[k.toInt()];
        y += line;
        const QRect swatch(box.left() + 10, y - line + 5, line - 4, line - 6);
        if (l.type == MapLayer::Raster && l.style.mode == RasterStyle::SingleBand) {
            QLinearGradient g(swatch.topLeft(), swatch.topRight());
            for (const auto &s : RasterStyle::rampStops(l.style.ramp)) g.setColorAt(s.first, s.second);
            p.fillRect(swatch, g);
        } else if (l.type == MapLayer::Raster) {
            p.fillRect(swatch, QColor(110, 140, 110));
        } else {
            p.fillRect(swatch, l.color);
        }
        p.setPen(QColor(220, 230, 240));
        p.drawText(swatch.right() + 8, y, QFontMetrics(font).elidedText(l.name, Qt::ElideRight, boxW - line - 24));
    }
    y += line + 4;
    p.setPen(QColor(170, 185, 200));
    p.drawText(box.left() + 10, y, QFontMetrics(font).elidedText(QStringLiteral("CRS: ") + m_crs.label(), Qt::ElideRight, boxW - 20));
    y += line;
    p.drawText(box.left() + 10, y, QDateTime::currentDateTime().toString(QStringLiteral("dd.MM.yyyy HH:mm")) + QStringLiteral(" · GeoMap AI"));
    // Bottom left: scale bar (ground distance at the centre, geodesic) above the sources line.
    const double sourcesTop = size.height() - line - 8;
    const double factor = double(size.width()) / qMax(1.0, m_viewSize.width());
    if (m_scaleBarPixels > 0) {
        const double px = m_scaleBarPixels * factor;
        const QPointF a(24, sourcesTop - 6), b(24 + px, sourcesTop - 6);
        p.setPen(QPen(QColor(255, 255, 255), 3));
        p.drawLine(a, b);
        p.drawLine(a, a - QPointF(0, 8));
        p.drawLine(b, b - QPointF(0, 8));
        QString text = MapService::formatLength(m_scaleBarMeters);
        if (paperMetersPerPixel > 0)   // ground metres per image pixel / paper metres per image pixel
            text += QStringLiteral("   1 : %1 (A4)").arg(QLocale(QLocale::Russian).toString(qRound64(m_scaleBarMeters / px / paperMetersPerPixel)));
        p.drawText(QPointF(24, a.y() - 10), text);
    }
    p.setPen(QPen(QColor(255, 255, 255), 2));
    const QPointF n(size.width() - 40, size.height() - 70);
    p.drawLine(n, n - QPointF(0, 36));
    p.drawLine(n - QPointF(0, 36), n - QPointF(7, 24));
    p.drawLine(n - QPointF(0, 36), n - QPointF(-7, 24));
    p.drawText(QPointF(n.x() - 6, n.y() - 42), QStringLiteral("N"));
    const QString north = QStringLiteral("север сетки");
    const int northWidth = QFontMetrics(font).horizontalAdvance(north);
    p.drawText(QPointF(qMin(n.x() - northWidth / 2.0, size.width() - northWidth - 8.0), n.y() + line), north);
    // Sources.
    QStringList sources;
    for (const MapLayer &l : m_layers)   // only what is drawn
        if (l.visible && !l.source.isEmpty() && (l.raster || l.vector.valid)) sources << QFileInfo(l.source).fileName();
    if (!sources.isEmpty()) {
        p.setPen(QColor(200, 210, 220));
        p.drawText(QRectF(24, sourcesTop, size.width() - 200, line + 4), Qt::AlignLeft,
                   QFontMetrics(font).elidedText(QStringLiteral("Источники: ") + sources.join(QStringLiteral(", ")),
                                                 Qt::ElideRight, size.width() - 220));
    }
    return image;
}
} // namespace

QImage MapService::renderMap(const QSize &size, bool decorations) const
{
    const ExportState state{m_layers, m_crs, m_center, m_resolution, m_viewSize, m_scaleBarPixels, m_scaleBarMeters};
    return renderState(state, size, decorations);
}

void MapService::exportMap(const QUrl &url)
{
    if (m_exporting || !m_crs.isValid()) return;
    const QString path = url.toLocalFile();
    const bool pdf = path.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive);
    m_exporting = true;
    emit exportChanged();
    // Twice the screen size for PNG; the printable area of A4 landscape for PDF. The PDF view
    // keeps the screen's centre and covers the screen's ground width.
    const QSize size = pdf ? PdfImageSize : (m_viewSize * 2).toSize();
    const ExportState state{m_layers, m_crs, m_center, m_resolution, m_viewSize, m_scaleBarPixels, m_scaleBarMeters};
    QThread *thread = QThread::create([this, path, pdf, size, state] {
        const QImage image = renderState(state, size, true, pdf ? 0.0254 / PdfDpi : 0.0);
        bool ok = false;
        if (pdf) {
            QPdfWriter writer(path);
            writer.setPageLayout(QPageLayout(QPageSize(QPageSize::A4), QPageLayout::Landscape,
                                             QMarginsF(PdfMarginMm, PdfMarginMm, PdfMarginMm, PdfMarginMm), QPageLayout::Millimeter));
            writer.setResolution(int(PdfDpi));
            writer.setTitle(QStringLiteral("Карта GeoMap AI"));
            writer.setCreator(QStringLiteral("GeoMap AI"));
            QPainter p;
            if (p.begin(&writer)) {
                const QRect page = p.viewport();
                const QSize scaled = image.size().scaled(page.size(), Qt::KeepAspectRatio);
                p.drawImage(QRect(page.topLeft(), scaled), image);
                ok = p.end();
            }
        } else {
            ok = image.save(path, "PNG");
        }
        QMetaObject::invokeMethod(this, [this, ok, path] {
            m_exporting = false;
            emit exportChanged();
            emit exportFinished(ok, ok ? QStringLiteral("Карта сохранена: %1").arg(QDir::toNativeSeparators(path))
                                       : QStringLiteral("Не удалось сохранить карту: %1").arg(QDir::toNativeSeparators(path)));
        }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}
