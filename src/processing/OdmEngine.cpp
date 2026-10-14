#include "OdmEngine.h"
#include "GeoTiffInfo.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QtEndian>

namespace {
constexpr int RecentLines = 200;
constexpr qint64 GiB = 1024LL * 1024 * 1024;

QString gigabytes(qint64 bytes)
{
    return QLocale(QLocale::Russian).toString(double(bytes) / GiB, 'f', 1);
}

QStringList bindMount(const QString &source, const QString &target, bool readOnly, bool windowsHost)
{
    if (windowsHost) {
        // Docker on Windows parses the drive letter of "C:\dir:/target[:ro]" itself;
        // Windows paths cannot contain other colons.
        return {QStringLiteral("-v"), source + QLatin1Char(':') + target + (readOnly ? QStringLiteral(":ro") : QString())};
    }
    // --mount is CSV: quote the source field when it contains a comma or a quote.
    QString src = QStringLiteral("source=") + source;
    if (src.contains(QLatin1Char(',')) || src.contains(QLatin1Char('"')))
        src = QLatin1Char('"') + src.replace(QLatin1String("\""), QLatin1String("\"\"")) + QLatin1Char('"');
    QString spec = QStringLiteral("type=bind,") + src + QStringLiteral(",target=") + target;
    if (readOnly) spec += QStringLiteral(",readonly");
    return {QStringLiteral("--mount"), spec};
}

// "LAS/LAZ, N точек" from the public header block, or empty if the magic is wrong.
QString describeLas(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QByteArray h = file.read(255);
    if (h.size() < 227 || !h.startsWith("LASF")) return QStringLiteral("повреждён: нет сигнатуры LASF");
    const auto *u = reinterpret_cast<const uchar *>(h.constData());
    const int major = h[24], minor = h[25];
    quint64 points = qFromLittleEndian<quint32>(u + 107);
    if (points == 0 && minor >= 4 && h.size() >= 255) points = qFromLittleEndian<quint64>(u + 247);
    return QStringLiteral("LAS %1.%2, точек: %3").arg(major).arg(minor).arg(points);
}
} // namespace

OdmEngine::OdmEngine(QObject *parent) : IProcessingEngine(parent), m_docker(findDocker())
{
}

OdmEngine::~OdmEngine()
{
    abort(m_runningJob);
}

QString OdmEngine::findDocker()
{
    // Explicit client, e.g. a Docker install outside PATH (docs/PROCESSING_ODM.md).
    const QString custom = qEnvironmentVariable("GEOMAP_DOCKER");
    if (!custom.isEmpty()) return custom;
    QString docker = QStandardPaths::findExecutable(QStringLiteral("docker"));
#ifdef Q_OS_WIN
    if (docker.isEmpty()) {
        // Docker Desktop per-user and per-machine installs; PATH may be stale in a running session.
        const QStringList candidates = {
            QDir(qEnvironmentVariable("LOCALAPPDATA")).filePath(QStringLiteral("Programs/DockerDesktop/resources/bin/docker.exe")),
            QDir(qEnvironmentVariable("ProgramFiles")).filePath(QStringLiteral("Docker/Docker/resources/bin/docker.exe")),
        };
        for (const QString &c : candidates) if (QFileInfo(c).isFile()) return c;
    }
#endif
    return docker;
}

QString OdmEngine::imageReference()
{
    return QStringLiteral("opendronemap/odm@") + QLatin1String(ImageDigest);
}

EngineCapabilities OdmEngine::capabilities() const
{
    EngineCapabilities c;
    c.id = QStringLiteral("odm");
    c.displayName = QStringLiteral("OpenDroneMap (Docker)");
    c.version = QLatin1String(Version);
    c.supportsCancel = true;
    c.supportsPause = false;
    c.outputs = {QStringLiteral("orthophoto"), QStringLiteral("dsm"), QStringLiteral("dtm"),
                 QStringLiteral("pointcloud"), QStringLiteral("mesh"), QStringLiteral("report")};
    // opendm/context.py supported_extensions in ODM 3.6.2, minus raw formats GeoMap AI does not import.
    c.inputSuffixes = {QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("png"),
                       QStringLiteral("tif"), QStringLiteral("tiff")};
    c.minimumImages = 2;   // structure from motion needs at least one overlapping pair
    return c;
}

QStringList OdmEngine::resourceWarnings(int imageCount, const ProcessingProfile &profile,
                                        const EnvironmentReport &env) const
{
    QStringList warnings;
    if (imageCount < 5)
        warnings << QStringLiteral("Снимков мало (%1): реконструкция возможна только при большом перекрытии кадров.")
                        .arg(imageCount);
    if (profile.id == QLatin1String("high") && env.memoryBytes > 0 && env.memoryBytes < 16 * GiB)
        warnings << QStringLiteral("«Высокое качество» при %1 ГБ памяти Docker может завершиться нехваткой памяти (код 137).")
                        .arg(gigabytes(env.memoryBytes));
    return warnings;
}

QVector<ProcessingProfile> OdmEngine::profiles() const
{
    // Documented in docs/PROCESSING_ODM.md; keep both in sync.
    return {
        {QStringLiteral("fast"), QStringLiteral("Быстрый просмотр"),
         QStringLiteral("Ортофотоплан по разреженной реконструкции, 10 см/пикс. Без плотного облака и рельефа."),
         {QStringLiteral("--fast-orthophoto"), QStringLiteral("--feature-quality"), QStringLiteral("medium"),
          QStringLiteral("--orthophoto-resolution"), QStringLiteral("10")}, 3},
        {QStringLiteral("balanced"), QStringLiteral("Сбалансированный"),
         QStringLiteral("Плотное облако среднего качества, ЦМП (DSM), ортофотоплан 5 см/пикс."),
         {QStringLiteral("--feature-quality"), QStringLiteral("high"), QStringLiteral("--pc-quality"), QStringLiteral("medium"),
          QStringLiteral("--dsm"), QStringLiteral("--orthophoto-resolution"), QStringLiteral("5"),
          QStringLiteral("--dem-resolution"), QStringLiteral("10"), QStringLiteral("--auto-boundary")}, 8},
        {QStringLiteral("high"), QStringLiteral("Высокое качество"),
         QStringLiteral("Максимальные признаки, плотное облако высокого качества, DSM и DTM, ортофотоплан 2 см/пикс. Долго и требует много памяти."),
         {QStringLiteral("--feature-quality"), QStringLiteral("ultra"), QStringLiteral("--pc-quality"), QStringLiteral("high"),
          QStringLiteral("--dsm"), QStringLiteral("--dtm"), QStringLiteral("--orthophoto-resolution"), QStringLiteral("2"),
          QStringLiteral("--dem-resolution"), QStringLiteral("5"), QStringLiteral("--mesh-octree-depth"), QStringLiteral("12"),
          QStringLiteral("--auto-boundary")}, 15},
    };
}

QStringList OdmEngine::stageIds()
{
    return {QStringLiteral("dataset"), QStringLiteral("split"), QStringLiteral("merge"), QStringLiteral("opensfm"),
            QStringLiteral("openmvs"), QStringLiteral("odm_filterpoints"), QStringLiteral("odm_meshing"),
            QStringLiteral("mvs_texturing"), QStringLiteral("odm_georeferencing"), QStringLiteral("odm_dem"),
            QStringLiteral("odm_orthophoto"), QStringLiteral("odm_report"), QStringLiteral("odm_postprocess")};
}

QString OdmEngine::stageTitle(const QString &stage) const
{
    static const QHash<QString, QString> titles = {
        {QStringLiteral("dataset"), QStringLiteral("Загрузка снимков")},
        {QStringLiteral("split"), QStringLiteral("Разбиение на подмодели")},
        {QStringLiteral("merge"), QStringLiteral("Слияние подмоделей")},
        {QStringLiteral("opensfm"), QStringLiteral("Структура из движения (OpenSfM)")},
        {QStringLiteral("openmvs"), QStringLiteral("Плотное облако точек (OpenMVS)")},
        {QStringLiteral("odm_filterpoints"), QStringLiteral("Фильтрация точек")},
        {QStringLiteral("odm_meshing"), QStringLiteral("Построение сетки")},
        {QStringLiteral("mvs_texturing"), QStringLiteral("Текстурирование")},
        {QStringLiteral("odm_georeferencing"), QStringLiteral("Геопривязка")},
        {QStringLiteral("odm_dem"), QStringLiteral("Модели рельефа (DSM/DTM)")},
        {QStringLiteral("odm_orthophoto"), QStringLiteral("Ортофотоплан")},
        {QStringLiteral("odm_report"), QStringLiteral("Отчёт")},
        {QStringLiteral("odm_postprocess"), QStringLiteral("Завершение")},
    };
    return titles.value(stage, stage);
}

QString OdmEngine::containerName(const QString &jobId)
{
    return QStringLiteral("geomap-odm-") + jobId;
}

QStringList OdmEngine::runArguments(const ProcessingJob &job, const QString &jobDirectory, bool windowsHost)
{
    QString root = QDir(jobDirectory).absolutePath();
    QString images = root + QStringLiteral("/images");
    if (windowsHost) {
        root = QDir::toNativeSeparators(root);
        images = QDir::toNativeSeparators(images);
    }
    QStringList args{QStringLiteral("run"), QStringLiteral("--rm"), QStringLiteral("--name"), containerName(job.id)};
    args += bindMount(root, QStringLiteral("/datasets/code"), false, windowsHost);
    // Input photos read-only: they are hard links to the project copies.
    args += bindMount(images, QStringLiteral("/datasets/code/images"), true, windowsHost);
    args << imageReference() << QStringLiteral("--project-path") << QStringLiteral("/datasets") << QStringLiteral("code");
    args += job.arguments;
    return args;
}

QString OdmEngine::stripAnsi(const QString &line)
{
    static const QRegularExpression ansi(QStringLiteral("\x1B\\[[0-9;]*[A-Za-z]"));
    QString clean = line;
    clean.remove(ansi);
    return clean;
}

QString OdmEngine::parseStage(const QString &line)
{
    static const QRegularExpression running(QStringLiteral("\\bRunning ([a-z_]+) stage\\b"));
    const QRegularExpressionMatch m = running.match(line);
    return m.hasMatch() ? m.captured(1) : QString();
}

bool OdmEngine::validateExtraArguments(const QStringList &arguments, QString *error)
{
    static const QSet<QString> flags = {
        QStringLiteral("--dsm"), QStringLiteral("--dtm"), QStringLiteral("--fast-orthophoto"),
        QStringLiteral("--skip-3dmodel"), QStringLiteral("--skip-report"), QStringLiteral("--auto-boundary"),
        QStringLiteral("--use-3dmesh"), QStringLiteral("--pc-ept")};
    static const QSet<QString> qualities = {QStringLiteral("ultra"), QStringLiteral("high"), QStringLiteral("medium"),
                                            QStringLiteral("low"), QStringLiteral("lowest")};
    auto fail = [error](const QString &message) {
        if (error) *error = message;
        return false;
    };
    for (int i = 0; i < arguments.size(); ++i) {
        const QString &a = arguments[i];
        if (flags.contains(a)) continue;
        const bool valued = a == QLatin1String("--feature-quality") || a == QLatin1String("--pc-quality")
            || a == QLatin1String("--orthophoto-resolution") || a == QLatin1String("--dem-resolution")
            || a == QLatin1String("--min-num-features") || a == QLatin1String("--mesh-octree-depth")
            || a == QLatin1String("--max-concurrency");
        if (!valued)
            return fail(QStringLiteral("Параметр «%1» не поддерживается. Допустимы: --feature-quality, --pc-quality, "
                                       "--orthophoto-resolution, --dem-resolution, --min-num-features, --mesh-octree-depth, "
                                       "--max-concurrency, --dsm, --dtm, --fast-orthophoto, --skip-3dmodel, --skip-report, "
                                       "--auto-boundary, --use-3dmesh, --pc-ept.").arg(a));
        if (i + 1 >= arguments.size()) return fail(QStringLiteral("Для «%1» не указано значение.").arg(a));
        const QString v = arguments[++i];
        bool ok = false;
        if (a.endsWith(QLatin1String("-quality"))) ok = qualities.contains(v);
        else if (a.endsWith(QLatin1String("-resolution"))) { const double d = v.toDouble(&ok); ok = ok && d > 0 && d <= 10000; }
        else if (a == QLatin1String("--mesh-octree-depth")) { const int n = v.toInt(&ok); ok = ok && n >= 1 && n <= 14; }
        else { const int n = v.toInt(&ok); ok = ok && n > 0 && n <= 1000000; }
        if (!ok) return fail(QStringLiteral("Недопустимое значение «%1» для «%2».").arg(v, a));
    }
    return true;
}

int OdmEngine::concurrencyFor(int cpus, qint64 memoryBytes)
{
    // ODM needs roughly 1 GB per thread (ODM help for --max-concurrency); keep 1 GB spare.
    int n = cpus > 0 ? cpus : QThread::idealThreadCount();
    const int memoryGb = int(memoryBytes / GiB);
    if (memoryGb > 1) n = qMin(n, memoryGb - 1);
    return qMax(1, n);
}

void OdmEngine::runDocker(const QStringList &arguments, int timeoutMs, DockerDone done)
{
    auto *process = new QProcess(this);
    auto *timer = new QTimer(process);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, process, [process] { process->kill(); });
    connect(process, &QProcess::finished, this, [process, done](int code, QProcess::ExitStatus status) {
        const QByteArray out = process->readAllStandardOutput();
        process->deleteLater();
        done(status == QProcess::NormalExit ? code : -1, out);
    });
    connect(process, &QProcess::errorOccurred, this, [process, done](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;   // other errors also emit finished()
        process->deleteLater();
        done(-2, {});
    });
    process->start(m_docker, arguments);
    timer->start(timeoutMs);
}

void OdmEngine::validateEnvironment()
{
    EnvironmentReport report;
    report.checked = true;
    report.engineVersion = QLatin1String(Version);
    if (m_docker.isEmpty()) m_docker = findDocker();
    if (m_docker.isEmpty() || !QFileInfo(m_docker).isFile()) {
        report.engineMissing = true;
        report.summary = QStringLiteral("Docker не найден");
        report.problems << QStringLiteral("Docker не найден. Он нужен только для фотограмметрической обработки; "
                                          "проекты, импорт и просмотр работают без него.");
        report.hints << QStringLiteral("Установите Docker Desktop (https://www.docker.com/products/docker-desktop/) "
                                       "и нажмите «Проверить снова».");
        finishEnvironment(report);
        return;
    }
    runDocker({QStringLiteral("version"), QStringLiteral("--format"), QStringLiteral("{{.Server.Version}}")}, 20000,
              [this, report](int code, const QByteArray &out) mutable {
        const QString server = QString::fromUtf8(out).trimmed();
        if (code != 0 || server.isEmpty()) {
            report.engineMissing = true;
            report.summary = QStringLiteral("Docker не запущен");
            report.problems << QStringLiteral("Docker установлен, но его служба не отвечает.");
            report.hints << QStringLiteral("Запустите Docker Desktop, дождитесь готовности и нажмите «Проверить снова».");
            finishEnvironment(report);
            return;
        }
        runDocker({QStringLiteral("image"), QStringLiteral("inspect"), QStringLiteral("--format"), QStringLiteral("{{.Id}}"),
                   imageReference()}, 20000, [this, report, server](int imageCode, const QByteArray &) mutable {
            if (imageCode != 0) {
                report.imageMissing = true;
                report.problems << QStringLiteral("Образ OpenDroneMap %1 не загружен.").arg(QLatin1String(Version));
                report.hints << QStringLiteral("Нажмите «Загрузить образ ODM»: около 630 МБ загрузки, около 2,4 ГБ на диске.");
            }
            runDocker({QStringLiteral("info"), QStringLiteral("--format"), QStringLiteral("{{.NCPU}} {{.MemTotal}}")}, 20000,
                      [this, report, server](int infoCode, const QByteArray &info) mutable {
                const QStringList parts = QString::fromUtf8(info).simplified().split(QLatin1Char(' '));
                if (infoCode == 0 && parts.size() >= 2) {
                    report.cpus = parts[0].toInt();
                    report.memoryBytes = parts[1].toLongLong();
                }
                if (report.memoryBytes > 0 && report.memoryBytes < 8 * GiB)
                    report.warnings << QStringLiteral("Docker выделено %1 ГБ памяти. ODM требует около 1 ГБ на поток; "
                                                      "большие наборы снимков могут не поместиться.")
                                           .arg(gigabytes(report.memoryBytes));
                report.available = report.problems.isEmpty();
                report.summary = report.available
                    ? QStringLiteral("Docker %1 · OpenDroneMap %2 · %3 CPU, %4 ГБ памяти")
                          .arg(server, QLatin1String(Version)).arg(report.cpus)
                          .arg(gigabytes(report.memoryBytes))
                    : report.problems.constFirst();
                finishEnvironment(report);
            });
        });
    });
}

void OdmEngine::finishEnvironment(EnvironmentReport report)
{
    emit environmentChecked(report);
}

void OdmEngine::installRuntime()
{
    if (m_docker.isEmpty()) {
        emit installFinished(false, QStringLiteral("Docker не найден"));
        return;
    }
    auto *process = new QProcess(this);
    process->setProcessChannelMode(QProcess::MergedChannels);
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process] {
        const QStringList lines = QString::fromUtf8(process->readAllStandardOutput()).split(QRegularExpression(QStringLiteral("[\r\n]+")),
                                                                                           Qt::SkipEmptyParts);
        for (const QString &line : lines) emit installProgress(stripAnsi(line).trimmed());
    });
    connect(process, &QProcess::finished, this, [this, process](int code, QProcess::ExitStatus status) {
        process->deleteLater();
        const bool ok = status == QProcess::NormalExit && code == 0;
        emit installFinished(ok, ok ? QStringLiteral("Образ OpenDroneMap %1 загружен.").arg(QLatin1String(Version))
                                    : QStringLiteral("Не удалось загрузить образ (код %1). Проверьте подключение к интернету и Docker.").arg(code));
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        process->deleteLater();
        emit installFinished(false, QStringLiteral("Не удалось запустить docker"));
    });
    process->start(m_docker, {QStringLiteral("pull"), QLatin1String(ImageTag) + QLatin1Char('@') + QLatin1String(ImageDigest)});
}

bool OdmEngine::createJob(ProcessingJob &job, const ProcessingProfile &profile, const QStringList &extraArguments,
                          const EnvironmentReport &env, QString *error)
{
    if (!validateExtraArguments(extraArguments, error)) return false;
    job.engine = QStringLiteral("odm");
    job.engineVersion = QLatin1String(Version);
    job.arguments = profile.arguments + extraArguments;
    if (!extraArguments.contains(QStringLiteral("--max-concurrency")))
        job.arguments << QStringLiteral("--max-concurrency") << QString::number(concurrencyFor(env.cpus, env.memoryBytes));
    job.stageCount = int(stageIds().size());
    return true;
}

bool OdmEngine::start(const ProcessingJob &job, const QString &jobDirectory, QString *error)
{
    if (m_run) {
        if (error) *error = QStringLiteral("Движок уже выполняет задание %1").arg(m_runningJob);
        return false;
    }
    if (m_docker.isEmpty()) m_docker = findDocker();
    if (m_docker.isEmpty()) {
        if (error) *error = QStringLiteral("Docker не найден");
        return false;
    }
    m_runningJob = job.id;
    m_cancelRequested = false;
    m_partialLine.clear();
    m_stage.clear();
    m_stageIndex = 0;
    m_recent.clear();
    m_errors.clear();
    m_run = new QProcess(this);
    m_run->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_run, &QProcess::readyReadStandardOutput, this, &OdmEngine::handleOutput);
    connect(m_run, &QProcess::finished, this, &OdmEngine::handleRunFinished);
    connect(m_run, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart || !m_run) return;
        const QString id = m_runningJob;
        const QString message = QStringLiteral("Не удалось запустить docker: %1").arg(m_run->errorString());
        m_run->deleteLater();
        m_run = nullptr;
        m_runningJob.clear();
        emit finished(id, JobOutcome::Failed, -1, message);
    });
#ifdef Q_OS_WIN
    const bool windowsHost = true;
#else
    const bool windowsHost = false;
#endif
    const QStringList arguments = runArguments(job, jobDirectory, windowsHost);
    emit logLine(job.id, QStringLiteral("$ docker ") + arguments.join(QLatin1Char(' ')));
    m_run->start(m_docker, arguments);
    return true;
}

void OdmEngine::handleOutput()
{
    if (!m_run) return;
    QByteArray data = m_partialLine + m_run->readAllStandardOutput();
    // ODM and PDAL also use '\r' for progress; treat it as a line break.
    data.replace('\r', '\n');
    const int last = data.lastIndexOf('\n');
    m_partialLine = last >= 0 ? data.mid(last + 1) : data;
    if (last < 0) return;
    const QList<QByteArray> lines = data.left(last).split('\n');
    for (const QByteArray &raw : lines) {
        const QString line = stripAnsi(QString::fromUtf8(raw)).trimmed();
        if (line.isEmpty()) continue;
        m_recent.append(line);
        if (m_recent.size() > RecentLines) m_recent.removeFirst();
        if (line.contains(QLatin1String("ERROR")) || line.startsWith(QLatin1String("Traceback"))
            || line.contains(QLatin1String("Exception"))) {
            m_errors.append(line);
            if (m_errors.size() > 20) m_errors.removeFirst();
        }
        const QString stage = parseStage(line);
        if (!stage.isEmpty() && stage != m_stage) {
            m_stage = stage;
            m_stageIndex = int(stageIds().indexOf(stage)) + 1;
            emit stageChanged(m_runningJob, m_stage, m_stageIndex, int(stageIds().size()));
        }
        emit logLine(m_runningJob, line);
    }
}

void OdmEngine::handleRunFinished(int exitCode, QProcess::ExitStatus status)
{
    if (!m_run) return;
    if (!m_partialLine.isEmpty()) {
        m_partialLine.append('\n');
        handleOutput();
    }
    const QString id = m_runningJob;
    JobOutcome outcome = JobOutcome::Failed;
    QString message;
    if (m_cancelRequested) {
        outcome = JobOutcome::Cancelled;
        message = QStringLiteral("Обработка отменена пользователем.");
    } else if (status == QProcess::NormalExit && exitCode == 0) {
        outcome = JobOutcome::Succeeded;
        message = QStringLiteral("OpenDroneMap завершил обработку.");
    } else if (exitCode == 137) {
        message = QStringLiteral("Процесс ODM остановлен системой (код 137). Частая причина — нехватка памяти, "
                                 "выделенной Docker; попробуйте профиль «Быстрый просмотр» или увеличьте память Docker.");
    } else {
        const QString detail = !m_errors.isEmpty() ? m_errors.constLast()
                             : !m_recent.isEmpty() ? m_recent.constLast() : QString();
        message = QStringLiteral("ODM завершился с ошибкой (код %1). %2").arg(exitCode).arg(detail);
    }
    m_run->deleteLater();
    m_run = nullptr;
    m_runningJob.clear();
    emit finished(id, outcome, exitCode, message.trimmed());
}

void OdmEngine::cancel(const QString &jobId)
{
    if (!m_run || jobId != m_runningJob || m_cancelRequested) return;
    m_cancelRequested = true;
    runDocker({QStringLiteral("stop"), QStringLiteral("-t"), QStringLiteral("10"), containerName(jobId)}, 60000,
              [this, jobId](int, const QByteArray &) {
        // The client exits once the container stops; if it does not (container never started,
        // daemon unresponsive), end the client and make sure no container is left behind.
        QTimer::singleShot(10000, this, [this, jobId] {
            if (m_run && m_runningJob == jobId) {
                forceRemoveContainer(jobId, false);
                m_run->kill();
            }
        });
    });
}

void OdmEngine::abort(const QString &jobId)
{
    if (!m_run || jobId != m_runningJob) return;
    m_run->disconnect(this);
    forceRemoveContainer(jobId, true);
    m_run->kill();
    m_run->waitForFinished(5000);
    delete m_run;
    m_runningJob.clear();
}

bool OdmEngine::isRunning(const QString &jobId) const
{
    return m_run && m_runningJob == jobId;
}

JobProgress OdmEngine::readProgress(const QString &jobId) const
{
    JobProgress p;
    if (jobId != m_runningJob) return p;
    p.stage = m_stage;
    p.stageIndex = m_stageIndex;
    p.stageCount = int(stageIds().size());
    p.recentLog = m_recent.mid(qMax(0, int(m_recent.size()) - 50));
    return p;
}

QVector<Artifact> OdmEngine::collectOutputs(const QString &jobDirectory) const
{
    struct Known { const char *kind; const char *file; };
    static const Known known[] = {
        {"orthophoto", "odm_orthophoto/odm_orthophoto.tif"},
        {"dsm", "odm_dem/dsm.tif"},
        {"dtm", "odm_dem/dtm.tif"},
        {"pointcloud", "odm_georeferencing/odm_georeferenced_model.laz"},
        {"mesh", "odm_texturing/odm_textured_model_geo.obj"},
        {"mesh", "odm_texturing_25d/odm_textured_model_geo.obj"},
        {"report", "odm_report/report.pdf"},
    };
    QVector<Artifact> outputs;
    const QDir dir(jobDirectory);
    for (const Known &k : known) {
        const QString file = QLatin1String(k.file);
        const QFileInfo info(dir.filePath(file));
        if (!info.isFile() || info.size() == 0) continue;
        Artifact a;
        a.kind = QLatin1String(k.kind);
        a.file = file;
        a.bytes = info.size();
        if (file.endsWith(QLatin1String(".tif"))) {
            a.details = GeoTiffInfo::read(info.filePath()).describe();
        } else if (file.endsWith(QLatin1String(".laz"))) {
            a.details = describeLas(info.filePath());
        } else if (file.endsWith(QLatin1String(".pdf"))) {
            QFile f(info.filePath());
            a.details = f.open(QIODevice::ReadOnly) && f.read(4) == "%PDF" ? QStringLiteral("PDF-отчёт ODM")
                                                                            : QStringLiteral("повреждён: не PDF");
        } else if (file.contains(QLatin1String("_25d"))) {
            a.details = QStringLiteral("OBJ, 2.5D-сетка с текстурами");
        } else {
            a.details = QStringLiteral("OBJ, 3D-сетка с текстурами");
        }
        outputs.append(a);
    }
    return outputs;
}

void OdmEngine::cleanupInterrupted(const ProcessingJob &job)
{
    if (!m_docker.isEmpty()) forceRemoveContainer(job.id, false);
}

void OdmEngine::forceRemoveContainer(const QString &jobId, bool wait)
{
    const QStringList args{QStringLiteral("rm"), QStringLiteral("-f"), containerName(jobId)};
    if (!wait) {
        runDocker(args, 60000, [](int, const QByteArray &) {});
        return;
    }
    QProcess process;
    process.start(m_docker, args);
    process.waitForFinished(30000);
}
