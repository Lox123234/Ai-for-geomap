#pragma once

#include <QString>

// Size, georeferencing and CRS of a (Big)TIFF/GeoTIFF read from its first IFD and GeoKeys.
// A bounded parser for result panels; full raster support comes with GDAL (stage C).
struct GeoTiffInfo
{
    bool valid = false;         // a readable TIFF header and first IFD
    bool bigTiff = false;
    int width = 0;
    int height = 0;
    int bands = 0;
    bool georeferenced = false; // pixel scale and tie point present
    double pixelSizeX = 0;      // in CRS units (metres for projected systems)
    double pixelSizeY = 0;
    double originX = 0;         // CRS coordinates of the upper-left corner
    double originY = 0;
    int epsg = 0;               // ProjectedCSTypeGeoKey, else GeographicTypeGeoKey; 0 = unknown
    bool projected = false;
    QString error;

    static GeoTiffInfo read(const QString &path);
    QString describe() const;   // "EPSG:32630, 5,0 см/пикс, 2048×1536, каналов: 4"
};
