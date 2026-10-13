#pragma once

#include "PhotoModel.h"
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QVector>
#include <atomic>

class QThread;

// Imports images into a project on a worker thread: scans folders, skips duplicates,
// reads metadata, copies files into photos/ and prepares thumbnails.
// The owner (GUI thread) pulls results and progress after the corresponding signals.
class ImportJob final : public QObject
{
    Q_OBJECT
public:
    struct Request
    {
        QStringList files;           // explicit files...
        QString folder;              // ...or a canonical folder scanned recursively
        QString projectDir;          // canonical project folder; never scanned
        QSet<QString> knownSources;  // canonical source paths already in the project
        int maxFiles = 10000;
    };
    struct Progress
    {
        bool scanning = false;
        int total = 0;               // images queued after scanning
        int processed = 0;
        int imported = 0;
        int duplicates = 0;
        int unsupported = 0;
        int unreadable = 0;
        int copyFailed = 0;
        bool truncated = false;      // the folder held more than maxFiles images
        QString currentFile;
        int skipped() const { return duplicates + unsupported + unreadable + copyFailed; }
    };

    explicit ImportJob(Request request, QObject *parent = nullptr);
    ~ImportJob() override;           // cancels and joins the worker

    static bool isSupportedImage(const QString &path);

    void start();
    void cancel() { m_cancel = true; }
    void wait();
    bool isCancelled() const { return m_cancel; }
    Progress progress() const;
    QVector<Photo> takeResults();
    QStringList issues() const;      // "<file> — <reason>" for skipped files
    QString failure() const;         // set when the whole import was refused

signals:
    void progressChanged();          // throttled to ~10 per second
    void resultsAvailable();
    void finished();

private:
    enum class Issue { Duplicate, Unsupported, Unreadable, CopyFailed };
    void run();
    QStringList collectCandidates();
    bool hasFreeSpace(const QStringList &files);
    void importFile(const QString &path, QSet<QString> &known);
    void addIssue(const QString &path, Issue issue);
    void publishProgress(bool force);

    const Request m_request;
    QThread *m_thread = nullptr;
    std::atomic_bool m_cancel{false};
    std::atomic_bool m_resultsPending{false};
    qint64 m_lastProgress = 0;       // worker thread only
    mutable QMutex m_mutex;          // guards the members below
    Progress m_progress;
    QVector<Photo> m_results;
    QStringList m_issues;
    QString m_failure;
};
