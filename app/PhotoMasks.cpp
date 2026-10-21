#include "PhotoMasks.h"
#include "AiService.h"
#include "SamSegmenter.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QLocale>
#include <QPainter>
#include <QPointer>
#include <QThreadPool>
#include <QtMath>
#include <cstring>

namespace {
QString overlayPath(const QString &project, const QString &photoFile)
{
    return QDir(project).filePath(QStringLiteral("cache/masks/") + QFileInfo(photoFile).completeBaseName() + QStringLiteral("_overlay.png"));
}

// Red where the photo is excluded, transparent elsewhere; small enough for the preview.
void writeOverlay(const QString &project, const QString &photoFile, const QImage &maskIn)
{
    if (maskIn.isNull()) return;
    const QImage mask = maskIn.convertToFormat(QImage::Format_Grayscale8).scaled(maskIn.size().boundedTo(QSize(1600, 1600)), Qt::KeepAspectRatio,
                                                                                  Qt::FastTransformation);
    QImage overlay(mask.size(), QImage::Format_ARGB32);
    for (int y = 0; y < mask.height(); ++y) {
        const uchar *m = mask.constScanLine(y);
        QRgb *o = reinterpret_cast<QRgb *>(overlay.scanLine(y));
        for (int x = 0; x < mask.width(); ++x) o[x] = m[x] < 128 ? qRgba(240, 60, 70, 140) : qRgba(0, 0, 0, 0);
    }
    const QString path = overlayPath(project, photoFile);
    QDir().mkpath(QFileInfo(path).absolutePath());
    overlay.save(path, "PNG");
}

// The photo's size as displayed (EXIF orientation applied); invalid when it does not read.
QSize displayedSize(const QString &photoPath)
{
    QImageReader photo(photoPath);
    photo.setAutoTransform(true);
    QSize size = photo.size();
    if (photo.transformation() & QImageIOHandler::TransformationRotate90) size.transpose();
    return size;
}

// `object` (255 = the area) into the photo's mask file: excluded, or given back with `include`.
// A mask that keeps everything is removed. `written`: the mask now (null when none).
bool combine(const QString &project, const QString &photoFile, const QImage &objectIn, bool include, QImage *written, QString *error)
{
    const QString maskPath = QDir(project).filePath(PhotoMasks::maskFile(photoFile));
    const QSize size = displayedSize(QDir(project).filePath(photoFile));
    if (!size.isValid()) {
        if (error) *error = QStringLiteral("Снимок не читается");
        return false;
    }
    QImage mask;
    if (QFileInfo::exists(maskPath)) mask = QImage(maskPath).convertToFormat(QImage::Format_Grayscale8);
    if (mask.size() != size) {
        mask = QImage(size, QImage::Format_Grayscale8);
        mask.fill(255);
    }
    const QImage object = objectIn.convertToFormat(QImage::Format_Grayscale8).scaled(size, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    bool any = false;
    for (int y = 0; y < size.height(); ++y) {
        uchar *m = mask.scanLine(y);
        const uchar *o = object.constScanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            if (o[x] >= 128) m[x] = include ? 255 : 0;
            any = any || m[x] < 128;
        }
    }
    if (!any) {
        QFile::remove(maskPath);
        QFile::remove(overlayPath(project, photoFile));
        *written = QImage();
        return true;
    }
    if (!mask.save(maskPath, "PNG")) {
        if (error) *error = QStringLiteral("Не удалось сохранить маску %1").arg(QDir::toNativeSeparators(maskPath));
        return false;
    }
    *written = mask;
    return true;
}
} // namespace

PhotoMasks::PhotoMasks(ai::AiService *ai, QObject *parent) : QObject(parent), m_ai(ai) {}

PhotoMasks::~PhotoMasks()
{
    if (m_copyCancel) m_copyCancel->store(true);
}

void PhotoMasks::setProject(const QString &directory)
{
    if (m_copyCancel) m_copyCancel->store(true);   // the copy of a mask to many photos stops
    m_copyCancel.reset();
    m_busy = false;
    m_project = directory.isEmpty() ? QString() : QDir(directory).absolutePath();
    m_encodedPhoto.clear();
    ++m_generation;
    m_status.clear();
    ++m_revision;
    emit changed();
}

QString PhotoMasks::maskFile(const QString &photoFile)
{
    const QFileInfo fi(photoFile);
    const QString dir = fi.path();
    return (dir.isEmpty() || dir == QLatin1String(".") ? QString() : dir + QLatin1Char('/')) + fi.completeBaseName() + QStringLiteral("_mask.png");
}

bool PhotoMasks::hasMask(const QString &photoFile) const
{
    return !m_project.isEmpty() && !photoFile.isEmpty() && QFileInfo::exists(QDir(m_project).filePath(maskFile(photoFile)));
}

int PhotoMasks::maskCount() const
{
    if (m_project.isEmpty()) return 0;
    return int(QDir(QDir(m_project).filePath(QStringLiteral("photos"))).entryList({QStringLiteral("*_mask.png")}, QDir::Files).size());
}

QUrl PhotoMasks::overlayUrl(const QString &photoFile) const
{
    if (!hasMask(photoFile)) return {};
    const QString overlay = overlayPath(m_project, photoFile);
    if (!QFileInfo::exists(overlay)) {
        QImageReader r(QDir(m_project).filePath(maskFile(photoFile)));
        writeOverlay(m_project, photoFile, r.read());
    }
    QUrl url = QUrl::fromLocalFile(overlay);
    url.setQuery(QStringLiteral("v=%1").arg(m_revision));
    return url;
}

bool PhotoMasks::applyMask(const QString &photoFile, const QImage &objectIn, bool include, QString *error)
{
    QImage mask;
    if (!combine(m_project, photoFile, objectIn, include, &mask, error)) return false;
    if (!mask.isNull()) writeOverlay(m_project, photoFile, mask);
    ++m_revision;
    emit changed();
    return true;
}

QString PhotoMasks::maskRect(const QString &photoFile, double x0, double y0, double x1, double y1, bool include)
{
    if (m_project.isEmpty() || photoFile.isEmpty()) return QStringLiteral("Выберите снимок.");
    if (m_busy) return QStringLiteral("Подождите — маска ещё меняется.");
    const QRectF r = QRectF(QPointF(qBound(0.0, qMin(x0, x1), 1.0), qBound(0.0, qMin(y0, y1), 1.0)),
                            QPointF(qBound(0.0, qMax(x0, x1), 1.0), qBound(0.0, qMax(y0, y1), 1.0)));
    if (r.width() <= 0 || r.height() <= 0) return QStringLiteral("Рамка вне снимка.");
    const QSize size = displayedSize(QDir(m_project).filePath(photoFile));
    if (!size.isValid()) return QStringLiteral("Снимок не читается");
    QImage object(size, QImage::Format_Grayscale8);
    object.fill(0);
    const QRect pixels(QPoint(qFloor(r.left() * size.width()), qFloor(r.top() * size.height())),
                       QPoint(qCeil(r.right() * size.width()) - 1, qCeil(r.bottom() * size.height()) - 1));
    for (int y = pixels.top(); y <= pixels.bottom(); ++y) memset(object.scanLine(y) + pixels.left(), 255, size_t(pixels.width()));
    QString error;
    if (!applyMask(photoFile, object, include, &error)) return error;
    const QLocale ru(QLocale::Russian);
    m_status = QStringLiteral("%1 %2 % снимка").arg(include ? QStringLiteral("Возвращено в обработку:") : QStringLiteral("Исключено из обработки:"),
                                                     ru.toString(100.0 * r.width() * r.height(), 'f', 1));
    emit changed();
    return {};
}

QString PhotoMasks::copyToPhotos(const QString &photoFile, const QStringList &photoFiles)
{
    if (m_project.isEmpty() || photoFile.isEmpty()) return QStringLiteral("Выберите снимок.");
    if (m_busy) return QStringLiteral("Подождите — маска ещё меняется.");
    if (!hasMask(photoFile)) return QStringLiteral("У снимка нет маски.");
    QStringList targets = photoFiles;
    targets.removeAll(photoFile);
    if (targets.isEmpty()) return QStringLiteral("Других снимков нет.");
    const QImage source = QImage(QDir(m_project).filePath(maskFile(photoFile))).convertToFormat(QImage::Format_Grayscale8);
    const QSize size = displayedSize(QDir(m_project).filePath(photoFile));
    if (source.isNull() || source.size() != size) return QStringLiteral("Маска снимка не читается.");
    // The excluded areas as the area to exclude everywhere else.
    QImage object(size, QImage::Format_Grayscale8);
    for (int y = 0; y < size.height(); ++y) {
        const uchar *m = source.constScanLine(y);
        uchar *o = object.scanLine(y);
        for (int x = 0; x < size.width(); ++x) o[x] = m[x] < 128 ? 255 : 0;
    }
    const int generation = ++m_generation;
    m_busy = true;
    m_status = QStringLiteral("Маска на снимки: 0 из %1…").arg(targets.size());
    emit changed();
    const QString project = m_project;
    if (m_copyCancel) m_copyCancel->store(true);
    const auto cancel = m_copyCancel = std::make_shared<std::atomic_bool>(false);
    QPointer<PhotoMasks> self(this);
    QThreadPool::globalInstance()->start([self, project, targets, object, size, generation, cancel] {
        int done = 0, otherSize = 0, failed = 0;
        QString lastError;
        for (int i = 0; i < targets.size(); ++i) {
            if (cancel->load()) return;   // another project, or the program closes
            if (displayedSize(QDir(project).filePath(targets[i])) != size) {
                ++otherSize;
            } else {
                QImage mask;
                QString error;
                if (combine(project, targets[i], object, false, &mask, &error)) {
                    ++done;
                    if (!mask.isNull()) writeOverlay(project, targets[i], mask);
                } else {
                    ++failed;
                    lastError = error;
                }
            }
            if ((i + 1) % 10 == 0)
                QMetaObject::invokeMethod(qApp, [self, generation, i, total = targets.size()] {
                    if (!self || self->m_generation != generation) return;
                    self->m_status = QStringLiteral("Маска на снимки: %1 из %2…").arg(i + 1).arg(total);
                    emit self->changed();
                }, Qt::QueuedConnection);
        }
        QMetaObject::invokeMethod(qApp, [self, generation, done, otherSize, failed, lastError] {
            if (!self || self->m_generation != generation) return;
            self->m_busy = false;
            self->m_copyCancel.reset();
            ++self->m_revision;
            QString message = QStringLiteral("Маска добавлена к %1 сн.").arg(done);
            if (otherSize > 0) message += QStringLiteral(" · другой размер: %1").arg(otherSize);
            if (failed > 0) message += QStringLiteral(" · ошибок: %1 (%2)").arg(failed).arg(lastError);
            self->m_status = message;
            emit self->changed();
            emit self->finished(failed == 0 && done > 0, message);
        }, Qt::QueuedConnection);
    });
    return {};
}

QString PhotoMasks::segmentAt(const QString &photoFile, double nx, double ny, bool include)
{
    if (m_project.isEmpty() || photoFile.isEmpty()) return QStringLiteral("Выберите снимок.");
    if (m_busy) return QStringLiteral("ИИ ещё работает.");
    if (!m_ai) return QStringLiteral("Локальный AI не подключён.");
    const QString problem = m_ai->segmentationProblem();
    if (!problem.isEmpty()) return problem;
    if (nx < 0 || ny < 0 || nx > 1 || ny > 1) return QStringLiteral("Щелчок вне снимка.");
    const ai::ModelCard card = m_ai->segmentationCard();
    const QString photoPath = QDir(m_project).filePath(photoFile);
    const bool encode = m_encodedPhoto != photoPath;
    const int generation = ++m_generation;
    m_busy = true;
    m_status = encode ? QStringLiteral("ИИ: анализ снимка…") : QStringLiteral("ИИ: выделение…");
    emit changed();
    QThreadPool::globalInstance()->start([this, card, photoPath, photoFile, nx, ny, include, encode, generation] {
        QString error;
        QImage objectMask;
        float score = 0;
        bool encoded = false;
        QElapsedTimer timer;
        timer.start();
        auto segmenter = ai::cachedSegmenter(card, &error);
        QSize size;
        std::unique_lock<std::mutex> lock;   // shared with the map's AI selection
        if (segmenter) lock = std::unique_lock<std::mutex>(segmenter->mutex());
        const QString key = QStringLiteral("photo|") + photoPath;
        if (segmenter && segmenter->imageKey() == key) encoded = true;
        else if (segmenter) {
            QImageReader r(photoPath);
            r.setAutoTransform(true);
            // SAM sees at most 1024 px: decoding a quarter of a 12 MP photo is enough and fast.
            const QSize full = r.size();
            if (full.isValid() && qMax(full.width(), full.height()) > 2048) r.setScaledSize(full / 2);
            const QImage image = r.read();
            if (image.isNull()) error = QStringLiteral("Снимок не читается");
            else encoded = segmenter->setImage(image, &error, key);
            if (!encoded) segmenter.reset();
        }
        if (segmenter) {
            size = segmenter->imageSize();
            const ai::SamSegmenter::Result res = segmenter->segment({QPointF(nx * size.width(), ny * size.height())}, {1});
            if (res.ok()) {
                objectMask = res.mask;
                score = res.score;
            } else {
                error = res.error;
            }
        }
        const double seconds = timer.elapsed() / 1000.0;
        QMetaObject::invokeMethod(this, [this, error, objectMask, photoFile, photoPath, include, encode, encoded, generation, score, seconds] {
            m_busy = false;
            if (encode) m_encodedPhoto = encoded ? photoPath : QString();
            if (generation != m_generation) {
                m_status.clear();
                emit changed();
                return;
            }
            QString message = error;
            bool ok = error.isEmpty() && !objectMask.isNull();
            if (ok) ok = applyMask(photoFile, objectMask, include, &message);
            if (ok) {
                qint64 pixels = 0;
                for (int y = 0; y < objectMask.height(); ++y)
                    for (int x = 0; x < objectMask.width(); ++x) pixels += objectMask.constScanLine(y)[x] >= 128;
                const double share = 100.0 * pixels / qMax<qint64>(1, qint64(objectMask.width()) * objectMask.height());
                message = QStringLiteral("%1 %2 % снимка (оценка модели %3, %4 с)")
                              .arg(include ? QStringLiteral("Возвращено в обработку:") : QStringLiteral("Исключено из обработки:"),
                                   QLocale(QLocale::Russian).toString(share, 'f', 1), QLocale(QLocale::Russian).toString(score, 'f', 2),
                                   QLocale(QLocale::Russian).toString(seconds, 'f', 1));
            }
            m_status = ok ? message : QStringLiteral("ИИ: %1").arg(message);
            emit changed();
            emit finished(ok, m_status);
        });
    });
    return {};
}

void PhotoMasks::clearMask(const QString &photoFile)
{
    if (m_project.isEmpty()) return;
    QFile::remove(QDir(m_project).filePath(maskFile(photoFile)));
    QFile::remove(QDir(m_project).filePath(QStringLiteral("cache/masks/") + QFileInfo(photoFile).completeBaseName() + QStringLiteral("_overlay.png")));
    m_status = QStringLiteral("Маска снята: снимок используется целиком");
    ++m_revision;
    emit changed();
}
