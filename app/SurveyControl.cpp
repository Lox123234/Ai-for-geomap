#include "SurveyControl.h"
#include "Geodesy.h"
#include "MapService.h"
#include "PhotoModel.h"
#include "TrueOrthophoto.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <cpl_conv.h>
#include <ogr_srs_api.h>

namespace {
const QString DataFile = QStringLiteral("gcp.json");

struct Data
{
    Crs crs;
    QVector<SurveyControl::Point> points;
    QVector<SurveyControl::ScaleBar> bars;
    QString calibration;
};

Data readData(const QString &project)
{
    Data d;
    QFile f(QDir(project).filePath(DataFile));
    if (!f.open(QIODevice::ReadOnly)) return d;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    if (!o.value(QStringLiteral("crs")).toString().isEmpty()) d.crs = Crs::fromWkt(o.value(QStringLiteral("crs")).toString());
    for (const QJsonValue &v : o.value(QStringLiteral("points")).toArray()) {
        const QJsonObject p = v.toObject();
        SurveyControl::Point point;
        point.id = p.value(QStringLiteral("id")).toString();
        point.name = p.value(QStringLiteral("name")).toString();
        point.x = p.value(QStringLiteral("x")).toDouble();
        point.y = p.value(QStringLiteral("y")).toDouble();
        point.z = p.value(QStringLiteral("z")).toDouble();
        point.check = p.value(QStringLiteral("check")).toBool();
        for (const QJsonValue &ov : p.value(QStringLiteral("observations")).toArray()) {
            const QJsonObject obs = ov.toObject();
            point.observations.insert(obs.value(QStringLiteral("photo")).toString(),
                                      QPointF(obs.value(QStringLiteral("x")).toDouble(), obs.value(QStringLiteral("y")).toDouble()));
        }
        if (!point.id.isEmpty()) d.points << point;
    }
    for (const QJsonValue &v : o.value(QStringLiteral("scaleBars")).toArray()) {
        const QJsonObject b = v.toObject();
        d.bars << SurveyControl::ScaleBar{b.value(QStringLiteral("id")).toString(), b.value(QStringLiteral("a")).toString(),
                                          b.value(QStringLiteral("b")).toString(), b.value(QStringLiteral("distance")).toDouble()};
    }
    d.calibration = o.value(QStringLiteral("calibration")).toString();
    return d;
}

// Size of a photo as displayed (EXIF orientation applied), from its header.
QSize displayedSize(const QString &path)
{
    QImageReader r(path);
    r.setAutoTransform(true);
    QSize size = r.size();
    if (r.transformation() & QImageIOHandler::TransformationRotate90) size.transpose();
    return size;
}

// The first line of gcp_list.txt: EPSG code or PROJ string of the points' CRS.
QString crsHeader(const Crs &crs, QString *error)
{
    if (!crs.isValid()) {
        *error = QStringLiteral("Не задана система координат опорных точек");
        return {};
    }
    if (crs.epsg() > 0) return QStringLiteral("EPSG:%1").arg(crs.epsg());
    QString header;
    OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
    if (OSRSetFromUserInput(srs, crs.wkt().toUtf8().constData()) == OGRERR_NONE) {
        char *proj = nullptr;
        if (OSRExportToProj4(srs, &proj) == OGRERR_NONE && proj) header = QString::fromUtf8(proj).trimmed();
        CPLFree(proj);
    }
    OSRDestroySpatialReference(srs);
    if (header.isEmpty()) *error = QStringLiteral("Систему координат точек нельзя записать для движка: задайте её кодом EPSG");
    return header;
}

QString gcpName(const QString &name, QSet<QString> *used)
{
    QString n = name.simplified();
    n.replace(QLatin1Char(' '), QLatin1Char('_'));
    if (n.isEmpty()) n = QStringLiteral("gcp");
    QString unique = n;
    for (int i = 2; used->contains(unique); ++i) unique = n + QLatin1Char('_') + QString::number(i);
    used->insert(unique);
    return unique;
}

// gcp_list.txt of OpenDroneMap: the control points seen on at least `minImages` photos whose
// file names are in `images` (all photos when empty). Pixels have their centres at integers.
QString gcpListText(const QString &project, const Data &d, const QSet<QString> &images, int minImages, int *used,
                    QStringList *dropped, QString *error)
{
    *used = 0;
    const QString header = crsHeader(d.crs, error);
    if (header.isEmpty()) return {};
    const int decimals = d.crs.isGeographic() ? 9 : 4;
    QString text = header + QLatin1Char('\n');
    QSet<QString> names;
    QHash<QString, QSize> sizes;
    for (const SurveyControl::Point &p : d.points) {
        if (p.check) continue;
        QStringList lines;
        const QString name = gcpName(p.name, &names);
        for (auto it = p.observations.cbegin(); it != p.observations.cend(); ++it) {
            const QString image = QFileInfo(it.key()).fileName();
            if (!images.isEmpty() && !images.contains(image)) continue;
            if (!sizes.contains(it.key())) sizes.insert(it.key(), displayedSize(QDir(project).filePath(it.key())));
            const QSize size = sizes.value(it.key());
            if (!size.isValid()) continue;   // the photo is gone
            lines << QStringLiteral("%1 %2 %3 %4 %5 %6 %7")
                         .arg(QString::number(p.x, 'f', decimals), QString::number(p.y, 'f', decimals), QString::number(p.z, 'f', 4),
                              QString::number(it.value().x() * size.width() - 0.5, 'f', 2),
                              QString::number(it.value().y() * size.height() - 0.5, 'f', 2), image, name);
        }
        if (lines.size() < minImages) {
            if (dropped) *dropped << p.name;
            continue;
        }
        text += lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
        ++*used;
    }
    return text;
}

// CRS of the job's results (the reconstruction is in it, shifted by the offset of coords.txt).
Crs jobCrs(const QString &job)
{
    const QString wkt = ortho::resultCrsWkt(job);
    return wkt.isEmpty() ? Crs() : Crs::fromWkt(wkt);
}

// Ray of a photo position ([0, 1] of the photo) in the frame of the reconstruction.
void unproject(const ortho::Shot &s, const QPointF &n, double dir[3])
{
    s.ray(n.x() * s.camera.width - 0.5, n.y() * s.camera.height - 0.5, dir);
}

bool solve3(const double A[9], const double b[3], double x[3])
{
    const double det = A[0] * (A[4] * A[8] - A[5] * A[7]) - A[1] * (A[3] * A[8] - A[5] * A[6]) + A[2] * (A[3] * A[7] - A[4] * A[6]);
    if (std::fabs(det) < 1e-12) return false;
    for (int c = 0; c < 3; ++c) {
        double M[9];
        std::copy(A, A + 9, M);
        for (int r = 0; r < 3; ++r) M[r * 3 + c] = b[r];
        x[c] = (M[0] * (M[4] * M[8] - M[5] * M[7]) - M[1] * (M[3] * M[8] - M[5] * M[6]) + M[2] * (M[3] * M[7] - M[4] * M[6])) / det;
    }
    return true;
}

// The point seen at `positions` on `shots`: midpoint of the rays, refined by Gauss–Newton on
// the reprojection error. `reprojection` = RMS error on the photos in pixels.
bool triangulate(const QVector<const ortho::Shot *> &shots, const QVector<QPointF> &positions, double X[3], double *reprojection)
{
    const int n = int(shots.size());
    if (n < 2) return false;
    double A[9] = {0}, b[3] = {0};
    for (int i = 0; i < n; ++i) {
        double d[3];
        unproject(*shots[i], positions[i], d);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) {
                const double m = (r == c ? 1.0 : 0.0) - d[r] * d[c];
                A[r * 3 + c] += m;
                b[r] += m * shots[i]->centre[c];
            }
    }
    if (!solve3(A, b, X)) return false;
    auto residuals = [&](const double P[3], std::vector<double> *res) {
        res->clear();
        for (int i = 0; i < n; ++i) {
            double px, py, depth;
            if (!shots[i]->project(P, &px, &py, &depth)) return false;
            res->push_back(px + 0.5 - positions[i].x() * shots[i]->camera.width);
            res->push_back(py + 0.5 - positions[i].y() * shots[i]->camera.height);
        }
        return true;
    };
    std::vector<double> r0, r1;
    if (!residuals(X, &r0)) return false;
    for (int it = 0; it < 10; ++it) {
        std::vector<std::vector<double>> cols(3);   // numeric Jacobian by coordinate
        bool ok = true;
        for (int k = 0; k < 3 && ok; ++k) {
            double P[3] = {X[0], X[1], X[2]};
            const double h = 1e-4 * qMax(1.0, std::fabs(X[k]) * 1e-6);
            P[k] += h;
            ok = residuals(P, &r1);
            if (ok) {
                cols[k].resize(r1.size());
                for (size_t j = 0; j < r1.size(); ++j) cols[k][j] = (r1[j] - r0[j]) / h;
            }
        }
        if (!ok) break;
        double N[9] = {0}, g[3] = {0};
        for (int a = 0; a < 3; ++a) {
            for (int c = 0; c < 3; ++c)
                for (size_t j = 0; j < r0.size(); ++j) N[a * 3 + c] += cols[a][j] * cols[c][j];
            for (size_t j = 0; j < r0.size(); ++j) g[a] -= cols[a][j] * r0[j];
        }
        double step[3];
        if (!solve3(N, g, step)) break;
        const double P[3] = {X[0] + step[0], X[1] + step[1], X[2] + step[2]};
        if (!residuals(P, &r1)) break;
        double e0 = 0, e1 = 0;
        for (double v : r0) e0 += v * v;
        for (double v : r1) e1 += v * v;
        if (e1 > e0) break;
        std::copy(P, P + 3, X);
        r0 = r1;
        if (std::sqrt(step[0] * step[0] + step[1] * step[1] + step[2] * step[2]) < 1e-6) break;
    }
    double e = 0;
    for (double v : r0) e += v * v;
    if (reprojection) *reprojection = std::sqrt(e / n);
    return true;
}

QVariantMap rmse(const QVector<QVector<double>> &d)   // rows of dx, dy, dz
{
    double sx = 0, sy = 0, sz = 0;
    for (const QVector<double> &r : d) {
        sx += r[0] * r[0];
        sy += r[1] * r[1];
        sz += r[2] * r[2];
    }
    const int n = int(d.size());
    QVariantMap m;
    m.insert(QStringLiteral("count"), n);
    if (n == 0) return m;
    m.insert(QStringLiteral("rmseX"), std::sqrt(sx / n));
    m.insert(QStringLiteral("rmseY"), std::sqrt(sy / n));
    m.insert(QStringLiteral("rmseZ"), std::sqrt(sz / n));
    m.insert(QStringLiteral("rmseXY"), std::sqrt((sx + sy) / n));
    m.insert(QStringLiteral("rmse"), std::sqrt((sx + sy + sz) / n));
    return m;
}

bool isNumber(const QString &s, double *v)
{
    bool ok = false;
    *v = QString(s).replace(QLatin1Char(','), QLatin1Char('.')).toDouble(&ok);
    return ok && std::isfinite(*v);
}

// One line of a coordinate list: the text before the first number is the name, then the
// numbers. Separators are tried in order; the first giving ≥ 2 numbers wins, so decimal commas
// work with semicolons, tabs and spaces.
bool parseCoordinates(const QString &line, QString *name, QVector<double> *numbers)
{
    const QRegularExpression spaces(QStringLiteral("\\s+"));
    for (int mode = 0; mode < 4; ++mode) {
        QStringList cols;
        if (mode == 0) cols = line.split(QLatin1Char(';'));
        else if (mode == 1) cols = line.split(QLatin1Char('\t'));
        else if (mode == 2) cols = line.split(QLatin1Char(','));
        else cols = line.split(spaces, Qt::SkipEmptyParts);
        if (cols.size() < 2) continue;
        QStringList text;
        numbers->clear();
        for (QString c : cols) {
            c = c.trimmed();
            double v;
            if (numbers->isEmpty() && !isNumber(c, &v)) text << c;
            else if (isNumber(c, &v)) numbers->append(v);
            else break;
        }
        if (numbers->size() < 2) continue;
        *name = text.join(mode == 3 ? QStringLiteral(" ") : QStringLiteral(", ")).trimmed();
        return true;
    }
    return false;
}
} // namespace

struct SurveyControl::Reconstruction
{
    QString job, error;
    QDateTime modified;
    ortho::Scene scene;
    Crs crs;
    const ortho::Shot *shot(const QString &photoFile) const
    {
        const QString name = QFileInfo(photoFile).fileName();
        for (const ortho::Shot &s : scene.shots)
            if (QFileInfo(s.image).fileName() == name) return &s;
        return nullptr;
    }
};

SurveyControl::SurveyControl(QObject *parent) : QObject(parent) {}
SurveyControl::~SurveyControl() = default;

void SurveyControl::setProject(const QString &directory, const PhotoModel *photos, const MapService *map)
{
    m_project = directory.isEmpty() ? QString() : QDir(directory).absolutePath();
    m_photos = photos;
    m_map = map;
    m_reconstruction.reset();
    m_sizes.clear();
    load();
    emit changed();
}

QString SurveyControl::calibrationPath(const QString &project)
{
    return QDir(project).filePath(QStringLiteral("calibration/cameras.json"));
}

void SurveyControl::load()
{
    const Data d = m_project.isEmpty() ? Data{} : readData(m_project);
    m_crs = d.crs;
    m_points = d.points;
    m_bars = d.bars;
    m_calibration = QFileInfo::exists(calibrationPath(m_project)) ? d.calibration : QString();
    if (m_calibration.isEmpty() && !m_project.isEmpty() && QFileInfo::exists(calibrationPath(m_project)))
        m_calibration = QStringLiteral("?");
}

void SurveyControl::save()
{
    if (m_project.isEmpty()) return;
    QJsonArray points;
    for (const Point &p : m_points) {
        QJsonArray obs;
        for (auto it = p.observations.cbegin(); it != p.observations.cend(); ++it)
            obs.append(QJsonObject{{QStringLiteral("photo"), it.key()},
                                   {QStringLiteral("x"), std::round(it.value().x() * 1e7) / 1e7},
                                   {QStringLiteral("y"), std::round(it.value().y() * 1e7) / 1e7}});
        points.append(QJsonObject{{QStringLiteral("id"), p.id}, {QStringLiteral("name"), p.name}, {QStringLiteral("x"), p.x},
                                  {QStringLiteral("y"), p.y}, {QStringLiteral("z"), p.z}, {QStringLiteral("check"), p.check},
                                  {QStringLiteral("observations"), obs}});
    }
    QJsonArray bars;
    for (const ScaleBar &b : m_bars)
        bars.append(QJsonObject{{QStringLiteral("id"), b.id}, {QStringLiteral("a"), b.markerA}, {QStringLiteral("b"), b.markerB},
                                {QStringLiteral("distance"), b.distance}});
    QJsonObject root{{QStringLiteral("version"), 1}, {QStringLiteral("points"), points}, {QStringLiteral("scaleBars"), bars}};
    if (m_crs.isValid()) root.insert(QStringLiteral("crs"), m_crs.wkt());
    if (!m_calibration.isEmpty() && m_calibration != QLatin1String("?")) root.insert(QStringLiteral("calibration"), m_calibration);
    QSaveFile f(QDir(m_project).filePath(DataFile));
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(root).toJson());
        f.commit();
    }
    emit changed();
}

QString SurveyControl::newId() const
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
}

SurveyControl::Point *SurveyControl::find(const QString &id)
{
    for (Point &p : m_points)
        if (p.id == id) return &p;
    return nullptr;
}

int SurveyControl::controlCount() const
{
    int n = 0;
    for (const Point &p : m_points) n += !p.check;
    return n;
}

int SurveyControl::checkCount() const
{
    return int(m_points.size()) - controlCount();
}

QVariantList SurveyControl::pointsForQml() const
{
    QVariantList list;
    for (const Point &p : m_points) {
        QStringList photos = p.observations.keys();
        list << QVariantMap{{QStringLiteral("id"), p.id}, {QStringLiteral("name"), p.name}, {QStringLiteral("x"), p.x},
                            {QStringLiteral("y"), p.y}, {QStringLiteral("z"), p.z}, {QStringLiteral("check"), p.check},
                            {QStringLiteral("photos"), int(photos.size())}, {QStringLiteral("photoFiles"), photos}};
    }
    return list;
}

QString SurveyControl::setCrs(const QString &crsText)
{
    QString error;
    const Crs crs = Crs::fromUserInput(crsText.trimmed(), &error);
    if (!crs.isValid()) return error.isEmpty() ? QStringLiteral("Неизвестная система координат") : error;
    m_crs = crs;
    save();
    return {};
}

QString SurveyControl::addPoint(const QString &name, double x, double y, double z)
{
    if (m_project.isEmpty()) return {};
    Point p;
    p.id = newId();
    p.name = name.trimmed().isEmpty() ? QStringLiteral("Точка %1").arg(m_points.size() + 1) : name.trimmed();
    p.x = x;
    p.y = y;
    p.z = z;
    m_points << p;
    save();
    return p.id;
}

void SurveyControl::removePoint(const QString &id)
{
    for (int i = 0; i < m_points.size(); ++i)
        if (m_points[i].id == id) {
            m_points.removeAt(i);
            save();
            return;
        }
}

void SurveyControl::renamePoint(const QString &id, const QString &name)
{
    if (Point *p = find(id); p && !name.trimmed().isEmpty()) {
        p->name = name.trimmed();
        save();
    }
}

void SurveyControl::setCheck(const QString &id, bool check)
{
    if (Point *p = find(id); p && p->check != check) {
        p->check = check;
        save();
    }
}

void SurveyControl::setCoordinates(const QString &id, double x, double y, double z)
{
    if (Point *p = find(id)) {
        p->x = x;
        p->y = y;
        p->z = z;
        save();
    }
}

void SurveyControl::setObservation(const QString &id, const QString &photoFile, double nx, double ny)
{
    Point *p = find(id);
    if (!p || photoFile.isEmpty() || nx < 0 || ny < 0 || nx > 1 || ny > 1) return;
    p->observations.insert(photoFile, QPointF(nx, ny));
    save();
}

void SurveyControl::removeObservation(const QString &id, const QString &photoFile)
{
    if (Point *p = find(id); p && p->observations.remove(photoFile) > 0) save();
}

QSize SurveyControl::photoSize(const QString &photoFile) const
{
    auto it = m_sizes.constFind(photoFile);
    if (it != m_sizes.constEnd()) return it.value();
    const QSize size = displayedSize(QDir(m_project).filePath(photoFile));
    m_sizes.insert(photoFile, size);
    return size;
}

QVariantMap SurveyControl::importFile(const QUrl &url, const QString &crsText, bool northFirst)
{
    QVariantMap result;
    auto fail = [&result](const QString &e) {
        result.insert(QStringLiteral("error"), e);
        return result;
    };
    if (m_project.isEmpty()) return fail(QStringLiteral("Откройте проект"));
    QFile f(url.isLocalFile() ? url.toLocalFile() : url.toString());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return fail(QStringLiteral("Не удалось открыть %1").arg(f.fileName()));
    QStringList lines = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
    for (QString &l : lines) {
        l = l.trimmed();
        if (l.startsWith(QChar(0xFEFF))) l.remove(0, 1);
    }
    lines.removeAll(QString());
    if (lines.isEmpty()) return fail(QStringLiteral("Файл пуст"));

    Crs fileCrs;
    if (!crsText.trimmed().isEmpty()) {
        QString error;
        fileCrs = Crs::fromUserInput(crsText.trimmed(), &error);
        if (!fileCrs.isValid()) return fail(error.isEmpty() ? QStringLiteral("Неизвестная система координат") : error);
    }
    // ODM gcp_list.txt: the first line is the CRS.
    const QString first = lines.first();
    const bool gcpList = first.startsWith(QLatin1String("EPSG:"), Qt::CaseInsensitive) || first.startsWith(QLatin1String("+proj"))
                         || first.contains(QRegularExpression(QStringLiteral("^WGS84 UTM \\d+[NS]$"), QRegularExpression::CaseInsensitiveOption));
    if (gcpList) {
        const QRegularExpressionMatch m = QRegularExpression(QStringLiteral("UTM (\\d+)([NS])"), QRegularExpression::CaseInsensitiveOption).match(first);
        const Crs header = m.hasMatch() ? Crs::fromEpsg((m.captured(2).toUpper() == QLatin1String("S") ? 32700 : 32600) + m.captured(1).toInt())
                                        : Crs::fromUserInput(first);
        if (!header.isValid()) return fail(QStringLiteral("Не распознана система координат «%1»").arg(first));
        fileCrs = header;
        lines.removeFirst();
    }
    if (!fileCrs.isValid()) fileCrs = m_crs;
    if (!fileCrs.isValid()) return fail(QStringLiteral("Укажите систему координат точек (например, EPSG:32637)"));
    if (!m_crs.isValid()) m_crs = fileCrs;
    CoordinateTransform toPoints(fileCrs, m_crs);
    if (!toPoints.isValid()) return fail(toPoints.error());

    // Photos by file name and by original name (a gcp_list.txt made for the original files).
    QHash<QString, QString> photoByName;
    if (m_photos)
        for (const Photo &p : m_photos->photos()) {
            photoByName.insert(QFileInfo(p.file).fileName().toLower(), p.file);
            photoByName.insert(p.name.toLower(), p.file);
        }
    int added = 0, observations = 0;
    QStringList unknownPhotos;
    QHash<QString, int> byName;   // points of this file
    for (int i = 0; i < m_points.size(); ++i) byName.insert(m_points[i].name, i);
    int lineNo = gcpList ? 1 : 0;
    for (const QString &line : std::as_const(lines)) {
        ++lineNo;
        if (line.startsWith(QLatin1Char('#'))) continue;
        double x = 0, y = 0, z = 0;
        QString name;
        if (gcpList) {
            const QStringList cols = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            double px = 0, py = 0;
            if (cols.size() < 6 || !isNumber(cols[0], &x) || !isNumber(cols[1], &y) || !isNumber(cols[2], &z) || !isNumber(cols[3], &px)
                || !isNumber(cols[4], &py))
                continue;
            name = cols.size() > 6 ? cols[6] : QStringLiteral("%1 %2").arg(cols[0], cols[1]);
            QPointF p(x, y);
            if (!toPoints.transform(p)) continue;
            int index = byName.value(name, -1);
            if (index < 0) {
                Point point;
                point.id = newId();
                point.name = name;
                point.x = p.x();
                point.y = p.y();
                point.z = z;
                m_points << point;
                index = int(m_points.size()) - 1;
                byName.insert(name, index);
                ++added;
            }
            const QString photo = photoByName.value(cols[5].toLower());
            const QSize size = photo.isEmpty() ? QSize() : photoSize(photo);
            if (!size.isValid()) {
                if (!unknownPhotos.contains(cols[5])) unknownPhotos << cols[5];
                continue;
            }
            m_points[index].observations.insert(photo, QPointF((px + 0.5) / size.width(), (py + 0.5) / size.height()));
            ++observations;
            continue;
        }
        // name x y [z] or x y [z]; a header or a comment has no numbers.
        QVector<double> numbers;
        if (!parseCoordinates(line, &name, &numbers)) continue;
        if (name.isEmpty() && numbers.size() >= 4) name = QString::number(numbers.takeFirst(), 'g', 12);   // numeric names: 1 x y z
        if (name.isEmpty()) name = QStringLiteral("Точка %1").arg(m_points.size() + 1);
        x = numbers[0];
        y = numbers[1];
        if (northFirst) std::swap(x, y);
        z = numbers.size() > 2 ? numbers[2] : 0;
        QPointF p(x, y);
        if (!toPoints.transform(p)) continue;
        if (byName.contains(name)) {
            Point &existing = m_points[byName.value(name)];
            existing.x = p.x();
            existing.y = p.y();
            existing.z = z;
            continue;
        }
        Point point;
        point.id = newId();
        point.name = name;
        point.x = p.x();
        point.y = p.y();
        point.z = z;
        m_points << point;
        byName.insert(name, int(m_points.size()) - 1);
        ++added;
    }
    save();
    result.insert(QStringLiteral("added"), added);
    result.insert(QStringLiteral("observations"), observations);
    if (!unknownPhotos.isEmpty())
        result.insert(QStringLiteral("warning"), QStringLiteral("Снимков нет в проекте (%1): %2").arg(unknownPhotos.size())
                                                     .arg(unknownPhotos.mid(0, 5).join(QStringLiteral(", "))));
    if (added == 0 && observations == 0) result.insert(QStringLiteral("error"), QStringLiteral("В файле не найдено точек"));
    return result;
}

QString SurveyControl::exportGcpList(const QUrl &url) const
{
    Data d;
    d.crs = m_crs;
    d.points = m_points;
    int used = 0;
    QString error;
    const QString text = gcpListText(m_project, d, {}, 1, &used, nullptr, &error);
    if (!error.isEmpty()) return error;
    if (used == 0) return QStringLiteral("Нет опорных точек, отмеченных на снимках");
    QSaveFile f(url.isLocalFile() ? url.toLocalFile() : url.toString());
    if (!f.open(QIODevice::WriteOnly) || f.write(text.toUtf8()) < 0 || !f.commit())
        return QStringLiteral("Не удалось записать %1").arg(QDir::toNativeSeparators(f.fileName()));
    return {};
}

SurveyControl::JobInputs SurveyControl::prepareJob(const QString &project, const QString &jobDirectory, const QStringList &images)
{
    JobInputs r;
    const QDir job(jobDirectory);
    const QString gcpTarget = job.filePath(QStringLiteral("gcp_list.txt")), camerasTarget = job.filePath(QStringLiteral("geomap_cameras.json"));
    QFile::remove(gcpTarget);
    QFile::remove(camerasTarget);
    const Data d = readData(project);
    const bool anyControl = std::any_of(d.points.cbegin(), d.points.cend(), [](const Point &p) { return !p.check; });
    if (anyControl) {
        QSet<QString> names;
        for (const QString &file : images) names.insert(QFileInfo(file).fileName());
        const QString text = gcpListText(project, d, names, 3, &r.points, &r.dropped, &r.error);
        if (r.error.isEmpty() && r.points >= 3) {
            QSaveFile f(gcpTarget);
            if (!f.open(QIODevice::WriteOnly) || f.write(text.toUtf8()) < 0 || !f.commit())
                r.error = QStringLiteral("Не удалось записать %1").arg(QDir::toNativeSeparators(gcpTarget));
            else r.gcpUsed = true;
        }
    }
    const QString calibration = calibrationPath(project);
    if (QFileInfo::exists(calibration)) {
        if (QFile::copy(calibration, camerasTarget)) r.calibration = d.calibration.isEmpty() ? QStringLiteral("?") : d.calibration;
        else r.error = QStringLiteral("Не удалось скопировать калибровку камеры в папку задания");
    }
    return r;
}

QString SurveyControl::newestReconstructedJob() const
{
    const QDir jobs(QDir(m_project).filePath(QStringLiteral("jobs")));
    const QStringList ids = jobs.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed);
    for (const QString &id : ids)
        if (QFileInfo::exists(jobs.filePath(id + QStringLiteral("/opensfm/reconstruction.json")))
            && QFileInfo::exists(jobs.filePath(id + QStringLiteral("/odm_georeferencing/coords.txt"))))
            return id;
    return {};
}

const SurveyControl::Reconstruction *SurveyControl::reconstruction(const QString &jobId) const
{
    if (m_project.isEmpty() || jobId.isEmpty()) return nullptr;
    const QString job = QDir(m_project).filePath(QStringLiteral("jobs/") + jobId);
    const QDateTime modified = QFileInfo(QDir(job).filePath(QStringLiteral("opensfm/reconstruction.json"))).lastModified();
    if (m_reconstruction && m_reconstruction->job == jobId && m_reconstruction->modified == modified) return m_reconstruction.get();
    auto rec = std::make_shared<Reconstruction>();
    rec->job = jobId;
    rec->modified = modified;
    const QString images = QDir(job).exists(QStringLiteral("images")) ? QDir(job).filePath(QStringLiteral("images"))
                                                                      : QDir(m_project).filePath(QStringLiteral("photos"));
    if (!ortho::readOdmJob(job, &rec->scene, &rec->error, images) && images != QDir(m_project).filePath(QStringLiteral("photos"))) {
        rec->scene = {};
        rec->error.clear();
        ortho::readOdmJob(job, &rec->scene, &rec->error, QDir(m_project).filePath(QStringLiteral("photos")));
    }
    if (rec->error.isEmpty()) {
        rec->crs = jobCrs(job);
        if (!rec->crs.isValid()) rec->error = QStringLiteral("Не определена система координат результатов задания");
    }
    m_reconstruction = rec;
    return rec.get();
}

QVariantList SurveyControl::marksOnPhoto(const QString &photoFile) const
{
    QVariantList marks;
    if (m_project.isEmpty() || photoFile.isEmpty()) return marks;
    const Reconstruction *rec = reconstruction(newestReconstructedJob());
    const ortho::Shot *shot = rec && rec->error.isEmpty() ? rec->shot(photoFile) : nullptr;
    std::unique_ptr<CoordinateTransform> toJob;
    if (shot && m_crs.isValid()) toJob = std::make_unique<CoordinateTransform>(m_crs, rec->crs);
    for (const Point &p : m_points) {
        QVariantMap m{{QStringLiteral("id"), p.id}, {QStringLiteral("name"), p.name}, {QStringLiteral("check"), p.check}};
        const auto obs = p.observations.constFind(photoFile);
        if (obs != p.observations.constEnd()) {
            m.insert(QStringLiteral("nx"), obs->x());
            m.insert(QStringLiteral("ny"), obs->y());
            m.insert(QStringLiteral("placed"), true);
            marks << m;
            continue;
        }
        if (!shot) continue;
        // Where the point should be: from its other photos (the reconstruction's own frame,
        // right even when the job was not fitted to the points) or from its coordinates.
        double X[3];
        QVector<const ortho::Shot *> shots;
        QVector<QPointF> positions;
        for (auto it = p.observations.cbegin(); it != p.observations.cend(); ++it)
            if (const ortho::Shot *s = rec->shot(it.key())) {
                shots << s;
                positions << it.value();
            }
        bool known = triangulate(shots, positions, X, nullptr);
        if (!known && toJob && toJob->isValid()) {
            double x = p.x, y = p.y;
            known = toJob->transform(x, y);
            X[0] = x - rec->scene.offsetX;
            X[1] = y - rec->scene.offsetY;
            X[2] = p.z;
        }
        double px, py, depth;
        if (!known || !shot->project(X, &px, &py, &depth)) continue;
        const double nx = (px + 0.5) / shot->camera.width, ny = (py + 0.5) / shot->camera.height;
        if (nx < 0 || ny < 0 || nx > 1 || ny > 1) continue;
        m.insert(QStringLiteral("nx"), nx);
        m.insert(QStringLiteral("ny"), ny);
        m.insert(QStringLiteral("placed"), false);
        marks << m;
    }
    return marks;
}

QVariantMap SurveyControl::accuracy(const QString &jobIdIn) const
{
    QVariantMap result;
    const QString jobId = jobIdIn.isEmpty() ? newestReconstructedJob() : jobIdIn;
    if (jobId.isEmpty()) {
        result.insert(QStringLiteral("error"), QStringLiteral("Нет завершённого задания с реконструкцией"));
        return result;
    }
    result.insert(QStringLiteral("job"), jobId);
    result.insert(QStringLiteral("gcpUsed"), QFileInfo::exists(QDir(m_project).filePath(QStringLiteral("jobs/") + jobId + QStringLiteral("/gcp_list.txt"))));
    const Reconstruction *rec = reconstruction(jobId);
    if (!rec || !rec->error.isEmpty()) {
        result.insert(QStringLiteral("error"), rec ? rec->error : QStringLiteral("Задание не найдено"));
        return result;
    }
    if (!m_crs.isValid()) {
        result.insert(QStringLiteral("error"), QStringLiteral("Не задана система координат точек"));
        return result;
    }
    CoordinateTransform toJob(m_crs, rec->crs);
    if (!toJob.isValid()) {
        result.insert(QStringLiteral("error"), toJob.error());
        return result;
    }
    QVariantList rows;
    QVector<QVector<double>> control, check;
    for (const Point &p : m_points) {
        QVector<const ortho::Shot *> shots;
        QVector<QPointF> positions;
        for (auto it = p.observations.cbegin(); it != p.observations.cend(); ++it)
            if (const ortho::Shot *s = rec->shot(it.key())) {
                shots << s;
                positions << it.value();
            }
        QVariantMap row{{QStringLiteral("id"), p.id}, {QStringLiteral("name"), p.name}, {QStringLiteral("check"), p.check},
                        {QStringLiteral("photos"), int(shots.size())}};
        double X[3], reprojection = 0, x = p.x, y = p.y;
        if (shots.size() >= 2 && triangulate(shots, positions, X, &reprojection) && toJob.transform(x, y)) {
            const double dx = X[0] + rec->scene.offsetX - x, dy = X[1] + rec->scene.offsetY - y, dz = X[2] - p.z;
            row.insert(QStringLiteral("dx"), dx);
            row.insert(QStringLiteral("dy"), dy);
            row.insert(QStringLiteral("dz"), dz);
            row.insert(QStringLiteral("d"), std::sqrt(dx * dx + dy * dy + dz * dz));
            row.insert(QStringLiteral("reprojection"), reprojection);
            (p.check ? check : control) << QVector<double>{dx, dy, dz};
        }
        rows << row;
    }
    result.insert(QStringLiteral("rows"), rows);
    result.insert(QStringLiteral("control"), rmse(control));
    result.insert(QStringLiteral("check"), rmse(check));
    result.insert(QStringLiteral("crs"), rec->crs.label());
    return result;
}

QString SurveyControl::exportAccuracy(const QUrl &url, const QString &jobId) const
{
    const QVariantMap a = accuracy(jobId);
    if (!a.value(QStringLiteral("error")).toString().isEmpty()) return a.value(QStringLiteral("error")).toString();
    QString text = QString(QChar(0xFEFF)) + QStringLiteral("точка;тип;снимков;dX, м;dY, м;dZ, м;ошибка, м;на снимках, пикс\n");
    const QLocale ru(QLocale::Russian);
    auto num = [&ru](const QVariant &v, int decimals) { return v.isValid() ? ru.toString(v.toDouble(), 'f', decimals) : QString(); };
    for (const QVariant &v : a.value(QStringLiteral("rows")).toList()) {
        const QVariantMap r = v.toMap();
        text += QStringLiteral("%1;%2;%3;%4;%5;%6;%7;%8\n")
                    .arg(r.value(QStringLiteral("name")).toString(),
                         r.value(QStringLiteral("check")).toBool() ? QStringLiteral("контрольная") : QStringLiteral("опорная"),
                         QString::number(r.value(QStringLiteral("photos")).toInt()), num(r.value(QStringLiteral("dx")), 3),
                         num(r.value(QStringLiteral("dy")), 3), num(r.value(QStringLiteral("dz")), 3), num(r.value(QStringLiteral("d")), 3),
                         num(r.value(QStringLiteral("reprojection")), 2));
    }
    for (const char *kind : {"control", "check"}) {
        const QVariantMap m = a.value(QString::fromLatin1(kind)).toMap();
        if (m.value(QStringLiteral("count")).toInt() == 0) continue;
        text += QStringLiteral("СКО (%1, %2 т.);;;%3;%4;%5;%6;\n")
                    .arg(qstrcmp(kind, "control") == 0 ? QStringLiteral("опорные") : QStringLiteral("контрольные"))
                    .arg(m.value(QStringLiteral("count")).toInt())
                    .arg(num(m.value(QStringLiteral("rmseX")), 3), num(m.value(QStringLiteral("rmseY")), 3),
                         num(m.value(QStringLiteral("rmseZ")), 3), num(m.value(QStringLiteral("rmse")), 3));
    }
    QSaveFile f(url.isLocalFile() ? url.toLocalFile() : url.toString());
    if (!f.open(QIODevice::WriteOnly) || f.write(text.toUtf8()) < 0 || !f.commit())
        return QStringLiteral("Не удалось записать %1").arg(QDir::toNativeSeparators(f.fileName()));
    return {};
}

QVariantList SurveyControl::cameras() const
{
    QString calibrated;
    QFile f(calibrationPath(m_project));
    if (!m_project.isEmpty() && f.open(QIODevice::ReadOnly)) calibrated = QString::fromUtf8(f.readAll()).toLower();
    QMap<QString, QVariantMap> groups;
    if (m_photos)
        for (const Photo &p : m_photos->photos()) {
            const QString model = QStringLiteral("%1 %2").arg(p.cameraMake, p.cameraModel).simplified();
            const QString key = QStringLiteral("%1|%2x%3").arg(model).arg(p.width).arg(p.height);
            QVariantMap &g = groups[key];
            if (g.isEmpty()) {
                g.insert(QStringLiteral("model"), model.isEmpty() ? QStringLiteral("Неизвестная камера") : model);
                g.insert(QStringLiteral("width"), p.width);
                g.insert(QStringLiteral("height"), p.height);
                // OpenDroneMap names a camera "<make> <model> <width> <height> <projection> <focal>".
                const QString id = QStringLiteral("%1 %2 %3").arg(model.toLower()).arg(qMax(p.width, p.height)).arg(qMin(p.width, p.height));
                const QString id2 = QStringLiteral("%1 %2 %3").arg(model.toLower()).arg(p.width).arg(p.height);
                g.insert(QStringLiteral("calibrated"), !calibrated.isEmpty() && (calibrated.contains(id) || calibrated.contains(id2)));
            }
            g.insert(QStringLiteral("count"), g.value(QStringLiteral("count")).toInt() + 1);
        }
    QVariantList list;
    for (const QVariantMap &g : groups) list << g;
    return list;
}

QVariantList SurveyControl::jobCalibrations() const
{
    QVariantList list;
    if (m_project.isEmpty()) return list;
    const QDir jobs(QDir(m_project).filePath(QStringLiteral("jobs")));
    for (const QString &id : jobs.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed)) {
        QFile f(jobs.filePath(id + QStringLiteral("/cameras.json")));
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QJsonObject cams = QJsonDocument::fromJson(f.readAll()).object();
        if (cams.isEmpty()) continue;
        QVariantList cameras;
        for (auto it = cams.begin(); it != cams.end(); ++it) {
            const QJsonObject c = it.value().toObject();
            const double size = qMax(c.value(QStringLiteral("width")).toDouble(), c.value(QStringLiteral("height")).toDouble());
            const double focal = c.contains(QStringLiteral("focal_x")) ? c.value(QStringLiteral("focal_x")).toDouble()
                                                                       : c.value(QStringLiteral("focal")).toDouble();
            QVariantMap cam{{QStringLiteral("id"), it.key()},
                            {QStringLiteral("projection"), c.value(QStringLiteral("projection_type")).toString()},
                            {QStringLiteral("width"), c.value(QStringLiteral("width")).toInt()},
                            {QStringLiteral("height"), c.value(QStringLiteral("height")).toInt()},
                            {QStringLiteral("focalPx"), focal * size}};
            for (const char *k : {"c_x", "c_y", "k1", "k2", "k3", "p1", "p2"})
                if (c.contains(QLatin1String(k))) cam.insert(QString::fromLatin1(k), c.value(QLatin1String(k)).toDouble());
            cameras << cam;
        }
        list << QVariantMap{{QStringLiteral("job"), id},
                            {QStringLiteral("date"), QFileInfo(f).lastModified().toString(QStringLiteral("dd.MM.yyyy HH:mm"))},
                            {QStringLiteral("used"), id == m_calibration},
                            {QStringLiteral("cameras"), cameras}};
    }
    return list;
}

QString SurveyControl::useCalibration(const QString &jobId)
{
    if (m_project.isEmpty()) return QStringLiteral("Откройте проект");
    const QString source = QDir(m_project).filePath(QStringLiteral("jobs/") + jobId + QStringLiteral("/cameras.json"));
    QFile f(source);
    if (!f.open(QIODevice::ReadOnly) || QJsonDocument::fromJson(f.readAll()).object().isEmpty())
        return QStringLiteral("В задании %1 нет вычисленной калибровки (cameras.json)").arg(jobId);
    const QString target = calibrationPath(m_project);
    QDir().mkpath(QFileInfo(target).absolutePath());
    QFile::remove(target);
    if (!QFile::copy(source, target)) return QStringLiteral("Не удалось сохранить калибровку в %1").arg(QDir::toNativeSeparators(target));
    m_calibration = jobId;
    save();
    return {};
}

void SurveyControl::clearCalibration()
{
    if (m_project.isEmpty()) return;
    QFile::remove(calibrationPath(m_project));
    m_calibration.clear();
    save();
}

QVariantList SurveyControl::scaleBarsForQml() const
{
    QVariantList list;
    QHash<QString, MapService::Marker> markers;
    if (m_map)
        for (const MapService::Marker &m : m_map->projectMarkers()) markers.insert(m.id, m);
    for (const ScaleBar &b : m_bars) {
        QVariantMap m{{QStringLiteral("id"), b.id}, {QStringLiteral("distance"), b.distance}};
        const auto a = markers.constFind(b.markerA), c = markers.constFind(b.markerB);
        m.insert(QStringLiteral("a"), a != markers.constEnd() ? a->name : QStringLiteral("(удалён)"));
        m.insert(QStringLiteral("b"), c != markers.constEnd() ? c->name : QStringLiteral("(удалён)"));
        if (a != markers.constEnd() && c != markers.constEnd()) {
            double measured = Geodesy::distance(QPointF(a->lon, a->lat), QPointF(c->lon, c->lat));
            if (std::isfinite(a->z) && std::isfinite(c->z)) measured = std::hypot(measured, c->z - a->z);
            m.insert(QStringLiteral("measured"), measured);
            m.insert(QStringLiteral("error"), measured - b.distance);
        }
        list << m;
    }
    return list;
}

QString SurveyControl::addScaleBar(const QString &markerA, const QString &markerB, double distance)
{
    if (m_project.isEmpty()) return QStringLiteral("Откройте проект");
    if (markerA == markerB) return QStringLiteral("Выберите два разных маркера");
    if (!(distance > 0)) return QStringLiteral("Укажите измеренное расстояние больше нуля");
    bool a = false, b = false;
    if (m_map)
        for (const MapService::Marker &m : m_map->projectMarkers()) {
            a = a || m.id == markerA;
            b = b || m.id == markerB;
        }
    if (!a || !b) return QStringLiteral("Маркер не найден");
    m_bars << ScaleBar{newId(), markerA, markerB, distance};
    save();
    return {};
}

void SurveyControl::removeScaleBar(const QString &id)
{
    for (int i = 0; i < m_bars.size(); ++i)
        if (m_bars[i].id == id) {
            m_bars.removeAt(i);
            save();
            return;
        }
}
