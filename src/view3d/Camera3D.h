#pragma once

#include "Scene3D.h"

#include <QMatrix4x4>
#include <QPointF>
#include <QSizeF>

namespace view3d {

// Orbit camera of the 3D view in a Z-up scene (X east, Y north): yaw around Z (0° = looking
// north), pitch above the horizon, distance from the target. Perspective or orthographic.
struct Camera
{
    enum Preset { Top, Bottom, Front, Back, Left, Right, Iso };

    QVector3D target;
    float distance = 100.0f;
    float yaw = 30.0f;          // degrees, 0 = looking towards +Y (north)
    float pitch = 35.0f;        // degrees above the horizon, (-89, 89)
    float fovY = 45.0f;         // degrees, perspective
    bool orthographic = false;
    float aspect = 1.5f;        // viewport width / height, for fitting

    QVector3D forward() const;  // unit vector from the eye to the target
    QVector3D eye() const { return target - forward() * distance; }
    QVector3D up() const;
    QMatrix4x4 view() const;
    QMatrix4x4 projection(float aspect, float nearPlane, float farPlane) const;
    float orthoHeight() const;  // visible world height in orthographic mode

    // Target at the centre of the box, the nearest distance that shows the box corners, or the
    // given sample points of the data when there are any (tighter for irregular shapes).
    void fit(const Bounds &bounds, const QVector<QVector3D> &samples = {});
    void setPreset(Preset preset, const Bounds &bounds, const QVector<QVector3D> &samples = {});
    void orbit(float dxPixels, float dyPixels);
    void pan(float dxPixels, float dyPixels, float viewportHeight);
    void zoom(float steps);      // > 0 closer

    // Ray through a point of the viewport (pixels, origin top-left).
    void ray(const QPointF &pixel, const QSizeF &viewport, QVector3D *origin, QVector3D *direction) const;
};

struct Pick
{
    bool hit = false;
    QVector3D position;          // scene coordinates
    float distance = 0;          // along the ray
};

// Nearest point of a cloud to a ray, within `tolerance` world units of the ray; points are
// visited every `stride`-th for speed on large clouds.
Pick pickPoint(const PointCloud &cloud, const QVector3D &origin, const QVector3D &direction, float tolerance, int stride = 1);
// First triangle hit by the ray (Möller–Trumbore).
Pick pickMesh(const Mesh &mesh, const QVector3D &origin, const QVector3D &direction);

} // namespace view3d
