#pragma once

#include "Crs.h"
#include "RasterSource.h"
#include "TerrainAnalysis.h"
#include "VectorSource.h"
#include <QAbstractListModel>
#include <QColor>
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QSizeF>
#include <QTimer>
#include <QUndoStack>
#include <QUrl>
#include <QVariantMap>
#include <atomic>
#include <memory>

class PhotoModel;
class TileCache;
class QPainter;

// One layer of the map. Rasters and vectors come from files; "photos" (GPS positions of the
// project photos) and "drawings" (features drawn by the user) belong to the project.
struct MapLayer
{
    enum Type { Raster, Vector, Photos, Drawings };
    QString id;
    Type type = Raster;
    QString name;
    QString source;          // absolute path (raster/vector)
    QString vectorLayer;     // OGR layer name
    bool visible = true;
    double opacity = 1.0;
    QColor color = QColor(255, 196, 61);
    Crs assignedCrs;         // chosen by the user when the file has no CRS
    bool needsCrs = false;
    bool loading = false;
    QString status;          // problem or background work, shown under the layer name
    RasterInfo rasterInfo;
    RasterStyle style;
    std::shared_ptr<RasterSource> raster;
    int version = 0;         // changes with style/pyramids: part of the tile keys
    VectorData vector;       // features in the map CRS
    std::shared_ptr<std::atomic_bool> cancel;   // background work of this layer
};

class MapService;

// Layers for QML, top of the list = drawn last (on top).
class LayerListModel final : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Roles {
        IdRole = Qt::UserRole + 1, NameRole, TypeRole, VisibleRole, OpacityRole, StatusRole, CrsRole,
        SummaryRole, NeedsCrsRole, LoadingRole, ColorRole, LegendRole, StyleRole, BandsRole
    };
    explicit LayerListModel(MapService *service);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    void reset();
    void layerChanged(int layerIndex);
private:
    MapService *m_service;
};

// The map of the open project: layers, map CRS, view, tools, measurements, drawings (with
// undo/redo), persistence in <project>/map.json and vectors/drawings.geojson, export.
// Coordinates of the view are in the map CRS; screen y grows downwards, map y upwards.
class MapService final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool hasProject READ hasProject NOTIFY projectChanged)
    Q_PROPERTY(bool crsDefined READ crsDefined NOTIFY crsChanged)
    Q_PROPERTY(QString crsLabel READ crsLabel NOTIFY crsChanged)
    Q_PROPERTY(QString crsNote READ crsNote NOTIFY crsChanged)
    Q_PROPERTY(LayerListModel *layerModel READ layerModel CONSTANT)
    Q_PROPERTY(int layerCount READ layerCount NOTIFY layersChanged)
    Q_PROPERTY(QString selectedLayer READ selectedLayer WRITE selectLayer NOTIFY selectionChanged)
    Q_PROPERTY(QString tool READ tool WRITE setTool NOTIFY toolChanged)
    Q_PROPERTY(QString cursorText READ cursorText NOTIFY cursorChanged)
    Q_PROPERTY(QString cursorWgs84 READ cursorWgs84 NOTIFY cursorChanged)
    Q_PROPERTY(QString cursorValue READ cursorValue NOTIFY cursorChanged)
    Q_PROPERTY(QString measurementText READ measurementText NOTIFY measurementChanged)
    Q_PROPERTY(QString measurementDetails READ measurementDetails NOTIFY measurementChanged)
    Q_PROPERTY(double scaleBarMeters READ scaleBarMeters NOTIFY viewChanged)
    Q_PROPERTY(double scaleBarPixels READ scaleBarPixels NOTIFY viewChanged)
    Q_PROPERTY(QString scaleText READ scaleText NOTIFY viewChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY undoChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY undoChanged)
    Q_PROPERTY(QString undoText READ undoText NOTIFY undoChanged)
    Q_PROPERTY(QString redoText READ redoText NOTIFY undoChanged)
    Q_PROPERTY(int drawingCount READ drawingCount NOTIFY drawingsChanged)
    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY drawingsChanged)
    Q_PROPERTY(bool exporting READ exporting NOTIFY exportChanged)
    Q_PROPERTY(QString gisVersion READ gisVersion CONSTANT)
    Q_PROPERTY(QStringList problems READ problems CONSTANT)
    // Terrain analysis: background raster/contour jobs, the profile and volume tools.
    Q_PROPERTY(bool analysisBusy READ analysisBusy NOTIFY analysisChanged)
    Q_PROPERTY(QString analysisStatus READ analysisStatus NOTIFY analysisChanged)
    Q_PROPERTY(QVariantMap profile READ profileForQml NOTIFY profileChanged)
    Q_PROPERTY(int profileMarker READ profileMarker WRITE setProfileMarker NOTIFY profileMarkerChanged)
    Q_PROPERTY(QString volumeBase READ volumeBase WRITE setVolumeBase NOTIFY volumeSettingsChanged)   // level | boundaryMean | boundaryMin | bestFit | surface
    Q_PROPERTY(double volumeLevel READ volumeLevel WRITE setVolumeLevel NOTIFY volumeSettingsChanged)
    Q_PROPERTY(QString volumeSurface READ volumeSurface WRITE setVolumeSurface NOTIFY volumeSettingsChanged)   // layer id

public:
    explicit MapService(QObject *parent = nullptr);
    ~MapService() override;

    void setProject(const QString &directory, const PhotoModel *photos);
    QString projectDirectory() const { return m_project; }
    // Photos were added or removed: refresh their layer.
    void photosChanged();
    void setSelectedPhoto(int index);

    bool hasProject() const { return !m_project.isEmpty(); }
    bool crsDefined() const { return m_crs.isValid(); }
    QString crsLabel() const { return m_crs.label(); }
    QString crsNote() const { return m_crsNote; }
    Crs crs() const { return m_crs; }
    LayerListModel *layerModel() { return m_model; }
    int layerCount() const { return int(m_layers.size()); }
    const QVector<MapLayer> &layers() const { return m_layers; }
    QString selectedLayer() const { return m_selectedLayer; }
    void selectLayer(const QString &id);
    QString tool() const { return m_tool; }
    void setTool(const QString &tool);
    QString cursorText() const { return m_cursorText; }
    QString cursorWgs84() const { return m_cursorWgs84; }
    QString cursorValue() const { return m_cursorValue; }
    QString measurementText() const { return m_measurementText; }
    QString measurementDetails() const { return m_measurementDetails; }
    double scaleBarMeters() const { return m_scaleBarMeters; }
    double scaleBarPixels() const { return m_scaleBarPixels; }
    QString scaleText() const { return m_scaleText; }
    bool canUndo() const { return m_undo.canUndo(); }
    bool canRedo() const { return m_undo.canRedo(); }
    QString undoText() const { return m_undo.undoText(); }
    QString redoText() const { return m_undo.redoText(); }
    int drawingCount() const;
    bool hasSelection() const { return m_selectedFeature >= 0; }
    bool exporting() const { return m_exporting; }
    QString gisVersion() const;
    QStringList problems() const;

    // View (map units per screen pixel; centre in map CRS).
    QPointF center() const { return m_center; }
    double resolution() const { return m_resolution; }
    QSizeF viewSize() const { return m_viewSize; }
    void setViewSize(const QSizeF &size);
    void setMetersPerScreenPixel(double meters);   // physical size of a screen pixel, for 1:N
    QPointF toScreen(const QPointF &map) const;
    QPointF toMap(const QPointF &screen) const;
    void panBy(const QPointF &screenDelta);
    void zoomAt(const QPointF &screen, double factor);
    void setView(const QPointF &center, double resolution);
    // Draws every visible layer, drawings and the current sketch for the given view.
    void paint(QPainter *painter, const QSizeF &size, bool interactive);

    // Pointer input from the map item (screen coordinates).
    void pointerMoved(const QPointF &screen);
    void pointerPressed(const QPointF &screen, Qt::MouseButton button, Qt::KeyboardModifiers modifiers);
    void pointerDragged(const QPointF &screen);
    void pointerReleased(const QPointF &screen);
    void pointerDoubleClicked(const QPointF &screen);
    bool keyPressed(int key);

    Q_INVOKABLE QString addLayerFromUrl(const QUrl &url);
    Q_INVOKABLE QString addLayerFile(const QString &path) { return addLayerFromPath(path); }
    QString addLayerFromPath(const QString &path, bool zoom = true);
    Q_INVOKABLE void removeLayer(const QString &id);
    Q_INVOKABLE void moveLayer(const QString &id, int delta);   // +1 = up (drawn later)
    Q_INVOKABLE void setLayerVisible(const QString &id, bool visible);
    Q_INVOKABLE void setLayerOpacity(const QString &id, double opacity);
    Q_INVOKABLE void setLayerColor(const QString &id, const QColor &color);
    Q_INVOKABLE void setLayerStyle(const QString &id, const QVariantMap &style);
    Q_INVOKABLE QVariantMap layerDetails(const QString &id) const;
    // Assigns a CRS to a layer whose file has none. Returns an error text or "".
    Q_INVOKABLE QString assignLayerCrs(const QString &id, const QString &crsText);
    Q_INVOKABLE QString setMapCrs(const QString &crsText);
    Q_INVOKABLE QString describeCrs(const QString &crsText) const;   // label or error, for dialogs
    Q_INVOKABLE void zoomToLayer(const QString &id);
    Q_INVOKABLE void zoomToAll();
    Q_INVOKABLE void undo() { m_undo.undo(); }
    Q_INVOKABLE void redo() { m_undo.redo(); }
    Q_INVOKABLE void deleteSelected();
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE QString layerIdForSource(const QString &path) const;   // "" when not on the map
    Q_INVOKABLE void clearMeasurement();
    Q_INVOKABLE void finishSketch();
    // PNG or PDF by extension: current view with legend, scale bar, CRS and sources.
    Q_INVOKABLE void exportMap(const QUrl &url);
    Q_INVOKABLE QString exportDrawings(const QUrl &url);   // .geojson, .kml, .gpkg
    Q_INVOKABLE QString displayPath(const QUrl &url) const;

    // Elevation models on the map (single-band rasters placed in the map CRS): {id, name, min, max}.
    Q_INVOKABLE QVariantList elevationLayers() const;
    // The DEM the profile and volume tools read: the selected one, else the top visible one.
    Q_INVOKABLE QString terrainLayerId() const;
    Q_INVOKABLE QVariantMap contourSuggestion(const QString &layerId) const;   // {interval, base}
    // Builds slope | slopePercent | aspect | contours of a DEM layer in the background into
    // <project>/analysis and adds the result to the map. "" when started, else the reason.
    Q_INVOKABLE QString analyzeTerrain(const QString &layerId, const QString &kind, double interval = 0, double base = 0);
    Q_INVOKABLE void clearProfile();
    Q_INVOKABLE QString exportProfile(const QUrl &url);   // CSV: distance, elevation, x, y (map CRS)
    bool analysisBusy() const { return m_analysisBusy; }
    QString analysisStatus() const { return m_analysisStatus; }
    QVariantMap profileForQml() const;
    const terrain::Profile &profileResult() const { return m_profile; }
    int profileMarker() const { return m_profileMarker; }
    void setProfileMarker(int index);
    QString volumeBase() const { return m_volumeBase; }
    void setVolumeBase(const QString &base);
    double volumeLevel() const { return m_volumeLevel; }
    void setVolumeLevel(double level);
    QString volumeSurface() const { return m_volumeSurface; }
    void setVolumeSurface(const QString &layerId);
    const terrain::Volume &volumeResult() const { return m_volume; }

    // For tests and exports: measurement of a line/ring given in map coordinates.
    struct Measure { double length = 0; double area = 0; double perimeter = 0; double planarLength = 0; double planarArea = 0; };
    Measure measure(const QVector<QPointF> &mapPoints, bool closed) const;
    static QString formatLength(double meters);
    static QString formatArea(double squareMeters);
    QImage renderMap(const QSize &size, bool decorations) const;   // blocking

signals:
    void projectChanged();
    void crsChanged();
    void layersChanged();
    void selectionChanged();
    void toolChanged();
    void cursorChanged();
    void measurementChanged();
    void viewChanged();
    void undoChanged();
    void drawingsChanged();
    void exportChanged();
    void exportFinished(bool ok, const QString &message);
    void analysisChanged();
    void analysisFinished(bool ok, const QString &message);
    void profileChanged();
    void profileMarkerChanged();
    void volumeSettingsChanged();
    void terrainResultReady();   // a profile or volume finished (for tests and the UI)
    void repaint();
    void errorOccurred(const QString &message);

private:
    friend class LayerListModel;
    friend class DrawingCommand;
    int indexOf(const QString &id) const;
    MapLayer *layer(const QString &id);
    QString cacheDirectory() const;
    void loadState();
    void scheduleSave();
    void saveState() const;
    void saveDrawings() const;
    void loadDrawings();
    void chooseCrsFor(const MapLayer &layer);
    void setCrs(const Crs &crs, const QString &note);
    void reprojectAll();
    void prepareRaster(int index);
    void prepareVector(int index);
    void rebuildPhotos();
    void changed(int index);
    void updateScale();
    void updateMeasurement();
    void setSketch(const QVector<QPointF> &points);
    int hitFeature(const QPointF &screen) const;
    int hitVertex(int feature, const QPointF &screen) const;
    int drawingsIndex() const;
    VectorData *drawings();
    void applyDrawingChange();
    QString relativeSource(const QString &path) const;
    QString resolveSource(const QString &path) const;
    void computeProfile(const QVector<QPointF> &line);
    void computeVolume(const QVector<QPointF> &ring);
    bool isElevation(const MapLayer &l) const;

    QString m_project;
    const PhotoModel *m_photos = nullptr;
    int m_selectedPhoto = -1;
    Crs m_crs;
    QString m_crsNote;
    QVector<MapLayer> m_layers;   // drawing order: first = bottom
    LayerListModel *m_model;
    TileCache *m_tiles;
    QString m_selectedLayer;
    QString m_tool = QStringLiteral("pan");
    QPointF m_center;
    double m_resolution = 1.0;
    QSizeF m_viewSize = QSizeF(800, 600);
    double m_metersPerScreenPixel = 0.0254 / 96.0;
    QString m_cursorText, m_cursorWgs84, m_cursorValue;
    QString m_measurementText, m_measurementDetails;
    double m_scaleBarMeters = 0, m_scaleBarPixels = 0;
    QString m_scaleText;
    // Sketch of the active measuring/drawing tool, map coordinates.
    QVector<QPointF> m_sketch;
    QPointF m_hover;
    bool m_hasHover = false;
    bool m_sketchDone = false;
    // Pointer state.
    QPointF m_pressScreen;
    QPointF m_lastScreen;
    bool m_panning = false;
    int m_dragFeature = -1, m_dragVertex = -1;
    QPointF m_dragFrom;
    int m_selectedFeature = -1;
    bool m_ignoreRelease = false;
    qint64 m_nextFeatureId = 1;
    QUndoStack m_undo;
    QTimer m_saveTimer;
    bool m_exporting = false;
    bool m_loadingState = false;
    qint64 m_lastValueQuery = 0;
    // Terrain analysis.
    bool m_analysisBusy = false;
    QString m_analysisStatus;
    RasterStyle m_nextStyle;          // style of the next raster added by analyzeTerrain()
    bool m_useNextStyle = false;
    terrain::Profile m_profile;
    QString m_profileLayer;
    int m_profileMarker = -1;
    terrain::Volume m_volume;
    QString m_volumeBase = QStringLiteral("boundaryMean");
    double m_volumeLevel = 0;
    QString m_volumeSurface;
    QString m_terrainText, m_terrainDetails;   // result of the profile/volume tool
    int m_terrainGeneration = 0;
};
