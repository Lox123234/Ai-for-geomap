#include "ResultExport.h"

#include "GisEnvironment.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <cpl_vsi.h>
#include <gdal.h>
#include <gdal_utils.h>

namespace exporting {

namespace {

QByteArray nativePath(const QString &path) { return QDir::toNativeSeparators(path).toUtf8(); }

struct ArgList
{
    QList<QByteArray> items;
    QVector<char *> argv;
    char **data()
    {
        argv.clear();
        for (QByteArray &s : items) argv << s.data();
        argv << nullptr;
        return argv.data();
    }
};

int progress(double, const char *, void *data)
{
    const auto *cancel = static_cast<const std::atomic_bool *>(data);
    return cancel && *cancel ? FALSE : TRUE;
}

} // namespace

QString uniqueTarget(const QString &folder, const QString &name)
{
    const QFileInfo fi(name);
    const QString suffix = fi.suffix().isEmpty() ? QString() : QLatin1Char('.') + fi.suffix();
    const QString stem = suffix.isEmpty() ? name : name.left(name.size() - suffix.size());
    QString path = QDir(folder).filePath(name);
    for (int n = 2; QFileInfo::exists(path); ++n) path = QDir(folder).filePath(QStringLiteral("%1-%2%3").arg(stem).arg(n).arg(suffix));
    return path;
}

Result exportRaster(const QString &source, const QString &target, const RasterOptions &options, const std::atomic_bool *cancel)
{
    Result r;
    r.path = target;
    GisEnvironment::clearError();
    GDALDatasetH src = GDALOpenEx(nativePath(source).constData(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
    if (!src) {
        r.error = QStringLiteral("Не открывается %1: %2").arg(QFileInfo(source).fileName(), GisEnvironment::lastError());
        return r;
    }
    QDir().mkpath(QFileInfo(target).absolutePath());
    GDALDatasetH input = src;
    QByteArray memPath;
    // Warp first (into a lazy in-memory VRT) when the CRS or the cell size changes.
    if (options.crs.isValid() || options.resolution > 0) {
        ArgList args;
        args.items = {"-of", "VRT", "-r", "bilinear"};
        if (options.crs.isValid()) args.items << "-t_srs" << options.crs.wkt().toUtf8();
        if (options.resolution > 0)
            args.items << "-tr" << QByteArray::number(options.resolution, 'g', 12) << QByteArray::number(options.resolution, 'g', 12);
        int has = 0;
        const double nd = GDALGetRasterNoDataValue(GDALGetRasterBand(src, 1), &has);
        if (has) args.items << "-dstnodata" << QByteArray::number(nd, 'g', 17);
        GDALWarpAppOptions *warp = GDALWarpAppOptionsNew(args.data(), nullptr);
        memPath = QByteArray("/vsimem/geomap-export-") + QByteArray::number(quintptr(&r)) + ".vrt";
        int usage = 0;
        input = GDALWarp(memPath.constData(), nullptr, 1, &src, warp, &usage);
        GDALWarpAppOptionsFree(warp);
        if (!input) {
            GDALClose(src);
            r.error = QStringLiteral("Не удалось пересчитать растр: %1").arg(GisEnvironment::lastError());
            return r;
        }
    }
    ArgList args;
    if (options.cog) args.items = {"-of", "COG", "-co", "COMPRESS=DEFLATE", "-co", "BIGTIFF=IF_SAFER"};
    else args.items = {"-of", "GTiff", "-co", "COMPRESS=DEFLATE", "-co", "TILED=YES", "-co", "BIGTIFF=IF_SAFER"};
    GDALTranslateOptions *translate = GDALTranslateOptionsNew(args.data(), nullptr);
    GDALTranslateOptionsSetProgress(translate, progress, const_cast<std::atomic_bool *>(cancel));
    int usage = 0;
    GDALDatasetH out = GDALTranslate(nativePath(target).constData(), input, translate, &usage);
    GDALTranslateOptionsFree(translate);
    if (out) GDALClose(out);
    if (input != src) GDALClose(input);
    GDALClose(src);
    if (!memPath.isEmpty()) VSIUnlink(memPath.constData());
    if (!out || (cancel && *cancel)) {
        QFile::remove(target);
        r.error = cancel && *cancel ? QStringLiteral("Экспорт отменён") : QStringLiteral("GDAL: %1").arg(GisEnvironment::lastError());
        return r;
    }
    r.files << target;
    r.bytes = QFileInfo(target).size();
    return r;
}

Result copyFile(const QString &source, const QString &target)
{
    Result r;
    r.path = target;
    QDir().mkpath(QFileInfo(target).absolutePath());
    if (!QFile::copy(source, target)) {
        r.error = QStringLiteral("Не удалось скопировать %1").arg(QFileInfo(source).fileName());
        return r;
    }
    r.files << target;
    r.bytes = QFileInfo(target).size();
    return r;
}

Result exportObj(const QString &objPath, const QString &folder)
{
    Result r;
    QFile obj(objPath);
    if (!obj.open(QIODevice::ReadOnly)) {
        r.error = QStringLiteral("Модель не открывается: %1").arg(QFileInfo(objPath).fileName());
        return r;
    }
    const QDir sourceDir = QFileInfo(objPath).absoluteDir();
    // The MTL files the OBJ names, and the textures they name (paths relative to the OBJ).
    QStringList files{QFileInfo(objPath).fileName()};
    QSet<QString> seen;
    while (!obj.atEnd()) {
        const QByteArray line = obj.readLine().trimmed();
        if (!line.startsWith("mtllib ")) continue;
        const QString mtl = QString::fromUtf8(line.mid(7).trimmed());
        if (seen.contains(mtl)) continue;
        seen.insert(mtl);
        files << mtl;
        QFile m(sourceDir.filePath(mtl));
        if (!m.open(QIODevice::ReadOnly)) continue;
        while (!m.atEnd()) {
            const QByteArray l = m.readLine().trimmed();
            for (const char *key : {"map_Kd ", "map_Ka ", "map_Ks ", "map_d ", "map_Bump ", "bump ", "norm "}) {
                if (!l.startsWith(key)) continue;
                const QString tex = QString::fromUtf8(l.mid(int(qstrlen(key))).trimmed());
                if (!tex.isEmpty() && !seen.contains(tex)) {
                    seen.insert(tex);
                    files << tex;
                }
            }
        }
    }
    QDir().mkpath(folder);
    for (const QString &f : files) {
        const QString from = sourceDir.filePath(f);
        if (!QFileInfo(from).isFile()) {
            r.error = QStringLiteral("Нет файла модели: %1").arg(f);
            return r;
        }
        if (QDir::cleanPath(f).startsWith(QLatin1String("..")) || QDir::isAbsolutePath(f)) {
            r.error = QStringLiteral("Файл модели вне её папки: %1").arg(f);
            return r;
        }
        const QString to = QDir(folder).filePath(f);
        QDir().mkpath(QFileInfo(to).absolutePath());
        QFile::remove(to);
        if (!QFile::copy(from, to)) {
            r.error = QStringLiteral("Не удалось скопировать %1").arg(f);
            return r;
        }
        r.files << to;
        r.bytes += QFileInfo(to).size();
    }
    r.path = QDir(folder).filePath(files.first());
    return r;
}

} // namespace exporting
