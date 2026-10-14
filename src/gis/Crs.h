#pragma once

#include <QPointF>
#include <QString>
#include <QVector>

class OGRCoordinateTransformation;

// A coordinate reference system as a value: WKT plus the properties the UI needs. Built with
// GDAL/PROJ (EPSG database); safe to copy between threads (no OGR object is kept).
// Axis order is always "traditional GIS": x = easting/longitude, y = northing/latitude.
class Crs
{
public:
    Crs() = default;
    static Crs fromEpsg(int code, QString *error = nullptr);
    static Crs fromWkt(const QString &wkt, QString *error = nullptr);
    // "EPSG:32615", "32615", WKT or a PROJ string.
    static Crs fromUserInput(const QString &text, QString *error = nullptr);
    static Crs wgs84();

    bool isValid() const { return !m_wkt.isEmpty(); }
    QString wkt() const { return m_wkt; }
    int epsg() const { return m_epsg; }              // 0 without an EPSG code
    QString name() const { return m_name; }          // "WGS 84 / UTM zone 15N"
    QString label() const;                           // "EPSG:32615 · WGS 84 / UTM zone 15N"
    bool isGeographic() const { return m_geographic; }
    bool isProjected() const { return m_projected; }
    QString unitName() const { return m_unit; }      // "metre", "degree", "US survey foot"
    double metersPerUnit() const { return m_toMeters; }   // projected CRS only; 0 otherwise
    double semiMajor() const { return m_a; }         // ellipsoid of the datum
    double inverseFlattening() const { return m_invF; }
    Crs geographic() const;                          // geographic CRS of the same datum
    bool isSame(const Crs &other) const;

private:
    static Crs fromSpatialReference(void *srs);       // OGRSpatialReference*
    QString m_wkt;
    int m_epsg = 0;
    QString m_name;
    bool m_geographic = false;
    bool m_projected = false;
    QString m_unit;
    double m_toMeters = 0;
    double m_a = 0;
    double m_invF = 0;
};

// Point transformation between two CRSs. One instance per thread.
class CoordinateTransform
{
public:
    CoordinateTransform(const Crs &from, const Crs &to);
    ~CoordinateTransform();
    CoordinateTransform(const CoordinateTransform &) = delete;
    CoordinateTransform &operator=(const CoordinateTransform &) = delete;

    bool isValid() const { return m_identity || m_ct; }
    QString error() const { return m_error; }
    bool transform(double &x, double &y) const;
    bool transform(QPointF &p) const;
    bool transform(QVector<QPointF> &points) const;   // false if any point failed

private:
    OGRCoordinateTransformation *m_ct = nullptr;
    bool m_identity = false;
    QString m_error;
};
