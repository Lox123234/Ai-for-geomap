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
// A − B on the grid of A (B resampled bilinearly into A's CRS and cells): heights of vegetation and
// buildings (DSM − DTM) or changes between surveys. Nodata where either model has none.
RasterResult difference(const QString &demA, const QString &demB, const QString &out);

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

// Profiles across a polyline (given in `lineCrs`, a projected CRS): one every `spacing` metres
// along it, `halfWidth` metres to each side, left to right looking along the line. `sections`
// receives their end points (in `lineCrs`) when not null.
QVector<Profile> crossSections(const QString &dem, const QVector<QPointF> &line, const Crs &lineCrs, double spacing,
                               double halfWidth, int samples = 160, QVector<QVector<QPointF>> *sections = nullptr);

struct Viewshed
{
    QString path, error;
    double visibleArea = 0;    // m² seen within the radius
    double area = 0;           // m² of the DEM within the radius
    double observerZ = std::numeric_limits<double>::quiet_NaN();   // ground under the observer
    bool ok() const { return error.isEmpty(); }
};
// What an observer `observerHeight` metres above the surface at `observer` (in `observerCrs`)
// sees of points `targetHeight` above it within `radius` metres (GDAL viewshed, curvature and
// refraction of the Earth included): Byte GeoTIFF, 1 = visible, 0 = hidden, 255 = out of range.
Viewshed viewshed(const QString &dem, const QPointF &observer, const Crs &observerCrs, double observerHeight,
                  double targetHeight, double radius, const QString &out);

// Raster calculator: an expression over the bands of one raster (B1, B2, …), numbers, + − * /,
// parentheses and sqrt(), abs(), min(a, b), max(a, b). Float32 GeoTIFF; nodata (−9999) where an
// input pixel is masked (nodata or alpha) or the result is not a number. `above` gets the share of
// valid pixels whose value is greater than `threshold` when it is a number.
RasterResult calculate(const QString &raster, const QString &expression, const QString &out,
                       double threshold = std::numeric_limits<double>::quiet_NaN(), double *above = nullptr);
// "" when the expression is valid for a raster with `bands` bands, else the reason.
QString checkExpression(const QString &expression, int bands);
// Vegetation indices as expressions: exg (2g − r − b of chromatic coordinates), vari, gli (RGB)
// and ndvi (needs a near-infrared band). The bands are 1-based.
QString indexExpression(const QString &index, int red = 1, int green = 2, int blue = 3, int nir = 4);
double indexThreshold(const QString &index);   // value above which a pixel counts as vegetation

} // namespace terrain
