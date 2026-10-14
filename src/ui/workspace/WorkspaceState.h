#pragma once

#include <QObject>
#include <QVariant>

// Window and panel layout of the main window (sizes, visibility, active view), kept per user in
// QSettings under "workspace/". Values are whatever QML stores: numbers, booleans, SplitView
// states (byte arrays).
class WorkspaceState : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;

    Q_INVOKABLE QVariant value(const QString &key, const QVariant &fallback = {}) const;
    Q_INVOKABLE void setValue(const QString &key, const QVariant &value);
    Q_INVOKABLE bool contains(const QString &key) const;
    Q_INVOKABLE void clear();   // "Сбросить расположение панелей"

signals:
    void cleared();
};
