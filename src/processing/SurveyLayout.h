#pragma once

#include <QImage>
#include <QPolygonF>
#include <QRectF>
#include <QString>
#include <QVector>
#include <QtNumeric>
#include <cmath>

// The layout of a survey for its overview ("Схема съёмки"): where every photo was taken by its
// GPS and where the alignment of a job placed it, the error between the two, the photo's footprint
// on the ground (from the reconstruction, else from EXIF: altitude above take-off, focal length,
// heading), the flight path in capture order, and the job's orthophoto as the background. All in
// one projected CRS (the job's, else the UTM zone of the photos), metres.
namespace surveylayout {

struct PhotoIn
{
    int row = -1;           // row of the project's photo list
    QString name;           // DJI_0001.JPG
    QString path;           // absolute path of the project's copy (photos/<id>.jpg)
    QString takenAt;        // EXIF date (capture order)
    bool hasGps = false;
    double latitude = 0, longitude = 0;
};

struct Camera
{
    int row = -1;
    QString name;
    QString takenAt;
    bool hasGps = false;
    double gpsX = 0, gpsY = 0, gpsZ = qQNaN();      // EXIF GPS in the layout's CRS; Z: EXIF altitude
    bool aligned = false;                            // in the job's reconstruction
    double x = 0, y = 0, z = qQNaN();               // position after the alignment
    QPolygonF footprint;                             // on the ground; empty if unknown
    bool footprintAligned = false;                   // from the reconstruction (else from EXIF)
    bool hasError() const { return hasGps && aligned; }
    double dx() const { return x - gpsX; }
    double dy() const { return y - gpsY; }
    double dz() const { return std::isfinite(gpsZ) ? z - gpsZ : qQNaN(); }
};

struct Summary
{
    int photos = 0, withGps = 0, aligned = 0, notAligned = 0;
    int compared = 0;                                // aligned with GPS
    double rmseX = qQNaN(), rmseY = qQNaN(), rmseZ = qQNaN(), rmseXY = qQNaN(), rmse = qQNaN();   // metres
};

struct Layout
{
    QString error;                   // nothing to show and why
    QString crsWkt, crsLabel;
    QString job;                     // id of the job aligned; "" before any alignment
    QVector<Camera> cameras;         // in capture order
    double groundZ = qQNaN();        // footprints lie on the ground under each camera (DSM), else at this height
    QImage background;               // the job's orthophoto, reduced (null if none)
    QRectF backgroundRect;           // its extent in the layout's CRS
    QString backgroundName;
    QRectF extent;                   // of everything
    Summary summary;
    bool ok() const { return error.isEmpty(); }
};

// Reads the EXIF of the photos (footprints before alignment) and, if `jobDirectory` is given, its
// reconstruction (opensfm/reconstruction.json + odm_georeferencing/coords.txt), DSM and
// orthophoto (geomap/true_orthophoto.tif, else odm_orthophoto). `backgroundPixels`: longest side
// of the background image (0: none).
Layout compute(const QVector<PhotoIn> &photos, const QString &jobDirectory, int backgroundPixels = 2048);

// Height of the job's DSM at a point of its CRS; NaN outside it, on nodata or without a DSM.
double heightAt(const QString &jobDirectory, double x, double y);

} // namespace surveylayout
