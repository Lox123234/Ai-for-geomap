#pragma once

#include <QObject>
#include <QString>

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

private:
    QString m_logDirectory;
};
