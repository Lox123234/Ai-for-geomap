#pragma once
#include <QString>
#include <QtNumeric>

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
    // For footprints before alignment (NaN / 0 when the file has none).
    double altitude = qQNaN();           // GPS altitude, metres
    double focalLength = qQNaN();        // mm
    int focal35 = 0;                     // 35 mm equivalent, mm
    double relativeAltitude = qQNaN();   // above the take-off point (DJI XMP)
    double yaw = qQNaN();                // degrees clockwise from north (DJI XMP: gimbal, else flight)
    double pitch = qQNaN();              // gimbal pitch, −90 = nadir (DJI XMP)
    // Flight data of the XMP packet (DJI drone-dji; the accuracies also Camera:GPS…Accuracy of
    // Pix4D-style cameras), as OpenDroneMap reads it; NaN / −1 when absent.
    double absoluteAltitude = qQNaN();   // the drone's own altitude, metres
    double gimbalYaw = qQNaN(), gimbalRoll = qQNaN();
    double flightYaw = qQNaN(), flightPitch = qQNaN(), flightRoll = qQNaN();   // the drone's attitude
    int rtkFlag = -1;                    // DJI RtkFlag: 0 no fix, 16 single point, 34 RTK float, 50 RTK fixed
    double accuracyXy = qQNaN();         // σ of the position in plan, metres (DJI RtkStdLat/Lon: the larger)
    double accuracyZ = qQNaN();          // σ of the height, metres (DJI RtkStdHgt)
};

// Lightweight, bounded EXIF reader: JPEG APP1 and TIFF IFD metadata.
// Does not decode image pixels or require external libraries.
class ExifReader
{
public:
    static ImageMetadata read(const QString &path);
};
