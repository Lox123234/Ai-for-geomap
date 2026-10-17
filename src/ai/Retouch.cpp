#include "Retouch.h"
#include "Inpainter.h"
#include "RasterEdit.h"

#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QRect>
#include <gdal.h>
#include <vector>

namespace ai {

QString stretchMaskFor(const QString &orthophotoPath)
{
    const QFileInfo fi(orthophotoPath);
    const QString mask = fi.dir().filePath(fi.completeBaseName() + QStringLiteral("_stretch.tif"));
    return QFileInfo::exists(mask) ? mask : QString();
}

RetouchResult retouch(Inpainter &inpainter, const QString &rasterPath, const QString &maskPath, int minPixels,
                      const std::function<bool(double)> &progress)
{
    RetouchResult r;
    // The mask, whole (one byte a pixel).
    int w = 0, h = 0;
    std::vector<uchar> mask;
    {
        GDALDatasetH m = GDALOpen(QDir::toNativeSeparators(maskPath).toUtf8().constData(), GA_ReadOnly);
        GDALDatasetH o = GDALOpen(QDir::toNativeSeparators(rasterPath).toUtf8().constData(), GA_ReadOnly);
        if (!m || !o) {
            if (m) GDALClose(m);
            if (o) GDALClose(o);
            r.error = QStringLiteral("Не открыть %1").arg(QDir::toNativeSeparators(m ? rasterPath : maskPath));
            return r;
        }
        w = GDALGetRasterXSize(m);
        h = GDALGetRasterYSize(m);
        const bool same = w == GDALGetRasterXSize(o) && h == GDALGetRasterYSize(o);
        GDALClose(o);
        if (!same) {
            GDALClose(m);
            r.error = QStringLiteral("Маска не совпадает по размеру с ортофотопланом");
            return r;
        }
        mask.resize(size_t(w) * h);
        const CPLErr e = GDALRasterIO(GDALGetRasterBand(m, 1), GF_Read, 0, 0, w, h, mask.data(), w, h, GDT_Byte, 0, 0);
        GDALClose(m);
        if (e != CE_None) {
            r.error = QStringLiteral("Маска не читается");
            return r;
        }
    }
    // Parts smaller than minPixels stay (single pixels are not worth a painted patch); the rest
    // grows by 2 px over the borders of the streaks.
    {
        std::vector<int> stack, members;
        std::vector<uchar> seen(mask.size(), 0);
        for (size_t start = 0; start < mask.size(); ++start) {
            if (!mask[start] || seen[start]) continue;
            members.clear();
            stack = {int(start)};
            seen[start] = 1;
            while (!stack.empty()) {
                const int p = stack.back();
                stack.pop_back();
                members.push_back(p);
                const int px = p % w, py = p / w;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = px + dx, ny = py + dy;
                        if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                        const size_t q = size_t(ny) * w + nx;
                        if (mask[q] && !seen[q]) {
                            seen[q] = 1;
                            stack.push_back(int(q));
                        }
                    }
            }
            if (int(members.size()) < minPixels) {
                for (int p : members) mask[size_t(p)] = 0;
                r.skipped += members.size();
            }
        }
        for (int pass = 0; pass < 2; ++pass) {
            std::vector<uchar> grown = mask;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    if (mask[size_t(y) * w + x]) continue;
                    const bool near = (x > 0 && mask[size_t(y) * w + x - 1]) || (x + 1 < w && mask[size_t(y) * w + x + 1])
                                      || (y > 0 && mask[size_t(y - 1) * w + x]) || (y + 1 < h && mask[size_t(y + 1) * w + x]);
                    if (near) grown[size_t(y) * w + x] = 1;
                }
            mask.swap(grown);
        }
    }
    // Tiles in reading order: 256 px painted, 128 px of context around (the model's 512).
    const int T = 256, C = 128, tilesX = (w + T - 1) / T, tilesY = (h + T - 1) / T;
    std::vector<int> todo;
    for (int ty = 0; ty < tilesY; ++ty)
        for (int tx = 0; tx < tilesX; ++tx) {
            bool any = false;
            for (int y = ty * T; y < qMin(h, ty * T + T) && !any; ++y)
                for (int x = tx * T; x < qMin(w, tx * T + T) && !any; ++x) any = mask[size_t(y) * w + x] != 0;
            if (any) todo.push_back(ty * tilesX + tx);
        }
    const int N = Inpainter::WindowSize;
    for (size_t k = 0; k < todo.size(); ++k) {
        if (progress && !progress(double(k) / todo.size())) {
            r.error = QStringLiteral("Отменено");
            return r;
        }
        const int tx = todo[k] % tilesX, ty = todo[k] / tilesX;
        const QRect tile(tx * T, ty * T, qMin(T, w - tx * T), qMin(T, h - ty * T));
        // The model's window: the tile with its context, moved inside the raster when it fits.
        QRect window(tile.left() - C, tile.top() - C, N, N);
        if (w >= N) window.moveLeft(qBound(0, window.left(), w - N));
        if (h >= N) window.moveTop(qBound(0, window.top(), h - N));
        const QRect inside = window.intersected(QRect(0, 0, w, h));
        QImage rgb, alpha;
        if (!rasteredit::readRgb(rasterPath, inside, &rgb, &alpha, &r.error)) return r;
        rgb = rgb.convertToFormat(QImage::Format_RGB888);
        // Edges repeated outside the raster; masked: every stretched pixel not painted yet (the
        // model must not continue streaks it sees) and nodata; painted: the tile's only.
        QImage crop(N, N, QImage::Format_RGB888), cropMask(N, N, QImage::Format_Grayscale8);
        qint64 here = 0;
        for (int y = 0; y < N; ++y) {
            const int iy = qBound(0, window.top() + y, h - 1) - inside.top();
            const int ry = window.top() + y;
            uchar *c = crop.scanLine(y), *m = cropMask.scanLine(y);
            const uchar *s = rgb.constScanLine(qBound(0, iy, inside.height() - 1)), *a = alpha.constScanLine(qBound(0, iy, inside.height() - 1));
            for (int x = 0; x < N; ++x) {
                const int ix = qBound(0, qBound(0, window.left() + x, w - 1) - inside.left(), inside.width() - 1);
                const int rx = window.left() + x;
                std::copy(s + ix * 3, s + ix * 3 + 3, c + x * 3);
                const bool in = rx >= 0 && ry >= 0 && rx < w && ry < h;
                m[x] = in && (!a[ix] || mask[size_t(ry) * w + rx]) ? 255 : 0;
                if (in && a[ix] && mask[size_t(ry) * w + rx] && tile.contains(rx, ry)) ++here;
            }
        }
        if (!here) continue;
        const Inpainter::Result filled = inpainter.inpaintWindow(crop, cropMask);
        if (!filled.ok()) {
            r.error = filled.error;
            return r;
        }
        // Back into the raster: the tile's stretched pixels; they are context for later tiles.
        QImage out = rgb;
        for (int y = tile.top(); y <= tile.bottom(); ++y) {
            uchar *o = out.scanLine(y - inside.top());
            const uchar *f = filled.image.constScanLine(y - window.top()), *a = alpha.constScanLine(y - inside.top());
            for (int x = tile.left(); x <= tile.right(); ++x) {
                const size_t i = size_t(y) * w + x;
                if (!mask[i] || !a[x - inside.left()]) continue;
                std::copy(f + (x - window.left()) * 3, f + (x - window.left()) * 3 + 3, o + (x - inside.left()) * 3);
                mask[i] = 0;
            }
        }
        if (!rasteredit::writeRgb(rasterPath, tile, out.copy(tile.translated(-inside.topLeft())), &r.error)) return r;
        ++r.tiles;
        r.pixels += here;
    }
    if (progress) progress(1.0);
    return r;
}

} // namespace ai
