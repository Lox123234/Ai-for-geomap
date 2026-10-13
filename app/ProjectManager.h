#pragma once

#include "PhotoModel.h"
#include <QObject>
#include <QStringList>
#include <QVariantList>
#include <QUrl>

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
public:
    explicit ProjectManager(QObject *parent = nullptr);
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

    Q_INVOKABLE bool createProject(const QUrl &parentDirectory, const QString &name);
    Q_INVOKABLE bool openProject(const QUrl &projectDirectory);
    Q_INVOKABLE bool openRecent(const QString &directory);
    Q_INVOKABLE bool saveProject();
    Q_INVOKABLE void closeProject();
    // QList<QUrl> matches FileDialog.selectedFiles; a QVariantList parameter receives
    // each url double-wrapped (QVariant(QVariant(QUrl))) from QML in Qt 6.8.
    Q_INVOKABLE void importFiles(const QList<QUrl> &files);
    Q_INVOKABLE void importFolder(const QUrl &folder);
    Q_INVOKABLE void selectPhoto(int index);
    Q_INVOKABLE bool removeSelectedPhoto();
    Q_INVOKABLE void revealProject() const;
    Q_INVOKABLE QString displayPath(const QUrl &url) const;

signals:
    void projectChanged();
    void photosChanged();
    void selectionChanged();
    void recentProjectsChanged();
    void statusChanged();
    void busyChanged();
    void errorOccurred(const QString &error);

private:
    void updateStatus(const QString &status);
    void error(const QString &message);
    void setBusy(bool value);
    void updateRecents(const QString &path);
    void importPaths(const QStringList &files);
    bool writeProject();
    PhotoModel m_photos;
    QString m_projectPath;
    QString m_projectName;
    QString m_createdAt;
    QStringList m_recentProjects;
    QString m_status = QStringLiteral("Готов к работе");
    int m_selectedIndex = -1;
    bool m_busy = false;
};
