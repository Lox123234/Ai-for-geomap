#pragma once

#include <QImage>
#include <QRect>
#include <QString>

// Editing of an RGB(A) raster in place: windows of bands 1–3 are read and written, the
// internal overviews of the edited window are updated. Used on the program's own editable
// copies (the user's files are never written).
namespace rasteredit {

// A copy of `source` for editing: tiled GeoTIFF (DEFLATE), same bands and georeferencing,
// internal overviews (average) down to ~256 px. Never overwrites an existing file.
bool makeEditableCopy(const QString &source, const QString &target, QString *error);

// Bands 1–3 of `window` (pixels of the file) as RGB888; `alpha` gets band 4 (or 255 where the
// window is valid when there is no alpha band) as Grayscale8 when not null. Only Byte rasters.
bool readRgb(const QString &path, const QRect &window, QImage *rgb, QImage *alpha, QString *error);
// Writes bands 1–3 of `window` and refreshes the window in every overview.
bool writeRgb(const QString &path, const QRect &window, const QImage &rgb, QString *error);

} // namespace rasteredit
