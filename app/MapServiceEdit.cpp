// Map editing helpers: several selected drawings, the clipboard (GeoJSON), the coordinate grid
// and the project's markers.
#include "MapService.h"

#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QSaveFile>
#include <QTextStream>
#include <QUndoCommand>
#include <cmath>
#include <cpl_vsi.h>

namespace {
constexpr auto MarkersFile = "markers.json";
const QLocale &ru()
{
    static const QLocale locale(QLocale::Russian);
    return locale;
}
QClipboard *clipboard()
{
    return qobject_cast<QGuiApplication *>(QCoreApplication::instance()) ? QGuiApplication::clipboard() : nullptr;
}
QString csv(const QString &s)
{
    QString t = s;
    t.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QLatin1Char('"') + t + QLatin1Char('"');
}
} // namespace

// ------------------------------------------------------------------ selection

QVector<int> MapService::selectedIndices() const
{
    QVector<int> list;
    const int di = drawingsIndex();
    if (di < 0) return list;
    const QVector<VectorFeature> &features = m_layers[di].vector.features;
    if (m_selectedFeature >= 0 && m_selectedFeature < features.size()) list << m_selectedFeature;
    for (int i = 0; i < features.size(); ++i)
        if (i != m_selectedFeature && m_multiSelection.contains(features[i].id)) list << i;
    return list;
}

int MapService::selectionCount() const { return int(selectedIndices().size()); }

void MapService::selectAllDrawings()
{
    const int di = drawingsIndex();
    if (di < 0 || m_layers[di].vector.features.isEmpty()) return;
    const QVector<VectorFeature> &features = m_layers[di].vector.features;
    if (m_selectedFeature < 0) m_selectedFeature = 0;
    m_multiSelection.clear();
    for (int i = 0; i < features.size(); ++i)
        if (i != m_selectedFeature) m_multiSelection.insert(features[i].id);
    updateMeasurement();
    emit drawingsChanged();
    emit repaint();
}

void MapService::invertDrawingSelection()
{
    const int di = drawingsIndex();
    if (di < 0) return;
    const QVector<int> before = selectedIndices();
    const QVector<VectorFeature> &features = m_layers[di].vector.features;
    m_selectedFeature = -1;
    m_multiSelection.clear();
    for (int i = 0; i < features.size(); ++i) {
        if (before.contains(i)) continue;
        if (m_selectedFeature < 0) m_selectedFeature = i;
        else m_multiSelection.insert(features[i].id);
    }
    updateMeasurement();
    emit drawingsChanged();
    emit repaint();
}

// ------------------------------------------------------------------ clipboard

QString MapService::selectionGeoJson() const
{
    const int di = drawingsIndex();
    if (di < 0 || !m_crs.isValid()) return {};
    const CoordinateTransform toWgs(m_crs, Crs::wgs84());
    QJsonArray features;
    for (int i : selectedIndices()) {
        const VectorFeature &f = m_layers[di].vector.features[i];
        auto ring = [&](const QPolygonF &part, bool closed) {
            QVector<QPointF> pts(part.begin(), part.end());
            toWgs.transform(pts);
            if (closed && !pts.isEmpty() && pts.first() != pts.last()) pts << pts.first();
            QJsonArray a;
            for (const QPointF &p : pts) a.append(QJsonArray{std::round(p.x() * 1e9) / 1e9, std::round(p.y() * 1e9) / 1e9});
            return a;
        };
        QJsonObject geometry;
        if (f.kind == VectorFeature::Point && !f.parts.isEmpty()) {
            geometry = {{"type", "Point"}, {"coordinates", ring(f.parts.first(), false).first()}};
        } else if (f.kind == VectorFeature::Line && !f.parts.isEmpty()) {
            geometry = {{"type", "LineString"}, {"coordinates", ring(f.parts.first(), false)}};
        } else if (f.kind == VectorFeature::Polygon) {
            QJsonArray rings;
            for (const QPolygonF &p : f.parts) rings.append(ring(p, true));
            geometry = {{"type", "Polygon"}, {"coordinates", rings}};
        }
        if (geometry.isEmpty()) continue;
        features.append(QJsonObject{{"type", "Feature"}, {"geometry", geometry}, {"properties", QJsonObject::fromVariantMap(f.attributes)}});
    }
    if (features.isEmpty()) return {};
    return QString::fromUtf8(QJsonDocument(QJsonObject{{"type", "FeatureCollection"}, {"features", features}}).toJson(QJsonDocument::Compact));
}

QString MapService::copySelection()
{
    if (!hasSelection()) return QStringLiteral("Выделите объекты (инструмент «Правка / выбор», Ctrl+щелчок — несколько).");
    const QString json = selectionGeoJson();
    if (json.isEmpty()) return QStringLiteral("Не удалось подготовить объекты для копирования.");
    QClipboard *c = clipboard();
    if (!c) return QStringLiteral("Буфер обмена недоступен.");
    c->setText(json);
    return {};
}

QString MapService::pasteDrawings()
{
    QClipboard *c = clipboard();
    if (!c) return QStringLiteral("Буфер обмена недоступен.");
    return pasteGeoJson(c->text().toUtf8());
}

QString MapService::pasteGeoJson(const QByteArray &geojson)
{
    if (!m_crs.isValid()) return QStringLiteral("Сначала задайте систему координат карты.");
    if (drawingsIndex() < 0) return QStringLiteral("Нет слоя рисунков.");
    const QByteArray trimmed = geojson.trimmed();
    if (!trimmed.startsWith('{')) return QStringLiteral("В буфере обмена нет объектов GeoJSON.");
    static int counter = 0;
    const QString path = QStringLiteral("/vsimem/geomap-paste-%1.geojson").arg(++counter);
    VSILFILE *mem = VSIFileFromMemBuffer(path.toUtf8().constData(), reinterpret_cast<GByte *>(const_cast<char *>(trimmed.constData())),
                                         vsi_l_offset(trimmed.size()), FALSE);
    if (!mem) return QStringLiteral("Не удалось прочитать буфер обмена.");
    VSIFCloseL(mem);
    const VectorData data = VectorData::load(path, {}, {}, m_crs);
    VSIUnlink(path.toUtf8().constData());
    if (!data.valid || data.features.isEmpty())
        return data.error.isEmpty() ? QStringLiteral("В буфере обмена нет объектов GeoJSON.") : data.error;
    m_undo.beginMacro(QStringLiteral("Вставка объектов: %1").arg(data.features.size()));
    for (VectorFeature f : data.features) {
        f.id = m_nextFeatureId++;
        if (f.attributes.value(QStringLiteral("name")).toString().isEmpty())
            f.attributes.insert(QStringLiteral("name"), QStringLiteral("Объект %1").arg(f.id));
        addDrawing(f, QStringLiteral("Вставка объекта"));
    }
    m_undo.endMacro();
    return {};
}

// ------------------------------------------------------------------ grid

void MapService::setShowGrid(bool on)
{
    if (on == m_showGrid) return;
    m_showGrid = on;
    scheduleSave();
    emit viewOptionsChanged();
    emit repaint();
}

void MapService::paintGrid(QPainter *p, const QSizeF &size) const
{
    if (!m_crs.isValid() || size.isEmpty()) return;
    const QPointF a = toMap(QPointF(0, 0)), b = toMap(QPointF(size.width(), size.height()));
    const double x0 = qMin(a.x(), b.x()), x1 = qMax(a.x(), b.x()), y0 = qMin(a.y(), b.y()), y1 = qMax(a.y(), b.y());
    // A round step giving 4–10 lines across the longer side.
    const double raw = qMax(x1 - x0, y1 - y0) / 6;
    if (!(raw > 0)) return;
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    double step = 10 * mag;
    for (double m : {1.0, 2.0, 2.5, 5.0, 10.0})
        if (m * mag >= raw) { step = m * mag; break; }
    const int decimals = step >= 1 ? 0 : int(std::ceil(-std::log10(step)));
    const bool degrees = m_crs.isGeographic();
    QPen pen(QColor(255, 255, 255, 80), 1, Qt::DashLine);
    pen.setCosmetic(true);
    QFont font = p->font();
    font.setPixelSize(10);
    p->setFont(font);
    auto label = [&](double v) { return ru().toString(v, 'f', decimals) + (degrees ? QStringLiteral("°") : QString()); };
    for (double x = std::ceil(x0 / step) * step; x <= x1; x += step) {
        const double sx = toScreen(QPointF(x, y0)).x();
        p->setPen(pen);
        p->drawLine(QPointF(sx, 0), QPointF(sx, size.height()));
        p->setPen(QColor(235, 240, 245, 220));
        p->drawText(QPointF(sx + 3, 12), label(x));
    }
    for (double y = std::ceil(y0 / step) * step; y <= y1; y += step) {
        const double sy = toScreen(QPointF(x0, y)).y();
        p->setPen(pen);
        p->drawLine(QPointF(0, sy), QPointF(size.width(), sy));
        p->setPen(QColor(235, 240, 245, 220));
        p->drawText(QPointF(3, sy - 3), label(y));
    }
}

// ------------------------------------------------------------------ markers

void MapService::setShowMarkers(bool on)
{
    if (on == m_showMarkers) return;
    m_showMarkers = on;
    scheduleSave();
    emit viewOptionsChanged();
    emit repaint();
}

void MapService::paintMarkers(QPainter *p) const
{
    if (!m_showMarkers || m_markers.isEmpty() || !m_crs.isValid()) return;
    const CoordinateTransform toMapCrs(Crs::wgs84(), m_crs);
    QFont font = p->font();
    font.setPixelSize(11);
    font.setBold(true);
    p->setFont(font);
    for (const Marker &m : m_markers) {
        QPointF pt(m.lon, m.lat);
        if (!toMapCrs.transform(pt)) continue;
        const QPointF s = toScreen(pt);
        p->setPen(QPen(QColor(20, 24, 30), 2));
        p->setBrush(QColor(255, 120, 60));
        QPainterPath pin;   // a drop with its tip on the point
        pin.moveTo(s);
        pin.cubicTo(s + QPointF(-9, -12), s + QPointF(-8, -24), s + QPointF(0, -24));
        pin.cubicTo(s + QPointF(8, -24), s + QPointF(9, -12), s);
        p->drawPath(pin);
        p->setBrush(Qt::white);
        p->drawEllipse(s + QPointF(0, -16), 3.5, 3.5);
        p->setPen(QColor(255, 236, 220));
        p->drawText(s + QPointF(10, -14), m.name);
    }
}

QVariantList MapService::markersForQml() const
{
    QVariantList list;
    const CoordinateTransform toMapCrs(Crs::wgs84(), m_crs);
    for (const Marker &m : m_markers) {
        QPointF pt(m.lon, m.lat);
        const bool mapped = m_crs.isValid() && toMapCrs.transform(pt);
        list << QVariantMap{{"id", m.id}, {"name", m.name}, {"note", m.note}, {"lon", m.lon}, {"lat", m.lat}, {"z", m.z},
                            {"hasZ", std::isfinite(m.z)}, {"heightSource", m.heightSource}, {"x", mapped ? pt.x() : qQNaN()},
                            {"y", mapped ? pt.y() : qQNaN()}, {"created", m.created}};
    }
    return list;
}

QString MapService::addMarker(const QPointF &mapPoint, const QString &name)
{
    if (!m_crs.isValid() || m_project.isEmpty()) return {};
    Marker m;
    QPointF wgs = mapPoint;
    if (!CoordinateTransform(m_crs, Crs::wgs84()).transform(wgs)) return {};
    m.lon = wgs.x();
    m.lat = wgs.y();
    // The height of the top elevation model under it: models named as such (DSM, DTM, DEM, edited
    // copies) before other single-band rasters (slope, indices, calculator results).
    static const QStringList heightWords = {QStringLiteral("dsm"), QStringLiteral("dtm"), QStringLiteral("dem"), QStringLiteral("высот"),
                                            QStringLiteral("рельеф"), QStringLiteral("_правка"), QStringLiteral("_заполнено"), QStringLiteral("elev")};
    for (int pass = 0; pass < 2 && !std::isfinite(m.z); ++pass)
        for (int i = int(m_layers.size()) - 1; i >= 0; --i) {
            const MapLayer &l = m_layers[i];
            if (!l.visible || !isElevation(l) || !l.raster || isDerived(l)) continue;
            const QString file = QFileInfo(l.source).fileName();
            const QString key = (file + QLatin1Char(' ') + l.name).toLower();
            const bool named = std::any_of(heightWords.cbegin(), heightWords.cend(), [&key](const QString &w) { return key.contains(w); });
            if (pass == 0 && !named) continue;
            const double z = l.raster->valueAt(mapPoint, 1);
            if (!std::isfinite(z)) continue;
            m.z = z;
            m.heightSource = l.name;
            break;
        }
    for (const Marker &e : m_markers) m_nextMarker = qMax(m_nextMarker, e.id.mid(1).toInt() + 1);
    m.id = QStringLiteral("k%1").arg(m_nextMarker++);
    m.name = name.trimmed().isEmpty() ? QStringLiteral("М%1").arg(m_markers.size() + 1) : name.trimmed();
    m.created = QDateTime::currentDateTime();
    m_markers << m;
    saveMarkers();
    emit markersChanged();
    emit repaint();
    return m.id;
}

void MapService::removeMarker(const QString &id)
{
    for (int i = 0; i < m_markers.size(); ++i)
        if (m_markers[i].id == id) {
            m_markers.removeAt(i);
            saveMarkers();
            emit markersChanged();
            emit repaint();
            return;
        }
}

void MapService::renameMarker(const QString &id, const QString &name)
{
    for (Marker &m : m_markers)
        if (m.id == id && !name.trimmed().isEmpty() && m.name != name.trimmed()) {
            m.name = name.trimmed();
            saveMarkers();
            emit markersChanged();
            emit repaint();
        }
}

void MapService::setMarkerNote(const QString &id, const QString &note)
{
    for (Marker &m : m_markers)
        if (m.id == id && m.note != note) {
            m.note = note;
            saveMarkers();
            emit markersChanged();
        }
}

void MapService::zoomToMarker(const QString &id)
{
    for (const Marker &m : m_markers) {
        if (m.id != id || !m_crs.isValid()) continue;
        QPointF pt(m.lon, m.lat);
        if (CoordinateTransform(Crs::wgs84(), m_crs).transform(pt)) setView(pt, m_resolution);
    }
}

QString MapService::exportMarkers(const QUrl &url)
{
    if (m_markers.isEmpty()) return QStringLiteral("Маркеров нет.");
    QSaveFile f(url.toLocalFile());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return QStringLiteral("Не удалось записать %1").arg(displayPath(url));
    QTextStream out(&f);
    out << "\xEF\xBB\xBF";   // UTF-8 for spreadsheet programs
    out << "name,lon,lat,x,y,crs,z,height_source,note\n";
    const CoordinateTransform toMapCrs(Crs::wgs84(), m_crs);
    for (const Marker &m : m_markers) {
        QPointF pt(m.lon, m.lat);
        const bool mapped = m_crs.isValid() && toMapCrs.transform(pt);
        out << csv(m.name) << ',' << QString::number(m.lon, 'f', 9) << ',' << QString::number(m.lat, 'f', 9) << ','
            << (mapped ? QString::number(pt.x(), 'f', 3) : QString()) << ',' << (mapped ? QString::number(pt.y(), 'f', 3) : QString()) << ','
            << csv(m_crs.isValid() ? m_crs.label() : QString()) << ',' << (std::isfinite(m.z) ? QString::number(m.z, 'f', 3) : QString())
            << ',' << csv(m.heightSource) << ',' << csv(m.note) << '\n';
    }
    out.flush();
    if (!f.commit()) return QStringLiteral("Не удалось записать %1").arg(displayPath(url));
    return {};
}

void MapService::loadMarkers()
{
    m_markers.clear();
    m_nextMarker = 1;
    QFile f(QDir(m_project).filePath(QLatin1String(MarkersFile)));
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    if (root.value(QStringLiteral("schemaVersion")).toInt() != 1) return;
    for (const QJsonValue &v : root.value(QStringLiteral("markers")).toArray()) {
        const QJsonObject o = v.toObject();
        Marker m;
        m.id = o.value(QStringLiteral("id")).toString();
        m.name = o.value(QStringLiteral("name")).toString();
        m.note = o.value(QStringLiteral("note")).toString();
        m.lon = o.value(QStringLiteral("lon")).toDouble();
        m.lat = o.value(QStringLiteral("lat")).toDouble();
        m.z = o.contains(QStringLiteral("z")) ? o.value(QStringLiteral("z")).toDouble() : qQNaN();
        m.heightSource = o.value(QStringLiteral("heightSource")).toString();
        m.created = QDateTime::fromString(o.value(QStringLiteral("created")).toString(), Qt::ISODate);
        if (!m.id.isEmpty()) m_markers << m;
    }
}

void MapService::saveMarkers() const
{
    if (m_project.isEmpty()) return;
    QJsonArray list;
    for (const Marker &m : m_markers) {
        QJsonObject o{{"id", m.id}, {"name", m.name}, {"lon", m.lon}, {"lat", m.lat}, {"created", m.created.toString(Qt::ISODate)}};
        if (!m.note.isEmpty()) o.insert(QStringLiteral("note"), m.note);
        if (std::isfinite(m.z)) {
            o.insert(QStringLiteral("z"), m.z);
            o.insert(QStringLiteral("heightSource"), m.heightSource);
        }
        list.append(o);
    }
    QSaveFile f(QDir(m_project).filePath(QLatin1String(MarkersFile)));
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(QJsonObject{{"schemaVersion", 1}, {"markers", list}}).toJson(QJsonDocument::Indented));
        f.commit();
    }
}
