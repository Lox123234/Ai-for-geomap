#pragma once

#include "Georeference.h"
#include "MapService.h"

#include <QImage>
#include <QObject>
#include <QPointer>
#include <QUrl>
#include <QVariantList>
#include <atomic>
#include <memory>
#include <optional>

class ProjectManager;
class ProcessingService;

// Georeferencing of an image by control point links: the image
// (a photo of the project, a scan of a map, an orthophoto that lies off) is shown on the map where
// it is first placed — by the footprint of the photo (alignment, else GPS and EXIF), by its own
// georeferencing, else in the middle of the view; a link is a click on the image (in the image
// pane, or on the image shown on the map) and then a click on the map where that point really is.
// With every link the transformation is fitted again and the image moves (auto adjust); the table
// tells each link's residual on the map and its inverse residual in pixels, and the RMS error;
// a link can be switched off or typed in. "Трансформировать" writes the image warped into the map
// CRS as a GeoTIFF of the project (analysis/<name>_привязка.tif) and puts it on the map; the
// user's file is never changed. The links of every image are kept in <project>/georef/.
class GeorefService final : public QObject, public MapOverlay
{
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY imageChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY imageChanged)
    Q_PROPERTY(QString imageName READ imageName NOTIFY imageChanged)
    Q_PROPERTY(QString imagePath READ imagePath NOTIFY imageChanged)
    Q_PROPERTY(int imageWidth READ imageWidth NOTIFY imageChanged)
    Q_PROPERTY(int imageHeight READ imageHeight NOTIFY imageChanged)
    Q_PROPERTY(QString placement READ placement NOTIFY imageChanged)      // how the image was placed at first
    Q_PROPERTY(QString units READ units NOTIFY imageChanged)              // of the map: "м", "°"
    Q_PROPERTY(int method READ method WRITE setMethod NOTIFY linksChanged)
    Q_PROPERTY(QVariantList methods READ methods CONSTANT)                // {value, title, minimum}
    Q_PROPERTY(QVariantList links READ links NOTIFY linksChanged)
    Q_PROPERTY(int linkCount READ linkCount NOTIFY linksChanged)
    Q_PROPERTY(int enabledCount READ enabledCount NOTIFY linksChanged)
    Q_PROPERTY(bool fitted READ fitted NOTIFY linksChanged)               // enough links for the method
    Q_PROPERTY(QString fitText READ fitText NOTIFY linksChanged)          // RMS error, or what is missing
    Q_PROPERTY(double rmse READ rmse NOTIFY linksChanged)
    Q_PROPERTY(double inverseRmse READ inverseRmse NOTIFY linksChanged)
    Q_PROPERTY(bool autoAdjust READ autoAdjust WRITE setAutoAdjust NOTIFY optionsChanged)
    Q_PROPERTY(bool showImage READ showImage WRITE setShowImage NOTIFY optionsChanged)
    Q_PROPERTY(double opacity READ opacity WRITE setOpacity NOTIFY optionsChanged)
    Q_PROPERTY(bool pending READ pending NOTIFY pendingChanged)           // a point "from" waits for its "to"
    Q_PROPERTY(int selectedLink READ selectedLink WRITE setSelectedLink NOTIFY selectionChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)                    // transforming
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(double progress READ progress NOTIFY busyChanged)
public:
    GeorefService(MapService *map, ProjectManager *projects, ProcessingService *processing, QObject *parent = nullptr);
    ~GeorefService() override;

    bool active() const { return !m_path.isEmpty(); }
    bool loading() const { return m_loading; }
    QString imageName() const;
    QString imagePath() const { return m_path; }
    int imageWidth() const { return m_full.width(); }
    int imageHeight() const { return m_full.height(); }
    QString placement() const { return m_placement; }
    QString units() const;
    int method() const { return int(m_method); }
    void setMethod(int method);
    QVariantList methods() const;
    QVariantList links() const;
    int linkCount() const { return int(m_links.size()); }
    int enabledCount() const;
    bool fitted() const { return m_fit.ok() && m_fit.used >= georef::minimumLinks(m_method); }
    QString fitText() const;
    double rmse() const { return fitted() ? m_fit.rmse : 0; }
    double inverseRmse() const { return fitted() ? m_fit.inverseRmse : 0; }
    bool autoAdjust() const { return m_autoAdjust; }
    void setAutoAdjust(bool on);
    bool showImage() const { return m_showImage; }
    void setShowImage(bool on);
    double opacity() const { return m_opacity; }
    void setOpacity(double opacity);
    bool pending() const { return m_pending.has_value(); }
    int selectedLink() const { return m_selected; }
    void setSelectedLink(int index);
    bool busy() const { return m_busy; }
    QString status() const { return m_status; }
    double progress() const { return m_progress; }

    // The image (reduced to at most 6144 px; `imageScale` = its pixels per pixel of the file) and a
    // small copy for a distant view; the links; the transformation the map shows.
    const QImage &image() const { return m_image; }
    const QImage &smallImage() const { return m_small; }
    double imageScale() const { return m_scale; }
    const QVector<georef::Link> &linkList() const { return m_links; }
    const georef::Fit &fitResult() const { return m_fit; }
    std::optional<QPointF> pendingPoint() const { return m_pending; }
    const georef::Transform &displayTransform() const { return m_display; }

    // Opens an image file (any raster GDAL reads) or a photo of the project (its row) and starts
    // the georeferencing: the map shows the image, the tool of the map takes the clicks. "" or why not.
    Q_INVOKABLE QString openFile(const QUrl &url);
    Q_INVOKABLE QString openPhoto(int row);
    QString openPath(const QString &path, int photoRow = -1);
    Q_INVOKABLE void close();

    // A point "from" on the image (pixels of the file): the next click on the map is where it goes.
    Q_INVOKABLE void addImagePoint(double x, double y);
    Q_INVOKABLE void moveImagePoint(int link, double x, double y);
    Q_INVOKABLE void cancelPending();
    Q_INVOKABLE void setLinkEnabled(int index, bool on);
    Q_INVOKABLE void removeLink(int index);
    // field: "ix", "iy" (pixels), "mx", "my" (map); text in the user's locale. "" or the reason.
    Q_INVOKABLE QString setLinkValue(int index, const QString &field, const QString &text);
    Q_INVOKABLE void clearLinks();
    Q_INVOKABLE void applyNow();            // the map follows the links (with auto adjust off)
    Q_INVOKABLE void zoomToImage();
    Q_INVOKABLE void zoomToLink(int index);
    // Link file: tab-separated "x y X Y" per link, x, y in pixels with y up (−row).
    Q_INVOKABLE QString saveLinks(const QUrl &url);
    Q_INVOKABLE QString loadLinks(const QUrl &url);
    // Warps the image by the fitted transformation into <project>/analysis/<name>_привязка.tif
    // (resolution 0: the image's own) in the background and adds it to the map. "" when started.
    Q_INVOKABLE QString rectify(double resolution = 0);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE QString formatNumber(double value, int decimals) const;

    void mapClicked(const QPointF &mapPoint);
    void paintOverlay(QPainter *painter, const MapService &map) const override;
    struct Loaded;                           // an image read in the background

signals:
    void imageChanged();
    void linksChanged();
    void optionsChanged();
    void pendingChanged();
    void selectionChanged();
    void busyChanged();
    void statusChanged();
    void repaint();                          // the image pane
    void finished(bool ok, const QString &message, const QString &path);

private:
    void loaded(const std::shared_ptr<Loaded> &l, int generation);
    void setStatus(const QString &text);
    void refit();
    void saveSession() const;
    void restoreSession();
    QString sessionPath() const;
    bool inImage(const QPointF &p) const;
    std::optional<QPointF> imagePointAt(const QPointF &mapPoint) const;   // through the display transform
    void mapCrsChanged();

    QPointer<MapService> m_map;
    ProjectManager *m_projects = nullptr;
    ProcessingService *m_processing = nullptr;
    QString m_path, m_name, m_placement;
    int m_photoRow = -1;
    bool m_loading = false;
    int m_generation = 0;
    QSize m_full;
    QImage m_image, m_small;
    double m_scale = 1;
    Crs m_crs;                               // of the links' map points (the map's)
    georef::Method m_method = georef::Method::Affine;
    QVector<georef::Link> m_links;
    georef::Fit m_fit;
    georef::Transform m_initial, m_display;
    std::optional<QPointF> m_pending;
    int m_selected = -1;
    bool m_autoAdjust = true, m_showImage = true;
    double m_opacity = 0.75;
    bool m_busy = false;
    double m_progress = 0;
    QString m_status;
    std::shared_ptr<std::atomic_bool> m_cancel;
};
