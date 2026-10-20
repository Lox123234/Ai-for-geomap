#pragma once

#include <QImage>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QQuickImageProvider>
#include <QRectF>
#include <QVariantList>

class ProcessingService;
class Scene3DService;
namespace ai { class AiService; }

// Инструменты → Редактор текстуры: the texture of a job's 3D model edited in a copy
// (<job>/geomap/texture/: the OBJ, its MTL and the atlases), never in ODM's files — colour
// (brightness, contrast, saturation, warmth, gamma) of every atlas, spots painted over with the
// inpainting model (LaMa), one step back, the copy in the 3D view and with the job's results.
class TextureEditService final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList models READ models NOTIFY modelsChanged)       // {job, title}
    Q_PROPERTY(QString jobId READ jobId NOTIFY atlasesChanged)
    Q_PROPERTY(QVariantList atlases READ atlases NOTIFY atlasesChanged)    // {index, name, width, height}
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)          // previews to reload
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY revisionChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString status READ status NOTIFY busyChanged)
    Q_PROPERTY(double brightness READ brightness WRITE setBrightness NOTIFY adjustChanged)   // −1 … 1
    Q_PROPERTY(double contrast READ contrast WRITE setContrast NOTIFY adjustChanged)         // −1 … 1
    Q_PROPERTY(double saturation READ saturation WRITE setSaturation NOTIFY adjustChanged)   // −1 … 1
    Q_PROPERTY(double warmth READ warmth WRITE setWarmth NOTIFY adjustChanged)               // −1 … 1
    Q_PROPERTY(double gamma READ gamma WRITE setGamma NOTIFY adjustChanged)                  // 0,5 … 2
public:
    TextureEditService(ProcessingService *processing, Scene3DService *scene, ai::AiService *ai, QObject *parent = nullptr);

    QVariantList models() const;
    QString jobId() const { return m_job; }
    QVariantList atlases() const;
    int revision() const { return m_revision; }
    bool canUndo() const { return !m_undo.isEmpty(); }
    bool busy() const { return m_busy; }
    QString status() const { return m_status; }
    double brightness() const { return m_adjust.brightness; }
    double contrast() const { return m_adjust.contrast; }
    double saturation() const { return m_adjust.saturation; }
    double warmth() const { return m_adjust.warmth; }
    double gamma() const { return m_adjust.gamma; }
    void setBrightness(double v);
    void setContrast(double v);
    void setSaturation(double v);
    void setWarmth(double v);
    void setGamma(double v);

    // Opens the model of a job (its working copy, made on first use). "" or the reason.
    Q_INVOKABLE QString open(const QString &jobId);
    Q_INVOKABLE void resetAdjustments();
    // The colour settings into every atlas of the copy (in the background). "" when started.
    Q_INVOKABLE QString applyAdjustments();
    // Strokes {x, y} in [0, 1] of the atlas, `radius` in its width: painted over by LaMa. "" when started.
    Q_INVOKABLE QString retouch(int atlas, const QVariantList &strokes, double radius);
    Q_INVOKABLE void undo();
    Q_INVOKABLE QString restoreOriginal();      // the copy made again from ODM's model
    Q_INVOKABLE QString showIn3D();
    Q_INVOKABLE void openFolder();
    // The part of an atlas its charts take, in [0, 1] (the rest is empty): where the view starts.
    Q_INVOKABLE QRectF usedRect(int atlas) const;
    QString copyObj() const;                    // the OBJ of the copy

    // The atlas reduced to `maxSide`, with the colour settings applied (the preview).
    QImage preview(int atlas, int maxSide) const;
    struct Adjust
    {
        double brightness = 0, contrast = 0, saturation = 0, warmth = 0, gamma = 1;
        bool neutral() const { return brightness == 0 && contrast == 0 && saturation == 0 && warmth == 0 && gamma == 1; }
    };
    static QImage adjusted(const QImage &image, const Adjust &a);

signals:
    void modelsChanged();
    void atlasesChanged();
    void revisionChanged();
    void busyChanged();
    void adjustChanged();
    void finished(bool ok, const QString &message);

private:
    void setBusy(bool busy, const QString &status);
    QString atlasPath(int index) const;
    void loadAtlases();

    QPointer<ProcessingService> m_processing;
    QPointer<Scene3DService> m_scene;
    ai::AiService *m_ai = nullptr;
    QString m_job, m_source, m_copy;            // the job, ODM's model folder, the copy's folder
    QStringList m_atlases;                      // file names in the copy
    QVector<QSize> m_sizes;
    Adjust m_adjust;
    mutable QMutex m_cacheLock;
    mutable QHash<int, QImage> m_small;         // reduced atlases for the previews
    QStringList m_undo;                         // atlases saved before the last change (in .geomap-undo/)
    int m_revision = 0;
    bool m_busy = false;
    QString m_status;
};

// image://texture/<atlas>/<revision>: the preview of an atlas.
class TextureImageProvider final : public QQuickImageProvider
{
public:
    explicit TextureImageProvider(TextureEditService *service);
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;

private:
    QPointer<TextureEditService> m_service;
};
