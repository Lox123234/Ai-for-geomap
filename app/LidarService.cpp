#include "LidarService.h"
#include "Crs.h"
#include "MapService.h"
#include "OdmTools.h"
#include "Scene3DService.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QSaveFile>
#include <QThreadPool>
#include <cmath>

namespace {
QString pointCount(quint64 n)
{
    const QLocale ru(QLocale::Russian);
    if (n >= 1000000) return QStringLiteral("%1 млн").arg(ru.toString(n / 1e6, 'f', n >= 10000000 ? 0 : 1));
    if (n >= 10000) return QStringLiteral("%1 тыс.").arg(ru.toString(qRound(n / 1e3)));
    return ru.toString(qulonglong(n));
}

QString size(qint64 bytes)
{
    const QLocale ru(QLocale::Russian);
    if (bytes >= (qint64(1) << 30)) return QStringLiteral("%1 ГБ").arg(ru.toString(bytes / double(qint64(1) << 30), 'f', 1));
    if (bytes >= (1 << 20)) return QStringLiteral("%1 МБ").arg(ru.toString(bytes / double(1 << 20), 'f', 1));
    return QStringLiteral("%1 КБ").arg(ru.toString(qMax<qint64>(1, bytes / 1024)));
}
} // namespace

LidarService::LidarService(MapService *map, Scene3DService *scene, QObject *parent)
    : QObject(parent), m_map(map), m_scene(scene)
{
}

LidarService::~LidarService()
{
    if (m_cancel) m_cancel->store(true);
}

bool LidarService::available() const { return !odmtools::program(QStringLiteral("pdal")).isEmpty(); }

void LidarService::setProject(const QString &directory)
{
    if (m_cancel) m_cancel->store(true);
    ++m_generation;
    m_project = directory;
    m_clouds.clear();
    setBusy(false, {});
    load();
    emit cloudsChanged();
}

QString LidarService::relative(const QString &path) const
{
    const QString rel = QDir(m_project).relativeFilePath(path);
    return rel.startsWith(QLatin1String("..")) || QDir::isAbsolutePath(rel) ? QDir::cleanPath(path) : rel;
}

QString LidarService::absolute(const QString &path) const
{
    return path.isEmpty() || QDir::isAbsolutePath(path) ? path : QDir::cleanPath(QDir(m_project).filePath(path));
}

void LidarService::load()
{
    QFile f(QDir(m_project).filePath(QStringLiteral("lidar.json")));
    if (m_project.isEmpty() || !f.open(QIODevice::ReadOnly)) return;
    for (const QJsonValue &v : QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("clouds")).toArray()) {
        const QJsonObject o = v.toObject();
        Cloud c;
        c.info.path = absolute(o.value(QStringLiteral("path")).toString());
        c.name = o.value(QStringLiteral("name")).toString(QFileInfo(c.info.path).completeBaseName());
        c.info.points = quint64(o.value(QStringLiteral("points")).toDouble());
        c.info.bytes = qint64(o.value(QStringLiteral("bytes")).toDouble());
        c.info.crsWkt = o.value(QStringLiteral("crs")).toString();
        const QJsonArray e = o.value(QStringLiteral("extent")).toArray();
        if (e.size() == 4) c.info.extent = QRectF(QPointF(e[0].toDouble(), e[1].toDouble()), QPointF(e[2].toDouble(), e[3].toDouble()));
        c.info.minZ = o.value(QStringLiteral("minZ")).toDouble();
        c.info.maxZ = o.value(QStringLiteral("maxZ")).toDouble();
        c.info.format = o.value(QStringLiteral("format")).toString();
        c.info.compressed = o.value(QStringLiteral("compressed")).toBool();
        c.info.hasColor = o.value(QStringLiteral("color")).toBool();
        for (const QJsonValue &k : o.value(QStringLiteral("classes")).toArray()) c.info.classes << k.toInt();
        c.dsm = absolute(o.value(QStringLiteral("dsm")).toString());
        c.dtm = absolute(o.value(QStringLiteral("dtm")).toString());
        m_clouds << c;
    }
}

void LidarService::save() const
{
    if (m_project.isEmpty()) return;
    QJsonArray list;
    for (const Cloud &c : m_clouds) {
        QJsonArray classes;
        for (int k : c.info.classes) classes << k;
        list << QJsonObject{{QStringLiteral("path"), relative(c.info.path)},
                            {QStringLiteral("name"), c.name},
                            {QStringLiteral("points"), double(c.info.points)},
                            {QStringLiteral("bytes"), double(c.info.bytes)},
                            {QStringLiteral("crs"), c.info.crsWkt},
                            {QStringLiteral("extent"), QJsonArray{c.info.extent.left(), c.info.extent.top(), c.info.extent.right(), c.info.extent.bottom()}},
                            {QStringLiteral("minZ"), c.info.minZ},
                            {QStringLiteral("maxZ"), c.info.maxZ},
                            {QStringLiteral("format"), c.info.format},
                            {QStringLiteral("compressed"), c.info.compressed},
                            {QStringLiteral("color"), c.info.hasColor},
                            {QStringLiteral("classes"), classes},
                            {QStringLiteral("dsm"), c.dsm.isEmpty() ? QString() : relative(c.dsm)},
                            {QStringLiteral("dtm"), c.dtm.isEmpty() ? QString() : relative(c.dtm)}};
    }
    QSaveFile f(QDir(m_project).filePath(QStringLiteral("lidar.json")));
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(QJsonObject{{QStringLiteral("schemaVersion"), 1}, {QStringLiteral("clouds"), list}}).toJson());
        f.commit();
    }
}

QVariantList LidarService::clouds() const
{
    QVariantList list;
    const QLocale ru(QLocale::Russian);
    for (int i = 0; i < m_clouds.size(); ++i) {
        const Cloud &c = m_clouds[i];
        QStringList classes;
        for (int k : c.info.classes)
            if (k != 0 && k != 1) classes << lidar::classTitle(k);
        const Crs crs = Crs::fromWkt(c.info.crsWkt);
        list << QVariantMap{{QStringLiteral("index"), i},
                            {QStringLiteral("name"), c.name},
                            {QStringLiteral("path"), QDir::toNativeSeparators(c.info.path)},
                            {QStringLiteral("missing"), !QFileInfo::exists(c.info.path)},
                            {QStringLiteral("summary"), QStringList{pointCount(c.info.points) + QStringLiteral(" точек"), c.info.format, size(c.info.bytes)}.join(QStringLiteral(" · "))},
                            {QStringLiteral("crs"), crs.isValid() ? crs.label() : QStringLiteral("без системы координат")},
                            {QStringLiteral("heights"), QStringLiteral("высоты %1 – %2 м").arg(ru.toString(c.info.minZ, 'f', 1), ru.toString(c.info.maxZ, 'f', 1))},
                            {QStringLiteral("classes"), classes.isEmpty() ? QStringLiteral("без классов") : classes.join(QStringLiteral(", "))},
                            {QStringLiteral("hasGround"), c.info.classes.contains(2)},
                            {QStringLiteral("dsm"), c.dsm},
                            {QStringLiteral("dtm"), c.dtm},
                            {QStringLiteral("resolution"), lidar::suggestedResolution(c.info)}};
    }
    return list;
}

void LidarService::setBusy(bool busy, const QString &status)
{
    if (busy == m_busy && status == m_status) return;
    m_busy = busy;
    m_status = status;
    emit busyChanged();
}

QString LidarService::importUrls(const QList<QUrl> &urls)
{
    QStringList paths;
    for (const QUrl &u : urls) paths << (u.isLocalFile() ? u.toLocalFile() : u.toString());
    return importPaths(paths);
}

QString LidarService::importPaths(const QStringList &input)
{
    if (m_project.isEmpty()) return QStringLiteral("Проект не открыт");
    if (m_busy) return QStringLiteral("Подождите — идёт работа с облаком");
    if (!available()) return QStringLiteral("Нужен PDAL из состава OpenDroneMap — он не найден");
    QStringList paths;
    for (const QString &p : input) {
        const QString abs = QFileInfo(p).absoluteFilePath();
        bool known = false;
        for (const Cloud &c : m_clouds) known = known || QDir::cleanPath(c.info.path).compare(QDir::cleanPath(abs), Qt::CaseInsensitive) == 0;
        if (!known) paths << abs;
    }
    if (paths.isEmpty()) return QStringLiteral("Эти облака уже в проекте");
    m_cancel = std::make_shared<std::atomic_bool>(false);
    setBusy(true, QStringLiteral("Чтение облака…"));
    const auto cancel = m_cancel;
    const int generation = m_generation;
    QPointer<LidarService> self(this);
    QThreadPool::globalInstance()->start([self, paths, cancel, generation] {
        QVector<lidar::Info> read;
        for (const QString &p : paths) {
            if (cancel->load()) break;
            read << lidar::inspect(p, [cancel] { return cancel->load(); });
        }
        QMetaObject::invokeMethod(qApp, [self, read, generation] {
            if (!self || generation != self->m_generation) return;
            QStringList errors, added;
            for (const lidar::Info &info : read) {
                if (!info.ok()) {
                    errors << QStringLiteral("%1: %2").arg(QFileInfo(info.path).fileName(), info.error);
                    continue;
                }
                Cloud c;
                c.info = info;
                c.name = QFileInfo(info.path).completeBaseName();
                self->m_clouds << c;
                added << c.name;
            }
            self->save();
            emit self->cloudsChanged();
            self->setBusy(false, {});
            if (!added.isEmpty())
                emit self->finished(true, QStringLiteral("Добавлено облаков LiDAR: %1").arg(added.size()), {});
            if (!errors.isEmpty()) emit self->finished(false, errors.join('\n'), {});
        }, Qt::QueuedConnection);
    });
    return {};
}

void LidarService::remove(int index)
{
    if (index < 0 || index >= m_clouds.size() || m_busy) return;
    m_clouds.removeAt(index);
    save();
    emit cloudsChanged();
}

QString LidarService::showIn3D(int index)
{
    if (index < 0 || index >= m_clouds.size()) return QStringLiteral("Нет такого облака");
    if (!m_scene) return QStringLiteral("3D-сцена недоступна");
    const QString path = m_clouds[index].info.path;
    if (!QFileInfo::exists(path)) return QStringLiteral("Файл не найден: %1").arg(QDir::toNativeSeparators(path));
    if (!m_scene->layerIdForSource(path).isEmpty()) return {};
    return m_scene->addPointCloud(path);
}

double LidarService::suggestedResolution(int index) const
{
    return index >= 0 && index < m_clouds.size() ? lidar::suggestedResolution(m_clouds[index].info) : 1;
}

QString LidarService::buildSurface(int index, const QString &kind, double resolution)
{
    if (index < 0 || index >= m_clouds.size()) return QStringLiteral("Нет такого облака");
    if (m_busy) return QStringLiteral("Подождите — идёт работа с облаком");
    if (kind != QLatin1String("dsm") && kind != QLatin1String("dtm")) return QStringLiteral("Неизвестная модель");
    const Cloud c = m_clouds[index];
    if (!QFileInfo::exists(c.info.path)) return QStringLiteral("Файл не найден: %1").arg(QDir::toNativeSeparators(c.info.path));
    if (!Crs::fromWkt(c.info.crsWkt).isValid()) return QStringLiteral("У облака нет системы координат — модель не встанет на карту");
    const QString out = QDir(m_project).filePath(QStringLiteral("lidar/%1_%2.tif").arg(c.name, kind));
    if (m_map) m_map->releaseRasterFile(out);
    m_cancel = std::make_shared<std::atomic_bool>(false);
    const bool dtm = kind == QLatin1String("dtm");
    setBusy(true, dtm ? (c.info.classes.contains(2) ? QStringLiteral("DTM по точкам земли…") : QStringLiteral("Поиск земли и DTM…"))
                      : QStringLiteral("DSM по облаку…"));
    const auto cancel = m_cancel;
    const int generation = m_generation;
    QPointer<LidarService> self(this);
    const QString name = c.name, path = c.info.path;
    QThreadPool::globalInstance()->start([self, c, kind, resolution, out, cancel, generation, name, path, dtm] {
        const lidar::Surface s = lidar::buildSurface(c.info, kind, resolution, out, {}, [cancel] { return cancel->load(); });
        QMetaObject::invokeMethod(qApp, [self, s, cancel, generation, name, path, dtm, out] {
            if (!self) return;
            if (self->m_map) self->m_map->reopenRasterFile(out);
            if (generation != self->m_generation) return;
            self->setBusy(false, {});
            if (!s.ok()) {
                emit self->finished(false, cancel->load() ? QStringLiteral("Отменено") : s.error, {});
                return;
            }
            for (Cloud &k : self->m_clouds)
                if (k.info.path == path) (dtm ? k.dtm : k.dsm) = s.path;
            self->save();
            emit self->cloudsChanged();
            QString error;
            if (self->m_map && self->m_map->layerIdForSource(s.path).isEmpty()) error = self->m_map->addLayerFromPath(s.path);
            const QLocale ru(QLocale::Russian);
            emit self->finished(error.isEmpty(),
                                error.isEmpty() ? QStringLiteral("%1 по «%2»: %3 × %4, ячейка %5 м%6")
                                                      .arg(dtm ? QStringLiteral("DTM") : QStringLiteral("DSM"), name)
                                                      .arg(s.width).arg(s.height).arg(ru.toString(s.resolution, 'g', 3))
                                                      .arg(dtm && !s.groundFromFile ? QStringLiteral(" (земля найдена фильтром SMRF)") : QString())
                                                : error,
                                s.path);
        }, Qt::QueuedConnection);
    });
    return {};
}

void LidarService::cancel()
{
    if (m_cancel) m_cancel->store(true);
}
