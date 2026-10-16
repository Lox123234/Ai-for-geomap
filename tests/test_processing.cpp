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
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QProcessEnvironment>
#include <QSettings>
#include "Downloader.h"
#include "GeoTiffInfo.h"
#include "GisEnvironment.h"
#include "MockEngine.h"
#include "OdmEngine.h"
#include "PhotoModel.h"
#include "Platform.h"
#include "ProcessingService.h"
#include "SurveyControl.h"
#include "TestImages.h"
#include "TrueOrthophoto.h"
#include "Overlap.h"
#include <gdal.h>
#include <QUrl>
#include <cmath>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

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
    QTemporaryDir m_settings;
private slots:
    void initTestCase()
    {
        // ProcessingService keeps the preferred runtime in QSettings: never the user's registry.
        QVERIFY(m_settings.isValid());
        QCoreApplication::setOrganizationName(QStringLiteral("GeoMapAI-Tests"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settings.path());
        // Coordinate systems of the ground control points (PROJ database).
        QVERIFY2(GisEnvironment::initialize(QCoreApplication::applicationDirPath()), qPrintable(GisEnvironment::problems().join('\n')));
    }

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
        f.engine->env.problems = {QStringLiteral("Встроенный OpenDroneMap не найден")};
        QVERIFY(f.checkEnvironment());
        QVERIFY(!f.service->engineAvailable());
        QVERIFY(f.service->preflight(QStringLiteral("fast")).value("errors").toStringList().contains(QStringLiteral("Встроенный OpenDroneMap не найден")));
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

    // A photo's mask (photos/<name>_mask.png) is placed next to it for the engine.
    void masksGoWithTheirPhotos()
    {
        Fixture f;
        QVERIFY(f.init(2));
        QVERIFY(f.checkEnvironment());
        QImage mask(8, 8, QImage::Format_Grayscale8);
        mask.fill(255);
        QVERIFY(mask.save(QDir(f.project).filePath(QStringLiteral("photos/p1_mask.png"))));
        const QString id = f.service->startJob(QStringLiteral("fast"));
        QVERIFY(!id.isEmpty());
        QTRY_COMPARE(f.record->started, QStringList{id});
        QCOMPARE(readFile(f.jobFile(id, QStringLiteral("images/p1_mask.png"))), readFile(QDir(f.project).filePath(QStringLiteral("photos/p1_mask.png"))));
        QVERIFY(!QFileInfo::exists(f.jobFile(id, QStringLiteral("images/p0_mask.png"))));
        QTRY_VERIFY(QString::fromUtf8(readFile(f.jobFile(id, QStringLiteral("log.txt")))).contains(QStringLiteral("Маски снимков: 1")));
    }

    // Ground control points go to the job as gcp_list.txt (≥ 3 points on ≥ 3 of its photos).
    void controlGoesWithTheJob()
    {
        Fixture f;
        QVERIFY(f.init(4));
        QVERIFY(f.checkEnvironment());
        SurveyControl survey;
        survey.setProject(f.project, &f.photos);
        QCOMPARE(survey.setCrs(QStringLiteral("EPSG:32632")), QString());
        for (int i = 0; i < 4; ++i) {
            const QString id = survey.addPoint(QStringLiteral("ОП %1").arg(i + 1), 500000 + i, 5000000 + i, 100);
            for (int k = 0; k < (i == 3 ? 2 : 3); ++k) survey.setObservation(id, QStringLiteral("photos/p%1.jpg").arg(k), 0.25 + 0.1 * i, 0.5);
        }
        const QString id = f.service->startJob(QStringLiteral("fast"));
        QVERIFY(!id.isEmpty());
        QTRY_COMPARE(f.record->started, QStringList{id});
        const QStringList lines = QString::fromUtf8(readFile(f.jobFile(id, QStringLiteral("gcp_list.txt")))).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        QCOMPARE(lines.size(), 1 + 9);
        QCOMPARE(lines.first(), QStringLiteral("EPSG:32632"));
        QCOMPARE(lines.at(1), QStringLiteral("500000.0000 5000000.0000 100.0000 15.50 23.50 p0.jpg ОП_1"));   // 0.25·64 − 0.5, 0.5·48 − 0.5
        QTRY_VERIFY(QString::fromUtf8(readFile(f.jobFile(id, QStringLiteral("log.txt")))).contains(QStringLiteral("Опорные точки: 3")));
        QVERIFY(QString::fromUtf8(readFile(f.jobFile(id, QStringLiteral("log.txt")))).contains(QStringLiteral("пропущены: ОП 4")));
    }

    // Accuracy from a reconstruction with a known answer, the job's inputs, the calibration of
    // an earlier job, import and export of point lists.
    void surveyControlAccuracy()
    {
        QTemporaryDir tmp;
        const QString project = QDir(tmp.path()).filePath(QStringLiteral("Съёмка с опорными точками"));
        QVERIFY(QDir().mkpath(project + QStringLiteral("/photos")));
        // Six nadir photos 40 m above the ground, 10 m apart, with lens distortion; the poses as
        // OpenSfM stores them (axis-angle, translation) in a frame shifted by coords.txt.
        const double offX = 500000, offY = 5000000;
        QVector<Photo> list;
        QVector<ortho::Shot> shots;
        QJsonObject jsonShots;
        const QJsonObject camera{{"projection_type", "brown"}, {"width", 1200}, {"height", 800}, {"focal_x", 0.9}, {"focal_y", 0.9},
                                 {"c_x", 0.004}, {"c_y", -0.003}, {"k1", -0.02}, {"k2", 0.003}, {"k3", 0.0}, {"p1", 0.0005}, {"p2", -0.0004}};
        for (int i = 0; i < 6; ++i) {
            Photo p;
            p.id = QStringLiteral("s%1").arg(i);
            p.file = QStringLiteral("photos/%1.jpg").arg(p.id);
            p.name = QStringLiteral("DJI_%1.JPG").arg(i, 4, 10, QLatin1Char('0'));
            QImage img(1200, 800, QImage::Format_RGB32);
            img.fill(Qt::gray);
            QVERIFY(img.save(QDir(project).filePath(p.file), "JPG"));
            list << p;
            ortho::Shot s;
            s.image = QDir(project).filePath(p.file);
            s.camera.width = 1200;
            s.camera.height = 800;
            s.camera.fx = s.camera.fy = 0.9;
            s.camera.cx = 0.004;
            s.camera.cy = -0.003;
            s.camera.k1 = -0.02;
            s.camera.k2 = 0.003;
            s.camera.p1 = 0.0005;
            s.camera.p2 = -0.0004;
            const double R[9] = {1, 0, 0, 0, -1, 0, 0, 0, -1};
            std::copy(R, R + 9, s.R);
            const double C[3] = {(i % 3) * 10.0, (i / 3) * 10.0, 140};
            for (int k = 0; k < 3; ++k) s.t[k] = -(R[k * 3] * C[0] + R[k * 3 + 1] * C[1] + R[k * 3 + 2] * C[2]);
            std::copy(C, C + 3, s.centre);
            shots << s;
            jsonShots.insert(QFileInfo(p.file).fileName(),
                             QJsonObject{{"camera", "test camera"}, {"rotation", QJsonArray{3.14159265358979323846, 0, 0}},
                                         {"translation", QJsonArray{s.t[0], s.t[1], s.t[2]}}});
        }
        PhotoModel photos;
        photos.resetPhotos(list, project);
        const QString jobId = QStringLiteral("20261015-100000");
        const QString job = QDir(project).filePath(QStringLiteral("jobs/") + jobId);
        QVERIFY(writeFile(job + QStringLiteral("/opensfm/reconstruction.json"),
                          QJsonDocument(QJsonArray{QJsonObject{{"cameras", QJsonObject{{"test camera", camera}}}, {"shots", jsonShots},
                                                        {"points", QJsonObject{{"1", QJsonObject{{"coordinates", QJsonArray{5, 5, 100}}}},
                                                                               {"2", QJsonObject{{"coordinates", QJsonArray{15, 3, 100}}}}}}}}).toJson()));
        QVERIFY(writeFile(job + QStringLiteral("/odm_georeferencing/coords.txt"), "WGS84 UTM 32N\n500000 5000000\n"));
        QVERIFY(writeFile(job + QStringLiteral("/cameras.json"), QJsonDocument(QJsonObject{{"test camera", camera}}).toJson()));

        SurveyControl survey;
        survey.setProject(project, &photos);
        QCOMPARE(survey.setCrs(QStringLiteral("EPSG:32632")), QString());
        // Four control points, a check point surveyed 3, −2 and 5 cm off, a point on two photos.
        struct True { const char *name; double x, y, z; bool check; int photos; QVector<double> shift; };
        const QVector<True> truth = {{"A", 5, 5, 100.5, false, 6, {0, 0, 0}},     {"B", 15, 3, 101.2, false, 6, {0, 0, 0}},
                                     {"C", 2, 12, 99.8, false, 6, {0, 0, 0}},     {"D", 18, 11, 100.1, false, 6, {0, 0, 0}},
                                     {"K", 10, 6, 100.0, true, 6, {0.03, -0.02, 0.05}}, {"E", 9, 8, 100.3, false, 2, {0, 0, 0}}};
        QHash<QString, QString> ids;
        QHash<QString, QPointF> onFirst;   // true position on photo s0
        for (const True &t : truth) {
            const QString id = survey.addPoint(QString::fromLatin1(t.name), offX + t.x + t.shift[0], offY + t.y + t.shift[1], t.z + t.shift[2]);
            ids.insert(QString::fromLatin1(t.name), id);
            survey.setCheck(id, t.check);
            const double X[3] = {t.x, t.y, t.z};
            for (int i = 0; i < t.photos; ++i) {
                double px, py, depth;
                QVERIFY(shots[i].project(X, &px, &py, &depth));
                const QPointF n((px + 0.5) / 1200, (py + 0.5) / 800);
                QVERIFY(n.x() > 0 && n.x() < 1 && n.y() > 0 && n.y() < 1);
                survey.setObservation(id, list[i].file, n.x(), n.y());
                if (i == 0) onFirst.insert(QString::fromLatin1(t.name), n);
            }
        }
        QCOMPARE(survey.controlCount(), 5);
        QCOMPARE(survey.checkCount(), 1);

        QVariantMap a = survey.accuracy();
        QVERIFY2(a.value("error").toString().isEmpty(), qPrintable(a.value("error").toString()));
        QCOMPARE(a.value("job").toString(), jobId);
        QVERIFY(!a.value("gcpUsed").toBool());
        const QVariantMap control = a.value("control").toMap(), check = a.value("check").toMap();
        QCOMPARE(control.value("count").toInt(), 5);
        QVERIFY2(control.value("rmse").toDouble() < 1e-4, qPrintable(QString::number(control.value("rmse").toDouble())));
        QCOMPARE(check.value("count").toInt(), 1);
        for (const QVariant &v : a.value("rows").toList()) {
            const QVariantMap r = v.toMap();
            QVERIFY(r.value("reprojection").toDouble() < 1e-3);
            if (r.value("name").toString() != QLatin1String("K")) continue;
            QVERIFY(std::fabs(r.value("dx").toDouble() + 0.03) < 1e-4);
            QVERIFY(std::fabs(r.value("dy").toDouble() - 0.02) < 1e-4);
            QVERIFY(std::fabs(r.value("dz").toDouble() + 0.05) < 1e-4);
        }

        // Where a point not yet placed on a photo should be.
        survey.removeObservation(ids.value("A"), list[0].file);
        bool suggested = false;
        for (const QVariant &v : survey.marksOnPhoto(list[0].file)) {
            const QVariantMap m = v.toMap();
            if (m.value("id").toString() != ids.value("A")) continue;
            QVERIFY(!m.value("placed").toBool());
            QVERIFY(std::fabs(m.value("nx").toDouble() - onFirst.value("A").x()) < 1e-5);
            QVERIFY(std::fabs(m.value("ny").toDouble() - onFirst.value("A").y()) < 1e-5);
            suggested = true;
        }
        QVERIFY(suggested);

        // What a job receives: control points on ≥ 3 of its photos, the check point never.
        QStringList images;
        for (const Photo &p : list) images << p.file;
        const QString job2 = QDir(project).filePath(QStringLiteral("jobs/20261015-110000"));
        QVERIFY(QDir().mkpath(job2));
        SurveyControl::JobInputs in = SurveyControl::prepareJob(project, job2, images);
        QVERIFY2(in.error.isEmpty(), qPrintable(in.error));
        QVERIFY(in.gcpUsed);
        QCOMPARE(in.points, 4);
        QCOMPARE(in.dropped, QStringList{QStringLiteral("E")});
        QVERIFY(in.calibration.isEmpty());
        const QStringList gcp = QString::fromUtf8(readFile(job2 + QStringLiteral("/gcp_list.txt"))).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        QCOMPARE(gcp.size(), 1 + 5 + 6 + 6 + 6);
        QCOMPARE(gcp.first(), QStringLiteral("EPSG:32632"));
        QVERIFY(!gcp.join(QLatin1Char('\n')).contains(QStringLiteral(" K")));
        in = SurveyControl::prepareJob(project, job2, images.mid(0, 2));   // too few photos: no file
        QVERIFY(!in.gcpUsed);
        QVERIFY(!QFileInfo::exists(job2 + QStringLiteral("/gcp_list.txt")));

        // Calibration of the first job as the initial cameras of the next ones.
        const QVariantList calibrations = survey.jobCalibrations();
        QCOMPARE(calibrations.size(), 1);
        QCOMPARE(calibrations.first().toMap().value("cameras").toList().first().toMap().value("focalPx").toDouble(), 1080.0);
        QCOMPARE(survey.useCalibration(jobId), QString());
        QCOMPARE(survey.calibration(), jobId);
        QVERIFY(survey.useCalibration(QStringLiteral("20261015-110000")).contains(QStringLiteral("cameras.json")));
        in = SurveyControl::prepareJob(project, job2, images);
        QCOMPARE(in.calibration, jobId);
        QCOMPARE(readFile(job2 + QStringLiteral("/geomap_cameras.json")), readFile(job + QStringLiteral("/cameras.json")));
        survey.clearCalibration();
        in = SurveyControl::prepareJob(project, job2, images);
        QVERIFY(in.calibration.isEmpty());
        QVERIFY(!QFileInfo::exists(job2 + QStringLiteral("/geomap_cameras.json")));

        // The engine gets both files of the job folder.
        ProcessingJob pj;
        pj.id = QStringLiteral("20261015-110000");
        QVERIFY(survey.useCalibration(jobId).isEmpty());
        SurveyControl::prepareJob(project, job2, images);
        const QStringList args = OdmEngine::nativeArguments(pj, QStringLiteral("C:/odm"), QDir(project).filePath(QStringLiteral("jobs")));
        QCOMPARE(args.mid(args.size() - 4),
                 (QStringList{QStringLiteral("--gcp"), QDir::toNativeSeparators(job2 + QStringLiteral("/gcp_list.txt")),
                              QStringLiteral("--cameras"), QDir::toNativeSeparators(job2 + QStringLiteral("/geomap_cameras.json"))}));

        // Export, then import of the same gcp_list.txt restores the points and their positions.
        const QString exported = tmp.filePath(QStringLiteral("gcp_list.txt"));
        QCOMPARE(survey.exportGcpList(QUrl::fromLocalFile(exported)), QString());
        QVERIFY(QString::fromUtf8(readFile(exported)).contains(QStringLiteral(" s1.jpg B\n")));
        const QPointF before = survey.controlPoints().at(1).observations.value(list[1].file);
        for (const QString &id : ids) survey.removePoint(id);
        QCOMPARE(survey.controlPoints().size(), 0);
        QVariantMap imported = survey.importFile(QUrl::fromLocalFile(exported));
        QVERIFY2(imported.value("error").toString().isEmpty(), qPrintable(imported.value("error").toString()));
        QCOMPARE(imported.value("added").toInt(), 5);   // the check point is not in a gcp_list.txt
        QCOMPARE(imported.value("observations").toInt(), 5 + 6 + 6 + 6 + 2);
        const QPointF after = survey.controlPoints().at(1).observations.value(list[1].file);
        QVERIFY(std::fabs(after.x() - before.x()) < 1e-5 && std::fabs(after.y() - before.y()) < 1e-5);

        // Coordinate lists: semicolons with decimal commas, northing first as in Russian surveys.
        const QString csv = tmp.filePath(QStringLiteral("пункты.csv"));
        QVERIFY(writeFile(csv, QStringLiteral("Пункт;X;Y;H\nРп 12;5000002,5;500001,5;99,9\n").toUtf8()));
        imported = survey.importFile(QUrl::fromLocalFile(csv), QString(), true);
        QCOMPARE(imported.value("added").toInt(), 1);
        const SurveyControl::Point last = survey.controlPoints().last();
        QCOMPARE(last.name, QStringLiteral("Рп 12"));
        QCOMPARE(last.x, 500001.5);
        QCOMPARE(last.y, 5000002.5);
        QCOMPARE(last.z, 99.9);

        // Image overlap from the same job: six photos 10 m apart, each ~44 × 30 m on the ground.
        const overlap::Result o = overlap::fromJob(job, tmp.filePath(QStringLiteral("overlap.tif")), QDir(project).filePath(QStringLiteral("photos")));
        QVERIFY2(o.ok(), qUtf8Printable(o.error));
        QCOMPARE(o.photos, 6);
        QVERIFY2(o.median >= 3 && o.share2 > 0.6, qPrintable(QStringLiteral("median %1, ≥2 %2").arg(o.median).arg(o.share2)));
        QVERIFY(o.area > 30.0 * 20 && o.area < 80.0 * 60);   // the union of the footprints
        GDALDatasetH od = GDALOpen(o.path.toUtf8().constData(), GA_ReadOnly);
        QVERIFY(od);
        int maximum = 0;
        {
            std::vector<uchar> counts(size_t(o.width) * o.height);
            QCOMPARE(GDALRasterIO(GDALGetRasterBand(od, 1), GF_Read, 0, 0, o.width, o.height, counts.data(), o.width, o.height, GDT_Byte, 0, 0), CE_None);
            for (uchar c : counts) maximum = qMax<int>(maximum, c);
        }
        GDALClose(od);
        QCOMPARE(maximum, 6);   // the middle is seen by every photo

        // Everything is in gcp.json.
        SurveyControl reopened;
        reopened.setProject(project, &photos);
        QCOMPARE(reopened.controlPoints().size(), survey.controlPoints().size());
        QCOMPARE(reopened.crs().epsg(), 32632);
        QCOMPARE(reopened.calibration(), jobId);
    }

    // A failed ODM run is explained in plain words: which program failed on which stage, what to do.
    void odmFailureIsExplained()
    {
        const QStringList recent = {
            QStringLiteral("[INFO]    Running odm_georeferencing stage"),
            QStringLiteral("[INFO]    running pdal translate -i \"in.laz\" -o \"out.laz\" smrf --filters.smrf.scalar=1.25"),
            QStringLiteral("[ERROR]   Uh oh! Processing stopped because of strange values in the reconstruction. This is often a sign..."),
        };
        QString m = OdmEngine::explainFailure(recent, {recent.last()}, QStringLiteral("Геопривязка"), 1);
        QVERIFY2(m.contains(QStringLiteral("«Геопривязка»")) && m.contains(QStringLiteral("классификации точек земли"))
                     && m.contains(QStringLiteral("Продолжить")) && !m.contains(QStringLiteral("Uh oh")), qUtf8Printable(m));
        m = OdmEngine::explainFailure({QStringLiteral("OSError: [Errno 28] No space left on device")}, {}, QStringLiteral("Ортофотоплан"), 1);
        QVERIFY2(m.contains(QStringLiteral("место")), qUtf8Printable(m));
        m = OdmEngine::explainFailure({QStringLiteral("something")}, {}, QString(), 3);
        QVERIFY2(m.contains(QStringLiteral("код 3")) && m.contains(QStringLiteral("Журнал")), qUtf8Printable(m));
        // The point classification could not put its result in place (seen through a folder link
        // with Malwarebytes): the cause is named, and repeating would not help.
        const QStringList renamed = {
            QStringLiteral("Wrote C:\\w\\odm_georeferencing\\odm_georeferenced_model.classified.laz"),
            QStringLiteral("[WARNING] Cannot classify using OpenPointClass: [WinError 3] Системе не удается найти указанный путь: "
                           "'C:\\\\w\\\\odm_georeferencing\\\\odm_georeferenced_model.classified.laz' -> 'C:\\\\w\\\\odm_georeferencing\\\\odm_georeferenced_model.laz'"),
            QStringLiteral("[INFO]    Creating LAS file"),
            QStringLiteral("[INFO]    running pdal translate -i \"C:\\w\\odm_georeferencing\\odm_georeferenced_model.laz\" -o \"x.las\""),
            QStringLiteral("PDAL: Unable to open stream for 'C:\\w\\odm_georeferencing/odm_georeferenced_model.laz' with error 'No such file or directory'"),
            QStringLiteral("[ERROR]   Uh oh! Processing stopped because of strange values in the reconstruction."),
        };
        m = OdmEngine::explainFailure(renamed, {renamed.last()}, QStringLiteral("Геопривязка"), 1);
        QVERIFY2(m.contains(QStringLiteral("антивирус")) && m.contains(QStringLiteral("переименовать")), qUtf8Printable(m));
        OdmEngine odm;
        QVERIFY(!odm.retryable(1, m));
        QVERIFY(odm.retryable(1, OdmEngine::explainFailure(recent, {recent.last()}, QStringLiteral("Геопривязка"), 1)));
    }

    // A crashed stage is computed again automatically: first itself, then — failing on it again —
    // from the stage before it; at most twice. "Продолжить" then redoes the stage that failed.
    void crashedStageIsRetried()
    {
        Fixture f;
        QVERIFY(f.init(2));
        QVERIFY(f.checkEnvironment());
        f.engine->retry = true;
        const QString id = f.service->startJob(QStringLiteral("fast"));
        QTRY_COMPARE(f.record->started.size(), 1);
        auto from = [&] {
            const QStringList a = f.service->job(id).arguments;
            const int i = int(a.indexOf(QStringLiteral("--from")));
            return i >= 0 && i + 1 < a.size() ? a[i + 1] : QString();
        };
        f.engine->stage(QStringLiteral("b"), 2);
        f.engine->finish(JobOutcome::Failed, 1, QStringLiteral("crash"));
        QTRY_COMPARE(f.record->started.size(), 2);   // the same stage again
        QCOMPARE(from(), QStringLiteral("b"));
        f.engine->stage(QStringLiteral("b"), 2);
        f.engine->finish(JobOutcome::Failed, 1, QStringLiteral("crash"));
        QTRY_COMPARE(f.record->started.size(), 3);   // the stage before it
        QCOMPARE(from(), QStringLiteral("a"));
        f.engine->stage(QStringLiteral("c"), 3);
        f.engine->finish(JobOutcome::Failed, 1, QStringLiteral("crash"));
        QTRY_COMPARE(f.state(id), JobState::Failed);   // no third retry
        QTest::qWait(50);
        QCOMPARE(f.record->started.size(), 3);
        QVERIFY(QString::fromUtf8(readFile(f.jobFile(id, QStringLiteral("log.txt")))).contains(QStringLiteral("автоматически (попытка 2 из 2)")));
        QVERIFY(f.service->resumeJob(id));
        QTRY_COMPARE(f.record->started.size(), 4);
        QCOMPARE(from(), QStringLiteral("c"));
        // Without the engine's consent (memory, disk) nothing is repeated.
        f.engine->retry = false;
        f.engine->stage(QStringLiteral("c"), 3);
        f.engine->finish(JobOutcome::Failed, 1, QStringLiteral("out of memory"));
        QTRY_COMPARE(f.state(id), JobState::Failed);
        QCOMPARE(f.record->started.size(), 4);
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

    void resumesStoppedAndPartialJobs()
    {
        Fixture f;
        QVERIFY(f.init(2));
        QVERIFY(f.checkEnvironment());
        // A failed job continues in its own directory.
        const QString id = f.service->startJob(QStringLiteral("fast"));
        QTRY_COMPARE(f.state(id), JobState::Running);
        f.engine->finish(JobOutcome::Failed, 1, QStringLiteral("x"));
        QVERIFY(f.service->jobDetails(id).value("canResume").toBool());
        QVERIFY(f.service->resumeJob(id));
        QTRY_COMPARE(f.record->started, (QStringList{id, id}));
        QCOMPARE(f.service->job(id).runs, 2);
        QCOMPARE(f.service->jobCount(), 1);
        QTRY_VERIFY(QString::fromUtf8(readFile(f.jobFile(id, QStringLiteral("log.txt")))).contains(QStringLiteral("продолжение")));
        QVERIFY(writeFile(f.jobFile(id, QStringLiteral("out/ortho.tif")), geoTiff(false, false, 3072, 32630, 0.05)));
        f.engine->finish(JobOutcome::Succeeded, 0, QStringLiteral("ok"));
        QCOMPARE(f.state(id), JobState::Completed);
        QVERIFY(!f.service->jobDetails(id).value("canResume").toBool());

        // Alignment only: complete with the engine's report, then continue with another profile.
        const QString part = f.service->startJob(QStringLiteral("part"));
        QTRY_COMPARE(f.state(part), JobState::Running);
        QVERIFY(writeFile(f.jobFile(part, QStringLiteral("stats.json")), "{}"));
        f.engine->finish(JobOutcome::Succeeded, 0, QStringLiteral("ok"));
        QCOMPARE(f.state(part), JobState::Completed);
        QVERIFY(f.service->job(part).message.contains(QStringLiteral("Выравнивание готово")));
        QVariantMap details = f.service->jobDetails(part);
        QVERIFY(details.value("partial").toBool());
        QVERIFY(details.value("canResume").toBool());
        QVERIFY(details.value("missing").toStringList().isEmpty());
        QCOMPARE(details.value("quality").toMap().value("alignedImages").toInt(), 2);
        QVERIFY(f.service->resumeJob(part, QStringLiteral("fast"), {{"level", 3}}));
        QTRY_COMPARE(f.record->started.size(), 4);
        QCOMPARE(f.service->job(part).profile, QStringLiteral("fast"));
        QCOMPARE(f.service->job(part).arguments, (QStringList{QStringLiteral("--fast"), QStringLiteral("--level"), QStringLiteral("3")}));
        f.engine->finish(JobOutcome::Failed, 1, QStringLiteral("x"));

        // Intermediate files hold the runtime's paths: no resume in another runtime.
        f.engine->env.backend = QStringLiteral("other");
        QVERIFY(f.checkEnvironment());
        QSignalSpy errors(f.service.get(), &ProcessingService::errorOccurred);
        QVERIFY(!f.service->resumeJob(part));
        QCOMPARE(errors.count(), 1);
    }

    void passesStructuredOptionsAndNotes()
    {
        Fixture f;
        QVERIFY(f.init(2, false));
        QVERIFY(f.checkEnvironment());
        QVariantMap r = f.service->preflight(QStringLiteral("fast"), {}, {{"level", 2}});
        QVERIFY(r.value("ok").toBool());
        QCOMPARE(r.value("arguments").toString(), QStringLiteral("--fast --level 2"));
        QCOMPARE(r.value("notes").toStringList(), QStringList{QStringLiteral("no gps")});
        QCOMPARE(f.engine->lastDataset.images, 2);
        QCOMPARE(f.engine->lastDataset.withGps, 0);
        r = f.service->preflight(QStringLiteral("fast"), {}, {{"bad", 1}});
        QVERIFY(!r.value("ok").toBool());
        QVERIFY(r.value("errors").toStringList().contains(QStringLiteral("bad option bad")));

        const QString id = f.service->startJob(QStringLiteral("fast"), {}, {{"level", 2}});
        QCOMPARE(f.service->job(id).notes, QStringList{QStringLiteral("no gps")});
        QCOMPARE(f.service->job(id).backend, QStringLiteral("mock"));
        QTRY_COMPARE(f.state(id), JobState::Running);
        QTRY_VERIFY(QString::fromUtf8(readFile(f.jobFile(id, QStringLiteral("log.txt")))).contains(QStringLiteral("Автоматически: no gps")));
        QCOMPARE(readJson(f.jobFile(id, QStringLiteral("job.json"))).value("notes").toArray().size(), 1);
    }

    // ------------------------------------------------------------- OdmEngine

    // The engine is built into GeoMap AI: no Docker anywhere, old Docker jobs are refused clearly.
    void odmIsBuiltInWithoutDocker()
    {
        QTemporaryDir tmp;
        OdmEngine engine;
        engine.setNativeRoot(tmp.filePath(QStringLiteral("нет-odm")));
        QSignalSpy checked(&engine, &IProcessingEngine::environmentChecked);
        engine.validateEnvironment();
        QTRY_COMPARE(checked.count(), 1);
        const EnvironmentReport r = checked.first().first().value<EnvironmentReport>();
        QVERIFY(r.checked);
        QVERIFY(!r.available);
        QVERIFY(r.backend.isEmpty());
        QVERIFY(r.summary.contains(QStringLiteral("Встроенный OpenDroneMap не найден")));
        const QString all = (r.problems + r.hints + r.warnings + QStringList{r.summary, r.installTitle, r.installPrompt}).join(QLatin1Char('\n'));
        QVERIFY2(!all.contains(QStringLiteral("docker"), Qt::CaseInsensitive), qPrintable(all));

        ProcessingJob old;
        old.id = QStringLiteral("20261001-100000");
        old.backend = QStringLiteral("docker");   // written by 0.2.0
        QString error;
        QVERIFY(!engine.start(old, tmp.path(), &error));
        QVERIFY2(error.contains(QStringLiteral("0.2.0")), qPrintable(error));
        QVERIFY(!engine.isRunning(old.id));
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
        QCOMPARE(odm.profiles().size(), 4);
        // Every structured option of the UI is also accepted in the expert field.
        for (const QVariant &d : odm.optionDefinitions({})) {
            const QVariantMap def = d.toMap();
            QVERIFY(!def.value("title").toString().isEmpty());
            QVERIFY(OdmEngine::validateExtraArguments({QStringLiteral("--") + def.value("id").toString()}, &error)
                    || def.value("type").toString() != QLatin1String("flag"));
        }
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
        const QVector<ProcessingProfile> profiles = odm.profiles();
        EnvironmentReport env;
        env.cpus = 16;
        env.memoryBytes = 8 * gb;
        env.backend = QStringLiteral("native");
        JobRequest request;
        request.profile = &profiles[1];   // fast
        request.dataset = {20, 20, 1000};
        ProcessingJob job;
        QString error;
        QVERIFY(odm.createJob(job, request, env, &error));
        QCOMPARE(job.arguments.mid(job.arguments.size() - 2), (QStringList{QStringLiteral("--max-concurrency"), QStringLiteral("7")}));
        QCOMPARE(job.stageCount, int(OdmEngine::stageIds().size()));
        QCOMPARE(job.engineVersion, QStringLiteral("3.6.2"));
        QCOMPARE(job.backend, QStringLiteral("native"));
        ProcessingJob manual;
        request.extraArguments = {QStringLiteral("--max-concurrency"), QStringLiteral("2")};
        QVERIFY(odm.createJob(manual, request, env, &error));
        QCOMPARE(manual.arguments.count(QStringLiteral("--max-concurrency")), 1);
        request.extraArguments = {QStringLiteral("--evil")};
        QVERIFY(!odm.createJob(manual, request, env, &error));
        QVERIFY(odm.resourceWarnings(3, profiles[1], env).join(' ').contains(QStringLiteral("Снимков мало")));
        QVERIFY(odm.resourceWarnings(100, profiles[3], env).join(' ').contains(QStringLiteral("памят")));
        QVERIFY(odm.resourceWarnings(1500, profiles[1], env).join(' ').contains(QStringLiteral("Разбиение")));
    }

    void odmTurnsStructuredOptionsIntoArguments()
    {
        QString error;
        QStringList args = OdmEngine::optionArguments({{"feature-quality", "ultra"}, {"pc-filter", 2.5}, {"dsm", true},
                                                       {"dtm", false}, {"mesh-size", 300000}, {"gps-accuracy", ""}}, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(args, (QStringList{QStringLiteral("--dsm"), QStringLiteral("--feature-quality"), QStringLiteral("ultra"),
                                    QStringLiteral("--mesh-size"), QStringLiteral("300000"), QStringLiteral("--pc-filter"),
                                    QStringLiteral("2.5")}));
        QVERIFY(OdmEngine::optionArguments({{"feature-quality", "best"}}, &error).isEmpty());
        QVERIFY(error.contains(QStringLiteral("best")));
        QVERIFY(OdmEngine::optionArguments({{"pc-filter", 11}}, &error).isEmpty());
        QVERIFY(OdmEngine::optionArguments({{"mesh-size", 1000.5}}, &error).isEmpty());
        QVERIFY(OdmEngine::optionArguments({{"project-path", "/"}}, &error).isEmpty());
        QVERIFY(error.contains(QStringLiteral("project-path")));
        QVERIFY(OdmEngine::optionArguments({{"fast-orthophoto", true}}, &error).isEmpty());   // expert field only

        // Requests are adapted to the dataset and explained.
        OdmEngine odm;
        const QVector<ProcessingProfile> profiles = odm.profiles();
        EnvironmentReport env;
        env.backend = QStringLiteral("native");
        env.gpuName = QStringLiteral("NVIDIA Test");
        JobRequest request;
        request.profile = &profiles[2];   // balanced: has --auto-boundary
        request.dataset = {10, 0, 1000};
        request.options = {{"feature-type", "sift"}};
        ProcessingJob job;
        QVERIFY(odm.createJob(job, request, env, &error));
        QVERIFY(!job.arguments.contains(QStringLiteral("--auto-boundary")));
        QVERIFY(job.notes.join(' ').contains(QStringLiteral("auto-boundary")));
        QVERIFY(job.notes.join(' ').contains(QStringLiteral("NVIDIA Test")));
        request.options = {{"split", 400}};
        QVERIFY(!odm.createJob(job, request, env, &error));
        QVERIFY(error.contains(QStringLiteral("GPS")));
        // Settings override the profile: later options win in ODM's parser.
        request.dataset = {10, 10, 1000};
        request.options = {{"feature-quality", "lowest"}};
        QVERIFY(odm.createJob(job, request, env, &error));
        QCOMPARE(job.arguments[job.arguments.lastIndexOf(QStringLiteral("--feature-quality")) + 1], QStringLiteral("lowest"));

        QCOMPARE(odm.resumeArguments({QStringLiteral("--dsm"), QStringLiteral("--end-with"), QStringLiteral("opensfm"),
                                      QStringLiteral("--rerun-from"), QStringLiteral("odm_dem"), QStringLiteral("--rerun-all")}),
                 QStringList{QStringLiteral("--dsm")});
        // After a failure the stage is computed again; the one before it is known; crashes are
        // retried, running out of memory or space is not.
        QCOMPARE(odm.resumeArguments({QStringLiteral("--dsm"), QStringLiteral("--rerun-from"), QStringLiteral("opensfm")}, QStringLiteral("odm_georeferencing")),
                 (QStringList{QStringLiteral("--dsm"), QStringLiteral("--rerun-from"), QStringLiteral("odm_georeferencing")}));
        QCOMPARE(odm.resumeArguments({QStringLiteral("--dsm")}, QStringLiteral("unknown")), QStringList{QStringLiteral("--dsm")});
        QCOMPARE(odm.previousStage(QStringLiteral("odm_dem")), QStringLiteral("odm_georeferencing"));
        QVERIFY(odm.previousStage(QStringLiteral("dataset")).isEmpty());
        QVERIFY(odm.retryable(1, QStringLiteral("Сбой на этапе «Геопривязка»: PDAL при классификации точек земли (нужна для DTM) завершилась аварийно.")));
        QVERIFY(!odm.retryable(137, QStringLiteral("OpenDroneMap остановлен из-за нехватки памяти.")));
        QVERIFY(!odm.retryable(1, QStringLiteral("Обработка остановлена: на диске кончилось место.")));
    }

    void odmQualityReportFollowsStatsJson()
    {
        const QByteArray stats = R"({
          "processing_statistics": {"steps_times": {"Feature Extraction": 15.2, "Reconstruction": 6.5, "Total Time": 25.0}, "area": 7054.6},
          "reconstruction_statistics": {"components": 2, "has_gps": true, "has_gcp": false, "initial_shots_count": 18,
                                        "reconstructed_shots_count": 16, "reconstructed_points_count": 3251,
                                        "average_track_length": 3.04, "reprojection_error_pixels": 1.0827},
          "gps_errors": {"error": {"x": 0.1637, "y": 0.6004, "z": 0.1641}, "ce90": 0.9057, "le90": 0.2428},
          "gcp_errors": {},
          "odm_processing_statistics": {"average_gsd": 1.644},
          "point_cloud_statistics": {"dense": false, "stats": {"statistic": [{"count": 3237, "name": "X"}]}}
        })";
        const QVariantMap r = OdmEngine::parseQualityStats(stats);
        QCOMPARE(r.value("alignedImages").toInt(), 16);
        QCOMPARE(r.value("totalImages").toInt(), 18);
        QStringList rows;
        for (const QVariant &v : r.value("rows").toList())
            rows << v.toMap().value("label").toString() + QStringLiteral(": ") + v.toMap().value("value").toString();
        const QString all = rows.join('\n');
        QVERIFY2(all.contains(QStringLiteral("Выровнено снимков: 16 из 18")), qPrintable(all));
        QVERIFY(all.contains(QStringLiteral("Ошибка репроекции (RMS): 1,08 пикс")));
        QVERIFY(all.contains(QStringLiteral("Средний GSD: 1,64 см/пикс")));
        QVERIFY(QString(all).remove(QChar(0x00A0)).remove(QChar(0x202F)).contains(QStringLiteral("Площадь съёмки: 7055 м²")));
        QVERIFY(all.contains(QStringLiteral("Расхождение с GPS, RMSE X / Y / Z: 0,164 / 0,600 / 0,164 м")));
        QVERIFY(!all.contains(QStringLiteral("опорным")));   // no GCPs
        QVERIFY(QString(all).remove(QChar(0x00A0)).remove(QChar(0x202F)).contains(QStringLiteral("Точек в облаке (разреженном): 3237")));
        const QString warnings = r.value("warnings").toStringList().join('\n');
        QVERIFY(warnings.contains(QStringLiteral("Не выровнено снимков: 2")));
        QVERIFY(warnings.contains(QStringLiteral("2 несвязанных частей")));
        QCOMPARE(r.value("steps").toList().size(), 2);
        QVERIFY(OdmEngine::parseQualityStats("not json").isEmpty());
    }

    // ------------------------------------------------------- native runtime

    void nativeRuntimeIsPreparedLikeOdmScripts()
    {
        QTemporaryDir tmp;
        const QString root = tmp.filePath(QStringLiteral("odm"));
        const QProcessEnvironment base = [] {
            QProcessEnvironment e;
            e.insert(QStringLiteral("SystemRoot"), QStringLiteral("C:\\Windows"));
            e.insert(QStringLiteral("PYTHONHOME"), QStringLiteral("C:\\Python"));
            e.insert(QStringLiteral("PATH"), QStringLiteral("C:\\Other"));
            return e;
        }();
        const QProcessEnvironment env = OdmEngine::nativeEnvironment(root, tmp.filePath(QStringLiteral("cache")), base);
        const QString r = QDir::toNativeSeparators(root);
        QVERIFY(env.value(QStringLiteral("PATH")).startsWith(r + QStringLiteral("\\venv\\Scripts;") + r + QStringLiteral("\\SuperBuild\\install\\bin;")));
        QVERIFY(!env.value(QStringLiteral("PATH")).contains(QStringLiteral("C:\\Other")));
        QCOMPARE(env.value(QStringLiteral("GDAL_DATA")), r + QStringLiteral("\\SuperBuild\\install\\bin\\data\\gdal"));
        QCOMPARE(env.value(QStringLiteral("PROJ_LIB")), r + QStringLiteral("\\SuperBuild\\install\\bin\\data\\proj"));
        QCOMPARE(env.value(QStringLiteral("VIRTUAL_ENV")), r + QStringLiteral("\\venv"));
        QVERIFY(!env.contains(QStringLiteral("PYTHONHOME")));
        QCOMPARE(env.value(QStringLiteral("PYTHONUNBUFFERED")), QStringLiteral("1"));

        ProcessingJob job;
        job.id = QStringLiteral("20261014-150000");
        job.arguments = {QStringLiteral("--fast-orthophoto")};
        QCOMPARE(OdmEngine::nativeArguments(job, root, QStringLiteral("C:/work")),
                 (QStringList{QStringLiteral("-X"), QStringLiteral("utf8"), QDir::toNativeSeparators(root + QStringLiteral("/run.py")),
                              QStringLiteral("--project-path"), QStringLiteral("C:\\work").replace('\\', QDir::separator()),
                              QStringLiteral("20261014-150000"), QStringLiteral("--fast-orthophoto")}));

        // A moved installation: pyvenv.cfg and OpenCV's loader point at the new folder afterwards.
        QVERIFY(writeFile(root + QStringLiteral("/venv/pyvenv.cfg"), "home = D:\\old\\venv\\Scripts\ninclude-system-site-packages = false\n"));
        QVERIFY(writeFile(root + QStringLiteral("/venv/Lib/site-packages/cv2/config-3.12.py"), "old"));
        QString error;
        QVERIFY2(OdmEngine::patchNativeVirtualEnv(root, &error), qPrintable(error));
        const QString cfg = QString::fromUtf8(readFile(root + QStringLiteral("/venv/pyvenv.cfg")));
        QVERIFY(cfg.startsWith(QStringLiteral("home = ") + r + QStringLiteral("\\venv\\Scripts")));
        QVERIFY(QString::fromUtf8(readFile(root + QStringLiteral("/venv/Lib/site-packages/cv2/config.py"))).contains(r + QStringLiteral("\\SuperBuild\\install\\bin")));
        QVERIFY(QString::fromUtf8(readFile(root + QStringLiteral("/venv/Lib/site-packages/cv2/config-3.12.py"))).contains(QStringLiteral("python-3.12")));
        QVERIFY(OdmEngine::patchNativeVirtualEnv(root, &error));   // already current: nothing to do

#ifdef Q_OS_WIN
        QVERIFY(OdmEngine::nativeProblem(root).contains(QStringLiteral("run.py")));
        for (const char *f : {"run.py", "VERSION", "venv/Scripts/python.exe"}) QVERIFY(writeFile(root + QLatin1Char('/') + QLatin1String(f), "3.5.1"));
        for (const char *d : {"SuperBuild/install/bin", "opendm", "stages"}) QVERIFY(QDir().mkpath(root + QLatin1Char('/') + QLatin1String(d)));
        QVERIFY(OdmEngine::nativeProblem(root).contains(QStringLiteral("3.5.1")));
        QVERIFY(writeFile(root + QStringLiteral("/VERSION"), "3.6.2\n"));
        QCOMPARE(OdmEngine::nativeProblem(root), QString());
        qputenv("GEOMAP_ODM_HOME", root.toLocal8Bit());
        QCOMPARE(OdmEngine::nativeCandidates().constFirst(), QDir::cleanPath(root));
        qunsetenv("GEOMAP_ODM_HOME");
#else
        QVERIFY(!OdmEngine::nativeProblem(root).isEmpty());   // Windows only
#endif
    }

    void directoryLinksGiveToolsAnAsciiPath()
    {
        QTemporaryDir tmp;
        QVERIFY(Platform::isToolSafePath(QStringLiteral("C:\\Users\\JOHNSM~1\\AppData\\Local\\GeoMapAI")));
        QVERIFY(!Platform::isToolSafePath(QStringLiteral("C:\\Users\\Андрей\\GeoMapAI")));
        QVERIFY(!Platform::isToolSafePath(QStringLiteral("C:\\Program Files\\GeoMapAI")));
        QVERIFY(!Platform::isToolSafePath(QString()));

        const QString target = tmp.filePath(QStringLiteral("Проект с пробелом/jobs/1"));
        QVERIFY(writeFile(target + QStringLiteral("/images/a.jpg"), "photo"));
        const QString link = tmp.filePath(QStringLiteral("work/1"));
        QString error;
        QVERIFY2(Platform::createDirectoryLink(link, target, &error), qPrintable(error));
        QVERIFY(Platform::createDirectoryLink(link, target, &error));   // kept
        QCOMPARE(QDir::cleanPath(Platform::directoryLinkTarget(link)).toLower(), QDir::cleanPath(QFileInfo(target).absoluteFilePath()).toLower());
        QCOMPARE(readFile(link + QStringLiteral("/images/a.jpg")), QByteArray("photo"));
        // Tools create, rename and replace files through the link.
        QVERIFY(writeFile(link + QStringLiteral("/out.tmp"), "result"));
        QVERIFY(QFile::rename(link + QStringLiteral("/out.tmp"), link + QStringLiteral("/out.tif")));
        QCOMPARE(readFile(target + QStringLiteral("/out.tif")), QByteArray("result"));
#ifdef Q_OS_WIN
        // As ODM's point classification does (Python os.remove, then os.rename = MoveFileExW
        // without flags) in a subfolder: the georeferencing stage of the high-quality profile.
        {
            QVERIFY(writeFile(link + QStringLiteral("/odm_georeferencing/model.laz"), "old"));
            QVERIFY(writeFile(link + QStringLiteral("/odm_georeferencing/model.classified.laz"), "new"));
            const std::wstring from = QDir::toNativeSeparators(link + QStringLiteral("/odm_georeferencing/model.classified.laz")).toStdWString();
            const std::wstring to = QDir::toNativeSeparators(link + QStringLiteral("/odm_georeferencing/model.laz")).toStdWString();
            QVERIFY(DeleteFileW(to.c_str()));
            const BOOL moved = MoveFileExW(from.c_str(), to.c_str(), 0);
            const DWORD code = GetLastError();
            QProcess fsutil;
            fsutil.start(QStringLiteral("fsutil"), {QStringLiteral("reparsepoint"), QStringLiteral("query"), QDir::toNativeSeparators(link)});
            fsutil.waitForFinished(10000);
            QVERIFY2(moved, qPrintable(QStringLiteral("MoveFileExW: %1\n%2").arg(code).arg(QString::fromLocal8Bit(fsutil.readAllStandardOutput()))));
            QCOMPARE(readFile(target + QStringLiteral("/odm_georeferencing/model.laz")), QByteArray("new"));
        }
#endif

        QVERIFY(!Platform::removeDirectoryLink(target));   // not a link: refused
        QVERIFY(Platform::removeDirectoryLink(link));
        QVERIFY(!QFileInfo::exists(link));
        QCOMPARE(readFile(target + QStringLiteral("/images/a.jpg")), QByteArray("photo"));   // contents untouched
        QVERIFY(!Platform::createDirectoryLink(link, tmp.filePath(QStringLiteral("missing")), &error));

        // An ODM folder under a non-Latin path is used through an ASCII link in the data folder.
        Platform::setDataDirectory(tmp.filePath(QStringLiteral("data")));
        const QString odm = tmp.filePath(QStringLiteral("Загрузки/GeoMapAI/engines/odm"));
        QVERIFY(QDir().mkpath(odm));
        const QString effective = OdmEngine::effectiveNativeRoot(odm, &error);
        QVERIFY2(effective.endsWith(QStringLiteral("engines/odm-link")), qPrintable(effective + error));
        QVERIFY(Platform::isToolSafePath(QDir::toNativeSeparators(effective)) || !Platform::isToolSafePath(tmp.path()));
        QCOMPARE(QDir::cleanPath(Platform::directoryLinkTarget(effective)).toLower(), QDir::cleanPath(odm).toLower());
        const QString ascii = tmp.filePath(QStringLiteral("ascii/odm"));
        QVERIFY(QDir().mkpath(ascii));
        if (Platform::isToolSafePath(QDir::toNativeSeparators(ascii))) QCOMPARE(OdmEngine::effectiveNativeRoot(ascii, &error), ascii);
        QVERIFY(Platform::removeDirectoryLink(effective));
        Platform::setDataDirectory({});
    }

    void processTreeEndsGrandchildren()
    {
        QProcess process;
        ProcessTree::prepare(process);
#ifdef Q_OS_WIN
        // cmd.exe starts ping.exe: a grandchild of this test.
        process.start(QStringLiteral("cmd.exe"), {QStringLiteral("/c"), QStringLiteral("ping -n 60 127.0.0.1 >nul")});
#else
        process.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), QStringLiteral("sleep 60 & wait")});
#endif
        QVERIFY(process.waitForStarted(10000));
        ProcessTree tree;
        QVERIFY(tree.attach(process));
#ifdef Q_OS_WIN
        QTRY_VERIFY_WITH_TIMEOUT(tree.activeProcesses() >= 2, 10000);
#endif
        QElapsedTimer timer;
        timer.start();
        tree.terminate();
        QVERIFY(process.waitForFinished(10000));
        QVERIFY(timer.elapsed() < 5000);
#ifdef Q_OS_WIN
        QTRY_COMPARE_WITH_TIMEOUT(tree.activeProcesses(), 0, 5000);
#endif
    }

    void downloaderAcceptsOnlyTheExpectedFile()
    {
        QTemporaryDir tmp;
        const QByteArray payload = QByteArray(300000, 'x') + "end";
        const QString source = tmp.filePath(QStringLiteral("source.bin"));
        QVERIFY(writeFile(source, payload));
        const QByteArray sha = QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex();
        const QString target = tmp.filePath(QStringLiteral("загрузки/file.bin"));

        Downloader bad;
        QSignalSpy badDone(&bad, &Downloader::finished);
        bad.start(QUrl::fromLocalFile(source), target, QByteArray(64, '0'));
        QVERIFY(badDone.count() == 1 || badDone.wait(10000));
        QCOMPARE(badDone.at(0).at(0).toBool(), false);
        QVERIFY(badDone.at(0).at(1).toString().contains(QStringLiteral("Контрольная сумма")));
        QVERIFY(!QFileInfo::exists(target));
        QVERIFY(!QFileInfo::exists(target + QStringLiteral(".part")));

        Downloader good;
        QSignalSpy goodDone(&good, &Downloader::finished);
        good.start(QUrl::fromLocalFile(source), target, sha);
        QVERIFY(goodDone.count() == 1 || goodDone.wait(10000));
        QCOMPARE(goodDone.at(0).at(0).toBool(), true);
        QCOMPARE(readFile(target), payload);
        QCOMPARE(Downloader::fileSha256(target), sha);

        // An existing file with the right hash is reused without downloading.
        Downloader again;
        QSignalSpy againDone(&again, &Downloader::finished);
        again.start(QUrl::fromLocalFile(tmp.filePath(QStringLiteral("gone.bin"))), target, sha);
        QCOMPARE(againDone.count(), 1);
        QCOMPARE(againDone.at(0).at(0).toBool(), true);

        Downloader missing;
        QSignalSpy missingDone(&missing, &Downloader::finished);
        missing.start(QUrl::fromLocalFile(tmp.filePath(QStringLiteral("gone.bin"))), tmp.filePath(QStringLiteral("x.bin")), sha);
        QVERIFY(missingDone.count() == 1 || missingDone.wait(10000));
        QCOMPARE(missingDone.at(0).at(0).toBool(), false);
        QVERIFY(!QFileInfo::exists(tmp.filePath(QStringLiteral("x.bin.part"))));
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
        QVector<Artifact> outputs = odm.collectOutputs(tmp.path());
        QCOMPARE(outputs.size(), 4);
        QCOMPARE(outputs[0].kind, QStringLiteral("orthophoto"));
        QCOMPARE(outputs[0].details, QStringLiteral("ODM, EPSG:32630, 5,0 см/пикс, 100×80, каналов: 4"));
        QCOMPARE(outputs[1].kind, QStringLiteral("pointcloud"));
        QCOMPARE(outputs[1].details, QStringLiteral("LAS 1.2, точек: 12345"));
        QCOMPARE(outputs[2].kind, QStringLiteral("mesh"));
        QCOMPARE(outputs[2].file, QStringLiteral("odm_texturing_25d/odm_textured_model_geo.obj"));
        QCOMPARE(outputs[3].kind, QStringLiteral("report"));
        QCOMPARE(outputs[3].details, QStringLiteral("PDF-отчёт ODM"));
        // GeoMap AI's true orthophoto comes first: it is the project's orthophoto.
        QVERIFY(writeFile(dir.filePath(QStringLiteral("geomap/true_orthophoto.tif")), geoTiff(false, false, 3072, 32630, 0.02)));
        outputs = odm.collectOutputs(tmp.path());
        QCOMPARE(outputs.size(), 5);
        QCOMPARE(outputs[0].file, QStringLiteral("geomap/true_orthophoto.tif"));
        QVERIFY2(outputs[0].details.startsWith(QStringLiteral("истинный (GeoMap AI), EPSG:32630, 2,0 см/пикс")), qPrintable(outputs[0].details));
        QCOMPARE(outputs[1].file, QStringLiteral("odm_orthophoto/odm_orthophoto.tif"));
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
