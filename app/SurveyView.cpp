#include "SurveyView.h"
#include "Crs.h"
#include "PhotoModel.h"
#include "ProcessingService.h"
#include "ProjectManager.h"
#include "SurveyControl.h"

#include <QCursor>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QThreadPool>
#include <QTimer>
#include <algorithm>
#include <cmath>

namespace {
const QColor Gps(235, 240, 246);          // GPS position: a white ring
const QColor FootAligned(84, 182, 247);
const QColor FootExif(150, 170, 190);
const QColor Selected(245, 207, 124);
const QColor Control(255, 122, 69);
const QColor Check(178, 132, 255);
const QColor NotAligned(239, 83, 80);

// Error in height: green (0) → yellow → red (max).
QColor errorColour(double t)
{
    t = qBound(0.0, t, 1.0);
    const QColor a(58, 208, 122), b(245, 197, 66), c(239, 83, 80);
    const QColor &p = t < 0.5 ? a : b, &q = t < 0.5 ? b : c;
    const double u = t < 0.5 ? t * 2 : (t - 0.5) * 2;
    return QColor::fromRgbF(float(p.redF() + (q.redF() - p.redF()) * u), float(p.greenF() + (q.greenF() - p.greenF()) * u),
                            float(p.blueF() + (q.blueF() - p.blueF()) * u));
}

double niceStep(double v)   // 1, 2, 5 × 10^n not above v
{
    if (!(v > 0)) return 1;
    const double base = std::pow(10.0, std::floor(std::log10(v)));
    double nice = base;
    for (double k : {1.0, 2.0, 5.0, 10.0}) if (k * base <= v) nice = k * base;
    return nice;
}

QString metres(double v, int decimals = 2)
{
    return QLocale(QLocale::Russian).toString(v, 'f', decimals);
}
} // namespace

SurveyView::SurveyView(QQuickItem *parent) : QQuickPaintedItem(parent)
{
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton | Qt::MiddleButton);
    setAcceptHoverEvents(true);
    setAntialiasing(true);
    setFillColor(QColor(15, 26, 36));
    connect(this, &SurveyView::optionsChanged, this, [this] { update(); });
    connect(this, &QQuickItem::visibleChanged, this, [this] { if (isVisible() && !m_layout && !m_busy) refresh(); });
}

SurveyView::~SurveyView()
{
    ++m_generation;   // a computation still running is dropped
}

void SurveyView::setProjects(ProjectManager *p)
{
    if (m_projects == p) return;
    if (m_projects) disconnect(m_projects, nullptr, this, nullptr);
    m_projects = p;
    if (p) {
        connect(p, &ProjectManager::projectChanged, this, [this] { m_fitted = false; m_layout.reset(); m_jobId.clear(); refresh(); });
        connect(p, &ProjectManager::photosChanged, this, [this] { refresh(); });
    }
    emit sourcesChanged();
    refresh();
}

void SurveyView::setProcessing(ProcessingService *p)
{
    if (m_processing == p) return;
    if (m_processing) disconnect(m_processing, nullptr, this, nullptr);
    m_processing = p;
    if (p) connect(p, &ProcessingService::jobsChanged, this, [this] {
        // Jobs change state often while one runs: recompute only when the list of aligned ones changes.
        QStringList before;
        for (const QVariant &v : std::as_const(m_jobs)) before << v.toMap().value(QStringLiteral("id")).toString();
        QStringList now;
        for (const ProcessingJob &j : m_processing->jobs())
            if (j.state == JobState::Completed && QFileInfo::exists(QDir(m_processing->jobDirectory(j.id)).filePath(QStringLiteral("opensfm/reconstruction.json"))))
                now << j.id;
        std::sort(before.begin(), before.end());
        std::sort(now.begin(), now.end());
        if (before != now) refresh();
    });
    emit sourcesChanged();
    refresh();
}

void SurveyView::setSurvey(SurveyControl *s)
{
    if (m_survey == s) return;
    if (m_survey) disconnect(m_survey, nullptr, this, nullptr);
    m_survey = s;
    if (s) connect(s, &SurveyControl::changed, this, [this] { updatePoints(); update(); });
    emit sourcesChanged();
    updatePoints();
}

void SurveyView::setJobId(const QString &id)
{
    if (m_jobId == id) return;
    m_jobId = id;
    emit jobIdChanged();
    refresh();
}

QString SurveyView::jobDirectoryOf(const QString &id) const
{
    return m_processing && !id.isEmpty() ? m_processing->jobDirectory(id) : QString();
}

QString SurveyView::newestAlignedJob() const
{
    return m_jobs.isEmpty() ? QString() : m_jobs.first().toMap().value(QStringLiteral("id")).toString();
}

void SurveyView::refresh()
{
    // Aligned jobs of the active block, newest first.
    m_jobs.clear();
    if (m_processing && m_projects && m_projects->hasProject()) {
        QVector<ProcessingJob> list;
        for (const ProcessingJob &j : m_processing->jobs())
            if (j.state == JobState::Completed && m_processing->jobInChunk(j)
                && QFileInfo::exists(QDir(m_processing->jobDirectory(j.id)).filePath(QStringLiteral("opensfm/reconstruction.json"))))
                list << j;
        std::sort(list.begin(), list.end(), [](const ProcessingJob &a, const ProcessingJob &b) { return a.id > b.id; });
        for (const ProcessingJob &j : list) {
            QString title = j.profile;
            if (m_processing->engine())
                for (const ProcessingProfile &p : m_processing->engine()->profiles())
                    if (p.id == j.profile) title = p.title;
            m_jobs << QVariantMap{{QStringLiteral("id"), j.id},
                                  {QStringLiteral("title"), QStringLiteral("%1 · %2").arg(j.createdAt.toLocalTime().toString(QStringLiteral("dd.MM.yyyy HH:mm")), title)}};
        }
    }
    if (!isVisible() && m_layout) {   // recomputed when shown
        m_layout.reset();
        emit layoutChanged();
        return;
    }
    if (!m_projects || !m_projects->hasProject()) {
        m_layout.reset();
        emit layoutChanged();
        update();
        return;
    }
    QVector<surveylayout::PhotoIn> photos;
    const QString project = m_projects->projectPath();
    const QVector<Photo> &all = m_projects->photoModel()->photos();
    for (int i = 0; i < all.size(); ++i) {
        const Photo &p = all[i];
        surveylayout::PhotoIn in;
        in.row = i;
        in.name = p.name;
        in.path = QDir(project).filePath(p.file);
        in.takenAt = p.takenAt;
        in.hasGps = p.hasGps;
        in.latitude = p.latitude;
        in.longitude = p.longitude;
        photos << in;
    }
    bool known = false;
    for (const QVariant &v : std::as_const(m_jobs)) known = known || v.toMap().value(QStringLiteral("id")).toString() == m_jobId;
    const QString job = jobDirectoryOf(known ? m_jobId : newestAlignedJob());
    const int generation = ++m_generation;
    m_busy = true;
    emit layoutChanged();
    QThreadPool::globalInstance()->start([this, photos, job, generation] {
        auto layout = std::make_shared<const surveylayout::Layout>(surveylayout::compute(photos, job));
        QMetaObject::invokeMethod(this, [this, layout, generation] {
            if (generation != m_generation) return;
            const bool newExtent = !m_layout || m_layout->extent != layout->extent;
            m_layout = layout;
            m_busy = false;
            if (!m_fitted || newExtent) fitAll();
            updatePoints();
            emit layoutChanged();
            emit optionsChanged();   // the automatic error scale follows the layout
            update();
        }, Qt::QueuedConnection);
    });
}

QString SurveyView::status() const
{
    if (!m_projects || !m_projects->hasProject()) return QStringLiteral("Проект не открыт");
    if (m_busy && !m_layout) return QStringLiteral("Построение схемы…");
    return m_layout ? m_layout->error : QString();
}

QVariantMap SurveyView::summary() const
{
    QVariantMap m;
    if (!m_layout || !m_layout->ok()) return m;
    const surveylayout::Summary &s = m_layout->summary;
    m.insert(QStringLiteral("photos"), s.photos);
    m.insert(QStringLiteral("withGps"), s.withGps);
    m.insert(QStringLiteral("aligned"), s.aligned);
    m.insert(QStringLiteral("notAligned"), s.notAligned);
    m.insert(QStringLiteral("compared"), s.compared);
    m.insert(QStringLiteral("job"), m_layout->job);
    m.insert(QStringLiteral("crs"), m_layout->crsLabel);
    m.insert(QStringLiteral("background"), m_layout->backgroundName);
    const std::pair<const char *, double> rmse[] = {{"rmseX", s.rmseX}, {"rmseY", s.rmseY}, {"rmseZ", s.rmseZ}, {"rmseXY", s.rmseXY}, {"rmse", s.rmse}};
    for (const auto &[key, v] : rmse)
        if (std::isfinite(v)) m.insert(QString::fromLatin1(key), v);
    return m;
}

QVariantList SurveyView::cameraRows() const
{
    QVariantList rows;
    if (!m_layout) return rows;
    QVector<const surveylayout::Camera *> list;
    for (const surveylayout::Camera &c : m_layout->cameras) list << &c;
    // Worst first: not aligned, then the largest error; the rest by name.
    std::stable_sort(list.begin(), list.end(), [this](const surveylayout::Camera *a, const surveylayout::Camera *b) {
        const bool na = !m_layout->job.isEmpty() && !a->aligned, nb = !m_layout->job.isEmpty() && !b->aligned;
        if (na != nb) return na;
        const double ea = a->hasError() ? std::hypot(a->dx(), a->dy()) : -1, eb = b->hasError() ? std::hypot(b->dx(), b->dy()) : -1;
        if (ea != eb) return ea > eb;
        return QString::compare(a->name, b->name, Qt::CaseInsensitive) < 0;
    });
    for (const surveylayout::Camera *c : list) {
        QVariantMap r{{QStringLiteral("row"), c->row}, {QStringLiteral("name"), c->name}, {QStringLiteral("gps"), c->hasGps},
                      {QStringLiteral("aligned"), c->aligned}, {QStringLiteral("alignedKnown"), !m_layout->job.isEmpty()}};
        if (c->hasError()) {
            r.insert(QStringLiteral("dxy"), std::hypot(c->dx(), c->dy()));
            if (std::isfinite(c->dz())) r.insert(QStringLiteral("dz"), c->dz());
        }
        rows << r;
    }
    return rows;
}

void SurveyView::updatePoints()
{
    m_points.clear();
    m_pointRows.clear();
    m_pointSummary.clear();
    if (m_survey && m_layout && m_layout->ok() && m_survey->crs().isValid() && !m_survey->controlPoints().isEmpty()) {
        const Crs layoutCrs = Crs::fromWkt(m_layout->crsWkt);
        CoordinateTransform toLayout(m_survey->crs(), layoutCrs);
        QHash<QString, QVariantMap> errors;
        if (!m_layout->job.isEmpty()) {
            const QVariantMap a = m_survey->accuracy(m_layout->job);
            for (const QVariant &v : a.value(QStringLiteral("rows")).toList()) errors.insert(v.toMap().value(QStringLiteral("id")).toString(), v.toMap());
            for (const char *kind : {"control", "check"}) {
                const QVariantMap s = a.value(QString::fromLatin1(kind)).toMap();
                if (s.value(QStringLiteral("count")).toInt() > 0) m_pointSummary.insert(QString::fromLatin1(kind), s);
            }
        }
        for (const SurveyControl::Point &p : m_survey->controlPoints()) {
            Point q;
            q.id = p.id;
            q.name = p.name;
            q.check = p.check;
            q.photos = int(p.observations.size());
            double x = p.x, y = p.y;
            if (!toLayout.isValid() || !toLayout.transform(x, y)) continue;
            q.x = x;
            q.y = y;
            const QVariantMap e = errors.value(p.id);
            if (e.contains(QStringLiteral("dx"))) {
                q.solved = true;
                q.dx = e.value(QStringLiteral("dx")).toDouble();
                q.dy = e.value(QStringLiteral("dy")).toDouble();
                q.dz = e.value(QStringLiteral("dz")).toDouble();
            }
            m_points << q;
            QVariantMap row{{QStringLiteral("id"), q.id}, {QStringLiteral("name"), q.name}, {QStringLiteral("check"), q.check},
                            {QStringLiteral("photos"), q.photos}};
            if (q.solved) {
                row.insert(QStringLiteral("dxy"), std::hypot(q.dx, q.dy));
                row.insert(QStringLiteral("dz"), q.dz);
            }
            m_pointRows << row;
        }
    }
    emit pointsChanged();
}

double SurveyView::effectiveErrorScale() const
{
    if (m_errorScale > 0) return m_errorScale;
    if (!m_layout) return 1;
    // Automatic: the median plan error drawn as 4 % of the survey's size.
    std::vector<double> e;
    for (const surveylayout::Camera &c : m_layout->cameras)
        if (c.hasError()) e.push_back(std::hypot(c.dx(), c.dy()));
    if (e.empty()) return 1;
    std::nth_element(e.begin(), e.begin() + e.size() / 2, e.end());
    const double med = qMax(1e-3, e[e.size() / 2]);
    const double size = qMin(m_layout->extent.width(), m_layout->extent.height());
    return qMax(1.0, niceStep(0.04 * size / med));
}

QPointF SurveyView::toScreen(double x, double y) const
{
    return QPointF(width() / 2 + (x - m_centre.x()) / m_resolution, height() / 2 - (y - m_centre.y()) / m_resolution);
}

QPointF SurveyView::toMap(const QPointF &s) const
{
    return QPointF(m_centre.x() + (s.x() - width() / 2) * m_resolution, m_centre.y() - (s.y() - height() / 2) * m_resolution);
}

void SurveyView::fitAll()
{
    if (!m_layout || m_layout->extent.isNull() || width() < 10 || height() < 10) return;
    QRectF e = m_layout->extent;
    if (e.width() < 1 && e.height() < 1) e.adjust(-50, -50, 50, 50);
    m_centre = e.center();
    m_resolution = qMax(e.width() / qMax(1.0, width() - 60), e.height() / qMax(1.0, height() - 60));
    if (!(m_resolution > 0)) m_resolution = 1;
    m_fitted = true;
    m_autoFit = true;   // follows the size of the view until the user moves it
    updateScaleText();
    update();
}

void SurveyView::zoomToRow(int row)
{
    if (!m_layout) return;
    for (const surveylayout::Camera &c : m_layout->cameras)
        if (c.row == row) {
            m_centre = c.aligned ? QPointF(c.x, c.y) : QPointF(c.gpsX, c.gpsY);
            m_autoFit = false;
            if (!c.footprint.isEmpty()) {
                const QRectF r = c.footprint.boundingRect();
                m_resolution = qMax(r.width() / qMax(1.0, width() * 0.6), r.height() / qMax(1.0, height() * 0.6));
            }
            updateScaleText();
            update();
            return;
        }
}

void SurveyView::updateScaleText()
{
    const QString text = m_layout ? QStringLiteral("%1 м").arg(metres(niceStep(120 * m_resolution), niceStep(120 * m_resolution) < 1 ? 2 : 0)) : QString();
    if (text != m_scaleText) {
        m_scaleText = text;
        emit viewChanged();
    }
}

void SurveyView::setSelectedRow(int row)
{
    if (row == m_selectedRow) return;
    m_selectedRow = row;
    emit selectedRowChanged();
    update();
}

QPointF SurveyView::screenPositionOf(int row) const
{
    if (m_layout)
        for (const surveylayout::Camera &c : m_layout->cameras)
            if (c.row == row) {
                if (c.aligned) return toScreen(c.x, c.y);
                if (c.hasGps) return toScreen(c.gpsX, c.gpsY);
            }
    return QPointF(-1, -1);
}

int SurveyView::cameraAt(const QPointF &screen) const
{
    if (!m_layout) return -1;
    int best = -1;
    double bestD = 12;
    for (int i = 0; i < m_layout->cameras.size(); ++i) {
        const surveylayout::Camera &c = m_layout->cameras[i];
        for (int k = 0; k < 2; ++k) {
            if (k == 0 && !c.aligned) continue;
            if (k == 1 && !c.hasGps) continue;
            const QPointF s = k == 0 ? toScreen(c.x, c.y) : toScreen(c.gpsX, c.gpsY);
            const double d = std::hypot(s.x() - screen.x(), s.y() - screen.y());
            if (d < bestD) { bestD = d; best = i; }
        }
    }
    return best;
}

QString SurveyView::placePointAt(const QPointF &screen)
{
    if (!m_survey || !m_layout || !m_layout->ok()) return {};
    if (m_layout->job.isEmpty()) {
        emit message(QStringLiteral("Опорную точку по схеме можно поставить после выравнивания снимков (нужна модель высот). "
                                    "Точку с известными координатами добавьте в «Проект → Управление GCP…»."));
        return {};
    }
    const QPointF p = toMap(screen);
    double z = surveylayout::heightAt(jobDirectoryOf(m_layout->job), p.x(), p.y());
    const bool fromModel = std::isfinite(z);
    if (!fromModel) z = m_layout->groundZ;
    if (!std::isfinite(z)) {
        emit message(QStringLiteral("В этом месте нет модели высот задания: поставьте точку на снятом участке."));
        return {};
    }
    // The points' CRS: the job's, if the project has none yet.
    if (!m_survey->crs().isValid()) {
        const QString e = m_survey->setCrs(m_layout->crsWkt);
        if (!e.isEmpty()) {
            emit message(e);
            return {};
        }
    }
    double x = p.x(), y = p.y();
    CoordinateTransform toSurvey(Crs::fromWkt(m_layout->crsWkt), m_survey->crs());
    if (!toSurvey.isValid() || !toSurvey.transform(x, y)) {
        emit message(QStringLiteral("Не удалось пересчитать точку в систему координат опорных точек"));
        return {};
    }
    QSet<QString> names;
    for (const SurveyControl::Point &q : m_survey->controlPoints()) names.insert(q.name);
    int n = int(m_survey->controlPoints().size()) + 1;
    while (names.contains(QStringLiteral("ОП-%1").arg(n))) ++n;
    const QString name = QStringLiteral("ОП-%1").arg(n);
    const QString id = m_survey->addPoint(name, x, y, z);
    if (!id.isEmpty()) emit pointPlaced(id, name, fromModel);
    return id;
}

void SurveyView::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    if (!m_fitted || m_autoFit || oldGeometry.width() < 10) fitAll();
    update();
}

void SurveyView::mousePressEvent(QMouseEvent *event)
{
    m_pressPos = m_lastPos = event->position();
    m_dragged = false;
    forceActiveFocus();
    event->accept();
}

void SurveyView::mouseMoveEvent(QMouseEvent *event)
{
    const QPointF d = event->position() - m_lastPos;
    if (!m_dragged && std::hypot(event->position().x() - m_pressPos.x(), event->position().y() - m_pressPos.y()) > 3) m_dragged = true;
    if (m_dragged) {
        m_autoFit = false;
        m_centre -= QPointF(d.x() * m_resolution, -d.y() * m_resolution);
        update();
    }
    m_lastPos = event->position();
}

void SurveyView::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_dragged || event->button() != Qt::LeftButton) return;
    if (m_placing) {
        placePointAt(event->position());
        return;
    }
    const int i = cameraAt(event->position());
    if (i >= 0 && m_projects) m_projects->selectPhoto(m_layout->cameras[i].row);
}

void SurveyView::mouseDoubleClickEvent(QMouseEvent *event)
{
    const int i = m_placing ? -1 : cameraAt(event->position());
    if (i >= 0) emit photoActivated(m_layout->cameras[i].row);
}

void SurveyView::hoverMoveEvent(QHoverEvent *event)
{
    const int i = cameraAt(event->position());
    setCursor(QCursor(m_placing ? Qt::CrossCursor : i >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor));
    if (i != m_hover) {
        m_hover = i;
        update();
    }
}

void SurveyView::wheelEvent(QWheelEvent *event)
{
    const QPointF at = event->position(), before = toMap(at);
    const double f = std::pow(1.0015, -event->angleDelta().y());
    m_resolution = qBound(1e-4, m_resolution * f, 1e5);
    m_autoFit = false;
    const QPointF after = toMap(at);
    m_centre += before - after;
    updateScaleText();
    update();
    event->accept();
}

void SurveyView::paint(QPainter *p)
{
    p->setRenderHint(QPainter::Antialiasing);
    if (!m_layout || !m_layout->ok()) return;
    const surveylayout::Layout &L = *m_layout;
    const QRectF view(0, 0, width(), height());

    // --- Background: the orthophoto, else a grid.
    if (m_showBackground && !L.background.isNull()) {
        const QRectF r = L.backgroundRect;
        const QPointF a = toScreen(r.left(), r.bottom()), b = toScreen(r.right(), r.top());
        p->setOpacity(0.85);
        p->drawImage(QRectF(a, b), L.background);
        p->setOpacity(1);
    } else {
        const double step = niceStep(100 * m_resolution);
        QPen grid(QColor(36, 56, 72), 1);
        p->setPen(grid);
        const QPointF tl = toMap(QPointF(0, 0)), br = toMap(QPointF(width(), height()));
        for (double x = std::floor(tl.x() / step) * step; x <= br.x(); x += step) p->drawLine(toScreen(x, tl.y()), toScreen(x, br.y()));
        for (double y = std::floor(br.y() / step) * step; y <= tl.y(); y += step) p->drawLine(toScreen(tl.x(), y), toScreen(br.x(), y));
    }

    // --- Footprints.
    auto screenPoly = [this](const QPolygonF &poly) {
        QPolygonF s;
        for (const QPointF &q : poly) s << toScreen(q.x(), q.y());
        return s;
    };
    if (m_showFootprints)
        for (const surveylayout::Camera &c : L.cameras) {
            if (c.footprint.isEmpty() || c.row == m_selectedRow) continue;
            const QColor base = c.footprintAligned ? FootAligned : FootExif;
            QColor fill = base;
            fill.setAlpha(14);
            QColor line = base;
            line.setAlpha(c.row == (m_hover >= 0 ? L.cameras[m_hover].row : -2) ? 230 : 95);
            QPen pen(line, 1);
            if (!c.footprintAligned) pen.setStyle(Qt::DashLine);
            p->setPen(pen);
            p->setBrush(fill);
            p->drawPolygon(screenPoly(c.footprint));
        }

    // --- Flight path in capture order, with an arrow on every leg.
    if (m_showPath) {
        QPolygonF path;
        for (const surveylayout::Camera &c : L.cameras)
            if (c.aligned) path << toScreen(c.x, c.y);
            else if (c.hasGps) path << toScreen(c.gpsX, c.gpsY);
        p->setPen(QPen(QColor(255, 255, 255, 110), 1.4));
        p->setBrush(Qt::NoBrush);
        p->drawPolyline(path);
        p->setBrush(QColor(255, 255, 255, 150));
        p->setPen(Qt::NoPen);
        for (int i = 1; i < path.size(); ++i) {
            const QPointF a = path[i - 1], b = path[i], d = b - a;
            const double len = std::hypot(d.x(), d.y());
            if (len < 28) continue;
            const QPointF u = d / len, n(-u.y(), u.x()), m = (a + b) / 2;
            p->drawPolygon(QPolygonF{m + u * 5, m - u * 4 + n * 4, m - u * 4 - n * 4});
        }
    }

    // --- Cameras: GPS ring, error vector (exaggerated), estimated dot coloured by the error in height.
    const double k = effectiveErrorScale();
    double maxDz = 0.5;
    for (const surveylayout::Camera &c : L.cameras)
        if (std::isfinite(c.dz())) maxDz = qMax(maxDz, std::fabs(c.dz()));
    const bool alignedJob = !L.job.isEmpty();
    for (int i = 0; i < L.cameras.size(); ++i) {
        const surveylayout::Camera &c = L.cameras[i];
        const QPointF g = toScreen(c.gpsX, c.gpsY);
        if (c.hasError()) {
            const QPointF e = toScreen(c.gpsX + c.dx() * k, c.gpsY + c.dy() * k);
            p->setPen(QPen(QColor(255, 255, 255, 200), 1.3));
            p->drawLine(g, e);
            p->setPen(QPen(QColor(10, 18, 26), 1));
            p->setBrush(std::isfinite(c.dz()) ? errorColour(std::fabs(c.dz()) / maxDz) : QColor(84, 182, 247));
            p->drawEllipse(e, 4.2, 4.2);
            p->setBrush(Qt::NoBrush);
            p->setPen(QPen(Gps, 1.4));
            p->drawEllipse(g, 3.5, 3.5);
        } else if (alignedJob && !c.aligned && c.hasGps) {
            p->setPen(QPen(NotAligned, 2));
            p->drawLine(g + QPointF(-5, -5), g + QPointF(5, 5));
            p->drawLine(g + QPointF(-5, 5), g + QPointF(5, -5));
        } else if (c.aligned) {
            p->setPen(QPen(QColor(10, 18, 26), 1));
            p->setBrush(FootAligned);
            p->drawEllipse(toScreen(c.x, c.y), 4, 4);
        } else if (c.hasGps) {
            p->setPen(QPen(QColor(10, 18, 26), 1));
            p->setBrush(QColor(86, 182, 247));
            p->drawEllipse(g, 4, 4);
        }
    }

    // --- Control (orange) and check (violet) points: a triangle at the surveyed position, the
    // error to the triangulated one; hollow until marked on ≥ 2 photos of the job.
    for (const Point &q : std::as_const(m_points)) {
        const QPointF s = toScreen(q.x, q.y);
        const QColor col = q.check ? Check : Control;
        if (q.solved) {
            p->setPen(QPen(col, 1.5));
            p->drawLine(s, toScreen(q.x + q.dx * k, q.y + q.dy * k));
        }
        const QPolygonF tri{s + QPointF(0, -8), s + QPointF(7, 5), s + QPointF(-7, 5)};
        p->setPen(QPen(QColor(10, 18, 26), 1.2));
        p->setBrush(q.solved || q.photos > 0 ? col : QColor(col.red(), col.green(), col.blue(), 70));
        p->drawPolygon(tri);
        QFont f = p->font();
        f.setPixelSize(11);
        f.setBold(true);
        p->setFont(f);
        const QRectF label(s.x() + 9, s.y() - 18, 120, 16);
        p->setPen(QColor(0, 0, 0, 170));
        p->drawText(label.translated(1, 1), Qt::AlignLeft | Qt::AlignVCenter, q.name);
        p->setPen(col.lighter(130));
        p->drawText(label, Qt::AlignLeft | Qt::AlignVCenter, q.name);
    }

    // --- The selected photo (and the one under the mouse): footprint and name.
    auto highlight = [&](const surveylayout::Camera &c, bool selected) {
        if (!c.footprint.isEmpty() && selected) {
            QColor fill = Selected;
            fill.setAlpha(40);
            p->setPen(QPen(Selected, 2));
            p->setBrush(fill);
            p->drawPolygon(screenPoly(c.footprint));
        }
        const QPointF at = c.aligned ? toScreen(c.x, c.y) : toScreen(c.gpsX, c.gpsY);
        p->setPen(QPen(selected ? Selected : QColor(255, 255, 255), 2));
        p->setBrush(Qt::NoBrush);
        p->drawEllipse(at, 7, 7);
        QFont f = p->font();
        f.setPixelSize(12);
        f.setBold(true);
        p->setFont(f);
        QString text = c.name;
        if (c.hasError())
            text += QStringLiteral("  ·  в плане %1 м%2").arg(metres(std::hypot(c.dx(), c.dy())),
                                                               std::isfinite(c.dz()) ? QStringLiteral(", по высоте %1 м").arg(metres(c.dz())) : QString());
        else if (alignedJob && !c.aligned) text += QStringLiteral("  ·  не выровнен");
        const QFontMetricsF fm(f);
        QRectF box(at.x() + 11, at.y() - 26, fm.horizontalAdvance(text) + 14, 22);
        if (box.right() > width() - 4) box.moveRight(at.x() - 11);
        p->setPen(Qt::NoPen);
        p->setBrush(QColor(17, 27, 38, 225));
        p->drawRoundedRect(box, 5, 5);
        p->setPen(selected ? Selected : QColor(230, 238, 246));
        p->drawText(box, Qt::AlignCenter, text);
    };
    if (m_showLabels) {
        QFont f = p->font();
        f.setPixelSize(10);
        p->setFont(f);
        p->setPen(QColor(220, 230, 240, 200));
        for (const surveylayout::Camera &c : L.cameras) {
            const QPointF at = c.aligned ? toScreen(c.x, c.y) : toScreen(c.gpsX, c.gpsY);
            if (view.contains(at)) p->drawText(at + QPointF(7, -6), QFileInfo(c.name).completeBaseName());
        }
    }
    for (const surveylayout::Camera &c : L.cameras)
        if (c.row == m_selectedRow) highlight(c, true);
    if (m_hover >= 0 && m_hover < L.cameras.size() && L.cameras[m_hover].row != m_selectedRow) highlight(L.cameras[m_hover], false);

    // --- North arrow and scale bar.
    const QPointF n(width() - 28, 34);
    p->setPen(Qt::NoPen);
    p->setBrush(QColor(230, 238, 246));
    p->drawPolygon(QPolygonF{n + QPointF(0, -14), n + QPointF(7, 6), n + QPointF(0, 2), n + QPointF(-7, 6)});
    QFont f = p->font();
    f.setPixelSize(11);
    f.setBold(true);
    p->setFont(f);
    p->setPen(QColor(230, 238, 246));
    p->drawText(QRectF(n.x() - 10, n.y() + 6, 20, 16), Qt::AlignCenter, QStringLiteral("С"));
    const double barM = niceStep(120 * m_resolution), barPx = barM / m_resolution;
    const QPointF b0(width() - 24 - barPx, height() - 22);
    p->setPen(QPen(QColor(230, 238, 246), 2));
    p->drawLine(b0, b0 + QPointF(barPx, 0));
    p->drawLine(b0 + QPointF(0, -5), b0 + QPointF(0, 3));
    p->drawLine(b0 + QPointF(barPx, -5), b0 + QPointF(barPx, 3));
    f.setBold(false);
    p->setFont(f);
    p->drawText(QRectF(b0.x(), b0.y() - 20, barPx, 16), Qt::AlignCenter, m_scaleText);
}
