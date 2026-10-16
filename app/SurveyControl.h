#pragma once

#include "Crs.h"

#include <QHash>
#include <QMap>
#include <QObject>
#include <QPointF>
#include <QSize>
#include <QUrl>
#include <QVariantList>
#include <QVector>
#include <memory>

class MapService;
class PhotoModel;
namespace ortho { struct Scene; }

// Control of a survey (<project>/gcp.json): ground control points with surveyed coordinates and
// their positions on the photos, check points that only measure the accuracy, the camera
// calibration reused from an earlier job and scale bars between markers. Each job receives the
// control points seen on ≥ 3 of its photos as gcp_list.txt and the calibration as initial camera
// parameters (OpenDroneMap --gcp, --cameras). After a job every point is triangulated from its
// photo positions and the job's camera poses: the residuals and RMSE per axis say how accurate
// the reconstruction is (check points independently of the fit).
class SurveyControl final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList points READ pointsForQml NOTIFY changed)
    Q_PROPERTY(QString crsLabel READ crsLabel NOTIFY changed)
    Q_PROPERTY(int controlCount READ controlCount NOTIFY changed)
    Q_PROPERTY(int checkCount READ checkCount NOTIFY changed)
    Q_PROPERTY(QString calibration READ calibration NOTIFY changed)   // job of the calibration; "" = each job computes its own
    Q_PROPERTY(QVariantList scaleBars READ scaleBarsForQml NOTIFY changed)

public:
    struct Point
    {
        QString id, name;
        double x = 0, y = 0, z = 0;          // in the points' CRS
        bool check = false;                  // a check point: not given to the engine
        QMap<QString, QPointF> observations; // photo file ("photos/<id>.jpg") → position in [0, 1] of the displayed photo
    };
    struct ScaleBar
    {
        QString id, markerA, markerB;
        double distance = 0;                 // metres, measured on the ground
    };

    explicit SurveyControl(QObject *parent = nullptr);
    ~SurveyControl() override;
    void setProject(const QString &directory, const PhotoModel *photos = nullptr, const MapService *map = nullptr);

    const QVector<Point> &controlPoints() const { return m_points; }
    Crs crs() const { return m_crs; }
    QString crsLabel() const { return m_crs.isValid() ? m_crs.label() : QString(); }
    int controlCount() const;
    int checkCount() const;
    QVariantList pointsForQml() const;
    QString calibration() const { return m_calibration; }
    QVariantList scaleBarsForQml() const;

    // CRS of the points (EPSG code, WKT or PROJ); the points keep their numbers. "" or the reason.
    Q_INVOKABLE QString setCrs(const QString &crsText);
    // CSV/TXT "name x y z" (comma, semicolon, tab or spaces; header lines are skipped; with
    // northFirst the second column is the northing or latitude, as in Russian surveys) or an
    // ODM gcp_list.txt (CRS line, then x y z px py image [name]) with the photo positions.
    // Returns {error, added, observations}.
    Q_INVOKABLE QVariantMap importFile(const QUrl &url, const QString &crsText = {}, bool northFirst = false);
    Q_INVOKABLE QString addPoint(const QString &name, double x, double y, double z);   // new id
    Q_INVOKABLE void removePoint(const QString &id);
    Q_INVOKABLE void renamePoint(const QString &id, const QString &name);
    Q_INVOKABLE void setCheck(const QString &id, bool check);
    Q_INVOKABLE void setCoordinates(const QString &id, double x, double y, double z);
    // The point at (nx, ny) in [0, 1] of the displayed photo.
    Q_INVOKABLE void setObservation(const QString &id, const QString &photoFile, double nx, double ny);
    Q_INVOKABLE void removeObservation(const QString &id, const QString &photoFile);
    // On one photo: {id, name, check, nx, ny, placed: true} of the placed points and, from the
    // newest job with a reconstruction, {…, placed: false} where the others should be.
    Q_INVOKABLE QVariantList marksOnPhoto(const QString &photoFile) const;
    // Triangulation of every point with ≥ 2 photo positions in the job (newest with a
    // reconstruction when empty): {error, job, gcpUsed, rows[{name, check, photos, dx, dy, dz,
    // d, reprojection}], control{rmseX, rmseY, rmseZ, rmse, count}, check{…}}.
    Q_INVOKABLE QVariantMap accuracy(const QString &jobId = {}) const;
    Q_INVOKABLE QString exportGcpList(const QUrl &url) const;   // "" or the reason
    Q_INVOKABLE QString exportAccuracy(const QUrl &url, const QString &jobId = {}) const;   // CSV

    // Calibration: camera groups of the photos and the calibrations computed by finished jobs.
    Q_INVOKABLE QVariantList cameras() const;            // {model, width, height, count, calibrated}
    Q_INVOKABLE QVariantList jobCalibrations() const;    // {job, date, used, cameras[{id, focalPx, cx, cy, k1, k2, k3, p1, p2}]}
    Q_INVOKABLE QString useCalibration(const QString &jobId);   // "" or the reason
    Q_INVOKABLE void clearCalibration();

    // Scale bars between two markers with a distance measured on the ground: the map's
    // length between them and its error.
    Q_INVOKABLE QString addScaleBar(const QString &markerA, const QString &markerB, double distance);
    Q_INVOKABLE void removeScaleBar(const QString &id);

    // What a job takes from the project (ProcessingService, worker thread): gcp_list.txt with the
    // control points seen on ≥ 3 of its photos (≥ 3 such points, else no file) and the calibration
    // as geomap_cameras.json; files of an earlier run are replaced or removed.
    struct JobInputs
    {
        int points = 0;              // control points on ≥ 3 of the job's photos
        bool gcpUsed = false;        // gcp_list.txt written (≥ 3 such points)
        QStringList dropped;         // control points on fewer than 3 of the job's photos
        QString calibration;         // job the calibration came from; "" = none
        QString error;
    };
    static JobInputs prepareJob(const QString &project, const QString &jobDirectory, const QStringList &images);
    static QString calibrationPath(const QString &project);

signals:
    void changed();

private:
    struct Reconstruction;
    void load();
    void save();
    QString newId() const;
    Point *find(const QString &id);
    QString newestReconstructedJob() const;
    const Reconstruction *reconstruction(const QString &jobId) const;
    QSize photoSize(const QString &photoFile) const;
    QString m_project;
    const PhotoModel *m_photos = nullptr;
    const MapService *m_map = nullptr;
    Crs m_crs;
    QVector<Point> m_points;
    QVector<ScaleBar> m_bars;
    QString m_calibration;
    mutable std::shared_ptr<Reconstruction> m_reconstruction;   // cache of the last job read
    mutable QHash<QString, QSize> m_sizes;
};
