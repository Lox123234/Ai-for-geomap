#include "AppInfo.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
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

QUrl AppInfo::dialogFolder(const QString &projectPath, const QString &kind) const
{
    const QString remembered = QSettings().value(QStringLiteral("dialogs/") + kind).toString();
    if (!kind.isEmpty() && !remembered.isEmpty() && QFileInfo(remembered).isDir()) return QUrl::fromLocalFile(remembered);
    if (kind != QLatin1String("photos") && kind != QLatin1String("projects") && !projectPath.isEmpty() && QFileInfo(projectPath).isDir())
        return QUrl::fromLocalFile(projectPath);
    const QStandardPaths::StandardLocation where = kind == QLatin1String("photos") ? QStandardPaths::PicturesLocation
                                                                                     : QStandardPaths::DocumentsLocation;
    const QString folder = QStandardPaths::writableLocation(where);
    return QUrl::fromLocalFile(QFileInfo(folder).isDir() ? folder : QDir::homePath());
}

void AppInfo::rememberFolder(const QString &kind, const QUrl &chosen) const
{
    if (kind.isEmpty() || !chosen.isLocalFile()) return;
    const QFileInfo fi(chosen.toLocalFile());
    const QString folder = fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath();   // a file: its folder
    if (QFileInfo(folder).isDir()) QSettings().setValue(QStringLiteral("dialogs/") + kind, folder);
}

QString AppInfo::siblingLas(const QString &path) const
{
    if (path.endsWith(QLatin1String(".las"), Qt::CaseInsensitive)) return QFileInfo(path).isFile() ? path : QString();
    if (!path.endsWith(QLatin1String(".laz"), Qt::CaseInsensitive)) return {};
    const QString las = path.left(path.size() - 4) + QStringLiteral(".las");
    return QFileInfo(las).isFile() ? las : QString();
}
