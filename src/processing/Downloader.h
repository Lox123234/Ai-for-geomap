#pragma once

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QUrl>

class QNetworkAccessManager;

// Downloads one file and accepts it only with the expected SHA-256. Data goes to
// "<destination>.part" and is renamed after the check, so a broken or cancelled download never
// looks complete. A destination that already has the expected hash is reused.
class Downloader final : public QObject
{
    Q_OBJECT
public:
    explicit Downloader(QObject *parent = nullptr);
    ~Downloader() override;

    void start(const QUrl &url, const QString &destination, const QByteArray &sha256Hex);
    void cancel();
    bool isRunning() const { return m_reply != nullptr; }
    static QByteArray fileSha256(const QString &path);   // lower-case hex, empty if unreadable

signals:
    void progress(qint64 received, qint64 total);   // total < 0 when unknown
    void finished(bool ok, const QString &message);  // destination path, or the reason

private:
    void onReadyRead();
    void onFinished();
    void fail(const QString &message);

    QNetworkAccessManager *m_network;
    QPointer<QNetworkReply> m_reply;
    QFile m_part;
    QCryptographicHash m_hash{QCryptographicHash::Sha256};
    QString m_destination;
    QByteArray m_expected;
    qint64 m_received = 0;
    bool m_cancelled = false;
    QElapsedTimer m_lastProgress;
};
