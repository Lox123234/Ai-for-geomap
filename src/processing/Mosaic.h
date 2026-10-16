#pragma once

#include <QPointF>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

// Mosaic of rasters of the same kind (orthophotos of several chunks, or their DSM/DTM) into one
// GeoTIFF: what a chunk split apart is put together again. Every output pixel is taken from the
// raster whose centre (of its chunk's cell when the chunk came from a split, else of its extent)
// is nearest among those with data there, so seams run along the middle of the overlaps, away
// from the weak edges of each reconstruction; across a seam the two rasters are blended over a
// few metres. Rasters in another CRS are reprojected to the CRS of the first.
namespace mosaic {

struct Input
{
    QString path;
    QString name;              // for messages
    QVector<QPointF> cell;     // optional: the chunk's cell, WGS 84 (longitude, latitude)
};

struct Options
{
    double resolution = 0;     // output units per pixel; 0 = the finest input
    double blend = 1.0;        // metres on each side of a seam over which two rasters are blended
    std::function<bool(double)> progress;   // 0..1; false cancels
};

struct Result
{
    QString error;
    int width = 0, height = 0;
    int bands = 0;
    double resolution = 0;
    int inputs = 0;
    QString crs;               // name of the output CRS
    QStringList notes;         // inputs that were reprojected or skipped
    double seconds = 0;
    bool ok() const { return error.isEmpty(); }
};

// Writes `output` (tiled, DEFLATE, overviews; via output.partial.tif). RGB(A) Byte rasters keep
// an alpha band; single-band rasters keep their nodata value (−9999 if they had none).
Result build(const QVector<Input> &inputs, const QString &output, const Options &options = {});

} // namespace mosaic
