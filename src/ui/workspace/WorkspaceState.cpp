#include "WorkspaceState.h"

#include <QSettings>

namespace {
QString keyOf(const QString &key)
{
    return QStringLiteral("workspace/") + key;
}
} // namespace

QVariant WorkspaceState::value(const QString &key, const QVariant &fallback) const
{
    return QSettings().value(keyOf(key), fallback);
}

void WorkspaceState::setValue(const QString &key, const QVariant &value)
{
    QSettings().setValue(keyOf(key), value);
}

bool WorkspaceState::contains(const QString &key) const
{
    return QSettings().contains(keyOf(key));
}

void WorkspaceState::clear()
{
    QSettings().remove(QStringLiteral("workspace"));
    emit cleared();
}
