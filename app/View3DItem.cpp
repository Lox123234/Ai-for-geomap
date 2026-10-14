#include "View3DItem.h"

#include <QFile>
#include <QSet>
#include <QMetaObject>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QtMath>
#include <functional>
#include <map>
#include <memory>
#include <rhi/qrhi.h>

using namespace view3d;

namespace {

QShader loadShader(const QString &name)
{
    QFile f(QStringLiteral(":/qt/qml/GeoMapAI/shaders/") + name + QStringLiteral(".qsb"));
    return f.open(QIODevice::ReadOnly) ? QShader::fromSerialized(f.readAll()) : QShader();
}

// Colours in memory order R, G, B, A for UNormByte4.
quint32 rgbaBytes(quint32 argb)
{
    const quint32 a = argb >> 24, r = (argb >> 16) & 0xff, g = (argb >> 8) & 0xff, b = argb & 0xff;
    return r | (g << 8) | (b << 16) | (a << 24);
}

struct PointsUniforms
{
    float mvp[16];
    float offset[4];
    float params[4];
    float heights[4];
    float pad[4];
};
struct MeshUniforms
{
    float mvp[16];
    float offset[4];
    float flags[4];
    float heights[4];
    float light[4];
};

class View3DRenderer : public QQuickRhiItemRenderer
{
public:
    explicit View3DRenderer(View3DItem *item) : m_item(item) {}

    void initialize(QRhiCommandBuffer *) override
    {
        if (m_rhi != rhi()) {   // new device: drop everything
            m_gpu.clear();
            m_pointPipeline.reset();
            m_meshPipeline.reset();
            m_rhi = rhi();
        }
        if (!m_corners) createShared();
        QRhiRenderPassDescriptor *rp = renderTarget()->renderPassDescriptor();
        if (!m_pointPipeline || m_sampleCount != renderTarget()->sampleCount() || !m_rp || !m_rp->isCompatible(rp)) {
            m_sampleCount = renderTarget()->sampleCount();
            m_rp = rp;
            createPipelines();
        }
        QMetaObject::invokeMethod(m_item, [item = QPointer<View3DItem>(m_item)] { if (item) item->setGpuReady(true); }, Qt::QueuedConnection);
    }

    void synchronize(QQuickRhiItem *item) override
    {
        auto *view = static_cast<View3DItem *>(item);
        if (view->scene()) m_snapshot = view->scene()->snapshot();
        else m_snapshot = {};
    }

    void render(QRhiCommandBuffer *cb) override
    {
        QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
        syncGpu(u);
        const QSize size = renderTarget()->pixelSize();
        const float aspect = float(size.width()) / float(qMax(1, size.height()));
        const Camera &cam = m_snapshot.camera;
        const float vs = m_snapshot.verticalScale;
        Bounds b = m_snapshot.bounds;
        float radius = 10.0f;
        if (b.valid) {
            b.min.setZ(b.min.z() * vs);
            b.max.setZ(b.max.z() * vs);
            radius = qMax(1.0f, b.radius());
        }
        const float dist = (cam.eye() - (b.valid ? b.center() : cam.target)).length();
        const float farPlane = dist + radius * 3.0f + 10.0f;
        const float nearPlane = qMax(farPlane * 1e-5f, qMax(0.01f, dist - radius * 3.0f));
        QMatrix4x4 model;
        model.scale(1.0f, 1.0f, vs);
        const QMatrix4x4 mvp = m_rhi->clipSpaceCorrMatrix() * cam.projection(aspect, cam.orthographic ? -farPlane : nearPlane, farPlane)
                               * cam.view() * model;
        const float zMin = m_snapshot.bounds.valid ? m_snapshot.bounds.min.z() : 0.0f;
        const float zMax = m_snapshot.bounds.valid ? m_snapshot.bounds.max.z() : 1.0f;
        const float dpr = float(size.height()) / float(qMax(1.0, m_item->height()));

        QVector<std::function<void()>> draws;
        for (const auto &layer : m_snapshot.layers) {
            if (!layer.visible) continue;
            auto it = m_gpu.find(layer.id);
            if (it == m_gpu.end() || !it->second.ready) continue;
            Gpu &g = it->second;
            const QVector3D off = offsetOf(layer);
            if (g.points) {
                PointsUniforms pu{};
                memcpy(pu.mvp, mvp.constData(), sizeof(pu.mvp));
                pu.offset[0] = off.x(); pu.offset[1] = off.y(); pu.offset[2] = off.z(); pu.offset[3] = vs;
                pu.params[0] = float(size.width()); pu.params[1] = float(size.height());
                pu.params[2] = m_snapshot.pointSize * dpr; pu.params[3] = m_snapshot.heightColors ? 1.0f : 0.0f;
                pu.heights[0] = zMin; pu.heights[1] = zMax; pu.heights[2] = g.hasColor ? 1.0f : 0.0f;
                u->updateDynamicBuffer(g.ubo.get(), 0, sizeof(pu), &pu);
                Gpu *gp = &g;
                draws << [this, cb, gp] {
                    cb->setGraphicsPipeline(m_pointPipeline.get());
                    cb->setShaderResources(gp->srb.get());
                    const QRhiCommandBuffer::VertexInput inputs[] = {{m_corners.get(), 0}, {gp->vbuf.get(), 0}};
                    cb->setVertexInput(0, 2, inputs);
                    cb->draw(4, quint32(gp->count));
                };
            } else {
                for (Part &part : g.parts) {
                    MeshUniforms mu{};
                    memcpy(mu.mvp, mvp.constData(), sizeof(mu.mvp));
                    mu.offset[0] = off.x(); mu.offset[1] = off.y(); mu.offset[2] = off.z(); mu.offset[3] = vs;
                    mu.flags[0] = part.textured ? 1.0f : 0.0f;
                    // Textured photogrammetry already carries its light; shade colours and terrain.
                    mu.flags[1] = m_snapshot.lighting && (!part.textured || g.terrain) ? 1.0f : 0.0f;
                    mu.flags[2] = m_snapshot.heightColors ? 1.0f : 0.0f;
                    mu.flags[3] = g.hasColor ? 1.0f : 0.0f;
                    mu.heights[0] = zMin; mu.heights[1] = zMax;
                    const float az = qDegreesToRadians(315.0f), el = qDegreesToRadians(45.0f);
                    mu.light[0] = std::sin(az) * std::cos(el); mu.light[1] = std::cos(az) * std::cos(el); mu.light[2] = std::sin(el);
                    mu.light[3] = 0.35f;
                    u->updateDynamicBuffer(part.ubo.get(), 0, sizeof(mu), &mu);
                    Gpu *gp = &g;
                    Part *pp = &part;
                    draws << [this, cb, gp, pp] {
                        cb->setGraphicsPipeline(m_meshPipeline.get());
                        cb->setShaderResources(pp->srb.get());
                        const QRhiCommandBuffer::VertexInput input(gp->vbuf.get(), 0);
                        cb->setVertexInput(0, 1, &input, pp->ibuf.get(), 0, QRhiCommandBuffer::IndexUInt32);
                        cb->drawIndexed(quint32(pp->count));
                    };
                }
            }
        }
        cb->beginPass(renderTarget(), QColor(23, 27, 34), {1.0f, 0}, u);
        cb->setViewport(QRhiViewport(0, 0, float(size.width()), float(size.height())));
        for (const auto &draw : draws) draw();
        cb->endPass();
    }

private:
    struct Part
    {
        std::unique_ptr<QRhiBuffer> ibuf;
        std::unique_ptr<QRhiBuffer> ubo;
        std::unique_ptr<QRhiTexture> texture;
        std::unique_ptr<QRhiShaderResourceBindings> srb;
        int count = 0;
        bool textured = false;
    };
    struct Gpu
    {
        int version = 0;
        bool ready = false;
        bool points = false;
        bool terrain = false;
        bool hasColor = false;
        int count = 0;
        std::unique_ptr<QRhiBuffer> vbuf;
        std::unique_ptr<QRhiBuffer> ubo;
        std::unique_ptr<QRhiShaderResourceBindings> srb;
        std::vector<Part> parts;
    };

    QVector3D offsetOf(const Scene3DService::Layer &l) const
    {
        const Origin o = l.cloud ? l.cloud->origin : l.mesh ? l.mesh->origin : Origin{};
        const Origin &s = m_snapshot.origin;
        if (!o.valid || !s.valid) return {};
        return QVector3D(float(o.x - s.x), float(o.y - s.y), float(o.z - s.z));
    }

    void createShared()
    {
        static const float corners[] = {-1, -1, 1, -1, -1, 1, 1, 1};
        m_corners.reset(m_rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(corners)));
        m_corners->create();
        m_white.reset(m_rhi->newTexture(QRhiTexture::RGBA8, QSize(1, 1)));
        m_white->create();
        m_sampler.reset(m_rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::Linear,
                                          QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
        m_sampler->create();
        m_layoutUbo.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(MeshUniforms)));
        m_layoutUbo->create();
        QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
        u->uploadStaticBuffer(m_corners.get(), corners);
        QImage white(1, 1, QImage::Format_RGBA8888);
        white.fill(Qt::white);
        u->uploadTexture(m_white.get(), white);
        m_pendingShared = u;
    }

    void createPipelines()
    {
        const auto stages = QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;
        m_pointLayout.reset(m_rhi->newShaderResourceBindings());
        m_pointLayout->setBindings({QRhiShaderResourceBinding::uniformBuffer(0, stages, m_layoutUbo.get())});
        m_pointLayout->create();
        m_meshLayout.reset(m_rhi->newShaderResourceBindings());
        m_meshLayout->setBindings({QRhiShaderResourceBinding::uniformBuffer(0, stages, m_layoutUbo.get()),
                                   QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                                             m_white.get(), m_sampler.get())});
        m_meshLayout->create();

        m_pointPipeline.reset(m_rhi->newGraphicsPipeline());
        m_pointPipeline->setShaderStages({{QRhiShaderStage::Vertex, loadShader(QStringLiteral("points.vert"))},
                                          {QRhiShaderStage::Fragment, loadShader(QStringLiteral("points.frag"))}});
        QRhiVertexInputLayout pl;
        pl.setBindings({{2 * sizeof(float)}, {4 * sizeof(float), QRhiVertexInputBinding::PerInstance}});
        pl.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0},
                          {1, 1, QRhiVertexInputAttribute::Float3, 0},
                          {1, 2, QRhiVertexInputAttribute::UNormByte4, 3 * sizeof(float)}});
        m_pointPipeline->setVertexInputLayout(pl);
        m_pointPipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
        m_pointPipeline->setDepthTest(true);
        m_pointPipeline->setDepthWrite(true);
        m_pointPipeline->setSampleCount(m_sampleCount);
        m_pointPipeline->setShaderResourceBindings(m_pointLayout.get());
        m_pointPipeline->setRenderPassDescriptor(m_rp);
        m_pointPipeline->create();

        m_meshPipeline.reset(m_rhi->newGraphicsPipeline());
        m_meshPipeline->setShaderStages({{QRhiShaderStage::Vertex, loadShader(QStringLiteral("mesh.vert"))},
                                         {QRhiShaderStage::Fragment, loadShader(QStringLiteral("mesh.frag"))}});
        QRhiVertexInputLayout ml;
        ml.setBindings({{9 * sizeof(float)}});
        ml.setAttributes({{0, 0, QRhiVertexInputAttribute::Float3, 0},
                          {0, 1, QRhiVertexInputAttribute::Float3, 3 * sizeof(float)},
                          {0, 2, QRhiVertexInputAttribute::Float2, 6 * sizeof(float)},
                          {0, 3, QRhiVertexInputAttribute::UNormByte4, 8 * sizeof(float)}});
        m_meshPipeline->setVertexInputLayout(ml);
        m_meshPipeline->setTopology(QRhiGraphicsPipeline::Triangles);
        m_meshPipeline->setCullMode(QRhiGraphicsPipeline::None);
        m_meshPipeline->setDepthTest(true);
        m_meshPipeline->setDepthWrite(true);
        m_meshPipeline->setSampleCount(m_sampleCount);
        m_meshPipeline->setShaderResourceBindings(m_meshLayout.get());
        m_meshPipeline->setRenderPassDescriptor(m_rp);
        m_meshPipeline->create();
    }

    // Creates GPU resources for new or changed layers, drops removed ones.
    void syncGpu(QRhiResourceUpdateBatch *u)
    {
        if (m_pendingShared) {
            u->merge(m_pendingShared);
            m_pendingShared->release();
            m_pendingShared = nullptr;
        }
        QSet<QString> alive;
        const int maxTexture = m_rhi->resourceLimit(QRhi::TextureSizeMax);
        const auto stages = QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;
        for (const auto &layer : m_snapshot.layers) {
            alive.insert(layer.id);
            if (layer.loading || !layer.error.isEmpty() || (!layer.cloud && !layer.mesh)) continue;
            Gpu &g = m_gpu[layer.id];
            if (g.ready && g.version == layer.version) continue;
            g = Gpu();
            g.version = layer.version;
            if (layer.cloud) {
                const PointCloud &c = *layer.cloud;
                g.points = true;
                g.hasColor = !c.colors.isEmpty();
                g.count = int(c.positions.size());
                QByteArray data(g.count * 16, Qt::Uninitialized);
                char *p = data.data();
                for (int i = 0; i < g.count; ++i, p += 16) {
                    memcpy(p, &c.positions[i], 12);
                    const quint32 col = rgbaBytes(g.hasColor ? c.colors[i] : 0xffc8c8c8u);
                    memcpy(p + 12, &col, 4);
                }
                g.vbuf.reset(m_rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, quint32(qMax(16, int(data.size())))));
                g.vbuf->create();
                u->uploadStaticBuffer(g.vbuf.get(), data.constData());
                g.ubo.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(PointsUniforms)));
                g.ubo->create();
                g.srb.reset(m_rhi->newShaderResourceBindings());
                g.srb->setBindings({QRhiShaderResourceBinding::uniformBuffer(0, stages, g.ubo.get())});
                g.srb->create();
            } else {
                const Mesh &m = *layer.mesh;
                g.terrain = layer.kind == QLatin1String("terrain");
                g.hasColor = !m.colors.isEmpty();
                const int n = int(m.positions.size());
                QByteArray data(n * 36, Qt::Uninitialized);
                char *p = data.data();
                for (int i = 0; i < n; ++i, p += 36) {
                    memcpy(p, &m.positions[i], 12);
                    const QVector3D nrm = i < m.normals.size() ? m.normals[i] : QVector3D(0, 0, 1);
                    memcpy(p + 12, &nrm, 12);
                    const QVector2D uv = i < m.uvs.size() ? m.uvs[i] : QVector2D();
                    memcpy(p + 24, &uv, 8);
                    const quint32 col = rgbaBytes(i < m.colors.size() ? m.colors[i] : 0xffcccccc);
                    memcpy(p + 32, &col, 4);
                }
                g.vbuf.reset(m_rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, quint32(qMax(36, int(data.size())))));
                g.vbuf->create();
                u->uploadStaticBuffer(g.vbuf.get(), data.constData());
                for (const Mesh::Part &mp : m.parts) {
                    if (mp.indices.isEmpty()) continue;
                    Part part;
                    part.count = int(mp.indices.size());
                    part.ibuf.reset(m_rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::IndexBuffer, quint32(mp.indices.size() * 4)));
                    part.ibuf->create();
                    u->uploadStaticBuffer(part.ibuf.get(), mp.indices.constData());
                    part.ubo.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(MeshUniforms)));
                    part.ubo->create();
                    QRhiTexture *texture = m_white.get();
                    if (!mp.texture.isNull() && !m.uvs.isEmpty()) {
                        QImage image = mp.texture;
                        if (image.width() > maxTexture || image.height() > maxTexture)
                            image = image.scaled(maxTexture, maxTexture, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                        part.texture.reset(m_rhi->newTexture(QRhiTexture::RGBA8, image.size(), 1, QRhiTexture::MipMapped | QRhiTexture::UsedWithGenerateMips));
                        part.texture->create();
                        u->uploadTexture(part.texture.get(), image);
                        u->generateMips(part.texture.get());
                        texture = part.texture.get();
                        part.textured = true;
                    }
                    if (!m_mipSampler) {
                        m_mipSampler.reset(m_rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::Linear,
                                                             QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
                        m_mipSampler->create();
                    }
                    part.srb.reset(m_rhi->newShaderResourceBindings());
                    part.srb->setBindings({QRhiShaderResourceBinding::uniformBuffer(0, stages, part.ubo.get()),
                                           QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, texture,
                                                                                     m_mipSampler.get())});
                    part.srb->create();
                    g.parts.push_back(std::move(part));
                }
            }
            g.ready = true;
        }
        for (auto it = m_gpu.begin(); it != m_gpu.end();) {
            if (!alive.contains(it->first)) it = m_gpu.erase(it);
            else ++it;
        }
    }

    View3DItem *m_item;
    QRhi *m_rhi = nullptr;
    QRhiRenderPassDescriptor *m_rp = nullptr;
    int m_sampleCount = 1;
    Scene3DService::Snapshot m_snapshot;
    std::map<QString, Gpu> m_gpu;   // GPU resources by layer id (move-only)
    std::unique_ptr<QRhiBuffer> m_corners;
    std::unique_ptr<QRhiBuffer> m_layoutUbo;
    std::unique_ptr<QRhiTexture> m_white;
    std::unique_ptr<QRhiSampler> m_sampler;
    std::unique_ptr<QRhiSampler> m_mipSampler;
    std::unique_ptr<QRhiShaderResourceBindings> m_pointLayout;
    std::unique_ptr<QRhiShaderResourceBindings> m_meshLayout;
    std::unique_ptr<QRhiGraphicsPipeline> m_pointPipeline;
    std::unique_ptr<QRhiGraphicsPipeline> m_meshPipeline;
    QRhiResourceUpdateBatch *m_pendingShared = nullptr;
};

} // namespace

View3DItem::View3DItem(QQuickItem *parent) : QQuickRhiItem(parent)
{
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton | Qt::MiddleButton);
    setSampleCount(4);
}

void View3DItem::setScene(Scene3DService *scene)
{
    if (scene == m_scene) return;
    if (m_scene) disconnect(m_scene, nullptr, this, nullptr);
    m_scene = scene;
    if (m_scene) {
        connect(m_scene, &Scene3DService::changed, this, &QQuickItem::update);
        if (width() > 0 && height() > 0) m_scene->setViewAspect(float(width() / height()));
    }
    emit sceneChanged();
    update();
}

void View3DItem::setGpuReady(bool ready)
{
    if (ready == m_gpuReady) return;
    m_gpuReady = ready;
    emit gpuReadyChanged();
}

void View3DItem::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickRhiItem::geometryChange(newGeometry, oldGeometry);
    if (m_scene && newGeometry.width() > 0 && newGeometry.height() > 0)
        m_scene->setViewAspect(float(newGeometry.width() / newGeometry.height()));
}

QQuickRhiItemRenderer *View3DItem::createRenderer()
{
    return new View3DRenderer(this);
}

void View3DItem::mousePressEvent(QMouseEvent *event)
{
    m_last = m_pressed = event->position();
    m_button = event->button();
    m_dragged = false;
    forceActiveFocus();
    event->accept();
}

void View3DItem::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_scene) return;
    const QPointF d = event->position() - m_last;
    m_last = event->position();
    if ((event->position() - m_pressed).manhattanLength() > 3) m_dragged = true;
    const QString tool = m_scene->tool();
    if (m_button == Qt::RightButton || m_button == Qt::MiddleButton || tool == QLatin1String("pan")) {
        m_scene->pan(float(d.x()), float(d.y()), float(height()));
    } else if (tool == QLatin1String("zoom")) {
        m_scene->zoom(float(-d.y() / 20.0));
    } else {
        m_scene->orbit(float(d.x()), float(d.y()));
    }
    event->accept();
}

void View3DItem::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_scene && !m_dragged && m_button == Qt::LeftButton)
        m_scene->pickAt(float(event->position().x()), float(event->position().y()), float(width()), float(height()));
    m_button = Qt::NoButton;
    event->accept();
}

void View3DItem::wheelEvent(QWheelEvent *event)
{
    if (m_scene) m_scene->zoom(float(event->angleDelta().y()) / 120.0f);
    event->accept();
}
