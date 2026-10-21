#include "VectorSource.h"
#include "GisEnvironment.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QSet>
#include <gdal_priv.h>
#include <ogrsf_frmts.h>
#include <memory>

namespace {
// GDAL's virtual file systems (/vsimem/, /vsizip/ …) keep their forward slashes.
QByteArray utf8(const QString &path)
{
    return (path.startsWith(QLatin1String("/vsi")) ? path : QDir::toNativeSeparators(path)).toUtf8();
}

struct DatasetCloser { void operator()(GDALDataset *ds) const { GDALClose(ds); } };
using Dataset = std::unique_ptr<GDALDataset, DatasetCloser>;

Dataset openVector(const QString &path, bool update = false)
{
    return Dataset(GDALDataset::Open(utf8(path).constData(),
                                     GDAL_OF_VECTOR | (update ? GDAL_OF_UPDATE : GDAL_OF_READONLY), nullptr, nullptr, nullptr));
}

QPolygonF lineOf(const OGRSimpleCurve *curve)
{
    QPolygonF line;
    line.reserve(curve->getNumPoints());
    for (int i = 0; i < curve->getNumPoints(); ++i) line << QPointF(curve->getX(i), curve->getY(i));
    return line;
}

// Flattens any geometry into the parts of one feature; returns false for unsupported types.
void collect(const OGRGeometry *g, VectorFeature &f, bool &hasPoint, bool &hasLine, bool &hasPolygon)
{
    if (!g || g->IsEmpty()) return;
    std::unique_ptr<OGRGeometry> linear;
    if (g->hasCurveGeometry()) {
        linear.reset(g->getLinearGeometry());
        g = linear.get();
    }
    switch (wkbFlatten(g->getGeometryType())) {
    case wkbPoint: {
        const auto *p = g->toPoint();
        f.parts << QPolygonF({QPointF(p->getX(), p->getY())});
        hasPoint = true;
        break;
    }
    case wkbLineString:
        f.parts << lineOf(g->toLineString());
        hasLine = true;
        break;
    case wkbPolygon: {
        const auto *poly = g->toPolygon();
        for (const OGRLinearRing *ring : *poly) f.parts << lineOf(ring);
        hasPolygon = true;
        break;
    }
    case wkbMultiPoint:
    case wkbMultiLineString:
    case wkbMultiPolygon:
    case wkbGeometryCollection:
        for (const OGRGeometry *part : *g->toGeometryCollection()) collect(part, f, hasPoint, hasLine, hasPolygon);
        break;
    default:
        break;
    }
}

bool standardWgs84(const QString &driver, QString *note)
{
    if (driver == QLatin1String("GeoJSON") || driver == QLatin1String("GeoJSONSeq")) {
        *note = QStringLiteral("WGS 84 по спецификации GeoJSON (RFC 7946)");
        return true;
    }
    if (driver == QLatin1String("KML") || driver == QLatin1String("LIBKML")) {
        *note = QStringLiteral("WGS 84 по спецификации KML");
        return true;
    }
    return false;
}
} // namespace

QStringList VectorData::layerNames(const QString &path, QString *error)
{
    GisEnvironment::clearError();
    Dataset ds = openVector(path);
    if (!ds) {
        if (error) *error = QStringLiteral("Не удалось открыть векторный файл: %1").arg(GisEnvironment::lastError());
        return {};
    }
    QStringList names;
    for (OGRLayer *layer : ds->GetLayers()) names << QString::fromUtf8(layer->GetName());
    if (names.isEmpty() && error) *error = QStringLiteral("В файле нет векторных слоёв.");
    return names;
}

bool VectorData::probe(const QString &path, const QString &layerName, Crs *crs, QRectF *extent, QString *error)
{
    GisEnvironment::clearError();
    Dataset ds = openVector(path);
    if (!ds) {
        if (error) *error = QStringLiteral("Не удалось открыть векторный файл: %1").arg(GisEnvironment::lastError());
        return false;
    }
    OGRLayer *layer = layerName.isEmpty() ? ds->GetLayer(0) : ds->GetLayerByName(layerName.toUtf8().constData());
    if (!layer) {
        if (error) *error = QStringLiteral("Слой «%1» не найден.").arg(layerName);
        return false;
    }
    *crs = {};
    if (const OGRSpatialReference *srs = layer->GetSpatialRef()) {
        char *wkt = nullptr;
        if (srs->exportToWkt(&wkt) == OGRERR_NONE && wkt) *crs = Crs::fromWkt(QString::fromUtf8(wkt));
        CPLFree(wkt);
    }
    QString note;
    if (!crs->isValid() && standardWgs84(QString::fromLatin1(ds->GetDriverName()), &note)) *crs = Crs::wgs84();
    OGREnvelope env;
    if (layer->GetExtent(&env, TRUE) == OGRERR_NONE)
        *extent = QRectF(env.MinX, env.MinY, env.MaxX - env.MinX, env.MaxY - env.MinY);
    return true;
}

VectorData VectorData::load(const QString &path, const QString &layerName, const Crs &crsOverride, const Crs &mapCrs,
                            int maxFeatures)
{
    VectorData data;
    data.path = path;
    data.layer = layerName;
    GisEnvironment::clearError();
    Dataset ds = openVector(path);
    if (!ds) {
        data.error = QStringLiteral("Не удалось открыть векторный файл: %1").arg(GisEnvironment::lastError());
        return data;
    }
    data.driver = QString::fromLatin1(ds->GetDriverName());
    OGRLayer *layer = layerName.isEmpty() ? ds->GetLayer(0) : ds->GetLayerByName(layerName.toUtf8().constData());
    if (!layer) {
        data.error = QStringLiteral("Слой «%1» не найден.").arg(layerName);
        return data;
    }
    data.layer = QString::fromUtf8(layer->GetName());
    if (const OGRSpatialReference *srs = layer->GetSpatialRef()) {
        char *wkt = nullptr;
        if (srs->exportToWkt(&wkt) == OGRERR_NONE && wkt) data.sourceCrs = Crs::fromWkt(QString::fromUtf8(wkt));
        CPLFree(wkt);
    }
    // GDAL already reports WGS 84 for GeoJSON/KML without a CRS; say where it comes from.
    QString standardNote;
    const bool standard = standardWgs84(data.driver, &standardNote);
    if (data.sourceCrs.isValid()) {
        if (standard && data.sourceCrs.isSame(Crs::wgs84())) data.crsNote = standardNote;
    } else {
        if (crsOverride.isValid()) {
            data.sourceCrs = crsOverride;
            data.crsNote = QStringLiteral("CRS указан пользователем");
        } else if (standard) {
            data.sourceCrs = Crs::wgs84();
            data.crsNote = standardNote;
        } else {
            data.error = QStringLiteral("У слоя не задана система координат.");
            return data;
        }
    }
    if (!mapCrs.isValid()) {
        data.error = QStringLiteral("У карты не задана система координат.");
        return data;
    }
    CoordinateTransform toMap(data.sourceCrs, mapCrs);
    if (!toMap.isValid()) {
        data.error = toMap.error();
        return data;
    }
    OGRFeatureDefn *defn = layer->GetLayerDefn();
    for (int i = 0; i < defn->GetFieldCount(); ++i) data.fields << QString::fromUtf8(defn->GetFieldDefn(i)->GetNameRef());

    layer->ResetReading();
    bool first = true;
    for (auto &feature : *layer) {
        if (data.features.size() >= maxFeatures) {
            data.truncated = true;
            break;
        }
        VectorFeature f;
        f.id = feature->GetFID();
        bool hasPoint = false, hasLine = false, hasPolygon = false;
        collect(feature->GetGeometryRef(), f, hasPoint, hasLine, hasPolygon);
        if (f.parts.isEmpty()) {
            ++data.skipped;
            continue;
        }
        f.kind = hasPolygon ? VectorFeature::Polygon : hasLine ? VectorFeature::Line : VectorFeature::Point;
        bool ok = true;
        for (QPolygonF &part : f.parts) {
            QVector<QPointF> pts(part.begin(), part.end());
            ok = toMap.transform(pts) && ok;
            part = QPolygonF(pts);
            f.bounds = f.bounds.isNull() ? part.boundingRect() : f.bounds.united(part.boundingRect());
        }
        if (!ok) {
            ++data.skipped;
            continue;
        }
        for (int i = 0; i < defn->GetFieldCount(); ++i) {
            if (!feature->IsFieldSetAndNotNull(i)) continue;
            f.attributes.insert(data.fields[i], QString::fromUtf8(feature->GetFieldAsString(i)));
        }
        if (f.kind == VectorFeature::Point) ++data.points;
        else if (f.kind == VectorFeature::Line) ++data.lines;
        else ++data.polygons;
        // Points have empty bounds: extend by the point itself.
        const QRectF b = f.bounds.isValid() ? f.bounds : QRectF(f.parts.first().first(), QSizeF(0, 0));
        data.extent = first ? b : data.extent.united(b);
        first = false;
        data.features << f;
    }
    data.valid = true;
    return data;
}

QString VectorData::summary() const
{
    if (!valid) return error;
    QStringList parts;
    if (polygons) parts << QStringLiteral("полигонов: %1").arg(polygons);
    if (lines) parts << QStringLiteral("линий: %1").arg(lines);
    if (points) parts << QStringLiteral("точек: %1").arg(points);
    if (parts.isEmpty()) parts << QStringLiteral("объектов нет");
    QString text = parts.join(QStringLiteral(", ")) + QStringLiteral(" · ") + sourceCrs.label();
    if (!crsNote.isEmpty()) text += QStringLiteral(" (%1)").arg(crsNote);
    if (skipped) text += QStringLiteral(" · пропущено: %1").arg(skipped);
    if (truncated) text += QStringLiteral(" · показаны первые %1").arg(features.size());
    return text;
}

QString VectorData::brief() const
{
    if (!valid) return error;
    QStringList parts;
    if (polygons) parts << QStringLiteral("полигонов: %1").arg(polygons);
    if (lines) parts << QStringLiteral("линий: %1").arg(lines);
    if (points) parts << QStringLiteral("точек: %1").arg(points);
    return parts.isEmpty() ? QStringLiteral("объектов нет") : parts.join(QStringLiteral(", "));
}

bool writeVectorFile(const QString &path, const QString &layerName, const QVector<VectorFeature> &features,
                     const Crs &mapCrs, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error) *error = message;
        return false;
    };
    const QString suffix = QFileInfo(path).suffix().toLower();
    QString driverName;
    Crs target = mapCrs;
    if (suffix == QLatin1String("geojson") || suffix == QLatin1String("json")) {
        driverName = QStringLiteral("GeoJSON");
        target = Crs::wgs84();
    } else if (suffix == QLatin1String("kml")) {
        driverName = QStringLiteral("KML");
        target = Crs::wgs84();
    } else if (suffix == QLatin1String("gpkg")) {
        driverName = QStringLiteral("GPKG");
    } else {
        return fail(QStringLiteral("Формат «%1» не поддерживается: используйте .geojson, .kml или .gpkg.").arg(suffix));
    }
    if (!target.isValid()) return fail(QStringLiteral("У карты не задана система координат."));
    GDALDriver *driver = GetGDALDriverManager()->GetDriverByName(driverName.toLatin1().constData());
    if (!driver) return fail(QStringLiteral("Драйвер %1 недоступен в этой сборке GDAL.").arg(driverName));
    // Same extension as the target: some drivers check it.
    const QString tmp = QFileInfo(path).absolutePath() + QStringLiteral("/.tmp-") + QFileInfo(path).fileName();
    QFile::remove(tmp);
    GisEnvironment::clearError();
    QList<QByteArray> optionStorage;
    QVector<const char *> creation;
    if (driverName == QLatin1String("GeoJSON")) {
        optionStorage << "RFC7946=YES" << "WRITE_BBOX=YES";
    }
    for (const QByteArray &o : optionStorage) creation << o.constData();
    creation << nullptr;
    Dataset ds(driver->Create(utf8(tmp).constData(), 0, 0, 0, GDT_Unknown, nullptr));
    if (!ds) return fail(QStringLiteral("Не удалось создать файл: %1").arg(GisEnvironment::lastError()));
    OGRSpatialReference srs;
    srs.importFromWkt(target.wkt().toUtf8().constData());
    srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    OGRLayer *layer = ds->CreateLayer(layerName.toUtf8().constData(), &srs, wkbUnknown, const_cast<char **>(creation.data()));
    if (!layer) return fail(QStringLiteral("Не удалось создать слой: %1").arg(GisEnvironment::lastError()));
    // A field is numeric (Real) when every value in it is a number, otherwise text.
    QStringList fields;
    QSet<QString> text;
    for (const VectorFeature &f : features)
        for (auto it = f.attributes.cbegin(); it != f.attributes.cend(); ++it) {
            if (!fields.contains(it.key())) fields << it.key();
            const int type = it.value().typeId();
            if (type != QMetaType::Double && type != QMetaType::Float && type != QMetaType::Int && type != QMetaType::LongLong)
                text.insert(it.key());
        }
    for (const QString &name : fields) {
        OGRFieldDefn field(name.toUtf8().constData(), text.contains(name) ? OFTString : OFTReal);
        layer->CreateField(&field);
    }
    CoordinateTransform toTarget(mapCrs, target);
    for (const VectorFeature &f : features) {
        OGRFeature feature(layer->GetLayerDefn());
        for (auto it = f.attributes.cbegin(); it != f.attributes.cend(); ++it) {
            if (text.contains(it.key())) feature.SetField(it.key().toUtf8().constData(), it.value().toString().toUtf8().constData());
            else feature.SetField(it.key().toUtf8().constData(), it.value().toDouble());
        }
        auto transformed = [&](const QPolygonF &part) {
            QVector<QPointF> pts(part.begin(), part.end());
            toTarget.transform(pts);
            return pts;
        };
        std::unique_ptr<OGRGeometry> geometry;
        if (f.kind == VectorFeature::Point && !f.parts.isEmpty()) {
            const QPointF p = transformed(f.parts.first()).first();
            geometry = std::make_unique<OGRPoint>(p.x(), p.y());
        } else if (f.kind == VectorFeature::Line && !f.parts.isEmpty()) {
            auto line = std::make_unique<OGRLineString>();
            for (const QPointF &p : transformed(f.parts.first())) line->addPoint(p.x(), p.y());
            geometry = std::move(line);
        } else if (f.kind == VectorFeature::Polygon && !f.parts.isEmpty()) {
            auto polygon = std::make_unique<OGRPolygon>();
            for (const QPolygonF &part : f.parts) {
                auto ring = std::make_unique<OGRLinearRing>();
                for (const QPointF &p : transformed(part)) ring->addPoint(p.x(), p.y());
                ring->closeRings();
                polygon->addRingDirectly(ring.release());
            }
            geometry = std::move(polygon);
        }
        if (!geometry) continue;
        feature.SetGeometryDirectly(geometry.release());
        if (layer->CreateFeature(&feature) != OGRERR_NONE)
            return fail(QStringLiteral("Не удалось записать объект: %1").arg(GisEnvironment::lastError()));
    }
    ds.reset();
    QFile::remove(path);
    if (!QFile::rename(tmp, path)) return fail(QStringLiteral("Не удалось сохранить %1").arg(QDir::toNativeSeparators(path)));
    return true;
}
