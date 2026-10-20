#include "CameraOptimizer.h"
#include "OdmEngine.h"
#include "OdmTools.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>

namespace cameraoptimizer {

namespace {
QString num(double v, int d) { return QLocale(QLocale::Russian).toString(v, 'f', d); }

Errors readStats(const QString &statsJson)
{
    Errors e;
    QFile f(statsJson);
    if (!f.open(QIODevice::ReadOnly)) return e;
    const QJsonObject s = QJsonDocument::fromJson(f.readAll()).object();
    const QJsonObject rec = s.value(QStringLiteral("reconstruction_statistics")).toObject();
    if (rec.contains(QStringLiteral("reprojection_error_pixels"))) e.reprojectionPx = rec.value(QStringLiteral("reprojection_error_pixels")).toDouble();
    e.points = rec.value(QStringLiteral("reconstructed_points_count")).toInt();
    const QJsonObject gps = s.value(QStringLiteral("gps_errors")).toObject(), gcp = s.value(QStringLiteral("gcp_errors")).toObject();
    if (gps.contains(QStringLiteral("average_error"))) e.gpsM = gps.value(QStringLiteral("average_error")).toDouble();
    if (gcp.contains(QStringLiteral("average_error"))) e.gcpM = gcp.value(QStringLiteral("average_error")).toDouble();
    return e;
}

bool copyOver(const QString &from, const QString &to)
{
    QFile::remove(to);
    return QFile::copy(from, to);
}
} // namespace

QString Result::summary() const
{
    QStringList parts;
    if (std::isfinite(before.reprojectionPx) && std::isfinite(after.reprojectionPx))
        parts << QStringLiteral("репроекция %1 → %2 пикс").arg(num(before.reprojectionPx, 2), num(after.reprojectionPx, 2));
    if (std::isfinite(before.gcpM) && std::isfinite(after.gcpM))
        parts << QStringLiteral("опорные точки %1 → %2 м").arg(num(before.gcpM, 3), num(after.gcpM, 3));
    if (std::isfinite(before.gpsM) && std::isfinite(after.gpsM))
        parts << QStringLiteral("GPS камер %1 → %2 м").arg(num(before.gpsM, 2), num(after.gpsM, 2));
    return parts.join(QStringLiteral(" · "));
}

QStringList staleOutputs()
{
    return {QStringLiteral("opensfm/reconstruction.topocentric.json"), QStringLiteral("opensfm/undistorted"),
            QStringLiteral("opensfm/reconstruction.ply"), QStringLiteral("opensfm/updated_config.txt"), QStringLiteral("cameras.json"),
            QStringLiteral("odm_filterpoints"), QStringLiteral("odm_meshing"), QStringLiteral("odm_texturing"),
            QStringLiteral("odm_texturing_25d"), QStringLiteral("odm_georeferencing/odm_georeferenced_model.laz"),
            QStringLiteral("odm_georeferencing/odm_georeferenced_model.las"), QStringLiteral("odm_georeferencing/odm_georeferenced_model.bounds.gpkg"),
            QStringLiteral("odm_georeferencing/odm_georeferenced_model.bounds.geojson"), QStringLiteral("odm_georeferencing/odm_georeferenced_model.boundary.json"),
            QStringLiteral("odm_georeferencing/odm_georeferenced_model.info.json"), QStringLiteral("odm_georeferencing/odm_georeferenced_model.summary.json"),
            QStringLiteral("odm_dem"), QStringLiteral("odm_orthophoto"), QStringLiteral("odm_report"), QStringLiteral("3d_tiles"),
            QStringLiteral("entwine_pointcloud"), QStringLiteral("geomap/true_orthophoto.tif"), QStringLiteral("geomap/true_surface.tif"),
            QStringLiteral("geomap/true_orthophoto_stretch.tif"), QStringLiteral("geomap/report.pdf"), QStringLiteral("geomap/tie_points.ply"), QStringLiteral("geomap/texture")};
}

QString removeStale(const QString &jobDirectory)
{
    const QDir dir(jobDirectory);
    for (const QString &rel : staleOutputs()) {
        const QString path = dir.filePath(rel);
        const QFileInfo fi(path);
        if (!fi.exists()) continue;
        const bool removed = fi.isDir() ? QDir(path).removeRecursively() : QFile::remove(path);
        if (!removed) return QDir::toNativeSeparators(path);
    }
    return {};
}

Result run(const QString &jobDirectory, bool refineCalibration, const std::atomic_bool *cancel)
{
    Result r;
    const QDir job(jobDirectory), osfm(job.filePath(QStringLiteral("opensfm")));
    const QString recon = osfm.filePath(QStringLiteral("reconstruction.json"));
    if (!QFileInfo::exists(recon)) {
        r.error = QStringLiteral("В задании нет выравнивания");
        return r;
    }
    QString e;
    const QString odm = odmtools::root(&e);
    if (odm.isEmpty()) {
        r.error = e;
        return r;
    }
    const QString package = QDir(odmtools::binDirectory()).filePath(QStringLiteral("opensfm"));
    // The script from the program's resources into an ASCII folder.
    const QString work = odmtools::workFolder(&e);
    if (work.isEmpty()) {
        r.error = e;
        return r;
    }
    const QString script = QDir(work).filePath(QStringLiteral("optimize_cameras.py"));
    if (!QFile::copy(QStringLiteral(":/geomap/scripts/optimize_cameras.py"), script)) {
        QDir(work).removeRecursively();
        r.error = QStringLiteral("Нет сценария оптимизации в составе программы");
        return r;
    }
    QFile::setPermissions(script, QFile::ReadOwner | QFile::WriteOwner);
    // Backups: the reconstruction ODM left (georeferenced), its local twin, the statistics.
    const QDir backup(job.filePath(QStringLiteral("geomap/optimize")));
    QDir().mkpath(backup.path());
    const QString topocentric = osfm.filePath(QStringLiteral("reconstruction.topocentric.json"));
    const QString stats = osfm.filePath(QStringLiteral("stats/stats.json"));
    copyOver(recon, backup.filePath(QStringLiteral("reconstruction.json")));
    if (QFileInfo::exists(topocentric)) copyOver(topocentric, backup.filePath(QStringLiteral("reconstruction.topocentric.json")));
    if (QFileInfo::exists(stats)) copyOver(stats, backup.filePath(QStringLiteral("stats.json")));
    auto restore = [&](const QString &why) {
        copyOver(backup.filePath(QStringLiteral("reconstruction.json")), recon);
        if (QFileInfo::exists(backup.filePath(QStringLiteral("stats.json")))) copyOver(backup.filePath(QStringLiteral("stats.json")), stats);
        QDir(work).removeRecursively();
        r.error = why;
        return r;
    };
    // OpenSfM bundles in its local frame (the georeferenced copy comes back from ODM afterwards).
    if (QFileInfo::exists(topocentric) && !copyOver(topocentric, recon)) return restore(QStringLiteral("Не удалось подготовить реконструкцию"));
    // The control points of the project, as the job took them.
    const QString gcp = job.filePath(QStringLiteral("gcp_list.txt")), osfmGcp = osfm.filePath(QStringLiteral("gcp_list.txt"));
    QFile::remove(osfmGcp);
    if (QFileInfo::exists(gcp)) {
        QFile::copy(gcp, osfmGcp);
        QFile in(gcp);
        if (in.open(QIODevice::ReadOnly)) {
            QStringList names;
            const QList<QByteArray> lines = in.readAll().split('\n');
            for (int i = 1; i < lines.size(); ++i) {
                const QList<QByteArray> f = lines[i].simplified().split(' ');
                if (f.size() >= 7 && !names.contains(QString::fromUtf8(f[6]))) names << QString::fromUtf8(f[6]);
                else if (f.size() == 6) names << QString::fromUtf8(f[0] + f[1] + f[2]);
            }
            names.removeDuplicates();
            r.controlPoints = int(names.size());
        }
    }
    const bool withGcp = r.controlPoints > 0;
    auto cancelled = [cancel] { return cancel && cancel->load(); };
    const QString python = OdmEngine::nativePython(odm);
    const QString main = QDir(package).filePath(QStringLiteral("bin/opensfm_main.py"));
    // Before: the statistics of the reconstruction as it is, with the control points.
    odmtools::Result s = odmtools::run(python, {QStringLiteral("-X"), QStringLiteral("utf8"), QDir::toNativeSeparators(main),
                                                QStringLiteral("compute_statistics"), odmtools::toolPath(osfm.path())}, 0, {}, cancelled);
    r.before = s.ok() ? readStats(stats) : readStats(backup.filePath(QStringLiteral("stats.json")));
    if (cancelled()) return restore(QStringLiteral("Отменено"));
    const odmtools::Result b = odmtools::run(python, {QStringLiteral("-X"), QStringLiteral("utf8"), QDir::toNativeSeparators(script),
                                                      odmtools::toolPath(osfm.path()), withGcp ? QStringLiteral("1") : QStringLiteral("0"),
                                                      refineCalibration ? QStringLiteral("1") : QStringLiteral("0"),
                                                      QDir::toNativeSeparators(package)},
                                             0, {}, cancelled);
    if (!b.ok() || !b.output.contains(QLatin1String("GEOMAP_RESULT"))) return restore(b.error.isEmpty() ? QStringLiteral("OpenSfM не завершил уравнивание") : b.error);
    s = odmtools::run(python, {QStringLiteral("-X"), QStringLiteral("utf8"), QDir::toNativeSeparators(main), QStringLiteral("compute_statistics"),
                               odmtools::toolPath(osfm.path())}, 0, {}, cancelled);
    if (!s.ok()) return restore(s.error);
    r.after = readStats(stats);
    QDir(work).removeRecursively();
    return r;
}

} // namespace cameraoptimizer
