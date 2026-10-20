#include "OdmTools.h"
#include "OdmEngine.h"
#include "Platform.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMutex>
#include <QProcess>
#include <QRandomGenerator>

namespace odmtools {

namespace {
QMutex g_lock;
QString g_root, g_rootError;
bool g_rootFound = false;
} // namespace

QString root(QString *error)
{
    QMutexLocker lock(&g_lock);
    if (!g_rootFound) {
        g_rootError = QStringLiteral("Встроенный OpenDroneMap не найден");
        for (const QString &c : OdmEngine::nativeCandidates()) {
            if (!OdmEngine::nativeProblem(c).isEmpty()) continue;
            QString e;
            const QString r = OdmEngine::effectiveNativeRoot(c, &e);
            if (r.isEmpty() || !OdmEngine::patchNativeVirtualEnv(r, &e)) {
                g_rootError = e;
                continue;
            }
            g_root = r;
            g_rootError.clear();
            break;
        }
        g_rootFound = !g_root.isEmpty();
    }
    if (error) *error = g_rootError;
    return g_root;
}

QString binDirectory()
{
    const QString r = root();
    return r.isEmpty() ? QString() : QDir(r).filePath(QStringLiteral("SuperBuild/install/bin"));
}

QString program(const QString &name)
{
    const QString bin = binDirectory();
    if (bin.isEmpty()) return {};
#ifdef Q_OS_WIN
    const QString path = QDir(bin).filePath(name + QStringLiteral(".exe"));
#else
    const QString path = QDir(bin).filePath(name);
#endif
    return QFileInfo::exists(path) ? path : QString();
}

Result run(const QString &programPath, const QStringList &arguments, int timeoutMs,
           const std::function<void(const QString &)> &onLine, const std::function<bool()> &cancelled)
{
    Result r;
    QString error;
    const QString odm = root(&error);
    if (odm.isEmpty()) {
        r.error = error;
        return r;
    }
    if (programPath.isEmpty() || !QFileInfo::exists(programPath)) {
        r.error = QStringLiteral("В составе OpenDroneMap нет программы %1").arg(QFileInfo(programPath).fileName());
        return r;
    }
    QProcess p;
    p.setProcessEnvironment(OdmEngine::nativeEnvironment(odm, QDir(Platform::dataDirectory()).filePath(QStringLiteral("cache/odm-pycache")),
                                                         QProcessEnvironment::systemEnvironment()));
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.setWorkingDirectory(odm);
    ProcessTree::prepare(p);
    p.start(programPath, arguments);
    if (!p.waitForStarted(30000)) {
        r.error = QStringLiteral("%1 не запускается: %2").arg(QFileInfo(programPath).fileName(), p.errorString());
        return r;
    }
    ProcessTree tree;
    tree.attach(p);
    QElapsedTimer clock;
    clock.start();
    QByteArray pending;
    auto drain = [&](bool all) {
        pending += p.readAll();
        int nl;
        while ((nl = int(pending.indexOf('\n'))) >= 0 || (all && !pending.isEmpty())) {
            const QByteArray line = nl >= 0 ? pending.left(nl) : pending;
            pending = nl >= 0 ? pending.mid(nl + 1) : QByteArray();
            const QString text = OdmEngine::stripAnsi(QString::fromUtf8(line).trimmed());
            r.output += text + '\n';
            if (onLine && !text.isEmpty()) onLine(text);
        }
    };
    bool stopped = false, timedOut = false;
    while (!p.waitForFinished(200)) {
        drain(false);
        if (p.state() == QProcess::NotRunning) break;
        if (cancelled && cancelled()) {
            stopped = true;
            tree.terminate();
            p.kill();
            p.waitForFinished(5000);
            break;
        }
        if (timeoutMs > 0 && clock.elapsed() > timeoutMs) {
            timedOut = true;
            tree.terminate();
            p.kill();
            p.waitForFinished(5000);
            break;
        }
    }
    drain(true);
    r.exitCode = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
    if (stopped) r.error = QStringLiteral("Отменено");
    else if (timedOut) r.error = QStringLiteral("%1 не уложился во время").arg(QFileInfo(programPath).completeBaseName());
    else if (r.exitCode != 0) {
        // The last meaningful lines say why.
        QStringList lines = r.output.split('\n', Qt::SkipEmptyParts);
        while (lines.size() > 3) lines.removeFirst();
        r.error = QStringLiteral("%1: ошибка (код %2)%3").arg(QFileInfo(programPath).completeBaseName()).arg(r.exitCode)
                      .arg(lines.isEmpty() ? QString() : QStringLiteral(": ") + lines.join(QStringLiteral(" · ")));
    }
    return r;
}

Result pdal(const QStringList &arguments, int timeoutMs, const std::function<void(const QString &)> &onLine,
            const std::function<bool()> &cancelled)
{
    return run(program(QStringLiteral("pdal")), arguments, timeoutMs, onLine, cancelled);
}

Result opensfm(const QString &command, const QString &dataset, const QStringList &extra,
               const std::function<void(const QString &)> &onLine, const std::function<bool()> &cancelled)
{
    const QString odm = root();
    const QString main = QDir(binDirectory()).filePath(QStringLiteral("opensfm/bin/opensfm_main.py"));
    if (odm.isEmpty() || !QFileInfo::exists(main)) {
        Result r;
        r.error = QStringLiteral("В составе OpenDroneMap нет OpenSfM");
        return r;
    }
    return run(OdmEngine::nativePython(odm),
               QStringList{QStringLiteral("-X"), QStringLiteral("utf8"), QDir::toNativeSeparators(main), command, toolPath(dataset)} + extra,
               0, onLine, cancelled);
}

QString toolPath(const QString &path)
{
    const QString native = QDir::toNativeSeparators(QDir::cleanPath(path));
    if (Platform::isToolSafePath(native) || !QFileInfo::exists(path)) return native;
    return QDir::toNativeSeparators(Platform::shortPath(path));
}

QString workFolder(QString *error)
{
    const QString base = Platform::toolSafeRoot();
    if (base.isEmpty()) {
        if (error) *error = QStringLiteral("Нет папки без пробелов и нелатинских символов для временных файлов");
        return {};
    }
    const QString dir = QDir(base).filePath(QStringLiteral("work/tmp-%1-%2")
                                                 .arg(QDateTime::currentMSecsSinceEpoch())
                                                 .arg(QRandomGenerator::global()->generate() % 100000));
    if (!QDir().mkpath(dir)) {
        if (error) *error = QStringLiteral("Не удалось создать %1").arg(QDir::toNativeSeparators(dir));
        return {};
    }
    return dir;
}

} // namespace odmtools
