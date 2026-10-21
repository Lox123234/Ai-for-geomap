#include "PhotoQualityService.h"
#include "ChunkService.h"
#include "ImageQuality.h"
#include "PhotoModel.h"
#include "ProjectManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QPointer>
#include <QSaveFile>
#include <QSet>
#include <QThread>
#include <QUrl>
#include <algorithm>

namespace {
const QString QualityFile = QStringLiteral("quality.json");
}

PhotoQualityService::PhotoQualityService(ProjectManager *projects, ChunkService *chunks, QObject *parent)
    : QObject(parent), m_projects(projects), m_chunks(chunks)
{
    // A photo takes up to about 60 MB while it is decoded and measured; a few at a time keep the
    // window responsive.
    m_pool.setMaxThreadCount(qBound(1, QThread::idealThreadCount() / 2, 4));
    if (m_projects) {
        // After the photo model was reset (projectChanged), unlike projectDirectoryChanged.
        connect(m_projects, &ProjectManager::projectChanged, this, [this] {
            if (m_projects->projectPath() != m_project) load();
        });
        connect(m_projects, &ProjectManager::photosChanged, this, &PhotoQualityService::changed);
    }
    if (m_chunks) connect(m_chunks, &ChunkService::changed, this, &PhotoQualityService::changed);
    load();
}

PhotoQualityService::~PhotoQualityService()
{
    if (m_cancel) m_cancel->store(true);
    m_pool.clear();
    m_pool.waitForDone();
}

void PhotoQualityService::load()
{
    if (m_cancel) m_cancel->store(true);
    m_cancel.reset();
    ++m_generation;
    m_done = m_total = m_measuredNow = 0;
    m_values.clear();
    m_threshold = imagequality::Threshold;
    m_project = m_projects && m_projects->hasProject() ? m_projects->projectPath() : QString();
    QFile f(QDir(m_project).filePath(QualityFile));
    if (!m_project.isEmpty() && f.size() <= 16 * 1024 * 1024 && f.open(QIODevice::ReadOnly)) {
        const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        if (root.value(QStringLiteral("schemaVersion")).toInt() == 1) {
            m_threshold = qBound(0.0, root.value(QStringLiteral("threshold")).toDouble(imagequality::Threshold), 1.0);
            const QJsonObject photos = root.value(QStringLiteral("photos")).toObject();
            for (auto it = photos.constBegin(); it != photos.constEnd(); ++it) {
                const QJsonObject o = it.value().toObject();
                Value v;
                v.sharpness = o.value(QStringLiteral("sharpness")).toDouble(-1);
                v.bytes = qint64(o.value(QStringLiteral("bytes")).toDouble());
                v.modified = QDateTime::fromString(o.value(QStringLiteral("modified")).toString(), Qt::ISODateWithMs);
                if (v.sharpness >= 0 && v.sharpness <= 1) m_values.insert(it.key(), v);
            }
        }
    }
    emit progressChanged();
    emit changed();
}

void PhotoQualityService::save() const
{
    if (m_project.isEmpty() || !m_projects) return;
    QJsonObject photos;
    for (const Photo &p : m_projects->photoModel()->photos()) {   // photos still in the project
        const auto v = m_values.constFind(p.id);
        if (v == m_values.cend()) continue;
        photos.insert(p.id, QJsonObject{{QStringLiteral("sharpness"), v->sharpness},
                                        {QStringLiteral("bytes"), double(v->bytes)},
                                        {QStringLiteral("modified"), v->modified.toString(Qt::ISODateWithMs)}});
    }
    const QJsonObject root{{QStringLiteral("schemaVersion"), 1},
                           {QStringLiteral("method"), QStringLiteral("Crété-Roffet 2007, tiles 8×6, halved to ≤ %1 px, 90th percentile").arg(imagequality::MaxSide)},
                           {QStringLiteral("threshold"), m_threshold},
                           {QStringLiteral("photos"), photos}};
    QSaveFile f(QDir(m_project).filePath(QualityFile));
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        f.commit();
    }
}

QStringList PhotoQualityService::chunkPhotos() const
{
    if (!m_projects || m_project.isEmpty()) return {};
    if (m_chunks)
        if (const ChunkService::Chunk *c = m_chunks->chunk(m_chunks->active())) return c->photos;
    QStringList ids;
    for (const Photo &p : m_projects->photoModel()->photos()) ids << p.id;
    return ids;
}

double PhotoQualityService::reference() const
{
    // The sharpest photos of the whole project, so that chunks are judged alike.
    QVector<double> values;
    if (m_projects)
        for (const Photo &p : m_projects->photoModel()->photos()) {
            const auto v = m_values.constFind(p.id);
            if (v != m_values.cend()) values << v->sharpness;
        }
    return imagequality::reference(values);
}

double PhotoQualityService::qualityOf(const QString &photoId) const
{
    const auto v = m_values.constFind(photoId);
    if (v == m_values.cend()) return -1;
    const double q = imagequality::quality(v->sharpness, reference());
    return qIsNaN(q) ? -1 : q;
}

QVariantList PhotoQualityService::photos() const
{
    if (!m_projects) return {};
    const QVector<Photo> &all = m_projects->photoModel()->photos();
    QHash<QString, int> rowOf;
    for (int i = 0; i < all.size(); ++i) rowOf.insert(all[i].id, i);
    const double ref = reference();
    const QLocale ru(QLocale::Russian);
    struct Row { double key; int row; QVariantMap map; };
    QVector<Row> rows;
    for (const QString &id : chunkPhotos()) {
        const int row = rowOf.value(id, -1);
        if (row < 0) continue;
        const Photo &p = all[row];
        const auto v = m_values.constFind(id);
        const double q = v == m_values.cend() ? qQNaN() : imagequality::quality(v->sharpness, ref);
        const bool known = !qIsNaN(q);
        rows.append({known ? q : 2.0, row,
                     QVariantMap{{QStringLiteral("id"), id},
                                 {QStringLiteral("row"), row},
                                 {QStringLiteral("name"), p.name},
                                 {QStringLiteral("thumb"), QUrl(QStringLiteral("image://thumbs/") + QFileInfo(p.file).fileName())},
                                 {QStringLiteral("quality"), known ? q : -1.0},
                                 {QStringLiteral("text"), known ? ru.toString(q, 'f', 2) : QStringLiteral("—")},
                                 {QStringLiteral("blurred"), known && q < m_threshold},
                                 {QStringLiteral("enabled"), !m_chunks || m_chunks->photoEnabled(id)}}});
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.key < b.key; });
    QVariantList list;
    for (const Row &r : rows) list << r.map;
    return list;
}

int PhotoQualityService::measured() const
{
    int n = 0;
    for (const QString &id : chunkPhotos()) n += m_values.contains(id) ? 1 : 0;
    return n;
}

int PhotoQualityService::countBlurred(bool enabledOnly) const
{
    const double ref = reference();
    int n = 0;
    for (const QString &id : chunkPhotos()) {
        const auto v = m_values.constFind(id);
        if (v == m_values.cend() || !(imagequality::quality(v->sharpness, ref) < m_threshold)) continue;
        if (!enabledOnly || !m_chunks || m_chunks->photoEnabled(id)) ++n;
    }
    return n;
}

void PhotoQualityService::setThreshold(double threshold)
{
    const double t = qBound(0.05, qRound(threshold * 100) / 100.0, 0.95);
    if (qFuzzyCompare(t, m_threshold)) return;
    m_threshold = t;
    save();
    emit changed();
}

QString PhotoQualityService::estimate(bool all)
{
    if (m_project.isEmpty()) return QStringLiteral("Проект не открыт");
    if (busy()) return QStringLiteral("Оценка уже идёт");
    const QVector<Photo> &photos = m_projects->photoModel()->photos();
    QHash<QString, const Photo *> byId;
    for (const Photo &p : photos) byId.insert(p.id, &p);
    struct Task { QString id, path; };
    QVector<Task> tasks;
    const QDir dir(m_project);
    const QStringList ids = chunkPhotos();
    for (const QString &id : ids) {
        const Photo *p = byId.value(id);
        if (!p) continue;
        const QString path = dir.filePath(p->file);
        const QFileInfo info(path);
        const auto v = m_values.constFind(id);
        if (!all && v != m_values.cend() && v->bytes == info.size() && v->modified == info.lastModified()) continue;
        tasks.append({id, path});
    }
    if (ids.isEmpty()) return QStringLiteral("В блоке нет снимков");
    if (tasks.isEmpty()) return QStringLiteral("Все снимки блока уже оценены");
    m_cancel = std::make_shared<std::atomic_bool>(false);
    m_done = m_measuredNow = 0;
    m_total = int(tasks.size());
    m_failed.clear();
    emit progressChanged();
    const auto cancel = m_cancel;
    const int generation = m_generation;
    QPointer<PhotoQualityService> self(this);
    for (const Task &t : std::as_const(tasks))
        m_pool.start([self, t, cancel, generation] {
            imagequality::Result r;
            const QFileInfo info(t.path);
            const bool skipped = cancel->load();
            if (!skipped) r = info.isFile() ? imagequality::measure(t.path) : imagequality::Result{qQNaN(), QStringLiteral("файл не найден")};
            QMetaObject::invokeMethod(qApp, [self, t, r, skipped, generation, bytes = info.size(), modified = info.lastModified()] {
                if (!self || generation != self->m_generation || !self->m_cancel) return;
                if (!skipped && r.ok()) {
                    self->m_values.insert(t.id, {r.sharpness, bytes, modified});
                    ++self->m_measuredNow;
                } else if (!skipped) {
                    self->m_failed << QStringLiteral("%1: %2").arg(QFileInfo(t.path).fileName(), r.error);
                }
                ++self->m_done;
                emit self->progressChanged();
                if (self->m_done == self->m_total) self->finish(self->m_cancel->load());
            }, Qt::QueuedConnection);
        });
    return {};
}

void PhotoQualityService::cancel()
{
    if (m_cancel) m_cancel->store(true);   // the photos not started yet are skipped
}

void PhotoQualityService::finish(bool cancelled)
{
    m_cancel.reset();
    save();
    emit progressChanged();
    emit changed();
    if (!m_failed.isEmpty())
        emit finished(false, QStringLiteral("Не прочитаны (%1): %2").arg(m_failed.size()).arg(m_failed.mid(0, 5).join(QStringLiteral("; "))));
    const QString counted = QStringLiteral("нечётких: %1").arg(blurred());
    if (cancelled) emit finished(true, QStringLiteral("Оценка остановлена: %1 из %2 · %3").arg(m_measuredNow).arg(m_total).arg(counted));
    else if (m_measuredNow > 0) emit finished(true, QStringLiteral("Оценено снимков: %1 · %2").arg(m_measuredNow).arg(counted));
}

QString PhotoQualityService::disableBlurred()
{
    if (!m_chunks) return QStringLiteral("Блоки недоступны");
    const double ref = reference();
    QStringList ids;
    for (const QString &id : chunkPhotos()) {
        const auto v = m_values.constFind(id);
        if (v != m_values.cend() && imagequality::quality(v->sharpness, ref) < m_threshold) ids << id;
    }
    if (ids.isEmpty()) return QStringLiteral("Нечётких снимков нет");
    return m_chunks->setPhotosEnabled(ids, false);
}

QString PhotoQualityService::enableAll()
{
    if (!m_chunks) return QStringLiteral("Блоки недоступны");
    return m_chunks->setPhotosEnabled(chunkPhotos(), true);
}
