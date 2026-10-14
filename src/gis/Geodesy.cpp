#include "Geodesy.h"
#include "Crs.h"
#include <geodesic.h>
#include <cmath>

Geodesy::Ellipsoid Geodesy::ellipsoidOf(const Crs &crs)
{
    Ellipsoid e;
    if (crs.isValid() && crs.semiMajor() > 0) {
        e.a = crs.semiMajor();
        e.f = crs.inverseFlattening() > 0 ? 1.0 / crs.inverseFlattening() : 0.0;   // 0: sphere
    }
    return e;
}

double Geodesy::distance(const QPointF &from, const QPointF &to, const Ellipsoid &e)
{
    geod_geodesic g;
    geod_init(&g, e.a, e.f);
    double s12 = 0, azi1 = 0, azi2 = 0;
    geod_inverse(&g, from.y(), from.x(), to.y(), to.x(), &s12, &azi1, &azi2);
    return s12;
}

double Geodesy::length(const QVector<QPointF> &line, const Ellipsoid &e)
{
    geod_geodesic g;
    geod_init(&g, e.a, e.f);
    double total = 0;
    for (int i = 1; i < line.size(); ++i) {
        double s12 = 0, azi1 = 0, azi2 = 0;
        geod_inverse(&g, line[i - 1].y(), line[i - 1].x(), line[i].y(), line[i].x(), &s12, &azi1, &azi2);
        total += s12;
    }
    return total;
}

Geodesy::Polygon Geodesy::polygon(const QVector<QPointF> &ring, const Ellipsoid &e)
{
    Polygon result;
    QVector<QPointF> points = ring;
    if (points.size() > 1 && points.first() == points.last()) points.removeLast();
    if (points.size() < 3) return result;
    geod_geodesic g;
    geod_init(&g, e.a, e.f);
    geod_polygon p;
    geod_polygon_init(&p, 0);
    for (const QPointF &pt : points) geod_polygon_addpoint(&g, &p, pt.y(), pt.x());
    double area = 0, perimeter = 0;
    geod_polygon_compute(&g, &p, 0, 1, &area, &perimeter);
    result.area = std::fabs(area);
    result.perimeter = perimeter;
    return result;
}
