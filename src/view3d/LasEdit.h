#pragma once

#include "Crs.h"

#include <QMap>
#include <QPolygonF>
#include <QString>
#include <QVector>

// Editing of uncompressed LAS point clouds (1.0–1.4, point formats 0–10) into a new file: points
// inside a polygon removed, kept or given a class, and a classification of every point by its
// height above a terrain model (DTM) and its colour. Header, variable-length records, extended
// records and every other point attribute are kept; counts and bounds are recomputed.
namespace las {

struct Result
{
    QString path, error;
    qint64 total = 0;              // points read
    qint64 written = 0;            // points in the new file
    qint64 changed = 0;            // points removed or reclassified
    QMap<int, qint64> classes;     // ASPRS class → points in the new file
    bool ok() const { return error.isEmpty(); }
};

enum class Operation { DeleteInside, KeepInside, ClassifyInside };

// The CRS of a LAS file from its WKT (2112) or GeoTIFF keys (34735) record; invalid without one.
Crs fileCrs(const QString &path);
// `rings` (outer ring, then holes) are in `ringsCrs`; the file's CRS is used, or `fallback` when
// the file has none.
Result edit(const QString &in, const QString &out, Operation op, const QVector<QPolygonF> &rings, const Crs &ringsCrs,
            int classCode = 7, const Crs &fallback = {});

struct ClassifyOptions
{
    double groundTolerance = 0.25;   // |height above DTM| within this → ground (2)
    double greenness = 0.03;         // ExG = (2G − R − B) / (R + G + B) above this → vegetation
    double lowVegetation = 0.5;      // vegetation lower than this → low (3), lower than mediumVegetation → medium (4), else high (5)
    double mediumVegetation = 2.0;
    double building = 2.0;           // not green and higher than this → building (6); between ground and this → unclassified (1)
    double noiseBelow = 1.0;         // deeper than this under the DTM → low noise (7)
};
// Every point classified by height above `dtm` (any CRS GDAL reads; points are transformed) and,
// when the file has colours, by greenness.
Result classify(const QString &in, const QString &out, const QString &dtm, const ClassifyOptions &options = {},
                const Crs &fallback = {});

} // namespace las
