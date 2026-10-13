#pragma once

#include <QImage>
#include <QString>

// Small JPEG previews of project photos in <project>/cache/thumbs.
// Thread-safe (no shared state); used by the import worker and the QML image provider.
namespace ThumbnailCache {

// Longest side of a cached preview: sharp in the 156x108 list cells at 200 % DPI.
constexpr int MaxSide = 320;

// Cache file for a manifest path such as "photos/<id>.jpg"; empty if the path is unsafe.
QString thumbnailPath(const QString &projectDir, const QString &photoFile);

// Returns the cached preview, creating it from the project copy when it is missing,
// older than the photo or unreadable. A failed cache write still returns the image.
QImage load(const QString &projectDir, const QString &photoFile, QString *error = nullptr);

} // namespace ThumbnailCache
