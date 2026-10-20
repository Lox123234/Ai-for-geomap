#include "MapService.h"
#include "WebTiles.h"
#include "AiService.h"
#include "Geodesy.h"
#include "GisEnvironment.h"
#include "Inpainter.h"
#include "MapTiles.h"
#include "MaskPolygons.h"
#include "PhotoModel.h"
#include "RasterEdit.h"
#include "Retouch.h"
#include "SamSegmenter.h"
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
#include <QSettings>
#include <QTextStream>
#include <QThread>
#include <QThreadPool>
#include <QUndoCommand>
#include <QUuid>
#include <cmath>
#include <functional>
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
    QSet<qint64> selectedIds;   // further selected drawings (outlined, without vertex handles)
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
        const bool alsoSelected = handlesFor && !isSelected && f.selectedIds.contains(feature.id);
        QPen pen(isSelected || alsoSelected ? QColor(255, 255, 255) : color, isSelected || alsoSelected ? 3 : 2);
        if (alsoSelected) pen.setStyle(Qt::DashLine);
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
        // The selected drawing is followed by its id: insertions and removals shift positions.
        const int selected = m_service->m_selectedFeature;
        const qint64 selectedId = selected >= 0 && selected < d->features.size() ? d->features[selected].id : -1;
        applyChange(d, forward);
        m_service->m_selectedFeature = -1;
        for (int i = 0; i < d->features.size() && selectedId >= 0; ++i)
            if (d->features[i].id == selectedId) m_service->m_selectedFeature = i;
        m_service->applyDrawingChange();
    }
    void applyChange(VectorData *d, bool forward)
    {
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
        }
    }
    MapService *m_service;
    Kind m_kind;
};

// A window of an edited raster before and after an AI edit (bands 1–3).
class RasterPatchCommand final : public QUndoCommand
{
public:
    RasterPatchCommand(MapService *service, const QString &path, const QRect &window, const QImage &before, const QImage &after)
        : QUndoCommand(QStringLiteral("Удаление объекта с ортофотоплана (ИИ)")), m_service(service), m_path(path), m_window(window),
          m_before(before), m_after(after) {}
    void redo() override
    {
        if (m_first) m_first = false;   // already written when the edit was made
        else write(m_after);
    }
    void undo() override { write(m_before); }

private:
    void write(const QImage &image)
    {
        QString error;
        if (!rasteredit::writeRgb(m_path, m_window, image, &error)) emit m_service->errorOccurred(error);
        m_service->rasterEdited(m_path);
    }
    MapService *m_service;
    QString m_path;
    QRect m_window;
    QImage m_before, m_after;
    bool m_first = true;
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
    // Online basemap: tiles that arrive redraw its part of the map (at most four times a second).
    m_webRepaint.setSingleShot(true);
    m_webRepaint.setInterval(250);
    connect(&m_webRepaint, &QTimer::timeout, this, [this] {
        bool any = false;
        for (MapLayer &l : m_layers)
            if (l.basemap && l.source.startsWith(QLatin1String("web:"))) {
                ++l.version;
                any = true;
            }
        if (any) emit repaint();
    });
    connect(webtiles::TileStore::instance(), &webtiles::TileStore::arrived, this, &MapService::webTilesArrived);
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
    if (path.startsWith(QLatin1String("web:"))) return path;   // an online basemap, not a file
    const QString rel = QDir(m_project).relativeFilePath(path);
    return rel.startsWith(QLatin1String("..")) || QDir::isAbsolutePath(rel) ? QDir::cleanPath(path) : rel;
}

QString MapService::resolveSource(const QString &path) const
{
    if (path.startsWith(QLatin1String("web:"))) return path;
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
    m_terrainText.clear();
    m_terrainDetails.clear();
    ++m_terrainGeneration;
    m_profile = {};
    m_volume = {};
    m_profileMarker = -1;
    m_cross.clear();
    m_crossLines.clear();
    m_crossIndex = 0;
    m_pointValues.clear();
    m_measurements.clear();
    m_shownMeasurement.clear();
    m_multiSelection.clear();
    m_markers.clear();
    m_overlap.clear();
    m_showGrid = false;
    m_showMarkers = true;
    ++m_aiGeneration;
    m_aiCandidate.clear();
    m_aiPoints.clear();
    m_aiLabels.clear();
    m_aiText.clear();
    m_aiDetails.clear();
    if (!m_aiBusy) m_aiStatus.clear();
    m_project = directory.isEmpty() ? QString() : QDir(directory).absolutePath();
    m_photos = photos;
    if (!m_project.isEmpty()) {
        loadState();
        loadMeasurements();
        loadMarkers();
    }
    emit profileChanged();
    emit measurementsChanged();
    emit aiChanged();
    emit markersChanged();
    emit viewOptionsChanged();
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
        m_showGrid = state.value("grid").toBool(false);
        m_showMarkers = state.value("markers").toBool(true);
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
        l.basemap = o.value("basemap").toBool();
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
    if (l.basemap) o.insert("basemap", true);
        layers.append(o);
    }
    const QJsonObject state{{"schemaVersion", 1}, {"crs", m_crs.isValid() ? m_crs.wkt() : QString()},
                            {"crsEpsg", m_crs.epsg()}, {"crsNote", m_crsNote},
                            {"view", QJsonObject{{"x", m_center.x()}, {"y", m_center.y()}, {"resolution", m_resolution}}},
                            {"layers", layers}, {"grid", m_showGrid}, {"markers", m_showMarkers}};
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
            f.attributes.insert(QStringLiteral("area_m2"), toMillimetres(polygonArea(f)));
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
        if (m_useNextStyle) {   // analysis results come with their own colour ramp
            l.style = m_nextStyle;
            l.status = QStringLiteral("style");
        }
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

void MapService::releaseRasterFile(const QString &path)
{
    const QString wanted = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    for (MapLayer &l : m_layers) {
        if (l.type != MapLayer::Raster || QDir::cleanPath(QFileInfo(l.source).absoluteFilePath()).compare(wanted, Qt::CaseInsensitive) != 0) continue;
        if (l.cancel) *l.cancel = true;   // statistics or pyramids of the old file
        l.raster.reset();                 // its handles close with the last tile rendering from it
    }
    m_tiles->clear();
}

void MapService::reopenRasterFile(const QString &path)
{
    const QString wanted = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    bool any = false;
    for (int i = 0; i < m_layers.size(); ++i) {
        MapLayer &l = m_layers[i];
        if (l.type != MapLayer::Raster || QDir::cleanPath(QFileInfo(l.source).absoluteFilePath()).compare(wanted, Qt::CaseInsensitive) != 0) continue;
        l.status = QStringLiteral("style");   // keep the style the user set
        prepareRaster(i);
        ++l.version;
        changed(i);
        any = true;
    }
    if (any) emit repaint();
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
    if (l.source.startsWith(QLatin1String("web:"))) {   // online basemap: tiles drawn as the view needs them
        const webtiles::Provider provider = webtiles::provider(l.source.mid(4));
        if (!m_crs.isValid()) {
            l.status = QStringLiteral("Подложка появится, когда у карты будет система координат.");
            return;
        }
        auto web = std::make_shared<webtiles::WebRasterSource>(provider, m_crs, m_center);   // ±100 km around the view
        if (!web->isValid()) {
            l.status = web->error();
            return;
        }
        l.rasterInfo = RasterInfo{};
        l.rasterInfo.valid = true;
        l.rasterInfo.path = l.source;
        l.rasterInfo.driver = QStringLiteral("XYZ");
        l.rasterInfo.bands = 4;
        l.rasterInfo.alphaBand = 4;
        l.rasterInfo.dataType = QStringLiteral("Byte");
        l.rasterInfo.hasGeoTransform = true;
        l.rasterInfo.crs = Crs::fromEpsg(3857);
        l.raster = web;
        return;
    }
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
    const bool firstCrs = !m_crs.isValid();
    if (firstCrs) {
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
    // The first data of a new project: the map shows it, not an empty view at the origin.
    if (firstCrs && !m_loadingState) {
        zoomToAll();
        scheduleSave();
    }
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

void MapService::releaseDirectory(const QString &directory)
{
    const QString prefix = QDir::cleanPath(QDir(directory).absolutePath()) + QLatin1Char('/');
    QStringList ids;
    for (const MapLayer &l : std::as_const(m_layers))
        if (!l.source.isEmpty() && QDir::cleanPath(l.source).startsWith(prefix, Qt::CaseInsensitive)) ids << l.id;
    for (const QString &id : ids) removeLayer(id);
    if (!ids.isEmpty()) m_tiles->clear();
}

void MapService::moveLayer(const QString &id, int delta)
{
    const int i = indexOf(id);
    const int j = i + delta;
    if (i < 0 || j < 0 || j >= m_layers.size()) return;
    if (m_layers[i].basemap || m_layers[j].basemap) return;   // the background stays at the bottom
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
    if (size.isEmpty()) return;
    const bool first = !m_viewSizeKnown;
    m_viewSizeKnown = true;
    if (size == m_viewSize && !(first && m_fitPending)) return;
    m_viewSize = size;
    if (m_fitPending) {   // a fit to the data asked for before the map knew its size
        m_fitPending = false;
        zoomToAll();
    }
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
    // The data: visible layers (else all), never a basemap — an online basemap spans ±100 km
    // around where the view was, and the view would centre on that instead.
    if (!m_viewSizeKnown) m_fitPending = true;   // fitted again when the map's real size arrives
    QRectF all;
    bool any = false;
    for (int pass = 0; pass < 2 && !any; ++pass)
        for (const MapLayer &l : m_layers) {
            QRectF e;
            if (l.basemap || l.source.startsWith(QLatin1String("web:")) || (pass == 0 && !l.visible) || !layerExtent(l, &e)) continue;
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
    f.selectedIds = m_multiSelection;
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
    if (m_overlay) {
        painter->save();
        m_overlay->paintOverlay(painter, *this);
        painter->restore();
    }
    if (m_showGrid) paintGrid(painter, size);
    paintMarkers(painter);
    // Sketch of the measuring/drawing tool.
    if (!m_sketch.isEmpty()) {
        QVector<QPointF> pts = m_sketch;
        if (m_hasHover && !m_sketchDone) pts << m_hover;
        QPolygonF screen;
        for (const QPointF &p : pts) screen << toScreen(p);
        const bool area = m_tool == QLatin1String("area") || m_tool == QLatin1String("polygon") || m_tool == QLatin1String("volume");
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
    paintAnalysis(painter);
    // Contour proposed by the AI selection and its prompts (+ on the object, − excluded).
    if (!m_aiCandidate.isEmpty() || !m_aiPoints.isEmpty()) {
        QPainterPath path;
        path.setFillRule(Qt::OddEvenFill);
        for (const QPolygonF &ring : m_aiCandidate) {
            QPolygonF s;
            for (const QPointF &p : ring) s << toScreen(p);
            path.addPolygon(s);
            path.closeSubpath();
        }
        QPen pen(QColor(255, 92, 205), 2);
        pen.setCosmetic(true);
        painter->setPen(pen);
        painter->setBrush(QColor(255, 92, 205, 60));
        painter->drawPath(path);
        for (int i = 0; i < m_aiPoints.size(); ++i) {
            const QPointF s = toScreen(m_aiPoints[i]);
            const bool on = m_aiLabels.value(i) == 1;
            painter->setPen(QPen(QColor(20, 24, 30), 1.5));
            painter->setBrush(on ? QColor(90, 220, 120) : QColor(240, 80, 80));
            painter->drawEllipse(s, 6, 6);
            painter->setPen(QPen(Qt::white, 2));
            painter->drawLine(s - QPointF(3, 0), s + QPointF(3, 0));
            if (on) painter->drawLine(s - QPointF(0, 3), s + QPointF(0, 3));
        }
    }
    // The saved measurement chosen in the panel.
    for (const SavedMeasurement &s : m_measurements) {
        if (s.id != m_shownMeasurement || s.geometry.size() < 2 || !m_crs.isValid()) continue;
        QVector<QPointF> pts = s.geometry;
        if (!CoordinateTransform(Crs::wgs84(), m_crs).transform(pts)) break;
        QPolygonF screen;
        for (const QPointF &p : pts) screen << toScreen(p);
        QPen pen(QColor(80, 230, 255), 3);
        pen.setCosmetic(true);
        painter->setPen(pen);
        const bool ring = s.kind == QLatin1String("area") || s.kind == QLatin1String("volume");
        QColor fill(80, 230, 255, 50);
        painter->setBrush(ring ? QBrush(fill) : Qt::NoBrush);
        if (ring && screen.size() > 2) painter->drawPolygon(screen);
        else painter->drawPolyline(screen);
        break;
    }
    // Point of the elevation profile under the chart cursor.
    if (interactive && m_profileMarker >= 0 && m_profileMarker < m_profile.points.size()) {
        const terrain::ProfilePoint &pp = m_profile.points[m_profileMarker];
        const QPointF s = toScreen(QPointF(pp.x, pp.y));
        painter->setPen(QPen(QColor(20, 24, 30), 2));
        painter->setBrush(QColor(80, 230, 255));
        painter->drawEllipse(s, 6, 6);
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

void MapService::pointerPressed(const QPointF &screen, Qt::MouseButton button, Qt::KeyboardModifiers modifiers)
{
    m_pressScreen = m_lastScreen = screen;
    m_pressModifiers = modifiers;
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
        const VectorData *d = drawings();
        if (modifiers.testFlag(Qt::ControlModifier) && d && hit >= 0) {
            // Ctrl+click: one more drawing in the selection, or one less.
            const qint64 id = d->features[hit].id;
            if (hit == m_selectedFeature) {
                m_selectedFeature = -1;
                if (!m_multiSelection.isEmpty()) {   // another selected one becomes the primary
                    const qint64 next = *m_multiSelection.cbegin();
                    m_multiSelection.remove(next);
                    for (int i = 0; i < d->features.size(); ++i) if (d->features[i].id == next) m_selectedFeature = i;
                }
            } else if (m_multiSelection.contains(id)) {
                m_multiSelection.remove(id);
            } else if (m_selectedFeature < 0) {
                m_selectedFeature = hit;
            } else {
                m_multiSelection.insert(id);
            }
            updateMeasurement();
            emit drawingsChanged();
            emit repaint();
            return;
        }
        if (hit != m_selectedFeature || !m_multiSelection.isEmpty()) {
            m_selectedFeature = hit;
            m_multiSelection.clear();
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
        || m_tool == QLatin1String("polygon") || m_tool == QLatin1String("profile") || m_tool == QLatin1String("volume")
        || m_tool == QLatin1String("crossProfile") || m_tool == QLatin1String("slope") || m_tool == QLatin1String("angle")) {
        if (m_sketchDone) {
            m_sketch.clear();
            m_sketchDone = false;
            m_terrainText.clear();
            m_terrainDetails.clear();
            m_pointValues.clear();
        }
        m_sketch << map;
        // Two points make a slope, three an angle.
        if ((m_tool == QLatin1String("slope") && m_sketch.size() == 2) || (m_tool == QLatin1String("angle") && m_sketch.size() == 3)) {
            finishSketch();
            return;
        }
        updateMeasurement();
        emit repaint();
    } else if (m_tool == QLatin1String("height") || m_tool == QLatin1String("viewshed")) {
        m_sketch = {map};
        m_sketchDone = true;
        m_terrainText.clear();
        m_terrainDetails.clear();
        m_pointValues.clear();
        if (m_tool == QLatin1String("height")) computeHeight(map);
        else computeViewshed(map);
        updateMeasurement();
        emit repaint();
    } else if (m_tool == QLatin1String("aiSelect")) {
        const QString error = aiSelectAt(map, m_pressModifiers.testFlag(Qt::ShiftModifier),
                                         m_pressModifiers.testFlag(Qt::ControlModifier) || m_pressModifiers.testFlag(Qt::AltModifier));
        if (!error.isEmpty()) {
            m_aiStatus = error;
            emit aiChanged();
        }
    } else if (m_tool == QLatin1String("marker")) {
        addMarker(map);
    } else if (m_tool == QLatin1String("georef")) {
        emit georefClicked(map);
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
    if (m_tool == QLatin1String("aiSelect")) return;   // the first click already asked the model
    finishSketch();
}

void MapService::finishSketch()
{
    if (m_tool == QLatin1String("aiSelect")) {
        acceptAiCandidate();
        return;
    }
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
        if (m_tool == QLatin1String("profile")) computeProfile(m_sketch);
        else if (m_tool == QLatin1String("volume")) computeVolume(m_sketch);
        else if (m_tool == QLatin1String("crossProfile")) computeCrossSections(m_sketch);
        else if (m_tool == QLatin1String("slope") && m_sketch.size() >= 2) computeSlope({m_sketch.first(), m_sketch.last()});
        else if (m_tool == QLatin1String("angle") && m_sketch.size() >= 3) computeAngle(m_sketch);
    }
    updateMeasurement();
    emit repaint();
}

bool MapService::keyPressed(int key)
{
    if (key == Qt::Key_Escape) {
        if (m_tool == QLatin1String("georef")) emit georefCancelled();
        if (!m_aiCandidate.isEmpty() || m_aiBusy) clearAiCandidate();
        m_sketch.clear();
        m_sketchDone = false;
        m_terrainText.clear();
        m_terrainDetails.clear();
        if (hasSelection()) {
            m_selectedFeature = -1;
            m_multiSelection.clear();
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
    if (key == Qt::Key_Delete && hasSelection()) {
        deleteSelected();
        return true;
    }
    return false;
}

void MapService::setOverlay(const MapOverlay *overlay)
{
    m_overlay = overlay;
    emit repaint();
}

void MapService::setTool(const QString &tool)
{
    if (tool == m_tool) return;
    m_tool = tool;
    m_sketch.clear();
    m_sketchDone = false;
    m_terrainText.clear();
    m_terrainDetails.clear();
    ++m_terrainGeneration;
    m_pointValues.clear();
    if (!m_cross.isEmpty()) {
        m_cross.clear();
        m_crossLines.clear();
        m_crossIndex = 0;
        emit crossChanged();
    }
    if (!m_aiCandidate.isEmpty() || m_aiBusy || !m_aiStatus.isEmpty()) clearAiCandidate();
    if (tool != QLatin1String("edit") && hasSelection()) {
        m_selectedFeature = -1;
        m_multiSelection.clear();
        emit drawingsChanged();
    }
    updateMeasurement();
    emit toolChanged();
    emit repaint();
}

void MapService::addDrawing(const VectorFeature &feature, const QString &undoText)
{
    auto *cmd = new DrawingCommand(this, DrawingCommand::Add, undoText);
    cmd->feature = feature;
    m_undo.push(cmd);
}

void MapService::deleteSelected()
{
    VectorData *d = drawings();
    QVector<int> indices = selectedIndices();
    if (!d || indices.isEmpty()) return;
    // Highest index first, so the positions of the others stay valid; one undo step.
    std::sort(indices.begin(), indices.end(), std::greater<int>());
    if (indices.size() > 1) m_undo.beginMacro(QStringLiteral("Удаление объектов: %1").arg(indices.size()));
    const QVector<VectorFeature> features = d->features;
    m_multiSelection.clear();
    for (int i : std::as_const(indices)) {
        auto *cmd = new DrawingCommand(this, DrawingCommand::Remove, QStringLiteral("Удаление объекта"));
        cmd->feature = features[i];
        cmd->index = i;
        m_undo.push(cmd);
    }
    if (indices.size() > 1) m_undo.endMacro();
}

void MapService::clearSelection()
{
    if (!hasSelection()) return;
    m_selectedFeature = -1;
    m_multiSelection.clear();
    emit drawingsChanged();
    emit repaint();
}

QString MapService::layerIdForSource(const QString &path) const
{
#ifdef Q_OS_WIN
    constexpr Qt::CaseSensitivity cs = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity cs = Qt::CaseSensitive;
#endif
    const QString wanted = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    for (const MapLayer &l : m_layers)
        if (!l.source.isEmpty() && QDir::cleanPath(QFileInfo(l.source).absoluteFilePath()).compare(wanted, cs) == 0)
            return l.id;
    return {};
}

void MapService::clearMeasurement()
{
    m_sketch.clear();
    m_sketchDone = false;
    m_terrainText.clear();
    m_terrainDetails.clear();
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
    if (!m_multiSelection.isEmpty()) {   // drop the ids of drawings that are gone
        QSet<qint64> present;
        if (const VectorData *d = drawings())
            for (const VectorFeature &f : d->features) if (m_multiSelection.contains(f.id)) present.insert(f.id);
        m_multiSelection = present;
    }
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

double MapService::polygonArea(const VectorFeature &f) const
{
    double area = 0;
    for (int k = 0; k < f.parts.size(); ++k) {
        const double a = measure(QVector<QPointF>(f.parts[k].begin(), f.parts[k].end()), true).area;
        area += k == 0 ? a : -a;
    }
    return qMax(0.0, area);
}

// ----------------------------------------------------------- AI selection

QString MapService::aiSelectAt(const QPointF &mapPoint, bool add, bool exclude)
{
    if (!m_crs.isValid()) return QStringLiteral("Сначала задайте систему координат карты.");
    if (m_aiBusy) return QStringLiteral("ИИ ещё обрабатывает предыдущий щелчок.");
    if (!m_ai) return QStringLiteral("Локальный AI не подключён.");
    const QString problem = m_ai->segmentationProblem();
    if (!problem.isEmpty()) return problem;
    // Shift/Ctrl refine the candidate while the click is inside the image the model has seen.
    const bool refine = (add || exclude) && !m_aiCandidate.isEmpty() && m_aiWindow.contains(mapPoint) && indexOf(m_aiLayerId) >= 0;
    if (exclude && !refine) return QStringLiteral("Сначала выделите объект щелчком, затем исключайте лишнее Ctrl+щелчком.");
    int index = -1;
    if (refine) {
        index = indexOf(m_aiLayerId);
        m_aiPoints << mapPoint;
        m_aiLabels << (exclude ? 0 : 1);
    } else {
        // The top visible colour raster under the click (an orthophoto or a background image).
        for (int i = int(m_layers.size()) - 1; i >= 0 && index < 0; --i) {
            const MapLayer &l = m_layers[i];
            if (l.visible && l.type == MapLayer::Raster && l.raster && l.raster->isValid() && l.style.mode == RasterStyle::Rgb
                && l.raster->mapExtent().contains(mapPoint))
                index = i;
        }
        if (index < 0) return QStringLiteral("Под щелчком нет видимого ортофотоплана или другого цветного растра.");
        const QSizeF span(m_viewSize.width() * m_resolution, m_viewSize.height() * m_resolution);
        const QRectF view(m_center.x() - span.width() / 2, m_center.y() - span.height() / 2, span.width(), span.height());
        const QRectF window = view.intersected(m_layers[index].raster->mapExtent());
        if (window.width() < 16 * m_resolution || window.height() < 16 * m_resolution)
            return QStringLiteral("В окне карты слишком маленький участок растра: приблизьте его.");
        const double longest = qMax(window.width(), window.height());
        m_aiImageSize = QSize(qMax(16, int(std::lround(1024 * window.width() / longest))), qMax(16, int(std::lround(1024 * window.height() / longest))));
        m_aiWindow = window;
        m_aiLayerId = m_layers[index].id;
        m_aiPoints = {mapPoint};
        m_aiLabels = {1};
        m_aiCandidate.clear();
        m_aiText.clear();
        m_aiDetails.clear();
    }
    runAiSelection(index);
    return {};
}

void MapService::runAiSelection(int index)
{
    const MapLayer &l = m_layers[index];
    const ai::ModelCard card = m_ai->segmentationCard();
    const QRectF window = m_aiWindow;
    const QSize size = m_aiImageSize;
    // What the shared segmenter must have encoded for these prompts.
    const QString key = QStringLiteral("%1|%2|%3|%4 %5 %6 %7|%8x%9")
                            .arg(l.source, QString::number(l.version), card.directory)
                            .arg(window.x(), 0, 'g', 17).arg(window.y(), 0, 'g', 17).arg(window.width(), 0, 'g', 17).arg(window.height(), 0, 'g', 17)
                            .arg(size.width()).arg(size.height());
    const bool encode = key != m_aiImageKey;
    QVector<QPointF> pixels;   // row 0 = top (north) of the window
    for (const QPointF &p : std::as_const(m_aiPoints))
        pixels << QPointF((p.x() - window.left()) / window.width() * size.width(), (window.bottom() - p.y()) / window.height() * size.height());
    const QVector<int> labels = m_aiLabels;
    const int generation = ++m_aiGeneration;
    const std::shared_ptr<RasterSource> source = l.raster;
    m_aiLayerName = l.name;
    m_aiModelName = card.name.section(QStringLiteral(" — "), 0, 0);   // "MobileSAM"
    m_aiBusy = true;
    m_aiStatus = encode ? QStringLiteral("ИИ: анализ изображения (%1)…").arg(m_aiModelName) : QStringLiteral("ИИ: построение контура…");
    emit aiChanged();
    QThreadPool::globalInstance()->start([this, card, source, window, size, key, encode, pixels, labels, generation] {
        QElapsedTimer timer;
        timer.start();
        QString error;
        bool encoded = false;
        ai::SamSegmenter::Result result;
        QVector<ai::MaskPolygon> polygons;
        std::shared_ptr<ai::SamSegmenter> segmenter = ai::cachedSegmenter(card, &error);
        // The segmenter is shared (photo masks use it too): held while it encodes and decodes,
        // and re-encoded only when it holds another image.
        std::unique_lock<std::mutex> lock;
        if (segmenter) lock = std::unique_lock<std::mutex>(segmenter->mutex());
        if (segmenter && segmenter->imageKey() != key) {
            encoded = segmenter->setImage(source->renderRegion(window, size), &error, key);
            if (!encoded) segmenter.reset();
        } else if (segmenter) {
            encoded = true;
        }
        if (segmenter) {
            result = segmenter->segment(pixels, labels);
            error = result.error;
            if (result.ok()) polygons = ai::maskPolygons(result.mask, &error);
        }
        const double seconds = timer.elapsed() / 1000.0;
        const float score = result.score;
        QMetaObject::invokeMethod(this, [this, key, encode, encoded, error, polygons, pixels, labels, window, size, generation, seconds, score] {
            m_aiBusy = false;
            m_aiImageKey = encoded ? key : QString();
            if (generation != m_aiGeneration) {   // cleared, another tool or another project meanwhile
                m_aiStatus.clear();
                emit aiChanged();
                return;
            }
            // The object: the largest polygon that contains a click on the object.
            int best = -1;
            for (int i = 0; i < polygons.size(); ++i) {
                bool hit = false;
                for (int j = 0; j < pixels.size() && !hit; ++j) hit = labels[j] == 1 && polygons[i].contains(pixels[j]);
                if (hit && (best < 0 || polygons[i].area > polygons[best].area)) best = i;
            }
            if (!error.isEmpty() || best < 0) {
                m_aiStatus = !error.isEmpty() ? QStringLiteral("ИИ: %1").arg(error)
                                              : QStringLiteral("ИИ не выделил объект под щелчком. Щёлкните ближе к центру объекта или приблизьте карту.");
                // The failed prompt is dropped; a refinement keeps the previous candidate.
                if (!m_aiPoints.isEmpty()) {
                    m_aiPoints.removeLast();
                    m_aiLabels.removeLast();
                }
                if (m_aiCandidate.isEmpty()) {
                    m_aiPoints.clear();
                    m_aiLabels.clear();
                }
                emit aiChanged();
                emit aiFinished(false, m_aiStatus);
                emit repaint();
                return;
            }
            // Pixel edges → map. SAM's masks are upsampled from a quarter of the image, so their
            // edges wobble by a pixel or two: simplified with a tolerance of 1.5 px.
            auto toMap = [&](const QPolygonF &ring) {
                QPolygonF out;
                for (const QPointF &p : ai::simplifyRing(ring, 1.5))
                    out << QPointF(window.left() + p.x() * window.width() / size.width(), window.bottom() - p.y() * window.height() / size.height());
                return out;
            };
            const ai::MaskPolygon &object = polygons[best];
            m_aiCandidate = {toMap(object.outer)};
            for (const QPolygonF &hole : object.holes)   // specks inside the object are not holes
                if (std::fabs(shoelace(QVector<QPointF>(hole.begin(), hole.end()))) >= qMax(16.0, object.area * 0.01)) m_aiCandidate << toMap(hole);
            m_aiScore = score;
            m_aiStatus = QStringLiteral("ИИ: контур готов (%1 с)").arg(number(seconds, 1));
            setAiText();
            updateMeasurement();
            emit aiChanged();
            emit aiFinished(true, m_aiText);
            emit repaint();
        });
    });
}

void MapService::setAiText()
{
    VectorFeature f;
    f.kind = VectorFeature::Polygon;
    f.parts = m_aiCandidate;
    const Measure m = measure(QVector<QPointF>(m_aiCandidate.first().begin(), m_aiCandidate.first().end()), true);
    int positive = 0;
    for (int label : std::as_const(m_aiLabels)) positive += label == 1;
    m_aiText = QStringLiteral("Объект ИИ: площадь %1, периметр %2").arg(formatArea(polygonArea(f)), formatLength(m.perimeter));
    m_aiDetails = QStringLiteral("%1 · оценка модели %2 · растр «%3» · подсказок: %4 на объекте, %5 исключено. "
                                 "Enter — добавить на карту, Shift+щелчок — добавить часть, Ctrl+щелчок — исключить, Esc — сбросить.")
                      .arg(m_aiModelName, number(m_aiScore, 2), m_aiLayerName)
                      .arg(positive).arg(m_aiLabels.size() - positive);
}

QString MapService::acceptAiCandidate()
{
    if (m_aiCandidate.isEmpty() || m_aiBusy) return {};
    VectorFeature f;
    f.kind = VectorFeature::Polygon;
    f.id = m_nextFeatureId++;
    f.parts = m_aiCandidate;
    f.bounds = m_aiCandidate.first().boundingRect();
    const QString name = QStringLiteral("ИИ-объект %1").arg(f.id);
    f.attributes.insert(QStringLiteral("name"), name);
    f.attributes.insert(QStringLiteral("method"), QStringLiteral("ИИ: %1, выделение по щелчку").arg(m_aiModelName));
    f.attributes.insert(QStringLiteral("ai_score"), std::round(m_aiScore * 1000.0) / 1000.0);
    f.attributes.insert(QStringLiteral("source_layer"), m_aiLayerName);
    auto *cmd = new DrawingCommand(this, DrawingCommand::Add, QStringLiteral("Добавление объекта ИИ"));
    cmd->feature = f;
    m_undo.push(cmd);
    m_aiCandidate.clear();
    m_aiPoints.clear();
    m_aiLabels.clear();
    m_aiText.clear();
    m_aiDetails.clear();
    m_aiStatus = QStringLiteral("Добавлено на карту: %1").arg(name);
    updateMeasurement();
    emit aiChanged();
    emit repaint();
    return name;
}

void MapService::rasterEdited(const QString &path)
{
    const QString wanted = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    for (int i = 0; i < m_layers.size(); ++i) {
        MapLayer &l = m_layers[i];
        if (l.type != MapLayer::Raster || QDir::cleanPath(QFileInfo(l.source).absoluteFilePath()).compare(wanted, Qt::CaseInsensitive) != 0) continue;
        if (l.raster) l.raster->reload();
        ++l.version;
        changed(i);
    }
    emit repaint();
}

QString MapService::removeFromOrthophoto(double growMeters)
{
    if (m_project.isEmpty() || !m_crs.isValid()) return QStringLiteral("Откройте проект с картой.");
    if (m_aiBusy) return QStringLiteral("ИИ ещё работает.");
    if (!m_ai) return QStringLiteral("Локальный AI не подключён.");
    const QString problem = m_ai->inpaintingProblem();
    if (!problem.isEmpty()) return problem;
    // The outline: the AI proposal, else the selected drawn polygon.
    QVector<QPolygonF> rings = m_aiCandidate;
    const bool fromCandidate = !rings.isEmpty();
    if (!fromCandidate) {
        const VectorData *d = drawings();
        if (d && m_selectedFeature >= 0 && m_selectedFeature < d->features.size() && d->features[m_selectedFeature].kind == VectorFeature::Polygon)
            rings = d->features[m_selectedFeature].parts;
    }
    if (rings.isEmpty() || rings.first().size() < 3)
        return QStringLiteral("Выделите объект: щелчком инструмента «ИИ-выделение» или выберите нарисованный полигон.");
    QRectF bounds = rings.first().boundingRect();
    int index = fromCandidate ? indexOf(m_aiLayerId) : -1;
    for (int i = int(m_layers.size()) - 1; i >= 0 && index < 0; --i) {
        const MapLayer &l = m_layers[i];
        if (l.visible && l.type == MapLayer::Raster && l.raster && l.raster->isValid() && l.style.mode == RasterStyle::Rgb
            && l.raster->mapExtent().intersects(bounds))
            index = i;
    }
    if (index < 0) return QStringLiteral("Под объектом нет ортофотоплана.");
    const MapLayer &ortho = m_layers[index];
    const RasterInfo info = ortho.rasterInfo;
    if (info.dataType != QLatin1String("Byte") || info.bands < 3) return QStringLiteral("Удалять объекты можно только с цветного 8-битного растра.");
    const double *gt = info.geoTransform;
    if (gt[2] != 0 || gt[4] != 0) return QStringLiteral("Повёрнутые растры пока не правятся.");
    // The outline in pixels of the file.
    const Crs layerCrs = ortho.assignedCrs.isValid() ? ortho.assignedCrs : info.crs;
    CoordinateTransform toLayer(m_crs, layerCrs);
    QVector<QPolygonF> pixelRings;
    for (const QPolygonF &ring : std::as_const(rings)) {
        QVector<QPointF> pts(ring.begin(), ring.end());
        if (!toLayer.transform(pts)) return QStringLiteral("Не удалось пересчитать контур в систему координат растра.");
        QPolygonF px;
        for (const QPointF &p : pts) px << QPointF((p.x() - gt[0]) / gt[1], (p.y() - gt[3]) / gt[5]);
        pixelRings << px;
    }
    // Size of a pixel on the ground, for the growth in metres.
    const QPointF c = pixelRings.first().boundingRect().center();
    QVector<QPointF> probe{QPointF(gt[0] + c.x() * gt[1], gt[3] + c.y() * gt[5]), QPointF(gt[0] + (c.x() + 1) * gt[1], gt[3] + c.y() * gt[5])};
    CoordinateTransform(layerCrs, m_crs).transform(probe);
    const double pixelMeters = qMax(1e-6, measure(probe, false).length);
    const double growPixels = qMax(0.0, growMeters) / pixelMeters;
    // Edits go to the program's own copy of the raster.
    const QString analysis = QDir(m_project).filePath(QStringLiteral("analysis"));
    const bool inPlace = QDir::cleanPath(QFileInfo(ortho.source).absolutePath()).compare(QDir::cleanPath(analysis), Qt::CaseInsensitive) == 0
                         && QFileInfo(ortho.source).completeBaseName().contains(QStringLiteral("_ИИ-правка"));
    QString target = ortho.source;
    if (!inPlace) {
        const QString base = QFileInfo(ortho.source).completeBaseName() + QStringLiteral("_ИИ-правка");
        target = QDir(analysis).filePath(base + QStringLiteral(".tif"));
        for (int n = 2; QFileInfo::exists(target); ++n) target = QDir(analysis).filePath(QStringLiteral("%1 (%2).tif").arg(base).arg(n));
    }
    const ai::ModelCard card = m_ai->inpaintingCard();
    const QString source = ortho.source, layerId = ortho.id, layerName = ortho.name;
    const QString sourceMask = stretchMaskOf(ortho);   // the copy keeps it, for the retouch
    QVector<QPointF> outlineWgs(rings.first().begin(), rings.first().end());
    CoordinateTransform(m_crs, Crs::wgs84()).transform(outlineWgs);
    const double area = [&] {
        VectorFeature f;
        f.kind = VectorFeature::Polygon;
        f.parts = rings;
        return polygonArea(f);
    }();
    ++m_aiGeneration;
    m_aiBusy = true;
    m_aiStatus = inPlace ? QStringLiteral("ИИ: удаление объекта с ортофотоплана…") : QStringLiteral("Подготовка копии ортофотоплана для правки…");
    emit aiChanged();
    const QSize rasterSize(info.width, info.height);
    QThreadPool::globalInstance()->start([this, card, source, target, inPlace, pixelRings, growPixels, rasterSize, layerId, layerName,
                                          fromCandidate, outlineWgs, area, sourceMask] {
        QString error;
        QRect window;
        QImage before;
        ai::Inpainter::Result result;
        int pixels = 0;
        QElapsedTimer timer;
        timer.start();
        if (!inPlace && !rasteredit::makeEditableCopy(source, target, &error)) {}
        else {
            // The object and enough surroundings for the model's 512 px context.
            QRectF pb;
            for (const QPolygonF &r : pixelRings) pb = pb.isNull() ? r.boundingRect() : pb.united(r.boundingRect());
            const QRect box = pb.toAlignedRect().adjusted(-int(growPixels) - 1, -int(growPixels) - 1, int(growPixels) + 1, int(growPixels) + 1);
            const int margin = qMax(256, qMin(1500, qMax(box.width(), box.height())));
            window = box.adjusted(-margin, -margin, margin, margin).intersected(QRect(QPoint(0, 0), rasterSize));
            QImage alpha;
            if (window.isEmpty()) error = QStringLiteral("Контур вне растра");
            else if (rasteredit::readRgb(target, window, &before, &alpha, &error)) {
                QImage mask(window.size(), QImage::Format_Grayscale8);
                mask.fill(0);
                {
                    QPainter p(&mask);
                    p.translate(-window.topLeft());
                    QPainterPath path;
                    path.setFillRule(Qt::OddEvenFill);
                    for (const QPolygonF &r : pixelRings) {
                        path.addPolygon(r);
                        path.closeSubpath();
                    }
                    p.setPen(growPixels > 0.5 ? QPen(Qt::white, 2 * growPixels, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin) : QPen(Qt::NoPen));
                    p.setBrush(Qt::white);
                    p.drawPath(path);
                }
                for (int y = 0; y < mask.height(); ++y) {   // nodata is not painted over
                    uchar *m = mask.scanLine(y);
                    const uchar *a = alpha.constScanLine(y);
                    for (int x = 0; x < mask.width(); ++x) {
                        if (!a[x]) m[x] = 0;
                        pixels += m[x] > 0;
                    }
                }
                std::shared_ptr<ai::Inpainter> inpainter = pixels ? ai::cachedInpainter(card, &error) : nullptr;
                if (!pixels) error = QStringLiteral("Контур не задевает данных ортофотоплана");
                else if (inpainter) {
                    result = inpainter->inpaint(before, mask);
                    if (!result.ok()) error = result.error;
                    else rasteredit::writeRgb(target, window, result.image, &error);
                }
            }
        }
        const double seconds = timer.elapsed() / 1000.0;
        QMetaObject::invokeMethod(this, [this, card, target, inPlace, window, before, result, pixels, error, layerId, layerName, fromCandidate,
                                         outlineWgs, area, seconds, sourceMask] {
            m_aiBusy = false;
            if (!error.isEmpty()) {
                m_aiStatus = QStringLiteral("ИИ: %1").arg(error);
                emit aiChanged();
                emit aiFinished(false, m_aiStatus);
                return;
            }
            if (!inPlace) {
                // The copy replaces the original on the map: right above it, the original hidden.
                const QString added = addLayerFromPath(target, false);
                const QString copyId = layerIdForSource(target);
                if (!added.isEmpty() || copyId.isEmpty()) {
                    m_aiStatus = QStringLiteral("ИИ: правка сохранена в %1, но слой не добавлен: %2").arg(QDir::toNativeSeparators(target), added);
                    emit aiChanged();
                    emit aiFinished(false, m_aiStatus);
                    return;
                }
                if (MapLayer *copy = layer(copyId)) copy->name = QStringLiteral("%1 (правка ИИ)").arg(layerName);
                while (indexOf(copyId) > indexOf(layerId) + 1) moveLayer(copyId, -1);
                setLayerVisible(layerId, false);
                selectLayer(copyId);
                changed(indexOf(copyId));
                scheduleSave();
            } else {
                rasterEdited(target);
            }
            m_undo.push(new RasterPatchCommand(this, target, window, before, result.image));
            // What was painted by the model, for whoever uses the raster later.
            QFile log(target + QStringLiteral(".edits.json"));
            QJsonObject root;
            if (log.open(QIODevice::ReadOnly)) root = QJsonDocument::fromJson(log.readAll()).object();
            log.close();
            QJsonArray edits = root.value(QStringLiteral("edits")).toArray();
            if (!sourceMask.isEmpty() && !root.contains(QStringLiteral("stretchMask"))) root.insert(QStringLiteral("stretchMask"), sourceMask);
            QJsonArray outline;
            for (const QPointF &p : outlineWgs) outline.append(QJsonArray{p.x(), p.y()});
            edits.append(QJsonObject{{"time", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                                     {"method", QStringLiteral("удаление объекта: дорисовано ИИ (%1)").arg(card.name.section(QStringLiteral(" — "), 0, 0))},
                                     {"model", card.id}, {"modelSha256", card.files.value(0).sha256},
                                     {"window", QJsonArray{window.x(), window.y(), window.width(), window.height()}},
                                     {"pixels", pixels}, {"areaM2", std::round(area * 100) / 100}, {"outlineWgs84", outline}});
            root.insert(QStringLiteral("note"), QStringLiteral("Пиксели этих правок дорисованы моделью по окружению; это не съёмка."));
            root.insert(QStringLiteral("edits"), edits);
            if (log.open(QIODevice::WriteOnly | QIODevice::Truncate)) log.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
            if (fromCandidate) {
                m_aiCandidate.clear();
                m_aiPoints.clear();
                m_aiLabels.clear();
                m_aiText.clear();
                m_aiDetails.clear();
                updateMeasurement();
            }
            m_aiStatus = QStringLiteral("Объект убран с ортофотоплана: %1 пикс. дорисовано за %2 с%3")
                             .arg(pixels).arg(number(seconds, 1))
                             .arg(result.scaled ? QStringLiteral(" (крупная область — дорисована в уменьшенном окне)") : QString());
            emit aiChanged();
            emit aiFinished(true, m_aiStatus);
            emit repaint();
        });
    });
    return {};
}

QString MapService::stretchMaskOf(const MapLayer &l) const
{
    if (l.type != MapLayer::Raster) return {};
    const QString next = ai::stretchMaskFor(l.source);
    if (!next.isEmpty()) return next;
    QFile log(l.source + QStringLiteral(".edits.json"));
    if (!log.open(QIODevice::ReadOnly)) return {};
    const QString recorded = QJsonDocument::fromJson(log.readAll()).object().value(QStringLiteral("stretchMask")).toString();
    return !recorded.isEmpty() && QFileInfo::exists(recorded) ? recorded : QString();
}

QString MapService::retouchLayerId() const
{
    auto fits = [this](const MapLayer &l) {
        return l.type == MapLayer::Raster && l.raster && l.raster->isValid() && l.style.mode == RasterStyle::Rgb && !stretchMaskOf(l).isEmpty();
    };
    const int selected = indexOf(m_selectedLayer);
    if (selected >= 0 && fits(m_layers[selected])) return m_layers[selected].id;
    for (int i = int(m_layers.size()) - 1; i >= 0; --i)
        if (m_layers[i].visible && fits(m_layers[i])) return m_layers[i].id;
    return {};
}

QString MapService::retouchOrthophoto()
{
    if (m_project.isEmpty()) return QStringLiteral("Откройте проект с картой.");
    if (m_aiBusy) return QStringLiteral("ИИ ещё работает.");
    if (!m_ai) return QStringLiteral("Локальный AI не подключён.");
    const QString problem = m_ai->inpaintingProblem();
    if (!problem.isEmpty()) return problem;
    const int index = indexOf(retouchLayerId());
    if (index < 0)
        return QStringLiteral("На карте нет истинного ортофотоплана с маской растянутых участков. Постройте его (Обработка → Истинный ортофотоплан) "
                              "и покажите на карте: «Вид → Ортофотоплан».");
    const MapLayer &ortho = m_layers[index];
    if (ortho.rasterInfo.dataType != QLatin1String("Byte") || ortho.rasterInfo.bands < 3)
        return QStringLiteral("Ретушь работает с цветным 8-битным растром.");
    const QString mask = stretchMaskOf(ortho);
    const QString analysis = QDir(m_project).filePath(QStringLiteral("analysis"));
    const bool inPlace = QDir::cleanPath(QFileInfo(ortho.source).absolutePath()).compare(QDir::cleanPath(analysis), Qt::CaseInsensitive) == 0
                         && QFileInfo(ortho.source).completeBaseName().contains(QStringLiteral("_ИИ-"));
    QString target = ortho.source;
    if (!inPlace) {
        const QString base = QFileInfo(ortho.source).completeBaseName() + QStringLiteral("_ИИ-ретушь");
        target = QDir(analysis).filePath(base + QStringLiteral(".tif"));
        for (int n = 2; QFileInfo::exists(target); ++n) target = QDir(analysis).filePath(QStringLiteral("%1 (%2).tif").arg(base).arg(n));
    }
    const ai::ModelCard card = m_ai->inpaintingCard();
    const QString source = ortho.source, layerId = ortho.id, layerName = ortho.name;
    const double pixelArea = std::fabs(ortho.rasterInfo.geoTransform[1] * ortho.rasterInfo.geoTransform[5]);
    ++m_aiGeneration;
    m_aiBusy = true;
    m_aiStatus = QStringLiteral("ИИ-ретушь: подготовка копии ортофотоплана…");
    emit aiChanged();
    QThreadPool::globalInstance()->start([this, card, source, target, inPlace, mask, layerId, layerName, pixelArea] {
        QString error;
        ai::RetouchResult result;
        QElapsedTimer timer;
        timer.start();
        if (inPlace || rasteredit::makeEditableCopy(source, target, &error)) {
            std::shared_ptr<ai::Inpainter> inpainter = ai::cachedInpainter(card, &error);
            if (inpainter) {
                int last = -1;
                result = ai::retouch(*inpainter, target, mask, 400, [this, &last](double v) {
                    const int percent = int(v * 100);
                    if (percent != last) {
                        last = percent;
                        QMetaObject::invokeMethod(this, [this, percent] {
                            m_aiStatus = QStringLiteral("ИИ-ретушь растянутых участков: %1 %").arg(percent);
                            emit aiChanged();
                        });
                    }
                    return true;
                });
                if (!result.ok()) error = result.error;
            }
        }
        const double seconds = timer.elapsed() / 1000.0;
        QMetaObject::invokeMethod(this, [this, card, target, inPlace, mask, result, error, layerId, layerName, pixelArea, seconds] {
            m_aiBusy = false;
            if (!error.isEmpty()) {
                if (!inPlace) QFile::remove(target);
                m_aiStatus = QStringLiteral("ИИ-ретушь: %1").arg(error);
                emit aiChanged();
                emit aiFinished(false, m_aiStatus);
                return;
            }
            if (!inPlace) {
                const QString added = addLayerFromPath(target, false);
                const QString copyId = layerIdForSource(target);
                if (!added.isEmpty() || copyId.isEmpty()) {
                    m_aiStatus = QStringLiteral("ИИ-ретушь сохранена в %1, но слой не добавлен: %2").arg(QDir::toNativeSeparators(target), added);
                    emit aiChanged();
                    emit aiFinished(false, m_aiStatus);
                    return;
                }
                if (MapLayer *copy = layer(copyId)) copy->name = QStringLiteral("%1 (ретушь ИИ)").arg(layerName);
                while (indexOf(copyId) > indexOf(layerId) + 1) moveLayer(copyId, -1);
                setLayerVisible(layerId, false);
                selectLayer(copyId);
                changed(indexOf(copyId));
                scheduleSave();
            } else {
                rasterEdited(target);
            }
            QFile log(target + QStringLiteral(".edits.json"));
            QJsonObject root;
            if (log.open(QIODevice::ReadOnly)) root = QJsonDocument::fromJson(log.readAll()).object();
            log.close();
            QJsonArray edits = root.value(QStringLiteral("edits")).toArray();
            edits.append(QJsonObject{{"time", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                                     {"method", QStringLiteral("ретушь растянутых участков: дорисовано ИИ (%1)").arg(card.name.section(QStringLiteral(" — "), 0, 0))},
                                     {"model", card.id}, {"modelSha256", card.files.value(0).sha256}, {"mask", QDir::toNativeSeparators(mask)},
                                     {"pixels", double(result.pixels)}, {"areaM2", std::round(result.pixels * pixelArea * 100) / 100}});
            root.insert(QStringLiteral("note"), QStringLiteral("Пиксели этих правок дорисованы моделью по окружению; это не съёмка."));
            root.insert(QStringLiteral("stretchMask"), mask);
            root.insert(QStringLiteral("edits"), edits);
            if (log.open(QIODevice::WriteOnly | QIODevice::Truncate)) log.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
            m_aiStatus = result.pixels == 0 ? QStringLiteral("ИИ-ретушь: растянутых участков крупнее 400 пикс. нет — дорисовывать нечего")
                                            : QStringLiteral("ИИ-ретушь: дорисовано %1 м² (%2 пикс.) за %3 с; исходный ортофотоплан не изменён и скрыт")
                                                  .arg(number(result.pixels * pixelArea, 1)).arg(result.pixels).arg(number(seconds, 0));
            emit aiChanged();
            emit aiFinished(true, m_aiStatus);
            emit repaint();
        });
    });
    return {};
}

void MapService::clearAiCandidate()
{
    ++m_aiGeneration;   // a running selection is dropped when it finishes
    m_aiCandidate.clear();
    m_aiPoints.clear();
    m_aiLabels.clear();
    m_aiText.clear();
    m_aiDetails.clear();
    if (!m_aiBusy) m_aiStatus.clear();
    updateMeasurement();
    emit aiChanged();
    emit repaint();
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
        closed = m_tool == QLatin1String("area") || m_tool == QLatin1String("polygon") || m_tool == QLatin1String("volume");
    } else if (m_selectedFeature >= 0 && m_selectedFeature < drawingCount()) {
        const VectorFeature &f = m_layers[drawingsIndex()].vector.features[m_selectedFeature];
        if (!f.parts.isEmpty()) pts = QVector<QPointF>(f.parts.first().begin(), f.parts.first().end());
        closed = f.kind == VectorFeature::Polygon;
        what = f.attributes.value(QStringLiteral("name")).toString() + QStringLiteral(": ");
    }
    const QString ellipsoid = m_crs.isValid() ? m_crs.geographic().name() : QString();
    if (m_tool == QLatin1String("aiSelect") && m_selectedFeature < 0) {
        m_measurementText = !m_aiText.isEmpty() ? m_aiText : QStringLiteral("Щёлкните по объекту на ортофотоплане — ИИ обведёт его контур");
        m_measurementDetails = m_aiDetails;
        emit measurementChanged();
        return;
    }
    const bool terrainTool = isTerrainTool();
    if (terrainTool && m_sketchDone && !m_terrainText.isEmpty()) {
        m_measurementText = m_terrainText;
        m_measurementDetails = m_terrainDetails;
        emit measurementChanged();
        return;
    }
    if (pts.size() < 2 || m_tool == QLatin1String("height") || m_tool == QLatin1String("viewshed")) {
        m_measurementText = m_tool == QLatin1String("length") ? QStringLiteral("Щёлкайте по карте, чтобы измерить длину")
                          : m_tool == QLatin1String("area") ? QStringLiteral("Щёлкайте по карте, чтобы измерить площадь")
                          : m_tool == QLatin1String("profile") ? QStringLiteral("Проведите линию профиля по модели высот")
                          : m_tool == QLatin1String("volume") ? QStringLiteral("Обведите участок для расчёта объёма")
                          : m_tool == QLatin1String("crossProfile") ? QStringLiteral("Проведите ось (Enter — готово): профили поперёк неё через заданный шаг")
                          : m_tool == QLatin1String("slope") ? QStringLiteral("Щёлкните две точки: уклон и перепад высот между ними")
                          : m_tool == QLatin1String("height") ? QStringLiteral("Щёлкните точку: её высота по моделям высот")
                          : m_tool == QLatin1String("angle") ? QStringLiteral("Щёлкните три точки: угол при второй")
                          : m_tool == QLatin1String("viewshed") ? QStringLiteral("Щёлкните точку наблюдения: что видно с неё по модели поверхности")
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
            f.attributes.insert(QStringLiteral("area_m2"), toMillimetres(polygonArea(f)));
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

// ----------------------------------------------------------------- terrain analysis

namespace {
QString volumeText(double m3)
{
    const QLocale ru(QLocale::Russian);
    return ru.toString(m3, 'f', m3 >= 1000 ? 0 : 1) + QStringLiteral(" м³");
}
QString heightText(double m) { return QLocale(QLocale::Russian).toString(m, 'f', 2) + QStringLiteral(" м"); }
// File name part from a layer name: letters, digits, - and _ only.
QString safeName(const QString &name)
{
    QString s;
    for (const QChar c : name) s += (c.isLetterOrNumber() || c == QLatin1Char('-') || c == QLatin1Char('_')) ? c : QLatin1Char('_');
    return s.isEmpty() ? QStringLiteral("dem") : s;
}
QString uniquePath(const QString &dir, const QString &stem, const QString &suffix)
{
    QString path = QDir(dir).filePath(stem + suffix);
    for (int n = 2; QFileInfo::exists(path); ++n) path = QDir(dir).filePath(QStringLiteral("%1-%2%3").arg(stem).arg(n).arg(suffix));
    return path;
}
} // namespace

bool MapService::isElevation(const MapLayer &l) const
{
    return l.type == MapLayer::Raster && !l.basemap && l.rasterInfo.valid && l.rasterInfo.bands == 1 && l.raster && !l.needsCrs;
}

bool MapService::isDerived(const MapLayer &l) const
{
    if (m_project.isEmpty()) return false;
    const QString analysis = QDir::cleanPath(QDir(m_project).filePath(QStringLiteral("analysis")));
    const QString file = QFileInfo(l.source).fileName();
    return QDir::cleanPath(QFileInfo(l.source).absolutePath()).compare(analysis, Qt::CaseInsensitive) == 0
           && !file.contains(QStringLiteral("_правка")) && !file.contains(QStringLiteral("_заполнено"));
}

QVariantList MapService::elevationLayers() const
{
    QVariantList list;   // top first; the models of heights before the products of the analysis
    for (int pass = 0; pass < 2; ++pass)
        for (int i = int(m_layers.size()) - 1; i >= 0; --i) {
            const MapLayer &l = m_layers[i];
            if (!isElevation(l) || isDerived(l) != (pass == 1)) continue;
            list << QVariantMap{{"id", l.id}, {"name", l.name}, {"min", l.style.min}, {"max", l.style.max}, {"visible", l.visible},
                                {"derived", pass == 1}};
        }
    return list;
}

QString MapService::terrainLayerId() const
{
    // The selected model, else the top visible one — a model of heights before a product of the
    // analysis (a slope map or a viewshed just added on top is not what the tools should measure).
    const int selected = indexOf(m_selectedLayer);
    for (int pass = 0; pass < 2; ++pass) {
        const auto fits = [&](const MapLayer &l) { return isElevation(l) && (pass == 1 || !isDerived(l)); };
        if (selected >= 0 && fits(m_layers[selected])) return m_layers[selected].id;
        for (int i = int(m_layers.size()) - 1; i >= 0; --i)
            if (m_layers[i].visible && fits(m_layers[i])) return m_layers[i].id;
    }
    return {};
}

QVariantMap MapService::contourSuggestion(const QString &layerId) const
{
    const int i = indexOf(layerId);
    if (i < 0) return {};
    const MapLayer &l = m_layers[i];
    return {{"interval", terrain::suggestedInterval(l.style.min, l.style.max)}, {"base", 0.0}, {"min", l.style.min}, {"max", l.style.max}};
}

QString MapService::analyzeTerrain(const QString &layerId, const QString &kind, double interval, double base, const QString &otherLayerId)
{
    if (m_project.isEmpty()) return QStringLiteral("Откройте проект.");
    if (m_analysisBusy) return QStringLiteral("Уже идёт расчёт: %1").arg(m_analysisStatus);
    const int i = indexOf(layerId);
    if (i < 0 || !isElevation(m_layers[i]))
        return QStringLiteral("Выберите модель высот (одноканальный растр с системой координат) на карте.");
    const MapLayer &l = m_layers[i];
    const QString dir = QDir(m_project).filePath(QStringLiteral("analysis"));
    const QString stem = safeName(l.name);
    QString out, title;
    RasterStyle style;
    style.mode = RasterStyle::SingleBand;
    style.band = 1;
    if (kind == QLatin1String("slope") || kind == QLatin1String("slopePercent")) {
        const bool percent = kind == QLatin1String("slopePercent");
        out = uniquePath(dir, stem + (percent ? QStringLiteral("-slope-percent") : QStringLiteral("-slope")), QStringLiteral(".tif"));
        title = percent ? QStringLiteral("Карта уклонов (%)") : QStringLiteral("Карта уклонов (°)");
        style.ramp = RasterStyle::Slope;
        style.min = 0;
        style.max = percent ? 100 : 45;
    } else if (kind == QLatin1String("aspect")) {
        out = uniquePath(dir, stem + QStringLiteral("-aspect"), QStringLiteral(".tif"));
        title = QStringLiteral("Карта экспозиции склонов");
        style.ramp = RasterStyle::Aspect;
        style.min = 0;
        style.max = 360;
    } else if (kind == QLatin1String("difference")) {
        const int j = indexOf(otherLayerId);
        if (j < 0 || !isElevation(m_layers[j])) return QStringLiteral("Выберите вторую модель высот (B).");
        if (j == i) return QStringLiteral("Выберите две разные модели высот.");
        out = uniquePath(dir, stem + QStringLiteral("-minus-") + safeName(m_layers[j].name), QStringLiteral(".tif"));
        title = QStringLiteral("Разность «%1» − «%2»").arg(l.name, m_layers[j].name);
        style.ramp = RasterStyle::Spectral;   // symmetric range set from the result: zero yellow, higher red, lower blue
    } else if (kind == QLatin1String("contours")) {
        if (!(interval > 0)) return QStringLiteral("Укажите шаг горизонталей больше нуля.");
        const QString step = QString::number(interval, 'g', 6).replace(QLatin1Char('.'), QLatin1Char('_'));
        out = uniquePath(dir, stem + QStringLiteral("-contours-%1m").arg(step), QStringLiteral(".gpkg"));
        title = QStringLiteral("Горизонтали");
    } else {
        return QStringLiteral("Неизвестный вид анализа: %1").arg(kind);
    }
    m_analysisBusy = true;
    m_analysisStatus = QStringLiteral("%1 по «%2»…").arg(title, l.name);
    emit analysisChanged();
    const QString source = l.source, name = l.name;
    const QString other = kind == QLatin1String("difference") ? m_layers[indexOf(otherLayerId)].source : QString();
    QThreadPool::globalInstance()->start([this, kind, source, other, out, interval, base, title, name, style] {
        QString error, message;
        RasterStyle finalStyle = style;
        if (kind == QLatin1String("contours")) {
            const terrain::ContourResult r = terrain::contours(source, out, interval, base);
            error = r.error;
            if (r.ok())
                message = QStringLiteral("%1 по «%2»: %3 линий, шаг %4 м, уровни %5 – %6.")
                              .arg(title, name).arg(r.lines).arg(QLocale(QLocale::Russian).toString(interval))
                              .arg(heightText(r.minLevel), heightText(r.maxLevel));
            if (r.ok() && r.lines == 0) error = QStringLiteral("В пределах высот модели нет ни одного уровня с шагом %1 м").arg(interval);
        } else if (kind == QLatin1String("difference")) {
            const terrain::RasterResult r = terrain::difference(source, other, out);
            error = r.error;
            if (r.ok() && r.stats.valid) {
                const double span = qMax(0.1, qMax(std::fabs(r.stats.min), std::fabs(r.stats.max)));
                finalStyle.min = -span;
                finalStyle.max = span;
                const QLocale ru(QLocale::Russian);
                message = QStringLiteral("%1: от %2 до %3, в среднем %4 (положительные — A выше B).")
                              .arg(title, heightText(r.stats.min), heightText(r.stats.max), heightText(r.stats.mean));
            }
        } else {
            const terrain::RasterResult r = kind == QLatin1String("aspect") ? terrain::aspect(source, out)
                                                                            : terrain::slope(source, out, kind == QLatin1String("slopePercent"));
            error = r.error;
            if (r.ok() && r.stats.valid) {
                const QLocale ru(QLocale::Russian);
                const QString unit = kind == QLatin1String("slopePercent") ? QStringLiteral(" %") : QStringLiteral("°");
                message = kind == QLatin1String("aspect")
                    ? QStringLiteral("%1 по «%2» построена (0° — север, 90° — восток; ровные участки без значения).").arg(title, name)
                    : QStringLiteral("%1 по «%2»: от %3%6 до %4%6, в среднем %5%6.")
                          .arg(title, name, ru.toString(r.stats.min, 'f', 1), ru.toString(r.stats.max, 'f', 1), ru.toString(r.stats.mean, 'f', 1), unit);
            }
        }
        QMetaObject::invokeMethod(this, [this, kind, out, error, message, finalStyle] {
            m_analysisBusy = false;
            m_analysisStatus.clear();
            emit analysisChanged();
            if (!error.isEmpty()) {
                emit analysisFinished(false, error);
                return;
            }
            m_nextStyle = finalStyle;
            m_useNextStyle = kind != QLatin1String("contours");
            const QString addError = addLayerFromPath(out, false);
            m_useNextStyle = false;
            if (!addError.isEmpty()) {
                emit analysisFinished(false, addError);
                return;
            }
            const QString id = layerIdForSource(out);
            if (kind == QLatin1String("contours") && !id.isEmpty()) setLayerColor(id, QColor(226, 150, 72));
            emit analysisFinished(true, message + QStringLiteral(" Файл: %1").arg(QDir::toNativeSeparators(out)));
        }, Qt::QueuedConnection);
    });
    return {};
}

void MapService::computeProfile(const QVector<QPointF> &line)
{
    const QString id = terrainLayerId();
    const int i = indexOf(id);
    if (i < 0) {
        m_terrainText = QStringLiteral("Нет модели высот на карте");
        m_terrainDetails = QStringLiteral("Добавьте DSM или DTM (Вид → Цифровая модель высот, Файл → Импорт DEM) и проведите линию снова.");
        return;
    }
    if (line.size() < 2) return;
    const QString source = m_layers[i].source, name = m_layers[i].name;
    const Crs crs = m_crs;
    const int generation = ++m_terrainGeneration;
    m_terrainText = QStringLiteral("Построение профиля по «%1»…").arg(name);
    m_terrainDetails.clear();
    QThreadPool::globalInstance()->start([this, source, name, line, crs, generation] {
        const terrain::Profile p = terrain::profile(source, line, crs, 600);
        QMetaObject::invokeMethod(this, [this, p, name, generation] {
            if (generation != m_terrainGeneration) return;   // another line or tool meanwhile
            m_profile = p;
            m_profileLayer = name;
            m_terrainLayerName = name;
            m_profileMarker = -1;
            if (p.ok()) {
                const QLocale ru(QLocale::Russian);
                m_terrainText = QStringLiteral("Профиль: %1, высоты %2 – %3").arg(formatLength(p.length), heightText(p.minZ), heightText(p.maxZ));
                m_terrainDetails = QStringLiteral("По «%1»: подъём %2, спуск %3, наибольший уклон %4° (между соседними точками профиля, шаг %5).")
                                       .arg(name, heightText(p.ascent), heightText(p.descent), ru.toString(p.maxSlope, 'f', 1),
                                            formatLength(p.points.size() > 1 ? p.length / (p.points.size() - 1) : 0));
                if (p.noData > 0) m_terrainDetails += QStringLiteral(" Без данных: %1 точек из %2.").arg(p.noData).arg(p.points.size());
            } else {
                m_terrainText = QStringLiteral("Профиль не построен");
                m_terrainDetails = p.error;
            }
            emit profileChanged();
            updateMeasurement();
            emit terrainResultReady();
            emit repaint();
        }, Qt::QueuedConnection);
    });
}

void MapService::computeVolume(const QVector<QPointF> &ring)
{
    const QString id = terrainLayerId();
    const int i = indexOf(id);
    if (i < 0) {
        m_terrainText = QStringLiteral("Нет модели высот на карте");
        m_terrainDetails = QStringLiteral("Добавьте DSM или DTM и обведите участок снова.");
        return;
    }
    if (ring.size() < 3) return;
    static const QHash<QString, terrain::Base> bases = {
        {QStringLiteral("level"), terrain::Base::Level}, {QStringLiteral("boundaryMean"), terrain::Base::BoundaryMean},
        {QStringLiteral("boundaryMin"), terrain::Base::BoundaryMin}, {QStringLiteral("bestFit"), terrain::Base::BestFit},
        {QStringLiteral("surface"), terrain::Base::Surface}};
    const terrain::Base base = bases.value(m_volumeBase, terrain::Base::BoundaryMean);
    QString surfacePath, surfaceName;
    if (base == terrain::Base::Surface) {
        const int s = indexOf(m_volumeSurface);
        if (s < 0 || !isElevation(m_layers[s]) || s == i) {
            m_terrainText = QStringLiteral("Выберите базовую поверхность");
            m_terrainDetails = QStringLiteral("Для сравнения поверхностей укажите вторую модель высот (например, DTM под DSM).");
            return;
        }
        surfacePath = m_layers[s].source;
        surfaceName = m_layers[s].name;
    }
    const QString source = m_layers[i].source, name = m_layers[i].name;
    const Crs crs = m_crs;
    const double level = m_volumeLevel;
    const int generation = ++m_terrainGeneration;
    m_terrainText = QStringLiteral("Расчёт объёма по «%1»…").arg(name);
    m_terrainDetails.clear();
    QThreadPool::globalInstance()->start([this, source, name, ring, crs, base, level, surfacePath, surfaceName, generation] {
        const terrain::Volume v = terrain::volume(source, ring, crs, base, level, surfacePath);
        QMetaObject::invokeMethod(this, [this, v, name, base, surfaceName, generation] {
            if (generation != m_terrainGeneration) return;
            m_volume = v;
            m_terrainLayerName = name;
            if (v.ok()) {
                const QLocale ru(QLocale::Russian);
                m_terrainText = QStringLiteral("Над базой %1 · под базой %2 · баланс %3")
                                    .arg(volumeText(v.above), volumeText(v.below), volumeText(v.net()));
                QString baseText;
                switch (base) {
                case terrain::Base::Level: baseText = QStringLiteral("горизонтальная плоскость %1").arg(heightText(v.baseLevel)); break;
                case terrain::Base::BoundaryMean: baseText = QStringLiteral("средняя высота по контуру %1").arg(heightText(v.baseLevel)); break;
                case terrain::Base::BoundaryMin: baseText = QStringLiteral("наименьшая высота по контуру %1").arg(heightText(v.baseLevel)); break;
                case terrain::Base::BestFit: baseText = QStringLiteral("плоскость, подобранная по контуру"); break;
                case terrain::Base::Surface: baseText = QStringLiteral("поверхность «%1»").arg(surfaceName); break;
                }
                m_terrainDetails = QStringLiteral("Модель «%1», база: %2. Площадь %3 (%4 ячеек, в плоскости проекции).")
                                       .arg(name, baseText, formatArea(v.area), ru.toString(v.cells));
                if (v.noData > 0) m_terrainDetails += QStringLiteral(" Ячеек без данных: %1 — не учтены.").arg(ru.toString(v.noData));
            } else {
                m_terrainText = QStringLiteral("Объём не рассчитан");
                m_terrainDetails = v.error;
            }
            updateMeasurement();
            emit terrainResultReady();
        }, Qt::QueuedConnection);
    });
}

QVariantMap MapService::profileForQml() const
{
    if (!m_profile.ok() || m_profile.points.isEmpty()) return {{"valid", false}};
    QVariantList distances, heights;
    for (const terrain::ProfilePoint &p : m_profile.points) {
        distances << p.distance;
        heights << p.z;   // NaN where there is no data
    }
    return {{"valid", true}, {"layer", m_profileLayer}, {"length", m_profile.length}, {"minZ", m_profile.minZ}, {"maxZ", m_profile.maxZ},
            {"ascent", m_profile.ascent}, {"descent", m_profile.descent}, {"maxSlope", m_profile.maxSlope},
            {"noData", m_profile.noData}, {"distances", distances}, {"heights", heights},
            {"sections", int(m_cross.size())}, {"section", m_crossIndex}, {"halfWidth", m_crossHalfWidth}};
}

void MapService::setProfileMarker(int index)
{
    if (index == m_profileMarker) return;
    m_profileMarker = index;
    emit profileMarkerChanged();
    emit repaint();
}

void MapService::clearProfile()
{
    m_profile = {};
    m_profileMarker = -1;
    emit profileChanged();
    emit repaint();
}

QString MapService::exportProfile(const QUrl &url)
{
    if (!m_profile.ok() || m_profile.points.isEmpty()) return QStringLiteral("Профиль ещё не построен.");
    QSaveFile f(url.toLocalFile());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return QStringLiteral("Не удалось записать %1").arg(displayPath(url));
    QTextStream out(&f);
    out << "distance_m,elevation_m,x,y\n";
    for (const terrain::ProfilePoint &p : m_profile.points)
        out << QString::number(p.distance, 'f', 3) << ',' << (std::isfinite(p.z) ? QString::number(p.z, 'f', 3) : QString()) << ','
            << QString::number(p.x, 'f', 3) << ',' << QString::number(p.y, 'f', 3) << '\n';
    out.flush();
    if (!f.commit()) return QStringLiteral("Не удалось записать %1").arg(displayPath(url));
    return {};
}

void MapService::setVolumeBase(const QString &base)
{
    if (base == m_volumeBase) return;
    m_volumeBase = base;
    emit volumeSettingsChanged();
    if (m_tool == QLatin1String("volume") && m_sketchDone) computeVolume(m_sketch);
    updateMeasurement();
}

void MapService::setVolumeLevel(double level)
{
    if (qFuzzyCompare(level, m_volumeLevel)) return;
    m_volumeLevel = level;
    emit volumeSettingsChanged();
    if (m_tool == QLatin1String("volume") && m_sketchDone && m_volumeBase == QLatin1String("level")) computeVolume(m_sketch);
    updateMeasurement();
}

void MapService::setVolumeSurface(const QString &layerId)
{
    if (layerId == m_volumeSurface) return;
    m_volumeSurface = layerId;
    emit volumeSettingsChanged();
    if (m_tool == QLatin1String("volume") && m_sketchDone && m_volumeBase == QLatin1String("surface")) computeVolume(m_sketch);
    updateMeasurement();
}

// ----------------------------------------------------------------- saved measurements

namespace {
constexpr auto MeasurementsFile = "measurements.json";
QString kindTitle(const QString &kind)
{
    if (kind == QLatin1String("length")) return QStringLiteral("Длина");
    if (kind == QLatin1String("area")) return QStringLiteral("Площадь");
    if (kind == QLatin1String("profile")) return QStringLiteral("Профиль высот");
    if (kind == QLatin1String("volume")) return QStringLiteral("Объём");
    if (kind == QLatin1String("slope")) return QStringLiteral("Уклон");
    if (kind == QLatin1String("height")) return QStringLiteral("Высота точки");
    if (kind == QLatin1String("angle")) return QStringLiteral("Угол");
    if (kind == QLatin1String("viewshed")) return QStringLiteral("Видимость");
    return kind;
}
QString csvField(const QString &s)
{
    QString t = s;
    t.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QLatin1Char('"') + t + QLatin1Char('"');
}
} // namespace

bool MapService::canSaveMeasurement() const
{
    if (!m_crs.isValid() || !m_sketchDone) return false;
    if (m_tool == QLatin1String("length")) return m_sketch.size() >= 2;
    if (m_tool == QLatin1String("area")) return m_sketch.size() >= 3;
    if (m_tool == QLatin1String("profile"))
        return m_profile.ok() && !m_profile.points.isEmpty() && m_terrainText.startsWith(QStringLiteral("Профиль:"));
    if (m_tool == QLatin1String("volume")) return m_volume.ok() && m_volume.cells > 0 && m_terrainText.startsWith(QStringLiteral("Над базой"));
    if (m_tool == QLatin1String("slope") || m_tool == QLatin1String("height") || m_tool == QLatin1String("angle") || m_tool == QLatin1String("viewshed"))
        return !m_pointValues.isEmpty();
    return false;
}

QString MapService::saveMeasurement(const QString &name)
{
    if (!canSaveMeasurement()) return {};
    SavedMeasurement s;
    s.kind = m_tool;
    s.text = m_measurementText;
    s.details = m_measurementDetails;
    s.crs = m_crs.label();
    s.created = QDateTime::currentDateTime();
    s.geometry = m_sketch;
    if (!CoordinateTransform(m_crs, Crs::wgs84()).transform(s.geometry)) return {};
    const auto mm = [](double v) { return std::round(v * 1000.0) / 1000.0; };
    if (s.kind == QLatin1String("length") || s.kind == QLatin1String("area")) {
        const Measure m = measure(m_sketch, s.kind == QLatin1String("area"));
        s.method = QStringLiteral("Геодезически на эллипсоиде %1 (Karney, PROJ)").arg(m_crs.geographic().name());
        if (s.kind == QLatin1String("length")) s.values = {{"length_m", mm(m.length)}};
        else s.values = {{"area_m2", mm(m.area)}, {"perimeter_m", mm(m.perimeter)}};
    } else if (s.kind == QLatin1String("profile")) {
        s.layer = m_terrainLayerName;
        s.method = QStringLiteral("Высоты модели «%1», билинейно между центрами ячеек, %2 точек; длина по эллипсоиду")
                       .arg(m_terrainLayerName).arg(m_profile.points.size());
        s.values = {{"length_m", mm(m_profile.length)}, {"min_z", mm(m_profile.minZ)}, {"max_z", mm(m_profile.maxZ)},
                    {"ascent_m", mm(m_profile.ascent)}, {"descent_m", mm(m_profile.descent)}, {"max_slope_deg", mm(m_profile.maxSlope)}};
    } else if (s.kind == QLatin1String("slope") || s.kind == QLatin1String("height") || s.kind == QLatin1String("angle")
               || s.kind == QLatin1String("viewshed")) {
        s.layer = m_terrainLayerName;
        s.values = m_pointValues;
        s.method = s.kind == QLatin1String("angle") ? QStringLiteral("Азимуты сторон по сетке %1, длины — по эллипсоиду").arg(m_crs.label())
                 : s.kind == QLatin1String("viewshed") ? QStringLiteral("GDAL viewshed по «%1» с учётом кривизны Земли и рефракции").arg(m_terrainLayerName)
                 : QStringLiteral("Значения модели «%1» (билинейно между центрами ячеек)").arg(m_terrainLayerName);
    } else {
        s.layer = m_terrainLayerName;
        static const QHash<QString, QString> bases = {
            {QStringLiteral("level"), QStringLiteral("заданная высота")}, {QStringLiteral("boundaryMean"), QStringLiteral("средняя высота по контуру")},
            {QStringLiteral("boundaryMin"), QStringLiteral("наименьшая высота по контуру")},
            {QStringLiteral("bestFit"), QStringLiteral("плоскость по контуру (МНК)")}, {QStringLiteral("surface"), QStringLiteral("другая поверхность")}};
        s.method = QStringLiteral("Сумма по ячейкам модели «%1» с центром внутри контура; база: %2")
                       .arg(m_terrainLayerName, bases.value(m_volumeBase, m_volumeBase));
        s.values = {{"above_m3", mm(m_volume.above)}, {"below_m3", mm(m_volume.below)}, {"net_m3", mm(m_volume.net())},
                    {"area_m2", mm(m_volume.area)}};
        if (std::isfinite(m_volume.baseLevel)) s.values.insert(QStringLiteral("base_z"), mm(m_volume.baseLevel));
    }
    for (const SavedMeasurement &e : m_measurements) m_nextMeasurementId = qMax(m_nextMeasurementId, e.id.mid(1).toInt() + 1);
    s.id = QStringLiteral("m%1").arg(m_nextMeasurementId++);
    s.name = name.trimmed().isEmpty() ? QStringLiteral("%1 %2").arg(kindTitle(s.kind)).arg(m_measurements.size() + 1) : name.trimmed();
    m_measurements << s;
    saveMeasurements();
    emit measurementsChanged();
    return s.id;
}

void MapService::removeMeasurement(const QString &id)
{
    for (int i = 0; i < m_measurements.size(); ++i)
        if (m_measurements[i].id == id) {
            m_measurements.removeAt(i);
            if (m_shownMeasurement == id) m_shownMeasurement.clear();
            saveMeasurements();
            emit measurementsChanged();
            emit repaint();
            return;
        }
}

void MapService::renameMeasurement(const QString &id, const QString &name)
{
    for (SavedMeasurement &s : m_measurements)
        if (s.id == id && !name.trimmed().isEmpty() && s.name != name.trimmed()) {
            s.name = name.trimmed();
            saveMeasurements();
            emit measurementsChanged();
        }
}

void MapService::showMeasurement(const QString &id)
{
    m_shownMeasurement = id;
    for (const SavedMeasurement &s : m_measurements) {
        if (s.id != id || !m_crs.isValid()) continue;
        QVector<QPointF> pts = s.geometry;
        if (!CoordinateTransform(Crs::wgs84(), m_crs).transform(pts) || pts.isEmpty()) break;
        QRectF box(pts.first(), QSizeF());
        for (const QPointF &p : pts) box = box.united(QRectF(p, QSizeF()));
        const double res = qMax(box.width() / qMax(1.0, m_viewSize.width() * 0.7), box.height() / qMax(1.0, m_viewSize.height() * 0.7));
        setView(box.center(), qMax(res, 1e-6));
        break;
    }
    emit measurementsChanged();
    emit repaint();
}

QVariantList MapService::measurementsForQml() const
{
    QVariantList list;
    const QLocale ru(QLocale::Russian);
    for (const SavedMeasurement &s : m_measurements)
        list << QVariantMap{{"id", s.id}, {"name", s.name}, {"kind", s.kind}, {"kindTitle", kindTitle(s.kind)}, {"text", s.text},
                            {"details", s.details}, {"layer", s.layer}, {"method", s.method}, {"crs", s.crs},
                            {"created", ru.toString(s.created, QStringLiteral("dd.MM.yyyy HH:mm"))}, {"shown", s.id == m_shownMeasurement}};
    return list;
}

QString MapService::exportMeasurements(const QUrl &url)
{
    if (m_measurements.isEmpty()) return QStringLiteral("Нет сохранённых измерений.");
    QSaveFile f(url.toLocalFile());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return QStringLiteral("Не удалось записать %1").arg(displayPath(url));
    QTextStream out(&f);
    out.setEncoding(QStringConverter::Utf8);
    out.setGenerateByteOrderMark(true);   // Excel recognises UTF-8
    static const QStringList numbers = {"length_m", "area_m2", "perimeter_m", "min_z", "max_z", "ascent_m", "descent_m", "max_slope_deg",
                                        "above_m3", "below_m3", "net_m3", "base_z"};
    out << "name,kind,result,dem,method,crs,created," << numbers.join(QLatin1Char(',')) << ",wkt_wgs84\n";
    for (const SavedMeasurement &s : m_measurements) {
        QStringList coords;
        for (const QPointF &p : s.geometry) coords << QStringLiteral("%1 %2").arg(p.x(), 0, 'f', 8).arg(p.y(), 0, 'f', 8);
        const bool ring = s.kind == QLatin1String("area") || s.kind == QLatin1String("volume");
        if (ring && !coords.isEmpty()) coords << coords.first();
        const QString wkt = ring ? QStringLiteral("POLYGON((%1))").arg(coords.join(QStringLiteral(", ")))
                                 : QStringLiteral("LINESTRING(%1)").arg(coords.join(QStringLiteral(", ")));
        out << csvField(s.name) << ',' << s.kind << ',' << csvField(s.text) << ',' << csvField(s.layer) << ',' << csvField(s.method) << ','
            << csvField(s.crs) << ',' << s.created.toString(Qt::ISODate);
        for (const QString &k : numbers) out << ',' << (s.values.contains(k) ? QString::number(s.values.value(k).toDouble(), 'f', 3) : QString());
        out << ',' << csvField(wkt) << '\n';
    }
    out.flush();
    if (!f.commit()) return QStringLiteral("Не удалось записать %1").arg(displayPath(url));
    return {};
}

void MapService::loadMeasurements()
{
    QFile f(QDir(m_project).filePath(QString::fromLatin1(MeasurementsFile)));
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    if (root.value("schemaVersion").toInt() != 1) return;
    for (const QJsonValue &v : root.value("measurements").toArray()) {
        const QJsonObject o = v.toObject();
        SavedMeasurement s;
        s.id = o.value("id").toString();
        s.name = o.value("name").toString();
        s.kind = o.value("kind").toString();
        s.text = o.value("text").toString();
        s.details = o.value("details").toString();
        s.layer = o.value("layer").toString();
        s.method = o.value("method").toString();
        s.crs = o.value("crs").toString();
        s.created = QDateTime::fromString(o.value("created").toString(), Qt::ISODate);
        s.values = o.value("values").toObject().toVariantMap();
        for (const QJsonValue &p : o.value("geometry").toArray()) {
            const QJsonArray xy = p.toArray();
            if (xy.size() >= 2) s.geometry << QPointF(xy[0].toDouble(), xy[1].toDouble());
        }
        if (!s.id.isEmpty() && s.geometry.size() >= 2) m_measurements << s;
    }
}

void MapService::saveMeasurements() const
{
    if (m_project.isEmpty()) return;
    QJsonArray list;
    for (const SavedMeasurement &s : m_measurements) {
        QJsonArray geometry;
        for (const QPointF &p : s.geometry) geometry.append(QJsonArray{p.x(), p.y()});
        list.append(QJsonObject{{"id", s.id}, {"name", s.name}, {"kind", s.kind}, {"text", s.text}, {"details", s.details},
                                {"layer", s.layer}, {"method", s.method}, {"crs", s.crs}, {"created", s.created.toString(Qt::ISODate)},
                                {"values", QJsonObject::fromVariantMap(s.values)}, {"geometry", geometry}});
    }
    QSaveFile f(QDir(m_project).filePath(QString::fromLatin1(MeasurementsFile)));
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write(QJsonDocument(QJsonObject{{"schemaVersion", 1}, {"measurements", list}}).toJson(QJsonDocument::Indented));
    f.commit();
}

// ----------------------------------------------------------------- basemap

QString MapService::addBasemap(const QUrl &url)
{
    return addBasemapFromPath(url.isLocalFile() ? url.toLocalFile() : url.toString());
}

QString MapService::addBasemapFromPath(const QString &path)
{
    const QString error = addLayerFromPath(path, false);
    if (!error.isEmpty()) return error;
    const int i = indexOf(layerIdForSource(path));
    if (i < 0) return QStringLiteral("Подложка не добавлена.");
    if (m_layers[i].type != MapLayer::Raster) {
        removeLayer(m_layers[i].id);
        return QStringLiteral("Подложкой может быть только растр (MBTiles, GeoPackage, GeoTIFF).");
    }
    MapLayer l = m_layers.takeAt(i);
    l.basemap = true;
    l.name = QStringLiteral("Подложка · ") + l.name;
    m_layers.insert(0, l);   // under everything
    m_model->reset();
    scheduleSave();
    emit layersChanged();
    emit repaint();
    return {};
}

QVariantList MapService::basemapProviders() const
{
    QVariantList list;
    QSettings settings;
    QString current;
    for (const MapLayer &l : m_layers)
        if (l.basemap && l.source.startsWith(QLatin1String("web:"))) current = l.source.mid(4);
    for (const webtiles::Provider &p : webtiles::providers())
        list << QVariantMap{{"id", p.id}, {"title", p.title}, {"kind", p.kind}, {"attribution", p.attribution}, {"terms", p.termsUrl},
                            {"accepted", settings.value(QStringLiteral("basemap/accepted/") + p.id).toBool()}, {"current", p.id == current}};
    return list;
}

void MapService::acceptBasemapTerms(const QString &providerId)
{
    QSettings().setValue(QStringLiteral("basemap/accepted/") + providerId, true);
}

QString MapService::setOnlineBasemap(const QString &providerId)
{
    if (m_project.isEmpty()) return QStringLiteral("Откройте проект.");
    const webtiles::Provider p = webtiles::provider(providerId);
    if (!p.isValid()) return QStringLiteral("Неизвестная подложка.");
    if (!providerId.startsWith(QLatin1String("custom:")) && !QSettings().value(QStringLiteral("basemap/accepted/") + providerId).toBool())
        return QStringLiteral("Сначала примите условия использования подложки.");
    if (!m_crs.isValid()) return QStringLiteral("У карты ещё нет системы координат: добавьте снимки с GPS или задайте её (Проект → Система координат карты).");
    // One background at a time: the previous one (online or from a file) is replaced.
    for (int i = int(m_layers.size()) - 1; i >= 0; --i)
        if (m_layers[i].basemap) {
            if (m_layers[i].cancel) *m_layers[i].cancel = true;
            m_layers.removeAt(i);
        }
    MapLayer l;
    l.id = QUuid::createUuid().toString(QUuid::Id128).left(12);
    l.type = MapLayer::Raster;
    l.source = QStringLiteral("web:") + providerId;
    l.name = QStringLiteral("Подложка · ") + p.title;
    l.basemap = true;
    m_layers.insert(0, l);
    prepareRaster(0);
    m_model->reset();
    scheduleSave();
    emit layersChanged();
    emit repaint();
    return m_layers[0].status.isEmpty() ? QString() : m_layers[0].status;
}

QString MapService::basemapAttribution() const
{
    for (const MapLayer &l : m_layers)
        if (l.basemap && l.visible && l.source.startsWith(QLatin1String("web:"))) return webtiles::provider(l.source.mid(4)).attribution;
    return {};
}

QString MapService::onlineBasemap() const
{
    for (const MapLayer &l : m_layers)
        if (l.basemap && l.visible && l.source.startsWith(QLatin1String("web:"))) return l.source.mid(4);
    return {};
}

void MapService::webTilesArrived()
{
    // New tiles of an online basemap: its map tiles are drawn again (from the tile cache, cheap).
    if (m_webRepaint.isActive()) return;
    m_webRepaint.start();
}

bool MapService::hasBasemap() const
{
    for (const MapLayer &l : m_layers) if (l.basemap) return true;
    return false;
}

bool MapService::basemapVisible() const
{
    for (const MapLayer &l : m_layers) if (l.basemap && l.visible) return true;
    return false;
}

void MapService::setBasemapVisible(bool visible)
{
    for (const MapLayer &l : m_layers)
        if (l.basemap) setLayerVisible(l.id, visible);
}