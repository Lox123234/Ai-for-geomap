#include "PhotoMasks.h"
#include "AiService.h"
#include "SamSegmenter.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QLocale>
#include <QPainter>
#include <QThreadPool>

PhotoMasks::PhotoMasks(ai::AiService *ai, QObject *parent) : QObject(parent), m_ai(ai) {}

void PhotoMasks::setProject(const QString &directory)
{
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
    const QString overlay = QDir(m_project).filePath(QStringLiteral("cache/masks/") + QFileInfo(photoFile).completeBaseName() + QStringLiteral("_overlay.png"));
    if (!QFileInfo::exists(overlay)) {
        QImageReader r(QDir(m_project).filePath(maskFile(photoFile)));
        writeOverlay(photoFile, r.read());
    }
    QUrl url = QUrl::fromLocalFile(overlay);
    url.setQuery(QStringLiteral("v=%1").arg(m_revision));
    return url;
}

void PhotoMasks::writeOverlay(const QString &photoFile, const QImage &maskIn) const
{
    if (maskIn.isNull()) return;
    // A small overlay is enough to show the excluded areas over a downscaled preview.
    const QImage mask = maskIn.convertToFormat(QImage::Format_Grayscale8).scaled(maskIn.size().boundedTo(QSize(1600, 1600)), Qt::KeepAspectRatio,
                                                                                  Qt::FastTransformation);
    QImage overlay(mask.size(), QImage::Format_ARGB32);
    for (int y = 0; y < mask.height(); ++y) {
        const uchar *m = mask.constScanLine(y);
        QRgb *o = reinterpret_cast<QRgb *>(overlay.scanLine(y));
        for (int x = 0; x < mask.width(); ++x) o[x] = m[x] < 128 ? qRgba(240, 60, 70, 140) : qRgba(0, 0, 0, 0);
    }
    const QString path = QDir(m_project).filePath(QStringLiteral("cache/masks/") + QFileInfo(photoFile).completeBaseName() + QStringLiteral("_overlay.png"));
    QDir().mkpath(QFileInfo(path).absolutePath());
    overlay.save(path, "PNG");
}

bool PhotoMasks::applyMask(const QString &photoFile, const QImage &objectIn, bool include, QString *error)
{
    const QString photoPath = QDir(m_project).filePath(photoFile), maskPath = QDir(m_project).filePath(maskFile(photoFile));
    QImageReader photo(photoPath);
    photo.setAutoTransform(true);
    QSize size = photo.size();
    if (photo.transformation() & QImageIOHandler::TransformationRotate90) size.transpose();
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
    for (int y = 0; y < size.height(); ++y) {
        uchar *m = mask.scanLine(y);
        const uchar *o = object.constScanLine(y);
        for (int x = 0; x < size.width(); ++x)
            if (o[x] >= 128) m[x] = include ? 255 : 0;
    }
    // A mask that keeps everything is no mask.
    bool any = false;
    for (int y = 0; y < size.height() && !any; ++y) {
        const uchar *m = mask.constScanLine(y);
        for (int x = 0; x < size.width() && !any; ++x) any = m[x] < 128;
    }
    if (!any) {
        QFile::remove(maskPath);
    } else if (!mask.save(maskPath, "PNG")) {
        if (error) *error = QStringLiteral("Не удалось сохранить маску %1").arg(QDir::toNativeSeparators(maskPath));
        return false;
    }
    if (any) writeOverlay(photoFile, mask);
    ++m_revision;
    emit changed();
    return true;
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
