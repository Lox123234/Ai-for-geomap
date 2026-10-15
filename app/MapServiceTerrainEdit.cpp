// Editors of elevation models and point clouds with an outline drawn on the map (or proposed by
// the AI selection). Edits go to the program's copies in <project>/analysis; the user's files are
// never written.
#include "LasEdit.h"
#include "MapService.h"
#include "RasterEdit.h"

#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QThreadPool>
#include <QUndoCommand>
#include <cmath>
#include <gdal.h>
#include <gdal_alg.h>

namespace {
const QLocale &ru()
{
    static const QLocale locale(QLocale::Russian);
    return locale;
}
QString stemOf(const QString &name)
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

// A window of an edited elevation model before and after an edit.
class DemPatchCommand final : public QUndoCommand
{
public:
    DemPatchCommand(MapService *service, const QString &path, const QRect &window, std::vector<float> before, std::vector<float> after,
                    const QString &text)
        : QUndoCommand(text), m_service(service), m_path(path), m_window(window), m_before(std::move(before)), m_after(std::move(after)) {}
    void redo() override
    {
        if (m_first) m_first = false;
        else write(m_after);
    }
    void undo() override { write(m_before); }

private:
    void write(const std::vector<float> &values)
    {
        QString error;
        if (!rasteredit::writeBand(m_path, m_window, values, &error)) emit m_service->errorOccurred(error);
        m_service->rasterEdited(m_path);
    }
    MapService *m_service;
    QString m_path;
    QRect m_window;
    std::vector<float> m_before, m_after;
    bool m_first = true;
};

// Heights inside `mask` (cells != 0) recomputed by `operation`; NaN = no data.
void applyDemOperation(const QString &operation, std::vector<float> &v, const std::vector<uchar> &mask, int w, int h, double level)
{
    const auto at = [w](int x, int y) { return size_t(y) * w + x; };
    if (operation == QLatin1String("level")) {
        for (size_t i = 0; i < v.size(); ++i) if (mask[i]) v[i] = float(level);
        return;
    }
    if (operation == QLatin1String("smooth")) {
        for (int pass = 0; pass < 3; ++pass) {
            const std::vector<float> src = v;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    if (!mask[at(x, y)]) continue;
                    double s = 0;
                    int n = 0;
                    for (int dy = -2; dy <= 2; ++dy)
                        for (int dx = -2; dx <= 2; ++dx) {
                            const int nx = x + dx, ny = y + dy;
                            if (nx < 0 || ny < 0 || nx >= w || ny >= h || !std::isfinite(src[at(nx, ny)])) continue;
                            s += src[at(nx, ny)];
                            ++n;
                        }
                    if (n) v[at(x, y)] = float(s / n);
                }
        }
        return;
    }
    // The boundary: cells with data just outside the mask (up to 2 cells away).
    std::vector<int> boundary;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (mask[at(x, y)] || !std::isfinite(v[at(x, y)])) continue;
            bool near = false;
            for (int dy = -2; dy <= 2 && !near; ++dy)
                for (int dx = -2; dx <= 2 && !near; ++dx) {
                    const int nx = x + dx, ny = y + dy;
                    near = nx >= 0 && ny >= 0 && nx < w && ny < h && mask[at(nx, ny)];
                }
            if (near) boundary.push_back(int(at(x, y)));
        }
    if (boundary.size() < 3) return;
    if (operation == QLatin1String("plane")) {
        // Least squares z = a + b·x + c·y through the boundary cells.
        double s[3][4] = {};
        for (int i : boundary) {
            const double x = i % w, y = i / w, z = v[size_t(i)], row[3] = {1, x, y};
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) s[r][c] += row[r] * row[c];
                s[r][3] += row[r] * z;
            }
        }
        for (int c = 0; c < 3; ++c) {   // Gauss–Jordan
            int p = c;
            for (int r = c + 1; r < 3; ++r) if (std::fabs(s[r][c]) > std::fabs(s[p][c])) p = r;
            std::swap(s[c], s[p]);
            if (std::fabs(s[c][c]) < 1e-12) return;
            for (int r = 0; r < 3; ++r) {
                if (r == c) continue;
                const double f = s[r][c] / s[c][c];
                for (int k = c; k < 4; ++k) s[r][k] -= f * s[c][k];
            }
        }
        const double a = s[0][3] / s[0][0], b = s[1][3] / s[1][1], c = s[2][3] / s[2][2];
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                if (mask[at(x, y)]) v[at(x, y)] = float(a + b * x + c * y);
        return;
    }
    // "interpolate": inverse-distance weighting from the boundary cells (subsampled), then a
    // light smoothing so the patch has no visible facets.
    std::vector<int> pts = boundary;
    const size_t limit = 1500;
    if (pts.size() > limit) {
        std::vector<int> sub;
        for (size_t k = 0; k < pts.size(); k += (pts.size() + limit - 1) / limit) sub.push_back(pts[k]);
        pts.swap(sub);
    }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (!mask[at(x, y)]) continue;
            double sw = 0, sz = 0;
            for (int i : pts) {
                const double dx = i % w - x, dy = i / w - y, d2 = dx * dx + dy * dy;
                const double wgt = 1.0 / (d2 * d2 + 1e-9);   // power 4: dominated by the nearest edge
                sw += wgt;
                sz += wgt * v[size_t(i)];
            }
            if (sw > 0) v[at(x, y)] = float(sz / sw);
        }
}
} // namespace

QVector<QPolygonF> MapService::outlineRings() const
{
    if (!m_aiCandidate.isEmpty()) return m_aiCandidate;
    const int di = drawingsIndex();
    if (di >= 0 && m_selectedFeature >= 0 && m_selectedFeature < m_layers[di].vector.features.size()) {
        const VectorFeature &f = m_layers[di].vector.features[m_selectedFeature];
        if (f.kind == VectorFeature::Polygon) return f.parts;
    }
    return {};
}

bool MapService::hasOutline() const { return !outlineRings().isEmpty(); }

void MapService::showEditedCopy(int index, const QString &copy, const QString &suffix)
{
    const QString originalId = m_layers[index].id, originalName = m_layers[index].name;
    const RasterStyle style = m_layers[index].style;
    m_nextStyle = style;
    m_useNextStyle = m_layers[index].type == MapLayer::Raster;
    const QString added = addLayerFromPath(copy, false);
    m_useNextStyle = false;
    const QString copyId = layerIdForSource(copy);
    if (!added.isEmpty() || copyId.isEmpty()) return;
    if (MapLayer *l = layer(copyId)) l->name = QStringLiteral("%1 (%2)").arg(originalName, suffix);
    while (indexOf(copyId) > indexOf(originalId) + 1) moveLayer(copyId, -1);
    setLayerVisible(originalId, false);
    selectLayer(copyId);
    changed(indexOf(copyId));
    scheduleSave();
}

QString MapService::editDem(const QString &operation, double level, double growMeters)
{
    static const QStringList operations = {QStringLiteral("interpolate"), QStringLiteral("plane"), QStringLiteral("level"), QStringLiteral("smooth")};
    if (!operations.contains(operation)) return QStringLiteral("Неизвестная операция: %1").arg(operation);
    if (m_project.isEmpty() || !m_crs.isValid()) return QStringLiteral("Откройте проект с картой.");
    if (m_analysisBusy) return QStringLiteral("Уже идёт расчёт: %1").arg(m_analysisStatus);
    const QVector<QPolygonF> rings = outlineRings();
    if (rings.isEmpty()) return QStringLiteral("Обведите участок: нарисуйте полигон (и выберите его) или выделите объект ИИ-щелчком.");
    const int index = indexOf(terrainLayerId());
    if (index < 0) return QStringLiteral("На карте нет модели высот (DSM или DTM).");
    const MapLayer &dem = m_layers[index];
    const RasterInfo info = dem.rasterInfo;
    const double *gt = info.geoTransform;
    if (gt[2] != 0 || gt[4] != 0) return QStringLiteral("Повёрнутые модели высот пока не правятся.");
    const Crs demCrs = dem.assignedCrs.isValid() ? dem.assignedCrs : info.crs;
    CoordinateTransform toDem(m_crs, demCrs);
    QVector<QPolygonF> pixelRings;
    for (const QPolygonF &ring : rings) {
        QVector<QPointF> pts(ring.begin(), ring.end());
        if (!toDem.transform(pts)) return QStringLiteral("Не удалось пересчитать контур в систему координат модели.");
        QPolygonF px;
        for (const QPointF &p : pts) px << QPointF((p.x() - gt[0]) / gt[1], (p.y() - gt[3]) / gt[5]);
        pixelRings << px;
    }
    const double cellMeters = std::fabs(gt[1]) * (demCrs.isProjected() ? demCrs.metersPerUnit() : 111320.0);
    const double growPixels = qMax(0.0, growMeters) / qMax(1e-6, cellMeters);
    const QString analysis = QDir(m_project).filePath(QStringLiteral("analysis"));
    const bool inPlace = QFileInfo(dem.source).absolutePath().compare(QDir::cleanPath(analysis), Qt::CaseInsensitive) == 0
                         && QFileInfo(dem.source).completeBaseName().contains(QStringLiteral("_правка"));
    const QString target = inPlace ? dem.source : freePath(analysis, stemOf(QFileInfo(dem.source).completeBaseName()) + QStringLiteral("_правка"), QStringLiteral(".tif"));
    static const QHash<QString, QString> titles = {{QStringLiteral("interpolate"), QStringLiteral("Удаление объекта с модели высот")},
                                                   {QStringLiteral("plane"), QStringLiteral("Выравнивание по плоскости контура")},
                                                   {QStringLiteral("level"), QStringLiteral("Задание высоты участка")},
                                                   {QStringLiteral("smooth"), QStringLiteral("Сглаживание участка")}};
    const QString title = titles.value(operation);
    const QString source = dem.source, layerId = dem.id;
    const QSize size(info.width, info.height);
    m_analysisBusy = true;
    m_analysisStatus = QStringLiteral("%1 «%2»…").arg(title, dem.name);
    emit analysisChanged();
    QThreadPool::globalInstance()->start([this, operation, level, pixelRings, growPixels, inPlace, source, target, size, title, layerId] {
        QString error;
        QRect window;
        std::vector<float> before, after;
        int cells = 0;
        if (inPlace || rasteredit::makeEditableCopy(source, target, &error)) {
            QRectF b;
            for (const QPolygonF &r : pixelRings) b = b.isNull() ? r.boundingRect() : b.united(r.boundingRect());
            const int margin = int(std::ceil(growPixels)) + 6;
            window = b.toAlignedRect().adjusted(-margin, -margin, margin, margin).intersected(QRect(QPoint(0, 0), size));
            if (window.isEmpty()) error = QStringLiteral("Контур вне модели высот");
            else if (rasteredit::readBand(target, window, &before, &error)) {
                QImage mask(window.size(), QImage::Format_Grayscale8);
                mask.fill(0);
                {
                    QPainter p(&mask);
                    p.translate(-window.topLeft());
                    QPainterPath path;
                    path.setFillRule(Qt::OddEvenFill);
                    for (const QPolygonF &r : pixelRings) {
                        path.addPolygon(r);
                        path.closeSubpath();
                    }
                    p.setPen(growPixels > 0.5 ? QPen(Qt::white, 2 * growPixels, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin) : QPen(Qt::NoPen));
                    p.setBrush(Qt::white);
                    p.drawPath(path);
                }
                std::vector<uchar> m(before.size());
                for (int y = 0; y < window.height(); ++y)
                    for (int x = 0; x < window.width(); ++x) {
                        const size_t i = size_t(y) * window.width() + x;
                        m[i] = mask.constScanLine(y)[x] && std::isfinite(before[i]);   // holes of the model stay holes
                        cells += m[i];
                    }
                if (!cells) error = QStringLiteral("Контур не задевает ячеек модели с данными");
                else {
                    after = before;
                    applyDemOperation(operation, after, m, window.width(), window.height(), level);
                    rasteredit::writeBand(target, window, after, &error);
                }
            }
        }
        QMetaObject::invokeMethod(this, [this, error, target, inPlace, window, before, after, title, layerId, cells] {
            m_analysisBusy = false;
            m_analysisStatus.clear();
            emit analysisChanged();
            if (!error.isEmpty()) {
                emit analysisFinished(false, error);
                return;
            }
            const int index = indexOf(layerId);
            if (!inPlace && index >= 0) showEditedCopy(index, target, QStringLiteral("правка"));
            else rasterEdited(target);
            m_undo.push(new DemPatchCommand(this, target, window, before, after, title));
            emit analysisFinished(true, QStringLiteral("%1: изменено ячеек %2. Файл: %3 (исходная модель не изменена; Ctrl+Z — отменить)")
                                            .arg(title).arg(cells).arg(QDir::toNativeSeparators(target)));
        }, Qt::QueuedConnection);
    });
    return {};
}

QString MapService::fillDemGaps(const QString &layerId)
{
    if (m_project.isEmpty()) return QStringLiteral("Откройте проект.");
    if (m_analysisBusy) return QStringLiteral("Уже идёт расчёт: %1").arg(m_analysisStatus);
    const int index = indexOf(layerId.isEmpty() ? terrainLayerId() : layerId);
    if (index < 0 || !isElevation(m_layers[index])) return QStringLiteral("Выберите модель высот на карте.");
    const MapLayer &dem = m_layers[index];
    if (qint64(dem.rasterInfo.width) * dem.rasterInfo.height > 200'000'000) return QStringLiteral("Модель слишком велика для заполнения целиком.");
    const QString target = freePath(QDir(m_project).filePath(QStringLiteral("analysis")),
                                    stemOf(QFileInfo(dem.source).completeBaseName()) + QStringLiteral("_заполнено"), QStringLiteral(".tif"));
    const QString source = dem.source, id = dem.id, name = dem.name;
    m_analysisBusy = true;
    m_analysisStatus = QStringLiteral("Заполнение пропусков «%1»…").arg(name);
    emit analysisChanged();
    QThreadPool::globalInstance()->start([this, source, target, id, name] {
        QString error;
        qint64 filled = 0;
        if (rasteredit::makeEditableCopy(source, target, &error)) {
            GDALDatasetH ds = GDALOpen(QDir::toNativeSeparators(target).toUtf8().constData(), GA_Update);
            GDALRasterBandH band = ds ? GDALGetRasterBand(ds, 1) : nullptr;
            const int w = ds ? GDALGetRasterXSize(ds) : 0, h = ds ? GDALGetRasterYSize(ds) : 0;
            std::vector<float> v(size_t(w) * h);
            int has = 0;
            const double nodata = band ? GDALGetRasterNoDataValue(band, &has) : 0;
            if (!band || GDALRasterIO(band, GF_Read, 0, 0, w, h, v.data(), w, h, GDT_Float32, 0, 0) != CE_None || !has) {
                error = has ? QStringLiteral("Модель высот не читается") : QStringLiteral("У модели нет значения «нет данных» — пропусков нет");
            } else {
                // Holes = cells without data not connected to the edge of the raster.
                std::vector<uchar> outside(v.size(), 0);
                std::vector<int> stack;
                auto empty = [&](size_t i) { return v[i] == float(nodata) || !std::isfinite(v[i]); };
                for (int x = 0; x < w; ++x) for (int y : {0, h - 1}) if (empty(size_t(y) * w + x)) stack.push_back(y * w + x);
                for (int y = 0; y < h; ++y) for (int x : {0, w - 1}) if (empty(size_t(y) * w + x)) stack.push_back(y * w + x);
                while (!stack.empty()) {
                    const int i = stack.back();
                    stack.pop_back();
                    if (outside[size_t(i)]) continue;
                    outside[size_t(i)] = 1;
                    const int x = i % w, y = i / w;
                    const int n[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
                    for (const auto &q : n)
                        if (q[0] >= 0 && q[1] >= 0 && q[0] < w && q[1] < h && !outside[size_t(q[1]) * w + q[0]] && empty(size_t(q[1]) * w + q[0]))
                            stack.push_back(q[1] * w + q[0]);
                }
                // Mask band for GDALFillNodata: 255 = data or outside (kept), 0 = a hole to fill.
                GDALDatasetH mem = GDALCreate(GDALGetDriverByName("MEM"), "", w, h, 1, GDT_Byte, nullptr);
                std::vector<uchar> keep(v.size());
                for (size_t i = 0; i < v.size(); ++i) {
                    keep[i] = empty(i) && !outside[i] ? 0 : 255;
                    filled += keep[i] == 0;
                }
                GDALRasterIO(GDALGetRasterBand(mem, 1), GF_Write, 0, 0, w, h, keep.data(), w, h, GDT_Byte, 0, 0);
                if (filled > 0 && GDALFillNodata(band, GDALGetRasterBand(mem, 1), qMax(w, h), 0, 1, nullptr, nullptr, nullptr) != CE_None)
                    error = QStringLiteral("GDAL: %1").arg(QString::fromUtf8(CPLGetLastErrorMsg()));
                // GDALFillNodata also fills the outside cells it reaches: put them back.
                if (error.isEmpty() && filled > 0) {
                    std::vector<float> f(v.size());
                    GDALRasterIO(band, GF_Read, 0, 0, w, h, f.data(), w, h, GDT_Float32, 0, 0);
                    for (size_t i = 0; i < f.size(); ++i) if (outside[i]) f[i] = float(nodata);
                    GDALRasterIO(band, GF_Write, 0, 0, w, h, f.data(), w, h, GDT_Float32, 0, 0);
                    const int levels[] = {2, 4, 8, 16, 32, 64};
                    int count = 0;
                    while (count < 6 && qMax(w, h) / levels[count] >= 256) ++count;
                    if (count) GDALBuildOverviews(ds, "AVERAGE", count, const_cast<int *>(levels), 0, nullptr, nullptr, nullptr);
                }
                GDALClose(mem);
            }
            if (ds) GDALClose(ds);
        }
        QMetaObject::invokeMethod(this, [this, error, target, id, name, filled] {
            m_analysisBusy = false;
            m_analysisStatus.clear();
            emit analysisChanged();
            if (!error.isEmpty()) {
                QFile::remove(target);
                emit analysisFinished(false, error);
                return;
            }
            if (filled == 0) {
                QFile::remove(target);
                emit analysisFinished(true, QStringLiteral("В модели «%1» нет внутренних пропусков.").arg(name));
                return;
            }
            const int index = indexOf(id);
            if (index >= 0) showEditedCopy(index, target, QStringLiteral("пропуски заполнены"));
            emit analysisFinished(true, QStringLiteral("Пропуски модели «%1» заполнены: %2 ячеек. Файл: %3")
                                            .arg(name).arg(ru().toString(double(filled), 'f', 0), QDir::toNativeSeparators(target)));
        }, Qt::QueuedConnection);
    });
    return {};
}

QString MapService::editPointCloud(const QString &lasPath, const QString &operation, int classCode, const QString &dtmPath)
{
    if (m_project.isEmpty()) return QStringLiteral("Откройте проект.");
    if (m_analysisBusy) return QStringLiteral("Уже идёт расчёт: %1").arg(m_analysisStatus);
    if (!QFileInfo::exists(lasPath)) return QStringLiteral("Нет файла облака: %1").arg(QDir::toNativeSeparators(lasPath));
    if (lasPath.endsWith(QLatin1String(".laz"), Qt::CaseInsensitive))
        return QStringLiteral("Сжатый LAZ не редактируется; профили обработки сохраняют облако и в LAS (рядом с LAZ).");
    const bool whole = operation == QLatin1String("classify");
    las::Operation op = las::Operation::DeleteInside;
    if (operation == QLatin1String("keepInside")) op = las::Operation::KeepInside;
    else if (operation == QLatin1String("classifyInside")) op = las::Operation::ClassifyInside;
    else if (operation != QLatin1String("deleteInside") && !whole) return QStringLiteral("Неизвестная операция: %1").arg(operation);
    const QVector<QPolygonF> rings = whole ? QVector<QPolygonF>() : outlineRings();
    if (!whole && rings.isEmpty()) return QStringLiteral("Обведите участок: нарисуйте полигон (и выберите его) или выделите объект ИИ-щелчком.");
    if (whole && !QFileInfo::exists(dtmPath)) return QStringLiteral("Для классификации нужна модель рельефа (DTM): постройте её профилем «Высокое качество».");
    const QString suffix = whole ? QStringLiteral("_классы") : QStringLiteral("_правка");
    const QString out = freePath(QDir(m_project).filePath(QStringLiteral("analysis")), stemOf(QFileInfo(lasPath).completeBaseName()) + suffix,
                                 QStringLiteral(".las"));
    const Crs crs = m_crs;
    m_analysisBusy = true;
    m_analysisStatus = whole ? QStringLiteral("Классификация облака точек…") : QStringLiteral("Правка облака точек…");
    emit analysisChanged();
    QThreadPool::globalInstance()->start([this, lasPath, out, op, rings, crs, classCode, dtmPath, whole] {
        const las::Result r = whole ? las::classify(lasPath, out, dtmPath) : las::edit(lasPath, out, op, rings, crs, classCode);
        QMetaObject::invokeMethod(this, [this, r, whole, op, classCode] {
            m_analysisBusy = false;
            m_analysisStatus.clear();
            emit analysisChanged();
            if (!r.ok()) {
                emit cloudEditFinished(false, r.error, QString());
                return;
            }
            QString message;
            if (whole) {
                static const QHash<int, QString> names = {{1, QStringLiteral("прочее")}, {2, QStringLiteral("земля")}, {3, QStringLiteral("низкая растительность")},
                                                          {4, QStringLiteral("средняя")}, {5, QStringLiteral("высокая")}, {6, QStringLiteral("здания")},
                                                          {7, QStringLiteral("шум")}};
                QStringList parts;
                for (auto it = r.classes.cbegin(); it != r.classes.cend(); ++it)
                    parts << QStringLiteral("%1 %2 %").arg(names.value(it.key(), QString::number(it.key())), ru().toString(100.0 * it.value() / qMax<qint64>(1, r.written), 'f', 1));
                message = QStringLiteral("Облако классифицировано по высоте над DTM и цвету: %1").arg(parts.join(QStringLiteral(", ")));
            } else if (op == las::Operation::ClassifyInside) {
                message = QStringLiteral("Класс %1 присвоен %2 точкам внутри контура").arg(classCode).arg(ru().toString(double(r.changed), 'f', 0));
            } else {
                message = QStringLiteral("Удалено точек: %1 из %2").arg(ru().toString(double(r.changed), 'f', 0), ru().toString(double(r.total), 'f', 0));
            }
            emit cloudEditFinished(true, message + QStringLiteral(". Новый файл: %1 (исходное облако не изменено)").arg(QDir::toNativeSeparators(r.path)), r.path);
        }, Qt::QueuedConnection);
    });
    return {};
}
