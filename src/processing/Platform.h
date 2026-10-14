#pragma once

#include <QString>

class QProcess;

// OS services the processing engines need: data folder, paths that third-party tools can
// handle, directory links, process trees and machine resources.
namespace Platform {

// Folder for engines, downloads and work links: GEOMAP_DATA_DIR, setDataDirectory(), or
// %LOCALAPPDATA%\GeoMapAI (~/.local/share/GeoMapAI).
QString dataDirectory();
void setDataDirectory(const QString &directory);

// True for paths made only of ASCII letters, digits and . _ - : \ / (no spaces). Several ODM
// components open files through narrow C runtime calls and fail on anything else.
bool isToolSafePath(const QString &path);
// The 8.3 alias of an existing path on Windows (may be identical); the path itself elsewhere.
QString shortPath(const QString &path);
// A writable tool-safe folder for work links and engines: the data folder, its 8.3 alias, or
// <system drive>\GeoMapAI. Created if needed; empty if none can be created.
QString toolSafeRoot();

// Directory link: an NTFS junction on Windows (no privileges needed), a symlink elsewhere.
// An existing link to the same target is kept. Never touches the target's contents.
bool createDirectoryLink(const QString &link, const QString &target, QString *error = nullptr);
QString directoryLinkTarget(const QString &link);   // empty if `link` is not a directory link
// Removes only the link itself; refuses anything that is not a directory link.
bool removeDirectoryLink(const QString &link);

int cpuCount();
qint64 totalMemoryBytes();

} // namespace Platform

// Every process started by one QProcess, including grandchildren: a Windows job object with
// kill-on-close (children die even if GeoMap AI crashes), a process group elsewhere.
class ProcessTree
{
public:
    ProcessTree();
    ~ProcessTree();
    ProcessTree(const ProcessTree &) = delete;
    ProcessTree &operator=(const ProcessTree &) = delete;

    // Call before QProcess::start() (sets up the process group on Unix).
    static void prepare(QProcess &process);
    // Call right after start(); children started later belong to the tree.
    bool attach(const QProcess &process);
    // Ends every process of the tree immediately.
    void terminate();
    // Processes currently in the tree (Windows); -1 where the OS does not report it.
    int activeProcesses() const;

private:
    void *m_job = nullptr;   // HANDLE on Windows
    qint64 m_pid = 0;
};
