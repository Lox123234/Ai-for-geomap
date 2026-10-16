// Analysis tools of the map: cross sections, slope between two points, height of a point, angle,
// viewshed, the raster calculator (vegetation indices) and the export of analysis results.
#include "MapService.h"
#include "Geodesy.h"
#include "Overlap.h"
#include "PhotoModel.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QPainter>
#include <QSaveFile>
#include <QTextStream>
#include <QThreadPool>
#include <cmath>
#include <functional>
#include <gdal.h>

namespace {
constexpr double Pi = 3.14159265358979323846;
const QLocale &ru()
{
    static const QLocale locale(QLocale::Russian);
    return locale;
}
QString num(double v, int decimals) { return ru().toString(v, 'f', decimals); }
QString metres(double m) { return num(m, 2) + QStringLiteral(" м"); }
QString signedMetres(double m) { return (m > 0 ? QStringLiteral("+") : QString()) + metres(m); }
QString fileStem(const QString &name)
{
    QString s;
    for (const QChar c : name) s += (c.isLetterOrNumber() || c == QLatin1Char('-') || c == QLatin1Char('_')) ? c : QLatin1Char('_');
    return s.isEmpty() ? QStringLiteral("layer") : s;
}
QString freePath(const QString &dir, const QString &stem, const QString &suffix)
{
    QString path = QDir(dir).filePath(stem + suffix);
    for (int n = 2; QFileInfo::exists(path); ++n) path = QDir(dir).filePath(QStringLiteral("%1-%2%3").arg(stem).arg(n).arg(suffix));
    return path;
}
} // namespace

bool MapService::isTerrainTool() const
{
    static const QStringList tools = {QStringLiteral("profile"), QStringLiteral("volume"), QStringLiteral("crossProfile"), QStringLiteral("slope"),
                                      QStringLiteral("height"), QStringLiteral("angle"), QStringLiteral("viewshed")};
    return tools.contains(m_tool);
}

// ------------------------------------------------------------------ cross sections

void MapService::setCrossSpacing(double m)
{
    m = qBound(0.1, m, 100000.0);
    if (qFuzzyCompare(m, m_crossSpacing)) return;
    m_crossSpacing = m;
    emit crossChanged();
    if (m_tool == QLatin1String("crossProfile") && m_sketchDone) computeCrossSections(m_sketch);
}

void MapService::setCrossHalfWidth(double m)
{
    m = qBound(0.1, m, 100000.0);
    if (qFuzzyCompare(m, m_crossHalfWidth)) return;
    m_crossHalfWidth = m;
    emit crossChanged();
    if (m_tool == QLatin1String("crossProfile") && m_sketchDone) computeCrossSections(m_sketch);
}

void MapService::setCrossIndex(int index)
{
    if (m_cross.isEmpty()) return;
    index = qBound(0, index, int(m_cross.size()) - 1);
    if (index == m_crossIndex && m_profile.points.size() == m_cross[index].points.size()) return;
    m_crossIndex = index;
    m_profile = m_cross[index];
    m_profileMarker = -1;
    emit crossChanged();
    emit profileChanged();
    emit repaint();
}

void MapService::computeCrossSections(const QVector<QPointF> &line)
{
    const int i = indexOf(terrainLayerId());
    if (i < 0) {
        m_terrainText = QStringLiteral("Нет модели высот на карте");
        m_terrainDetails = QStringLiteral("Добавьте DSM или DTM и проведите ось снова.");
        return;
    }
    if (line.size() < 2) return;
    const QString source = m_layers[i].source, name = m_layers[i].name;
    const Crs crs = m_crs;
    const double spacing = m_crossSpacing, half = m_crossHalfWidth;
    const int generation = ++m_terrainGeneration;
    m_terrainText = QStringLiteral("Построение поперечных профилей по «%1»…").arg(name);
    m_terrainDetails.clear();
    QThreadPool::globalInstance()->start([this, source, name, line, crs, spacing, half, generation] {
        QVector<QVector<QPointF>> lines;
        const QVector<terrain::Profile> sections = terrain::crossSections(source, line, crs, spacing, half, 160, &lines);
        QMetaObject::invokeMethod(this, [this, sections, lines, name, spacing, half, generation] {
            if (generation != m_terrainGeneration) return;
            m_cross = sections;
            m_crossLines = lines;
            m_crossIndex = 0;
            m_profileLayer = name;
            m_terrainLayerName = name;
            m_profileMarker = -1;
            if (!sections.isEmpty() && sections.first().ok()) {
                m_profile = sections.first();
                double lo = 1e300, hi = -1e300;
                for (const terrain::Profile &p : sections)
                    if (p.ok() && p.noData < p.points.size()) { lo = qMin(lo, p.minZ); hi = qMax(hi, p.maxZ); }
                m_terrainText = QStringLiteral("Поперечные профили: %1 шт., через %2, ширина ±%3").arg(sections.size()).arg(metres(spacing), metres(half));
                m_terrainDetails = QStringLiteral("По «%1»: высоты %2 – %3. Сечения нумеруются от начала оси; смещение отрицательно слева.")
                                       .arg(name, metres(lo), metres(hi));
            } else {
                m_profile = {};
                m_terrainText = QStringLiteral("Поперечные профили не построены");
                m_terrainDetails = sections.isEmpty() ? QStringLiteral("Ось короче шага") : sections.first().error;
            }
            emit crossChanged();
            emit profileChanged();
            updateMeasurement();
            emit terrainResultReady();
            emit repaint();
        }, Qt::QueuedConnection);
    });
}

QString MapService::exportCrossSections(const QUrl &url)
{
    if (m_cross.isEmpty()) return QStringLiteral("Поперечные профили ещё не построены.");
    QSaveFile f(url.toLocalFile());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return QStringLiteral("Не удалось записать %1").arg(displayPath(url));
    QTextStream out(&f);
    out << "section,station_m,offset_m,elevation_m,x,y\n";
    for (int s = 0; s < m_cross.size(); ++s)
        for (const terrain::ProfilePoint &p : m_cross[s].points)
            out << s + 1 << ',' << QString::number(s * m_crossSpacing, 'f', 3) << ',' << QString::number(p.distance - m_crossHalfWidth, 'f', 3) << ','
                << (std::isfinite(p.z) ? QString::number(p.z, 'f', 3) : QString()) << ',' << QString::number(p.x, 'f', 3) << ','
                << QString::number(p.y, 'f', 3) << '\n';
    out.flush();
    if (!f.commit()) return QStringLiteral("Не удалось записать %1").arg(displayPath(url));
    return {};
}

// ------------------------------------------------------------------ point tools

void MapService::computeSlope(const QVector<QPointF> &two)
{
    const int i = indexOf(terrainLayerId());
    m_pointValues.clear();
    if (i < 0) {
        m_terrainText = QStringLiteral("Нет модели высот на карте");
        m_terrainDetails = QStringLiteral("Уклон измеряется по модели высот (DSM или DTM).");
        return;
    }
    const QString source = m_layers[i].source, name = m_layers[i].name;
    const Crs crs = m_crs;
    const int generation = ++m_terrainGeneration;
    m_terrainText = QStringLiteral("Измерение уклона по «%1»…").arg(name);
    QThreadPool::globalInstance()->start([this, source, name, two, crs, generation] {
        const terrain::Profile p = terrain::profile(source, two, crs, 200);
        QMetaObject::invokeMethod(this, [this, p, name, generation] {
            if (generation != m_terrainGeneration) return;
            m_terrainLayerName = name;
            double z0 = qQNaN(), z1 = qQNaN();
            if (p.ok() && !p.points.isEmpty()) {
                z0 = p.points.first().z;
                z1 = p.points.last().z;
            }
            if (!p.ok() || !std::isfinite(z0) || !std::isfinite(z1) || p.length <= 0) {
                m_terrainText = QStringLiteral("Уклон не измерен");
                m_terrainDetails = p.ok() ? QStringLiteral("Одна из точек вне модели высот или на участке без данных.") : p.error;
            } else {
                const double dz = z1 - z0, degrees = std::atan2(dz, p.length) * 180 / Pi, percent = dz / p.length * 100;
                m_terrainText = QStringLiteral("Уклон %1° (%2 %), перепад %3 на %4")
                                    .arg(num(degrees, 1), num(percent, 1), signedMetres(dz), formatLength(p.length));
                m_terrainDetails = QStringLiteral("По «%1»: высоты %2 → %3; наибольший уклон между соседними точками %4°.")
                                       .arg(name, metres(z0), metres(z1), num(p.maxSlope, 1));
                m_pointValues = {{"slope_deg", std::round(degrees * 100) / 100}, {"slope_percent", std::round(percent * 100) / 100},
                                 {"dz_m", std::round(dz * 1000) / 1000}, {"length_m", std::round(p.length * 1000) / 1000},
                                 {"z_start", std::round(z0 * 1000) / 1000}, {"z_end", std::round(z1 * 1000) / 1000}};
            }
            updateMeasurement();
            emit terrainResultReady();
        }, Qt::QueuedConnection);
    });
}

void MapService::computeHeight(const QPointF &point)
{
    m_pointValues.clear();
    QStringList parts;
    double dsm = qQNaN(), dtm = qQNaN();
    QString first;
    for (int i = int(m_layers.size()) - 1; i >= 0; --i) {
        const MapLayer &l = m_layers[i];
        if (!isElevation(l) || !l.raster) continue;
        const double z = l.raster->valueAt(point, 1);
        if (!std::isfinite(z)) continue;
        if (first.isEmpty()) first = l.name;
        parts << QStringLiteral("%1: %2").arg(l.name, metres(z));
        m_pointValues.insert(QStringLiteral("z_") + fileStem(l.name), std::round(z * 1000) / 1000);
        const QString lower = QFileInfo(l.source).fileName().toLower() + l.name.toLower();
        if (lower.contains(QLatin1String("dsm")) && !std::isfinite(dsm)) dsm = z;
        if (lower.contains(QLatin1String("dtm")) && !std::isfinite(dtm)) dtm = z;
    }
    m_terrainLayerName = first;
    if (parts.isEmpty()) {
        m_terrainText = QStringLiteral("Высоты нет");
        m_terrainDetails = QStringLiteral("Под точкой нет модели высот с данными.");
        return;
    }
    m_terrainText = QStringLiteral("Высота %1").arg(parts.join(QStringLiteral("; ")));
    if (std::isfinite(dsm) && std::isfinite(dtm)) {
        m_terrainText += QStringLiteral("; над землёй %1").arg(metres(dsm - dtm));
        m_pointValues.insert(QStringLiteral("height_above_ground_m"), std::round((dsm - dtm) * 1000) / 1000);
    }
    QPointF wgs = point;
    CoordinateTransform(m_crs, Crs::wgs84()).transform(wgs);
    m_terrainDetails = QStringLiteral("Точка %1, %2 (%3); %4°, %5° (WGS 84). Значения ячеек моделей под точкой.")
                           .arg(num(point.x(), 2), num(point.y(), 2), m_crs.label(), num(wgs.y(), 7), num(wgs.x(), 7));
}

void MapService::computeAngle(const QVector<QPointF> &p)
{
    m_pointValues.clear();
    if (p.size() < 3) return;
    // Azimuths of both arms from the vertex (grid north of the map CRS, clockwise).
    auto azimuth = [](const QPointF &from, const QPointF &to) {
        double a = std::atan2(to.x() - from.x(), to.y() - from.y()) * 180 / Pi;
        return a < 0 ? a + 360 : a;
    };
    const double a1 = azimuth(p[1], p[0]), a2 = azimuth(p[1], p[2]);
    double angle = std::fabs(a2 - a1);
    if (angle > 180) angle = 360 - angle;
    const Measure m1 = measure({p[1], p[0]}, false), m2 = measure({p[1], p[2]}, false);
    m_terrainText = QStringLiteral("Угол %1°").arg(num(angle, 2));
    m_terrainDetails = QStringLiteral("Стороны %1 и %2; азимуты сторон %3° и %4° (от северного направления сетки %5, по часовой).")
                           .arg(formatLength(m1.length), formatLength(m2.length), num(a1, 2), num(a2, 2), m_crs.label());
    m_pointValues = {{"angle_deg", std::round(angle * 1000) / 1000}, {"azimuth1_deg", std::round(a1 * 1000) / 1000},
                     {"azimuth2_deg", std::round(a2 * 1000) / 1000}, {"side1_m", std::round(m1.length * 1000) / 1000},
                     {"side2_m", std::round(m2.length * 1000) / 1000}};
}

// ------------------------------------------------------------------ viewshed

void MapService::setViewObserverHeight(double m)
{
    m = qBound(0.0, m, 10000.0);
    if (qFuzzyCompare(m + 1, m_viewObserver + 1)) return;
    m_viewObserver = m;
    emit viewshedSettingsChanged();
}

void MapService::setViewTargetHeight(double m)
{
    m = qBound(0.0, m, 10000.0);
    if (qFuzzyCompare(m + 1, m_viewTarget + 1)) return;
    m_viewTarget = m;
    emit viewshedSettingsChanged();
}

void MapService::setViewRadius(double m)
{
    m = qBound(1.0, m, 100000.0);
    if (qFuzzyCompare(m, m_viewRadius)) return;
    m_viewRadius = m;
    emit viewshedSettingsChanged();
}

void MapService::computeViewshed(const QPointF &observer)
{
    const int i = indexOf(terrainLayerId());
    if (i < 0) {
        m_terrainText = QStringLiteral("Нет модели высот на карте");
        m_terrainDetails = QStringLiteral("Видимость считается по модели поверхности (DSM) или рельефа (DTM).");
        return;
    }
    if (m_analysisBusy) {
        m_terrainText = QStringLiteral("Уже идёт расчёт: %1").arg(m_analysisStatus);
        return;
    }
    const QString source = m_layers[i].source, name = m_layers[i].name;
    const QString out = freePath(QDir(m_project).filePath(QStringLiteral("analysis")), fileStem(name) + QStringLiteral("-viewshed"), QStringLiteral(".tif"));
    const Crs crs = m_crs;
    const double h = m_viewObserver, t = m_viewTarget, r = m_viewRadius;
    const int generation = ++m_terrainGeneration;
    m_analysisBusy = true;
    m_analysisStatus = QStringLiteral("Анализ видимости по «%1»…").arg(name);
    m_terrainText = m_analysisStatus;
    m_terrainDetails.clear();
    emit analysisChanged();
    QThreadPool::globalInstance()->start([this, source, name, out, observer, crs, h, t, r, generation] {
        const terrain::Viewshed v = terrain::viewshed(source, observer, crs, h, t, r, out);
        QMetaObject::invokeMethod(this, [this, v, name, h, t, r, generation] {
            m_analysisBusy = false;
            m_analysisStatus.clear();
            emit analysisChanged();
            m_terrainLayerName = name;
            QString message;
            if (!v.ok()) {
                message = v.error;
                if (generation == m_terrainGeneration) {
                    m_terrainText = QStringLiteral("Видимость не рассчитана");
                    m_terrainDetails = v.error;
                }
            } else {
                const double share = v.area > 0 ? v.visibleArea / v.area * 100 : 0;
                message = QStringLiteral("Видно %1 % участка в радиусе %2: %3 из %4").arg(num(share, 1), formatLength(r), formatArea(v.visibleArea), formatArea(v.area));
                if (generation == m_terrainGeneration) {
                    m_terrainText = message;
                    m_terrainDetails = QStringLiteral("По «%1»: наблюдатель %2 над поверхностью (высота земли %3), цели %4 над поверхностью; кривизна Земли и рефракция учтены.")
                                           .arg(name, metres(h), metres(v.observerZ), metres(t));
                    m_pointValues = {{"visible_percent", std::round(share * 100) / 100}, {"visible_m2", std::round(v.visibleArea * 100) / 100},
                                     {"area_m2", std::round(v.area * 100) / 100}, {"radius_m", r}, {"observer_height_m", h}, {"target_height_m", t}};
                }
                RasterStyle style;
                style.mode = RasterStyle::SingleBand;
                style.ramp = RasterStyle::Visibility;
                style.min = 0;
                style.max = 1;
                m_nextStyle = style;
                m_useNextStyle = true;
                const QString added = addLayerFromPath(v.path, false);
                m_useNextStyle = false;
                const QString id = layerIdForSource(v.path);
                if (added.isEmpty() && !id.isEmpty()) setLayerOpacity(id, 0.6);
            }
            updateMeasurement();
            emit terrainResultReady();
            emit analysisFinished(v.ok(), message);
            emit repaint();
        }, Qt::QueuedConnection);
    });
}

// ------------------------------------------------------------------ raster calculator

QVariantList MapService::rasterLayers() const
{
    QVariantList list;
    for (int i = int(m_layers.size()) - 1; i >= 0; --i) {
        const MapLayer &l = m_layers[i];
        if (l.type != MapLayer::Raster || !l.rasterInfo.valid || l.needsCrs) continue;
        list << QVariantMap{{"id", l.id}, {"name", l.name}, {"bands", l.rasterInfo.bands}, {"rgb", l.rasterInfo.bands >= 3},
                            {"elevation", isElevation(l)}};
    }
    return list;
}

QString MapService::checkRasterExpression(const QString &layerId, const QString &expression) const
{
    const int i = indexOf(layerId);
    if (i < 0) return QStringLiteral("Выберите растр.");
    return terrain::checkExpression(expression, m_layers[i].rasterInfo.bands);
}

QString MapService::calculateRaster(const QString &layerId, const QString &index, const QString &expressionIn, int red, int green, int blue, int nir)
{
    if (m_project.isEmpty()) return QStringLiteral("Откройте проект.");
    if (m_analysisBusy) return QStringLiteral("Уже идёт расчёт: %1").arg(m_analysisStatus);
    const int i = indexOf(layerId);
    if (i < 0 || m_layers[i].type != MapLayer::Raster || !m_layers[i].rasterInfo.valid) return QStringLiteral("Выберите растровый слой.");
    const MapLayer &l = m_layers[i];
    const int bands = l.rasterInfo.bands;
    QString expression = expressionIn.trimmed(), title;
    RasterStyle style;
    style.mode = RasterStyle::SingleBand;
    style.band = 1;
    style.ramp = RasterStyle::Vegetation;
    bool fixedRange = true;
    if (index == QLatin1String("exg") || index == QLatin1String("vari") || index == QLatin1String("gli")) {
        if (bands < 3) return QStringLiteral("Для индекса по RGB нужен цветной растр (ортофотоплан).");
        expression = terrain::indexExpression(index, red, green, blue);
        title = index == QLatin1String("exg") ? QStringLiteral("Индекс зелени ExG") : index == QLatin1String("vari") ? QStringLiteral("Индекс VARI")
                                                                                                                 : QStringLiteral("Индекс GLI");
        style.min = index == QLatin1String("vari") ? -0.3 : -0.2;
        style.max = index == QLatin1String("vari") ? 0.6 : 0.5;
    } else if (index == QLatin1String("ndvi")) {
        if (bands < 2 || nir < 1 || nir > bands || red < 1 || red > bands || nir == red)
            return QStringLiteral("Для NDVI нужен растр с ближним инфракрасным каналом (мультиспектральная съёмка). Укажите каналы красный и NIR; "
                                  "у обычного RGB-ортофотоплана NIR нет — используйте индексы по RGB (ExG, VARI, GLI).");
        expression = terrain::indexExpression(index, red, green, blue, nir);
        title = QStringLiteral("NDVI");
        style.min = -0.2;
        style.max = 0.9;
    } else if (index == QLatin1String("calc")) {
        const QString problem = terrain::checkExpression(expression, bands);
        if (!problem.isEmpty()) return problem;
        title = QStringLiteral("Растровый калькулятор: %1").arg(expression);
        style.ramp = RasterStyle::Spectral;
        fixedRange = false;
    } else {
        return QStringLiteral("Неизвестный индекс: %1").arg(index);
    }
    const QString out = freePath(QDir(m_project).filePath(QStringLiteral("analysis")), fileStem(l.name) + QLatin1Char('-') + index, QStringLiteral(".tif"));
    const double threshold = terrain::indexThreshold(index);
    m_analysisBusy = true;
    m_analysisStatus = QStringLiteral("%1 по «%2»…").arg(title, l.name);
    emit analysisChanged();
    const QString source = l.source, name = l.name;
    QThreadPool::globalInstance()->start([this, source, name, out, expression, title, style, fixedRange, threshold, index] {
        double above = qQNaN();
        const terrain::RasterResult r = terrain::calculate(source, expression, out, threshold, &above);
        QMetaObject::invokeMethod(this, [this, r, name, title, style, fixedRange, above, threshold, expression, index] {
            m_analysisBusy = false;
            m_analysisStatus.clear();
            emit analysisChanged();
            if (!r.ok()) {
                emit analysisFinished(false, r.error);
                return;
            }
            RasterStyle s = style;
            if (!fixedRange && r.stats.valid) {
                s.min = r.stats.min;
                s.max = r.stats.max > r.stats.min ? r.stats.max : r.stats.min + 1;
            }
            m_nextStyle = s;
            m_useNextStyle = true;
            const QString added = addLayerFromPath(r.path, false);
            m_useNextStyle = false;
            if (!added.isEmpty()) {
                emit analysisFinished(false, added);
                return;
            }
            QString message = QStringLiteral("%1 по «%2»").arg(title, name);
            if (r.stats.valid) message += QStringLiteral(": от %1 до %2, в среднем %3").arg(num(r.stats.min, 3), num(r.stats.max, 3), num(r.stats.mean, 3));
            if (std::isfinite(above))
                message += QStringLiteral("; растительность (значение > %1) — %2 % площади").arg(num(threshold, 2), num(above * 100, 1));
            if (index != QLatin1String("calc")) message += QStringLiteral(". Формула: %1").arg(expression);
            emit analysisFinished(true, message + QStringLiteral(". Файл: %1").arg(QDir::toNativeSeparators(r.path)));
        }, Qt::QueuedConnection);
    });
    return {};
}

// ------------------------------------------------------------------ image overlap

QString MapService::computeOverlap()
{
    if (m_project.isEmpty()) return QStringLiteral("Откройте проект.");
    if (m_analysisBusy) return QStringLiteral("Уже идёт расчёт: %1").arg(m_analysisStatus);
    // The newest job with a reconstruction; without one, the photos' own metadata.
    QString job;
    const QDir jobs(QDir(m_project).filePath(QStringLiteral("jobs")));
    for (const QString &id : jobs.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed))
        if (QFileInfo::exists(jobs.filePath(id + QStringLiteral("/opensfm/reconstruction.json")))
            && QFileInfo::exists(jobs.filePath(id + QStringLiteral("/odm_georeferencing/coords.txt")))) {
            job = jobs.filePath(id);
            break;
        }
    QStringList photos;
    if (m_photos)
        for (const Photo &p : m_photos->photos()) photos << QDir(m_project).filePath(p.file);
    if (job.isEmpty() && photos.isEmpty()) return QStringLiteral("В проекте нет снимков.");
    const QString out = freePath(QDir(m_project).filePath(QStringLiteral("analysis")),
                                 job.isEmpty() ? QStringLiteral("overlap-exif") : QStringLiteral("overlap-") + QFileInfo(job).fileName(), QStringLiteral(".tif"));
    const QString photoFolder = QDir(m_project).filePath(QStringLiteral("photos"));
    m_analysisBusy = true;
    m_analysisStatus = job.isEmpty() ? QStringLiteral("Оценка перекрытия по данным снимков…") : QStringLiteral("Перекрытие снимков по реконструкции…");
    emit analysisChanged();
    QThreadPool::globalInstance()->start([this, job, photos, out, photoFolder] {
        const overlap::Result r = job.isEmpty() ? overlap::fromPhotos(photos, out) : overlap::fromJob(job, out, photoFolder);
        QMetaObject::invokeMethod(this, [this, r] {
            m_analysisBusy = false;
            m_analysisStatus.clear();
            emit analysisChanged();
            if (!r.ok()) {
                emit analysisFinished(false, r.error);
                return;
            }
            RasterStyle style;
            style.mode = RasterStyle::SingleBand;
            style.ramp = RasterStyle::Overlap;
            style.min = 0;
            style.max = 10;
            m_nextStyle = style;
            m_useNextStyle = true;
            const QString added = addLayerFromPath(r.path, false);
            m_useNextStyle = false;
            if (!added.isEmpty()) {
                emit analysisFinished(false, added);
                return;
            }
            const QString id = layerIdForSource(r.path);
            if (!id.isEmpty()) {
                setLayerOpacity(id, 0.7);
                zoomToLayer(id);
            }
            m_overlap = QVariantMap{{"method", r.method}, {"job", r.job}, {"photos", r.photos}, {"skipped", r.skipped},
                                    {"area", r.area}, {"median", r.median}, {"share2", r.share2}, {"share3", r.share3},
                                    {"share5", r.share5}, {"share9", r.share9}, {"cell", r.cell}, {"note", r.note}, {"path", r.path}};
            emit overlapChanged();
            QString verdict = r.share3 < 0.9 ? QStringLiteral(" Участков, видимых меньше чем с 3 снимков, много: там возможны дыры и искажения.")
                                             : r.median < 5 ? QStringLiteral(" Перекрытие небольшое: для деревьев и зданий лучше 70–80 % (медиана ≥ 9 снимков).")
                                                            : QString();
            emit analysisFinished(true, QStringLiteral("Перекрытие (%1): медиана %2 снимков на точку; ≥ 3 снимков — %3 %, ≥ 5 — %4 %, ≥ 9 — %5 % площади %6.%7 %8")
                                            .arg(r.method == QLatin1String("job") ? QStringLiteral("по реконструкции задания ") + r.job
                                                                                   : QStringLiteral("оценка по EXIF"))
                                            .arg(num(r.median, 0), num(r.share3 * 100, 1), num(r.share5 * 100, 1), num(r.share9 * 100, 1), formatArea(r.area))
                                            .arg(verdict, r.note));
            emit repaint();
        }, Qt::QueuedConnection);
    });
    return {};
}

// ------------------------------------------------------------------ export of analysis results

QString MapService::exportAnalysis(const QUrl &folderUrl)
{
    if (m_project.isEmpty()) return QStringLiteral("Откройте проект.");
    const QString base = folderUrl.toLocalFile();
    if (base.isEmpty() || !QFileInfo(base).isDir()) return QStringLiteral("Выберите существующую папку.");
    const QString folder = freePath(base, QStringLiteral("%1 — анализ %2").arg(QFileInfo(m_project).fileName(),
                                                                                QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH-mm"))),
                                    QString());
    if (!QDir().mkpath(folder)) return QStringLiteral("Не удалось создать папку %1").arg(QDir::toNativeSeparators(folder));
    QStringList written, failed;
    const QDir analysis(QDir(m_project).filePath(QStringLiteral("analysis")));
    for (const QFileInfo &f : analysis.entryInfoList(QDir::Files, QDir::Name)) {
        if (f.fileName().endsWith(QLatin1String(".partial.tif"))) continue;
        const QString target = QDir(folder).filePath(f.fileName());
        (QFile::copy(f.absoluteFilePath(), target) ? written : failed) << f.fileName();
    }
    auto save = [&](const QString &name, const std::function<QString(const QUrl &)> &writer) {
        const QString error = writer(QUrl::fromLocalFile(QDir(folder).filePath(name)));
        if (error.isEmpty()) written << name;
    };
    if (!m_measurements.isEmpty()) save(QStringLiteral("измерения.csv"), [this](const QUrl &u) { return exportMeasurements(u); });
    if (drawingCount() > 0) save(QStringLiteral("чертежи.geojson"), [this](const QUrl &u) { return exportDrawings(u); });
    if (m_profile.ok() && !m_profile.points.isEmpty() && m_cross.isEmpty()) save(QStringLiteral("профиль.csv"), [this](const QUrl &u) { return exportProfile(u); });
    if (!m_cross.isEmpty()) save(QStringLiteral("поперечные-профили.csv"), [this](const QUrl &u) { return exportCrossSections(u); });
    if (written.isEmpty()) {
        QDir(folder).removeRecursively();
        return QStringLiteral("Результатов анализа пока нет: постройте карты уклонов, горизонтали, индексы или сохраните измерения.");
    }
    QString message = QStringLiteral("Результаты анализа (%1 файлов) сохранены в %2").arg(written.size()).arg(QDir::toNativeSeparators(folder));
    if (!failed.isEmpty()) message += QStringLiteral(". Не скопированы: %1").arg(failed.join(QStringLiteral(", ")));
    emit analysisFinished(failed.isEmpty(), message);
    return {};
}

// ------------------------------------------------------------------ drawing

void MapService::paintAnalysis(QPainter *painter) const
{
    if (m_tool == QLatin1String("crossProfile") && !m_crossLines.isEmpty()) {
        for (int i = 0; i < m_crossLines.size(); ++i) {
            const QVector<QPointF> &l = m_crossLines[i];
            QPen pen(i == m_crossIndex ? QColor(80, 230, 255) : QColor(255, 214, 102, 170), i == m_crossIndex ? 3 : 1.5);
            pen.setCosmetic(true);
            painter->setPen(pen);
            painter->drawLine(toScreen(l.first()), toScreen(l.last()));
            if (i == m_crossIndex || m_crossLines.size() <= 30 || i % 5 == 0) {
                painter->setPen(QColor(235, 240, 245));
                painter->drawText(toScreen(l.first()) + QPointF(4, -4), QString::number(i + 1));
            }
        }
    }
    if (m_tool == QLatin1String("viewshed") && !m_sketch.isEmpty()) {
        const QPointF s = toScreen(m_sketch.first());
        const double radius = m_viewRadius / qMax(1e-9, m_resolution * (m_crs.isProjected() ? m_crs.metersPerUnit() : 111320.0));
        QPen pen(QColor(80, 230, 255), 1.5, Qt::DashLine);
        pen.setCosmetic(true);
        painter->setPen(pen);
        painter->setBrush(Qt::NoBrush);
        painter->drawEllipse(s, radius, radius);
        painter->setPen(QPen(QColor(20, 24, 30), 2));
        painter->setBrush(QColor(80, 230, 255));
        painter->drawEllipse(s, 7, 7);
    }
}
