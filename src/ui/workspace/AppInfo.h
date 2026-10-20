#pragma once

#include <QObject>
#include <QString>
#include <QUrl>

// What the Help menu shows and opens: versions, the documentation folder (shipped next to the
// program, or the source tree in a developer build) and the folder of the operations journal.
class AppInfo : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(QString qtVersion READ qtVersion CONSTANT)
    Q_PROPERTY(QString docsDirectory READ docsDirectory CONSTANT)
    Q_PROPERTY(QString logDirectory READ logDirectory CONSTANT)

public:
    explicit AppInfo(const QString &logDirectory, QObject *parent = nullptr);

    QString version() const;
    QString qtVersion() const;
    QString docsDirectory() const;     // "" when no documentation is found
    QString logDirectory() const { return m_logDirectory; }

    Q_INVOKABLE QString docPath(const QString &name) const;    // e.g. "DEPENDENCIES.md", "" if missing
    Q_INVOKABLE bool openPath(const QString &path) const;      // file or folder in the system's program
    Q_INVOKABLE bool openLogDirectory() const;                 // creates it first
    Q_INVOKABLE bool fileExists(const QString &path) const;    // a file (not a folder) at the path
    Q_INVOKABLE QString siblingLas(const QString &path) const; // the .las next to a .laz (or the path), "" if none
    // Where a file dialog of a kind ("photos", "projects", "import", "export", …) opens — never the
    // program's folder, the working directory of a program started from the Start menu: the folder
    // last chosen in a dialog of that kind; else the project's folder when one is open; for photos
    // the Pictures folder, for projects and with no project the Documents folder.
    Q_INVOKABLE QUrl dialogFolder(const QString &projectPath, const QString &kind = {}) const;
    // Remembers the folder of what was chosen (a file: its folder) for dialogs of that kind.
    Q_INVOKABLE void rememberFolder(const QString &kind, const QUrl &chosen) const;

private:
    QString m_logDirectory;
};
