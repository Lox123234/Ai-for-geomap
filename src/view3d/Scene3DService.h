#pragma once

#include "Camera3D.h"
#include "Scene3D.h"

#include <QObject>
#include <QSizeF>
#include <QThreadPool>
#include <QUrl>
#include <QVariantList>
#include <memory>

// The 3D scene of the project: point clouds, models and terrains (DEM meshes) loaded in the
// background, a common origin (double precision), the camera, picking and 3D measurements.
// The QML view (View3DItem) draws a snapshot of it; everything here works without a GPU.
class Scene3DService : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList layers READ layersForQml NOTIFY layersChanged)
    Q_PROPERTY(int layerCount READ layerCount NOTIFY layersChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY layersChanged)
    Q_PROPERTY(QString status READ status NOTIFY layersChanged)
    Q_PROPERTY(int pointBudget READ pointBudget WRITE setPointBudget NOTIFY settingsChanged)
    Q_PROPERTY(float pointSize READ pointSize WRITE setPointSize NOTIFY settingsChanged)
    Q_PROPERTY(QString colorMode READ colorMode WRITE setColorMode NOTIFY settingsChanged)          // "rgb" | "height"
    Q_PROPERTY(float verticalScale READ verticalScale WRITE setVerticalScale NOTIFY settingsChanged) // visual only
    Q_PROPERTY(bool lighting READ lighting WRITE setLighting NOTIFY settingsChanged)
    Q_PROPERTY(bool showAxes READ showAxes WRITE setShowAxes NOTIFY settingsChanged)
    Q_PROPERTY(bool orthographic READ orthographic WRITE setOrthographic NOTIFY viewChanged)
    Q_PROPERTY(float yaw READ yaw NOTIFY viewChanged)
    Q_PROPERTY(float pitch READ pitch NOTIFY viewChanged)
    Q_PROPERTY(QString tool READ tool WRITE setTool NOTIFY toolChanged)                             // orbit | pan | zoom | measure
    Q_PROPERTY(QString pickText READ pickText NOTIFY pickChanged)
    Q_PROPERTY(QString measureText READ measureText NOTIFY pickChanged)

public:
    struct Layer
    {
        QString id;
        QString name;
        QString kind;          // pointcloud | mesh | terrain
        QString source;
        QString drape;         // terrain texture source (orthophoto), if any
        QString loaded;        // the file actually read, when it differs from `source`
        bool visible = true;
        bool loading = true;
        QString info;
        QString error;
        int version = 0;
        std::shared_ptr<const view3d::PointCloud> cloud;
        std::shared_ptr<const view3d::Mesh> mesh;
    };
    struct Snapshot
    {
        QVector<Layer> layers;
        view3d::Origin origin;
        view3d::Bounds bounds;      // scene coordinates, vertical scale not applied
        view3d::Camera camera;
        float pointSize = 2.0f;
        bool heightColors = false;
        float verticalScale = 1.0f;
        bool lighting = true;
        int revision = 0;
    };

    explicit Scene3DService(QObject *parent = nullptr);
    ~Scene3DService() override;

    Q_INVOKABLE QString addPointCloud(const QString &path);
    Q_INVOKABLE QString addModel(const QString &path);
    Q_INVOKABLE QString addTerrain(const QString &demPath, const QString &drapePath = {});
    Q_INVOKABLE QString addFile(const QString &path);   // by extension: .ply/.las → cloud, .obj → model, .tif → terrain
    Q_INVOKABLE QString addUrl(const QUrl &url, const QString &kind);   // file dialogs: kind pointcloud | mesh | terrain | ""
    Q_INVOKABLE void removeLayer(const QString &id);
    Q_INVOKABLE void setLayerVisible(const QString &id, bool visible);
    Q_INVOKABLE void clear();
    Q_INVOKABLE QString layerIdForSource(const QString &path) const;

    // Camera (viewport in device-independent pixels).
    Q_INVOKABLE void orbit(float dx, float dy);
    Q_INVOKABLE void pan(float dx, float dy, float viewportHeight);
    Q_INVOKABLE void zoom(float steps);
    Q_INVOKABLE void fitAll();
    void setViewAspect(float aspect);   // the view's width / height (fitting the scene)
    Q_INVOKABLE void setPreset(const QString &preset);   // top bottom front back left right iso
    // Pick at a viewport point: shows XYZ, and in the measure tool collects two points.
    Q_INVOKABLE bool pickAt(float x, float y, float width, float height);
    Q_INVOKABLE void clearMeasurement();

    Snapshot snapshot() const;
    const QVector<Layer> &layers() const { return m_layers; }
    QVariantList layersForQml() const;
    int layerCount() const { return int(m_layers.size()); }
    bool loading() const;
    QString status() const;
    view3d::Origin origin() const { return m_origin; }
    view3d::Bounds bounds() const;
    const view3d::Camera &camera() const { return m_camera; }
    QVector3D lastPick() const { return m_lastPick; }
    double lastPickAbsolute(int axis) const;
    bool georeferenced() const;   // every visible layer is in a known CRS (values in metres)

    int pointBudget() const { return m_pointBudget; }
    void setPointBudget(int points);
    float pointSize() const { return m_pointSize; }
    void setPointSize(float size);
    QString colorMode() const { return m_heightColors ? QStringLiteral("height") : QStringLiteral("rgb"); }
    void setColorMode(const QString &mode);
    float verticalScale() const { return m_verticalScale; }
    void setVerticalScale(float scale);
    bool lighting() const { return m_lighting; }
    void setLighting(bool on);
    bool showAxes() const { return m_showAxes; }
    void setShowAxes(bool on);
    bool orthographic() const { return m_camera.orthographic; }
    void setOrthographic(bool on);
    float yaw() const { return m_camera.yaw; }
    float pitch() const { return m_camera.pitch; }
    QString tool() const { return m_tool; }
    void setTool(const QString &tool);
    QString pickText() const { return m_pickText; }
    QString measureText() const { return m_measureText; }

signals:
    void layersChanged();
    void settingsChanged();
    void viewChanged();
    void toolChanged();
    void pickChanged();
    void errorOccurred(const QString &message);
    void changed();   // anything the view draws

private:
    QString add(const QString &kind, const QString &path, const QString &drape);
    void loaded(const QString &id, int version, std::shared_ptr<const view3d::PointCloud> cloud,
                std::shared_ptr<const view3d::Mesh> mesh, const QString &error);
    QVector3D layerOffset(const Layer &l) const;   // layer origin relative to the scene origin
    // Visible data for fitting the camera: bounds and up to ~20 000 sample points, heights
    // multiplied by the vertical exaggeration as drawn.
    view3d::Bounds drawnBounds(QVector<QVector3D> *samples) const;
    void applyPreset(view3d::Camera::Preset preset);

    QVector<Layer> m_layers;
    view3d::Origin m_origin;
    view3d::Camera m_camera;
    bool m_cameraFitted = false;
    int m_nextId = 1;
    int m_revision = 0;
    int m_pointBudget = 5000000;
    float m_pointSize = 2.0f;
    bool m_heightColors = false;
    float m_verticalScale = 1.0f;
    bool m_lighting = true;
    bool m_showAxes = true;
    QString m_tool = QStringLiteral("orbit");
    QVector3D m_lastPick;
    bool m_hasPick = false;
    QVector<QVector3D> m_measure;
    QString m_pickText;
    QString m_measureText;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    QThreadPool m_pool;   // loaders; the destructor waits for them (they post to this object)
};
