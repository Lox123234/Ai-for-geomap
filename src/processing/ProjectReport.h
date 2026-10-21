#pragma once

#include "ProcessingTypes.h"
#include "SurveyLayout.h"

#include <QDateTime>
#include <QImage>
#include <QString>
#include <QVariantMap>
#include <QVector>

// GeoMap AI's own processing report (Обработка → Генерация отчёта): a PDF of one job — the
// survey scheme with the camera errors, the quality of the alignment, the cameras and the control
// points, previews of the orthophoto and the elevation model, the results and the parameters.
namespace projectreport {

struct Input
{
    QString appVersion, engine;              // "0.7.0", "OpenDroneMap 3.6.2"
    QString project, chunk;                  // names
    QString jobId, jobDirectory, profile;    // profile: its title
    QDateTime started, finished;
    QStringList arguments;
    QVariantMap quality;                     // IProcessingEngine::qualityReport
    QVector<surveylayout::PhotoIn> photos;
    QVariantMap accuracy;                    // SurveyControl::accuracy (control and check points)
    QVector<Artifact> outputs;
};

// Writes the report; "" or the reason.
QString write(const Input &input, const QString &pdfPath);
// The pages of the report as images (96 dpi × scale): a preview.
QVector<QImage> pages(const Input &input, double scale = 1.0);

// The parts of the report, for tests and other uses.
QImage renderScheme(const surveylayout::Layout &layout, const QSize &size);
// The elevation model reduced to `maxSide`, coloured by height with hillshade; min and max heights.
QImage elevationPreview(const QString &demPath, int maxSide, double *minZ = nullptr, double *maxZ = nullptr);

} // namespace projectreport
