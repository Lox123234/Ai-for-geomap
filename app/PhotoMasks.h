#pragma once

#include <QImage>
#include <QObject>
#include <QString>
#include <QUrl>

namespace ai { class AiService; }

// Masks of the project's photos for the reconstruction: <project>/photos/<photo>_mask.png, white
// where the photo is used, black where it is not (sky, moving cars, people, glare on water).
// OpenDroneMap reads a mask with that name next to its photo; jobs receive them with the photos.
// An area is excluded or given back by a click with the AI selection (SAM) on the photo as it is
// displayed (EXIF orientation applied, like OpenCV in the engine).
class PhotoMasks final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(int revision READ revision NOTIFY changed)   // grows with every change (image cache key)

public:
    PhotoMasks(ai::AiService *ai, QObject *parent = nullptr);
    void setProject(const QString &directory);

    bool busy() const { return m_busy; }
    QString status() const { return m_status; }
    int revision() const { return m_revision; }

    static QString maskFile(const QString &photoFile);     // "photos/<uuid>.jpg" → "photos/<uuid>_mask.png"
    Q_INVOKABLE bool hasMask(const QString &photoFile) const;
    Q_INVOKABLE int maskCount() const;
    // Red where the photo is excluded, transparent elsewhere (for drawing over the photo).
    Q_INVOKABLE QUrl overlayUrl(const QString &photoFile) const;
    // Click at (nx, ny) in [0, 1] of the displayed photo: the object under it excluded, or given
    // back with `include`. "" when started, else the reason; finished() tells the result.
    Q_INVOKABLE QString segmentAt(const QString &photoFile, double nx, double ny, bool include = false);
    Q_INVOKABLE void clearMask(const QString &photoFile);
    // Combines `objectMask` (255 = the object) into the photo's mask; for tests and tools.
    bool applyMask(const QString &photoFile, const QImage &objectMask, bool include, QString *error);

signals:
    void changed();
    void finished(bool ok, const QString &message);

private:
    void writeOverlay(const QString &photoFile, const QImage &mask) const;
    ai::AiService *m_ai;
    QString m_project;
    bool m_busy = false;
    QString m_status;
    int m_revision = 0;
    QString m_encodedPhoto;   // the photo the shared segmenter holds
    int m_generation = 0;
};
