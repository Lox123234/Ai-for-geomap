#pragma once

#include <QMutex>
#include <QQuickAsyncImageProvider>
#include <QThreadPool>

// Serves image://thumbs/<file> from the current project's thumbnail cache.
// Loads run on a private thread pool; only files inside <project>/photos are reachable.
class ThumbnailProvider final : public QQuickAsyncImageProvider
{
public:
    ThumbnailProvider();
    ~ThumbnailProvider() override;

    void setProjectDirectory(const QString &directory);
    QQuickImageResponse *requestImageResponse(const QString &id, const QSize &requestedSize) override;

private:
    mutable QMutex m_mutex;
    QString m_projectDirectory;
    QThreadPool m_pool;
};
