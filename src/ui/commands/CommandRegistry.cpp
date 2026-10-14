#include "CommandRegistry.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QSet>
#include <QSettings>

namespace {
const char *SettingsGroup = "shortcuts";

QString stageText(const CommandRegistry::Command &c)
{
    if (c.status == QLatin1String("engine")) return QStringLiteral("Не поддерживается выбранным движком");
    return c.stage.isEmpty() ? QStringLiteral("Запланировано") : QStringLiteral("Запланировано: этап %1").arg(c.stage);
}
} // namespace

CommandRegistry::CommandRegistry(QObject *parent) : QObject(parent)
{
    QFile file(QStringLiteral(":/geomap/commands.json"));
    if (file.open(QIODevice::ReadOnly)) load(file.readAll());
    loadOverrides();
}

bool CommandRegistry::load(const QByteArray &json, QString *error)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (!doc.isObject()) {
        if (error) *error = parseError.errorString();
        return false;
    }
    const QJsonObject root = doc.object();
    m_commands.clear();
    m_index.clear();
    for (const QJsonValue &v : root.value("commands").toArray()) {
        const QJsonObject o = v.toObject();
        Command c;
        c.id = o.value("id").toString();
        c.title = o.value("title").toString();
        c.icon = o.value("icon").toString();
        c.defaultShortcut = o.value("shortcut").toString();
        c.status = o.value("status").toString();
        c.stage = o.value("stage").toString();
        c.reason = o.value("reason").toString();
        c.action = o.value("action").toString();
        c.enabledWhen = o.value("enabledWhen").toString();
        c.handler = o.value("handler").toString();
        c.kind = o.value("kind").toString();
        c.checkable = o.value("checkable").toBool();
        if (!m_index.contains(c.id)) m_index.insert(c.id, int(m_commands.size()));
        m_commands << c;
    }
    m_menus = root.value("menus").toArray().toVariantList();
    m_toolbar = root.value("toolbar").toArray().toVariantList();
    ++m_revision;
    emit shortcutsChanged();
    return true;
}

QStringList CommandRegistry::problems() const
{
    QStringList issues;
    QSet<QString> seen, shortcuts;
    for (const Command &c : m_commands) {
        if (c.id.isEmpty() || c.title.isEmpty()) issues << QStringLiteral("command without id or title: '%1'").arg(c.id);
        if (seen.contains(c.id)) issues << QStringLiteral("duplicate id: %1").arg(c.id);
        seen.insert(c.id);
        if (c.status == QLatin1String("ready")) {
            if (c.action.isEmpty() || c.enabledWhen.isEmpty() || c.handler.isEmpty())
                issues << QStringLiteral("%1: a ready command needs action, enabledWhen and handler").arg(c.id);
        } else if (c.status == QLatin1String("planned") || c.status == QLatin1String("engine")) {
            if (c.reason.isEmpty()) issues << QStringLiteral("%1: an unavailable command needs a reason").arg(c.id);
            if (c.status == QLatin1String("planned") && c.stage.isEmpty()) issues << QStringLiteral("%1: planned without stage").arg(c.id);
            if (!c.defaultShortcut.isEmpty() && c.status != QLatin1String("planned"))
                issues << QStringLiteral("%1: an engine-blocked command must not take a shortcut").arg(c.id);
        } else {
            issues << QStringLiteral("%1: unknown status '%2'").arg(c.id, c.status);
        }
        if (!c.defaultShortcut.isEmpty()) {
            bool ok = false;
            const QString key = normalizedShortcut(c.defaultShortcut, &ok);
            if (!ok) issues << QStringLiteral("%1: invalid shortcut '%2'").arg(c.id, c.defaultShortcut);
            else if (shortcuts.contains(key)) issues << QStringLiteral("%1: shortcut %2 is used twice").arg(c.id, key);
            shortcuts.insert(key);
        }
    }
    const auto checkIds = [&](const QVariantList &ids, const QString &where) {
        for (const QVariant &v : ids) {
            const QString id = v.toString();
            if (id != QLatin1String("-") && !m_index.contains(id)) issues << QStringLiteral("%1 refers to unknown command %2").arg(where, id);
        }
    };
    for (const QVariant &m : m_menus) {
        const QVariantMap menu = m.toMap();
        checkIds(menu.value("items").toList(), QStringLiteral("menu %1").arg(menu.value("title").toString()));
    }
    for (const QVariant &g : m_toolbar) {
        const QVariantMap group = g.toMap();
        checkIds(group.value("items").toList(), QStringLiteral("toolbar group %1").arg(group.value("group").toString()));
        for (const QVariant &v : group.value("items").toList())
            if (const Command *c = find(v.toString()); c && c->icon.isEmpty())
                issues << QStringLiteral("toolbar command %1 has no icon").arg(c->id);
    }
    return issues;
}

const CommandRegistry::Command *CommandRegistry::find(const QString &id) const
{
    const auto it = m_index.constFind(id);
    return it == m_index.constEnd() ? nullptr : &m_commands[*it];
}

QStringList CommandRegistry::ids() const
{
    QStringList list;
    for (const Command &c : m_commands) list << c.id;
    return list;
}

QVariantMap CommandRegistry::command(const QString &id) const
{
    const Command *c = find(id);
    if (!c) return {};
    return {{"id", c->id}, {"title", c->title}, {"icon", c->icon}, {"shortcut", shortcut(id)},
            {"shortcutText", shortcutText(id)}, {"defaultShortcut", c->defaultShortcut}, {"status", c->status},
            {"stage", c->stage}, {"reason", c->reason}, {"action", c->action}, {"enabledWhen", c->enabledWhen},
            {"handler", c->handler}, {"kind", c->kind}, {"checkable", c->checkable}};
}

QString CommandRegistry::title(const QString &id) const
{
    const Command *c = find(id);
    return c ? c->title : id;
}

QString CommandRegistry::icon(const QString &id) const
{
    const Command *c = find(id);
    return c ? c->icon : QString();
}

bool CommandRegistry::isReady(const QString &id) const
{
    const Command *c = find(id);
    return c && c->status == QLatin1String("ready");
}

bool CommandRegistry::isCheckable(const QString &id) const
{
    const Command *c = find(id);
    return c && c->checkable;
}

QString CommandRegistry::shortcut(const QString &id) const
{
    const Command *c = find(id);
    if (!c) return {};
    const auto it = m_overrides.constFind(id);
    return it != m_overrides.constEnd() ? *it : c->defaultShortcut;
}

QString CommandRegistry::shortcutText(const QString &id) const
{
    return QKeySequence::fromString(shortcut(id), QKeySequence::PortableText).toString(QKeySequence::NativeText);
}

QString CommandRegistry::unavailableReason(const QString &id) const
{
    const Command *c = find(id);
    if (!c) return QStringLiteral("Неизвестная команда");
    if (c->status == QLatin1String("ready")) return {};
    return stageText(*c) + QStringLiteral(". ") + c->reason;
}

QString CommandRegistry::tooltip(const QString &id) const
{
    const Command *c = find(id);
    if (!c) return id;
    QString text = c->title;
    text.remove(QChar(u'…'));
    const QString keys = shortcutText(id);
    if (!keys.isEmpty()) text += QStringLiteral(" (%1)").arg(keys);
    if (c->status == QLatin1String("ready")) {
        if (!c->action.isEmpty()) text += QStringLiteral("\n") + c->action;
    } else {
        text += QStringLiteral("\n") + unavailableReason(id);
    }
    return text;
}

QVariantList CommandRegistry::shortcutList() const
{
    QVariantList list;
    for (const Command &c : m_commands) {
        if (c.status != QLatin1String("ready") || c.kind == QLatin1String("recent")) continue;
        list << QVariantMap{{"id", c.id}, {"title", c.title}, {"shortcut", shortcut(c.id)}, {"shortcutText", shortcutText(c.id)},
                            {"defaultShortcut", c.defaultShortcut}, {"changed", m_overrides.contains(c.id)}};
    }
    return list;
}

QString CommandRegistry::normalizedShortcut(const QString &text, bool *ok)
{
    const QKeySequence seq = QKeySequence::fromString(text.trimmed(), QKeySequence::PortableText);
    const bool valid = seq.count() == 1 && seq[0].key() != Qt::Key_unknown;
    if (ok) *ok = valid;
    return valid ? seq.toString(QKeySequence::PortableText) : QString();
}

QString CommandRegistry::setShortcut(const QString &id, const QString &text)
{
    const Command *c = find(id);
    if (!c || c->status != QLatin1String("ready")) return QStringLiteral("Команде нельзя назначить клавиши.");
    QString key;
    if (!text.trimmed().isEmpty()) {
        bool ok = false;
        key = normalizedShortcut(text, &ok);
        if (!ok) return QStringLiteral("Не удалось распознать сочетание «%1». Пример: Ctrl+Shift+K.").arg(text);
        for (const Command &other : m_commands)
            if (other.id != id && !shortcut(other.id).isEmpty() && normalizedShortcut(shortcut(other.id)) == key)
                return QStringLiteral("%1 уже назначено команде «%2».").arg(text.trimmed(), other.title);
    }
    QSettings settings;
    settings.beginGroup(QLatin1String(SettingsGroup));
    if (key == normalizedShortcut(c->defaultShortcut)) {
        m_overrides.remove(id);
        settings.remove(id);
    } else {
        m_overrides.insert(id, key);
        settings.setValue(id, key);
    }
    ++m_revision;
    emit shortcutsChanged();
    return {};
}

void CommandRegistry::resetShortcuts()
{
    QSettings settings;
    settings.remove(QLatin1String(SettingsGroup));
    m_overrides.clear();
    ++m_revision;
    emit shortcutsChanged();
}

QString CommandRegistry::keyText(int key, int modifiers) const
{
    switch (key) {
    case Qt::Key_Control: case Qt::Key_Shift: case Qt::Key_Alt: case Qt::Key_Meta: case Qt::Key_AltGr:
    case Qt::Key_unknown: case 0:
        return {};
    default:
        break;
    }
    const auto mods = Qt::KeyboardModifiers(modifiers) & (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier);
    return QKeySequence(QKeyCombination(mods, Qt::Key(key))).toString(QKeySequence::PortableText);
}

void CommandRegistry::loadOverrides()
{
    QSettings settings;
    settings.beginGroup(QLatin1String(SettingsGroup));
    for (const QString &id : settings.childKeys()) {
        if (!find(id)) continue;
        const QString value = settings.value(id).toString();
        bool ok = value.isEmpty();
        const QString key = value.isEmpty() ? QString() : normalizedShortcut(value, &ok);
        if (ok) m_overrides.insert(id, key);
    }
}
