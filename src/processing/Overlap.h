#pragma once

#include <QString>
#include <QStringList>

// Image overlap: how many photos see each point of the ground (Обработка → Проверка перекрытия).
// After alignment the photos of a job are projected onto its surface model (or the median height
// of its sparse points); before alignment their footprints are estimated from EXIF and DJI XMP
// (35 mm equivalent focal length, height above the take-off point, heading) as nadir photos over
// flat ground. Writes a Byte GeoTIFF with the number of photos per cell (0 = none).
namespace overlap {

struct Result
{
    QString error, path;
    QString method;              // "job" (after alignment) or "exif" (estimate before it)
    QString job;
    int photos = 0;              // photos used
    int skipped = 0;             // photos without the data the method needs
    int width = 0, height = 0;
    double cell = 0;             // metres
    double area = 0;             // m² seen by at least one photo
    double median = 0;           // photos per point, median over the covered area
    double share2 = 0, share3 = 0, share5 = 0, share9 = 0;   // share of the covered area seen by ≥ 2, 3, 5, 9 photos
    QString note;                // assumptions of the estimate
    bool ok() const { return error.isEmpty(); }
};

// photoFolder: where the job's photos are when its images/ folder is gone (only their names matter).
Result fromJob(const QString &jobDirectory, const QString &outputPath, const QString &photoFolder = {});
Result fromPhotos(const QStringList &photoPaths, const QString &outputPath);

} // namespace overlap
