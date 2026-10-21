#include "GeorefService.h"
#include "GisEnvironment.h"
#include "ProcessingService.h"
#include "ProjectManager.h"
#include "SurveyLayout.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QSaveFile>
#include <QThreadPool>
#include <cmath>
#include <complex>
#include <gdal.h>
#include <limits>

namespace {
constexpr int MaxSide = 6144;     // longest side of the image kept for the panes
constexpr int SmallSide = 1024;   // the copy drawn when the image is small on the screen

QByteArray native(const QString &path) { return QDir::toNativeSeparators(path).toUtf8(); }

// "1 связь", "2 связи", "5 связей".
QString linkWord(int n)
{
    const int m10 = n % 10, m100 = n % 100;
    const char *w = m10 == 1 && m100 != 11 ? "связь" : m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14) ? "связи" : "связей";
    return QStringLiteral("%1 %2").arg(n).arg(QString::fromUtf8(w));
}

QPolygonF cornersOf(const QSize &s, const georef::Transform &t)
{
    QPolygonF q;
    for (const QPointF &p : {QPointF(0, 0), QPointF(s.width(), 0), QPointF(s.width(), s.height()), QPointF(0, s.height())}) q << t.map(p);
    return q;
}
} // namespace

struct GeorefService::Loaded
{
    QString error;
    QSize full;
    QImage image, small;
    double scale = 1;
    QPolygonF corners;   // where the image lies: its own georeferencing or the photo's footprint
    QString cornersWkt;
    QString placement;
    std::optional<QPointF> centre;   // else where the photo was taken (its GPS), in cornersWkt
};

GeorefService::GeorefService(MapService *map, ProjectManager *projects, ProcessingService *processing, QObject *parent)
    : QObject(parent), m_map(map), m_projects(projects), m_processing(processing)
{
    if (m_map) {
        connect(m_map, &MapService::georefClicked, this, &GeorefService::mapClicked);
        connect(m_map, &MapService::georefCancelled, this, &GeorefService::cancelPending);
        connect(m_map, &MapService::crsChanged, this, &GeorefService::mapCrsChanged);
        connect(m_map, &MapService::projectChanged, this, &GeorefService::close);
    }
}

GeorefService::~GeorefService()
{
    if (m_cancel) m_cancel->store(true);
    if (m_map) m_map->setOverlay(nullptr);
}

QString GeorefService::imageName() const { return m_name; }

QString GeorefService::units() const
{
    if (!m_crs.isValid()) return QStringLiteral("м");
    if (m_crs.isGeographic()) return QStringLiteral("°");
    const QString u = m_crs.unitName().toLower();
    return u.contains(QLatin1String("foot")) ? QStringLiteral("фт") : QStringLiteral("м");
}

void GeorefService::setMethod(int method)
{
    if (method < 1 || method > 4 || method == int(m_method)) return;
    m_method = georef::Method(method);
    refit();
    saveSession();
}

QVariantList GeorefService::methods() const
{
    QVariantList list;
    for (georef::Method m : {georef::Method::Affine, georef::Method::Projective, georef::Method::Polynomial2, georef::Method::Polynomial3})
        list << QVariantMap{{QStringLiteral("value"), int(m)}, {QStringLiteral("title"), georef::title(m)},
                            {QStringLiteral("minimum"), georef::minimumLinks(m)}};
    return list;
}

QVariantList GeorefService::links() const
{
    QVariantList list;
    const bool f = fitted();
    // The worst link is marked when it stands out: more links than the method needs, its residual
    // well above the RMS error, and not a rounding error (links agreeing exactly).
    int worst = -1;
    if (f && m_fit.used > georef::minimumLinks(m_method)) {
        for (int i = 0; i < m_links.size(); ++i)
            if (m_links[i].enabled && (worst < 0 || m_fit.residual.value(i) > m_fit.residual.value(worst))) worst = i;
        const double tiny = m_crs.isGeographic() ? 1e-8 : 1e-4;
        if (worst >= 0 && (m_fit.residual.value(worst) < 1.5 * m_fit.rmse || m_fit.residual.value(worst) < tiny)) worst = -1;
    }
    for (int i = 0; i < m_links.size(); ++i) {
        const georef::Link &l = m_links[i];
        const double r = f ? m_fit.residual.value(i, qQNaN()) : qQNaN(), ri = f ? m_fit.inverseResidual.value(i, qQNaN()) : qQNaN();
        list << QVariantMap{{QStringLiteral("index"), i},
                            {QStringLiteral("ix"), l.image.x()},
                            {QStringLiteral("iy"), l.image.y()},
                            {QStringLiteral("mx"), l.map.x()},
                            {QStringLiteral("my"), l.map.y()},
                            {QStringLiteral("enabled"), l.enabled},
                            {QStringLiteral("residual"), std::isfinite(r) ? r : -1.0},
                            {QStringLiteral("inverse"), std::isfinite(ri) ? ri : -1.0},
                            {QStringLiteral("worst"), i == worst}};
    }
    return list;
}

int GeorefService::enabledCount() const
{
    int n = 0;
    for (const georef::Link &l : m_links) n += l.enabled ? 1 : 0;
    return n;
}

QString GeorefService::formatNumber(double value, int decimals) const
{
    return std::isfinite(value) ? QLocale().toString(value, 'f', decimals) : QStringLiteral("—");
}

QString GeorefService::fitText() const
{
    const int need = georef::minimumLinks(m_method), n = enabledCount();
    if (!active()) return {};
    if (n < need)
        return QStringLiteral("Нужно ещё %1").arg(linkWord(need - n));
    if (!m_fit.ok()) return m_fit.error;
    const int d = m_crs.isGeographic() ? 7 : 3;
    QString t = QStringLiteral("СКО %1 %2 · %3 пикс.")
                    .arg(formatNumber(m_fit.rmse, d), units(), formatNumber(m_fit.inverseRmse, 2));
    if (n == need)
        t += QStringLiteral("\nДобавьте ещё связь, чтобы проверить точность");
    return t;
}

void GeorefService::setAutoAdjust(bool on)
{
    if (on == m_autoAdjust) return;
    m_autoAdjust = on;
    emit optionsChanged();
    if (on) refit();
}

void GeorefService::setShowImage(bool on)
{
    if (on == m_showImage) return;
    m_showImage = on;
    emit optionsChanged();
    if (m_map) m_map->requestRepaint();
}

void GeorefService::setOpacity(double opacity)
{
    opacity = qBound(0.05, opacity, 1.0);
    if (qFuzzyCompare(opacity, m_opacity)) return;
    m_opacity = opacity;
    emit optionsChanged();
    if (m_map) m_map->requestRepaint();
}

void GeorefService::setSelectedLink(int index)
{
    if (index < -1 || index >= m_links.size()) index = -1;
    if (index == m_selected) return;
    m_selected = index;
    emit selectionChanged();
    emit repaint();
    if (m_map) m_map->requestRepaint();
}

void GeorefService::setStatus(const QString &text)
{
    if (text == m_status) return;
    m_status = text;
    emit statusChanged();
}

// --- Opening.

// The image as RGB(A) for the panes, reduced to MaxSide; data that is not 8-bit is stretched over
// mean ± 2.5 σ of each band; nodata and an alpha band are transparent.
static void readImage(const QString &path, const std::shared_ptr<GeorefService::Loaded> &l)
{
    GisEnvironment::clearError();
    GDALDatasetH ds = GDALOpenEx(native(path).constData(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
    if (!ds) {
        l->error = QStringLiteral("Файл не открывается как изображение: %1").arg(GisEnvironment::lastError());
        return;
    }
    const int w = GDALGetRasterXSize(ds), h = GDALGetRasterYSize(ds), n = GDALGetRasterCount(ds);
    if (w <= 0 || h <= 0 || n <= 0) {
        GDALClose(ds);
        l->error = QStringLiteral("В файле нет изображения");
        return;
    }
    l->full = QSize(w, h);
    l->scale = qMin(1.0, double(MaxSide) / qMax(w, h));
    const int bw = qMax(1, int(std::lround(w * l->scale))), bh = qMax(1, int(std::lround(h * l->scale)));
    // Bands: red, green, blue (or one grey) and alpha.
    int rgb[3] = {1, 1, 1}, alpha = 0;
    for (int b = 1; b <= n; ++b)
        if (GDALGetRasterColorInterpretation(GDALGetRasterBand(ds, b)) == GCI_AlphaBand) alpha = b;
    if (n >= 3) {
        rgb[0] = 1;
        rgb[1] = 2;
        rgb[2] = 3;
        if (n == 4 && !alpha) alpha = 4;
    }
    const size_t count = size_t(bw) * bh;
    std::vector<float> v[4];
    std::vector<unsigned char> mask(count, 255);
    GDALRasterIOExtraArg arg;
    INIT_RASTERIO_EXTRA_ARG(arg);
    arg.eResampleAlg = l->scale < 1 ? GRIORA_Average : GRIORA_NearestNeighbour;
    for (int c = 0; c < (alpha ? 4 : 3); ++c) {
        const int b = c < 3 ? rgb[c] : alpha;
        GDALRasterBandH band = GDALGetRasterBand(ds, b);
        v[c].resize(count);
        if (GDALRasterIOEx(band, GF_Read, 0, 0, w, h, v[c].data(), bw, bh, GDT_Float32, 0, 0, &arg) != CE_None) {
            GDALClose(ds);
            l->error = QStringLiteral("Не удалось прочитать изображение: %1").arg(GisEnvironment::lastError());
            return;
        }
        if (c == 3) continue;
        int hasNoData = 0;
        const double noData = GDALGetRasterNoDataValue(band, &hasNoData);
        if (hasNoData)
            for (size_t i = 0; i < count; ++i)
                if (v[c][i] == float(noData)) mask[i] = 0;
        if (GDALGetRasterDataType(band) != GDT_Byte) {
            double mean = 0, sq = 0;
            size_t m = 0;
            for (size_t i = 0; i < count; i += 7)
                if (mask[i] && std::isfinite(v[c][i])) {
                    mean += v[c][i];
                    sq += double(v[c][i]) * v[c][i];
                    ++m;
                }
            mean = m ? mean / m : 0;
            const double sd = m ? std::sqrt(qMax(0.0, sq / m - mean * mean)) : 1;
            const double lo = mean - 2.5 * sd, span = qMax(1e-9, 5 * sd);
            for (float &x : v[c]) x = float(qBound(0.0, (x - lo) / span * 255, 255.0));
        }
    }
    QImage img(bw, bh, QImage::Format_ARGB32);
    for (int y = 0; y < bh; ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < bw; ++x) {
            const size_t i = size_t(y) * bw + x;
            int a = mask[i];
            if (alpha && a) a = qBound(0, int(v[3][i]), 255);
            auto c = [&](int k) { return qBound(0, int(std::lround(v[k][i])), 255); };
            line[x] = qRgba(c(0), c(1), c(2), a);
        }
    }
    l->image = img;
    l->small = img.scaled(QSize(SmallSide, SmallSide).boundedTo(img.size()), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    // Its own georeferencing (an orthophoto that lies off, a map already roughly placed).
    double gt[6];
    const char *wkt = GDALGetProjectionRef(ds);
    if (GDALGetGeoTransform(ds, gt) == CE_None && wkt && *wkt && !(gt[0] == 0 && gt[1] == 1 && gt[3] == 0 && gt[5] == 1)) {
        for (const QPointF &p : {QPointF(0, 0), QPointF(w, 0), QPointF(w, h), QPointF(0, h)})
            l->corners << QPointF(gt[0] + p.x() * gt[1] + p.y() * gt[2], gt[3] + p.x() * gt[4] + p.y() * gt[5]);
        l->cornersWkt = QString::fromUtf8(wkt);
        l->placement = QStringLiteral("по привязке файла");
    }
    GDALClose(ds);
}

QString GeorefService::openFile(const QUrl &url)
{
    return openPath(url.isLocalFile() ? url.toLocalFile() : url.toString());
}

QString GeorefService::openPhoto(int row)
{
    if (!m_projects || !m_projects->hasProject()) return QStringLiteral("Проект не открыт");
    const QVector<Photo> &photos = m_projects->photoModel()->photos();
    if (row < 0 || row >= photos.size()) return QStringLiteral("Снимок не выбран");
    return openPath(QDir(m_projects->projectPath()).filePath(photos[row].file), row);
}

QString GeorefService::openPath(const QString &path, int photoRow)
{
    if (!m_map || !m_map->hasProject()) return QStringLiteral("Проект не открыт");
    if (!m_map->crsDefined())
        return QStringLiteral("У карты нет системы координат: добавьте подложку или геопривязанный слой — по ним ставятся точки «куда»");
    if (!QFileInfo::exists(path)) return QStringLiteral("Файл не найден: %1").arg(QDir::toNativeSeparators(path));
    if (m_busy) return QStringLiteral("Идёт трансформирование — дождитесь его или отмените");
    close();
    m_path = QFileInfo(path).absoluteFilePath();
    m_photoRow = photoRow;
    m_name = QFileInfo(path).fileName();
    if (photoRow >= 0 && m_projects) m_name = m_projects->photoModel()->photos().value(photoRow).name;
    m_crs = m_map->crs();
    m_loading = true;
    const int generation = ++m_generation;
    setStatus(QStringLiteral("Открытие…"));
    emit imageChanged();
    // A photo of the project: its footprint (from the newest aligned job, else from GPS and EXIF).
    QVector<surveylayout::PhotoIn> photo;
    QString job;
    if (photoRow >= 0 && m_projects) {
        const Photo &p = m_projects->photoModel()->photos()[photoRow];
        surveylayout::PhotoIn in;
        in.row = photoRow;
        in.name = p.name;
        in.path = m_path;
        in.takenAt = p.takenAt;
        in.hasGps = p.hasGps;
        in.latitude = p.latitude;
        in.longitude = p.longitude;
        photo << in;
        if (m_processing) {
            QString newest;
            for (const ProcessingJob &j : m_processing->jobs())
                if (j.state == JobState::Completed && m_processing->jobInChunk(j) && j.id > newest
                    && QFileInfo::exists(QDir(m_processing->jobDirectory(j.id)).filePath(QStringLiteral("opensfm/reconstruction.json"))))
                    newest = j.id;
            if (!newest.isEmpty()) job = m_processing->jobDirectory(newest);
        }
    }
    const QString file = m_path;
    QPointer<GeorefService> self(this);
    QThreadPool::globalInstance()->start([self, file, photo, job, generation] {
        auto l = std::make_shared<Loaded>();
        readImage(file, l);
        if (l->error.isEmpty() && !photo.isEmpty()) {
            const surveylayout::Layout layout = surveylayout::compute(photo, job, 0);
            if (layout.ok() && !layout.cameras.isEmpty() && layout.cameras.first().footprint.size() == 4) {
                l->corners = layout.cameras.first().footprint;
                l->cornersWkt = layout.crsWkt;
                l->placement = layout.cameras.first().footprintAligned ? QStringLiteral("по выравниванию")
                                                                       : QStringLiteral("по GPS и EXIF (примерно)");
            } else if (layout.ok() && !layout.cameras.isEmpty() && layout.cameras.first().hasGps) {
                l->centre = QPointF(layout.cameras.first().gpsX, layout.cameras.first().gpsY);
                l->cornersWkt = layout.crsWkt;
            }
        }
        QMetaObject::invokeMethod(qApp, [self, l, generation] {
            if (self) self->loaded(l, generation);
        }, Qt::QueuedConnection);
    });
    return {};
}

void GeorefService::loaded(const std::shared_ptr<Loaded> &l, int generation)
{
    if (generation != m_generation || !active()) return;
    m_loading = false;
    if (!l->error.isEmpty()) {
        const QString error = l->error;
        close();
        setStatus(error);
        emit finished(false, error, {});
        return;
    }
    m_full = l->full;
    m_image = l->image;
    m_small = l->small;
    m_scale = l->scale;
    m_placement.clear();
    bool zoom = false;
    if (l->corners.size() == 4) {
        QVector<QPointF> c(l->corners.begin(), l->corners.end());
        const CoordinateTransform ct(Crs::fromWkt(l->cornersWkt), m_crs);
        if (ct.isValid() && ct.transform(c)) {
            m_initial = georef::Transform::fromCorners(QSizeF(m_full), QPolygonF(c));
            if (m_initial.isValid()) {
                m_placement = l->placement;
                zoom = true;
            }
        }
    }
    QPointF gps;
    bool atGps = false;
    if (!m_initial.isValid() && l->centre) {   // where the photo was taken; its size on the ground unknown
        gps = *l->centre;
        const CoordinateTransform ct(Crs::fromWkt(l->cornersWkt), m_crs);
        atGps = ct.isValid() && ct.transform(gps);
        if (atGps) m_map->setView(gps, m_map->resolution());
    }
    if (!m_initial.isValid()) {
        // Fit to the view: in its middle, over 60 % of it.
        const QSizeF v = m_map->viewSize();
        const double s = qMin(0.6 * v.width() / m_full.width(), 0.6 * v.height() / m_full.height()) * m_map->resolution();
        const QPointF c = atGps ? gps : m_map->center();
        const double hw = m_full.width() * s / 2, hh = m_full.height() * s / 2;
        m_initial = georef::Transform::fromCorners(QSizeF(m_full), QPolygonF({c + QPointF(-hw, hh), c + QPointF(hw, hh), c + QPointF(hw, -hh), c + QPointF(-hw, -hh)}));
        m_placement = atGps ? QStringLiteral("по GPS (размер примерный)")
                            : QStringLiteral("в центре карты");
    }
    m_display = m_initial;
    restoreSession();
    refit();
    m_map->setOverlay(this);
    m_map->setTool(QStringLiteral("georef"));
    if (zoom || !m_links.isEmpty()) zoomToImage();
    setStatus(QStringLiteral("Щёлкните точку на снимке"));
    emit imageChanged();
    emit repaint();
}

void GeorefService::close()
{
    ++m_generation;
    if (!active() && !m_loading) return;
    if (m_cancel) m_cancel->store(true);
    m_path.clear();
    m_name.clear();
    m_placement.clear();
    m_photoRow = -1;
    m_loading = false;
    m_full = {};
    m_image = m_small = QImage();
    m_links.clear();
    m_fit = {};
    m_initial = m_display = {};
    m_pending.reset();
    m_selected = -1;
    if (m_map) {
        m_map->setOverlay(nullptr);
        if (m_map->tool() == QLatin1String("georef")) m_map->setTool(QStringLiteral("pan"));
    }
    setStatus({});
    emit imageChanged();
    emit linksChanged();
    emit pendingChanged();
    emit selectionChanged();
    emit repaint();
}

// --- Links.

bool GeorefService::inImage(const QPointF &p) const
{
    return p.x() >= 0 && p.y() >= 0 && p.x() <= m_full.width() && p.y() <= m_full.height();
}

std::optional<QPointF> GeorefService::imagePointAt(const QPointF &target) const
{
    if (!m_display.isValid() || m_full.isEmpty()) return std::nullopt;
    // Closest node of a coarse grid, then Newton's method on the display transformation.
    QPointF p;
    double best = std::numeric_limits<double>::max();
    for (int j = 0; j <= 8; ++j)
        for (int i = 0; i <= 8; ++i) {
            const QPointF q(m_full.width() * i / 8.0, m_full.height() * j / 8.0), m = m_display.map(q);
            const double d = std::hypot(m.x() - target.x(), m.y() - target.y());
            if (d < best) {
                best = d;
                p = q;
            }
        }
    for (int it = 0; it < 30; ++it) {
        const QPointF m = m_display.map(p), mx = m_display.map(p + QPointF(1, 0)), my = m_display.map(p + QPointF(0, 1));
        const double a = mx.x() - m.x(), b = my.x() - m.x(), c = mx.y() - m.y(), d = my.y() - m.y(), det = a * d - b * c;
        if (!std::isfinite(det) || std::fabs(det) < 1e-30) return std::nullopt;
        const double fx = target.x() - m.x(), fy = target.y() - m.y();
        const QPointF step((d * fx - b * fy) / det, (-c * fx + a * fy) / det);
        p += step;
        if (std::hypot(step.x(), step.y()) < 1e-4) return p;
    }
    return std::nullopt;
}

void GeorefService::addImagePoint(double x, double y)
{
    if (!active() || m_loading) return;
    const QPointF p(x, y);
    if (!inImage(p)) return;
    m_pending = p;
    if (m_map && m_map->tool() != QLatin1String("georef")) m_map->setTool(QStringLiteral("georef"));
    setStatus(QStringLiteral("Теперь — это место на карте · Esc — отмена"));
    emit pendingChanged();
    emit repaint();
    if (m_map) m_map->requestRepaint();
}

void GeorefService::moveImagePoint(int link, double x, double y)
{
    if (link < 0 || link >= m_links.size() || !inImage(QPointF(x, y))) return;
    m_links[link].image = QPointF(x, y);
    refit();
    saveSession();
}

void GeorefService::cancelPending()
{
    if (!m_pending) return;
    m_pending.reset();
    setStatus(QStringLiteral("Точка отменена"));
    emit pendingChanged();
    emit repaint();
    if (m_map) m_map->requestRepaint();
}

void GeorefService::mapClicked(const QPointF &mapPoint)
{
    if (!active() || m_loading) return;
    if (m_pending) {
        m_links.push_back(georef::Link{*m_pending, mapPoint});
        m_pending.reset();
        m_selected = int(m_links.size()) - 1;
        refit();
        saveSession();
        emit pendingChanged();
        emit selectionChanged();
        const int need = georef::minimumLinks(m_method), n = enabledCount();
        setStatus(n < need ? QStringLiteral("Связь %1 · нужно ещё %2").arg(m_links.size()).arg(linkWord(need - n))
                           : QStringLiteral("Связь %1 · %2").arg(m_links.size()).arg(fitText().section('\n', 0, 0)));
        return;
    }
    // The first click on the image shown on the map, the second where it belongs.
    if (m_showImage) {
        const std::optional<QPointF> p = imagePointAt(mapPoint);
        if (p && inImage(*p)) {
            addImagePoint(p->x(), p->y());
            return;
        }
    }
    setStatus(QStringLiteral("Сначала — точка на снимке"));
}

void GeorefService::setLinkEnabled(int index, bool on)
{
    if (index < 0 || index >= m_links.size() || m_links[index].enabled == on) return;
    m_links[index].enabled = on;
    refit();
    saveSession();
}

void GeorefService::removeLink(int index)
{
    if (index < 0 || index >= m_links.size()) return;
    m_links.removeAt(index);
    if (m_selected >= m_links.size()) m_selected = int(m_links.size()) - 1;
    refit();
    saveSession();
    emit selectionChanged();
}

QString GeorefService::setLinkValue(int index, const QString &field, const QString &text)
{
    if (index < 0 || index >= m_links.size()) return QStringLiteral("Нет такой связи");
    QString t = text.trimmed();
    t.remove(QRegularExpression(QStringLiteral("[\\s\\x{00a0}\\x{202f}]")));
    bool ok = false;
    double v = QLocale().toDouble(t, &ok);
    if (!ok) v = QLocale::c().toDouble(QString(t).replace(',', '.'), &ok);
    if (!ok || !std::isfinite(v)) return QStringLiteral("Не число: %1").arg(text);
    georef::Link &l = m_links[index];
    if (field == QLatin1String("ix")) l.image.setX(v);
    else if (field == QLatin1String("iy")) l.image.setY(v);
    else if (field == QLatin1String("mx")) l.map.setX(v);
    else if (field == QLatin1String("my")) l.map.setY(v);
    else return QStringLiteral("Неизвестное поле");
    refit();
    saveSession();
    return {};
}

void GeorefService::clearLinks()
{
    if (m_links.isEmpty() && !m_pending) return;
    m_links.clear();
    m_pending.reset();
    m_selected = -1;
    refit();
    saveSession();
    emit pendingChanged();
    emit selectionChanged();
}

void GeorefService::applyNow()
{
    const bool was = m_autoAdjust;
    m_autoAdjust = true;
    refit();
    m_autoAdjust = was;
}

void GeorefService::refit()
{
    m_fit = georef::fit(m_links, m_method);
    if (m_autoAdjust) {
        QVector<georef::Link> on;
        for (const georef::Link &l : m_links)
            if (l.enabled) on << l;
        georef::Transform t;
        if (fitted()) {
            t = m_fit.forward;
        } else if (on.size() >= 3) {   // fewer links than the method needs: affine meanwhile
            const georef::Fit a = georef::fit(on, georef::Method::Affine);
            if (a.ok()) t = a.forward;
        }
        if (!t.isValid() && on.size() >= 2) {
            // Two links: shift, turn and scale (the image keeps its shape).
            auto z = [](const QPointF &p) { return std::complex<double>(p.x(), -p.y()); };
            const std::complex<double> z1 = z(on[0].image), z2 = z(on[1].image), w1(on[0].map.x(), on[0].map.y()), w2(on[1].map.x(), on[1].map.y());
            if (std::abs(z2 - z1) > 1e-9) {
                const std::complex<double> a = (w2 - w1) / (z2 - z1), b = w1 - a * z1;
                QVector<georef::Link> s;
                for (const QPointF &p : {QPointF(0, 0), QPointF(m_full.width(), 0), QPointF(0, m_full.height())}) {
                    const std::complex<double> w = a * z(p) + b;
                    s << georef::Link{p, QPointF(w.real(), w.imag())};
                }
                const georef::Fit sf = georef::fit(s, georef::Method::Affine);
                if (sf.ok()) t = sf.forward;
            }
        }
        if (!t.isValid() && !on.isEmpty() && m_initial.isValid()) {
            // One link: the first placement moved so that the link's point lands where it was put.
            const QPointF d = on[0].map - m_initial.map(on[0].image);
            QPolygonF c = cornersOf(m_full, m_initial);
            c.translate(d);
            t = georef::Transform::fromCorners(QSizeF(m_full), c);
        }
        m_display = t.isValid() ? t : m_initial;
    }
    emit linksChanged();
    emit repaint();
    if (m_map) m_map->requestRepaint();
}

void GeorefService::zoomToImage()
{
    if (!m_map || !m_display.isValid()) return;
    const QRectF r = cornersOf(m_full, m_display).boundingRect();
    const QSizeF v = m_map->viewSize();
    if (r.isEmpty() || v.isEmpty()) return;
    m_map->setView(r.center(), qMax(r.width() / v.width(), r.height() / v.height()) * 1.15);
}

void GeorefService::zoomToLink(int index)
{
    if (!m_map || index < 0 || index >= m_links.size()) return;
    setSelectedLink(index);
    m_map->setView(m_links[index].map, m_map->resolution());
}

// --- Files.

QString GeorefService::sessionPath() const
{
    if (!m_map || !m_map->hasProject() || m_path.isEmpty()) return {};
    QString key;
    if (m_photoRow >= 0 && m_projects) key = m_projects->photoModel()->photos().value(m_photoRow).id;
    if (key.isEmpty())
        key = QFileInfo(m_path).completeBaseName() + '_'
              + QString::fromLatin1(QCryptographicHash::hash(QDir::cleanPath(m_path).toLower().toUtf8(), QCryptographicHash::Md5).toHex().left(8));
    key.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("_"));
    return QDir(m_map->projectDirectory()).filePath(QStringLiteral("georef/%1.json").arg(key));
}

void GeorefService::saveSession() const
{
    const QString path = sessionPath();
    if (path.isEmpty() || !m_crs.isValid()) return;
    QJsonArray links;
    for (const georef::Link &l : m_links)
        links << QJsonObject{{QStringLiteral("ix"), l.image.x()}, {QStringLiteral("iy"), l.image.y()},
                             {QStringLiteral("mx"), l.map.x()}, {QStringLiteral("my"), l.map.y()}, {QStringLiteral("enabled"), l.enabled}};
    const QJsonObject o{{QStringLiteral("image"), QDir::toNativeSeparators(m_path)},
                        {QStringLiteral("width"), m_full.width()},
                        {QStringLiteral("height"), m_full.height()},
                        {QStringLiteral("method"), int(m_method)},
                        {QStringLiteral("crs"), m_crs.wkt()},
                        {QStringLiteral("links"), links}};
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(o).toJson());
        f.commit();
    }
}

void GeorefService::restoreSession()
{
    QFile f(sessionPath());
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    if (o.value(QStringLiteral("width")).toInt() != m_full.width() || o.value(QStringLiteral("height")).toInt() != m_full.height()) return;
    const int method = o.value(QStringLiteral("method")).toInt(1);
    if (method >= 1 && method <= 4) m_method = georef::Method(method);
    const CoordinateTransform ct(Crs::fromWkt(o.value(QStringLiteral("crs")).toString()), m_crs);
    if (!ct.isValid()) return;
    for (const QJsonValue &v : o.value(QStringLiteral("links")).toArray()) {
        const QJsonObject l = v.toObject();
        QPointF m(l.value(QStringLiteral("mx")).toDouble(), l.value(QStringLiteral("my")).toDouble());
        if (!ct.transform(m)) continue;
        m_links.push_back(georef::Link{QPointF(l.value(QStringLiteral("ix")).toDouble(), l.value(QStringLiteral("iy")).toDouble()), m,
                                       l.value(QStringLiteral("enabled")).toBool(true)});
    }
}

QString GeorefService::saveLinks(const QUrl &url)
{
    if (m_links.isEmpty()) return QStringLiteral("Нет связей");
    const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return QStringLiteral("Не удалось записать %1").arg(QDir::toNativeSeparators(path));
    QByteArray text;
    for (const georef::Link &l : m_links)
        text += QByteArray::number(l.image.x(), 'f', 4) + '\t' + QByteArray::number(-l.image.y(), 'f', 4) + '\t'
                + QByteArray::number(l.map.x(), 'f', 6) + '\t' + QByteArray::number(l.map.y(), 'f', 6) + '\n';
    f.write(text);
    if (!f.commit()) return QStringLiteral("Не удалось записать %1").arg(QDir::toNativeSeparators(path));
    setStatus(QStringLiteral("Связи сохранены"));
    return {};
}

QString GeorefService::loadLinks(const QUrl &url)
{
    if (!active() || m_loading) return QStringLiteral("Сначала откройте изображение");
    const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QStringLiteral("Файл не открывается: %1").arg(QDir::toNativeSeparators(path));
    QVector<georef::Link> read;
    bool down = false;   // y in rows (down) rather than −row
    for (const QByteArray &raw : f.readAll().split('\n')) {
        const QString line = QString::fromUtf8(raw).trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        const QStringList parts = line.split(QRegularExpression(QStringLiteral("[\\t;, ]+")), Qt::SkipEmptyParts);
        if (parts.size() < 4) continue;
        double v[4];
        bool ok = true;
        for (int i = 0; i < 4 && ok; ++i) v[i] = parts[i].toDouble(&ok);
        if (!ok) continue;   // a header
        read.push_back(georef::Link{QPointF(v[0], v[1]), QPointF(v[2], v[3])});
        down = down || v[1] > 0;
    }
    if (read.isEmpty()) return QStringLiteral("В файле нет связей (ожидается «x y X Y» в строке)");
    for (georef::Link &l : read)
        if (!down) l.image.setY(-l.image.y());
    m_links += read;
    refit();
    saveSession();
    setStatus(QStringLiteral("Загружено: %1").arg(linkWord(int(read.size()))));
    return {};
}

// --- Rectify.

QString GeorefService::rectify(double resolution)
{
    if (!active() || m_loading) return QStringLiteral("Сначала откройте изображение");
    if (m_busy) return QStringLiteral("Трансформирование уже идёт");
    if (!fitted()) return fitText();
    if (!m_map || !m_map->hasProject()) return QStringLiteral("Проект не открыт");
    const QString dir = QDir(m_map->projectDirectory()).filePath(QStringLiteral("analysis"));
    QString base = QFileInfo(m_name).completeBaseName() + QStringLiteral("_привязка");
    QString out = QDir(dir).filePath(base + QStringLiteral(".tif"));
    for (int i = 2; QFileInfo::exists(out) && !m_map->layerIdForSource(out).isEmpty(); ++i)
        out = QDir(dir).filePath(QStringLiteral("%1_%2.tif").arg(base).arg(i));
    m_busy = true;
    m_progress = 0;
    m_cancel = std::make_shared<std::atomic_bool>(false);
    emit busyChanged();
    setStatus(QStringLiteral("Создание GeoTIFF…"));
    const QString image = m_path;
    const georef::Transform forward = m_fit.forward;
    const Crs crs = m_crs;
    const auto cancel = m_cancel;
    const QString method = georef::title(m_method), units = this->units();
    const double rmse = m_fit.rmse, inverse = m_fit.inverseRmse;
    const int used = m_fit.used;
    QVector<georef::Link> links = m_links;
    QPointer<GeorefService> self(this);
    QThreadPool::globalInstance()->start([=] {
        double last = -1;
        const georef::WarpResult r = georef::warp(image, forward, crs, out, resolution, 0, [self, cancel, &last](double p) {
            if (p - last < 0.01 && p < 1) return !cancel->load();
            last = p;
            QMetaObject::invokeMethod(qApp, [self, p] {
                if (self && self->m_busy) {
                    self->m_progress = p;
                    emit self->busyChanged();
                }
            }, Qt::QueuedConnection);
            return !cancel->load();
        });
        if (r.ok()) {   // the links beside the result
            QJsonArray a;
            for (const georef::Link &l : links)
                a << QJsonObject{{QStringLiteral("ix"), l.image.x()}, {QStringLiteral("iy"), l.image.y()}, {QStringLiteral("mx"), l.map.x()},
                                 {QStringLiteral("my"), l.map.y()}, {QStringLiteral("enabled"), l.enabled}};
            QSaveFile f(out + QStringLiteral(".georef.json"));
            if (f.open(QIODevice::WriteOnly)) {
                f.write(QJsonDocument(QJsonObject{{QStringLiteral("image"), QDir::toNativeSeparators(image)}, {QStringLiteral("method"), method},
                                                  {QStringLiteral("rmse"), rmse}, {QStringLiteral("inverseRmse"), inverse},
                                                  {QStringLiteral("links"), a}, {QStringLiteral("crs"), crs.wkt()}}).toJson());
                f.commit();
            }
        }
        QMetaObject::invokeMethod(qApp, [self, r, cancel, method, units, rmse, used] {
            if (!self) return;
            self->m_busy = false;
            emit self->busyChanged();
            if (!r.ok()) {
                const QString msg = cancel->load() ? QStringLiteral("Отменено") : r.error;
                self->setStatus(msg);
                emit self->finished(false, msg, {});
                return;
            }
            const QString error = self->m_map ? self->m_map->addLayerFromPath(r.path) : QString();
            const QString msg = QStringLiteral("GeoTIFF по %2 (%1, СКО %3 %4): %5 × %6 пикс., пиксель %7 %4 — %8")
                                    .arg(method, linkWord(used), self->formatNumber(rmse, 3), units).arg(r.width).arg(r.height)
                                    .arg(self->formatNumber(r.resolution, 3), QDir::toNativeSeparators(r.path));
            self->setShowImage(false);
            self->setStatus(error.isEmpty() ? QStringLiteral("GeoTIFF готов и добавлен на карту") : error);
            emit self->finished(error.isEmpty(), error.isEmpty() ? msg : error, r.path);
        }, Qt::QueuedConnection);
    });
    return {};
}

void GeorefService::cancel()
{
    if (m_cancel) m_cancel->store(true);
}

void GeorefService::mapCrsChanged()
{
    if (!m_map || !active() || !m_crs.isValid() || !m_map->crsDefined() || m_map->crs().isSame(m_crs)) return;
    const CoordinateTransform ct(m_crs, m_map->crs());
    if (!ct.isValid()) return;
    for (georef::Link &l : m_links) ct.transform(l.map);
    QVector<QPointF> c;
    for (const QPointF &p : cornersOf(m_full, m_initial)) c << p;
    if (ct.transform(c)) m_initial = georef::Transform::fromCorners(QSizeF(m_full), QPolygonF(c));
    m_crs = m_map->crs();
    m_display = m_initial;
    refit();
    saveSession();
    emit imageChanged();
}

// --- Drawing on the map.

void GeorefService::paintOverlay(QPainter *painter, const MapService &map) const
{
    if (!active() || m_image.isNull() || !m_display.isValid()) return;
    auto screen = [&](const QPointF &px) { return map.toScreen(m_display.map(px)); };
    const QPolygonF corners = [&] {
        QPolygonF q;
        for (const QPointF &p : cornersOf(m_full, m_display)) q << map.toScreen(p);
        return q;
    }();
    if (m_showImage) {
        double side = 0;
        for (int i = 0; i < 4; ++i) side = qMax(side, QLineF(corners[i], corners[(i + 1) % 4]).length());
        const QImage &img = side < SmallSide * 1.3 ? m_small : m_image;
        const double k = double(img.width()) / m_full.width();
        const bool exact = m_display.method() == georef::Method::Affine || m_display.method() == georef::Method::Projective;
        const int g = exact ? 1 : 16;
        const QTransform base = painter->transform();
        painter->setOpacity(m_opacity);
        painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
        for (int j = 0; j < g; ++j)
            for (int i = 0; i < g; ++i) {
                const double x0 = m_full.width() * double(i) / g, x1 = m_full.width() * double(i + 1) / g;
                const double y0 = m_full.height() * double(j) / g, y1 = m_full.height() * double(j + 1) / g;
                const QRectF src(x0 * k, y0 * k, (x1 - x0) * k, (y1 - y0) * k);
                QPolygonF from, to;
                from << src.topLeft() << src.topRight() << src.bottomRight() << src.bottomLeft();
                to << screen(QPointF(x0, y0)) << screen(QPointF(x1, y0)) << screen(QPointF(x1, y1)) << screen(QPointF(x0, y1));
                QTransform t;
                if (!QTransform::quadToQuad(from, to, t)) continue;
                painter->setTransform(t * base);
                // A cell a little larger than its share hides the seams between cells.
                const QRectF draw = exact ? src : src.adjusted(-0.5, -0.5, 0.5, 0.5).intersected(QRectF(QPointF(0, 0), QSizeF(img.size())));
                painter->drawImage(draw, img, draw);
            }
        painter->setTransform(base);
        painter->setOpacity(1);
    }
    // Outline of the image.
    QPolygonF outline;
    const int n = m_display.method() == georef::Method::Affine || m_display.method() == georef::Method::Projective ? 1 : 24;
    for (int e = 0; e < 4; ++e)
        for (int s = 0; s < n; ++s) {
            const double t = double(s) / n;
            const QPointF a[4] = {{0, 0}, {double(m_full.width()), 0}, {double(m_full.width()), double(m_full.height())}, {0, double(m_full.height())}};
            outline << screen(a[e] + (a[(e + 1) % 4] - a[e]) * t);
        }
    QPen edge(QColor(0, 200, 255), 1.5, Qt::DashLine);
    edge.setCosmetic(true);
    painter->setPen(edge);
    painter->setBrush(Qt::NoBrush);
    painter->drawPolygon(outline);
    // Links: the point of the image where the transformation puts it (green), where it should be
    // (red), the blue line between them.
    auto cross = [&](const QPointF &p, const QColor &c, double r) {
        QPen pen(QColor(0, 0, 0, 160), 4);
        pen.setCosmetic(true);
        painter->setPen(pen);
        painter->drawLine(p - QPointF(r, 0), p + QPointF(r, 0));
        painter->drawLine(p - QPointF(0, r), p + QPointF(0, r));
        pen = QPen(c, 2);
        pen.setCosmetic(true);
        painter->setPen(pen);
        painter->drawLine(p - QPointF(r, 0), p + QPointF(r, 0));
        painter->drawLine(p - QPointF(0, r), p + QPointF(0, r));
    };
    QFont font = painter->font();
    font.setPixelSize(12);
    font.setBold(true);
    painter->setFont(font);
    for (int i = 0; i < m_links.size(); ++i) {
        const georef::Link &l = m_links[i];
        const QPointF from = screen(l.image), to = map.toScreen(l.map);
        const bool sel = i == m_selected;
        QPen line(l.enabled ? QColor(60, 140, 255) : QColor(150, 150, 150), sel ? 2.5 : 1.5);
        line.setCosmetic(true);
        painter->setPen(line);
        painter->drawLine(from, to);
        cross(from, l.enabled ? QColor(60, 220, 90) : QColor(160, 160, 160), sel ? 9 : 7);
        cross(to, l.enabled ? QColor(255, 60, 60) : QColor(160, 160, 160), sel ? 9 : 7);
        if (sel) {
            QPen ring(QColor(255, 220, 60), 2);
            ring.setCosmetic(true);
            painter->setPen(ring);
            painter->drawEllipse(to, 12, 12);
        }
        const QString label = QString::number(i + 1);
        const QRectF box(to + QPointF(9, -20), QSizeF(painter->fontMetrics().horizontalAdvance(label) + 8, 16));
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(0, 0, 0, 170));
        painter->drawRoundedRect(box, 3, 3);
        painter->setPen(l.enabled ? QColor(255, 255, 255) : QColor(170, 170, 170));
        painter->drawText(box, Qt::AlignCenter, label);
        painter->setBrush(Qt::NoBrush);
    }
    if (m_pending) {
        const QPointF p = screen(*m_pending);
        cross(p, QColor(255, 220, 60), 10);
        QPen ring(QColor(255, 220, 60), 2, Qt::DotLine);
        ring.setCosmetic(true);
        painter->setPen(ring);
        painter->drawEllipse(p, 14, 14);
    }
}
