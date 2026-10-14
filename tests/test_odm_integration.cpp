// Real reconstruction through ProcessingService + OdmEngine with a folder of overlapping drone
// photos (GEOMAP_ODM_DATASET), in the runtime the engine selects: the built-in ODM (Windows) or
// Docker; GEOMAP_ODM_BACKEND=native|docker forces one. Registered with ctest only when configured
// with -DGEOMAP_ODM_DATASET=<folder>; set GEOMAP_ODM_KEEP=1 to keep the project.
#include <QtTest>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <memory>
#include "ImportJob.h"
#include "OdmEngine.h"
#include "PhotoModel.h"
#include "ProcessingService.h"

class OdmIntegrationTest : public QObject
{
    Q_OBJECT
private:
    QTemporaryDir m_tmp{QDir::temp().filePath(QStringLiteral("GeoMap ODM тест-XXXXXX"))};
    QString m_project;
    PhotoModel m_photos;
    std::unique_ptr<ProcessingService> m_service;
    OdmEngine *m_engine = nullptr;

    bool containerExists(const QString &jobId) const
    {
        QProcess docker;
        docker.start(m_engine->dockerPath(), {QStringLiteral("ps"), QStringLiteral("-a"), QStringLiteral("-q"),
                                               QStringLiteral("--filter"), QStringLiteral("name=") + OdmEngine::containerName(jobId)});
        docker.waitForFinished(30000);
        return !docker.readAllStandardOutput().trimmed().isEmpty();
    }

private slots:
    void initTestCase()
    {
        const QString dataset = qEnvironmentVariable("GEOMAP_ODM_DATASET");
        if (dataset.isEmpty()) QSKIP("GEOMAP_ODM_DATASET is not set");
        QVERIFY(QFileInfo(dataset).isDir());
        QVERIFY(m_tmp.isValid());
        m_tmp.setAutoRemove(qEnvironmentVariableIsEmpty("GEOMAP_ODM_KEEP"));
        m_project = m_tmp.filePath(QStringLiteral("Проект Brighton"));
        QVERIFY(QDir().mkpath(m_project + QStringLiteral("/photos")));

        ImportJob::Request request;
        request.folder = QFileInfo(dataset).canonicalFilePath();
        request.projectDir = QFileInfo(m_project).canonicalFilePath();
        ImportJob import(request);
        QSignalSpy done(&import, &ImportJob::finished);
        import.start();
        QVERIFY(done.wait(600000));
        const QVector<Photo> photos = import.takeResults();
        QVERIFY2(photos.size() >= 2, qUtf8Printable(import.issues().join('\n')));
        m_photos.resetPhotos(photos, request.projectDir);
        qInfo("Imported %d photos into %s", int(photos.size()), qUtf8Printable(QDir::toNativeSeparators(m_project)));

        m_engine = new OdmEngine;
        m_service = std::make_unique<ProcessingService>(m_engine);
        m_service->setProject(request.projectDir, &m_photos);
        m_service->checkEnvironment();
        QTRY_VERIFY_WITH_TIMEOUT(m_service->environmentChecked() && !m_service->checkingEnvironment(), 120000);
        QVERIFY2(m_service->engineAvailable(), qUtf8Printable(m_service->environmentProblems().join('\n')));
        qInfo("%s", qUtf8Printable(m_service->environmentSummary()));
    }

    // With the image already present, `docker pull <tag>@<digest>` only confirms it (no download).
    void installRuntimeConfirmsThePinnedImage()
    {
        if (m_service->backend() != QLatin1String("docker"))
            QSKIP("Built-in ODM is installed; its installation is covered by the UI acceptance run");
        QSignalSpy env(m_service.get(), &ProcessingService::environmentChanged);
        m_service->installRuntime();
        QVERIFY(m_service->installing());
        QTRY_VERIFY_WITH_TIMEOUT(!m_service->installing(), 600000);
        QVERIFY2(m_service->installStatus().contains(QStringLiteral("загружен")), qUtf8Printable(m_service->installStatus()));
        QTRY_VERIFY_WITH_TIMEOUT(env.count() > 0 && !m_service->checkingEnvironment(), 120000);
        QVERIFY(m_service->engineAvailable());
    }

    void cancelStopsTheContainer()
    {
        const bool docker = m_service->backend() == QLatin1String("docker");
        const QString id = m_service->startJob(QStringLiteral("fast"));
        QVERIFY(!id.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(m_service->job(id).state == JobState::Running && !m_service->job(id).stage.isEmpty(), 300000);
        if (docker) QVERIFY(containerExists(id));
        m_service->cancelJob(id);
        QCOMPARE(m_service->job(id).state, JobState::Cancelling);
        QTRY_COMPARE_WITH_TIMEOUT(m_service->job(id).state, JobState::Cancelled, 120000);
        if (docker) QVERIFY(!containerExists(id));
        QVERIFY(m_service->deleteJob(id));
    }

    void fastProfileProducesAGeoreferencedOrthophoto()
    {
        QSignalSpy finished(m_service.get(), &ProcessingService::jobFinished);
        QStringList stages;
        connect(m_service.get(), &ProcessingService::jobUpdated, this, [&](const QString &id) {
            const QString stage = m_service->job(id).stage;
            if (!stage.isEmpty() && !stages.contains(stage)) stages << stage;
        });
        // The engine reads the photos through links; their bytes must not change.
        QHash<QString, QByteArray> before;
        for (const Photo &p : m_photos.photos()) {
            QFile f(QDir(m_project).filePath(p.file));
            QVERIFY(f.open(QIODevice::ReadOnly));
            before.insert(p.file, QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256));
        }
        QElapsedTimer timer;
        timer.start();
        const QString id = m_service->startJob(QStringLiteral("fast"));
        QVERIFY(!id.isEmpty());
        QVERIFY(finished.wait(2 * 3600 * 1000));
        const ProcessingJob job = m_service->job(id);
        qInfo("Job %s: %s in %lld s; stages: %s", qUtf8Printable(id), qUtf8Printable(jobStateText(job.state)),
              timer.elapsed() / 1000, qUtf8Printable(stages.join(QStringLiteral(", "))));
        QVERIFY2(job.state == JobState::Completed, qUtf8Printable(job.message));
        QCOMPARE(job.exitCode, 0);
        QVERIFY(stages.contains(QStringLiteral("opensfm")));

        const Artifact *ortho = nullptr;
        for (const Artifact &a : job.outputs) {
            qInfo("  %s  %s  %lld bytes  %s", qUtf8Printable(a.kind), qUtf8Printable(a.file), a.bytes, qUtf8Printable(a.details));
            if (a.kind == QLatin1String("orthophoto")) ortho = &a;
        }
        QVERIFY(ortho);
        QVERIFY2(ortho->details.startsWith(QStringLiteral("EPSG:")), qUtf8Printable(ortho->details));
        const QString log = QDir(m_service->jobDirectory(id)).filePath(QStringLiteral("log.txt"));
        QVERIFY(QFileInfo(log).size() > 1000);
        if (m_service->backend() == QLatin1String("docker")) QVERIFY(!containerExists(id));
        for (auto it = before.cbegin(); it != before.cend(); ++it) {
            QFile f(QDir(m_project).filePath(it.key()));
            QVERIFY(f.open(QIODevice::ReadOnly));
            QCOMPARE(QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256), it.value());
        }
        const QVariantMap quality = m_engine->qualityReport(m_service->jobDirectory(id));
        QVERIFY(quality.value(QStringLiteral("alignedImages")).toInt() > 0);
        qInfo("Quality: %d of %d images aligned, reprojection %.2f px, GSD %.2f cm",
              quality.value(QStringLiteral("alignedImages")).toInt(), quality.value(QStringLiteral("totalImages")).toInt(),
              quality.value(QStringLiteral("reprojectionPx")).toDouble(), quality.value(QStringLiteral("gsdCm")).toDouble());
    }
};

QTEST_GUILESS_MAIN(OdmIntegrationTest)
#include "test_odm_integration.moc"
