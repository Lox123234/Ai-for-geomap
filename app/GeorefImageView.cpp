#include "GeorefImageView.h"

#include <QCursor>
#include <QLocale>
#include <QPainter>
#include <cmath>

GeorefImageView::GeorefImageView(QQuickItem *parent) : QQuickPaintedItem(parent)
{
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton | Qt::MiddleButton);
    setAcceptHoverEvents(true);
    setOpaquePainting(true);
    setAntialiasing(true);
    setCursor(QCursor(Qt::CrossCursor));
}

void GeorefImageView::setService(GeorefService *service)
{
    if (m_service == service) return;
    if (m_service) disconnect(m_service, nullptr, this, nullptr);
    m_service = service;
    if (m_service) {
        connect(m_service, &GeorefService::repaint, this, [this] { update(); });
        connect(m_service, &GeorefService::imageChanged, this, &GeorefImageView::imageChanged);
    }
    imageChanged();
    emit serviceChanged();
}

void GeorefImageView::imageChanged()
{
    m_fitted = false;
    m_dragLink = -1;
    if (m_service && !m_service->image().isNull()) fitAll();
    update();
}

void GeorefImageView::setLoupe(bool on)
{
    if (on == m_loupe) return;
    m_loupe = on;
    emit loupeChanged();
    update();
}

void GeorefImageView::fitAll()
{
    if (!m_service || m_service->imageWidth() <= 0 || width() <= 0 || height() <= 0) return;
    const double w = m_service->imageWidth(), h = m_service->imageHeight();
    m_zoom = qMin(width() / w, height() / h) * 0.96;
    m_origin = QPointF(w / 2, h / 2) - QPointF(width() / 2, height() / 2) / m_zoom;
    m_fitted = true;
    emit viewChanged();
    update();
}

void GeorefImageView::zoomBy(double factor)
{
    const QPointF c(width() / 2, height() / 2), at = toImage(c);
    m_zoom = qBound(1e-3, m_zoom * factor, 64.0);
    m_origin = at - c / m_zoom;
    emit viewChanged();
    update();
}

void GeorefImageView::zoomToLink(int index)
{
    if (!m_service || index < 0 || index >= m_service->linkList().size()) return;
    m_zoom = qMax(m_zoom, 1.0);
    m_origin = m_service->linkList()[index].image - QPointF(width() / 2, height() / 2) / m_zoom;
    emit viewChanged();
    update();
}

void GeorefImageView::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() == oldGeometry.size()) return;
    if (!m_fitted) fitAll();
    else if (!oldGeometry.isEmpty()) {   // keep the centre
        const QPointF c = toImage(QPointF(oldGeometry.width() / 2, oldGeometry.height() / 2));
        m_origin = c - QPointF(newGeometry.width() / 2, newGeometry.height() / 2) / m_zoom;
    }
}

int GeorefImageView::linkAt(const QPointF &screen) const
{
    if (!m_service) return -1;
    int best = -1;
    double bestD = 9;
    const QVector<georef::Link> &links = m_service->linkList();
    for (int i = 0; i < links.size(); ++i) {
        const double d = QLineF(toScreen(links[i].image), screen).length();
        if (d < bestD) {
            bestD = d;
            best = i;
        }
    }
    return best;
}

void GeorefImageView::paint(QPainter *p)
{
    p->fillRect(QRectF(0, 0, width(), height()), QColor(22, 26, 31));
    if (!m_service || !m_service->active()) return;
    if (m_service->image().isNull()) {
        p->setPen(QColor(141, 154, 170));
        p->drawText(QRectF(0, 0, width(), height()), Qt::AlignCenter, m_service->loading() ? QStringLiteral("Открытие снимка…") : QString());
        return;
    }
    const double fw = m_service->imageWidth();
    const QImage &img = m_zoom * fw < m_service->smallImage().width() * 1.3 ? m_service->smallImage() : m_service->image();
    const double k = img.width() / fw;
    // The visible part of the image only.
    const QRectF visible = QRectF(toImage(QPointF(0, 0)), toImage(QPointF(width(), height()))).intersected(QRectF(0, 0, fw, m_service->imageHeight()));
    if (!visible.isEmpty()) {
        p->setRenderHint(QPainter::SmoothPixmapTransform, m_zoom < 2);
        const QRectF src(visible.x() * k, visible.y() * k, visible.width() * k, visible.height() * k);
        p->drawImage(QRectF(toScreen(visible.topLeft()), toScreen(visible.bottomRight())), img, src);
    }
    QPen frame(QColor(0, 200, 255, 120), 1);
    frame.setCosmetic(true);
    p->setPen(frame);
    p->setBrush(Qt::NoBrush);
    p->drawRect(QRectF(toScreen(QPointF(0, 0)), toScreen(QPointF(fw, m_service->imageHeight()))));

    auto cross = [&](const QPointF &at, const QColor &c, double r) {
        QPen pen(QColor(0, 0, 0, 170), 4);
        p->setPen(pen);
        p->drawLine(at - QPointF(r, 0), at + QPointF(r, 0));
        p->drawLine(at - QPointF(0, r), at + QPointF(0, r));
        pen = QPen(c, 2);
        p->setPen(pen);
        p->drawLine(at - QPointF(r, 0), at + QPointF(r, 0));
        p->drawLine(at - QPointF(0, r), at + QPointF(0, r));
    };
    QFont font = p->font();
    font.setPixelSize(12);
    font.setBold(true);
    p->setFont(font);
    const georef::Fit &fit = m_service->fitResult();
    const bool fitted = m_service->fitted();
    const QVector<georef::Link> &links = m_service->linkList();
    for (int i = 0; i < links.size(); ++i) {
        const georef::Link &l = links[i];
        const QPointF at = toScreen(i == m_dragLink ? m_dragTo : l.image);
        const bool sel = i == m_service->selectedLink();
        // Inverse residual: where the link's map point falls on the image (true size).
        if (fitted && l.enabled && i != m_dragLink) {
            const QPointF back = toScreen(fit.inverse.map(l.map));
            QPen r(QColor(255, 70, 70), 2);
            p->setPen(r);
            p->drawLine(at, back);
            p->setBrush(QColor(255, 70, 70));
            p->drawEllipse(back, 2.5, 2.5);
            p->setBrush(Qt::NoBrush);
        }
        cross(at, l.enabled ? QColor(60, 220, 90) : QColor(160, 160, 160), sel ? 10 : 7);
        if (sel) {
            p->setPen(QPen(QColor(255, 220, 60), 2));
            p->drawEllipse(at, 13, 13);
        }
        const QString label = QString::number(i + 1);
        const QRectF box(at + QPointF(9, -20), QSizeF(p->fontMetrics().horizontalAdvance(label) + 8, 16));
        p->setPen(Qt::NoPen);
        p->setBrush(QColor(0, 0, 0, 170));
        p->drawRoundedRect(box, 3, 3);
        p->setBrush(Qt::NoBrush);
        p->setPen(l.enabled ? Qt::white : QColor(170, 170, 170));
        p->drawText(box, Qt::AlignCenter, label);
    }
    if (const auto pending = m_service->pendingPoint()) {
        const QPointF at = toScreen(*pending);
        cross(at, QColor(255, 220, 60), 11);
        p->setPen(QPen(QColor(255, 220, 60), 2, Qt::DotLine));
        p->drawEllipse(at, 15, 15);
    }
    // Loupe: four times the view around the cursor, for a point put to the pixel.
    if (m_loupe && m_hover && m_dragLink < 0 && !m_panning) {
        const double side = 150, z = qMax(4.0, m_zoom * 4);
        const QRectF box(width() - side - 10, 10, side, side);
        const QPointF c = toImage(m_hoverAt);
        const double r = side / 2 / z;
        const QImage &big = m_service->image();
        const double kb = big.width() / fw;
        p->fillRect(box, QColor(22, 26, 31));
        p->setRenderHint(QPainter::SmoothPixmapTransform, false);
        const QRectF want(c - QPointF(r, r), c + QPointF(r, r));
        const QRectF part = want.intersected(QRectF(0, 0, fw, m_service->imageHeight()));
        if (!part.isEmpty()) {
            const QRectF to(box.topLeft() + (part.topLeft() - want.topLeft()) * z, part.size() * z);
            p->drawImage(to, big, QRectF(part.x() * kb, part.y() * kb, part.width() * kb, part.height() * kb));
        }
        p->setPen(QPen(QColor(255, 220, 60), 1));
        p->drawLine(QPointF(box.center().x(), box.top() + 4), QPointF(box.center().x(), box.bottom() - 4));
        p->drawLine(QPointF(box.left() + 4, box.center().y()), QPointF(box.right() - 4, box.center().y()));
        p->setPen(QPen(QColor(0, 200, 255), 1.5));
        p->drawRect(box);
    }
}

void GeorefImageView::mousePressEvent(QMouseEvent *event)
{
    forceActiveFocus();
    m_press = m_last = event->position();
    m_button = event->button();
    m_panning = false;
    m_dragLink = event->button() == Qt::LeftButton ? linkAt(event->position()) : -1;
    if (m_dragLink >= 0 && m_service) m_dragTo = m_service->linkList()[m_dragLink].image;
    event->accept();
}

void GeorefImageView::mouseMoveEvent(QMouseEvent *event)
{
    const QPointF pos = event->position();
    m_hoverAt = pos;
    if (m_dragLink >= 0 && (pos - m_press).manhattanLength() > 3) {
        m_dragTo = toImage(pos);
        update();
    } else if (m_dragLink < 0) {
        if (!m_panning && (pos - m_press).manhattanLength() > 4) m_panning = true;
        if (m_panning) {
            m_origin -= (pos - m_last) / m_zoom;
            emit viewChanged();
            update();
        }
    }
    m_last = pos;
    event->accept();
}

void GeorefImageView::mouseReleaseEvent(QMouseEvent *event)
{
    const QPointF pos = event->position();
    const bool moved = (pos - m_press).manhattanLength() > 3;
    if (m_service && m_button == Qt::LeftButton) {
        if (m_dragLink >= 0) {
            if (moved) {
                const QPointF to = toImage(pos);
                m_service->moveImagePoint(m_dragLink, to.x(), to.y());
            }
            m_service->setSelectedLink(m_dragLink);
        } else if (!m_panning) {
            const QPointF at = toImage(pos);
            m_service->addImagePoint(at.x(), at.y());
        }
    }
    m_dragLink = -1;
    m_panning = false;
    m_button = Qt::NoButton;
    update();
    event->accept();
}

void GeorefImageView::hoverMoveEvent(QHoverEvent *event)
{
    m_hover = true;
    m_hoverAt = event->position();
    if (m_service && m_service->imageWidth() > 0) {
        const QPointF at = toImage(m_hoverAt);
        const QString t = at.x() >= 0 && at.y() >= 0 && at.x() <= m_service->imageWidth() && at.y() <= m_service->imageHeight()
                              ? QStringLiteral("x %1  y %2 пикс.").arg(QLocale().toString(at.x(), 'f', 1), QLocale().toString(at.y(), 'f', 1))
                              : QString();
        if (t != m_cursorText) {
            m_cursorText = t;
            emit cursorChanged();
        }
    }
    if (m_loupe) update();
}

void GeorefImageView::hoverLeaveEvent(QHoverEvent *)
{
    m_hover = false;
    if (!m_cursorText.isEmpty()) {
        m_cursorText.clear();
        emit cursorChanged();
    }
    update();
}

void GeorefImageView::wheelEvent(QWheelEvent *event)
{
    const double steps = event->angleDelta().y() / 120.0;
    if (steps != 0) {
        const QPointF at = toImage(event->position());
        m_zoom = qBound(1e-3, m_zoom * std::pow(1.25, steps), 64.0);
        m_origin = at - event->position() / m_zoom;
        emit viewChanged();
        update();
    }
    event->accept();
}
