#include <QtTest>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtEndian>
#include <algorithm>
#include <cstring>
#include <memory>
#include "GeoTiffInfo.h"
#include "MockEngine.h"
#include "OdmEngine.h"
#include "PhotoModel.h"
#include "ProcessingService.h"
#include "TestImages.h"

namespace {
bool writeFile(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
QJsonObject readJson(const QString &path)
{
    return QJsonDocument::fromJson(readFile(path)).object();
}

// Minimal (Big)TIFF writer: tags only, no pixel data; enough for GeoTiffInfo.
class TiffBuilder
{
public:
    TiffBuilder(bool bigEndian, bool bigTiff) : m_be(bigEndian), m_big(bigTiff) {}
    void shorts(quint16 tag, const QVector<quint16> &v)
    {
        QByteArray d;
        for (quint16 x : v) d += u16(x);
        m_entries.append({tag, 3, quint64(v.size()), d});
    }
    void longs(quint16 tag, const QVector<quint32> &v)
    {
        QByteArray d;
        for (quint32 x : v) d += u32(x);
        m_entries.append({tag, 4, quint64(v.size()), d});
    }
    void doubles(quint16 tag, const QVector<double> &v)
    {
        QByteArray d;
        for (double x : v) {
            quint64 bits;
            std::memcpy(&bits, &x, sizeof bits);
            d += u64(bits);
        }
        m_entries.append({tag, 12, quint64(v.size()), d});
    }
    QByteArray build() const
    {
        auto entries = m_entries;
        std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) { return a.tag < b.tag; });
        QByteArray out = m_be ? QByteArray("MM") : QByteArray("II");
        if (m_big) out += u16(43) + u16(8) + u16(0) + u64(16);
        else out += u16(42) + u32(8);
        const int countSize = m_big ? 8 : 2, entrySize = m_big ? 20 : 12, inlineSize = m_big ? 8 : 4;
        const quint64 tailStart = quint64(out.size()) + countSize + entries.size() * entrySize + inlineSize;
        QByteArray ifd = m_big ? u64(quint64(entries.size())) : u16(quint16(entries.size()));
        QByteArray tail;
        for (const Entry &e : entries) {
            ifd += u16(e.tag) + u16(e.type) + (m_big ? u64(e.count) : u32(quint32(e.count)));
            if (e.data.size() <= inlineSize) {
                ifd += e.data + QByteArray(inlineSize - e.data.size(), '\0');
            } else {
                const quint64 offset = tailStart + quint64(tail.size());
                ifd += m_big ? u64(offset) : u32(quint32(offset));
                tail += e.data;
                if (tail.size() % 2) tail += '\0';
            }
        }
        ifd += m_big ? u64(0) : u32(0);
        return out + ifd + tail;
    }

private:
    struct Entry { quint16 tag; quint16 type; quint64 count; QByteArray data; };
    template <typename T> QByteArray bytes(T v) const
    {
        QByteArray b(int(sizeof(T)), '\0');
        if (m_be) qToBigEndian(v, b.data()); else qToLittleEndian(v, b.data());
        return b;
    }
    QByteArray u16(quint16 v) const { return bytes(v); }
    QByteArray u32(quint32 v) const { return bytes(v); }
    QByteArray u64(quint64 v) const { return bytes(v); }
    bool m_be, m_big;
    QVector<Entry> m_entries;
};

// GeoTIFF as ODM writes it: projected CRS key 3072 (or geographic 2048), pixel scale, tie point.
QByteArray geoTiff(bool bigEndian, bool bigTiff, int keyId, int epsg, double scale, int bands = 4)
{
    TiffBuilder t(bigEndian, bigTiff);
    t.longs(256, {100});
    t.longs(257, {80});
    t.shorts(277, {quint16(bands)});
    t.doubles(33550, {scale, scale, 0});
    t.doubles(33922, {0, 0, 0, 500000.0, 4649776.0, 0});
    const quint16 model = keyId == 3072 ? 1 : 2;
    t.shorts(34735, {1, 1, 0, 2, 1024, 0, 1, model, quint16(keyId), 0, 1, quint16(epsg)});
    return t.build();
}

QByteArray lasHeader(quint32 points)
{
    QByteArray h(227, '\0');
    h.replace(0, 4, "LASF");
    h[24] = 1;
    h[25] = 2;
    qToLittleEndian(points, h.data() + 107);
    return h;
}

// A project folder with JPEG photos (with or without GPS) and the matching photo model.
struct Fixture
{
    QTemporaryDir tmp;
    QString project;
    PhotoModel photos;
    std::shared_ptr<MockEngine::Record> record = std::make_shared<MockEngine::Record>();
    MockEngine *engine = nullptr;
    std::unique_ptr<ProcessingService> service;

    bool init(int count, bool gps = true)
    {
        if (!tmp.isValid()) return false;
        // Spaces and Cyrillic, as in real user folders.
        project = QDir(tmp.path()).filePath(QStringLiteral("Проект съёмки 1"));
        if (!QDir().mkpath(project + QStringLiteral("/photos"))) return false;
        QVector<Photo> list;
        for (int i = 0; i < count; ++i) {
            Photo p;
            p.id = QStringLiteral("p%1").arg(i);
            p.file = QStringLiteral("photos/%1.jpg").arg(p.id);
            p.name = QStringLiteral("DJI_%1.JPG").arg(i, 4, 10, QLatin1Char('0'));
            p.hasGps = gps;
            const QString path = QDir(project).filePath(p.file);
            if (!(gps ? TestImages::writeJpegWithGps(path, 64, 48) : TestImages::writeJpeg(path, {}, 64, 48))) return false;
            p.bytes = QFileInfo(path).size();
            list << p;
        }
        photos.resetPhotos(list, project);
        engine = new MockEngine(record);
        service = std::make_unique<ProcessingService>(engine);
        service->setProject(project, &photos);
        return true;
    }
    bool checkEnvironment()
    {
        service->checkEnvironment();
        return QTest::qWaitFor([this] { return !service->checkingEnvironment(); }, 5000);
    }
    QString jobFile(const QString &id, const QString &name) const
    {
        return QDir(project).filePath(QStringLiteral("jobs/%1/%2").arg(id, name));
    }
    JobState state(const QString &id) const { return service->job(id).state; }
};
} // namespace

class ProcessingTest : public QObject
{
    Q_OBJECT
private slots:
    void preflightReportsRealProblems()
    {
        {
            auto record = std::make_shared<MockEngine::Record>();
            ProcessingService none(new MockEngine(record));
            const QVariantMap r = none.preflight(QStringLiteral("fast"));
            QVERIFY(!r.value("ok").toBool());
            QVERIFY(r.value("errors").toStringList().contains(QStringLiteral("Проект не открыт.")));
            QVERIFY(none.startJob(QStringLiteral("fast")).isEmpty());
        }
        Fixture f;
        QVERIFY(f.init(3));
        QVariantMap r = f.service->preflight(QStringLiteral("fast"));
        QVERIFY(!r.value("ok").toBool());
        QVERIFY(r.value("errors").toStringList().join(' ').contains(QStringLiteral("не проверено")));

        QVERIFY(f.checkEnvironment());
        r = f.service->preflight(QStringLiteral("fast"));
        QVERIFY2(r.value("ok").toBool(), qPrintable(r.value("errors").toStringList().join('\n')));
        QCOMPARE(r.value("images").toInt(), 3);
        QCOMPARE(r.value("withGps").toInt(), 3);
        QVERIFY(r.value("warnings").toStringList().isEmpty());
        QCOMPARE(r.value("arguments").toString(), QStringLiteral("--fast"));
        QVERIFY(r.value("freeBytes").toDouble() > 0);

        r = f.service->preflight(QStringLiteral("fast"), QStringLiteral("--ok"));
        QVERIFY(r.value("ok").toBool());
        QCOMPARE(r.value("arguments").toString(), QStringLiteral("--fast --ok"));
        r = f.service->preflight(QStringLiteral("fast"), QStringLiteral("--ok \"--evil; rm -rf /\""));
        QVERIFY(!r.value("ok").toBool());
        QVERIFY(r.value("errors").toStringList().contains(QStringLiteral("bad option --evil; rm -rf /")));
        QVERIFY(!f.service->preflight(QStringLiteral("nope")).value("ok").toBool());

        f.service->setDiskReserveBytes(qint64(1) << 60);
        r = f.service->preflight(QStringLiteral("fast"));
        QVERIFY(!r.value("ok").toBool());
        QVERIFY(r.value("errors").toStringList().join(' ').contains(QStringLiteral("Недостаточно места")));
        f.service->setDiskReserveBytes(0);

        QVERIFY(QFile::remove(QDir(f.project).filePath(QStringLiteral("photos/p1.jpg"))));
        r = f.service->preflight(QStringLiteral("fast"));
        QVERIFY(!r.value("ok").toBool());
        QVERIFY(r.value("errors").toStringList().join(' ').contains(QStringLiteral("DJI_0001.JPG")));
        QVERIFY(r.value("errors").toStringList().join(' ').contains(QStringLiteral("не меньше 2")) == false);

        QVERIFY(QFile::remove(QDir(f.project).filePath(QStringLiteral("photos/p2.jpg"))));
        r = f.service->preflight(QStringLiteral("fast"));
        QVERIFY(r.value("errors").toStringList().join(' ').contains(QStringLiteral("не меньше 2")));

        f.engine->env.available = false;
        f.engine->env.problems = {QStringLiteral("Docker не найден")};
        QVERIFY(f.checkEnvironment());
        QVERIFY(!f.service->engineAvailable());
        QVERIFY(f.service->preflight(QStringLiteral("fast")).value("errors").toStringList().contains(QStringLiteral("Docker не найден")));
    }

    void warnsAboutMissingGps()
    {
        Fixture f;
        QVERIFY(f.init(2, false));
        QVERIFY(f.checkEnvironment());
        const QVariantMap r = f.service->preflight(QStringLiteral("fast"));
        QVERIFY(r.value("ok").toBool());
        QVERIFY(r.value("warnings").toStringList().join(' ').contains(QStringLiteral("нет GPS")));
    }

    void completesJobAndRegistersOutputs()
    {
        Fixture f;
        QVERIFY(f.init(3));
        QVERIFY(f.checkEnvironment());
        QSignalSpy finished(f.service.get(), &ProcessingService::jobFinished);
        QSignalSpy logs(f.service.get(), &ProcessingService::logUpdated);

        const QString id = f.service->startJob(QStringLiteral("fast"));
        QVERIFY(!id.isEmpty());
        QCOMPARE(f.state(id), JobState::Preparing);
        QVERIFY(f.service->active());
        QCOMPARE(f.service->jobModel()->rowCount(), 1);
        QTRY_COMPARE(f.record->started, QStringList{id});
        QCOMPARE(f.state(id), JobState::Running);
        QVERIFY(f.service->job(id).startedAt.isValid());

        // Inputs: every photo in <job>/images with the same bytes; the project copies stay.
        QCOMPARE(QDir(f.record->lastDirectory).absolutePath(), QDir(f.project).filePath(QStringLiteral("jobs/") + id));
        QCOMPARE(f.record->lastImages, (QStringList{QStringLiteral("p0.jpg"), QStringLiteral("p1.jpg"), QStringLiteral("p2.jpg")}));
        QCOMPARE(readFile(f.jobFile(id, QStringLiteral("images/p1.jpg"))),
                 readFile(QDir(f.project).filePath(QStringLiteral("photos/p1.jpg"))));

        f.engine->stage(QStringLiteral("opensfm"), 2);
        QCOMPARE(f.service->job(id).stage, QStringLiteral("opensfm"));
        QCOMPARE(f.service->job(id).stageIndex, 2);
        QCOMPARE(readJson(f.jobFile(id, QStringLiteral("job.json"))).value("stage").toString(), QStringLiteral("opensfm"));
        const QModelIndex row = f.service->jobModel()->index(0);
        QCOMPARE(row.data(JobListModel::StageTitleRole).toString(), QStringLiteral("Этап opensfm"));
        QCOMPARE(row.data(JobListModel::StateRole).toString(), QStringLiteral("running"));

        f.engine->log(QStringLiteral("строка журнала 1"));
        QTRY_VERIFY(logs.count() > 0);
        QVERIFY(QString::fromUtf8(readFile(f.jobFile(id, QStringLiteral("log.txt")))).contains(QStringLiteral("строка журнала 1")));
        QVERIFY(f.service->jobDetails(id).value("log").toString().contains(QStringLiteral("строка журнала 1")));

        QVERIFY(writeFile(f.jobFile(id, QStringLiteral("out/ortho.tif")), geoTiff(false, false, 3072, 32630, 0.05)));
        QVERIFY(writeFile(f.jobFile(id, QStringLiteral("out/report.pdf")), "%PDF-1.7"));
        f.engine->finish(JobOutcome::Succeeded, 0, QStringLiteral("ok"));
        QCOMPARE(f.state(id), JobState::Completed);
        QCOMPARE(finished.count(), 1);
        QVERIFY(!f.service->active());
        const ProcessingJob job = f.service->job(id);
        QCOMPARE(job.outputs.size(), 2);
        QVERIFY(job.finishedAt.isValid());
        QVERIFY(job.message.contains(QStringLiteral("Результатов: 2")));

        const QJsonObject saved = readJson(f.jobFile(id, QStringLiteral("job.json")));
        QCOMPARE(saved.value("state").toString(), QStringLiteral("completed"));
        QCOMPARE(saved.value("outputs").toArray().size(), 2);
        QCOMPARE(saved.value("images").toArray().size(), 3);

        const QVariantMap details = f.service->jobDetails(id);
        QCOMPARE(details.value("outputs").toList().size(), 2);
        QVERIFY(details.value("missing").toStringList().isEmpty());
        const QString log = QString::fromUtf8(readFile(f.jobFile(id, QStringLiteral("log.txt"))));
        QVERIFY(log.contains(QStringLiteral("Профиль: Быстрый (fast)")));
        QVERIFY(log.contains(QStringLiteral("Снимков: 3")));
        QVERIFY(log.contains(QStringLiteral("mock started")));
        QVERIFY(log.trimmed().endsWith(QStringLiteral("(код 0)")));
        // Finished jobs read their log from disk.
        QVERIFY(f.service->jobDetails(id).value("log").toString().contains(QStringLiteral("строка журнала 1")));

        // History survives a restart unchanged.
        f.service.reset();
        auto record = std::make_shared<MockEngine::Record>();
        ProcessingService reopened(new MockEngine(record));
        reopened.setProject(f.project, &f.photos);
        QCOMPARE(reopened.jobs().size(), 1);
        QCOMPARE(reopened.job(id).state, JobState::Completed);
        QCOMPARE(reopened.job(id).outputs.size(), 2);
        QVERIFY(record->cleaned.isEmpty());
    }

    void failureKeepsEngineMessage()
    {
        Fixture f;
        QVERIFY(f.init(2));
        QVERIFY(f.checkEnvironment());
        const QString id = f.service->startJob(QStringLiteral("fast"));
        QTRY_COMPARE(f.state(id), JobState::Running);
        f.engine->finish(JobOutcome::Failed, 1, QStringLiteral("ODM завершился с ошибкой (код 1). Not enough memory"));
        QCOMPARE(f.state(id), JobState::Failed);
        QCOMPARE(f.service->job(id).exitCode, 1);
        QCOMPARE(f.service->job(id).message, QStringLiteral("ODM завершился с ошибкой (код 1). Not enough memory"));
    }

    void successWithoutOutputsIsNotCompleted()
    {
        Fixture f;
        QVERIFY(f.init(2));
        QVERIFY(f.checkEnvironment());
        const QString id = f.service->startJob(QStringLiteral("fast"));
        QTRY_COMPARE(f.state(id), JobState::Running);
        f.engine->finish(JobOutcome::Succeeded, 0, QStringLiteral("ok"));
        QCOMPARE(f.state(id), JobState::Failed);
        QVERIFY(f.service->job(id).message.contains(QStringLiteral("результатов в папке задания нет")));
    }

    void startFailureIsReported()
    {
        Fixture f;
        QVERIFY(f.init(2));
        QVERIFY(f.checkEnvironment());
        f.engine->failStart = true;
        const QString id = f.service->startJob(QStringLiteral("fast"));
        QTRY_COMPARE(f.state(id), JobState::Failed);
        QCOMPARE(f.service->job(id).message, QStringLiteral("cannot start"));
        QVERIFY(!f.service->active());
    }

    void cancelsRunningJob()
    {
        Fixture f;
        QVERIFY(f.init(2));
        QVERIFY(f.checkEnvironment());
        const QString id = f.service->startJob(QStringLiteral("fast"));
        QTRY_COMPARE(f.state(id), JobState::Running);
        f.service->cancelJob(id);
        QCOMPARE(f.state(id), JobState::Cancelling);
        QCOMPARE(f.record->cancelled, QStringList{id});
        QTRY_COMPARE(f.state(id), JobState::Cancelled);
        QVERIFY(!f.service->active());
        QCOMPARE(readJson(f.jobFile(id, QStringLiteral("job.json"))).value("state").toString(), QStringLiteral("cancelled"));
    }

    void cancelsBeforeTheEngineStarts()
    {
        Fixture f;
        QVERIFY(f.init(2));
        QVERIFY(f.checkEnvironment());
        const QString id = f.service->startJob(QStringLiteral("fast"));
        QCOMPARE(f.state(id), JobState::Preparing);
        f.service->cancelJob(id);
        QCOMPARE(f.state(id), JobState::Cancelling);
        QTRY_COMPARE(f.state(id), JobState::Cancelled);
        QVERIFY(f.record->started.isEmpty());
    }

    void queueRunsOneJobAtATime()
    {
        Fixture f;
        QVERIFY(f.init(2));
        QVERIFY(f.checkEnvironment());
        const QString a = f.service->startJob(QStringLiteral("fast"));
        QTRY_COMPARE(f.state(a), JobState::Running);
        const QString b = f.service->startJob(QStringLiteral("fast"));
        const QString c = f.service->startJob(QStringLiteral("fast"));
        QVERIFY(a != b && b != c && a != c);
        QCOMPARE(f.state(b), JobState::Queued);
        QCOMPARE(f.state(c), JobState::Queued);
        f.service->cancelJob(b);
        QCOMPARE(f.state(b), JobState::Cancelled);

        QVERIFY(writeFile(f.jobFile(a, QStringLiteral("out/ortho.tif")), geoTiff(false, false, 3072, 32630, 0.05)));
        f.engine->finish(JobOutcome::Succeeded, 0, QStringLiteral("ok"));
        QCOMPARE(f.state(a), JobState::Completed);
        QTRY_COMPARE(f.state(c), JobState::Running);
        QCOMPARE(f.record->started, (QStringList{a, c}));
        // Each job has its own directory; the earlier result is untouched.
        QVERIFY(QFileInfo::exists(f.jobFile(a, QStringLiteral("out/ortho.tif"))));
        QVERIFY(!QFileInfo::exists(f.jobFile(c, QStringLiteral("out/ortho.tif"))));
    }

    void unreadablePhotoFailsPreparation()
    {
        Fixture f;
        QVERIFY(f.init(3));
        QVERIFY(f.checkEnvironment());
        QVERIFY(writeFile(QDir(f.project).filePath(QStringLiteral("photos/p1.jpg")), "not a jpeg"));
        const QString id = f.service->startJob(QStringLiteral("fast"));
        QTRY_COMPARE(f.state(id), JobState::Failed);
        QVERIFY(f.service->job(id).message.contains(QStringLiteral("p1.jpg")));
        QVERIFY(f.record->started.isEmpty());
    }

    void restartShowsActiveJobsAsInterrupted()
    {
        Fixture f;
        QVERIFY(f.init(2));
        ProcessingJob job;
        job.id = QStringLiteral("20260101-000000");
        job.profile = QStringLiteral("fast");
        job.state = JobState::Running;
        job.createdAt = QDateTime::currentDateTime().addDays(-1);
        QVERIFY(writeFile(f.jobFile(job.id, QStringLiteral("job.json")), QJsonDocument(job.toJson()).toJson()));
        QVERIFY(writeFile(f.jobFile(QStringLiteral("broken"), QStringLiteral("job.json")), "{ not json"));

        auto record = std::make_shared<MockEngine::Record>();
        ProcessingService service(new MockEngine(record));
        service.setProject(f.project, &f.photos);
        QCOMPARE(service.jobs().size(), 1);
        QCOMPARE(service.job(job.id).state, JobState::Interrupted);
        QVERIFY(service.job(job.id).message.contains(QStringLiteral("прервана")));
        QCOMPARE(record->cleaned, QStringList{job.id});
        QCOMPARE(readJson(f.jobFile(job.id, QStringLiteral("job.json"))).value("state").toString(), QStringLiteral("interrupted"));
        QVERIFY(!service.active());
    }

    void closingStopsTheRunningJob()
    {
        Fixture f;
        QVERIFY(f.init(2));
        QVERIFY(f.checkEnvironment());
        const QString id = f.service->startJob(QStringLiteral("fast"));
        QTRY_COMPARE(f.state(id), JobState::Running);
        const QString queued = f.service->startJob(QStringLiteral("fast"));
        f.service.reset();
        QCOMPARE(f.record->aborted, QStringList{id});
        QCOMPARE(readJson(f.jobFile(id, QStringLiteral("job.json"))).value("state").toString(), QStringLiteral("interrupted"));
        QCOMPARE(readJson(f.jobFile(queued, QStringLiteral("job.json"))).value("state").toString(), QStringLiteral("cancelled"));
        QVERIFY(QString::fromUtf8(readFile(f.jobFile(id, QStringLiteral("log.txt")))).contains(QStringLiteral("приложение закрыто")));
    }

    void switchingProjectStopsWork()
    {
        Fixture f;
        QVERIFY(f.init(2));
        QVERIFY(f.checkEnvironment());
        const QString id = f.service->startJob(QStringLiteral("fast"));
        QTRY_COMPARE(f.state(id), JobState::Running);
        f.service->setProject(QString(), nullptr);
        QCOMPARE(f.record->aborted, QStringList{id});
        QVERIFY(!f.service->active());
        QCOMPARE(f.service->jobCount(), 0);
        QCOMPARE(readJson(f.jobFile(id, QStringLiteral("job.json"))).value("state").toString(), QStringLiteral("interrupted"));
        // Reopening the project shows the stopped job as interrupted, not as running.
        f.service->setProject(f.project, &f.photos);
        QCOMPARE(f.state(id), JobState::Interrupted);
    }

    void deletingAJobKeepsProjectPhotos()
    {
        Fixture f;
        QVERIFY(f.init(2));
        QVERIFY(f.checkEnvironment());
        const QString done = f.service->startJob(QStringLiteral("fast"));
        QTRY_COMPARE(f.state(done), JobState::Running);
        f.engine->finish(JobOutcome::Failed, 1, QStringLiteral("x"));
        const QString running = f.service->startJob(QStringLiteral("fast"));
        QTRY_COMPARE(f.state(running), JobState::Running);
        QVERIFY(!f.service->deleteJob(running));

        const QByteArray before = readFile(QDir(f.project).filePath(QStringLiteral("photos/p0.jpg")));
        QVERIFY(f.service->deleteJob(done));
        QVERIFY(!QFileInfo::exists(QDir(f.project).filePath(QStringLiteral("jobs/") + done)));
        QCOMPARE(readFile(QDir(f.project).filePath(QStringLiteral("photos/p0.jpg"))), before);
        QVERIFY(!before.isEmpty());
        QCOMPARE(f.service->jobCount(), 1);
    }

    // ------------------------------------------------------------- OdmEngine

    void odmRunArgumentsNeedNoShell()
    {
        ProcessingJob job;
        job.id = QStringLiteral("20261014-120000");
        job.arguments = {QStringLiteral("--fast-orthophoto")};
        const QString image = OdmEngine::imageReference();
        QVERIFY(image.startsWith(QStringLiteral("opendronemap/odm@sha256:94a0dd27")));
#ifdef Q_OS_WIN
        const QString dir = QStringLiteral("C:/Users/Тест Пользователь/Проект, 1/jobs/20261014-120000");
        const QStringList win = OdmEngine::runArguments(job, dir, true);
        QCOMPARE(win, (QStringList{
            QStringLiteral("run"), QStringLiteral("--rm"), QStringLiteral("--name"), QStringLiteral("geomap-odm-20261014-120000"),
            QStringLiteral("-v"), QStringLiteral("C:\\Users\\Тест Пользователь\\Проект, 1\\jobs\\20261014-120000:/datasets/code"),
            QStringLiteral("-v"), QStringLiteral("C:\\Users\\Тест Пользователь\\Проект, 1\\jobs\\20261014-120000\\images:/datasets/code/images:ro"),
            image, QStringLiteral("--project-path"), QStringLiteral("/datasets"), QStringLiteral("code"),
            QStringLiteral("--fast-orthophoto")}));
#endif
        const QString linuxDir = QDir(QStringLiteral("/data/Проект, \"1\"/jobs/20261014-120000")).absolutePath();
        const QStringList lin = OdmEngine::runArguments(job, linuxDir, false);
        const QString quoted = QStringLiteral("\"source=") + QString(linuxDir).replace(QStringLiteral("\""), QStringLiteral("\"\""));
        QCOMPARE(lin.at(4), QStringLiteral("--mount"));
        QCOMPARE(lin.at(5), QStringLiteral("type=bind,") + quoted + QStringLiteral("\",target=/datasets/code"));
        QCOMPARE(lin.at(7), QStringLiteral("type=bind,") + quoted + QStringLiteral("/images\",target=/datasets/code/images,readonly"));
    }

    void odmParsesStagesAndValidatesOptions()
    {
        QCOMPARE(OdmEngine::parseStage(QStringLiteral("[INFO]    Running opensfm stage")), QStringLiteral("opensfm"));
        QCOMPARE(OdmEngine::parseStage(OdmEngine::stripAnsi(QStringLiteral("\x1b[39m[INFO]    Running odm_orthophoto stage\x1b[0m"))),
                 QStringLiteral("odm_orthophoto"));
        QVERIFY(OdmEngine::parseStage(QStringLiteral("[INFO]    Finished opensfm stage")).isEmpty());
        QCOMPARE(OdmEngine::stripAnsi(QStringLiteral("\x1b[93m[WARNING] x\x1b[0m")), QStringLiteral("[WARNING] x"));
        QCOMPARE(OdmEngine::stageIds().indexOf(QStringLiteral("opensfm")), 3);

        QString error;
        QVERIFY(OdmEngine::validateExtraArguments({QStringLiteral("--dsm"), QStringLiteral("--orthophoto-resolution"), QStringLiteral("2.5")}, &error));
        QVERIFY(!OdmEngine::validateExtraArguments({QStringLiteral("--orthophoto-resolution")}, &error));
        QVERIFY(error.contains(QStringLiteral("не указано значение")));
        QVERIFY(!OdmEngine::validateExtraArguments({QStringLiteral("--feature-quality"), QStringLiteral("best")}, &error));
        QVERIFY(!OdmEngine::validateExtraArguments({QStringLiteral("--max-concurrency"), QStringLiteral("0")}, &error));
        QVERIFY(!OdmEngine::validateExtraArguments({QStringLiteral("--rm")}, &error));
        QVERIFY(!OdmEngine::validateExtraArguments(QProcess::splitCommand(QStringLiteral("--dsm; rm -rf /")), &error));
        QVERIFY(!OdmEngine::validateExtraArguments(QProcess::splitCommand(QStringLiteral("--dsm --project-path /")), &error));

        OdmEngine odm;
        for (const ProcessingProfile &p : odm.profiles())
            QVERIFY2(OdmEngine::validateExtraArguments(p.arguments, &error), qPrintable(p.id + ": " + error));
        QCOMPARE(odm.profiles().size(), 3);
        QCOMPARE(odm.stageTitle(QStringLiteral("odm_orthophoto")), QStringLiteral("Ортофотоплан"));
        QCOMPARE(odm.stageTitle(QStringLiteral("future_stage")), QStringLiteral("future_stage"));
        QVERIFY(odm.capabilities().inputSuffixes.contains(QStringLiteral("jpg")));
    }

    void odmSizesConcurrencyByMemory()
    {
        const qint64 gb = 1LL << 30;
        QCOMPARE(OdmEngine::concurrencyFor(8, 16 * gb), 8);
        QCOMPARE(OdmEngine::concurrencyFor(16, 8 * gb), 7);
        QCOMPARE(OdmEngine::concurrencyFor(4, 0), 4);
        QCOMPARE(OdmEngine::concurrencyFor(2, 1 * gb), 2);
        QCOMPARE(OdmEngine::concurrencyFor(1, 512LL << 20), 1);

        OdmEngine odm;
        EnvironmentReport env;
        env.cpus = 16;
        env.memoryBytes = 8 * gb;
        ProcessingJob job;
        QString error;
        QVERIFY(odm.createJob(job, odm.profiles().constFirst(), {}, env, &error));
        QCOMPARE(job.arguments.mid(job.arguments.size() - 2), (QStringList{QStringLiteral("--max-concurrency"), QStringLiteral("7")}));
        QCOMPARE(job.stageCount, int(OdmEngine::stageIds().size()));
        QCOMPARE(job.engineVersion, QStringLiteral("3.6.2"));
        ProcessingJob manual;
        QVERIFY(odm.createJob(manual, odm.profiles().constFirst(), {QStringLiteral("--max-concurrency"), QStringLiteral("2")}, env, &error));
        QCOMPARE(manual.arguments.count(QStringLiteral("--max-concurrency")), 1);
        QVERIFY(!odm.createJob(manual, odm.profiles().constFirst(), {QStringLiteral("--evil")}, env, &error));
        QVERIFY(odm.resourceWarnings(3, odm.profiles().constFirst(), env).join(' ').contains(QStringLiteral("Снимков мало")));
        QVERIFY(odm.resourceWarnings(100, odm.profiles().at(2), env).join(' ').contains(QStringLiteral("137")));
    }

    void odmCollectsAndDescribesOutputs()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QDir dir(tmp.path());
        QVERIFY(writeFile(dir.filePath(QStringLiteral("odm_orthophoto/odm_orthophoto.tif")), geoTiff(false, false, 3072, 32630, 0.05)));
        QVERIFY(writeFile(dir.filePath(QStringLiteral("odm_georeferencing/odm_georeferenced_model.laz")), lasHeader(12345)));
        QVERIFY(writeFile(dir.filePath(QStringLiteral("odm_report/report.pdf")), "%PDF-1.7\n"));
        QVERIFY(writeFile(dir.filePath(QStringLiteral("odm_dem/dsm.tif")), QByteArray()));   // empty: not a result
        QVERIFY(writeFile(dir.filePath(QStringLiteral("odm_texturing_25d/odm_textured_model_geo.obj")), "v 0 0 0\n"));

        OdmEngine odm;
        const QVector<Artifact> outputs = odm.collectOutputs(tmp.path());
        QCOMPARE(outputs.size(), 4);
        QCOMPARE(outputs[0].kind, QStringLiteral("orthophoto"));
        QCOMPARE(outputs[0].details, QStringLiteral("EPSG:32630, 5,0 см/пикс, 100×80, каналов: 4"));
        QCOMPARE(outputs[1].kind, QStringLiteral("pointcloud"));
        QCOMPARE(outputs[1].details, QStringLiteral("LAS 1.2, точек: 12345"));
        QCOMPARE(outputs[2].kind, QStringLiteral("mesh"));
        QCOMPARE(outputs[2].file, QStringLiteral("odm_texturing_25d/odm_textured_model_geo.obj"));
        QCOMPARE(outputs[3].kind, QStringLiteral("report"));
        QCOMPARE(outputs[3].details, QStringLiteral("PDF-отчёт ODM"));
    }

    // ----------------------------------------------------------- GeoTiffInfo

    void readsGeoTiffVariants_data()
    {
        QTest::addColumn<bool>("bigEndian");
        QTest::addColumn<bool>("bigTiff");
        QTest::newRow("tiff-le") << false << false;
        QTest::newRow("tiff-be") << true << false;
        QTest::newRow("bigtiff-le") << false << true;
        QTest::newRow("bigtiff-be") << true << true;
    }
    void readsGeoTiffVariants()
    {
        QFETCH(bool, bigEndian);
        QFETCH(bool, bigTiff);
        QTemporaryDir tmp;
        const QString path = tmp.filePath(QStringLiteral("ortho.tif"));
        QVERIFY(writeFile(path, geoTiff(bigEndian, bigTiff, 3072, 32630, 0.05, 3)));
        const GeoTiffInfo info = GeoTiffInfo::read(path);
        QVERIFY2(info.valid, qPrintable(info.error));
        QCOMPARE(info.bigTiff, bigTiff);
        QCOMPARE(info.width, 100);
        QCOMPARE(info.height, 80);
        QCOMPARE(info.bands, 3);
        QVERIFY(info.georeferenced);
        QCOMPARE(info.epsg, 32630);
        QVERIFY(info.projected);
        QCOMPARE(info.pixelSizeX, 0.05);
        QCOMPARE(info.originX, 500000.0);
        QCOMPARE(info.originY, 4649776.0);
    }

    void describesGeographicUnknownAndBrokenFiles()
    {
        QTemporaryDir tmp;
        const QString geographic = tmp.filePath(QStringLiteral("wgs84.tif"));
        QVERIFY(writeFile(geographic, geoTiff(false, false, 2048, 4326, 0.0001)));
        GeoTiffInfo info = GeoTiffInfo::read(geographic);
        QCOMPARE(info.epsg, 4326);
        QVERIFY(!info.projected);
        QVERIFY(info.describe().contains(QStringLiteral("°/пикс")));

        const QString userDefined = tmp.filePath(QStringLiteral("custom.tif"));
        QVERIFY(writeFile(userDefined, geoTiff(false, false, 3072, 32767, 0.1)));
        QVERIFY(GeoTiffInfo::read(userDefined).describe().startsWith(QStringLiteral("CRS не определён")));

        const QString plain = tmp.filePath(QStringLiteral("plain.tif"));
        QImage image(20, 10, QImage::Format_RGB32);
        image.fill(Qt::gray);
        QVERIFY(image.save(plain, "TIFF"));
        info = GeoTiffInfo::read(plain);
        QVERIFY(info.valid);
        QVERIFY(!info.georeferenced);
        QCOMPARE(info.epsg, 0);
        QVERIFY(info.describe().contains(QStringLiteral("без геопривязки")));

        const QString notTiff = tmp.filePath(QStringLiteral("x.tif"));
        QVERIFY(writeFile(notTiff, "GIF89a......"));
        QVERIFY(!GeoTiffInfo::read(notTiff).valid);
        QByteArray truncated = geoTiff(false, false, 3072, 32630, 0.05);
        truncated.truncate(20);
        QVERIFY(writeFile(notTiff, truncated));
        QVERIFY(!GeoTiffInfo::read(notTiff).valid);
        QVERIFY(GeoTiffInfo::read(notTiff).describe().startsWith(QStringLiteral("не читается")));
    }
};

QTEST_GUILESS_MAIN(ProcessingTest)
#include "test_processing.moc"
