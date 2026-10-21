#pragma once

#include "ImportJob.h"
#include "PhotoModel.h"
#include <QElapsedTimer>
#include <QObject>
#include <QStringList>
#include <QVariantList>
#include <QUrl>
#include <memory>

class QLockFile;

class ProjectManager final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool hasProject READ hasProject NOTIFY projectChanged)
    Q_PROPERTY(QString projectName READ projectName NOTIFY projectChanged)
    Q_PROPERTY(QString projectPath READ projectPath NOTIFY projectChanged)
    Q_PROPERTY(int photoCount READ photoCount NOTIFY photosChanged)
    Q_PROPERTY(int gpsCount READ gpsCount NOTIFY photosChanged)
    Q_PROPERTY(int selectedIndex READ selectedIndex NOTIFY selectionChanged)
    Q_PROPERTY(QVariantMap selectedPhoto READ selectedPhoto NOTIFY selectionChanged)
    Q_PROPERTY(QVariantList geoPoints READ geoPoints NOTIFY photosChanged)
    Q_PROPERTY(QStringList recentProjects READ recentProjects NOTIFY recentProjectsChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(PhotoModel* photoModel READ photoModel CONSTANT)
    // Background import (valid while busy; the counters keep the last result afterwards).
    Q_PROPERTY(bool importScanning READ importScanning NOTIFY importProgressChanged)
    Q_PROPERTY(int importTotal READ importTotal NOTIFY importProgressChanged)
    Q_PROPERTY(int importProcessed READ importProcessed NOTIFY importProgressChanged)
    Q_PROPERTY(QString importCurrentFile READ importCurrentFile NOTIFY importProgressChanged)
    Q_PROPERTY(bool importCancelling READ importCancelling NOTIFY importProgressChanged)
    Q_PROPERTY(QStringList importIssues READ importIssues NOTIFY importIssuesChanged)
    Q_PROPERTY(QString description READ description WRITE setDescription NOTIFY projectChanged)
    Q_PROPERTY(QString createdAt READ createdAt NOTIFY projectChanged)
public:
    explicit ProjectManager(QObject *parent = nullptr);
    ~ProjectManager() override;
    bool hasProject() const { return !m_projectPath.isEmpty(); }
    QString projectName() const { return m_projectName; }
    QString projectPath() const { return m_projectPath; }
    int photoCount() const { return m_photos.rowCount(); }
    int gpsCount() const;
    int selectedIndex() const { return m_selectedIndex; }
    QVariantMap selectedPhoto() const { return m_photos.details(m_selectedIndex); }
    QVariantList geoPoints() const;
    QStringList recentProjects() const { return m_recentProjects; }
    QString statusMessage() const { return m_status; }
    bool busy() const { return m_busy; }
    PhotoModel *photoModel() { return &m_photos; }
    bool importScanning() const { return m_importProgress.scanning; }
    int importTotal() const { return m_importProgress.total; }
    int importProcessed() const { return m_importProgress.processed; }
    QString importCurrentFile() const { return m_importProgress.currentFile; }
    bool importCancelling() const { return m_import && m_import->isCancelled(); }
    QStringList importIssues() const { return m_importIssues; }

    Q_INVOKABLE bool createProject(const QUrl &parentDirectory, const QString &name);
    Q_INVOKABLE bool openProject(const QUrl &projectDirectory);
    Q_INVOKABLE bool openRecent(const QString &directory);
    Q_INVOKABLE bool saveProject();
    Q_INVOKABLE void closeProject();
    // QList<QUrl> matches FileDialog.selectedFiles; a QVariantList parameter receives
    // each url double-wrapped (QVariant(QVariant(QUrl))) from QML in Qt 6.8.
    Q_INVOKABLE void importFiles(const QList<QUrl> &files);
    Q_INVOKABLE void importFolder(const QUrl &folder);
    // Stops after the file being copied; everything imported so far is kept and saved.
    Q_INVOKABLE void cancelImport();
    Q_INVOKABLE void selectPhoto(int index);
    Q_INVOKABLE bool removeSelectedPhoto();
    // Flight data of a photo from its XMP (read from the file), as short texts for the photo's
    // properties: {height, gimbal, drone, gnss, rtk (DJI flag, −1 none)}; empty texts when absent.
    Q_INVOKABLE QVariantMap flightInfo(int row) const;
    // A list read through a QML property stays bound to it: every element access calls
    // geoPoints() again (O(n) each). Loops must take this detached copy instead.
    Q_INVOKABLE QVariantList geoPointsSnapshot() const { return geoPoints(); }
    Q_INVOKABLE void revealProject() const;
    Q_INVOKABLE QString displayPath(const QUrl &url) const;

    QString description() const { return m_description; }
    void setDescription(const QString &text);
    QString createdAt() const { return m_createdAt; }
    // The name shown for the project (the folder keeps its name). "" or the reason.
    Q_INVOKABLE QString renameProject(const QString &name);
    // Size of what saveProjectAs copies (everything but the lock and the cache), bytes.
    Q_INVOKABLE qint64 projectSize() const;
    // Copies the open project into <parentDirectory>/<name> in the background (not the lock and
    // the cache) and opens the copy; the original stays as it was. "" when started, else the reason.
    Q_INVOKABLE QString saveProjectAs(const QUrl &parentDirectory, const QString &name);
    // Read-only integrity check: {ok, checked[], problems[], warnings[]}.
    Q_INVOKABLE QVariantMap checkProject() const;

signals:
    // Emitted before the photo model resets, so image providers resolve the new
    // project when the delegates of the new model request their thumbnails.
    void projectDirectoryChanged(const QString &directory);
    void projectChanged();
    void photosChanged();
    void selectionChanged();
    void recentProjectsChanged();
    void statusChanged();
    void busyChanged();
    void errorOccurred(const QString &error);
    void importProgressChanged();
    void importIssuesChanged();
    void importFinished();
    void saveAsFinished(bool ok, const QString &message);

private:
    void updateStatus(const QString &status);
    void error(const QString &message);
    void setBusy(bool value);
    void updateRecents(const QString &path);
    void importPaths(const QStringList &files);
    void startImport(ImportJob::Request request);
    void collectImportResults();
    void finishImport();
    void clearImportIssues();
    void setProjectPath(const QString &path);
    bool acquireLock(const QString &path, std::unique_ptr<QLockFile> &lock);
    bool writeProject();
    PhotoModel m_photos;
    QString m_projectPath;
    QString m_projectName;
    QString m_createdAt;
    QString m_description;
    QStringList m_recentProjects;
    QString m_status = QStringLiteral("Готов к работе");
    int m_selectedIndex = -1;
    bool m_busy = false;
    std::unique_ptr<QLockFile> m_lock;
    ImportJob *m_import = nullptr;
    ImportJob::Progress m_importProgress;
    QStringList m_importIssues;
    QElapsedTimer m_lastCheckpoint;
};
