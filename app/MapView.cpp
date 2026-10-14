#include "MapView.h"
#include "MapService.h"
#include <QPainter>
#include <QQuickWindow>
#include <QScreen>

MapView::MapView(QQuickItem *parent) : QQuickPaintedItem(parent)
{
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton | Qt::MiddleButton);
    setAcceptHoverEvents(true);
    setFlag(ItemAcceptsInputMethod, false);
    setActiveFocusOnTab(true);
    setOpaquePainting(true);
    setAntialiasing(true);
}

void MapView::setService(MapService *service)
{
    if (m_service == service) return;
    if (m_service) disconnect(m_service, nullptr, this, nullptr);
    m_service = service;
    if (m_service) {
        connect(m_service, &MapService::repaint, this, [this] { update(); });
        m_service->setViewSize(size());
        updatePhysicalPixelSize();
    }
    emit serviceChanged();
    update();
}

void MapView::paint(QPainter *painter)
{
    if (m_service) m_service->paint(painter, size(), true);
}

void MapView::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    if (m_service && newGeometry.size() != oldGeometry.size()) m_service->setViewSize(newGeometry.size());
    updatePhysicalPixelSize();
}

void MapView::updatePhysicalPixelSize()
{
    // Physical size of one logical pixel, for the 1 : N scale.
    if (!m_service || !window() || !window()->screen()) return;
    const QScreen *screen = window()->screen();
    const double dpi = screen->physicalDotsPerInchX();
    if (dpi > 0) m_service->setMetersPerScreenPixel(0.0254 / dpi * screen->devicePixelRatio());
}

void MapView::mousePressEvent(QMouseEvent *event)
{
    forceActiveFocus();
    if (m_service) m_service->pointerPressed(event->position(), event->button(), event->modifiers());
    event->accept();
}

void MapView::mouseMoveEvent(QMouseEvent *event)
{
    if (m_service) {
        m_service->pointerDragged(event->position());
        m_service->pointerMoved(event->position());
    }
    event->accept();
}

void MapView::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_service && event->button() != Qt::RightButton) m_service->pointerReleased(event->position());
    event->accept();
}

void MapView::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (m_service && event->button() == Qt::LeftButton) m_service->pointerDoubleClicked(event->position());
    event->accept();
}

void MapView::hoverMoveEvent(QHoverEvent *event)
{
    if (m_service) m_service->pointerMoved(event->position());
}

void MapView::wheelEvent(QWheelEvent *event)
{
    if (!m_service) return;
    const double steps = event->angleDelta().y() / 120.0;
    if (steps != 0) m_service->zoomAt(event->position(), std::pow(1.25, steps));
    event->accept();
}

void MapView::keyPressEvent(QKeyEvent *event)
{
    if (m_service && m_service->keyPressed(event->key())) event->accept();
    else QQuickPaintedItem::keyPressEvent(event);
}
