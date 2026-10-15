#pragma once

#include "Crs.h"

#include <QString>
#include <QStringList>
#include <atomic>

// Writing processing results to a folder of the user's choice: rasters re-encoded or warped to
// another CRS / cell size as GeoTIFF or Cloud Optimized GeoTIFF, models with their MTL and
// textures, other files copied. Sources are never modified.
namespace exporting {

struct RasterOptions
{
    Crs crs;                 // invalid: keep the CRS of the source
    bool cog = false;        // Cloud Optimized GeoTIFF instead of a tiled GeoTIFF
    double resolution = 0;   // cell size in units of the target CRS; 0: keep (or GDAL's choice after warping)
};

struct Result
{
    QString path;            // written file (the OBJ for a model)
    QStringList files;       // every file written
    qint64 bytes = 0;
    QString error;
    bool ok() const { return error.isEmpty(); }
};

// GeoTIFF/COG with DEFLATE; warped (bilinear) when a CRS or a cell size is given.
Result exportRaster(const QString &source, const QString &target, const RasterOptions &options,
                    const std::atomic_bool *cancel = nullptr);
// An OBJ, its MTL files and every texture they name, into `folder` (the folder is created).
Result exportObj(const QString &objPath, const QString &folder);
Result copyFile(const QString &source, const QString &target);
// `folder/name`, or `folder/stem-2.ext`, `-3`, … when that exists.
QString uniqueTarget(const QString &folder, const QString &name);

} // namespace exporting
