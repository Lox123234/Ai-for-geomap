#pragma once

#include "Crs.h"

#include <QPointF>
#include <QString>
#include <QVector>
#include <limits>

// Terrain analysis of elevation models (the first band of a DEM/DSM/DTM GeoTIFF): slope and
// aspect rasters and contour lines through GDAL's own algorithms (gdaldem, GDALContourGenerate),
// elevation profiles along a line and cut/fill volumes inside a polygon. Source files are only
// read; results are new files. Heights are taken as they are in the file (metres for ODM).
namespace terrain {

struct Stats
{
    double min = 0, max = 0, mean = 0;
    bool valid = false;
};

struct RasterResult
{
    QString path;
    QString error;
    Stats stats;
    bool ok() const { return error.isEmpty(); }
};

// Slope in degrees (0..90) or percent (rise / run · 100). Horizontal units must be metres or a
// geographic CRS (then GDAL's 111 120 m per degree scale is used). The outer ring of cells has
// no 3×3 neighbourhood and stays nodata.
RasterResult slope(const QString &dem, const QString &out, bool percent = false);
// Direction the slope faces, degrees clockwise from north (0 = north, 90 = east); flat cells are nodata.
RasterResult aspect(const QString &dem, const QString &out);

struct ContourResult
{
    QString path;
    QString error;
    int lines = 0;
    double minLevel = 0, maxLevel = 0;
    bool ok() const { return error.isEmpty(); }
};
// Contour lines every `interval` (levels base + k·interval) into a GeoPackage layer "contours"
// with the field "elev" (metres).
ContourResult contours(const QString &dem, const QString &outGpkg, double interval, double base = 0);
// A round interval giving about 10–25 lines over the height range (1, 2, 2.5, 5 × 10^k).
double suggestedInterval(double minZ, double maxZ);

struct ProfilePoint
{
    double distance = 0;   // metres along the line (geodesic)
    double z = std::numeric_limits<double>::quiet_NaN();   // NaN where the DEM has no data
    double x = 0, y = 0;   // in the line's CRS
};

struct Profile
{
    QVector<ProfilePoint> points;
    double length = 0;                   // metres
    double minZ = 0, maxZ = 0;
    double ascent = 0, descent = 0;      // sums of rises and falls
    double maxSlope = 0;                 // degrees, between neighbouring samples
    int noData = 0;
    QString error;
    bool ok() const { return error.isEmpty(); }
};
// Heights along a polyline given in `lineCrs`, `samples` points evenly spaced along it, bilinear
// between DEM cells; distances on the ellipsoid.
Profile profile(const QString &dem, const QVector<QPointF> &line, const Crs &lineCrs, int samples = 512);

enum class Base {
    Level,          // a horizontal plane at `level`
    BoundaryMean,   // horizontal plane at the mean height along the polygon's boundary
    BoundaryMin,    // ... at the lowest height along the boundary
    BestFit,        // least-squares plane through the boundary (a stockpile on a slope)
    Surface,        // another elevation model (e.g. a DTM or an earlier survey)
};

struct Volume
{
    double above = 0;      // m³ of the surface above the base (fill / stockpile)
    double below = 0;      // m³ below the base (cut / pit)
    double area = 0;       // m², planar in the DEM's CRS
    double baseLevel = std::numeric_limits<double>::quiet_NaN();   // horizontal bases
    qint64 cells = 0, noData = 0;
    QString error;
    bool ok() const { return error.isEmpty(); }
    double net() const { return above - below; }
};
// Volumes between the DEM and a base inside a polygon given in `polygonCrs`, summed over the
// DEM cells whose centres are inside (planar cell areas of a projected CRS in metres).
Volume volume(const QString &dem, const QVector<QPointF> &polygon, const Crs &polygonCrs, Base base,
              double level = 0, const QString &baseSurface = {});

} // namespace terrain
