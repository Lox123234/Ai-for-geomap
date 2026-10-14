#include "Platform.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QThread>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>
#else
#include <csignal>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {
QString &dataDirectoryOverride()
{
    static QString directory;
    return directory;
}

#ifdef Q_OS_WIN
// REPARSE_DATA_BUFFER (ntifs.h) for mount points and symbolic links.
struct ReparseBuffer
{
    DWORD tag;
    WORD dataLength;
    WORD reserved;
    WORD substituteOffset;
    WORD substituteLength;
    WORD printOffset;
    WORD printLength;
    // Symbolic links have a ULONG Flags field here; mount points start the names directly.
    WCHAR names[1];
};
constexpr DWORD HeaderSize = 8;   // tag, dataLength, reserved

std::wstring wide(const QString &path)
{
    return QDir::toNativeSeparators(path).toStdWString();
}

HANDLE openReparsePoint(const QString &path, DWORD access)
{
    return CreateFileW(wide(path).c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                       OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
}
#endif

QString normalized(const QString &path)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath()));
}
} // namespace

QString Platform::dataDirectory()
{
    if (!dataDirectoryOverride().isEmpty()) return dataDirectoryOverride();
    const QString env = qEnvironmentVariable("GEOMAP_DATA_DIR");
    if (!env.isEmpty()) return QDir(env).absolutePath();
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)).filePath(QStringLiteral("GeoMapAI"));
}

void Platform::setDataDirectory(const QString &directory)
{
    dataDirectoryOverride() = directory.isEmpty() ? QString() : QDir(directory).absolutePath();
}

bool Platform::isToolSafePath(const QString &path)
{
    if (path.isEmpty()) return false;
    for (const QChar c : path) {
        const char16_t u = c.unicode();
        const bool ok = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9')
            || u == '.' || u == '_' || u == '-' || u == ':' || u == '\\' || u == '/' || u == '~';
        if (!ok) return false;
    }
    return true;
}

QString Platform::shortPath(const QString &path)
{
#ifdef Q_OS_WIN
    const std::wstring in = wide(path);
    const DWORD size = GetShortPathNameW(in.c_str(), nullptr, 0);
    if (size == 0) return path;
    std::wstring out(size, L'\0');
    const DWORD written = GetShortPathNameW(in.c_str(), out.data(), size);
    if (written == 0 || written >= size) return path;
    out.resize(written);
    return QDir::fromNativeSeparators(QString::fromStdWString(out));
#else
    return path;
#endif
}

QString Platform::toolSafeRoot()
{
    const QString data = dataDirectory();
    if (!QDir().mkpath(data)) return {};
    if (isToolSafePath(data)) return data;
#ifdef Q_OS_WIN
    const QString alias = shortPath(data);
    if (isToolSafePath(alias)) return alias;
    const QString drive = qEnvironmentVariable("SystemDrive", QStringLiteral("C:")) + QStringLiteral("/GeoMapAI");
    if (isToolSafePath(drive) && QDir().mkpath(drive)) return drive;
    return {};
#else
    return data;
#endif
}

QString Platform::directoryLinkTarget(const QString &link)
{
#ifdef Q_OS_WIN
    const DWORD attributes = GetFileAttributesW(wide(link).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)
        || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
        return {};
    HANDLE h = openReparsePoint(link, 0);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::vector<char> buffer(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, nullptr, 0, buffer.data(), DWORD(buffer.size()), &returned, nullptr);
    CloseHandle(h);
    if (!ok || returned < HeaderSize + 8) return {};
    const auto *b = reinterpret_cast<const ReparseBuffer *>(buffer.data());
    const char *names = nullptr;
    if (b->tag == IO_REPARSE_TAG_MOUNT_POINT) names = reinterpret_cast<const char *>(b->names);
    else if (b->tag == IO_REPARSE_TAG_SYMLINK) names = reinterpret_cast<const char *>(b->names) + sizeof(ULONG);
    else return {};
    if (names + b->substituteOffset + b->substituteLength > buffer.data() + returned) return {};
    QString target = QString::fromWCharArray(reinterpret_cast<const wchar_t *>(names + b->substituteOffset),
                                             b->substituteLength / sizeof(WCHAR));
    if (target.startsWith(QLatin1String("\\??\\"))) target.remove(0, 4);
    return QDir::fromNativeSeparators(target);
#else
    const QFileInfo info(link);
    return info.isSymLink() ? info.symLinkTarget() : QString();
#endif
}

bool Platform::createDirectoryLink(const QString &link, const QString &target, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error) *error = message;
        return false;
    };
    const QFileInfo targetInfo(target);
    if (!targetInfo.isDir()) return fail(QStringLiteral("Папка %1 не найдена").arg(QDir::toNativeSeparators(target)));
    const QString wanted = normalized(targetInfo.absoluteFilePath());
    const QString existing = directoryLinkTarget(link);
    if (!existing.isEmpty()) {
        if (normalized(existing).compare(wanted, Qt::CaseInsensitive) == 0) return true;
        if (!removeDirectoryLink(link)) return fail(QStringLiteral("Не удалось заменить ссылку %1").arg(QDir::toNativeSeparators(link)));
    } else if (QFileInfo::exists(link) || QFileInfo(link).isSymLink()) {
        return fail(QStringLiteral("%1 уже существует и не является ссылкой").arg(QDir::toNativeSeparators(link)));
    }
    QDir().mkpath(QFileInfo(link).absolutePath());
#ifdef Q_OS_WIN
    if (!CreateDirectoryW(wide(link).c_str(), nullptr))
        return fail(QStringLiteral("Не удалось создать %1 (код %2)").arg(QDir::toNativeSeparators(link)).arg(GetLastError()));
    const std::wstring print = wide(wanted);
    const std::wstring substitute = L"\\??\\" + print;
    const DWORD substituteBytes = DWORD(substitute.size() * sizeof(WCHAR));
    const DWORD printBytes = DWORD(print.size() * sizeof(WCHAR));
    const DWORD dataLength = 8 + substituteBytes + sizeof(WCHAR) + printBytes + sizeof(WCHAR);
    std::vector<char> buffer(HeaderSize + dataLength, 0);
    auto *b = reinterpret_cast<ReparseBuffer *>(buffer.data());
    b->tag = IO_REPARSE_TAG_MOUNT_POINT;
    b->dataLength = WORD(dataLength);
    b->substituteOffset = 0;
    b->substituteLength = WORD(substituteBytes);
    b->printOffset = WORD(substituteBytes + sizeof(WCHAR));
    b->printLength = WORD(printBytes);
    char *names = reinterpret_cast<char *>(b->names);
    std::memcpy(names, substitute.c_str(), substituteBytes);
    std::memcpy(names + b->printOffset, print.c_str(), printBytes);
    HANDLE h = openReparsePoint(link, GENERIC_WRITE);
    BOOL ok = h != INVALID_HANDLE_VALUE;
    DWORD returned = 0;
    if (ok) ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buffer.data(), DWORD(buffer.size()), nullptr, 0, &returned, nullptr);
    const DWORD code = GetLastError();
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (!ok) {
        RemoveDirectoryW(wide(link).c_str());
        return fail(QStringLiteral("Не удалось создать ссылку на папку %1 (код %2)").arg(QDir::toNativeSeparators(target)).arg(code));
    }
    return true;
#else
    if (!QFile::link(wanted, link)) return fail(QStringLiteral("Не удалось создать ссылку %1").arg(link));
    return true;
#endif
}

bool Platform::removeDirectoryLink(const QString &link)
{
    if (directoryLinkTarget(link).isEmpty()) return false;
#ifdef Q_OS_WIN
    // RemoveDirectory on a reparse point deletes the link, never the target's contents.
    return RemoveDirectoryW(wide(link).c_str()) != 0;
#else
    return QFile::remove(link);
#endif
}

int Platform::cpuCount()
{
    return qMax(1, QThread::idealThreadCount());
}

qint64 Platform::totalMemoryBytes()
{
#ifdef Q_OS_WIN
    MEMORYSTATUSEX status;
    status.dwLength = sizeof status;
    return GlobalMemoryStatusEx(&status) ? qint64(status.ullTotalPhys) : 0;
#elif defined(_SC_PHYS_PAGES)
    const long pages = sysconf(_SC_PHYS_PAGES), size = sysconf(_SC_PAGE_SIZE);
    return pages > 0 && size > 0 ? qint64(pages) * size : 0;
#else
    return 0;
#endif
}

// ----------------------------------------------------------------- ProcessTree

ProcessTree::ProcessTree()
{
#ifdef Q_OS_WIN
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof info);
    }
    m_job = job;
#endif
}

ProcessTree::~ProcessTree()
{
#ifdef Q_OS_WIN
    // Kill-on-close: whatever is still running in the tree ends with the handle.
    if (m_job) CloseHandle(static_cast<HANDLE>(m_job));
#endif
}

void ProcessTree::prepare(QProcess &process)
{
#ifdef Q_OS_UNIX
    process.setChildProcessModifier([] { ::setpgid(0, 0); });
#else
    Q_UNUSED(process)
#endif
}

bool ProcessTree::attach(const QProcess &process)
{
    m_pid = process.processId();
    if (m_pid <= 0) return false;
#ifdef Q_OS_WIN
    if (!m_job) return false;
    HANDLE h = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, DWORD(m_pid));
    if (!h) return false;
    const BOOL ok = AssignProcessToJobObject(static_cast<HANDLE>(m_job), h);
    CloseHandle(h);
    return ok != 0;
#else
    return true;
#endif
}

int ProcessTree::activeProcesses() const
{
#ifdef Q_OS_WIN
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
    if (m_job && QueryInformationJobObject(static_cast<HANDLE>(m_job), JobObjectBasicAccountingInformation, &info,
                                           sizeof info, nullptr))
        return int(info.ActiveProcesses);
#endif
    return -1;
}

void ProcessTree::terminate()
{
#ifdef Q_OS_WIN
    if (m_job) TerminateJobObject(static_cast<HANDLE>(m_job), 1);
#else
    if (m_pid > 0) ::kill(-pid_t(m_pid), SIGKILL);
#endif
}
