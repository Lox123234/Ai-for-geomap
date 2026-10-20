#include "TextureEditService.h"
#include "AiService.h"
#include "Inpainter.h"
#include "ProcessingService.h"
#include "Scene3DService.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QThreadPool>
#include <QUrl>
#include <cmath>

namespace {
constexpr int PreviewSide = 2048;

// The model folder of a job: the full 3D model, else the 2.5D one; its OBJ name.
QString modelFolder(const QString &jobDir, QString *obj)
{
    for (const char *folder : {"odm_texturing", "odm_texturing_25d"}) {
        const QDir d(QDir(jobDir).filePath(QLatin1String(folder)));
        if (QFileInfo::exists(d.filePath(QStringLiteral("odm_textured_model_geo.obj")))) {
            if (obj) *obj = QStringLiteral("odm_textured_model_geo.obj");
            return d.path();
        }
    }
    return {};
}

// map_Kd files of an MTL, in order.
QStringList textureFiles(const QString &mtl)
{
    QStringList files;
    QFile f(mtl);
    if (!f.open(QIODevice::ReadOnly)) return files;
    for (const QByteArray &raw : f.readAll().split('\n')) {
        const QByteArray line = raw.trimmed();
        if (line.startsWith("map_Kd ")) {
            const QString name = QString::fromUtf8(line.mid(7).trimmed());
            if (!files.contains(name)) files << name;
        }
    }
    return files;
}

QString mtlOf(const QString &obj)
{
    QFile f(obj);
    if (!f.open(QIODevice::ReadOnly)) return {};
    while (!f.atEnd()) {
        const QByteArray line = f.readLine().trimmed();
        if (line.startsWith("mtllib ")) return QFileInfo(obj).dir().filePath(QString::fromUtf8(line.mid(7).trimmed()));
        if (line.startsWith("v ")) break;   // the header is over
    }
    return {};
}
} // namespace

TextureEditService::TextureEditService(ProcessingService *processing, Scene3DService *scene, ai::AiService *ai, QObject *parent)
    : QObject(parent), m_processing(processing), m_scene(scene), m_ai(ai)
{
    if (m_processing) connect(m_processing, &ProcessingService::jobsChanged, this, &TextureEditService::modelsChanged);
}

QVariantList TextureEditService::models() const
{
    QVariantList list;
    if (!m_processing) return list;
    for (const ProcessingJob &j : m_processing->jobs()) {
        if (j.state != JobState::Completed || !m_processing->jobInChunk(j)) continue;
        QString obj;
        const QString folder = modelFolder(m_processing->jobDirectory(j.id), &obj);
        if (folder.isEmpty()) continue;
        QString title = j.id;
        if (m_processing->engine())
            for (const ProcessingProfile &p : m_processing->engine()->profiles())
                if (p.id == j.profile) title = QStringLiteral("%1 · %2").arg(j.id, p.title);
        if (folder.endsWith(QLatin1String("_25d"))) title += QStringLiteral(" · 2.5D");
        list << QVariantMap{{QStringLiteral("job"), j.id}, {QStringLiteral("title"), title}};
    }
    return list;
}

QVariantList TextureEditService::atlases() const
{
    QVariantList list;
    for (int i = 0; i < m_atlases.size(); ++i)
        list << QVariantMap{{QStringLiteral("index"), i}, {QStringLiteral("name"), m_atlases[i]},
                            {QStringLiteral("width"), m_sizes.value(i).width()}, {QStringLiteral("height"), m_sizes.value(i).height()}};
    return list;
}

QString TextureEditService::copyObj() const
{
    return m_copy.isEmpty() ? QString() : QDir(m_copy).filePath(QStringLiteral("odm_textured_model_geo.obj"));
}

QString TextureEditService::atlasPath(int index) const
{
    return index >= 0 && index < m_atlases.size() ? QDir(m_copy).filePath(m_atlases[index]) : QString();
}

void TextureEditService::setBusy(bool busy, const QString &status)
{
    m_busy = busy;
    m_status = status;
    emit busyChanged();
}

QString TextureEditService::open(const QString &jobId)
{
    if (m_busy) return QStringLiteral("Подождите — идёт правка");
    if (!m_processing) return QStringLiteral("Проект не открыт");
    const QString dir = m_processing->jobDirectory(jobId);
    QString obj;
    const QString folder = modelFolder(dir, &obj);
    if (folder.isEmpty()) return QStringLiteral("В задании нет 3D-модели с текстурой");
    const QString copy = QDir(dir).filePath(QStringLiteral("geomap/texture"));
    // The working copy: made once, edits go on in it until "Вернуть исходную".
    if (!QFileInfo::exists(QDir(copy).filePath(obj))) {
        QDir().mkpath(copy);
        for (const QFileInfo &fi : QDir(folder).entryInfoList(QDir::Files)) {
            const QString suffix = fi.suffix().toLower();
            if (suffix != QLatin1String("obj") && suffix != QLatin1String("mtl") && suffix != QLatin1String("png")
                && suffix != QLatin1String("jpg") && suffix != QLatin1String("jpeg"))
                continue;
            QFile::remove(QDir(copy).filePath(fi.fileName()));
            if (!QFile::copy(fi.filePath(), QDir(copy).filePath(fi.fileName())))
                return QStringLiteral("Не удалось скопировать %1").arg(fi.fileName());
            QFile::setPermissions(QDir(copy).filePath(fi.fileName()), QFile::ReadOwner | QFile::WriteOwner);
        }
    }
    m_job = jobId;
    m_source = folder;
    m_copy = copy;
    m_undo.clear();
    {
        QMutexLocker lock(&m_cacheLock);
        m_small.clear();
    }
    loadAtlases();
    m_adjust = {};
    emit adjustChanged();
    ++m_revision;
    emit revisionChanged();
    emit atlasesChanged();
    if (m_atlases.isEmpty()) return QStringLiteral("У модели нет текстур");
    return {};
}

void TextureEditService::loadAtlases()
{
    m_atlases = textureFiles(mtlOf(copyObj()));
    m_sizes.clear();
    for (const QString &a : std::as_const(m_atlases)) m_sizes << QImageReader(QDir(m_copy).filePath(a)).size();
}

QImage TextureEditService::adjusted(const QImage &input, const Adjust &a)
{
    if (a.neutral()) return input;
    QImage img = input.convertToFormat(input.hasAlphaChannel() ? QImage::Format_ARGB32 : QImage::Format_RGB32);
    // Per channel: warmth (red up, blue down), brightness, contrast, gamma; then saturation.
    uchar lut[3][256];
    const double gain[3] = {1 + 0.12 * a.warmth, 1.0, 1 - 0.12 * a.warmth};
    for (int c = 0; c < 3; ++c)
        for (int v = 0; v < 256; ++v) {
            double x = v / 255.0 * gain[c];
            x += 0.25 * a.brightness;
            x = (x - 0.5) * (1 + a.contrast) + 0.5;
            x = std::pow(qBound(0.0, x, 1.0), 1.0 / qBound(0.2, a.gamma, 5.0));
            lut[c][v] = uchar(qBound(0, int(std::lround(x * 255)), 255));
        }
    const double s = 1 + a.saturation;
    for (int y = 0; y < img.height(); ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            const QRgb p = line[x];
            int r = lut[0][qRed(p)], g = lut[1][qGreen(p)], b = lut[2][qBlue(p)];
            if (s != 1) {
                const double l = 0.299 * r + 0.587 * g + 0.114 * b;
                r = qBound(0, int(l + (r - l) * s), 255);
                g = qBound(0, int(l + (g - l) * s), 255);
                b = qBound(0, int(l + (b - l) * s), 255);
            }
            line[x] = qRgba(r, g, b, qAlpha(p));
        }
    }
    return img;
}

QImage TextureEditService::preview(int atlas, int maxSide) const
{
    const QString path = atlasPath(atlas);
    if (path.isEmpty()) return {};
    QImage small;
    {
        QMutexLocker lock(&m_cacheLock);
        small = m_small.value(atlas);
    }
    if (small.isNull()) {
        QImageReader reader(path);
        const QSize s = reader.size();
        if (s.isValid() && qMax(s.width(), s.height()) > PreviewSide) reader.setScaledSize(s.scaled(PreviewSide, PreviewSide, Qt::KeepAspectRatio));
        small = reader.read();
        QMutexLocker lock(&m_cacheLock);
        m_small.insert(atlas, small);
    }
    QImage out = adjusted(small, m_adjust);
    if (maxSide > 0 && qMax(out.width(), out.height()) > maxSide) out = out.scaled(maxSide, maxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return out;
}

void TextureEditService::setBrightness(double v) { if (v != m_adjust.brightness) { m_adjust.brightness = qBound(-1.0, v, 1.0); emit adjustChanged(); } }
void TextureEditService::setContrast(double v) { if (v != m_adjust.contrast) { m_adjust.contrast = qBound(-1.0, v, 1.0); emit adjustChanged(); } }
void TextureEditService::setSaturation(double v) { if (v != m_adjust.saturation) { m_adjust.saturation = qBound(-1.0, v, 1.0); emit adjustChanged(); } }
void TextureEditService::setWarmth(double v) { if (v != m_adjust.warmth) { m_adjust.warmth = qBound(-1.0, v, 1.0); emit adjustChanged(); } }
void TextureEditService::setGamma(double v) { if (v != m_adjust.gamma) { m_adjust.gamma = qBound(0.5, v, 2.0); emit adjustChanged(); } }

void TextureEditService::resetAdjustments()
{
    m_adjust = {};
    emit adjustChanged();
}

QString TextureEditService::applyAdjustments()
{
    if (m_copy.isEmpty() || m_atlases.isEmpty()) return QStringLiteral("Сначала выберите модель");
    if (m_busy) return QStringLiteral("Подождите — идёт правка");
    if (m_adjust.neutral()) return QStringLiteral("Цвет не изменён");
    const Adjust a = m_adjust;
    QStringList paths;
    for (int i = 0; i < m_atlases.size(); ++i) paths << atlasPath(i);
    const QString undoDir = QDir(m_copy).filePath(QStringLiteral(".geomap-undo"));
    setBusy(true, QStringLiteral("Цвет текстуры…"));
    QPointer<TextureEditService> self(this);
    QThreadPool::globalInstance()->start([self, a, paths, undoDir] {
        QString error;
        QDir(undoDir).removeRecursively();
        QDir().mkpath(undoDir);
        QStringList saved;
        for (const QString &p : paths) {
            const QString keep = QDir(undoDir).filePath(QFileInfo(p).fileName());
            if (!QFile::copy(p, keep)) { error = QStringLiteral("Не удалось сохранить копию для отмены"); break; }
            saved << QFileInfo(p).fileName();
            const QImage out = adjusted(QImage(p), a);
            if (out.isNull() || !out.save(p)) { error = QStringLiteral("Не удалось записать %1").arg(QFileInfo(p).fileName()); break; }
        }
        QMetaObject::invokeMethod(qApp, [self, error, saved] {
            if (!self) return;
            self->m_undo = saved;
            {
                QMutexLocker lock(&self->m_cacheLock);
                self->m_small.clear();
            }
            self->m_adjust = {};
            emit self->adjustChanged();
            self->setBusy(false, {});
            ++self->m_revision;
            emit self->revisionChanged();
            emit self->finished(error.isEmpty(), error.isEmpty() ? QStringLiteral("Цвет текстуры изменён") : error);
        }, Qt::QueuedConnection);
    });
    return {};
}

QString TextureEditService::retouch(int atlas, const QVariantList &strokes, double radius)
{
    const QString path = atlasPath(atlas);
    if (path.isEmpty()) return QStringLiteral("Сначала выберите текстуру");
    if (m_busy) return QStringLiteral("Подождите — идёт правка");
    if (strokes.isEmpty()) return QStringLiteral("Закрасьте кистью то, что убрать");
    if (!m_ai) return QStringLiteral("ИИ недоступен");
    const ai::ModelCard card = m_ai->inpaintingCard();
    if (!card.isValid()) return m_ai->inpaintingProblem();
    QVector<QPointF> points;
    for (const QVariant &v : strokes) {
        const QVariantMap m = v.toMap();
        points << QPointF(m.value(QStringLiteral("x")).toDouble(), m.value(QStringLiteral("y")).toDouble());
    }
    const QString undoDir = QDir(m_copy).filePath(QStringLiteral(".geomap-undo"));
    setBusy(true, QStringLiteral("Дорисовка…"));
    QPointer<TextureEditService> self(this);
    QThreadPool::globalInstance()->start([self, card, path, points, radius, undoDir, atlas] {
        QString error;
        QImage image(path);
        if (image.isNull()) error = QStringLiteral("Текстура не читается");
        QImage mask;
        if (error.isEmpty()) {
            mask = QImage(image.size(), QImage::Format_Grayscale8);
            mask.fill(0);
            QPainter p(&mask);
            p.setRenderHint(QPainter::Antialiasing);
            QPen pen(Qt::white, qMax(2.0, radius * 2 * image.width()), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            p.setPen(pen);
            QPainterPath stroke;
            bool started = false;
            for (const QPointF &q : points) {
                if (!std::isfinite(q.x())) { started = false; continue; }   // NaN separates strokes
                const QPointF at(q.x() * image.width(), q.y() * image.height());
                if (!started) { stroke.moveTo(at); started = true; }
                stroke.lineTo(at);
                if (stroke.elementCount() == 1) stroke.lineTo(at + QPointF(0.1, 0));
            }
            p.drawPath(stroke);
        }
        if (error.isEmpty()) {
            std::shared_ptr<ai::Inpainter> inpainter = ai::cachedInpainter(card, &error);
            if (inpainter) {
                const ai::Inpainter::Result r = inpainter->inpaint(image.convertToFormat(QImage::Format_RGB888), mask);
                if (!r.ok()) error = r.error;
                else {
                    QDir(undoDir).removeRecursively();
                    QDir().mkpath(undoDir);
                    QFile::copy(path, QDir(undoDir).filePath(QFileInfo(path).fileName()));
                    QImage out = r.image;
                    if (image.hasAlphaChannel()) {   // the atlas keeps its transparency
                        out = out.convertToFormat(QImage::Format_ARGB32);
                        const QImage alpha = image.convertToFormat(QImage::Format_ARGB32);
                        for (int y = 0; y < out.height(); ++y) {
                            QRgb *o = reinterpret_cast<QRgb *>(out.scanLine(y));
                            const QRgb *s = reinterpret_cast<const QRgb *>(alpha.constScanLine(y));
                            for (int x = 0; x < out.width(); ++x) o[x] = qRgba(qRed(o[x]), qGreen(o[x]), qBlue(o[x]), qAlpha(s[x]));
                        }
                    }
                    if (!out.save(path)) error = QStringLiteral("Не удалось записать %1").arg(QFileInfo(path).fileName());
                }
            }
        }
        QMetaObject::invokeMethod(qApp, [self, error, path, atlas] {
            if (!self) return;
            if (error.isEmpty()) self->m_undo = {QFileInfo(path).fileName()};
            {
                QMutexLocker lock(&self->m_cacheLock);
                self->m_small.remove(atlas);
            }
            self->setBusy(false, {});
            ++self->m_revision;
            emit self->revisionChanged();
            emit self->finished(error.isEmpty(), error.isEmpty() ? QStringLiteral("Дорисовано ИИ — это не съёмка") : error);
        }, Qt::QueuedConnection);
    });
    return {};
}

void TextureEditService::undo()
{
    if (m_undo.isEmpty() || m_busy) return;
    const QDir undoDir(QDir(m_copy).filePath(QStringLiteral(".geomap-undo")));
    for (const QString &name : std::as_const(m_undo)) {
        QFile::remove(QDir(m_copy).filePath(name));
        QFile::copy(undoDir.filePath(name), QDir(m_copy).filePath(name));
    }
    m_undo.clear();
    {
        QMutexLocker lock(&m_cacheLock);
        m_small.clear();
    }
    ++m_revision;
    emit revisionChanged();
    emit finished(true, QStringLiteral("Отменено"));
}

QString TextureEditService::restoreOriginal()
{
    if (m_job.isEmpty()) return QStringLiteral("Сначала выберите модель");
    if (m_busy) return QStringLiteral("Подождите — идёт правка");
    const QString job = m_job;
    if (m_scene) {
        const QString layer = m_scene->layerIdForSource(copyObj());
        if (!layer.isEmpty()) m_scene->removeLayer(layer);
    }
    QDir(m_copy).removeRecursively();
    const QString error = open(job);
    if (error.isEmpty()) emit finished(true, QStringLiteral("Текстура как после обработки"));
    return error;
}

QString TextureEditService::showIn3D()
{
    if (!m_scene || m_copy.isEmpty()) return QStringLiteral("Сначала выберите модель");
    const QString layer = m_scene->layerIdForSource(copyObj());
    if (!layer.isEmpty()) m_scene->removeLayer(layer);   // loaded again with the new texture
    if (m_processing) m_processing->refreshOutputs(m_job);
    return m_scene->addModel(copyObj());
}

void TextureEditService::openFolder()
{
    if (!m_copy.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(m_copy));
}

TextureImageProvider::TextureImageProvider(TextureEditService *service)
    : QQuickImageProvider(QQuickImageProvider::Image), m_service(service)
{
}

QImage TextureImageProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{
    // "<atlas>/<revision>[/<adjust key>]"
    const int atlas = id.section(QLatin1Char('/'), 0, 0).toInt();
    const QImage img = m_service ? m_service->preview(atlas, qMax(requestedSize.width(), requestedSize.height())) : QImage();
    if (size) *size = img.size();
    return img;
}
