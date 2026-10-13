#pragma once
#include <QString>

struct ImageMetadata
{
    bool valid = false;
    int width = 0;
    int height = 0;
    QString cameraMake;
    QString cameraModel;
    QString dateTime;
    bool hasGps = false;
    double latitude = 0;
    double longitude = 0;
};

// Lightweight, bounded EXIF reader: JPEG APP1 and TIFF IFD metadata.
// Does not decode image pixels or require external libraries.
class ExifReader
{
public:
    static ImageMetadata read(const QString &path);
};
