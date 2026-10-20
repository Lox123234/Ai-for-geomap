#include "ResultNames.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

namespace resultnames {

QString titleOf(const QString &pathIn)
{
    const QString path = QDir::cleanPath(QDir::fromNativeSeparators(pathIn));
    // A result of a processing job: <project>/jobs/<yyyyMMdd-hhmmss>/<file>.
    static const QRegularExpression job(QStringLiteral("/jobs/(\\d{8}-\\d{6})/(.+)$"));
    const QRegularExpressionMatch m = job.match(path);
    if (m.hasMatch()) {
        const QString file = m.captured(2).toLower();
        static const QList<QPair<QString, QString>> known = {
            {QStringLiteral("geomap/true_orthophoto.tif"), QStringLiteral("Истинный ортофотоплан")},
            {QStringLiteral("odm_orthophoto/odm_orthophoto.tif"), QStringLiteral("Ортофотоплан ODM")},
            {QStringLiteral("odm_dem/dsm.tif"), QStringLiteral("DSM")},
            {QStringLiteral("odm_dem/dtm.tif"), QStringLiteral("DTM")},
            {QStringLiteral("odm_georeferencing/odm_georeferenced_model.laz"), QStringLiteral("Облако точек")},
            {QStringLiteral("odm_georeferencing/odm_georeferenced_model.las"), QStringLiteral("Облако точек")},
            {QStringLiteral("odm_filterpoints/point_cloud.ply"), QStringLiteral("Облако точек")},
            {QStringLiteral("opensfm/reconstruction.ply"), QStringLiteral("Связующие точки")},
            {QStringLiteral("geomap/tie_points.ply"), QStringLiteral("Связующие точки")},
            {QStringLiteral("opensfm/undistorted/openmvs/scene_dense_dense_filtered.ply"), QStringLiteral("Плотное облако")},
            {QStringLiteral("geomap/texture/odm_textured_model_geo.obj"), QStringLiteral("3D-модель (правка текстуры)")},
            {QStringLiteral("odm_texturing/odm_textured_model_geo.obj"), QStringLiteral("3D-модель")},
            {QStringLiteral("odm_texturing_25d/odm_textured_model_geo.obj"), QStringLiteral("2.5D-модель")},
            {QStringLiteral("opensfm/stats/overlap.tif"), QStringLiteral("Перекрытие снимков")},
            {QStringLiteral("geomap/true_surface.tif"), QStringLiteral("Поверхность истинного ортофотоплана")},
        };
        for (const auto &k : known)
            if (file == k.first) {
                const QDateTime when = QDateTime::fromString(m.captured(1), QStringLiteral("yyyyMMdd-hhmmss"));
                return when.isValid() ? QStringLiteral("%1 · %2").arg(k.second, when.toString(QStringLiteral("dd.MM HH:mm"))) : k.second;
            }
        return {};
    }
    // Project files: LiDAR surfaces, mosaics of chunks.
    const QFileInfo fi(path);
    const QString folder = fi.absoluteDir().dirName(), base = fi.completeBaseName();
    if (folder == QLatin1String("lidar") && (base.endsWith(QLatin1String("_dsm")) || base.endsWith(QLatin1String("_dtm"))))
        return QStringLiteral("%1 по LiDAR · %2").arg(base.right(3).toUpper(), base.left(base.size() - 4));
    if (folder == QLatin1String("mosaics")) {
        static const QRegularExpression mosaic(QStringLiteral("^(orthophoto|dsm|dtm)-(\\d{8}-\\d{6})$"));
        const QRegularExpressionMatch mm = mosaic.match(base);
        if (mm.hasMatch()) {
            const QString kind = mm.captured(1) == QLatin1String("orthophoto") ? QStringLiteral("Ортофотоплан") : mm.captured(1).toUpper();
            const QDateTime when = QDateTime::fromString(mm.captured(2), QStringLiteral("yyyyMMdd-hhmmss"));
            return QStringLiteral("%1 блоков · %2").arg(kind, when.toString(QStringLiteral("dd.MM HH:mm")));
        }
    }
    return {};
}

} // namespace resultnames
