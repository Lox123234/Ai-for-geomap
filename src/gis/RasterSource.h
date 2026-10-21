#pragma once

#include "Crs.h"
#include <QImage>
#include <QMutex>
#include <QRectF>
#include <QString>
#include <QVariantMap>
#include <QVector>
#include <atomic>
#include <functional>
#include <memory>

// What a raster file contains, read once with GDAL.
struct RasterInfo
{
    bool valid = false;
    QString error;
    QString path;
    QString driver;
    int width = 0;
    int height = 0;
    int bands = 0;
    QString dataType;            // of band 1: "Byte", "UInt16", "Float32", ...
    int overviews = 0;           // of band 1
    int alphaBand = 0;           // 1-based, 0 = none
    QVector<double> noData;      // per band; NaN when not set
    bool hasGeoTransform = false;
    double geoTransform[6] = {0, 1, 0, 0, 0, 1};
    Crs crs;                     // invalid when the file has none
    QRectF extent;               // in the file's CRS (bounding box of the four corners)
    double pixelSizeX = 0;       // in CRS units
    double pixelSizeY = 0;

    static RasterInfo read(const QString &path);
    QString summary() const;     // "2493×2453, 4 канала (Byte), 5,0 см/пикс, EPSG:32615"
    QString brief() const;       // for a list: "5,0 см/пикс", or what is wrong ("CRS не задан")
};

// How a raster is drawn: three bands as RGB, or one band through a colour ramp
// (elevation models). Values are scaled from [min, max] of each used band.
struct RasterStyle
{
    enum Mode { Rgb, SingleBand };
    // Slope: flat green → steep red; Aspect: compass hues; Vegetation: soil → dense green; Visibility: hidden red, visible green;
    // Overlap: photos per point, 1 red … 9+ dark green (min 0, max 10)
    enum Ramp { Gray, Terrain, Spectral, Slope, Aspect, Vegetation, Visibility, Overlap };
    Mode mode = Rgb;
    int red = 1, green = 2, blue = 3;   // 1-based bands
    int alpha = 0;                      // alpha band, 0 = none
    int band = 1;                       // SingleBand
    Ramp ramp = Terrain;
    double min = 0, max = 255;          // SingleBand value range
    QVector<double> rgbMin = {0, 0, 0};
    QVector<double> rgbMax = {255, 255, 255};
    bool hillshade = false;             // SingleBand: multiply with a hillshade (elevation)

    static RasterStyle automatic(const RasterInfo &info, const QVector<QPair<double, double>> &bandRanges);
    QVariantMap toVariant() const;
    static RasterStyle fromVariant(const QVariantMap &map, const RasterStyle &fallback);
    static QVector<QPair<double, QColor>> rampStops(Ramp ramp);
    static QColor rampColor(Ramp ramp, double t);   // t in [0, 1]
};

// Tiles of one raster in the map CRS, rendered on any thread. GDAL handles are not shared
// between threads: each render takes a handle from a pool (opening one if needed), so tiles
// render in parallel. The source file is never modified; pyramids go to a cache folder.
class RasterSource
{
public:
    static constexpr int TileSize = 256;

    // `crsOverride` replaces the file's CRS (user-assigned when the file has none).
    RasterSource(const QString &path, const Crs &crsOverride, const Crs &mapCrs);
    virtual ~RasterSource();

    bool isValid() const { return m_valid; }
    QString error() const { return m_error; }
    QString renderPath() const;          // file or cached VRT with pyramids
    // Pixel grid in the map CRS (warped grid when the CRS differs or the file is rotated).
    int gridWidth() const { return m_gridWidth; }
    int gridHeight() const { return m_gridHeight; }
    const double *gridTransform() const { return m_gridTransform; }
    QRectF mapExtent() const;            // in the map CRS
    int maxLevel() const;                // coarsest level that still has > 1 pixel per tile

    void setStyle(const RasterStyle &style);
    RasterStyle style() const;
    // Switches to a cached VRT that has pyramids (after PyramidBuilder succeeded).
    void setPyramidPath(const QString &vrtPath);
    // The file was edited in place: later renders open fresh handles.
    void reload();

    // Tile (level, tx, ty) of the map-CRS grid: covers TileSize << level grid pixels.
    // Returns a null image for tiles outside the grid or on read errors.
    virtual QImage renderTile(int level, int tx, int ty) const;
    // Any region of the map (export): the map rectangle drawn into `size` pixels.
    virtual QImage renderRegion(const QRectF &mapRect, const QSize &size) const;
    // Value of `band` at a map point; NaN outside the raster or on nodata.
    virtual double valueAt(const QPointF &mapPoint, int band) const;

protected:
    RasterSource() = default;   // sources that are not files (online basemaps) fill the grid themselves

    struct Handle;
    Handle *acquire() const;
    void release(Handle *h) const;
    QImage render(void *dataset, int x0, int y0, int w, int h, int outW, int outH, const RasterStyle &style) const;

    QString m_path;
    QString m_pyramidPath;
    Crs m_sourceCrs;
    Crs m_mapCrs;
    bool m_warp = false;
    bool m_valid = false;
    mutable QString m_error;
    int m_gridWidth = 0;
    int m_gridHeight = 0;
    double m_gridTransform[6] = {0, 1, 0, 0, 0, 1};
    QVector<double> m_noData;
    mutable QMutex m_mutex;              // pool, style, pyramid path
    mutable QVector<Handle *> m_free;
    mutable int m_generation = 0;        // handles of older generations are closed on release
    RasterStyle m_style;
};

// Builds pyramids (overviews) for a large raster without touching the file: a VRT in the
// cache folder references the file and gets its overviews in <name>.vrt.ovr.
class PyramidBuilder
{
public:
    // Overviews are useful when the raster is larger than this and has none of its own.
    static bool needed(const RasterInfo &info);
    static QString cachePathFor(const QString &rasterPath, const QString &cacheDirectory);
    // Blocking; call on a worker thread. progress(0..1) returns false to cancel.
    static bool build(const QString &rasterPath, const QString &vrtPath,
                      const std::function<bool(double)> &progress, QString *error);
    static bool isComplete(const QString &vrtPath);
};
