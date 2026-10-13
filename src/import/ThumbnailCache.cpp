#include "ThumbnailCache.h"
#include "ProjectFormat.h"
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QSaveFile>

namespace {
void setError(QString *error, const QString &message)
{
    if (error) *error = message;
}
} // namespace

QString ThumbnailCache::thumbnailPath(const QString &projectDir, const QString &photoFile)
{
    if (!ProjectFormat::isSafePhotoFile(photoFile)) return {};
    // "<id>.tif" -> "<id>_tif.jpg": unique per photo file, always a JPEG.
    const QString name = QFileInfo(photoFile).fileName().replace(QLatin1Char('.'), QLatin1Char('_'));
    return QDir(projectDir).filePath(QStringLiteral("%1/%2.jpg").arg(ProjectFormat::ThumbnailsDir, name));
}

QImage ThumbnailCache::load(const QString &projectDir, const QString &photoFile, QString *error)
{
    const QString cached = thumbnailPath(projectDir, photoFile);
    if (cached.isEmpty()) {
        setError(error, QStringLiteral("Unsafe photo path: %1").arg(photoFile));
        return {};
    }
    const QFileInfo source(QDir(projectDir).filePath(photoFile));
    if (!source.isFile()) {
        setError(error, QStringLiteral("Photo not found: %1").arg(photoFile));
        return {};
    }
    const QFileInfo cache(cached);
    if (cache.isFile() && cache.lastModified() >= source.lastModified()) {
        const QImage image(cached);
        if (!image.isNull()) return image;
    }

    QImageReader reader(source.filePath());
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    // Scaled decoding: JPEG decodes directly at a fraction of the full resolution.
    if (size.isValid() && (size.width() > MaxSide || size.height() > MaxSide))
        reader.setScaledSize(size.scaled(MaxSide, MaxSide, Qt::KeepAspectRatio));
    QImage image = reader.read();
    if (image.isNull()) {
        setError(error, reader.errorString());
        return {};
    }
    if (image.width() > MaxSide || image.height() > MaxSide)
        image = image.scaled(MaxSide, MaxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    if (QDir().mkpath(cache.absolutePath())) {
        QSaveFile file(cached);
        if (file.open(QIODevice::WriteOnly) && image.save(&file, "JPG", 85)) file.commit();
        else file.cancelWriting();
    }
    return image;
}
