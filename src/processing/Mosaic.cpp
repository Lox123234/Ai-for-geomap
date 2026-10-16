#include "Mosaic.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>
#include <cpl_conv.h>
#include <cpl_string.h>
#include <gdal.h>
#include <gdalwarper.h>
#include <ogr_srs_api.h>

namespace mosaic {
namespace {

QByteArray native(const QString &path) { return QDir::toNativeSeparators(path).toUtf8(); }

constexpr float OutputNoData = -9999.0f;
constexpr int Tile = 1024;

struct Source
{
    GDALDatasetH file = nullptr;
    GDALDatasetH view = nullptr;    // in the output CRS: the file itself or a warped VRT over it
    double gt[6] = {};
    int w = 0, h = 0;
    double cx = 0, cy = 0;          // centre in the output CRS: of the chunk's cell, else of the extent
    bool alpha = false;             // last band is alpha
    bool hasNoData = false;
    double noData = 0;
    QString name;
    // The current tile: values band by band and whether each pixel has data.
    std::vector<float> values;
    std::vector<unsigned char> valid;
    bool inTile = false;
};

struct Closer
{
    std::vector<Source> &sources;
    ~Closer()
    {
        for (Source &s : sources) {
            if (s.view && s.view != s.file) GDALClose(s.view);
            if (s.file) GDALClose(s.file);
        }
    }
};

} // namespace

Result build(const QVector<Input> &inputs, const QString &output, const Options &opt)
{
    QElapsedTimer timer;
    timer.start();
    Result res;
    auto progress = [&](double v) { return !opt.progress || opt.progress(v); };
    if (inputs.isEmpty()) {
        res.error = QStringLiteral("Нет растров для объединения.");
        return res;
    }
    std::vector<Source> sources;
    Closer closer{sources};
    QString dstWkt;
    OGRSpatialReferenceH dst = nullptr;
    int bands = 0;
    bool rgb = false;
    for (const Input &in : inputs) {
        Source s;
        s.name = in.name.isEmpty() ? QFileInfo(in.path).fileName() : in.name;
        s.file = GDALOpenEx(native(in.path).constData(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
        if (!s.file) {
            res.error = QStringLiteral("Не открыть растр «%1»: %2").arg(s.name, QString::fromUtf8(CPLGetLastErrorMsg()));
            break;
        }
        const QString wkt = QString::fromUtf8(GDALGetProjectionRef(s.file));
        double gt[6];
        if (wkt.isEmpty() || GDALGetGeoTransform(s.file, gt) != CE_None) {
            res.error = QStringLiteral("У растра «%1» нет геопривязки.").arg(s.name);
            sources.push_back(s);
            break;
        }
        if (gt[2] != 0 || gt[4] != 0 || gt[5] >= 0) {
            res.error = QStringLiteral("Растр «%1» повёрнут — не поддерживается.").arg(s.name);
            sources.push_back(s);
            break;
        }
        const int count = GDALGetRasterCount(s.file);
        const GDALDataType type = GDALGetRasterDataType(GDALGetRasterBand(s.file, 1));
        const bool isRgb = count >= 3 && type == GDT_Byte;
        if (dstWkt.isEmpty()) {
            dstWkt = wkt;
            dst = OSRNewSpatialReference(dstWkt.toUtf8().constData());
            rgb = isRgb;
            bands = rgb ? 3 : 1;
            if (!rgb && count != 1) {
                res.error = QStringLiteral("«%1»: объединяются ортофотопланы (RGB) или модели высот (один канал), здесь каналов %2.")
                                .arg(s.name).arg(count);
                sources.push_back(s);
                break;
            }
        } else if (isRgb != rgb || (!rgb && count != 1)) {
            res.error = QStringLiteral("«%1» другого вида, чем «%2»: объединяются растры одного вида (ортофотопланы или модели высот).")
                            .arg(s.name, sources.front().name);
            sources.push_back(s);
            break;
        }
        s.alpha = count >= 2 && GDALGetRasterColorInterpretation(GDALGetRasterBand(s.file, count)) == GCI_AlphaBand;
        int has = 0;
        s.noData = GDALGetRasterNoDataValue(GDALGetRasterBand(s.file, 1), &has);
        s.hasNoData = has != 0;
        // Another CRS: a warped view in the output CRS.
        OGRSpatialReferenceH srs = OSRNewSpatialReference(wkt.toUtf8().constData());
        const bool same = srs && dst && OSRIsSame(srs, dst);
        OSRDestroySpatialReference(srs);
        if (same) {
            s.view = s.file;
        } else {
            s.view = GDALAutoCreateWarpedVRT(s.file, nullptr, dstWkt.toUtf8().constData(), GRA_Bilinear, 0.125, nullptr);
            if (!s.view) {
                res.error = QStringLiteral("«%1» не перепроецируется в систему координат «%2».").arg(s.name, sources.front().name);
                sources.push_back(s);
                break;
            }
            res.notes << QStringLiteral("«%1» перепроецирован в систему координат «%2»").arg(s.name, sources.front().name);
        }
        GDALGetGeoTransform(s.view, s.gt);
        s.w = GDALGetRasterXSize(s.view);
        s.h = GDALGetRasterYSize(s.view);
        s.cx = s.gt[0] + s.gt[1] * s.w / 2;
        s.cy = s.gt[3] + s.gt[5] * s.h / 2;
        if (in.cell.size() >= 3 && dst) {
            OGRSpatialReferenceH wgs = OSRNewSpatialReference(nullptr);
            OSRSetWellKnownGeogCS(wgs, "WGS84");
            OSRSetAxisMappingStrategy(wgs, OAMS_TRADITIONAL_GIS_ORDER);
            OSRSetAxisMappingStrategy(dst, OAMS_TRADITIONAL_GIS_ORDER);
            OGRCoordinateTransformationH ct = OCTNewCoordinateTransformation(wgs, dst);
            double sx = 0, sy = 0;
            int n = 0;
            for (const QPointF &p : in.cell) {
                double x = p.x(), y = p.y();
                if (ct && OCTTransform(ct, 1, &x, &y, nullptr)) {
                    sx += x;
                    sy += y;
                    ++n;
                }
            }
            if (n > 0) {
                s.cx = sx / n;
                s.cy = sy / n;
            }
            if (ct) OCTDestroyCoordinateTransformation(ct);
            OSRDestroySpatialReference(wgs);
        }
        sources.push_back(s);
    }
    if (dst && res.error.isEmpty()) {
        const char *name = OSRGetName(dst);
        res.crs = name ? QString::fromUtf8(name) : QString();
    }
    const bool geographic = dst && OSRIsGeographic(dst);
    if (dst) OSRDestroySpatialReference(dst);
    if (!res.error.isEmpty()) return res;
    res.inputs = int(sources.size());

    // The output grid: the union of the extents at the finest resolution.
    double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300, finest = 1e300;
    for (const Source &s : sources) {
        x0 = std::min(x0, s.gt[0]);
        x1 = std::max(x1, s.gt[0] + s.gt[1] * s.w);
        y1 = std::max(y1, s.gt[3]);
        y0 = std::min(y0, s.gt[3] + s.gt[5] * s.h);
        finest = std::min({finest, std::fabs(s.gt[1]), std::fabs(s.gt[5])});
    }
    const double r = opt.resolution > 0 ? opt.resolution : finest;
    const double W = std::ceil((x1 - x0) / r), H = std::ceil((y1 - y0) / r);
    if (!(r > 0) || W < 1 || H < 1 || W > 300000 || H > 300000 || W * H > 4.0e10) {
        res.error = QStringLiteral("Объединённый растр слишком велик или пуст (%1 × %2 пикселей).").arg(W, 0, 'f', 0).arg(H, 0, 'f', 0);
        return res;
    }
    res.width = int(W);
    res.height = int(H);
    res.resolution = r;
    res.bands = rgb ? 4 : 1;
    const double blend = std::max(1e-9, (geographic ? opt.blend / 111320.0 : opt.blend));

    const QString partial = output.endsWith(QLatin1String(".tif"), Qt::CaseInsensitive)
                                ? output.left(output.size() - 4) + QStringLiteral(".partial.tif") : output + QStringLiteral(".partial");
    QDir().mkpath(QFileInfo(output).absolutePath());
    QFile::remove(partial);
    char **create = nullptr;
    create = CSLSetNameValue(create, "TILED", "YES");
    create = CSLSetNameValue(create, "COMPRESS", "DEFLATE");
    create = CSLSetNameValue(create, "BIGTIFF", "IF_SAFER");
    if (rgb) {
        create = CSLSetNameValue(create, "PHOTOMETRIC", "RGB");
        create = CSLSetNameValue(create, "ALPHA", "YES");
    }
    GDALDatasetH out = GDALCreate(GDALGetDriverByName("GTiff"), native(partial).constData(), res.width, res.height, res.bands,
                                  rgb ? GDT_Byte : GDT_Float32, create);
    CSLDestroy(create);
    if (!out) {
        res.error = QStringLiteral("Не создать %1: %2").arg(QDir::toNativeSeparators(partial), QString::fromUtf8(CPLGetLastErrorMsg()));
        return res;
    }
    const double outGt[6] = {x0, r, 0, y1, 0, -r};
    GDALSetGeoTransform(out, const_cast<double *>(outGt));
    GDALSetProjection(out, dstWkt.toUtf8().constData());
    if (rgb) {
        GDALSetRasterColorInterpretation(GDALGetRasterBand(out, 4), GCI_AlphaBand);
    } else {
        GDALSetRasterNoDataValue(GDALGetRasterBand(out, 1), sources.front().hasNoData ? sources.front().noData : OutputNoData);
    }
    const float outNoData = rgb ? 0.0f : float(sources.front().hasNoData ? sources.front().noData : OutputNoData);

    const int tilesX = (res.width + Tile - 1) / Tile, tilesY = (res.height + Tile - 1) / Tile;
    std::vector<float> outValues;
    std::vector<unsigned char> outBytes;
    std::vector<float> readBuf;
    bool cancelled = false, failed = false;
    for (int ty = 0; ty < tilesY && !cancelled && !failed; ++ty)
        for (int tx = 0; tx < tilesX && !cancelled && !failed; ++tx) {
            const int c0 = tx * Tile, r0 = ty * Tile;
            const int tw = std::min(Tile, res.width - c0), th = std::min(Tile, res.height - r0);
            const double tileX0 = x0 + c0 * r, tileY1 = y1 - r0 * r;
            const double tileX1 = tileX0 + tw * r, tileY0 = tileY1 - th * r;
            // Each source's share of the tile.
            for (Source &s : sources) {
                s.inTile = false;
                const double sx0 = s.gt[0], sx1 = s.gt[0] + s.gt[1] * s.w, sy1 = s.gt[3], sy0 = s.gt[3] + s.gt[5] * s.h;
                const double ix0 = std::max(tileX0, sx0), ix1 = std::min(tileX1, sx1);
                const double iy0 = std::max(tileY0, sy0), iy1 = std::min(tileY1, sy1);
                if (ix1 <= ix0 || iy1 <= iy0) continue;
                // Output pixels wholly inside the source.
                const int oc0 = std::clamp(int(std::ceil((ix0 - tileX0) / r - 1e-6)), 0, tw);
                const int oc1 = std::clamp(int(std::floor((ix1 - tileX0) / r + 1e-6)), 0, tw);
                const int or0 = std::clamp(int(std::ceil((tileY1 - iy1) / r - 1e-6)), 0, th);
                const int or1 = std::clamp(int(std::floor((tileY1 - iy0) / r + 1e-6)), 0, th);
                if (oc1 <= oc0 || or1 <= or0) continue;
                const int bw = oc1 - oc0, bh = or1 - or0;
                double px0 = (tileX0 + oc0 * r - s.gt[0]) / s.gt[1], px1 = (tileX0 + oc1 * r - s.gt[0]) / s.gt[1];
                double py0 = (tileY1 - or0 * r - s.gt[3]) / s.gt[5], py1 = (tileY1 - or1 * r - s.gt[3]) / s.gt[5];
                px0 = std::clamp(px0, 0.0, double(s.w));
                px1 = std::clamp(px1, 0.0, double(s.w));
                py0 = std::clamp(py0, 0.0, double(s.h));
                py1 = std::clamp(py1, 0.0, double(s.h));
                if (px1 - px0 <= 1e-9 || py1 - py0 <= 1e-9) continue;
                const int nx0 = int(std::floor(px0)), ny0 = int(std::floor(py0));
                const int nxs = std::min(s.w, int(std::ceil(px1))) - nx0, nys = std::min(s.h, int(std::ceil(py1))) - ny0;
                if (nxs <= 0 || nys <= 0) continue;
                const int readBands = GDALGetRasterCount(s.view);
                const int used = rgb ? std::min(readBands, 4) : 1;
                std::vector<int> bandList(used);
                for (int b = 0; b < used; ++b) bandList[b] = b + 1;
                if (rgb && used == 4 && !s.alpha) bandList.resize(3);
                const int nb = int(bandList.size());
                readBuf.assign(size_t(bw) * bh * nb, 0.0f);
                GDALRasterIOExtraArg extra;
                INIT_RASTERIO_EXTRA_ARG(extra);
                extra.eResampleAlg = rgb ? GRIORA_Bilinear : GRIORA_NearestNeighbour;   // heights: never mixed with nodata
                extra.bFloatingPointWindowValidity = TRUE;
                extra.dfXOff = px0;
                extra.dfYOff = py0;
                extra.dfXSize = px1 - px0;
                extra.dfYSize = py1 - py0;
                if (GDALDatasetRasterIOEx(s.view, GF_Read, nx0, ny0, nxs, nys, readBuf.data(), bw, bh, GDT_Float32, nb, bandList.data(),
                                          sizeof(float), GSpacing(sizeof(float)) * bw, GSpacing(sizeof(float)) * bw * bh, &extra)
                    != CE_None) {
                    res.error = QStringLiteral("Не прочитать «%1»: %2").arg(s.name, QString::fromUtf8(CPLGetLastErrorMsg()));
                    failed = true;
                    break;
                }
                const size_t n = size_t(tw) * th;
                s.values.assign(n * size_t(rgb ? 3 : 1), 0.0f);
                s.valid.assign(n, 0);
                const size_t plane = size_t(bw) * bh;
                for (int y = 0; y < bh; ++y)
                    for (int x = 0; x < bw; ++x) {
                        const size_t i = size_t(y) * bw + x, o = size_t(or0 + y) * tw + (oc0 + x);
                        bool ok;
                        if (rgb) {
                            const float rr = readBuf[i], gg = readBuf[plane + i], bb = readBuf[2 * plane + i];
                            ok = s.alpha ? readBuf[3 * plane + i] >= 128.0f : (rr > 0 || gg > 0 || bb > 0);
                            s.values[o] = rr;
                            s.values[n + o] = gg;
                            s.values[2 * n + o] = bb;
                        } else {
                            const float v = readBuf[i];
                            ok = std::isfinite(v) && !(s.hasNoData && std::fabs(double(v) - s.noData) < 1e-6) && v > -1e30f;
                            s.values[o] = v;
                        }
                        s.valid[o] = ok ? 1 : 0;
                    }
                s.inTile = true;
            }
            if (failed) break;
            // Every pixel from the nearest source with data, blended with the second nearest near the seam.
            const size_t n = size_t(tw) * th;
            const int outBands = rgb ? 3 : 1;
            outValues.assign(n * outBands, outNoData);
            outBytes.assign(rgb ? n : 0, 0);
            for (int y = 0; y < th; ++y) {
                const double my = tileY1 - (y + 0.5) * r;
                for (int x = 0; x < tw; ++x) {
                    const double mx = tileX0 + (x + 0.5) * r;
                    const size_t o = size_t(y) * tw + x;
                    int best = -1, second = -1;
                    double bd = 1e300, sd = 1e300;
                    for (int k = 0; k < int(sources.size()); ++k) {
                        const Source &s = sources[size_t(k)];
                        if (!s.inTile || !s.valid[o]) continue;
                        const double d = std::hypot(mx - s.cx, my - s.cy);
                        if (d < bd) {
                            second = best;
                            sd = bd;
                            best = k;
                            bd = d;
                        } else if (d < sd) {
                            second = k;
                            sd = d;
                        }
                    }
                    if (best < 0) continue;
                    // dj - di grows twice as fast as the distance from the bisector.
                    double w = 1.0;
                    if (second >= 0) w = std::clamp(0.5 + (sd - bd) / (4.0 * blend), 0.5, 1.0);
                    const Source &a = sources[size_t(best)];
                    for (int b = 0; b < outBands; ++b) {
                        double v = a.values[size_t(b) * n + o];
                        if (w < 1.0) v = w * v + (1.0 - w) * sources[size_t(second)].values[size_t(b) * n + o];
                        outValues[size_t(b) * n + o] = float(v);
                    }
                    if (rgb) outBytes[o] = 255;
                }
            }
            if (rgb) {
                std::vector<unsigned char> bytes(n * 4);
                for (int b = 0; b < 3; ++b)
                    for (size_t i = 0; i < n; ++i)
                        bytes[size_t(b) * n + i] = outBytes[i] ? (unsigned char)std::clamp(int(std::lround(outValues[size_t(b) * n + i])), 0, 255) : 0;
                std::copy(outBytes.begin(), outBytes.end(), bytes.begin() + std::ptrdiff_t(3 * n));
                if (GDALDatasetRasterIO(out, GF_Write, c0, r0, tw, th, bytes.data(), tw, th, GDT_Byte, 4, nullptr, 0, 0, 0) != CE_None)
                    failed = true;
            } else if (GDALRasterIO(GDALGetRasterBand(out, 1), GF_Write, c0, r0, tw, th, outValues.data(), tw, th, GDT_Float32, 0, 0) != CE_None) {
                failed = true;
            }
            if (failed && res.error.isEmpty()) res.error = QStringLiteral("Не записать объединённый растр: %1").arg(QString::fromUtf8(CPLGetLastErrorMsg()));
            if (!progress(0.9 * double(ty * tilesX + tx + 1) / double(tilesX * tilesY))) cancelled = true;
        }
    if (!failed && !cancelled) {
        std::vector<int> levels;
        for (int f = 2; std::max(res.width, res.height) / f >= 256; f *= 2) levels.push_back(f);
        if (!levels.empty())
            GDALBuildOverviews(out, rgb ? "AVERAGE" : "NEAREST", int(levels.size()), levels.data(), 0, nullptr, nullptr, nullptr);
    }
    GDALClose(out);
    if (cancelled || failed) {
        QFile::remove(partial);
        if (cancelled && res.error.isEmpty()) res.error = QStringLiteral("Объединение отменено.");
        return res;
    }
    QFile::remove(output);
    if (!QFile::rename(partial, output)) {
        res.error = QStringLiteral("Не удалось переименовать %1 в %2").arg(QDir::toNativeSeparators(partial), QDir::toNativeSeparators(output));
        return res;
    }
    progress(1.0);
    res.seconds = timer.elapsed() / 1000.0;
    return res;
}

} // namespace mosaic
