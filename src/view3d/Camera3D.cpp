#include "Camera3D.h"

#include <QtMath>
#include <cmath>
#include <limits>

namespace view3d {

QVector3D Camera::forward() const
{
    const float y = qDegreesToRadians(yaw), p = qDegreesToRadians(pitch);
    // Looking down for positive pitch.
    return QVector3D(std::sin(y) * std::cos(p), std::cos(y) * std::cos(p), -std::sin(p)).normalized();
}

QVector3D Camera::up() const
{
    const QVector3D f = forward();
    QVector3D right = QVector3D::crossProduct(f, QVector3D(0, 0, 1));
    if (right.lengthSquared() < 1e-8f) right = QVector3D(std::cos(qDegreesToRadians(yaw)), -std::sin(qDegreesToRadians(yaw)), 0);
    return QVector3D::crossProduct(right.normalized(), f).normalized();
}

QMatrix4x4 Camera::view() const
{
    QMatrix4x4 m;
    m.lookAt(eye(), target, up());
    return m;
}

float Camera::orthoHeight() const
{
    return 2.0f * distance * std::tan(qDegreesToRadians(fovY) * 0.5f);
}

QMatrix4x4 Camera::projection(float aspect, float nearPlane, float farPlane) const
{
    QMatrix4x4 m;
    if (orthographic) {
        const float h = orthoHeight() * 0.5f, w = h * aspect;
        m.ortho(-w, w, -h, h, nearPlane, farPlane);
    } else {
        m.perspective(fovY, aspect, nearPlane, farPlane);
    }
    return m;
}

void Camera::fit(const Bounds &bounds, const QVector<QVector3D> &samples)
{
    target = bounds.valid ? bounds.center() : QVector3D();
    if (!bounds.valid) {
        distance = 10.0f;
        return;
    }
    // Every corner of the box inside the view: in camera axes a corner at (x right, y up,
    // z forward from the target) is seen when |y| <= (d + z)·tan(fov/2) and |x| <= that·aspect
    // (orthographic: without the depth term).
    const QVector3D f = forward(), u = up();
    const QVector3D r = QVector3D::crossProduct(f, u).normalized();
    const float t = std::tan(qDegreesToRadians(fovY) * 0.5f), a = qMax(0.1f, aspect);
    float d = 0.5f;
    const int count = samples.isEmpty() ? 8 : int(samples.size());
    for (int i = 0; i < count; ++i) {
        const QVector3D c = !samples.isEmpty() ? samples[i]
            : QVector3D((i & 1) ? bounds.max.x() : bounds.min.x(), (i & 2) ? bounds.max.y() : bounds.min.y(),
                        (i & 4) ? bounds.max.z() : bounds.min.z());
        const QVector3D v = c - target;
        const float x = std::fabs(QVector3D::dotProduct(v, r)), y = std::fabs(QVector3D::dotProduct(v, u));
        const float z = orthographic ? 0.0f : QVector3D::dotProduct(v, f);
        d = qMax(d, qMax(y / t, x / (t * a)) - z);
        if (!orthographic) d = qMax(d, 0.5f - z);   // in front of the eye
    }
    distance = d * 1.08f;
}

void Camera::setPreset(Preset preset, const Bounds &bounds, const QVector<QVector3D> &samples)
{
    switch (preset) {
    case Top: yaw = 0; pitch = 89.9f; break;
    case Bottom: yaw = 0; pitch = -89.9f; break;
    case Front: yaw = 0; pitch = 0; break;      // looking north
    case Back: yaw = 180; pitch = 0; break;
    case Left: yaw = 90; pitch = 0; break;      // looking east
    case Right: yaw = 270; pitch = 0; break;
    case Iso: yaw = 30; pitch = 35; break;
    }
    fit(bounds, samples);
}

void Camera::orbit(float dxPixels, float dyPixels)
{
    yaw = std::fmod(yaw + dxPixels * 0.3f + 360.0f, 360.0f);
    pitch = qBound(-89.9f, pitch + dyPixels * 0.3f, 89.9f);
}

void Camera::pan(float dxPixels, float dyPixels, float viewportHeight)
{
    const float worldPerPixel = orthoHeight() / qMax(1.0f, viewportHeight);
    const QVector3D f = forward(), u = up();
    const QVector3D right = QVector3D::crossProduct(f, u).normalized();
    target += (-right * dxPixels + u * dyPixels) * worldPerPixel;
}

void Camera::zoom(float steps)
{
    distance = qBound(0.01f, distance * std::pow(0.85f, steps), 1e7f);
}

void Camera::ray(const QPointF &pixel, const QSizeF &viewport, QVector3D *origin, QVector3D *direction) const
{
    const float aspect = float(viewport.width() / qMax(1.0, viewport.height()));
    const float nx = float(2.0 * pixel.x() / qMax(1.0, viewport.width()) - 1.0);
    const float ny = float(1.0 - 2.0 * pixel.y() / qMax(1.0, viewport.height()));
    const QVector3D f = forward(), u = up();
    const QVector3D right = QVector3D::crossProduct(f, u).normalized();
    const float halfH = orthoHeight() * 0.5f;
    if (orthographic) {
        *origin = eye() + right * (nx * halfH * aspect) + u * (ny * halfH);
        *direction = f;
    } else {
        const float t = std::tan(qDegreesToRadians(fovY) * 0.5f);
        *origin = eye();
        *direction = (f + right * (nx * t * aspect) + u * (ny * t)).normalized();
    }
}

Pick pickPoint(const PointCloud &cloud, const QVector3D &origin, const QVector3D &direction, float tolerance, int stride)
{
    Pick best;
    float bestScore = std::numeric_limits<float>::max();
    const QVector3D d = direction.normalized();
    for (int i = 0; i < cloud.positions.size(); i += qMax(1, stride)) {
        const QVector3D v = cloud.positions[i] - origin;
        const float along = QVector3D::dotProduct(v, d);
        if (along <= 0) continue;
        const float off = (v - d * along).length();
        if (off > tolerance) continue;
        // Prefer the nearest along the ray among points close to it.
        const float score = along + off * 4.0f;
        if (score < bestScore) {
            bestScore = score;
            best.hit = true;
            best.position = cloud.positions[i];
            best.distance = along;
        }
    }
    return best;
}

Pick pickMesh(const Mesh &mesh, const QVector3D &origin, const QVector3D &direction)
{
    Pick best;
    best.distance = std::numeric_limits<float>::max();
    const QVector3D d = direction.normalized();
    for (const Mesh::Part &part : mesh.parts)
        for (int i = 0; i + 2 < part.indices.size(); i += 3) {
            const QVector3D &a = mesh.positions[part.indices[i]], &b = mesh.positions[part.indices[i + 1]],
                            &c = mesh.positions[part.indices[i + 2]];
            const QVector3D e1 = b - a, e2 = c - a;
            const QVector3D p = QVector3D::crossProduct(d, e2);
            const float det = QVector3D::dotProduct(e1, p);
            if (std::fabs(det) < 1e-12f) continue;
            const float inv = 1.0f / det;
            const QVector3D s = origin - a;
            const float u = QVector3D::dotProduct(s, p) * inv;
            if (u < 0 || u > 1) continue;
            const QVector3D q = QVector3D::crossProduct(s, e1);
            const float v = QVector3D::dotProduct(d, q) * inv;
            if (v < 0 || u + v > 1) continue;
            const float t = QVector3D::dotProduct(e2, q) * inv;
            if (t > 0 && t < best.distance) {
                best.hit = true;
                best.distance = t;
                best.position = origin + d * t;
            }
        }
    if (!best.hit) best.distance = 0;
    return best;
}

} // namespace view3d
