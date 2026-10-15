#include "ResultExporter.h"

#include "Crs.h"
#include "ProcessingService.h"
#include "ProcessingTypes.h"
#include "ResultExport.h"

#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QRegularExpression>

namespace {
QString kindTitle(const QString &kind, const QString &file)
{
    if (kind == QLatin1String("orthophoto")) return QStringLiteral("Ортофотоплан");
    if (kind == QLatin1String("dsm")) return QStringLiteral("Цифровая модель поверхности (DSM)");
    if (kind == QLatin1String("dtm")) return QStringLiteral("Цифровая модель рельефа (DTM)");
    if (kind == QLatin1String("pointcloud")) return file.endsWith(QLatin1String(".las")) ? QStringLiteral("Облако точек (LAS)") : QStringLiteral("Облако точек (LAZ)");
    if (kind == QLatin1String("mesh")) return file.contains(QLatin1String("25d")) ? QStringLiteral("2.5D-модель с текстурами") : QStringLiteral("3D-модель с текстурами");
    if (kind == QLatin1String("report")) return QStringLiteral("Отчёт обработки (PDF)");
    return kind;
}
bool isRaster(const QString &file) { return file.endsWith(QLatin1String(".tif"), Qt::CaseInsensitive); }
} // namespace

ResultExporter::ResultExporter(ProcessingService *processing, QObject *parent) : QObject(parent), m_processing(processing)
{
    m_pool.setMaxThreadCount(1);
}

ResultExporter::~ResultExporter()
{
    if (m_cancel) *m_cancel = true;
    m_pool.waitForDone();
}

QVariantList ResultExporter::results(const QString &jobId) const
{
    QVariantList list;
    if (!m_processing) return list;
    const ProcessingJob job = m_processing->job(jobId);
    for (const Artifact &a : job.outputs)
        list << QVariantMap{{"file", a.file}, {"kind", a.kind}, {"title", kindTitle(a.kind, a.file)}, {"details", a.details},
                            {"bytes", double(a.bytes)}, {"raster", isRaster(a.file)},
                            {"size", QLocale(QLocale::Russian).formattedDataSize(a.bytes, 1, QLocale::DataSizeTraditionalFormat)}};
    return list;
}

QString ResultExporter::latestJobWithResults() const
{
    if (!m_processing) return {};
    for (const ProcessingJob &j : m_processing->jobs())   // newest first
        if (!j.outputs.isEmpty()) return j.id;
    return {};
}

QString ResultExporter::start(const QString &jobId, const QStringList &files, const QUrl &folder, const QVariantMap &options)
{
    if (m_busy) return QStringLiteral("Экспорт уже идёт.");
    if (!m_processing) return QStringLiteral("Нет проекта.");
    const ProcessingJob job = m_processing->job(jobId);
    if (job.id.isEmpty()) return QStringLiteral("Задание не найдено.");
    if (files.isEmpty()) return QStringLiteral("Выберите результаты для экспорта.");
    const QString target = folder.isLocalFile() ? folder.toLocalFile() : folder.toString();
    if (target.isEmpty() || !QDir().mkpath(target)) return QStringLiteral("Папка недоступна: %1").arg(QDir::toNativeSeparators(target));
    const QString jobDir = m_processing->jobDirectory(jobId);
    if (QDir::cleanPath(QFileInfo(target).absoluteFilePath()).startsWith(QDir::cleanPath(QFileInfo(jobDir).absoluteFilePath())))
        return QStringLiteral("Выберите папку вне папки задания.");
    exporting::RasterOptions raster;
    const QString crsText = options.value(QStringLiteral("crs")).toString().trimmed();
    if (!crsText.isEmpty()) {
        QString error;
        raster.crs = Crs::fromUserInput(crsText, &error);
        if (!raster.crs.isValid()) return QStringLiteral("Система координат не распознана: %1").arg(error);
    }
    raster.cog = options.value(QStringLiteral("cog")).toBool();
    raster.resolution = options.value(QStringLiteral("resolution")).toDouble();
    if (raster.resolution < 0) return QStringLiteral("Размер ячейки должен быть больше нуля.");
    QStringList known;
    for (const Artifact &a : job.outputs) known << a.file;
    for (const QString &f : files)
        if (!known.contains(f)) return QStringLiteral("Нет такого результата: %1").arg(f);

    // Name part of the CRS for reprojected rasters: "epsg4326".
    QString crsTag;
    if (raster.crs.isValid()) {
        const QRegularExpressionMatch m = QRegularExpression(QStringLiteral("EPSG:(\\d+)")).match(raster.crs.label());
        crsTag = m.hasMatch() ? QStringLiteral("-epsg") + m.captured(1) : QStringLiteral("-reprojected");
    }
    m_cancel = std::make_shared<std::atomic_bool>(false);
    m_busy = true;
    m_status = QStringLiteral("Экспорт…");
    emit busyChanged();
    emit statusChanged();
    auto cancel = m_cancel;
    m_pool.start([this, jobId, jobDir, files, target, raster, crsTag, cancel] {
        QStringList written, problems;
        qint64 bytes = 0;
        int n = 0;
        for (const QString &file : files) {
            if (*cancel) break;
            ++n;
            const QString source = QDir(jobDir).filePath(file);
            const QString base = QFileInfo(file).completeBaseName(), suffix = QFileInfo(file).suffix();
            QMetaObject::invokeMethod(this, [this, n, total = files.size(), file] {
                m_status = QStringLiteral("Экспорт %1 из %2: %3").arg(n).arg(total).arg(QFileInfo(file).fileName());
                emit statusChanged();
            }, Qt::QueuedConnection);
            exporting::Result r;
            if (isRaster(file)) {
                const QString name = QStringLiteral("%1-%2%3%4.tif").arg(jobId, base, crsTag, raster.cog ? QStringLiteral("-cog") : QString());
                r = exporting::exportRaster(source, exporting::uniqueTarget(target, name), raster, cancel.get());
            } else if (suffix.compare(QLatin1String("obj"), Qt::CaseInsensitive) == 0) {
                // The model keeps its folder: OBJ, MTL and textures side by side.
                const QString folderName = QStringLiteral("%1-%2").arg(jobId, QFileInfo(file).dir().dirName());
                r = exporting::exportObj(source, exporting::uniqueTarget(target, folderName));
            } else {
                r = exporting::copyFile(source, exporting::uniqueTarget(target, QStringLiteral("%1-%2.%3").arg(jobId, base, suffix)));
            }
            if (r.ok()) {
                written << r.path;
                bytes += r.bytes;
            } else {
                problems << QStringLiteral("%1: %2").arg(QFileInfo(file).fileName(), r.error);
            }
        }
        const bool cancelled = *cancel;
        QMetaObject::invokeMethod(this, [this, written, problems, bytes, target, cancelled] {
            m_busy = false;
            m_status.clear();
            emit busyChanged();
            emit statusChanged();
            const QString size = QLocale(QLocale::Russian).formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
            QString message = cancelled ? QStringLiteral("Экспорт отменён. ") : QString();
            message += QStringLiteral("Сохранено %1 (%2) в %3.").arg(written.size()).arg(size, QDir::toNativeSeparators(target));
            if (!problems.isEmpty()) message += QStringLiteral(" Ошибки: ") + problems.join(QStringLiteral("; "));
            emit finished(problems.isEmpty() && !cancelled, message, written);
        }, Qt::QueuedConnection);
    });
    return {};
}

void ResultExporter::cancel()
{
    if (m_cancel) *m_cancel = true;
}
