#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

// True orthophoto from a finished reconstruction: every output pixel is taken directly from the
// original photo that sees it most vertically, at the height of the surface model (DSM), with a
// visibility check (z-buffer of the DSM per photo). Unlike a texture draped over a smoothed
// mesh, crowns and roofs are not smeared: each object higher than `objectHeight` above the
// ground is taken whole from one photo (no seam through a tree), the ground pixel by pixel with
// soft transitions, and the photos' brightness is balanced on their overlaps.
namespace ortho {

// OpenSfM camera (normalised by max(width, height); Brown distortion).
struct Camera
{
    int width = 0, height = 0;
    double fx = 0, fy = 0, cx = 0, cy = 0, k1 = 0, k2 = 0, k3 = 0, p1 = 0, p2 = 0;
};

struct Shot
{
    QString image;       // path of the photo
    QString name;        // its name in the project (DJI_0001.JPG), else the file name
    Camera camera;
    double R[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};   // X_camera = R · X_local + t
    double t[3] = {0, 0, 0};
    double centre[3] = {0, 0, 0};                // -Rᵀt
    bool project(const double local[3], double *px, double *py, double *depth) const;   // pixels of the photo
    void ray(double px, double py, double dir[3]) const;   // inverse of project(): unit direction of a pixel
};

struct Scene
{
    QVector<Shot> shots;
    double offsetX = 0, offsetY = 0;   // local frame + offset = coordinates of the DSM's CRS
};

// The photos and their poses from an ODM job folder: opensfm/reconstruction.json (+ the offset
// from odm_georeferencing/coords.txt), photos from images/ (or `photoFolder`). Without
// `requirePhotos` the poses are read even when photos are missing (the 3D view of the cameras).
bool readOdmJob(const QString &jobDirectory, Scene *scene, QString *error, const QString &photoFolder = {},
                bool requirePhotos = true);
// WKT of the CRS the job's results and its shifted reconstruction are in (from its DSM or
// orthophoto, else the "WGS84 UTM 32N" line of coords.txt); "" if unknown.
QString resultCrsWkt(const QString &jobDirectory);

struct Options
{
    double resolution = 0;        // metres per pixel; 0 = the ground sample distance of the photos
    double objectHeight = 2.0;    // objects (trees, buildings) above the ground taken from one photo
    QString groundPath;           // DTM for object heights; empty = estimated from the DSM
    QString pointCloudPath;       // dense point cloud (ODM point_cloud.ply, local frame) for the crowns' surface
    bool balanceColours = true;
    double blendDegrees = 3.0;    // ground: two photos whose view angles differ less than this are blended
    QString sourceMapPath;        // optional: Byte GeoTIFF, the photo of every pixel (index; 255 none) — seams at a glance
    QString surfacePath;          // optional: the surface built from the point cloud (Float32 GeoTIFF) for the 3D view
    // optional: Byte GeoTIFF on the grid of the orthophoto, 255 on its streaks — texture stretched
    // along the view of the photo used, where it saw the surface almost edge-on: nothing to measure
    // there. The input of the AI retouch (ai::retouch).
    QString stretchMaskPath;
    std::function<bool(double)> progress;   // 0..1; false cancels
};

struct Result
{
    QString error;
    int width = 0, height = 0;
    double resolution = 0;        // metres per pixel
    double groundSampleDistance = 0;
    int photos = 0, objects = 0;
    double coverage = 0;          // share of DSM pixels with colour
    double hidden = 0;            // share of them hidden from every photo by the DSM (taken from the most vertical one)
    double stretched = 0;         // share of them seen almost edge-on (see Options::stretchMaskPath)
    double seconds = 0;
    QVector<double> gains;        // brightness factor per photo (balanced)
    double pointCloudShare = 0;   // share of the surface taken from the dense point cloud
    QStringList notes;            // what could not be used (the point cloud), the result is still valid
    bool ok() const { return error.isEmpty(); }
};

// Writes an RGBA GeoTIFF (tiled, DEFLATE, overviews) in the CRS and extent of the DSM.
Result build(const Scene &scene, const QString &dsmPath, const QString &outputPath, const Options &options = {});

} // namespace ortho
