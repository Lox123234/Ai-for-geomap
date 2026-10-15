#pragma once

#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

// Local AI models are folders with a card (model.json): what the model does, its licence and
// origin, its files with SHA-256, inputs, outputs and limitations. A model is used only when its
// card is complete and every file matches its checksum.
namespace ai {

struct ModelFile
{
    QString role;     // e.g. encoder, decoder
    QString name;     // file name in the model folder
    QString sha256;   // lower-case hex
    QString url;      // where it was downloaded from (documentation; never fetched at run time)
};

struct ModelCard
{
    QString id, name, task, license, licenseFile, source, weights, conversion, authors, input, output, limitations;
    QVector<ModelFile> files;
    QString directory;
    QString error;    // why the card cannot be used
    bool isValid() const { return error.isEmpty(); }
    QString filePath(const QString &role) const;   // absolute path of the file with this role, or ""
    QVariantMap toVariant() const;
};

// Reads <directory>/model.json; `error` is set when it is missing or incomplete.
ModelCard readCard(const QString &directory);
// SHA-256 of every file; false with the first problem (missing file, wrong checksum).
bool verifyFiles(const ModelCard &card, QString *error);
// Cards of the folders directly inside each root (e.g. <app>/models); invalid cards included.
QVector<ModelCard> discoverModels(const QStringList &roots);

} // namespace ai
