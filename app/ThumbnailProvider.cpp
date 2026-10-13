#include "ThumbnailProvider.h"
#include "ThumbnailCache.h"
#include <QRunnable>
#include <QThread>
#include <atomic>
#include <memory>

namespace {
// Runs on the pool and reports through a queued signal, so a response the engine
// already deleted is simply disconnected (pattern of Qt's imageresponseprovider example).
class ThumbnailJob final : public QObject, public QRunnable
{
    Q_OBJECT
public:
    ThumbnailJob(QString projectDirectory, QString photoFile, QSize requestedSize,
                 std::shared_ptr<std::atomic_bool> cancelled)
        : m_projectDirectory(std::move(projectDirectory)), m_photoFile(std::move(photoFile)),
          m_requestedSize(requestedSize), m_cancelled(std::move(cancelled))
    {
    }

    void run() override
    {
        QImage image;
        QString error;
        if (*m_cancelled) {
            error = QStringLiteral("Cancelled");
        } else if (m_projectDirectory.isEmpty()) {
            error = QStringLiteral("No project is open");
        } else {
            image = ThumbnailCache::load(m_projectDirectory, m_photoFile, &error);
            // The delegates crop (PreserveAspectCrop): requestedSize is the area to cover.
            const QSize cover = m_requestedSize;
            if (!image.isNull() && cover.width() > 0 && cover.height() > 0
                && image.width() > cover.width() && image.height() > cover.height())
                image = image.scaled(cover, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        }
        emit done(image, error);
    }

signals:
    void done(const QImage &image, const QString &error);

private:
    const QString m_projectDirectory;
    const QString m_photoFile;
    const QSize m_requestedSize;
    const std::shared_ptr<std::atomic_bool> m_cancelled;
};

class ThumbnailResponse final : public QQuickImageResponse
{
public:
    ThumbnailResponse(const QString &projectDirectory, const QString &photoFile,
                      const QSize &requestedSize, QThreadPool *pool)
    {
        auto *job = new ThumbnailJob(projectDirectory, photoFile, requestedSize, m_cancelled);
        connect(job, &ThumbnailJob::done, this, [this](const QImage &image, const QString &error) {
            m_image = image;
            m_error = error;
            emit finished();
        });
        pool->start(job);
    }

    QQuickTextureFactory *textureFactory() const override
    {
        return QQuickTextureFactory::textureFactoryForImage(m_image);
    }
    QString errorString() const override { return m_error; }
    void cancel() override { *m_cancelled = true; }   // e.g. the delegate scrolled away

private:
    const std::shared_ptr<std::atomic_bool> m_cancelled = std::make_shared<std::atomic_bool>(false);
    QImage m_image;
    QString m_error;
};
} // namespace

ThumbnailProvider::ThumbnailProvider()
{
    // Leave cores for the import worker and the render thread.
    m_pool.setMaxThreadCount(qMax(2, QThread::idealThreadCount() / 2));
}

ThumbnailProvider::~ThumbnailProvider()
{
    m_pool.waitForDone();
}

void ThumbnailProvider::setProjectDirectory(const QString &directory)
{
    QMutexLocker lock(&m_mutex);
    m_projectDirectory = directory;
}

QQuickImageResponse *ThumbnailProvider::requestImageResponse(const QString &id, const QSize &requestedSize)
{
    QString directory;
    {
        QMutexLocker lock(&m_mutex);
        directory = m_projectDirectory;
    }
    // ThumbnailCache rejects names that would leave photos/ (separators, "..", drives).
    return new ThumbnailResponse(directory, QStringLiteral("photos/") + id, requestedSize, &m_pool);
}

#include "ThumbnailProvider.moc"
