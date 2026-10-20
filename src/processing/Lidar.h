#pragma once

#include <QRectF>
#include <QString>
#include <QVector>
#include <functional>

// LiDAR point clouds (LAS/LAZ) through PDAL of the bundled OpenDroneMap: what a file holds,
// LAZ unpacked for the 3D view, and elevation models built from the points — DSM (the highest
// point of each cell) and DTM (ground points: class 2 of the file, else found by SMRF).
namespace lidar {

struct Info
{
    QString error;
    QString path;
    qint64 bytes = 0;
    quint64 points = 0;
    QString crsWkt;
    QRectF extent;                 // in the cloud's CRS
    double minZ = 0, maxZ = 0;
    bool compressed = false;
    QString format;                // "LAS 1.4, формат 7"
    QVector<int> classes;          // ASPRS classes present
    bool hasColor = false;
    bool ok() const { return error.isEmpty(); }
};

using LineFn = std::function<void(const QString &)>;
using CancelFn = std::function<bool()>;

Info inspect(const QString &path, const CancelFn &cancelled = {});
// The LAS to read: the file itself, or a LAZ unpacked once into the cache (by path, size and date).
QString uncompressed(const QString &path, QString *error, const CancelFn &cancelled = {});

struct Surface
{
    QString error, path;
    int width = 0, height = 0;
    double resolution = 0;
    bool groundFromFile = false;   // DTM: class 2 of the file (else SMRF)
    bool ok() const { return error.isEmpty(); }
};
// kind "dsm" or "dtm"; resolution 0: suggestedResolution(). Written to `output` (GeoTIFF, float32,
// nodata −9999, the cloud's CRS).
Surface buildSurface(const Info &cloud, const QString &kind, double resolution, const QString &output,
                     const LineFn &onLine = {}, const CancelFn &cancelled = {});
// About two points per cell, rounded to a tidy value (5 cm … 10 m).
double suggestedResolution(const Info &cloud);
QString classTitle(int asprsClass);

} // namespace lidar
