#pragma once

#include <QString>
#include <functional>

// Retouch of an orthophoto by a mask: the pixels where the photos recorded nothing usable (a
// slope seen almost edge-on by every photo, its few pixels stretched into streaks) are painted
// by the inpainting model from their surroundings. The result is a plausible picture, not a
// measurement; the caller keeps the original and records what was painted.
namespace ai {

class Inpainter;

struct RetouchResult
{
    int tiles = 0;           // model runs
    qint64 pixels = 0;       // pixels painted
    qint64 skipped = 0;      // masked pixels in parts too small to repaint (left as they were)
    QString error;
    bool ok() const { return error.isEmpty(); }
};

// Paints the pixels of `maskPath` (Byte raster on the grid of `rasterPath`, non-zero = repaint)
// into the RGB(A) GeoTIFF `rasterPath` in place. Parts smaller than `minPixels` are left; the
// rest, grown by 2 px, is painted tile by tile: 256 px in a 512 px window (the model at full
// resolution) where every stretched pixel not painted yet is hidden from the model, so it
// continues the real surroundings and the tiles painted before, not the streaks. `progress`
// (0…1) may cancel.
RetouchResult retouch(Inpainter &inpainter, const QString &rasterPath, const QString &maskPath, int minPixels,
                      const std::function<bool(double)> &progress = {});

// The mask a true orthophoto was built with, next to it: "<name>_stretch.tif"; empty if none.
QString stretchMaskFor(const QString &orthophotoPath);

} // namespace ai
