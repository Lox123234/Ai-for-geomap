#pragma once

#include "MapService.h"
#include <QPointer>
#include <QQuickPaintedItem>

// The map canvas for QML: paints MapService's frame and forwards pointer, wheel and key
// input. Raster tiles arrive asynchronously; the item repaints when they do.
class MapView : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(MapService *service READ service WRITE setService NOTIFY serviceChanged)
public:
    explicit MapView(QQuickItem *parent = nullptr);
    MapService *service() const { return m_service; }
    void setService(MapService *service);
    void paint(QPainter *painter) override;

signals:
    void serviceChanged();

protected:
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void updatePhysicalPixelSize();
    QPointer<MapService> m_service;
};
