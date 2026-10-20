#pragma once

#include <QString>
#include <QStringList>
#include <atomic>
#include <cmath>

// Обработка → Оптимизация камер: bundle adjustment of an aligned job's reconstruction with the
// project's control points, by OpenSfM of the bundled OpenDroneMap (optimize_cameras.py, as OpenSfM
// ends its own alignment), then the job's results are built again from the new camera poses.
namespace cameraoptimizer {

struct Errors
{
    double reprojectionPx = qQNaN();
    double gpsM = qQNaN();          // average error of the cameras against their GPS
    double gcpM = qQNaN();          // average error of the control points
    int points = 0;
};

struct Result
{
    QString error;
    Errors before, after;
    int controlPoints = 0;          // used in the bundle
    QString summary() const;        // "репроекция 1,00 → 1,02 пикс · опорные точки 2,00 → 0,003 м"
    bool ok() const { return error.isEmpty(); }
};

// In <job>/opensfm: the control points of <job>/gcp_list.txt (if any) for OpenSfM, the
// reconstruction bundled in its local frame, the statistics before and after; on failure the
// reconstruction is restored. `refineCalibration`: the camera parameters are optimised too.
Result run(const QString &jobDirectory, bool refineCalibration, const std::atomic_bool *cancel = nullptr);

// What ODM computes again after the cameras moved (relative to the job folder): removed before the
// job continues, so that its stages run again from the undistortion on.
QStringList staleOutputs();
// Removes them; "" or the first file that could not be removed.
QString removeStale(const QString &jobDirectory);

} // namespace cameraoptimizer
