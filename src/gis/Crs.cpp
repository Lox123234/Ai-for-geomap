#include "Crs.h"
#include "GisEnvironment.h"
#include <cpl_conv.h>
#include <ogr_spatialref.h>
#include <memory>

namespace {
void setError(QString *error, const QString &message)
{
    if (error) *error = message;
}
} // namespace

Crs Crs::fromSpatialReference(void *handle)
{
    auto *srs = static_cast<OGRSpatialReference *>(handle);
    srs->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    Crs crs;
    char *wkt = nullptr;
    const char *options[] = {"FORMAT=WKT2_2019", nullptr};
    if (srs->exportToWkt(&wkt, options) != OGRERR_NONE || !wkt) {
        CPLFree(wkt);
        return {};
    }
    crs.m_wkt = QString::fromUtf8(wkt);
    CPLFree(wkt);
    // The EPSG code of the CRS itself (also found for WKT without AUTHORITY when it matches).
    const char *authority = srs->GetAuthorityName(nullptr);
    const char *code = srs->GetAuthorityCode(nullptr);
    if (authority && code && QByteArray(authority).compare("EPSG", Qt::CaseInsensitive) == 0) {
        crs.m_epsg = QByteArray(code).toInt();
    } else {
        OGRSpatialReference copy(*srs);
        if (copy.AutoIdentifyEPSG() == OGRERR_NONE && copy.GetAuthorityCode(nullptr))
            crs.m_epsg = QByteArray(copy.GetAuthorityCode(nullptr)).toInt();
    }
    crs.m_name = QString::fromUtf8(srs->GetName() ? srs->GetName() : "");
    crs.m_geographic = srs->IsGeographic();
    crs.m_projected = srs->IsProjected();
    if (crs.m_projected) {
        const char *unit = nullptr;
        crs.m_toMeters = srs->GetLinearUnits(&unit);
        crs.m_unit = QString::fromUtf8(unit ? unit : "");
    } else if (crs.m_geographic) {
        const char *unit = nullptr;
        srs->GetAngularUnits(&unit);
        crs.m_unit = QString::fromUtf8(unit ? unit : "degree");
    }
    OGRErr err = OGRERR_NONE;
    crs.m_a = srs->GetSemiMajor(&err);
    crs.m_invF = srs->GetInvFlattening(&err);
    return crs;
}

Crs Crs::fromEpsg(int code, QString *error)
{
    GisEnvironment::clearError();
    OGRSpatialReference srs;
    if (code <= 0 || srs.importFromEPSG(code) != OGRERR_NONE) {
        setError(error, QStringLiteral("EPSG:%1 не найден в базе PROJ. %2").arg(code).arg(GisEnvironment::lastError()));
        return {};
    }
    return fromSpatialReference(&srs);
}

Crs Crs::fromWkt(const QString &wkt, QString *error)
{
    GisEnvironment::clearError();
    OGRSpatialReference srs;
    const QByteArray text = wkt.toUtf8();
    if (wkt.trimmed().isEmpty() || srs.importFromWkt(text.constData()) != OGRERR_NONE) {
        setError(error, QStringLiteral("Описание системы координат не распознано. %1").arg(GisEnvironment::lastError()));
        return {};
    }
    return fromSpatialReference(&srs);
}

Crs Crs::fromUserInput(const QString &text, QString *error)
{
    const QString t = text.trimmed();
    bool number = false;
    const int code = t.toInt(&number);
    if (number) return fromEpsg(code, error);
    GisEnvironment::clearError();
    OGRSpatialReference srs;
    // No network access and no reading of files named in the text (what GDAL calls
    // SET_FROM_USER_INPUT_LIMITATIONS; the constant itself is not exported from the DLL).
    const char *const limitations[] = {"ALLOW_NETWORK_ACCESS=NO", "ALLOW_FILE_ACCESS=NO", nullptr};
    if (t.isEmpty() || srs.SetFromUserInput(t.toUtf8().constData(), limitations) != OGRERR_NONE) {
        setError(error, QStringLiteral("Система координат «%1» не распознана. %2").arg(t, GisEnvironment::lastError()));
        return {};
    }
    return fromSpatialReference(&srs);
}

Crs Crs::wgs84()
{
    static const Crs crs = fromEpsg(4326);
    return crs;
}

QString Crs::label() const
{
    if (!isValid()) return QStringLiteral("CRS не задан");
    return m_epsg ? QStringLiteral("EPSG:%1 · %2").arg(m_epsg).arg(m_name) : m_name;
}

Crs Crs::geographic() const
{
    if (!isValid()) return {};
    if (m_geographic) return *this;
    OGRSpatialReference srs;
    if (srs.importFromWkt(m_wkt.toUtf8().constData()) != OGRERR_NONE) return {};
    std::unique_ptr<OGRSpatialReference> geo(srs.CloneGeogCS());
    return geo ? fromSpatialReference(geo.get()) : Crs();
}

bool Crs::isSame(const Crs &other) const
{
    if (!isValid() || !other.isValid()) return false;
    if (m_wkt == other.m_wkt) return true;
    OGRSpatialReference a, b;
    if (a.importFromWkt(m_wkt.toUtf8().constData()) != OGRERR_NONE
        || b.importFromWkt(other.m_wkt.toUtf8().constData()) != OGRERR_NONE)
        return false;
    const char *options[] = {"IGNORE_DATA_AXIS_TO_SRS_AXIS_MAPPING=YES", "CRITERION=EQUIVALENT_EXCEPT_AXIS_ORDER_GEOGCRS", nullptr};
    return a.IsSame(&b, options);
}

// ------------------------------------------------------------ transformation

CoordinateTransform::CoordinateTransform(const Crs &from, const Crs &to)
{
    if (!from.isValid() || !to.isValid()) {
        m_error = QStringLiteral("Система координат не задана.");
        return;
    }
    if (from.isSame(to)) {
        m_identity = true;
        return;
    }
    OGRSpatialReference a, b;
    a.importFromWkt(from.wkt().toUtf8().constData());
    b.importFromWkt(to.wkt().toUtf8().constData());
    a.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    b.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    GisEnvironment::clearError();
    m_ct = OGRCreateCoordinateTransformation(&a, &b);
    if (!m_ct)
        m_error = QStringLiteral("Нет пересчёта %1 → %2. %3").arg(from.label(), to.label(), GisEnvironment::lastError());
}

CoordinateTransform::~CoordinateTransform()
{
    OGRCoordinateTransformation::DestroyCT(m_ct);
}

bool CoordinateTransform::transform(double &x, double &y) const
{
    if (m_identity) return true;
    if (!m_ct) return false;
    return m_ct->Transform(1, &x, &y) != 0;
}

bool CoordinateTransform::transform(QPointF &p) const
{
    double x = p.x(), y = p.y();
    if (!transform(x, y)) return false;
    p = QPointF(x, y);
    return true;
}

bool CoordinateTransform::transform(QVector<QPointF> &points) const
{
    if (m_identity || points.isEmpty()) return m_identity || points.isEmpty();
    if (!m_ct) return false;
    QVector<double> xs(points.size()), ys(points.size());
    QVector<int> ok(points.size());
    for (int i = 0; i < points.size(); ++i) {
        xs[i] = points[i].x();
        ys[i] = points[i].y();
    }
    m_ct->Transform(int(points.size()), xs.data(), ys.data(), nullptr, ok.data());
    bool all = true;
    for (int i = 0; i < points.size(); ++i) {
        points[i] = QPointF(xs[i], ys[i]);
        all = all && ok[i];
    }
    return all;
}
