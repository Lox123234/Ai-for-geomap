#pragma once

#include "Scene3DService.h"

#include <QPointF>
#include <QPointer>
#include <QQuickRhiItem>

// The 3D view: draws the Scene3DService snapshot with the GPU through Qt's RHI (Direct3D,
// Vulkan, Metal or OpenGL, whatever Qt Quick uses) — point clouds as round screen-sized points,
// meshes and terrains with textures, vertex colours or heights, and sun lighting. Mouse: left
// drag by tool (orbit, pan, zoom), right or middle drag pans, wheel zooms, a click picks.
class View3DItem : public QQuickRhiItem
{
    Q_OBJECT
    Q_PROPERTY(Scene3DService *scene READ scene WRITE setScene NOTIFY sceneChanged)
    Q_PROPERTY(bool gpuReady READ gpuReady NOTIFY gpuReadyChanged)   // false with the software renderer

public:
    explicit View3DItem(QQuickItem *parent = nullptr);

    Scene3DService *scene() const { return m_scene; }
    void setScene(Scene3DService *scene);
    bool gpuReady() const { return m_gpuReady; }
    void setGpuReady(bool ready);

signals:
    void sceneChanged();
    void gpuReadyChanged();

protected:
    QQuickRhiItemRenderer *createRenderer() override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private:
    QPointer<Scene3DService> m_scene;
    QPointF m_last;
    QPointF m_pressed;
    Qt::MouseButton m_button = Qt::NoButton;
    bool m_dragged = false;
    bool m_gpuReady = false;
};
