#include "AppInfo.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QUrl>

AppInfo::AppInfo(const QString &logDirectory, QObject *parent) : QObject(parent), m_logDirectory(logDirectory) {}

QString AppInfo::version() const
{
    return QCoreApplication::applicationVersion();
}

QString AppInfo::qtVersion() const
{
    return QString::fromLatin1(qVersion());
}

QString AppInfo::docsDirectory() const
{
    QStringList candidates{QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("docs"))};
#ifdef GEOMAP_SOURCE_DIR
    candidates << QStringLiteral(GEOMAP_SOURCE_DIR "/docs");
#endif
    for (const QString &dir : candidates)
        if (QFileInfo::exists(QDir(dir).filePath(QStringLiteral("ARCHITECTURE.md")))) return QDir::cleanPath(dir);
    return {};
}

QString AppInfo::docPath(const QString &name) const
{
    const QString dir = docsDirectory();
    if (dir.isEmpty() || name.contains(QLatin1String(".."))) return {};
    const QString path = QDir(dir).filePath(name);
    return QFileInfo::exists(path) ? path : QString();
}

bool AppInfo::openPath(const QString &path) const
{
    return !path.isEmpty() && QFileInfo::exists(path) && QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

bool AppInfo::openLogDirectory() const
{
    return !m_logDirectory.isEmpty() && QDir().mkpath(m_logDirectory) && openPath(m_logDirectory);
}

bool AppInfo::fileExists(const QString &path) const
{
    return !path.isEmpty() && QFileInfo(path).isFile();
}

QString AppInfo::siblingLas(const QString &path) const
{
    if (path.endsWith(QLatin1String(".las"), Qt::CaseInsensitive)) return QFileInfo(path).isFile() ? path : QString();
    if (!path.endsWith(QLatin1String(".laz"), Qt::CaseInsensitive)) return {};
    const QString las = path.left(path.size() - 4) + QStringLiteral(".las");
    return QFileInfo(las).isFile() ? las : QString();
}
