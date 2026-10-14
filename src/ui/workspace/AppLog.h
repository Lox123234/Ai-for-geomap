#pragma once

#include <QAbstractListModel>
#include <QDateTime>
#include <QVector>

// Operations journal of the bottom panel: what the services actually reported (project events,
// import results, job state changes, exports, errors). Newest last, at most MaxEntries in memory;
// every entry is also appended to <logDirectory>/geomap-YYYYMMDD.log when a directory is set.
class AppLog : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(QString logDirectory READ logDirectory CONSTANT)
    Q_PROPERTY(QString lastText READ lastText NOTIFY countChanged)

public:
    enum Level { Info, Success, Warning, Error };
    Q_ENUM(Level)
    enum Roles { TimeRole = Qt::UserRole + 1, LevelRole, SourceRole, TextRole };
    static constexpr int MaxEntries = 2000;

    struct Entry
    {
        QDateTime time;
        Level level = Info;
        QString source;   // "Проект", "Импорт", "Обработка", "Карта", ...
        QString text;
    };

    explicit AppLog(const QString &logDirectory = {}, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return int(m_entries.size()); }
    QString logDirectory() const { return m_directory; }
    QString lastText() const { return m_entries.isEmpty() ? QString() : m_entries.constLast().text; }
    const QVector<Entry> &entries() const { return m_entries; }

    Q_INVOKABLE void add(int level, const QString &source, const QString &text);
    Q_INVOKABLE void clear();
    Q_INVOKABLE QString logFilePath() const;

signals:
    void countChanged();

private:
    QVector<Entry> m_entries;
    QString m_directory;
};
