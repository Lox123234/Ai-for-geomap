#include "ModelCard.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace ai {

QString ModelCard::filePath(const QString &role) const
{
    for (const ModelFile &f : files)
        if (f.role == role) return QDir(directory).filePath(f.name);
    return {};
}

QVariantMap ModelCard::toVariant() const
{
    QVariantList list;
    for (const ModelFile &f : files) list << QVariantMap{{"role", f.role}, {"name", f.name}, {"sha256", f.sha256}};
    return {{"id", id}, {"name", name}, {"task", task}, {"license", license}, {"source", source}, {"weights", weights},
            {"conversion", conversion}, {"authors", authors}, {"input", input}, {"output", output}, {"limitations", limitations},
            {"directory", QDir::toNativeSeparators(directory)}, {"files", list}, {"error", error}, {"valid", isValid()}};
}

ModelCard readCard(const QString &directory)
{
    ModelCard c;
    c.directory = QDir(directory).absolutePath();
    c.id = QFileInfo(directory).fileName();
    QFile f(QDir(directory).filePath(QStringLiteral("model.json")));
    if (!f.open(QIODevice::ReadOnly)) {
        c.error = QStringLiteral("Нет карточки модели (model.json)");
        return c;
    }
    QJsonParseError parse;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll(), &parse).object();
    if (parse.error != QJsonParseError::NoError || o.value("schemaVersion").toInt() != 1) {
        c.error = QStringLiteral("Карточка модели повреждена или другой версии");
        return c;
    }
    c.id = o.value("id").toString(c.id);
    c.name = o.value("name").toString();
    c.task = o.value("task").toString();
    c.license = o.value("license").toString();
    c.licenseFile = o.value("licenseFile").toString();
    c.source = o.value("source").toString();
    c.weights = o.value("weights").toString();
    c.conversion = o.value("conversion").toString();
    c.authors = o.value("authors").toString();
    c.input = o.value("input").toString();
    c.output = o.value("output").toString();
    c.limitations = o.value("limitations").toString();
    for (const QJsonValue &v : o.value("files").toArray()) {
        const QJsonObject fo = v.toObject();
        ModelFile mf{fo.value("role").toString(), fo.value("name").toString(), fo.value("sha256").toString().toLower(), fo.value("url").toString()};
        // Plain file names only: a card cannot point outside its folder.
        if (mf.name.isEmpty() || mf.name.contains(QLatin1Char('/')) || mf.name.contains(QLatin1Char('\\')) || mf.name.contains(QStringLiteral(".."))) {
            c.error = QStringLiteral("Недопустимое имя файла в карточке: %1").arg(mf.name);
            return c;
        }
        c.files << mf;
    }
    // A model without a stated licence or origin is not used.
    if (c.name.isEmpty() || c.task.isEmpty()) c.error = QStringLiteral("В карточке нет названия или задачи модели");
    else if (c.license.isEmpty()) c.error = QStringLiteral("В карточке не указана лицензия модели");
    else if (c.source.isEmpty()) c.error = QStringLiteral("В карточке не указан источник модели");
    else if (c.files.isEmpty()) c.error = QStringLiteral("В карточке нет файлов модели");
    for (const ModelFile &mf : c.files)
        if (c.error.isEmpty() && mf.sha256.size() != 64) c.error = QStringLiteral("Нет SHA-256 у файла %1").arg(mf.name);
    return c;
}

bool verifyFiles(const ModelCard &card, QString *error)
{
    for (const ModelFile &f : card.files) {
        QFile file(QDir(card.directory).filePath(f.name));
        if (!file.open(QIODevice::ReadOnly)) {
            if (error) *error = QStringLiteral("Нет файла модели %1").arg(f.name);
            return false;
        }
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!hash.addData(&file) || QString::fromLatin1(hash.result().toHex()) != f.sha256) {
            if (error) *error = QStringLiteral("Файл %1 повреждён или подменён (SHA-256 не совпадает)").arg(f.name);
            return false;
        }
    }
    return true;
}

QVector<ModelCard> discoverModels(const QStringList &roots)
{
    QVector<ModelCard> cards;
    for (const QString &root : roots) {
        const QDir dir(root);
        if (!dir.exists()) continue;
        for (const QFileInfo &sub : dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
            if (QFileInfo::exists(sub.absoluteFilePath() + QStringLiteral("/model.json"))) cards << readCard(sub.absoluteFilePath());
    }
    return cards;
}

} // namespace ai
