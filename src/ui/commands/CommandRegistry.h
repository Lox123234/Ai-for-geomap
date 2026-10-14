#pragma once

#include <QHash>
#include <QObject>
#include <QStringList>
#include <QVariantList>
#include <QVector>

// The single list of user commands (menus, toolbar, shortcuts, context menus), read from
// commands.json. A command is "ready" (works), "planned" (shown disabled with the stage that
// brings it) or "engine" (the processing engine cannot do it on its own). QML binds menu items,
// toolbar buttons and shortcuts to the same id, so they can never disagree.
//
// User shortcuts override the defaults and are kept in QSettings ("shortcuts/<id>").
class CommandRegistry : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList menus READ menus CONSTANT)
    Q_PROPERTY(QVariantList toolbar READ toolbar CONSTANT)
    Q_PROPERTY(int revision READ revision NOTIFY shortcutsChanged)   // bindings re-read shortcuts on change

public:
    struct Command
    {
        QString id;
        QString title;
        QString icon;              // file name in qml/icons without ".svg"
        QString defaultShortcut;   // QKeySequence portable text
        QString status;            // ready | planned | engine
        QString stage;             // planned: e.g. "D (0.4)"
        QString reason;            // planned/engine: why it is not available
        QString action;            // ready: what it does
        QString enabledWhen;       // ready: when it is enabled
        QString handler;           // ready: implementing C++ method or QML part
        QString kind;              // "" or "recent" (submenu of recent projects)
        bool checkable = false;
    };

    explicit CommandRegistry(QObject *parent = nullptr);   // loads the built-in commands.json

    bool load(const QByteArray &json, QString *error = nullptr);
    QStringList problems() const;          // consistency of the definitions (empty = fine)
    const QVector<Command> &commands() const { return m_commands; }
    const Command *find(const QString &id) const;

    QVariantList menus() const { return m_menus; }
    QVariantList toolbar() const { return m_toolbar; }
    int revision() const { return m_revision; }

    Q_INVOKABLE QStringList ids() const;
    Q_INVOKABLE QVariantMap command(const QString &id) const;
    Q_INVOKABLE QString title(const QString &id) const;
    Q_INVOKABLE QString icon(const QString &id) const;
    Q_INVOKABLE bool isReady(const QString &id) const;
    Q_INVOKABLE bool isCheckable(const QString &id) const;
    Q_INVOKABLE QString shortcut(const QString &id) const;        // effective, portable text
    Q_INVOKABLE QString shortcutText(const QString &id) const;    // effective, as shown to the user
    Q_INVOKABLE QString unavailableReason(const QString &id) const;
    Q_INVOKABLE QString tooltip(const QString &id) const;
    Q_INVOKABLE QVariantList shortcutList() const;
    // Sets the user's shortcut ("" removes it). Returns an error text, or "" when applied.
    Q_INVOKABLE QString setShortcut(const QString &id, const QString &text);
    Q_INVOKABLE void resetShortcuts();
    // Portable text of a key press (for capturing a new shortcut); "" for a lone modifier key.
    Q_INVOKABLE QString keyText(int key, int modifiers) const;

    static QString normalizedShortcut(const QString &text, bool *ok = nullptr);

signals:
    void shortcutsChanged();

private:
    void loadOverrides();

    QVector<Command> m_commands;
    QHash<QString, int> m_index;
    QHash<QString, QString> m_overrides;   // id -> portable text ("" = removed)
    QVariantList m_menus;
    QVariantList m_toolbar;
    int m_revision = 0;
};
