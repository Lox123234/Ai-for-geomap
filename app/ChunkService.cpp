#include "ChunkService.h"

#include "Mosaic.h"
#include "PhotoModel.h"
#include "ProcessingService.h"
#include "ProcessingTypes.h"
#include "ProjectManager.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QPointer>
#include <QSaveFile>
#include <QSet>
#include <QtMath>
#include <algorithm>
#include <cmath>

namespace {
const QString ChunksFile = QStringLiteral("chunks.json");
const QString MosaicsDir = QStringLiteral("mosaics");

QString now() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs); }

const QLocale &ru()
{
    static const QLocale locale(QLocale::Russian);
    return locale;
}

QString kindTitle(const QString &kind)
{
    if (kind == QLatin1String("orthophoto")) return QStringLiteral("ортофотоплан");
    if (kind == QLatin1String("dsm")) return QStringLiteral("DSM");
    if (kind == QLatin1String("dtm")) return QStringLiteral("DTM");
    return kind;
}
} // namespace

// ------------------------------------------------------------------ photos of the active chunk

void ChunkPhotoModel::setPhotos(const QStringList &ids, const QStringList &disabled)
{
    QSet<QString> set(ids.cbegin(), ids.cend()), off(disabled.cbegin(), disabled.cend());
    if (set != m_ids) {
        m_ids = std::move(set);
        invalidateFilter();
    }
    if (off != m_disabled) {
        m_disabled = std::move(off);
        if (rowCount() > 0) emit dataChanged(index(0, 0), index(rowCount() - 1, 0), {EnabledRole});
    }
}

QVariant ChunkPhotoModel::data(const QModelIndex &index, int role) const
{
    if (role == EnabledRole) return !m_disabled.contains(QSortFilterProxyModel::data(index, PhotoModel::IdRole).toString());
    return QSortFilterProxyModel::data(index, role);
}

QHash<int, QByteArray> ChunkPhotoModel::roleNames() const
{
    QHash<int, QByteArray> names = QSortFilterProxyModel::roleNames();
    names.insert(EnabledRole, "photoEnabled");
    return names;
}

bool ChunkPhotoModel::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    return m_ids.contains(sourceModel()->index(sourceRow, 0, sourceParent).data(PhotoModel::IdRole).toString());
}

// ------------------------------------------------------------------ chunks

ChunkService::ChunkService(ProjectManager *projects, ProcessingService *processing, QObject *parent)
    : QObject(parent), m_projects(projects), m_processing(processing)
{
    m_pool.setMaxThreadCount(1);
    if (m_projects) {
        m_photoModel.setSourceModel(m_projects->photoModel());
        // After the photo model was reset (projectChanged), unlike projectDirectoryChanged.
        connect(m_projects, &ProjectManager::projectChanged, this, [this] {
            if (m_projects->projectPath() != m_project) load();
        });
        connect(m_projects, &ProjectManager::photosChanged, this, &ChunkService::sync);
    }
    if (m_processing) connect(m_processing, &ProcessingService::jobsChanged, this, &ChunkService::changed);
    load();
}

ChunkService::~ChunkService()
{
    cancelMerge();
    m_pool.waitForDone();
    if (m_processing) m_processing->setChunk({}, {}, true);   // the processing outlives the chunks: all photos again
}

const ChunkService::Chunk *ChunkService::chunk(const QString &id) const
{
    for (const Chunk &c : m_chunks)
        if (c.id == id) return &c;
    return nullptr;
}

ChunkService::Chunk *ChunkService::find(const QString &id)
{
    for (Chunk &c : m_chunks)
        if (c.id == id) return &c;
    return nullptr;
}

QString ChunkService::activeName() const
{
    const Chunk *c = chunk(m_active);
    return c ? c->name : QString();
}

int ChunkService::activePhotoCount() const
{
    const Chunk *c = chunk(m_active);
    return c ? int(c->photos.size()) : 0;
}

int ChunkService::activeDisabledCount() const
{
    const Chunk *c = chunk(m_active);
    return c ? int(c->disabled.size()) : 0;
}

bool ChunkService::jobInChunk(const ProcessingJob &job, const QString &chunkId) const
{
    return ProcessingService::jobInChunk(job, chunkId, !m_chunks.isEmpty() && m_chunks.first().id == chunkId);
}

QString ChunkService::nameOf(const QString &id) const
{
    if (id.isEmpty()) return m_chunks.isEmpty() ? QString() : m_chunks.first().name;
    const Chunk *c = chunk(id);
    return c ? c->name : QStringLiteral("удалённый блок");
}

QVariantList ChunkService::chunksForQml() const
{
    QVariantList list;
    const QVector<ProcessingJob> jobs = m_processing ? m_processing->jobs() : QVector<ProcessingJob>();
    const QVector<Photo> &photos = m_projects ? m_projects->photoModel()->photos() : QVector<Photo>();
    QHash<QString, const Photo *> byId;
    for (const Photo &p : photos) byId.insert(p.id, &p);
    for (const Chunk &c : m_chunks) {
        int jobCount = 0, done = 0, gps = 0;
        for (const ProcessingJob &j : jobs)
            if (jobInChunk(j, c.id)) {
                ++jobCount;
                if (j.state == JobState::Completed) ++done;
            }
        for (const QString &id : c.photos)
            if (const Photo *p = byId.value(id)) gps += p->hasGps ? 1 : 0;
        list << QVariantMap{{"id", c.id}, {"name", c.name}, {"photos", int(c.photos.size())}, {"disabled", int(c.disabled.size())},
                            {"gps", gps}, {"jobs", jobCount}, {"completed", done}, {"active", c.id == m_active}, {"split", !c.cell.isEmpty()}};
    }
    return list;
}

void ChunkService::load()
{
    m_project = m_projects ? m_projects->projectPath() : QString();
    m_chunks.clear();
    m_active.clear();
    m_stored = false;
    if (!m_project.isEmpty()) {
        QFile f(QDir(m_project).filePath(ChunksFile));
        if (f.size() <= 32 * 1024 * 1024 && f.open(QIODevice::ReadOnly)) {
            const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
            if (root.value("schemaVersion").toInt() == 1) {
                QSet<QString> ids;
                for (const QJsonValue &v : root.value("chunks").toArray()) {
                    const QJsonObject o = v.toObject();
                    Chunk c;
                    c.id = o.value("id").toString();
                    if (c.id.isEmpty() || ids.contains(c.id)) continue;
                    ids.insert(c.id);
                    c.name = o.value("name").toString(c.id);
                    for (const QJsonValue &p : o.value("photos").toArray()) c.photos << p.toString();
                    for (const QJsonValue &p : o.value("disabled").toArray()) c.disabled << p.toString();
                    for (const QJsonValue &p : o.value("cell").toArray()) {
                        const QJsonArray xy = p.toArray();
                        if (xy.size() == 2) c.cell << QPointF(xy.at(0).toDouble(), xy.at(1).toDouble());
                    }
                    c.created = o.value("created").toString();
                    m_chunks << c;
                }
                m_active = root.value("active").toString();
                m_stored = !m_chunks.isEmpty();
            }
        }
        if (m_chunks.isEmpty()) {
            // A project before chunks: one chunk with every photo (the file is written once chunks are used).
            Chunk c;
            c.id = QStringLiteral("c1");
            c.name = QStringLiteral("Блок 1");
            c.created = now();
            m_chunks << c;
        }
    }
    if (!chunk(m_active)) m_active = m_chunks.isEmpty() ? QString() : m_chunks.first().id;
    sync();
    publish();
    emit changed();
}

void ChunkService::sync()
{
    if (m_project.isEmpty() || m_chunks.isEmpty() || !m_projects) {
        publish();
        return;
    }
    const QVector<Photo> &photos = m_projects->photoModel()->photos();
    QHash<QString, int> order;
    for (int i = 0; i < photos.size(); ++i) order.insert(photos[i].id, i);
    bool modified = false;
    QSet<QString> assigned;
    for (Chunk &c : m_chunks) {
        QStringList kept;
        QSet<QString> seen;
        for (const QString &id : std::as_const(c.photos))
            if (order.contains(id) && !seen.contains(id)) {
                kept << id;
                seen.insert(id);
            }
        std::sort(kept.begin(), kept.end(), [&order](const QString &a, const QString &b) { return order.value(a) < order.value(b); });
        if (kept != c.photos) {
            c.photos = kept;
            modified = true;
        }
        QStringList off;
        for (const QString &id : std::as_const(c.disabled))
            if (seen.contains(id) && !off.contains(id)) off << id;
        if (off != c.disabled) {
            c.disabled = off;
            modified = true;
        }
        assigned.unite(seen);
    }
    // New photos (imported, or of a project before chunks) join the active chunk.
    Chunk *target = find(m_active);
    if (!target) target = &m_chunks.first();
    for (const Photo &p : photos)
        if (!assigned.contains(p.id)) {
            target->photos << p.id;
            modified = true;
        }
    if (modified && m_stored) save();
    publish();
    if (modified) emit changed();
}

bool ChunkService::save()
{
    if (m_project.isEmpty()) return false;
    QJsonArray chunks;
    for (const Chunk &c : std::as_const(m_chunks)) {
        QJsonObject o{{"id", c.id}, {"name", c.name}, {"photos", QJsonArray::fromStringList(c.photos)}, {"created", c.created}};
        if (!c.disabled.isEmpty()) o.insert("disabled", QJsonArray::fromStringList(c.disabled));
        if (!c.cell.isEmpty()) {
            QJsonArray cell;
            for (const QPointF &p : c.cell) cell.append(QJsonArray{p.x(), p.y()});
            o.insert("cell", cell);
        }
        chunks.append(o);
    }
    const QJsonObject root{{"schemaVersion", 1}, {"active", m_active}, {"chunks", chunks}};
    QSaveFile f(QDir(m_project).filePath(ChunksFile));
    const QByteArray data = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit()) {
        emit errorOccurred(QStringLiteral("Не удалось сохранить блоки проекта (chunks.json)."));
        return false;
    }
    m_stored = true;
    // Jobs from before chunks are the first chunk's: written into them, so they stay with it.
    if (m_processing && !m_chunks.isEmpty()) m_processing->adoptJobs(m_chunks.first().id);
    return true;
}

void ChunkService::publish()
{
    const Chunk *c = chunk(m_active);
    const QStringList photos = c ? c->photos : QStringList(), disabled = c ? c->disabled : QStringList();
    m_photoModel.setPhotos(photos, disabled);
    QStringList processed;
    for (const QString &id : photos)
        if (!disabled.contains(id)) processed << id;
    if (m_processing) m_processing->setChunk(c ? c->id : QString(), processed, c && m_chunks.first().id == c->id, int(disabled.size()));
}

QString ChunkService::newId() const
{
    int n = 0;
    for (const Chunk &c : m_chunks)
        if (c.id.startsWith(QLatin1Char('c'))) n = qMax(n, c.id.mid(1).toInt());
    return QStringLiteral("c%1").arg(n + 1);
}

QString ChunkService::uniqueName(const QString &base) const
{
    auto taken = [this](const QString &name) {
        for (const Chunk &c : m_chunks)
            if (c.name.compare(name, Qt::CaseInsensitive) == 0) return true;
        return false;
    };
    if (!taken(base)) return base;
    for (int n = 2;; ++n) {
        const QString name = QStringLiteral("%1 (%2)").arg(base).arg(n);
        if (!taken(name)) return name;
    }
}

bool ChunkService::setActive(const QString &id)
{
    if (!chunk(id)) return false;
    if (id == m_active) return true;
    m_active = id;
    save();
    publish();
    emit changed();
    return true;
}

QString ChunkService::create(const QString &name)
{
    if (m_project.isEmpty()) {
        emit errorOccurred(QStringLiteral("Откройте проект."));
        return {};
    }
    Chunk c;
    c.id = newId();
    c.name = name.trimmed().isEmpty() ? uniqueName(QStringLiteral("Блок %1").arg(m_chunks.size() + 1)) : uniqueName(name.trimmed());
    c.created = now();
    m_chunks << c;
    m_active = c.id;
    save();
    publish();
    emit changed();
    return c.id;
}

QString ChunkService::duplicate(const QString &id)
{
    const Chunk *source = chunk(id);
    if (!source) {
        emit errorOccurred(QStringLiteral("Нет такого блока."));
        return {};
    }
    Chunk c = *source;
    c.id = newId();
    c.name = uniqueName(source->name + QStringLiteral(" (копия)"));
    c.created = now();
    m_chunks << c;
    m_active = c.id;
    save();
    publish();
    emit changed();
    return c.id;
}

QString ChunkService::rename(const QString &id, const QString &name)
{
    Chunk *c = find(id);
    if (!c) return QStringLiteral("Нет такого блока.");
    const QString n = name.trimmed();
    if (n.isEmpty()) return QStringLiteral("Введите название блока.");
    if (n.size() > 100) return QStringLiteral("Название длиннее 100 знаков.");
    for (const Chunk &other : std::as_const(m_chunks))
        if (other.id != id && other.name.compare(n, Qt::CaseInsensitive) == 0) return QStringLiteral("Блок «%1» уже есть.").arg(n);
    if (c->name == n) return {};
    c->name = n;
    save();
    emit changed();
    return {};
}

QString ChunkService::remove(const QString &id)
{
    const int index = int(std::find_if(m_chunks.cbegin(), m_chunks.cend(), [&id](const Chunk &c) { return c.id == id; }) - m_chunks.cbegin());
    if (index >= m_chunks.size()) return QStringLiteral("Нет такого блока.");
    if (m_chunks.size() == 1) return QStringLiteral("Последний блок проекта удалить нельзя.");
    const Chunk removed = m_chunks.takeAt(index);
    // Photos of no other chunk stay in the project, in the first chunk.
    QSet<QString> elsewhere;
    for (const Chunk &c : std::as_const(m_chunks)) elsewhere.unite(QSet<QString>(c.photos.cbegin(), c.photos.cend()));
    for (const QString &p : removed.photos)
        if (!elsewhere.contains(p)) m_chunks.first().photos << p;
    if (m_active == id) m_active = m_chunks.first().id;
    save();
    sync();   // order of the moved photos
    publish();
    emit changed();
    return {};
}

QString ChunkService::removePhoto(const QString &chunkId, const QString &photoId)
{
    Chunk *c = find(chunkId);
    if (!c) return QStringLiteral("Нет такого блока.");
    if (!c->photos.contains(photoId)) return QStringLiteral("Снимка нет в блоке.");
    bool elsewhere = false;
    for (const Chunk &other : std::as_const(m_chunks))
        if (other.id != chunkId && other.photos.contains(photoId)) elsewhere = true;
    if (!elsewhere) return QStringLiteral("Снимок есть только в этом блоке: сначала добавьте его в другой блок или удалите из проекта.");
    c->photos.removeAll(photoId);
    c->disabled.removeAll(photoId);
    save();
    publish();
    emit changed();
    return {};
}

QString ChunkService::addPhoto(const QString &chunkId, const QString &photoId)
{
    Chunk *c = find(chunkId);
    if (!c) return QStringLiteral("Нет такого блока.");
    if (c->photos.contains(photoId)) return {};
    c->photos << photoId;
    save();
    sync();   // keeps the project's order
    emit changed();
    return {};
}

QString ChunkService::setPhotosEnabled(const QStringList &photoIds, bool enabled)
{
    Chunk *c = find(m_active);
    if (!c) return QStringLiteral("Нет активного блока.");
    bool modified = false;
    for (const QString &id : photoIds) {
        if (!c->photos.contains(id)) continue;
        if (enabled) modified = c->disabled.removeAll(id) > 0 || modified;
        else if (!c->disabled.contains(id)) {
            c->disabled << id;
            modified = true;
        }
    }
    if (!modified) return {};
    if (!save()) return QStringLiteral("Не удалось сохранить блоки проекта (chunks.json).");
    publish();
    emit changed();
    return {};
}

bool ChunkService::photoEnabled(const QString &photoId) const
{
    const Chunk *c = chunk(m_active);
    return !c || !c->disabled.contains(photoId);
}

QStringList ChunkService::activePhotoFiles() const
{
    const Chunk *c = chunk(m_active);
    if (!c || !m_projects) return {};
    const QSet<QString> ids(c->photos.cbegin(), c->photos.cend());
    QStringList files;
    for (const Photo &p : m_projects->photoModel()->photos())
        if (ids.contains(p.id)) files << p.file;
    return files;
}

// ------------------------------------------------------------------ split

QVector<ChunkService::Part> ChunkService::parts(const Chunk &source, const QVariantMap &options, QStringList *notes, int *noPosition,
                                               int *skipped, QString *error) const
{
    QVector<Part> result;
    *noPosition = 0;
    *skipped = 0;
    const int minPhotos = qMax(2, options.value(QStringLiteral("minPhotos"), 3).toInt());
    const QVector<Photo> &all = m_projects->photoModel()->photos();
    QHash<QString, const Photo *> byId;
    for (const Photo &p : all) byId.insert(p.id, &p);
    QVector<const Photo *> photos;
    for (const QString &id : source.photos)
        if (const Photo *p = byId.value(id)) photos << p;
    const QString mode = options.value(QStringLiteral("mode"), QStringLiteral("grid")).toString();

    if (mode == QLatin1String("flights")) {
        // Flights: a pause between consecutive shots longer than gapMinutes starts a new one.
        const double gap = qBound(0.5, options.value(QStringLiteral("gapMinutes"), 5.0).toDouble(), 24.0 * 60.0);
        QVector<QPair<QDateTime, const Photo *>> timed;
        for (const Photo *p : photos) {
            const QDateTime t = QDateTime::fromString(p->takenAt.left(19), QStringLiteral("yyyy:MM:dd HH:mm:ss"));
            if (t.isValid()) timed.append({t, p});
            else ++*noPosition;
        }
        std::stable_sort(timed.begin(), timed.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        QVector<QStringList> groups;
        for (int i = 0; i < timed.size(); ++i) {
            if (i == 0 || timed[i - 1].first.secsTo(timed[i].first) > gap * 60.0) groups.append(QStringList());
            groups.last() << timed[i].second->id;
        }
        if (groups.size() < 2) {
            *error = timed.isEmpty() ? QStringLiteral("У снимков блока нет времени съёмки (EXIF): по полётам не разделить.")
                                     : QStringLiteral("Пауз дольше %1 мин между снимками нет: блок снят одним полётом.").arg(ru().toString(gap));
            return {};
        }
        int k = 0;
        for (const QStringList &g : groups) {
            ++k;
            if (g.size() < minPhotos) {
                ++*skipped;
                notes->append(QStringLiteral("Полёт %1: %2 сн. — меньше %3, не выделен").arg(k).arg(g.size()).arg(minPhotos));
                continue;
            }
            result.append({QStringLiteral("%1 · полёт %2").arg(source.name).arg(k), g, {}});
        }
    } else {
        // Grid over the photo positions: rows × columns, each cell widened by the overlap.
        const int rows = qBound(1, options.value(QStringLiteral("rows"), 2).toInt(), 20);
        const int cols = qBound(1, options.value(QStringLiteral("cols"), 2).toInt(), 20);
        const double overlap = qBound(0.0, options.value(QStringLiteral("overlap"), 20.0).toDouble(), 50.0) / 100.0;
        if (rows * cols < 2) {
            *error = QStringLiteral("Задайте хотя бы две части (ряды × столбцы).");
            return {};
        }
        QVector<const Photo *> placed;
        double lat0 = 0, lon0 = 0;
        for (const Photo *p : photos) {
            if (!p->hasGps) {
                ++*noPosition;
                continue;
            }
            placed << p;
            lat0 += p->latitude;
            lon0 += p->longitude;
        }
        if (placed.size() < 2) {
            *error = QStringLiteral("Для деления сеткой нужны снимки с GPS (в блоке: %1).").arg(placed.size());
            return {};
        }
        lat0 /= placed.size();
        lon0 /= placed.size();
        // Local metres (equirectangular around the centre; enough to sort photos into cells).
        const double kx = 111320.0 * std::cos(qDegreesToRadians(lat0)), ky = 110574.0;
        QVector<QPointF> xy;
        double minX = 1e300, maxX = -1e300, minY = 1e300, maxY = -1e300;
        for (const Photo *p : placed) {
            const QPointF q((p->longitude - lon0) * kx, (p->latitude - lat0) * ky);
            xy << q;
            minX = qMin(minX, q.x());
            maxX = qMax(maxX, q.x());
            minY = qMin(minY, q.y());
            maxY = qMax(maxY, q.y());
        }
        if (maxX - minX < 1) { minX -= 0.5; maxX += 0.5; }
        if (maxY - minY < 1) { minY -= 0.5; maxY += 0.5; }
        const double cw = (maxX - minX) / cols, ch = (maxY - minY) / rows, mx = cw * overlap, my = ch * overlap;
        auto lonLat = [&](double x, double y) { return QPointF(lon0 + x / kx, lat0 + y / ky); };
        QVector<Part> cells;
        for (int r = 0; r < rows; ++r)
            for (int c = 0; c < cols; ++c) {
                // Row 0 in the north; the last row and column end exactly at the outermost photos.
                const double x0 = minX + c * cw, x1 = c == cols - 1 ? maxX : x0 + cw;
                const double y1 = maxY - r * ch, y0 = r == rows - 1 ? minY : y1 - ch;
                constexpr double eps = 1e-6;
                Part part;
                for (int i = 0; i < placed.size(); ++i)
                    if (xy[i].x() >= x0 - mx - eps && xy[i].x() <= x1 + mx + eps && xy[i].y() >= y0 - my - eps && xy[i].y() <= y1 + my + eps)
                        part.photos << placed[i]->id;
                part.cell = {lonLat(x0, y1), lonLat(x1, y1), lonLat(x1, y0), lonLat(x0, y0)};
                const QString where = rows > 1 && cols > 1 ? QStringLiteral("ряд %1, столбец %2").arg(r + 1).arg(c + 1)
                                                           : QStringLiteral("часть %1").arg(r * cols + c + 1);
                if (part.photos.size() < minPhotos) {
                    ++*skipped;
                    notes->append(QStringLiteral("%1: %2 сн. — меньше %3, не выделена").arg(where).arg(part.photos.size()).arg(minPhotos));
                    continue;
                }
                part.name = QStringLiteral("%1 · %2").arg(source.name, where);
                cells << part;
            }
        result = cells;
        notes->prepend(QStringLiteral("Ячейка %1 × %2 м, перекрытие %3 %").arg(ru().toString(cw, 'f', 0), ru().toString(ch, 'f', 0))
                           .arg(qRound(overlap * 100)));
    }
    if (*noPosition > 0)
        notes->append(mode == QLatin1String("flights") ? QStringLiteral("Без времени съёмки: %1 сн. — остаются только в исходном блоке").arg(*noPosition)
                                                       : QStringLiteral("Без GPS: %1 сн. — остаются только в исходном блоке").arg(*noPosition));
    if (result.size() < 2 && error->isEmpty())
        *error = QStringLiteral("Получилось частей с %1 и более снимками: %2 — делить нечего.").arg(minPhotos).arg(result.size());
    return result;
}

QVariantMap ChunkService::splitPreview(const QString &id, const QVariantMap &options) const
{
    const Chunk *source = chunk(id);
    if (!source) return {{"ok", false}, {"error", QStringLiteral("Нет такого блока.")}};
    QStringList notes;
    int noPosition = 0, skipped = 0;
    QString error;
    const QVector<Part> list = parts(*source, options, &notes, &noPosition, &skipped, &error);
    QVariantList partList;
    for (const Part &p : list) partList << QVariantMap{{"name", p.name}, {"photos", int(p.photos.size())}};
    return {{"ok", error.isEmpty()}, {"error", error}, {"parts", partList}, {"noPosition", noPosition}, {"skipped", skipped},
            {"notes", notes}};
}

QVariantMap ChunkService::split(const QString &id, const QVariantMap &options)
{
    const Chunk *source = chunk(id);
    if (!source) return {{"ok", false}, {"error", QStringLiteral("Нет такого блока.")}};
    QStringList notes;
    int noPosition = 0, skipped = 0;
    QString error;
    const QVector<Part> list = parts(*source, options, &notes, &noPosition, &skipped, &error);
    if (!error.isEmpty()) return {{"ok", false}, {"error", error}, {"notes", notes}};
    const QStringList disabled = source->disabled;   // before m_chunks grows
    QStringList created;
    for (const Part &p : list) {
        Chunk c;
        c.id = newId();
        c.name = uniqueName(p.name);
        c.photos = p.photos;
        for (const QString &id : disabled)
            if (p.photos.contains(id)) c.disabled << id;
        c.cell = p.cell;
        c.created = now();
        m_chunks << c;
        created << c.id;
    }
    save();
    sync();
    publish();
    emit changed();
    return {{"ok", true}, {"error", QString()}, {"created", created}, {"notes", notes}};
}

// ------------------------------------------------------------------ merge

QString ChunkService::mergePhotos(const QStringList &ids, const QString &name)
{
    if (ids.size() < 2) {
        emit errorOccurred(QStringLiteral("Выберите хотя бы два блока."));
        return {};
    }
    QSet<QString> photos, enabled;
    QStringList names;
    for (const QString &id : ids) {
        const Chunk *c = chunk(id);
        if (!c) {
            emit errorOccurred(QStringLiteral("Нет такого блока."));
            return {};
        }
        photos.unite(QSet<QString>(c->photos.cbegin(), c->photos.cend()));
        for (const QString &p : c->photos)
            if (!c->disabled.contains(p)) enabled.insert(p);
        names << c->name;
    }
    Chunk merged;
    merged.id = newId();
    merged.name = uniqueName(name.trimmed().isEmpty() ? QStringLiteral("Объединённый блок") : name.trimmed());
    merged.photos = QStringList(photos.cbegin(), photos.cend());
    for (const QString &p : std::as_const(merged.photos))   // off where it was off in every chunk
        if (!enabled.contains(p)) merged.disabled << p;
    merged.created = now();
    m_chunks << merged;
    m_active = merged.id;
    save();
    sync();   // the project's order
    publish();
    emit changed();
    return merged.id;
}

QVariantList ChunkService::resultsOf(const QStringList &ids, const QString &kind) const
{
    QVariantList list;
    const QVector<ProcessingJob> jobs = m_processing ? m_processing->jobs() : QVector<ProcessingJob>();
    for (const QString &id : ids) {
        const Chunk *c = chunk(id);
        if (!c) continue;
        QVariantMap row{{"chunk", id}, {"name", c->name}, {"path", QString()}, {"job", QString()}};
        for (const ProcessingJob &j : jobs) {   // newest first
            if (j.state != JobState::Completed || !jobInChunk(j, id)) continue;
            const auto artifact = std::find_if(j.outputs.cbegin(), j.outputs.cend(), [&kind](const Artifact &a) { return a.kind == kind; });
            if (artifact == j.outputs.cend()) continue;
            row["path"] = QDir(m_processing->jobDirectory(j.id)).filePath(artifact->file);
            row["job"] = j.id;
            break;
        }
        list << row;
    }
    return list;
}

QString ChunkService::mergeResults(const QStringList &ids, const QString &kind)
{
    if (m_project.isEmpty()) return QStringLiteral("Откройте проект.");
    if (merging()) return QStringLiteral("Объединение уже идёт.");
    if (kind != QLatin1String("orthophoto") && kind != QLatin1String("dsm") && kind != QLatin1String("dtm"))
        return QStringLiteral("Объединяются ортофотопланы, DSM или DTM.");
    if (ids.size() < 2) return QStringLiteral("Выберите хотя бы два блока.");
    QVector<mosaic::Input> inputs;
    QStringList missing;
    for (const QVariant &v : resultsOf(ids, kind)) {
        const QVariantMap row = v.toMap();
        const QString path = row.value("path").toString();
        if (path.isEmpty()) {
            missing << row.value("name").toString();
            continue;
        }
        mosaic::Input in;
        in.path = path;
        in.name = row.value("name").toString();
        if (const Chunk *c = chunk(row.value("chunk").toString())) in.cell = c->cell;
        inputs << in;
    }
    if (!missing.isEmpty())
        return QStringLiteral("Нет готового результата «%1» у блоков: %2. Обработайте их или снимите отметку.").arg(kindTitle(kind), missing.join(QStringLiteral(", ")));
    if (inputs.size() < 2) return QStringLiteral("Нужны результаты хотя бы двух блоков.");
    const QString output = QDir(m_project).filePath(MosaicsDir + QLatin1Char('/') + kind + QLatin1Char('-')
                                                    + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")) + QStringLiteral(".tif"));
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_mergeCancel = cancel;
    m_mergeStatus = QStringLiteral("Объединение: %1 блоков…").arg(inputs.size());
    emit mergeChanged();
    QPointer<ChunkService> self(this);
    const QString title = kindTitle(kind);
    m_pool.start([self, inputs, output, cancel, title] {
        mosaic::Options options;
        int lastPercent = -1;
        options.progress = [&](double v) {
            const int percent = int(v * 100);
            if (percent != lastPercent && self) {
                lastPercent = percent;
                QMetaObject::invokeMethod(self, [self, percent, title] {
                    if (!self || !self->merging()) return;
                    self->m_mergeStatus = QStringLiteral("Объединение: %1 — %2 %").arg(title).arg(percent);
                    emit self->mergeChanged();
                }, Qt::QueuedConnection);
            }
            return !*cancel;
        };
        const mosaic::Result r = mosaic::build(inputs, output, options);
        QMetaObject::invokeMethod(self, [self, r, output, title, count = int(inputs.size())] {
            if (!self) return;
            self->m_mergeCancel.reset();
            QString message;
            if (r.ok()) {
                message = QStringLiteral("Объединённый %1 из %2 блоков: %3 × %4, %5 см/пикс, за %6 с")
                              .arg(title).arg(count).arg(r.width).arg(r.height)
                              .arg(ru().toString(r.resolution * 100, 'f', 1), ru().toString(r.seconds, 'f', 0));
                if (!r.notes.isEmpty()) message += QStringLiteral(". ") + r.notes.join(QStringLiteral("; "));
            } else {
                message = r.error;
            }
            self->m_mergeStatus = message;
            emit self->mergeChanged();
            emit self->mergeFinished(r.ok(), r.ok() ? output : QString(), message);
        }, Qt::QueuedConnection);
    });
    return {};
}

void ChunkService::cancelMerge()
{
    if (m_mergeCancel) *m_mergeCancel = true;
}
