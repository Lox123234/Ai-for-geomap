#pragma once

#include <QString>
#include <QStringList>
#include <functional>

// The programs that come with the bundled OpenDroneMap — PDAL (LiDAR, LAZ), OpenSfM (bundle
// adjustment), Obj2Tiles and entwine (3D Tiles) — for GeoMap AI's own steps. Each runs as a
// separate process with ODM's environment, as ODM runs them; nothing is linked into GeoMap AI.
// Calls block: use them from a worker thread.
namespace odmtools {

// The ODM build in use (an ASCII path: a link to it when its folder has other characters), its
// tools folder, a tool by name ("pdal" → <bin>/pdal.exe); empty when missing.
QString root(QString *error = nullptr);
QString binDirectory();
QString program(const QString &name);

struct Result
{
    int exitCode = -1;
    QString output;      // stdout and stderr, merged
    QString error;       // why it did not run or failed, in plain words
    bool ok() const { return error.isEmpty() && exitCode == 0; }
};

// Runs a program of ODM. onLine gets each line of output; cancelled() is polled (true: the
// process tree is ended). timeoutMs 0: no limit.
Result run(const QString &program, const QStringList &arguments, int timeoutMs = 0,
           const std::function<void(const QString &)> &onLine = {}, const std::function<bool()> &cancelled = {});
Result pdal(const QStringList &arguments, int timeoutMs = 0, const std::function<void(const QString &)> &onLine = {},
            const std::function<bool()> &cancelled = {});
// opensfm <command> <dataset> [extra] through ODM's Python.
Result opensfm(const QString &command, const QString &dataset, const QStringList &extra = {},
               const std::function<void(const QString &)> &onLine = {}, const std::function<bool()> &cancelled = {});

// A path the tools can open: itself when ASCII-only, else its 8.3 alias (existing paths only).
QString toolPath(const QString &path);
// A new empty folder with an ASCII path for the output of a tool (moved to its place after);
// the caller removes it. Empty with an error if none can be made.
QString workFolder(QString *error = nullptr);

} // namespace odmtools
