#pragma once

#include <QImage>
#include <QString>
#include <QVector>
#include <QVector2D>
#include <QVector3D>

// 3D data of the viewer, kept in single precision relative to a scene origin in double
// precision (UTM coordinates do not fit a float with centimetre resolution). Loaders cap what
// they keep in memory and report how much of the source was shown.
namespace view3d {

struct Origin
{
    double x = 0, y = 0, z = 0;
    bool valid = false;
};

struct Bounds
{
    QVector3D min, max;
    bool valid = false;
    void add(const QVector3D &p);
    void add(const Bounds &b);
    QVector3D center() const { return (min + max) * 0.5f; }
    QVector3D size() const { return max - min; }
    float radius() const { return valid ? (max - min).length() * 0.5f : 1.0f; }
};

struct PointCloud
{
    QVector<QVector3D> positions;   // relative to the origin
    QVector<quint32> colors;        // 0xAARRGGBB, empty without colours
    QVector<quint8> classes;        // ASPRS classes (LAS); empty when nothing is classified
    QVector<quint8> intensity;      // LAS intensity scaled to 0..255 over the cloud; empty when absent
    quint64 totalPoints = 0;        // in the source
    Bounds bounds;
    Origin origin;
    bool georeferenced = false;     // coordinates in a known CRS (ODM coords.txt, LAS CRS record)
    QString crs;                    // its description, when known
    QString crsWkt;                 // its definition, when known (the basemap under the scene)
    QString source;
    QString error;
    bool isValid() const { return error.isEmpty() && !positions.isEmpty(); }
};

struct Mesh
{
    struct Part
    {
        QString material;
        QImage texture;             // empty: vertex colours or a plain colour
        QVector<quint32> indices;   // triangles
    };
    QVector<QVector3D> positions;   // relative to the origin
    QVector<QVector2D> uvs;         // empty without texture coordinates
    QVector<QVector3D> normals;     // per vertex
    QVector<quint32> colors;        // per vertex, empty if none
    QVector<Part> parts;
    quint64 triangles = 0;
    Bounds bounds;
    Origin origin;
    bool georeferenced = false;     // ODM model (coords.txt) or a DEM with a CRS
    QString crs;
    QString crsWkt;                 // definition of the CRS, when known
    QString source;
    QString error;
    QString note;                   // e.g. textures that were scaled down
    bool isValid() const { return error.isEmpty() && !positions.isEmpty() && !parts.isEmpty(); }
};

// Offset of ODM's local model coordinates: odm_georeferencing/coords.txt (2nd line "E N").
bool readOdmOffset(const QString &coordsFile, Origin *origin, QString *crsLine = nullptr);
// ODM keeps coords.txt next to its outputs: finds it for a file inside a job folder.
QString odmCoordsFor(const QString &file);

// PLY (ascii / binary): vertices with optional colours in any property order; faces make a mesh.
// At most maxPoints points are kept (every k-th), totalPoints tells how many there were.
PointCloud readPointCloudPly(const QString &path, int maxPoints, const Origin &origin = {});
// LAS 1.0–1.4, point formats 0–10, uncompressed (LAZ is reported as not supported yet).
PointCloud readPointCloudLas(const QString &path, int maxPoints, const Origin &origin = {});
PointCloud readPointCloud(const QString &path, int maxPoints, const Origin &origin = {});   // by extension

// Colour and name of an ASPRS point class (2 ground, 3–5 vegetation, 6 building, 9 water, …).
quint32 classColor(int asprsClass);   // 0xAARRGGBB
QString className(int asprsClass);

// Wavefront OBJ with MTL materials and map_Kd textures; polygons are triangulated; textures
// larger than maxTexture are scaled down. Positions are taken as they are (local for ODM).
Mesh readObj(const QString &path, int maxTexture = 8192);
// PLY with faces (e.g. ODM's odm_mesh.ply), vertex colours if present.
Mesh readMeshPly(const QString &path);

// A DEM (GeoTIFF) as a terrain mesh of at most maxGrid × maxGrid vertices; nodata cells leave
// holes. Vertex colours from an elevation ramp; `drape` (optional) is an image of the DEM's
// extent used as texture (e.g. the orthophoto). Coordinates relative to `origin`, or to the
// DEM's corner when the origin is not valid (the origin used is stored in the mesh).
Mesh demMesh(const QString &path, int maxGrid, const Origin &origin = {}, const QImage &drape = {});

// An image of the DEM's extent cut from an orthophoto in the same CRS (for draping a terrain),
// at most maxSize pixels on the longer side; null image with a note when it does not fit.
QImage readDrape(const QString &orthoPath, const QString &demPath, int maxSize, QString *note = nullptr);

// Normals for a mesh without them (area-weighted).
void computeNormals(Mesh &mesh);

} // namespace view3d
