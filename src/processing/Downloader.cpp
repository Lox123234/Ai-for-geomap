#include "Downloader.h"
#include <QDir>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

Downloader::Downloader(QObject *parent) : QObject(parent), m_network(new QNetworkAccessManager(this))
{
}

Downloader::~Downloader()
{
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
    }
    if (m_part.isOpen()) {
        m_part.close();
        m_part.remove();
    }
}

QByteArray Downloader::fileSha256(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) return {};
    return hash.result().toHex();
}

void Downloader::start(const QUrl &url, const QString &destination, const QByteArray &sha256Hex)
{
    if (m_reply) return;
    m_destination = destination;
    m_expected = sha256Hex.toLower();
    m_received = 0;
    m_cancelled = false;
    m_hash.reset();
    if (QFileInfo(destination).isFile() && fileSha256(destination) == m_expected) {
        const qint64 size = QFileInfo(destination).size();
        emit progress(size, size);
        emit finished(true, destination);
        return;
    }
    if (!QDir().mkpath(QFileInfo(destination).absolutePath())) {
        fail(QStringLiteral("Не удалось создать папку %1").arg(QDir::toNativeSeparators(QFileInfo(destination).absolutePath())));
        return;
    }
    m_part.setFileName(destination + QStringLiteral(".part"));
    if (!m_part.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail(QStringLiteral("Не удалось записать %1: %2").arg(QDir::toNativeSeparators(m_part.fileName()), m_part.errorString()));
        return;
    }
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("GeoMapAI"));
    request.setTransferTimeout(60000);   // no data for a minute: give up
    m_lastProgress.start();
    m_reply = m_network->get(request);
    connect(m_reply, &QNetworkReply::readyRead, this, &Downloader::onReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &Downloader::onFinished);
}

void Downloader::cancel()
{
    if (!m_reply) return;
    m_cancelled = true;
    m_reply->abort();   // finished() follows
}

void Downloader::onReadyRead()
{
    if (!m_reply) return;
    const QByteArray data = m_reply->readAll();
    if (data.isEmpty()) return;
    if (m_part.write(data) != data.size()) {
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
        fail(QStringLiteral("Ошибка записи на диск (нет места?): %1").arg(m_part.errorString()));
        return;
    }
    m_hash.addData(data);
    m_received += data.size();
    if (m_lastProgress.elapsed() >= 250) {
        m_lastProgress.restart();
        const QVariant length = m_reply->header(QNetworkRequest::ContentLengthHeader);
        emit progress(m_received, length.isValid() ? length.toLongLong() : -1);
    }
}

void Downloader::onFinished()
{
    QNetworkReply *reply = m_reply;
    if (!reply) return;
    onReadyRead();
    if (!m_reply) return;   // failed while writing
    m_reply = nullptr;
    reply->deleteLater();
    if (m_cancelled) {
        fail(QStringLiteral("Загрузка отменена."));
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        fail(QStringLiteral("Ошибка загрузки: %1").arg(reply->errorString()));
        return;
    }
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status != 0 && status != 200) {
        fail(QStringLiteral("Сервер ответил кодом %1").arg(status));
        return;
    }
    m_part.close();
    const QByteArray actual = m_hash.result().toHex();
    if (actual != m_expected) {
        m_part.remove();
        emit finished(false, QStringLiteral("Контрольная сумма не совпадает: файл повреждён или подменён "
                                            "(ожидалось %1, получено %2). Файл удалён.")
                                 .arg(QString::fromLatin1(m_expected.left(16)), QString::fromLatin1(actual.left(16))));
        return;
    }
    QFile::remove(m_destination);
    if (!m_part.rename(m_destination)) {
        fail(QStringLiteral("Не удалось сохранить %1").arg(QDir::toNativeSeparators(m_destination)));
        return;
    }
    emit progress(m_received, m_received);
    emit finished(true, m_destination);
}

void Downloader::fail(const QString &message)
{
    if (m_part.isOpen()) m_part.close();
    m_part.remove();
    emit finished(false, message);
}
