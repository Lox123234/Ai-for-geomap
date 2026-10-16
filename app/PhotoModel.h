#pragma once

#include <QAbstractListModel>
#include <QDateTime>
#include <QJsonArray>
#include <QVariantMap>
#include <QVector>

struct Photo
{
    QString id;
    QString file;      // Relative path within the project
    QString name;
    QString sourcePath; // Informational only; never needed to open the project
    QString cameraMake;
    QString cameraModel;
    QString takenAt;
    qint64 bytes = 0;
    int width = 0;
    int height = 0;
    bool hasGps = false;
    double latitude = 0;
    double longitude = 0;
};

class PhotoModel final : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Roles {
        NameRole = Qt::UserRole + 1, UrlRole, WidthRole, HeightRole,
        HasGpsRole, LatitudeRole, LongitudeRole, CameraRole, TakenAtRole, IdRole,
        ThumbRole,     // image://thumbs/<file>, served from the project's thumbnail cache
        RowRole        // the row in this model (through the chunk filter: the project's row of the photo)
    };
    explicit PhotoModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    void resetPhotos(const QVector<Photo> &photos, const QString &baseDirectory);
    void append(const Photo &photo);
    void append(const QVector<Photo> &photos);
    bool erase(int row);
    Photo at(int row) const;
    QVariantMap details(int row) const;
    const QVector<Photo> &photos() const { return m_photos; }
private:
    QVector<Photo> m_photos;
    QString m_baseDirectory;
};
