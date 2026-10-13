#include "PhotoModel.h"
#include <QDir>
#include <QUrl>

PhotoModel::PhotoModel(QObject *parent) : QAbstractListModel(parent) {}

int PhotoModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_photos.size();
}

QVariant PhotoModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_photos.size())
        return {};
    const Photo &p = m_photos[index.row()];
    switch (role) {
    case NameRole: return p.name;
    case UrlRole: return QUrl::fromLocalFile(QDir(m_baseDirectory).filePath(p.file));
    case WidthRole: return p.width;
    case HeightRole: return p.height;
    case HasGpsRole: return p.hasGps;
    case LatitudeRole: return p.latitude;
    case LongitudeRole: return p.longitude;
    case CameraRole: return (p.cameraMake + " " + p.cameraModel).trimmed();
    case TakenAtRole: return p.takenAt;
    case IdRole: return p.id;
    default: return {};
    }
}

QHash<int, QByteArray> PhotoModel::roleNames() const
{
    return {
        {NameRole, "photoName"}, {UrlRole, "photoUrl"},
        {WidthRole, "photoWidth"}, {HeightRole, "photoHeight"},
        {HasGpsRole, "photoHasGps"}, {LatitudeRole, "photoLatitude"},
        {LongitudeRole, "photoLongitude"}, {CameraRole, "photoCamera"},
        {TakenAtRole, "photoTakenAt"}, {IdRole, "photoId"}
    };
}

void PhotoModel::resetPhotos(const QVector<Photo> &photos, const QString &baseDirectory)
{
    beginResetModel();
    m_photos = photos;
    m_baseDirectory = baseDirectory;
    endResetModel();
}

void PhotoModel::append(const Photo &photo)
{
    const int row = m_photos.size();
    beginInsertRows({}, row, row);
    m_photos.append(photo);
    endInsertRows();
}

bool PhotoModel::erase(int row)
{
    if (row < 0 || row >= m_photos.size()) return false;
    beginRemoveRows({}, row, row);
    m_photos.removeAt(row);
    endRemoveRows();
    return true;
}

Photo PhotoModel::at(int row) const
{
    return (row >= 0 && row < m_photos.size()) ? m_photos.at(row) : Photo{};
}

QVariantMap PhotoModel::details(int row) const
{
    if (row < 0 || row >= m_photos.size()) return {};
    const Photo &p = m_photos.at(row);
    return {
        {"id", p.id}, {"name", p.name},
        {"url", QUrl::fromLocalFile(QDir(m_baseDirectory).filePath(p.file))},
        {"width", p.width}, {"height", p.height}, {"bytes", p.bytes},
        {"camera", (p.cameraMake + " " + p.cameraModel).trimmed()},
        {"takenAt", p.takenAt}, {"hasGps", p.hasGps},
        {"latitude", p.latitude}, {"longitude", p.longitude}
    };
}
