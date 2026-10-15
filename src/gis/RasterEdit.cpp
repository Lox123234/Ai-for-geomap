#include "RasterEdit.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QVector>
#include <cmath>
#include <cpl_string.h>
#include <gdal.h>
#include <gdal_utils.h>

namespace rasteredit {

namespace {
QByteArray native(const QString &path) { return QDir::toNativeSeparators(path).toUtf8(); }

QString gdalError(const QString &what)
{
    const QString detail = QString::fromUtf8(CPLGetLastErrorMsg());
    return detail.isEmpty() ? what : QStringLiteral("%1: %2").arg(what, detail);
}

bool rgbBands(GDALDatasetH ds, QString *error)
{
    if (GDALGetRasterCount(ds) < 3 || GDALGetRasterDataType(GDALGetRasterBand(ds, 1)) != GDT_Byte) {
        if (error) *error = QStringLiteral("Правка возможна только для цветного 8-битного растра (RGB или RGBA)");
        return false;
    }
    return true;
}

// Band-interleaved window I/O of bands 1–3 into a packed RGB888 image.
CPLErr rgbIO(GDALDatasetH ds, GDALRWFlag flag, const QRect &w, QImage &rgb)
{
    int bands[3] = {1, 2, 3};
    return GDALDatasetRasterIO(ds, flag, w.x(), w.y(), w.width(), w.height(), rgb.bits(), w.width(), w.height(), GDT_Byte, 3, bands,
                               3, rgb.bytesPerLine(), 1);
}
} // namespace

bool makeEditableCopy(const QString &source, const QString &target, QString *error)
{
    if (QFileInfo::exists(target)) {
        if (error) *error = QStringLiteral("Файл уже существует: %1").arg(QDir::toNativeSeparators(target));
        return false;
    }
    GDALDatasetH src = GDALOpenEx(native(source).constData(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
    if (!src) {
        if (error) *error = gdalError(QStringLiteral("Не удалось открыть растр"));
        return false;
    }
    if (!rgbBands(src, error)) {
        GDALClose(src);
        return false;
    }
    QDir().mkpath(QFileInfo(target).absolutePath());
    const char *args[] = {"-of", "GTiff", "-co", "TILED=YES", "-co", "COMPRESS=DEFLATE", "-co", "PREDICTOR=2", "-co", "BIGTIFF=IF_SAFER",
                          "-co", "NUM_THREADS=ALL_CPUS", nullptr};
    GDALTranslateOptions *options = GDALTranslateOptionsNew(const_cast<char **>(args), nullptr);
    int usage = 0;
    GDALDatasetH copy = GDALTranslate(native(target).constData(), src, options, &usage);
    GDALTranslateOptionsFree(options);
    GDALClose(src);
    if (!copy || usage) {
        if (copy) GDALClose(copy);
        QFile::remove(target);
        if (error) *error = gdalError(QStringLiteral("Не удалось создать копию растра для правки"));
        return false;
    }
    // Internal overviews: the map reads them when zoomed out, edits refresh them.
    QVector<int> levels;
    for (int f = 2; qMax(GDALGetRasterXSize(copy), GDALGetRasterYSize(copy)) / f >= 256; f *= 2) levels << f;
    bool ok = true;
    if (!levels.isEmpty())
        ok = GDALBuildOverviews(copy, "AVERAGE", int(levels.size()), levels.data(), 0, nullptr, nullptr, nullptr) == CE_None;
    GDALClose(copy);
    if (!ok) {
        QFile::remove(target);
        if (error) *error = gdalError(QStringLiteral("Не удалось построить пирамиды копии"));
    }
    return ok;
}

bool readRgb(const QString &path, const QRect &window, QImage *rgb, QImage *alpha, QString *error)
{
    GDALDatasetH ds = GDALOpenEx(native(path).constData(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
    if (!ds) {
        if (error) *error = gdalError(QStringLiteral("Не удалось открыть растр"));
        return false;
    }
    const QRect bounds(0, 0, GDALGetRasterXSize(ds), GDALGetRasterYSize(ds));
    bool ok = rgbBands(ds, error);
    if (ok && !bounds.contains(window)) {
        ok = false;
        if (error) *error = QStringLiteral("Окно за пределами растра");
    }
    if (ok) {
        *rgb = QImage(window.size(), QImage::Format_RGB888);
        ok = rgbIO(ds, GF_Read, window, *rgb) == CE_None;
        if (ok && alpha) {
            *alpha = QImage(window.size(), QImage::Format_Grayscale8);
            GDALRasterBandH a = GDALGetRasterCount(ds) >= 4 && GDALGetRasterColorInterpretation(GDALGetRasterBand(ds, 4)) == GCI_AlphaBand
                                    ? GDALGetRasterBand(ds, 4) : nullptr;
            if (a) ok = GDALRasterIO(a, GF_Read, window.x(), window.y(), window.width(), window.height(), alpha->bits(), window.width(),
                                     window.height(), GDT_Byte, 1, alpha->bytesPerLine()) == CE_None;
            else alpha->fill(255);
        }
        if (!ok && error && error->isEmpty()) *error = gdalError(QStringLiteral("Ошибка чтения растра"));
    }
    GDALClose(ds);
    return ok;
}

bool writeRgb(const QString &path, const QRect &window, const QImage &rgbIn, QString *error)
{
    QImage rgb = rgbIn.convertToFormat(QImage::Format_RGB888);
    if (rgb.size() != window.size()) {
        if (error) *error = QStringLiteral("Размер изображения не совпадает с окном");
        return false;
    }
    GDALDatasetH ds = GDALOpenEx(native(path).constData(), GDAL_OF_RASTER | GDAL_OF_UPDATE, nullptr, nullptr, nullptr);
    if (!ds) {
        if (error) *error = gdalError(QStringLiteral("Растр недоступен для записи"));
        return false;
    }
    const QRect bounds(0, 0, GDALGetRasterXSize(ds), GDALGetRasterYSize(ds));
    bool ok = rgbBands(ds, error) && bounds.contains(window) && rgbIO(ds, GF_Write, window, rgb) == CE_None;
    // Overviews: the window grown to whole overview pixels, averaged from the full resolution.
    const int count = ok ? GDALGetOverviewCount(GDALGetRasterBand(ds, 1)) : 0;
    for (int o = 0; o < count && ok; ++o) {
        GDALRasterBandH ov[3];
        for (int b = 0; b < 3; ++b) ov[b] = GDALGetOverview(GDALGetRasterBand(ds, b + 1), o);
        const int ow = GDALGetRasterBandXSize(ov[0]), oh = GDALGetRasterBandYSize(ov[0]);
        const double fx = double(bounds.width()) / ow, fy = double(bounds.height()) / oh;
        const int ox0 = int(std::floor(window.left() / fx)), oy0 = int(std::floor(window.top() / fy));
        const int ox1 = qMin(ow, int(std::ceil((window.right() + 1) / fx))), oy1 = qMin(oh, int(std::ceil((window.bottom() + 1) / fy)));
        if (ox1 <= ox0 || oy1 <= oy0) continue;
        const QRect full = QRect(QPoint(int(ox0 * fx), int(oy0 * fy)), QPoint(qMin(bounds.width(), int(std::ceil(ox1 * fx))) - 1,
                                                                             qMin(bounds.height(), int(std::ceil(oy1 * fy))) - 1));
        QImage source(full.size(), QImage::Format_RGB888);
        ok = rgbIO(ds, GF_Read, full, source) == CE_None;
        if (!ok) break;
        QImage reduced = source.scaled(ox1 - ox0, oy1 - oy0, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                             .convertToFormat(QImage::Format_RGB888);
        for (int b = 0; b < 3 && ok; ++b)
            ok = GDALRasterIO(ov[b], GF_Write, ox0, oy0, reduced.width(), reduced.height(), reduced.bits() + b, reduced.width(),
                              reduced.height(), GDT_Byte, 3, reduced.bytesPerLine()) == CE_None;
    }
    if (!ok && error && error->isEmpty()) *error = gdalError(QStringLiteral("Ошибка записи растра"));
    GDALClose(ds);
    return ok;
}

} // namespace rasteredit
