#include "AiService.h"
#include "Platform.h"
#include "SamSegmenter.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QSettings>
#include <QUrl>

namespace ai {

AiService::AiService(QObject *parent) : QObject(parent)
{
    m_chosen = QSettings().value(QStringLiteral("ai/segmentationModel")).toString();
    refresh();
}

QStringList AiService::modelRoots()
{
    QStringList roots;
    const QString env = qEnvironmentVariable("GEOMAP_MODELS_DIR");
    if (!env.isEmpty()) roots << env.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    roots << QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("models"));
    roots << QDir(Platform::dataDirectory()).filePath(QStringLiteral("models"));
    return roots;
}

bool AiService::runtimeAvailable() const { return ai::runtimeAvailable(); }
QString AiService::runtimeVersion() const { return ai::runtimeVersion(); }
QString AiService::userModelsFolder() const { return QDir::toNativeSeparators(QDir(Platform::dataDirectory()).filePath(QStringLiteral("models"))); }

bool AiService::supported(const ModelCard &card)
{
    // SAM-family models: an image encoder and a prompt (mask) decoder.
    if (card.task == QLatin1String(SegmentationTask))
        return !card.filePath(QStringLiteral("encoder")).isEmpty() && !card.filePath(QStringLiteral("decoder")).isEmpty();
    // LaMa-like models: image and mask in, the filled image out.
    if (card.task == QLatin1String(InpaintingTask)) return !card.filePath(QStringLiteral("inpainter")).isEmpty();
    return false;
}

ModelCard AiService::inpaintingCard() const
{
    for (const ModelCard &c : m_models)
        if (c.isValid() && c.task == QLatin1String(InpaintingTask) && supported(c)) return c;
    ModelCard none;
    none.error = QStringLiteral("Нет модели дорисовки (LaMa) в папке models рядом с программой или в %1").arg(userModelsFolder());
    return none;
}

QString AiService::inpaintingProblem() const
{
    if (!runtimeAvailable()) return ai::runtimeVersion();
    const ModelCard c = inpaintingCard();
    return c.isValid() ? QString() : c.error;
}

void AiService::refresh()
{
    m_models = discoverModels(modelRoots());
    emit modelsChanged();
}

QVariantList AiService::modelsForQml() const
{
    const QString chosen = segmentationCard().id;
    QVariantList list;
    for (const ModelCard &c : m_models) {
        QVariantMap m = c.toVariant();
        m.insert(QStringLiteral("supported"), supported(c));
        m.insert(QStringLiteral("chosen"), c.isValid() && (c.id == chosen || (c.task == QLatin1String(InpaintingTask) && c.id == inpaintingCard().id)));
        list << m;
    }
    return list;
}

QString AiService::segmentationModel() const { return segmentationCard().id; }

void AiService::setSegmentationModel(const QString &id)
{
    if (id == m_chosen) return;
    m_chosen = id;
    QSettings().setValue(QStringLiteral("ai/segmentationModel"), id);
    emit modelsChanged();
}

ModelCard AiService::segmentationCard() const
{
    const ModelCard *first = nullptr;
    for (const ModelCard &c : m_models) {
        if (!c.isValid() || c.task != QLatin1String(SegmentationTask) || !supported(c)) continue;
        if (c.id == m_chosen) return c;
        if (!first) first = &c;
    }
    if (first) return *first;
    ModelCard none;
    none.error = QStringLiteral("Нет модели для выделения объектов (папка models рядом с программой или %1)").arg(userModelsFolder());
    return none;
}

QString AiService::segmentationProblem() const
{
    if (!runtimeAvailable()) return ai::runtimeVersion();
    const ModelCard c = segmentationCard();
    return c.isValid() ? QString() : c.error;
}

bool AiService::openModelsFolder()
{
    const QString folder = userModelsFolder();
    QDir().mkpath(folder);
    return QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
}

} // namespace ai
