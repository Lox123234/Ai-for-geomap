#pragma once

#include "Camera3D.h"
#include "Scene3D.h"

#include <QObject>
#include <QRectF>
#include <QSizeF>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <memory>

// The 3D scene of the project: point clouds, models and terrains (DEM meshes) loaded in the
// background, a common origin (double precision), the camera, picking and 3D measurements.
// Under georeferenced data the map's online basemap lies as a ground plane in the scene's CRS,
// and the photos of the job are shown at their reconstructed positions (cameras with
// thumbnails). The QML view (View3DItem) draws a snapshot of it; everything here works without a GPU.
class Scene3DService : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList layers READ layersForQml NOTIFY layersChanged)
    Q_PROPERTY(int layerCount READ layerCount NOTIFY layersChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY layersChanged)
    Q_PROPERTY(QString status READ status NOTIFY layersChanged)
    Q_PROPERTY(int pointBudget READ pointBudget WRITE setPointBudget NOTIFY settingsChanged)
    Q_PROPERTY(float pointSize READ pointSize WRITE setPointSize NOTIFY settingsChanged)
    Q_PROPERTY(QString colorMode READ colorMode WRITE setColorMode NOTIFY settingsChanged)          // "rgb" | "height" | "classes" | "intensity"
    Q_PROPERTY(float verticalScale READ verticalScale WRITE setVerticalScale NOTIFY settingsChanged) // visual only
    Q_PROPERTY(bool lighting READ lighting WRITE setLighting NOTIFY settingsChanged)
    Q_PROPERTY(bool showAxes READ showAxes WRITE setShowAxes NOTIFY settingsChanged)
    Q_PROPERTY(bool orthographic READ orthographic WRITE setOrthographic NOTIFY viewChanged)
    Q_PROPERTY(float yaw READ yaw NOTIFY viewChanged)
    Q_PROPERTY(float pitch READ pitch NOTIFY viewChanged)
    Q_PROPERTY(QString tool READ tool WRITE setTool NOTIFY toolChanged)                             // orbit | pan | zoom | measure
    Q_PROPERTY(QString pickText READ pickText NOTIFY pickChanged)
    Q_PROPERTY(QString measureText READ measureText NOTIFY pickChanged)
    // Online basemap under the scene (the map's: provider id, "" none) and its attribution.
    Q_PROPERTY(QString basemap READ basemap WRITE setBasemap NOTIFY basemapChanged)
    Q_PROPERTY(QString basemapAttribution READ basemapAttribution NOTIFY basemapChanged)
    Q_PROPERTY(bool showCameras READ showCameras WRITE setShowCameras NOTIFY settingsChanged)

public:
    struct Layer
    {
        QString id;
        QString name;
        QString kind;          // pointcloud | mesh | terrain; overlays: basemap | cameras
        QString source;
        QString drape;         // terrain texture source (orthophoto), if any
        QString loaded;        // the file actually read, when it differs from `source`
        QString job;           // ODM job folder the file belongs to, if any (its cameras)
        bool overlay = false;  // basemap, cameras: context, not data (not fitted, picked or measured)
        bool visible = true;
        bool loading = true;
        QString info;
        QString error;
        int version = 0;
        std::shared_ptr<const view3d::PointCloud> cloud;
        std::shared_ptr<const view3d::Mesh> mesh;
    };
    // A photo of the job at its reconstructed pose (scene coordinates).
    struct CameraMark
    {
        QString name;
        QVector3D centre;
        QVector3D corners[4];  // its rectangle in front of the camera: top-left, top-right, bottom-right, bottom-left
    };
    struct Snapshot
    {
        QVector<Layer> layers;
        view3d::Origin origin;
        view3d::Bounds bounds;      // data, scene coordinates, vertical scale not applied
        view3d::Bounds extent;      // everything drawn, overlays included (clipping planes)
        view3d::Camera camera;
        float pointSize = 2.0f;
        int colorMode = 0;          // 0 source colours, 1 height, 2 point classes, 3 intensity
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
    QString colorMode() const;
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

    QString basemap() const { return m_basemap; }
    // A built-in provider is used only after its terms were accepted (as on the map).
    void setBasemap(const QString &providerId);
    QString basemapAttribution() const;
    bool showCameras() const { return m_showCameras; }
    void setShowCameras(bool on);
    const QVector<CameraMark> &cameras() const { return m_cameras; }

signals:
    void basemapChanged();
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
    // Overlays follow the data: cameras of the job, the basemap around the data and cameras.
    void dataChanged();
    Layer *overlay(const QString &id);
    void setOverlay(Layer layer);   // adds or replaces by id
    void removeOverlays();
    void loadCameras(const QString &job);
    QString sceneCrsWkt() const;
    void scheduleBasemap(bool force);
    void buildBasemap();

    QVector<Layer> m_layers;
    view3d::Origin m_origin;
    view3d::Camera m_camera;
    bool m_cameraFitted = false;
    int m_nextId = 1;
    int m_revision = 0;
    int m_pointBudget = 5000000;
    float m_pointSize = 2.0f;
    int m_colorMode = 0;
    float m_verticalScale = 1.0f;
    bool m_lighting = true;
    bool m_showAxes = true;
    QString m_tool = QStringLiteral("orbit");
    QVector3D m_lastPick;
    bool m_hasPick = false;
    QVector<QVector3D> m_measure;
    QString m_pickText;
    QString m_measureText;
    QString m_basemap;
    QTimer m_basemapTimer;
    bool m_basemapForce = false;     // redraw even if the area is the same (new tiles)
    bool m_basemapBusy = false;
    bool m_basemapAgain = false;     // asked again while drawing
    bool m_basemapComplete = false;  // every tile of the drawn area was there
    int m_basemapGeneration = 0;
    QString m_basemapKey;            // provider and area of the basemap drawn
    bool m_showCameras = true;
    QString m_camerasJob;            // job whose cameras are loaded or loading
    QVector<CameraMark> m_cameras;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    QThreadPool m_pool;   // loaders; the destructor waits for them (they post to this object)
};
