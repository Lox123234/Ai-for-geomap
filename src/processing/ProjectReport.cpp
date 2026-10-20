#include "ProjectReport.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLocale>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPainterPath>
#include <QPdfWriter>
#include <QTextDocument>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <gdal.h>
#include <memory>

namespace projectreport {

namespace {
constexpr double Pi = 3.14159265358979323846;

const QLocale &ru()
{
    static const QLocale l(QLocale::Russian);
    return l;
}
QString num(double v, int decimals) { return std::isfinite(v) ? ru().toString(v, 'f', decimals) : QStringLiteral("—"); }
QString esc(const QString &s) { return s.toHtmlEscaped(); }

QColor ramp(double t)   // 0 low … 1 high: green, yellow, brown, light grey
{
    static const QColor stops[] = {QColor(38, 115, 77), QColor(120, 170, 80), QColor(230, 215, 110), QColor(185, 125, 70),
                                   QColor(140, 95, 75), QColor(235, 235, 235)};
    t = qBound(0.0, t, 1.0) * 5;
    const int i = qMin(4, int(t));
    const double f = t - i;
    const QColor a = stops[i], b = stops[i + 1];
    return QColor(int(a.red() + (b.red() - a.red()) * f), int(a.green() + (b.green() - a.green()) * f),
                  int(a.blue() + (b.blue() - a.blue()) * f));
}

QColor errorColor(double dz)   // |dz|: green under 1 m … red over 5 m
{
    const double t = qBound(0.0, (std::fabs(dz) - 1) / 4, 1.0);
    return t < 0.5 ? QColor(58 + int(t * 2 * (245 - 58)), 208 - int(t * 2 * (208 - 197)), 122 - int(t * 2 * (122 - 66)))
                   : QColor(245 - int((t - 0.5) * 2 * (245 - 239)), 197 - int((t - 0.5) * 2 * (197 - 83)), 66 + int((t - 0.5) * 2 * (80 - 66)));
}

// OpenSfM's steps (stats.json) in Russian.
QString stepTitle(const QString &step)
{
    static const QHash<QString, QString> titles = {
        {QStringLiteral("Feature Extraction"), QStringLiteral("Поиск точек")},
        {QStringLiteral("Features Matching"), QStringLiteral("Сопоставление")},
        {QStringLiteral("Tracks Merging"), QStringLiteral("Треки")},
        {QStringLiteral("Reconstruction"), QStringLiteral("Выравнивание")},
    };
    return titles.value(step, step);
}

QString sizeText(qint64 bytes)
{
    if (bytes >= (qint64(1) << 30)) return QStringLiteral("%1 ГБ").arg(num(bytes / double(qint64(1) << 30), 1));
    if (bytes >= (1 << 20)) return QStringLiteral("%1 МБ").arg(num(bytes / double(1 << 20), 1));
    return QStringLiteral("%1 КБ").arg(num(qMax<qint64>(1, bytes / 1024), 0));
}

QString kindTitle(const Artifact &a)
{
    if (a.kind == QLatin1String("orthophoto")) return a.file.startsWith(QLatin1String("geomap/")) ? QStringLiteral("Истинный ортофотоплан") : QStringLiteral("Ортофотоплан");
    if (a.kind == QLatin1String("dsm")) return QStringLiteral("DSM");
    if (a.kind == QLatin1String("dtm")) return QStringLiteral("DTM");
    if (a.kind == QLatin1String("pointcloud")) return QStringLiteral("Облако точек");
    if (a.kind == QLatin1String("mesh")) return QStringLiteral("3D-модель");
    if (a.kind == QLatin1String("tiles")) return QStringLiteral("3D-тайлы");
    if (a.kind == QLatin1String("report")) return QStringLiteral("Отчёт");
    return a.kind;
}
} // namespace

QImage renderScheme(const surveylayout::Layout &layout, const QSize &size)
{
    QImage img(size, QImage::Format_RGB32);
    img.fill(QColor(246, 247, 249));
    if (!layout.ok() || layout.extent.isEmpty()) return img;
    QRectF ext = layout.extent;
    if (!layout.backgroundRect.isEmpty()) ext = ext.united(layout.backgroundRect);
    const double pad = 0.06 * qMax(ext.width(), ext.height());
    ext.adjust(-pad, -pad, pad, pad);
    const double s = qMin(size.width() / ext.width(), size.height() / ext.height());
    const QPointF o((size.width() - ext.width() * s) / 2, (size.height() - ext.height() * s) / 2);
    auto at = [&](double x, double y) { return QPointF(o.x() + (x - ext.left()) * s, o.y() + (ext.bottom() - y) * s); };
    const double k = size.width() / 700.0;   // sizes for the width the report shows
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    if (!layout.background.isNull()) {
        const QRectF r = layout.backgroundRect;
        p.drawImage(QRectF(at(r.left(), r.bottom()), at(r.right(), r.top())), layout.background);
    }
    // Footprints, the flight path, the cameras and their errors.
    p.setPen(QPen(QColor(40, 110, 220, 110), 1 * k));
    p.setBrush(QColor(40, 110, 220, 22));
    for (const surveylayout::Camera &c : layout.cameras) {
        if (c.footprint.size() != 4) continue;
        QPolygonF poly;
        for (const QPointF &q : c.footprint) poly << at(q.x(), q.y());
        p.drawPolygon(poly);
    }
    QPolygonF path;
    for (const surveylayout::Camera &c : layout.cameras)
        if (c.hasGps) path << at(c.gpsX, c.gpsY);
    p.setPen(QPen(QColor(80, 90, 105, 150), 1.2 * k, Qt::DashLine));
    p.setBrush(Qt::NoBrush);
    p.drawPolyline(path);
    double maxErr = 0;
    for (const surveylayout::Camera &c : layout.cameras)
        if (c.hasError()) maxErr = qMax(maxErr, std::hypot(c.dx(), c.dy()));
    // The largest error drawn about 8 % of the scheme long, the scale a round number (1, 2, 5 × 10ⁿ).
    double exaggeration = 1;
    if (maxErr > 0) {
        const double raw = qMax(1.0, 0.08 * qMax(ext.width(), ext.height()) / maxErr), p10 = std::pow(10.0, std::floor(std::log10(raw)));
        for (double m : {1.0, 2.0, 5.0, 10.0})
            if (m * p10 <= raw) exaggeration = m * p10;
    }
    for (const surveylayout::Camera &c : layout.cameras) {
        if (c.hasError()) {
            const QPointF a = at(c.gpsX, c.gpsY), b = at(c.gpsX + c.dx() * exaggeration, c.gpsY + c.dy() * exaggeration);
            p.setPen(QPen(QColor(20, 30, 45, 200), 1.5 * k));
            p.drawLine(a, b);
            p.setPen(QPen(Qt::white, 1 * k));
            p.setBrush(std::isfinite(c.dz()) ? errorColor(c.dz()) : QColor(40, 110, 220));
            p.drawEllipse(b, 4 * k, 4 * k);
        }
        if (c.hasGps) {
            p.setPen(QPen(QColor(20, 30, 45), 1.2 * k));
            p.setBrush(Qt::white);
            p.drawEllipse(at(c.gpsX, c.gpsY), 3 * k, 3 * k);
        }
    }
    // Scale bar and north.
    const double meters = std::pow(10.0, std::floor(std::log10(ext.width() / 5)));
    const double bar = meters * s * (meters * s < size.width() / 8.0 ? 5 : meters * s < size.width() / 4.0 ? 2 : 1);
    const double barMeters = bar / s;
    const QPointF b0(16 * k, size.height() - 18 * k);
    p.setPen(QPen(QColor(20, 30, 45), 2 * k));
    p.drawLine(b0, b0 + QPointF(bar, 0));
    p.drawLine(b0, b0 + QPointF(0, -5 * k));
    p.drawLine(b0 + QPointF(bar, 0), b0 + QPointF(bar, -5 * k));
    QFont f = p.font();
    f.setPixelSize(qMax(10, int(11 * k)));
    p.setFont(f);
    p.drawText(QRectF(b0.x(), b0.y() - 22 * k, 200 * k, 16 * k), barMeters >= 1000 ? QStringLiteral("%1 км").arg(num(barMeters / 1000, 1))
                                                                        : QStringLiteral("%1 м").arg(num(barMeters, 0)));
    const QPointF n(size.width() - 24 * k, 34 * k);
    QPainterPath arrow;
    arrow.moveTo(n + QPointF(0, -18 * k));
    arrow.lineTo(n + QPointF(7 * k, 6 * k));
    arrow.lineTo(n);
    arrow.lineTo(n + QPointF(-7 * k, 6 * k));
    arrow.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(20, 30, 45));
    p.drawPath(arrow);
    p.setPen(QColor(20, 30, 45));
    p.drawText(QRectF(n.x() - 10 * k, n.y() + 6 * k, 20 * k, 16 * k), Qt::AlignCenter, QStringLiteral("С"));
    if (maxErr > 0) {
        p.drawText(QRectF(16 * k, 10 * k, size.width() - 80 * k, 18 * k), QStringLiteral("Ошибки камер увеличены ×%1").arg(num(exaggeration, 0)));
    }
    return img;
}

QImage elevationPreview(const QString &demPath, int maxSide, double *minZ, double *maxZ)
{
    GDALDatasetH ds = GDALOpen(QDir::toNativeSeparators(demPath).toUtf8().constData(), GA_ReadOnly);
    if (!ds) return {};
    const int w = GDALGetRasterXSize(ds), h = GDALGetRasterYSize(ds);
    const double k = qMin(1.0, double(maxSide) / qMax(w, h));
    const int bw = qMax(2, int(w * k)), bh = qMax(2, int(h * k));
    std::vector<float> z(size_t(bw) * bh);
    GDALRasterBandH band = GDALGetRasterBand(ds, 1);
    int hasNoData = 0;
    const double noData = GDALGetRasterNoDataValue(band, &hasNoData);
    double gt[6];
    GDALGetGeoTransform(ds, gt);
    const bool read = GDALRasterIO(band, GF_Read, 0, 0, w, h, z.data(), bw, bh, GDT_Float32, 0, 0) == CE_None;
    GDALClose(ds);
    if (!read) return {};
    auto valid = [&](float v) { return std::isfinite(v) && !(hasNoData && std::fabs(v - noData) < 1e-3) && v > -9000; };
    std::vector<float> sorted;
    for (float v : z) if (valid(v)) sorted.push_back(v);
    if (sorted.empty()) return {};
    std::sort(sorted.begin(), sorted.end());
    const double lo = sorted[size_t(sorted.size() * 0.02)], hi = sorted[size_t(sorted.size() * 0.98) - (sorted.size() > 1 ? 1 : 0)];
    if (minZ) *minZ = lo;
    if (maxZ) *maxZ = hi;
    const double cell = std::fabs(gt[1]) / k;   // metres per preview pixel
    QImage img(bw, bh, QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    for (int y = 1; y + 1 < bh; ++y)
        for (int x = 1; x + 1 < bw; ++x) {
            const float v = z[size_t(y) * bw + x];
            if (!valid(v)) continue;
            auto at = [&](int xx, int yy) { const float u = z[size_t(yy) * bw + xx]; return valid(u) ? double(u) : double(v); };
            // Hillshade from the north-west, 45°.
            const double dzdx = (at(x + 1, y) - at(x - 1, y)) / (2 * cell), dzdy = (at(x, y + 1) - at(x, y - 1)) / (2 * cell);
            const double slope = std::atan(std::hypot(dzdx, dzdy)), aspect = std::atan2(dzdy, -dzdx);
            const double zen = 45 * Pi / 180, az = 315 * Pi / 180;
            const double shade = qBound(0.0, std::cos(zen) * std::cos(slope) + std::sin(zen) * std::sin(slope) * std::cos(az - Pi / 2 - aspect), 1.0);
            const QColor c = ramp(hi > lo ? (v - lo) / (hi - lo) : 0.5);
            const double m = 0.55 + 0.45 * shade;
            img.setPixel(x, y, qRgb(int(c.red() * m), int(c.green() * m), int(c.blue() * m)));
        }
    return img;
}

namespace {
// A4 with 15 mm margins, laid out at 96 dpi: 680 × 1009 units per page.
constexpr double PageW = (210 - 30) / 25.4 * 96, PageH = (297 - 30) / 25.4 * 96;

std::unique_ptr<QTextDocument> build(const Input &in)
{
    const double pageW = PageW, pageH = PageH;
    const int contentW = int(pageW) - 10;
    auto holder = std::make_unique<QTextDocument>();
    QTextDocument &doc = *holder;
    doc.setPageSize(QSizeF(pageW, pageH));
    doc.setDocumentMargin(4);
    QFont font(QStringLiteral("Segoe UI"));
    font.setPixelSize(12);
    doc.setDefaultFont(font);

    // Data.
    const surveylayout::Layout layout = surveylayout::compute(in.photos, in.jobDirectory, 1600);
    const surveylayout::Summary &sum = layout.summary;
    const QImage scheme = renderScheme(layout, QSize(contentW * 2, int(contentW * 2 * 0.66)));
    doc.addResource(QTextDocument::ImageResource, QUrl(QStringLiteral("img://scheme")), scheme);
    QString dem;
    for (const char *k : {"dsm", "dtm"})
        for (const Artifact &a : in.outputs)
            if (dem.isEmpty() && a.kind == QLatin1String(k)) dem = QDir(in.jobDirectory).filePath(a.file);
    double zLo = 0, zHi = 0;
    const QImage elevation = dem.isEmpty() ? QImage() : elevationPreview(dem, 1400, &zLo, &zHi);
    if (!elevation.isNull()) doc.addResource(QTextDocument::ImageResource, QUrl(QStringLiteral("img://dem")), elevation);
    if (!layout.background.isNull()) doc.addResource(QTextDocument::ImageResource, QUrl(QStringLiteral("img://ortho")), layout.background);
    QImage legend(400, 14, QImage::Format_RGB32);
    for (int x = 0; x < legend.width(); ++x)
        for (int y = 0; y < legend.height(); ++y) legend.setPixel(x, y, ramp(double(x) / (legend.width() - 1)).rgb());
    doc.addResource(QTextDocument::ImageResource, QUrl(QStringLiteral("img://legend")), legend);

    auto quality = [&](const QString &label) {
        for (const QVariant &v : in.quality.value(QStringLiteral("rows")).toList())
            if (v.toMap().value(QStringLiteral("label")).toString().startsWith(label)) return v.toMap().value(QStringLiteral("value")).toString();
        return QString();
    };
    const QString gsd = quality(QStringLiteral("Средний GSD")), area = quality(QStringLiteral("Площадь съёмки"));
    const QVariantMap control = in.accuracy.value(QStringLiteral("control")).toMap(), check = in.accuracy.value(QStringLiteral("check")).toMap();
    const bool withPoints = check.value(QStringLiteral("count")).toInt() > 0 || control.value(QStringLiteral("count")).toInt() > 0;
    const double accuracy = check.value(QStringLiteral("count")).toInt() > 0 ? check.value(QStringLiteral("rmse")).toDouble()
                            : control.value(QStringLiteral("count")).toInt() > 0 ? control.value(QStringLiteral("rmse")).toDouble() : sum.rmse;
    const QString accuracyLabel = check.value(QStringLiteral("count")).toInt() > 0 ? QStringLiteral("СКО по контрольным точкам")
                                  : control.value(QStringLiteral("count")).toInt() > 0 ? QStringLiteral("СКО по опорным точкам")
                                                                                        : QStringLiteral("Расхождение с GPS камер");

    // Styles: what Qt rich text supports.
    QString h;
    h += QStringLiteral("<html><head><style>"
                        "h1 { font-size: 22px; color: #10233d; margin-bottom: 2px; }"
                        "h2 { font-size: 15px; color: #10233d; margin-top: 18px; margin-bottom: 6px; }"
                        ".sub { color: #5b6b80; font-size: 11px; }"
                        ".cap { color: #5b6b80; font-size: 10px; }"
                        ".big { font-size: 20px; font-weight: 600; color: #10233d; }"
                        ".lab { font-size: 10px; color: #5b6b80; }"
                        ".warn { color: #a5560b; }"
                        "th { background-color: #eef2f7; color: #33445a; font-size: 10px; font-weight: 600; text-align: left; }"
                        "td { font-size: 11px; color: #1c2a3c; }"
                        "</style></head><body>");
    h += QStringLiteral("<h1>Отчёт об обработке</h1>");
    h += QStringLiteral("<p class='sub'>%1%2 · задание %3 · %4 · %5</p>")
             .arg(esc(in.project), in.chunk.isEmpty() ? QString() : QStringLiteral(" · ") + esc(in.chunk), esc(in.jobId), esc(in.profile),
                  esc((in.finished.isValid() ? in.finished : QDateTime::currentDateTime()).toLocalTime().toString(QStringLiteral("dd.MM.yyyy HH:mm"))));
    // Key numbers.
    auto tile = [](const QString &value, const QString &label) {
        return QStringLiteral("<td width='25%' style='background-color:#f4f6f9; padding:8px'><span class='big'>%1</span><br/><span class='lab'>%2</span></td>")
            .arg(value.isEmpty() ? QStringLiteral("—") : value, label);
    };
    h += QStringLiteral("<table width='100%' cellspacing='6' cellpadding='0'><tr>");
    h += tile(sum.photos ? QStringLiteral("%1 / %2").arg(sum.aligned).arg(sum.photos) : QString(), QStringLiteral("снимков выровнено"));
    h += tile(gsd, QStringLiteral("средний GSD"));
    h += tile(area, QStringLiteral("площадь"));
    h += tile(std::isfinite(accuracy) ? num(accuracy, withPoints ? 3 : 2) + QStringLiteral(" м") : QString(), accuracyLabel);
    h += QStringLiteral("</tr></table>");

    // Survey scheme.
    h += QStringLiteral("<h2>Схема съёмки</h2>");
    h += QStringLiteral("<p><img src='img://scheme' width='%1'/></p>").arg(contentW);
    h += QStringLiteral("<p class='cap'>Белые точки — GPS снимков, линия — маршрут, синие рамки — охваты на земле%1; цвет точки — ошибка по высоте "
                        "(зелёный &lt; 1 м, красный &gt; 5 м).%2</p>")
             .arg(layout.job.isEmpty() ? QString() : QStringLiteral(" по выравниванию"),
                  layout.backgroundName.isEmpty() ? QString() : QStringLiteral(" Подложка — %1.").arg(esc(layout.backgroundName)));

    // Alignment quality.
    h += QStringLiteral("<h2>Качество выравнивания</h2><table width='100%' cellspacing='0' cellpadding='4' border='0'>");
    int row = 0;
    for (const QVariant &v : in.quality.value(QStringLiteral("rows")).toList()) {
        const QVariantMap r = v.toMap();
        h += QStringLiteral("<tr%3><td width='55%'>%1</td><td>%2</td></tr>")
                 .arg(esc(r.value(QStringLiteral("label")).toString()), esc(r.value(QStringLiteral("value")).toString()),
                      row++ % 2 ? QStringLiteral(" style='background-color:#f7f9fb'") : QString());
    }
    h += QStringLiteral("</table>");
    for (const QString &w : in.quality.value(QStringLiteral("warnings")).toStringList())
        h += QStringLiteral("<p class='warn'>⚠ %1</p>").arg(esc(w));

    // Cameras: the largest errors first.
    QVector<surveylayout::Camera> cams;
    for (const surveylayout::Camera &c : layout.cameras) if (c.hasError()) cams << c;
    std::sort(cams.begin(), cams.end(), [](const auto &a, const auto &b) { return std::hypot(a.dx(), a.dy()) > std::hypot(b.dx(), b.dy()); });
    if (!cams.isEmpty()) {
        h += QStringLiteral("<h2>Камеры: GPS против выравнивания</h2>");
        h += QStringLiteral("<p class='sub'>СКО X %1 · Y %2 · в плане %3 · по высоте %4 м</p>")
                 .arg(num(sum.rmseX, 2), num(sum.rmseY, 2), num(sum.rmseXY, 2), num(sum.rmseZ, 2));
        h += QStringLiteral("<table width='100%' cellspacing='0' cellpadding='4'><tr><th>Снимок</th><th>dX, м</th><th>dY, м</th><th>в плане, м</th><th>dZ, м</th></tr>");
        for (int i = 0; i < qMin(15, int(cams.size())); ++i) {
            const surveylayout::Camera &c = cams[i];
            h += QStringLiteral("<tr%6><td>%1</td><td>%2</td><td>%3</td><td>%4</td><td>%5</td></tr>")
                     .arg(esc(c.name), num(c.dx(), 2), num(c.dy(), 2), num(std::hypot(c.dx(), c.dy()), 2), num(c.dz(), 2),
                          i % 2 ? QStringLiteral(" style='background-color:#f7f9fb'") : QString());
        }
        h += QStringLiteral("</table>");
        if (cams.size() > 15) h += QStringLiteral("<p class='cap'>Показаны 15 снимков с наибольшей ошибкой из %1.</p>").arg(cams.size());
    }

    // Control and check points.
    const QVariantList points = in.accuracy.value(QStringLiteral("rows")).toList();
    if (!points.isEmpty()) {
        h += QStringLiteral("<h2>Опорные и контрольные точки</h2>");
        h += QStringLiteral("<table width='100%' cellspacing='0' cellpadding='4'><tr><th>Точка</th><th>Тип</th><th>Снимков</th><th>dX, м</th><th>dY, м</th><th>dZ, м</th><th>Ошибка, м</th></tr>");
        int i = 0;
        for (const QVariant &v : points) {
            const QVariantMap r = v.toMap();
            auto cell = [&](const char *k) { return r.contains(QString::fromLatin1(k)) ? num(r.value(QString::fromLatin1(k)).toDouble(), 3) : QStringLiteral("—"); };
            h += QStringLiteral("<tr%8><td>%1</td><td>%2</td><td>%3</td><td>%4</td><td>%5</td><td>%6</td><td>%7</td></tr>")
                     .arg(esc(r.value(QStringLiteral("name")).toString()),
                          r.value(QStringLiteral("check")).toBool() ? QStringLiteral("контрольная") : QStringLiteral("опорная"),
                          QString::number(r.value(QStringLiteral("photos")).toInt()), cell("dx"), cell("dy"), cell("dz"), cell("d"),
                          i++ % 2 ? QStringLiteral(" style='background-color:#f7f9fb'") : QString());
        }
        h += QStringLiteral("</table>");
        for (const auto &[m, title] : {std::pair{control, QStringLiteral("опорным")}, std::pair{check, QStringLiteral("контрольным")}})
            if (m.value(QStringLiteral("count")).toInt() > 0)
                h += QStringLiteral("<p class='sub'>СКО по %1 (%2 т.): X %3 · Y %4 · Z %5 · общая <b>%6 м</b></p>")
                         .arg(title).arg(m.value(QStringLiteral("count")).toInt())
                         .arg(num(m.value(QStringLiteral("rmseX")).toDouble(), 3), num(m.value(QStringLiteral("rmseY")).toDouble(), 3),
                              num(m.value(QStringLiteral("rmseZ")).toDouble(), 3), num(m.value(QStringLiteral("rmse")).toDouble(), 3));
        if (!in.accuracy.value(QStringLiteral("gcpUsed")).toBool())
            h += QStringLiteral("<p class='cap'>Точки не участвовали в обработке этого задания — это проверка его точности.</p>");
    }

    // Orthophoto and elevation model.
    if (!layout.background.isNull()) {
        h += QStringLiteral("<h2>Ортофотоплан</h2><p><img src='img://ortho' width='%1'/></p><p class='cap'>%2</p>")
                 .arg(qMin(contentW, int(440.0 * layout.background.width() / qMax(1, layout.background.height()))))
                 .arg(esc(layout.backgroundName));
    }
    if (!elevation.isNull()) {
        h += QStringLiteral("<h2>Модель высот</h2><p><img src='img://dem' width='%1'/></p>")
                 .arg(qMin(contentW, int(440.0 * elevation.width() / qMax(1, elevation.height()))));
        h += QStringLiteral("<p><img src='img://legend' width='300' height='10'/></p><p class='cap'>%1 м — %2 м (98 % высот), с отмывкой рельефа · %3</p>")
                 .arg(num(zLo, 1), num(zHi, 1), esc(QFileInfo(dem).fileName()));
    }

    // Results.
    h += QStringLiteral("<h2>Результаты</h2><table width='100%' cellspacing='0' cellpadding='4'><tr><th>Результат</th><th>Файл</th><th>Свойства</th><th>Размер</th></tr>");
    int r2 = 0;
    for (const Artifact &a : in.outputs)
        h += QStringLiteral("<tr%5><td>%1</td><td>%2</td><td>%3</td><td>%4</td></tr>")
                 .arg(kindTitle(a), esc(a.file), esc(a.details), sizeText(a.bytes), r2++ % 2 ? QStringLiteral(" style='background-color:#f7f9fb'") : QString());
    h += QStringLiteral("</table>");

    // Parameters and times.
    h += QStringLiteral("<h2>Параметры обработки</h2>");
    h += QStringLiteral("<p class='sub'>%1 · профиль «%2»%3</p>")
             .arg(esc(in.engine), esc(in.profile),
                  in.started.isValid() && in.finished.isValid()
                      ? QStringLiteral(" · %1 мин").arg(num(in.started.secsTo(in.finished) / 60.0, 1)) : QString());
    h += QStringLiteral("<p style='font-family: Consolas; font-size: 10px; color:#33445a'>%1</p>").arg(esc(in.arguments.join(QLatin1Char(' '))));
    const QVariantList steps = in.quality.value(QStringLiteral("steps")).toList();
    if (!steps.isEmpty()) {
        h += QStringLiteral("<table cellspacing='0' cellpadding='3'>");
        for (const QVariant &v : steps) {
            const QVariantMap s = v.toMap();
            h += QStringLiteral("<tr><td width='240'>%1</td><td>%2 с</td></tr>")
                     .arg(esc(stepTitle(s.value(QStringLiteral("title")).toString())), num(s.value(QStringLiteral("seconds")).toDouble(), 1));
        }
        h += QStringLiteral("</table>");
    }
    h += QStringLiteral("<p class='cap'><br/>Создано GeoMap AI %1 · %2</p></body></html>")
             .arg(esc(in.appVersion), esc(QDateTime::currentDateTime().toString(QStringLiteral("dd.MM.yyyy HH:mm"))));

    doc.setHtml(h);
    return holder;
}
} // namespace

QString write(const Input &in, const QString &pdfPath)
{
    if (!QDir().mkpath(QFileInfo(pdfPath).absolutePath()))
        return QStringLiteral("Папка недоступна: %1").arg(QDir::toNativeSeparators(QFileInfo(pdfPath).absolutePath()));
    const std::unique_ptr<QTextDocument> doc = build(in);
    QFile::remove(pdfPath);
    {
        QPdfWriter pdf(pdfPath);   // the end of the file is written when it goes
        pdf.setPageSize(QPageSize(QPageSize::A4));
        pdf.setPageMargins(QMarginsF(15, 15, 15, 15), QPageLayout::Millimeter);
        pdf.setResolution(300);
        pdf.setTitle(QStringLiteral("GeoMap AI — отчёт: %1").arg(in.project));
        pdf.setCreator(QStringLiteral("GeoMap AI %1").arg(in.appVersion));
        doc->print(&pdf);
    }
    if (QFileInfo(pdfPath).size() < 1000) return QStringLiteral("PDF не записан: %1").arg(QDir::toNativeSeparators(pdfPath));
    return {};
}

QVector<QImage> pages(const Input &in, double scale)
{
    const std::unique_ptr<QTextDocument> doc = build(in);
    QVector<QImage> list;
    const int margin = int(15 / 25.4 * 96 * scale);
    for (int i = 0; i < doc->pageCount(); ++i) {
        QImage img(int((PageW + 2 * 15 / 25.4 * 96) * scale), int((PageH + 2 * 15 / 25.4 * 96) * scale), QImage::Format_RGB32);
        img.fill(Qt::white);
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.translate(margin, margin);
        p.scale(scale, scale);
        p.translate(0, -i * PageH);
        doc->drawContents(&p, QRectF(0, i * PageH, PageW, PageH));
        list << img;
    }
    return list;
}

} // namespace projectreport
