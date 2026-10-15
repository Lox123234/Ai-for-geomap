#pragma once

#include "ModelCard.h"

#include <QObject>
#include <QVariantList>

// Local AI models for the interface: the cards found in <app>/models (shipped) and
// <data folder>/models (the user's own), the runtime, and the model chosen for each task.
// Inference itself runs in the services that use it (MapService: selection by click).
namespace ai {

class AiService final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool runtimeAvailable READ runtimeAvailable CONSTANT)
    Q_PROPERTY(QString runtimeVersion READ runtimeVersion CONSTANT)
    Q_PROPERTY(QString userModelsFolder READ userModelsFolder CONSTANT)
    Q_PROPERTY(QVariantList models READ modelsForQml NOTIFY modelsChanged)
    Q_PROPERTY(QString segmentationModel READ segmentationModel WRITE setSegmentationModel NOTIFY modelsChanged)
    Q_PROPERTY(QString segmentationProblem READ segmentationProblem NOTIFY modelsChanged)
    Q_PROPERTY(QString inpaintingProblem READ inpaintingProblem NOTIFY modelsChanged)

public:
    static constexpr const char *SegmentationTask = "interactive-segmentation";
    static constexpr const char *InpaintingTask = "inpainting";

    explicit AiService(QObject *parent = nullptr);
    // GEOMAP_MODELS_DIR (folders separated by ';'), <app>/models, <data folder>/models.
    static QStringList modelRoots();

    bool runtimeAvailable() const;
    QString runtimeVersion() const;
    QString userModelsFolder() const;
    QVariantList modelsForQml() const;
    const QVector<ModelCard> &models() const { return m_models; }
    QString segmentationModel() const;          // id of the card used by the click selection
    void setSegmentationModel(const QString &id);
    QString segmentationProblem() const;        // "" when the click selection can work, else why not
    ModelCard segmentationCard() const;         // invalid (with error) when there is none
    QString inpaintingProblem() const;          // "" when objects can be removed from orthophotos
    ModelCard inpaintingCard() const;           // the first valid inpainting model

    Q_INVOKABLE void refresh();
    Q_INVOKABLE bool openModelsFolder();        // creates the user's folder when needed

signals:
    void modelsChanged();

private:
    static bool supported(const ModelCard &card);
    QVector<ModelCard> m_models;
    QString m_chosen;
};

} // namespace ai
