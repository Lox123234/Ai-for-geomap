#pragma once

#include <QString>

// Layout of a project folder (see docs/PROJECT_FORMAT.md).
namespace ProjectFormat {

inline constexpr auto ManifestFile = "geomap.json";
inline constexpr auto LockFile = "geomap.lock";
inline constexpr auto PhotosDir = "photos";
// Derived data; safe to delete, rebuilt on demand.
inline constexpr auto ThumbnailsDir = "cache/thumbs";

// A manifest photo path must be "photos/<name>": no subfolders, separators,
// drive letters or traversal, so a crafted geomap.json cannot reach other files.
inline bool isSafePhotoFile(const QString &file)
{
    const QString prefix = QStringLiteral("photos/");
    if (!file.startsWith(prefix) || file.size() > 255) return false;
    const QString name = file.mid(prefix.size());
    return !name.isEmpty() && !name.contains(QLatin1Char('/')) && !name.contains(QLatin1Char('\\'))
        && !name.contains(QLatin1Char(':')) && !name.contains(QStringLiteral(".."));
}

} // namespace ProjectFormat
