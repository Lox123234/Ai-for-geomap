#pragma once

#include "Crs.h"
#include <QPolygonF>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

// A vector feature with coordinates in the map CRS.
struct VectorFeature
{
    enum Kind { Point, Line, Polygon };
    Kind kind = Point;
    QVector<QPolygonF> parts;    // points: one point per part; lines: one line per part;
                                 // polygons: all rings (drawn with the odd-even rule)
    QVariantMap attributes;
    QRectF bounds;
    qint64 id = -1;
};

// One OGR layer read into memory (GeoJSON, KML, GeoPackage, ...) and transformed to the map CRS.
struct VectorData
{
    bool valid = false;
    QString error;
    QString path;
    QString layer;               // OGR layer name
    QString driver;
    Crs sourceCrs;               // invalid: the file has none and nothing was assigned
    QString crsNote;             // e.g. "WGS 84 по спецификации GeoJSON (RFC 7946)"
    QVector<VectorFeature> features;
    QRectF extent;               // map CRS
    QStringList fields;
    int points = 0, lines = 0, polygons = 0;
    int skipped = 0;             // features without usable geometry or failed transformation
    bool truncated = false;      // more than the feature limit

    static QStringList layerNames(const QString &path, QString *error = nullptr);
    // CRS and extent (in that CRS) of a layer without reading its features. The CRS is
    // WGS 84 for GeoJSON/KML (their specifications) and invalid when the file has none.
    static bool probe(const QString &path, const QString &layer, Crs *crs, QRectF *extent, QString *error);
    // `crsOverride` is used when the layer has no CRS of its own.
    static VectorData load(const QString &path, const QString &layer, const Crs &crsOverride, const Crs &mapCrs,
                           int maxFeatures = 200000);
    QString summary() const;     // "полигонов: 12, линий: 3, точек: 5 · EPSG:4326"
    QString brief() const;       // for a list: "полигонов: 12, точек: 5"
};

// Writes features (map CRS) with OGR: GeoJSON (always WGS 84 lon/lat, RFC 7946),
// KML (WGS 84) or GeoPackage (map CRS). Format from the extension: .geojson/.json, .kml, .gpkg.
bool writeVectorFile(const QString &path, const QString &layerName, const QVector<VectorFeature> &features,
                     const Crs &mapCrs, QString *error);
