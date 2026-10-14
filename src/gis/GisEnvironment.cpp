#include "GisEnvironment.h"
#include <QDir>
#include <QFileInfo>
#include <QMutex>
#include <cpl_conv.h>
#include <cpl_error.h>
#include <gdal.h>
#include <ogr_srs_api.h>
#include <proj.h>

namespace {
QMutex mutex;
bool initialized = false;
QString projData;
QStringList issues;

QString firstExisting(const QStringList &candidates, const QString &marker)
{
    for (const QString &c : candidates) {
        if (c.isEmpty()) continue;
        if (QFileInfo::exists(QDir(c).filePath(marker))) return QDir::cleanPath(QDir(c).absolutePath());
    }
    return {};
}
} // namespace

bool GisEnvironment::initialize(const QString &applicationDirectory)
{
    QMutexLocker lock(&mutex);
    if (initialized) return issues.isEmpty();
    initialized = true;

    // Messages go nowhere by default; callers read CPLGetLastErrorMsg() of their thread.
    CPLSetErrorHandler(CPLQuietErrorHandler);
    // Keep the raster block cache bounded; tiles are cached separately as images.
    CPLSetConfigOption("GDAL_CACHEMAX", "256");
    // Offline application: never try to fetch grids or files from the network.
    CPLSetConfigOption("PROJ_NETWORK", "OFF");
    CPLSetConfigOption("GDAL_HTTP_TIMEOUT", "1");
    // Auxiliary .aux.xml files are never written next to user data.
    CPLSetConfigOption("GDAL_PAM_ENABLED", "NO");

    const QString app = applicationDirectory;
    QStringList projCandidates = {qEnvironmentVariable("PROJ_DATA"), qEnvironmentVariable("PROJ_LIB")};
    if (!app.isEmpty()) projCandidates << QDir(app).filePath(QStringLiteral("share/proj"));
#ifdef GEOMAP_PROJ_DATA
    projCandidates << QStringLiteral(GEOMAP_PROJ_DATA);
#endif
    projCandidates << QStringLiteral("/usr/share/proj") << QStringLiteral("/usr/local/share/proj");
    projData = firstExisting(projCandidates, QStringLiteral("proj.db"));
    if (projData.isEmpty()) {
        issues << QStringLiteral("Не найдена база систем координат PROJ (proj.db): пересчёт координат недоступен.");
    } else {
        const QByteArray path = QDir::toNativeSeparators(projData).toUtf8();
        const char *paths[] = {path.constData(), nullptr};
        OSRSetPROJSearchPaths(paths);
        const char *defaultPaths[] = {path.constData()};
        proj_context_set_search_paths(nullptr, 1, defaultPaths);
    }

    QStringList gdalCandidates = {qEnvironmentVariable("GDAL_DATA")};
    if (!app.isEmpty()) gdalCandidates << QDir(app).filePath(QStringLiteral("share/gdal"));
#ifdef GEOMAP_GDAL_DATA
    gdalCandidates << QStringLiteral(GEOMAP_GDAL_DATA);
#endif
    gdalCandidates << QStringLiteral("/usr/share/gdal");
    const QString gdalData = firstExisting(gdalCandidates, QStringLiteral("gdalvrt.xsd"));
    if (!gdalData.isEmpty()) CPLSetConfigOption("GDAL_DATA", QDir::toNativeSeparators(gdalData).toUtf8().constData());

    GDALAllRegister();
    return issues.isEmpty();
}

bool GisEnvironment::isInitialized()
{
    QMutexLocker lock(&mutex);
    return initialized;
}

QString GisEnvironment::projDataDirectory()
{
    QMutexLocker lock(&mutex);
    return projData;
}

QStringList GisEnvironment::problems()
{
    QMutexLocker lock(&mutex);
    return issues;
}

QString GisEnvironment::gdalVersion()
{
    return QString::fromLatin1(GDALVersionInfo("RELEASE_NAME"));
}

QString GisEnvironment::projVersion()
{
    const PJ_INFO info = proj_info();
    return QString::fromLatin1(info.version);
}

QString GisEnvironment::lastError()
{
    return QString::fromUtf8(CPLGetLastErrorMsg()).trimmed();
}

void GisEnvironment::clearError()
{
    CPLErrorReset();
}
