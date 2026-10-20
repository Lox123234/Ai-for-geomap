#pragma once

#include "GeorefService.h"

#include <QPointer>
#include <QQuickPaintedItem>

// The image pane of the georeferencing: the image with the points "from" of the links (numbered;
// red: where the map point of the link falls back on the image — the inverse residual), the point
// waiting for its place on the map, a loupe under the cursor. Click: a point "from"; drag a point:
// move it; drag elsewhere or with the right button: pan; wheel: zoom.
class GeorefImageView : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(GeorefService *service READ service WRITE setService NOTIFY serviceChanged)
    Q_PROPERTY(QString cursorText READ cursorText NOTIFY cursorChanged)
    Q_PROPERTY(bool loupe READ loupe WRITE setLoupe NOTIFY loupeChanged)
    Q_PROPERTY(double zoom READ zoom NOTIFY viewChanged)                 // screen pixels per image pixel
public:
    explicit GeorefImageView(QQuickItem *parent = nullptr);
    GeorefService *service() const { return m_service; }
    void setService(GeorefService *service);
    QString cursorText() const { return m_cursorText; }
    bool loupe() const { return m_loupe; }
    void setLoupe(bool on);
    double zoom() const { return m_zoom; }
    Q_INVOKABLE void fitAll();
    Q_INVOKABLE void zoomBy(double factor);
    Q_INVOKABLE void zoomToLink(int index);
    void paint(QPainter *painter) override;

signals:
    void serviceChanged();
    void cursorChanged();
    void loupeChanged();
    void viewChanged();

protected:
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void hoverLeaveEvent(QHoverEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    QPointF toScreen(const QPointF &image) const { return (image - m_origin) * m_zoom; }
    QPointF toImage(const QPointF &screen) const { return screen / m_zoom + m_origin; }
    int linkAt(const QPointF &screen) const;
    void imageChanged();

    QPointer<GeorefService> m_service;
    double m_zoom = 1;
    QPointF m_origin;                        // image pixel at the top-left corner of the pane
    bool m_fitted = false;
    QPointF m_press, m_last;
    Qt::MouseButton m_button = Qt::NoButton;
    bool m_panning = false;
    int m_dragLink = -1;
    QPointF m_dragTo;
    bool m_hover = false;
    QPointF m_hoverAt;
    QString m_cursorText;
    bool m_loupe = true;
};
