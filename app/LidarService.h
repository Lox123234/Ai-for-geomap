#pragma once

#include "Lidar.h"

#include <QObject>
#include <QPointer>
#include <QUrl>
#include <QVariantList>
#include <atomic>
#include <memory>

class MapService;
class Scene3DService;

// LiDAR clouds of the project (Файл → Импорт LiDAR): LAS/LAZ files listed in <project>/lidar.json
// (the files stay where they are), what each holds, the cloud in the 3D view, and a DSM or DTM
// built from it into <project>/lidar/ and put on the map.
class LidarService final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList clouds READ clouds NOTIFY cloudsChanged)
    Q_PROPERTY(int count READ count NOTIFY cloudsChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString status READ status NOTIFY busyChanged)
    Q_PROPERTY(bool available READ available CONSTANT)   // PDAL of the bundled ODM found
public:
    LidarService(MapService *map, Scene3DService *scene, QObject *parent = nullptr);
    ~LidarService() override;

    void setProject(const QString &directory);
    QVariantList clouds() const;
    int count() const { return int(m_clouds.size()); }
    bool busy() const { return m_busy; }
    QString status() const { return m_status; }
    bool available() const;

    // Reads the files (in the background) and adds them to the project. "" when started.
    Q_INVOKABLE QString importUrls(const QList<QUrl> &urls);
    QString importPaths(const QStringList &paths);
    Q_INVOKABLE void remove(int index);
    Q_INVOKABLE QString showIn3D(int index);
    // kind "dsm" | "dtm"; resolution 0: suggested. "" when started.
    Q_INVOKABLE QString buildSurface(int index, const QString &kind, double resolution = 0);
    Q_INVOKABLE double suggestedResolution(int index) const;
    Q_INVOKABLE void cancel();

signals:
    void cloudsChanged();
    void busyChanged();
    void finished(bool ok, const QString &message, const QString &path);

private:
    struct Cloud
    {
        lidar::Info info;
        QString name;
        QString dsm, dtm;   // built surfaces, absolute
    };
    void load();
    void save() const;
    void setBusy(bool busy, const QString &status);
    QString relative(const QString &path) const;
    QString absolute(const QString &path) const;

    QPointer<MapService> m_map;
    QPointer<Scene3DService> m_scene;
    QString m_project;
    QVector<Cloud> m_clouds;
    bool m_busy = false;
    QString m_status;
    std::shared_ptr<std::atomic_bool> m_cancel;
    int m_generation = 0;
};
