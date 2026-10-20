#pragma once

#include "Crs.h"
#include "RasterSource.h"
#include "TerrainAnalysis.h"
#include "VectorSource.h"
#include <QAbstractListModel>
#include <QColor>
#include <QDateTime>
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
namespace ai { class AiService; }

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
    bool basemap = false;    // background raster (MBTiles, GeoPackage, GeoTIFF): always the bottom layer
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

// Something drawn over the layers of the interactive map, under the markers (the georeferencing
// preview of an image and its links). Not part of exports.
class MapOverlay
{
public:
    virtual ~MapOverlay() = default;
    virtual void paintOverlay(QPainter *painter, const MapService &map) const = 0;
};

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
    // Saved measurements of the project (measurements.json): name, kind, values, method, CRS, geometry.
    Q_PROPERTY(QVariantList measurements READ measurementsForQml NOTIFY measurementsChanged)
    Q_PROPERTY(bool canSaveMeasurement READ canSaveMeasurement NOTIFY measurementChanged)
    // Background map: online (XYZ tiles of a provider whose terms the user accepted, downloaded as
    // the view needs them) or from a file (MBTiles, GeoPackage, GeoTIFF).
    Q_PROPERTY(bool hasBasemap READ hasBasemap NOTIFY layersChanged)
    Q_PROPERTY(bool basemapVisible READ basemapVisible NOTIFY layersChanged)
    Q_PROPERTY(QString basemapAttribution READ basemapAttribution NOTIFY layersChanged)
    // Provider of the online basemap while it is shown ("" otherwise): the 3D view lies on the same one.
    Q_PROPERTY(QString onlineBasemap READ onlineBasemap NOTIFY layersChanged)
    Q_PROPERTY(QString shownMeasurement READ shownMeasurement NOTIFY measurementsChanged)
    // Selection by click with a local AI model (tool "aiSelect"): the contour of the object under
    // the click on the top RGB raster, shown as a candidate until it is added to the drawings.
    Q_PROPERTY(bool aiBusy READ aiBusy NOTIFY aiChanged)
    Q_PROPERTY(QString aiStatus READ aiStatus NOTIFY aiChanged)
    Q_PROPERTY(bool hasAiCandidate READ hasAiCandidate NOTIFY aiChanged)
    // Cross sections (tool "crossProfile"): one every crossSpacing metres along the drawn line,
    // crossHalfWidth metres to each side; the profile panel shows section crossIndex.
    Q_PROPERTY(double crossSpacing READ crossSpacing WRITE setCrossSpacing NOTIFY crossChanged)
    Q_PROPERTY(double crossHalfWidth READ crossHalfWidth WRITE setCrossHalfWidth NOTIFY crossChanged)
    Q_PROPERTY(int crossIndex READ crossIndex WRITE setCrossIndex NOTIFY crossChanged)
    Q_PROPERTY(int crossCount READ crossCount NOTIFY crossChanged)
    // Viewshed (tool "viewshed"): observer and target heights above the surface, radius, metres.
    Q_PROPERTY(double viewObserverHeight READ viewObserverHeight WRITE setViewObserverHeight NOTIFY viewshedSettingsChanged)
    Q_PROPERTY(double viewTargetHeight READ viewTargetHeight WRITE setViewTargetHeight NOTIFY viewshedSettingsChanged)
    Q_PROPERTY(double viewRadius READ viewRadius WRITE setViewRadius NOTIFY viewshedSettingsChanged)
    // Coordinate grid over the map (persisted in map.json); markers of the project (markers.json).
    Q_PROPERTY(bool showGrid READ showGrid WRITE setShowGrid NOTIFY viewOptionsChanged)
    Q_PROPERTY(bool showMarkers READ showMarkers WRITE setShowMarkers NOTIFY viewOptionsChanged)
    Q_PROPERTY(QVariantList markers READ markersForQml NOTIFY markersChanged)
    Q_PROPERTY(int selectionCount READ selectionCount NOTIFY drawingsChanged)
    // Last image overlap map: {method, job, photos, area, median, share2, share3, share5, share9, note, path}.
    Q_PROPERTY(QVariantMap overlapInfo READ overlapInfo NOTIFY overlapChanged)

public:
    QVariantMap overlapInfo() const { return m_overlap; }
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
    bool hasSelection() const { return m_selectedFeature >= 0 || !m_multiSelection.isEmpty(); }
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
    void setOverlay(const MapOverlay *overlay);   // nullptr: none
    void requestRepaint() { emit repaint(); }

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
    // Layers of files inside `directory` are removed (their files closed), e.g. before that
    // processing job is deleted: open rasters would keep Windows from deleting them.
    void releaseDirectory(const QString &directory);
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
    // Builds slope | slopePercent | aspect | contours | difference (layer − otherLayer) of a DEM
    // layer in the background into <project>/analysis and adds the result to the map.
    // "" when started, else the reason.
    Q_INVOKABLE QString analyzeTerrain(const QString &layerId, const QString &kind, double interval = 0, double base = 0,
                                       const QString &otherLayerId = {});
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

    // Keeps the finished measurement of the active tool (length, area, profile, volume) in the
    // project. Returns the new id, or "" when there is nothing finished to save.
    Q_INVOKABLE QString saveMeasurement(const QString &name = {});
    Q_INVOKABLE void removeMeasurement(const QString &id);
    Q_INVOKABLE void renameMeasurement(const QString &id, const QString &name);
    Q_INVOKABLE void showMeasurement(const QString &id);   // highlight and zoom to it; "" hides
    Q_INVOKABLE QString exportMeasurements(const QUrl &url);   // CSV with WKT geometry (WGS 84)
    // Adds a raster file (MBTiles, GeoPackage tiles, GeoTIFF/COG, VRT) as the background of the
    // map, below every layer. "" on success, else the reason.
    Q_INVOKABLE QString addBasemap(const QUrl &url);
    QString addBasemapFromPath(const QString &path);
    Q_INVOKABLE void setBasemapVisible(bool visible);
    Q_INVOKABLE QVariantList basemapProviders() const;   // {id, title, kind, attribution, terms, accepted, current}
    Q_INVOKABLE void acceptBasemapTerms(const QString &providerId);
    Q_INVOKABLE QString setOnlineBasemap(const QString &providerId);   // "" or the reason
    QString basemapAttribution() const;
    QString onlineBasemap() const;
    void webTilesArrived();
    bool hasBasemap() const;
    bool basemapVisible() const;
    QVariantList measurementsForQml() const;
    bool canSaveMeasurement() const;
    QString shownMeasurement() const { return m_shownMeasurement; }
    struct SavedMeasurement
    {
        QString id, name, kind, text, details, layer, method;
        QVector<QPointF> geometry;   // longitude, latitude (WGS 84)
        QVariantMap values;          // numbers in metres, square and cubic metres, degrees
        QString crs;                 // map CRS when it was measured
        QDateTime created;
    };
    const QVector<SavedMeasurement> &savedMeasurements() const { return m_measurements; }

    // AI selection. The image the model sees is the view ∩ the raster, longest side 1024 px;
    // it is encoded once and reused while the view and the layer stay the same.
    void setAi(ai::AiService *service) { m_ai = service; }
    // A click at a map point: a new object, or (add/exclude) a refinement of the candidate.
    // "" when started, else the reason.
    Q_INVOKABLE QString aiSelectAt(const QPointF &mapPoint, bool add = false, bool exclude = false);
    Q_INVOKABLE QString acceptAiCandidate();   // into the drawings (undoable); the feature name, or ""
    Q_INVOKABLE void clearAiCandidate();
    // Removes the object outlined by the AI candidate (or the selected drawn polygon), grown by
    // `growMeters` for its edge and shadow, from the colour raster under it: an inpainting model
    // (LaMa) fills it from the surroundings in the program's editable copy of the raster
    // (<project>/analysis/<name>_ИИ-правка.tif), which replaces the original on the map. The
    // user's file is never written. Undoable; every edit is listed in <copy>.edits.json.
    // "" when started, else the reason.
    Q_INVOKABLE QString removeFromOrthophoto(double growMeters = 0.3);
    // AI retouch: the streaks of a true orthophoto (its mask <name>_stretch.tif, written when it is
    // built) repainted by the inpainting model in a copy in <project>/analysis, which replaces it
    // on the map; the original stays as it is (hidden). Recorded in <copy>.edits.json. The layer:
    // the selected one if it has a mask, else the top visible one with a mask. "" when started.
    Q_INVOKABLE QString retouchOrthophoto();
    // A raster file shown on the map is about to be replaced (a rebuilt result): its layers close it
    // (on Windows an open file cannot be replaced); after, they open it again keeping their style.
    void releaseRasterFile(const QString &path);
    void reopenRasterFile(const QString &path);
    Q_INVOKABLE QString retouchLayerId() const;   // the layer retouchOrthophoto() would work on; "" none
    void rasterEdited(const QString &path);   // a raster file changed in place: reload its layers

    // Raster calculator and vegetation indices of a raster layer, into <project>/analysis:
    // index exg | vari | gli (RGB bands), ndvi (red and near-infrared bands) or calc (`expression`
    // over B1, B2, …). "" when started, else the reason.
    Q_INVOKABLE QString calculateRaster(const QString &layerId, const QString &index, const QString &expression = {},
                                        int red = 1, int green = 2, int blue = 3, int nir = 4);
    Q_INVOKABLE QString checkRasterExpression(const QString &layerId, const QString &expression) const;
    Q_INVOKABLE QVariantList rasterLayers() const;   // {id, name, bands, rgb} of the raster layers (not elevation-only)
    // Image overlap map into <project>/analysis: from the newest job with a reconstruction, else
    // estimated from the photos' EXIF/XMP before alignment. "" when started, else the reason;
    // the result arrives as analysisFinished with the layer on the map and the statistics.
    Q_INVOKABLE QString computeOverlap();
    // Everything the analysis produced (rasters, contours, measurements, drawings, the profile and
    // the cross sections) copied into a new folder inside `folder`. "" or the reason; on success
    // analysisFinished(true, …) names the folder.
    Q_INVOKABLE QString exportAnalysis(const QUrl &folder);
    Q_INVOKABLE QString exportCrossSections(const QUrl &url);   // CSV: section, station, offset, elevation, x, y
    double crossSpacing() const { return m_crossSpacing; }
    void setCrossSpacing(double metres);
    double crossHalfWidth() const { return m_crossHalfWidth; }
    void setCrossHalfWidth(double metres);
    int crossIndex() const { return m_crossIndex; }
    void setCrossIndex(int index);
    int crossCount() const { return int(m_cross.size()); }
    double viewObserverHeight() const { return m_viewObserver; }
    void setViewObserverHeight(double metres);
    double viewTargetHeight() const { return m_viewTarget; }
    void setViewTargetHeight(double metres);
    double viewRadius() const { return m_viewRadius; }
    void setViewRadius(double metres);

    // Several drawings selected at once (MapServiceEdit.cpp): Ctrl+click in the edit tool adds or
    // removes one; delete, copy and the measurement work on all of them.
    int selectionCount() const;
    Q_INVOKABLE void selectAllDrawings();
    Q_INVOKABLE void invertDrawingSelection();
    // Selected drawings as a GeoJSON FeatureCollection (WGS 84, RFC 7946) in the system clipboard;
    // paste adds the features of GeoJSON in the clipboard (any CRS GDAL reads) to the drawings.
    // "" or the reason.
    Q_INVOKABLE QString copySelection();
    Q_INVOKABLE QString pasteDrawings();
    QString selectionGeoJson() const;
    QString pasteGeoJson(const QByteArray &geojson);   // one undo step; "" or the reason
    bool showGrid() const { return m_showGrid; }
    void setShowGrid(bool on);
    bool showMarkers() const { return m_showMarkers; }
    void setShowMarkers(bool on);

    // Markers: named points of the project with their WGS 84 position and the height of the
    // elevation model under them when they were placed (tool "marker").
    struct Marker
    {
        QString id, name, note;
        double lon = 0, lat = 0, z = std::numeric_limits<double>::quiet_NaN();
        QString heightSource;   // elevation layer the height came from
        QDateTime created;
    };
    const QVector<Marker> &projectMarkers() const { return m_markers; }
    QVariantList markersForQml() const;
    Q_INVOKABLE QString addMarker(const QPointF &mapPoint, const QString &name = {});   // new id, or ""
    Q_INVOKABLE void removeMarker(const QString &id);
    Q_INVOKABLE void renameMarker(const QString &id, const QString &name);
    Q_INVOKABLE void setMarkerNote(const QString &id, const QString &note);
    Q_INVOKABLE void zoomToMarker(const QString &id);
    Q_INVOKABLE QString exportMarkers(const QUrl &url);   // CSV: name, lon, lat, x, y (map CRS), z, note

    // DEM editor (MapServiceTerrainEdit.cpp), inside the AI candidate or the selected drawn polygon
    // grown by `growMeters`, on the elevation model of the terrain tools: "interpolate" (remove an
    // object: heights from its surroundings), "plane" (best-fit plane through the boundary),
    // "level" (the height `level`), "smooth". In the program's copy <project>/analysis/<name>_правка.tif,
    // which replaces the model on the map; undoable. "" when started, else the reason.
    Q_INVOKABLE QString editDem(const QString &operation, double level = 0, double growMeters = 0.5);
    // A new copy of the model with the holes inside it (not its outer edge) filled from around them.
    Q_INVOKABLE QString fillDemGaps(const QString &layerId);
    // Point clouds (LAS): "deleteInside", "keepInside", "classifyInside" (class `classCode`) with the
    // outline as above, or "classify" (every point by its height above `dtmPath` and its colour).
    // The result is a new LAS in <project>/analysis; cloudEditFinished tells where. "" or the reason.
    Q_INVOKABLE QString editPointCloud(const QString &lasPath, const QString &operation, int classCode = 7, const QString &dtmPath = {});
    Q_INVOKABLE bool hasOutline() const;   // an AI candidate or a selected drawn polygon
    bool aiBusy() const { return m_aiBusy; }
    QString aiStatus() const { return m_aiStatus; }
    bool hasAiCandidate() const { return !m_aiCandidate.isEmpty(); }
    const QVector<QPolygonF> &aiCandidate() const { return m_aiCandidate; }   // map CRS: outer ring, holes
    double aiScore() const { return m_aiScore; }

    // For tests and exports: measurement of a line/ring given in map coordinates.
    struct Measure { double length = 0; double area = 0; double perimeter = 0; double planarLength = 0; double planarArea = 0; };
    Measure measure(const QVector<QPointF> &mapPoints, bool closed) const;
    double polygonArea(const VectorFeature &polygon) const;   // geodesic, outer ring minus holes
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
    void measurementsChanged();
    void aiChanged();
    void aiFinished(bool ok, const QString &message);   // an AI selection finished (for tests and the UI)
    void crossChanged();
    void viewshedSettingsChanged();
    void viewOptionsChanged();
    void markersChanged();
    void overlapChanged();
    void cloudEditFinished(bool ok, const QString &message, const QString &path);
    void repaint();
    void errorOccurred(const QString &message);
    // Tool "georef": a click on the map (a point "to" of a georeferencing link, or "from" on the
    // image shown on the map) and Esc.
    void georefClicked(const QPointF &mapPoint);
    void georefCancelled();

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
    // The streak mask of a raster: next to it (a true orthophoto) or recorded in its edits log
    // (an edited copy of one); "" none.
    QString stretchMaskOf(const MapLayer &l) const;
    // A product of the analysis (slope, viewshed, indices…) in <project>/analysis: a single-band
    // raster, but not heights. Edited copies of a model (_правка, _заполнено) are heights.
    bool isDerived(const MapLayer &l) const;
    void loadMeasurements();
    void saveMeasurements() const;

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
    bool m_viewSizeKnown = false;   // setViewSize() called by the canvas (or a test)
    bool m_fitPending = false;      // zoomToAll() asked for before the size was known
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
    QTimer m_webRepaint;
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
    QString m_terrainLayerName;                // DEM of the last profile/volume
    QVector<SavedMeasurement> m_measurements;
    QString m_shownMeasurement;
    int m_nextMeasurementId = 1;
    // AI selection.
    void runAiSelection(int layerIndex);   // m_aiPoints/m_aiLabels on the image m_aiWindow/m_aiImageSize
    void setAiText();
    ai::AiService *m_ai = nullptr;      // outlives the map (created before it)
    Qt::KeyboardModifiers m_pressModifiers;
    bool m_aiBusy = false;
    QString m_aiStatus;
    int m_aiGeneration = 0;
    QVector<QPointF> m_aiPoints;        // prompts of the candidate, map CRS
    QVector<int> m_aiLabels;            // 1 = object, 0 = background
    QVector<QPolygonF> m_aiCandidate;   // map CRS: outer ring, then holes
    double m_aiScore = 0;
    QString m_aiLayerId, m_aiLayerName, m_aiModelName;
    QRectF m_aiWindow;                  // map rectangle of the encoded image
    QSize m_aiImageSize;
    QString m_aiImageKey;               // what the shared segmenter has encoded
    QString m_aiText, m_aiDetails;
    int m_nextAiId = 1;
    // Analysis tools (MapServiceAnalysis.cpp).
    bool isTerrainTool() const;
    void computeCrossSections(const QVector<QPointF> &line);
    void computeSlope(const QVector<QPointF> &two);
    void computeHeight(const QPointF &point);
    void computeAngle(const QVector<QPointF> &three);
    void computeViewshed(const QPointF &observer);
    void paintAnalysis(QPainter *painter) const;
    QVector<terrain::Profile> m_cross;
    QVector<QVector<QPointF>> m_crossLines;   // map CRS
    int m_crossIndex = 0;
    double m_crossSpacing = 10, m_crossHalfWidth = 15;
    double m_viewObserver = 1.7, m_viewTarget = 0, m_viewRadius = 300;
    QVariantMap m_pointValues;                // numbers of the last slope / height / angle (saved with it)
    // Selection, clipboard, grid and markers (MapServiceEdit.cpp).
    void paintGrid(QPainter *painter, const QSizeF &size) const;
    void paintMarkers(QPainter *painter) const;
    void loadMarkers();
    void saveMarkers() const;
    QVector<int> selectedIndices() const;      // the primary selection first
    void addDrawing(const VectorFeature &feature, const QString &undoText);   // undoable
    QVector<QPolygonF> outlineRings() const;   // AI candidate, else the selected polygon; map CRS
    // Puts an edited copy of layer `index` on the map in its place (the original hidden).
    void showEditedCopy(int index, const QString &copy, const QString &suffix);
    QSet<qint64> m_multiSelection;             // ids of further selected drawings
    bool m_showGrid = false;
    bool m_showMarkers = true;
    const MapOverlay *m_overlay = nullptr;
    QVector<Marker> m_markers;
    int m_nextMarker = 1;
    QVariantMap m_overlap;
};
