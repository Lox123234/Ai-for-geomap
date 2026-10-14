#include "AppLog.h"

#include <QDir>
#include <QFile>
#include <QTextStream>

AppLog::AppLog(const QString &logDirectory, QObject *parent) : QAbstractListModel(parent), m_directory(logDirectory) {}

int AppLog::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_entries.size());
}

QVariant AppLog::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_entries.size()) return {};
    const Entry &e = m_entries[index.row()];
    switch (role) {
    case TimeRole: return e.time.toString(QStringLiteral("HH:mm:ss"));
    case LevelRole: return int(e.level);
    case SourceRole: return e.source;
    case TextRole:
    case Qt::DisplayRole: return e.text;
    default: return {};
    }
}

QHash<int, QByteArray> AppLog::roleNames() const
{
    return {{TimeRole, "time"}, {LevelRole, "level"}, {SourceRole, "source"}, {TextRole, "text"}};
}

QString AppLog::logFilePath() const
{
    if (m_directory.isEmpty()) return {};
    return QDir(m_directory).filePath(QStringLiteral("geomap-%1.log").arg(QDate::currentDate().toString(QStringLiteral("yyyyMMdd"))));
}

void AppLog::add(int level, const QString &source, const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) return;
    Entry e{QDateTime::currentDateTime(), Level(qBound(0, level, int(Error))), source, trimmed};
    // Repeated identical messages (e.g. the same status twice) are kept once.
    if (!m_entries.isEmpty() && m_entries.constLast().text == e.text && m_entries.constLast().source == e.source) return;
    if (m_entries.size() >= MaxEntries) {
        beginRemoveRows({}, 0, 0);
        m_entries.removeFirst();
        endRemoveRows();
    }
    beginInsertRows({}, int(m_entries.size()), int(m_entries.size()));
    m_entries << e;
    endInsertRows();
    emit countChanged();

    if (m_directory.isEmpty() || !QDir().mkpath(m_directory)) return;
    QFile file(logFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) return;
    static const char *levels[] = {"INFO", "OK", "WARN", "ERROR"};
    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    out << e.time.toString(Qt::ISODateWithMs) << ' ' << levels[e.level] << " [" << e.source << "] "
        << QString(e.text).replace(QLatin1Char('\n'), QLatin1String(" / ")) << '\n';
}

void AppLog::clear()
{
    beginResetModel();
    m_entries.clear();
    endResetModel();
    emit countChanged();
}
