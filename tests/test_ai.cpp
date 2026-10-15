#include <QtTest>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QRandomGenerator>
#include <QSettings>
#include <QTemporaryDir>
#include <cmath>
#include <gdal.h>
#include <ogr_srs_api.h>
#include "AiService.h"
#include "GisEnvironment.h"
#include "Inpainter.h"
#include "MapService.h"
#include "RasterEdit.h"
#include "MaskPolygons.h"
#include "ModelCard.h"
#include "Platform.h"
#include "SamSegmenter.h"
#include "VectorSource.h"

namespace {
bool writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}

QByteArray card(const QString &license, const QString &fileName, const QByteArray &sha, const QString &task = QStringLiteral("classification"))
{
    const QJsonObject file{{"role", "weights"}, {"name", fileName}, {"sha256", QString::fromLatin1(sha)}};
    const QJsonObject o{{"schemaVersion", 1}, {"name", QStringLiteral("Тест")}, {"task", task}, {"license", license},
                        {"source", "https://example.org/model"}, {"files", QJsonArray{file}}};
    return QJsonDocument(o).toJson();
}

// An aerial-like scene: a mottled field, a road, a red-roofed building and a blue pond.
QImage scene(const QSize &size, const QRect &roof, const QPoint &pondCentre, int pondRadius)
{
    QImage image(size, QImage::Format_RGB32);
    QRandomGenerator rng(42);
    for (int y = 0; y < size.height(); ++y)
        for (int x = 0; x < size.width(); ++x) {
            const int n = int(rng.bounded(30)) - 15;
            const int patch = ((x / 37 + y / 29) % 2) * 12;
            image.setPixel(x, y, qRgb(qBound(0, 92 + n + patch, 255), qBound(0, 128 + n + patch, 255), qBound(0, 70 + n, 255)));
        }
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(150, 146, 140));
    p.drawRect(QRect(0, size.height() * 3 / 4, size.width(), size.height() / 14));   // road
    p.setBrush(QColor(196, 52, 40));
    p.drawRect(roof);
    p.setBrush(QColor(170, 40, 32));   // shaded half of the roof
    p.drawRect(QRect(roof.left(), roof.top(), roof.width(), roof.height() / 2));
    p.setBrush(QColor(40, 80, 170));
    p.drawEllipse(pondCentre, pondRadius, pondRadius);
    return image;
}

double iou(const QImage &mask, const std::function<bool(int, int)> &truth)
{
    qint64 both = 0, any = 0;
    for (int y = 0; y < mask.height(); ++y) {
        const uchar *line = mask.constScanLine(y);
        for (int x = 0; x < mask.width(); ++x) {
            const bool m = line[x] > 0, t = truth(x, y);
            both += m && t;
            any += m || t;
        }
    }
    return any ? double(both) / any : 0;
}

bool makeOrthophoto(const QString &path, const QImage &image, const double gt[6])
{
    GDALDatasetH ds = GDALCreate(GDALGetDriverByName("GTiff"), QDir::toNativeSeparators(path).toUtf8().constData(),
                                 image.width(), image.height(), 3, GDT_Byte, nullptr);
    if (!ds) return false;
    double transform[6];
    std::copy(gt, gt + 6, transform);
    GDALSetGeoTransform(ds, transform);
    OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
    OSRImportFromEPSG(srs, 32615);
    GDALSetSpatialRef(ds, srs);
    OSRDestroySpatialReference(srs);
    QVector<uchar> line(image.width());
    bool ok = true;
    for (int b = 1; b <= 3 && ok; ++b)
        for (int y = 0; y < image.height() && ok; ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const QRgb c = image.pixel(x, y);
                line[x] = uchar(b == 1 ? qRed(c) : b == 2 ? qGreen(c) : qBlue(c));
            }
            ok = GDALRasterIO(GDALGetRasterBand(ds, b), GF_Write, 0, y, image.width(), 1, line.data(), image.width(), 1, GDT_Byte, 0, 0) == CE_None;
        }
    GDALClose(ds);
    return ok;
}
} // namespace

class AiTest : public QObject
{
    Q_OBJECT
    QTemporaryDir m_settings;
    QTemporaryDir m_dir;

    ai::ModelCard segmentationModel()
    {
        ai::AiService service;
        return service.segmentationCard();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_settings.isValid() && m_dir.isValid());
        QCoreApplication::setOrganizationName(QStringLiteral("GeoMapAI-Tests"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settings.path());
        Platform::setDataDirectory(m_dir.filePath(QStringLiteral("data")));
        QVERIFY2(GisEnvironment::initialize(QCoreApplication::applicationDirPath()), qPrintable(GisEnvironment::problems().join('\n')));
        qInfo("%s", qPrintable(ai::runtimeVersion()));
    }

    // Cards: what is required, checksums, file names that would leave the folder.
    void modelCards()
    {
        const QString root = m_dir.filePath(QStringLiteral("cards"));
        const QByteArray weights("not really a model");
        const QByteArray sha = QCryptographicHash::hash(weights, QCryptographicHash::Sha256).toHex();
        QVERIFY(writeFile(root + QStringLiteral("/good/w.bin"), weights));
        QVERIFY(writeFile(root + QStringLiteral("/good/model.json"), card(QStringLiteral("MIT"), QStringLiteral("w.bin"), sha)));
        QVERIFY(writeFile(root + QStringLiteral("/nolicense/model.json"), card(QString(), QStringLiteral("w.bin"), sha)));
        QVERIFY(writeFile(root + QStringLiteral("/escape/model.json"), card(QStringLiteral("MIT"), QStringLiteral("../good/w.bin"), sha)));
        QVERIFY(writeFile(root + QStringLiteral("/broken/model.json"), "{ not json"));

        const ai::ModelCard good = ai::readCard(root + QStringLiteral("/good"));
        QVERIFY2(good.isValid(), qPrintable(good.error));
        QCOMPARE(good.id, QStringLiteral("good"));   // the folder name when the card has no id
        QCOMPARE(good.license, QStringLiteral("MIT"));
        QCOMPARE(good.filePath(QStringLiteral("weights")), QDir(root + QStringLiteral("/good")).filePath(QStringLiteral("w.bin")));
        QString error;
        QVERIFY2(ai::verifyFiles(good, &error), qPrintable(error));
        QVERIFY(writeFile(root + QStringLiteral("/good/w.bin"), weights + "!"));   // replaced weights
        QVERIFY(!ai::verifyFiles(good, &error));
        QVERIFY2(error.contains(QStringLiteral("SHA-256")), qPrintable(error));

        QVERIFY(ai::readCard(root + QStringLiteral("/nolicense")).error.contains(QStringLiteral("лицензия")));
        QVERIFY(ai::readCard(root + QStringLiteral("/escape")).error.contains(QStringLiteral("Недопустимое имя")));
        QVERIFY(!ai::readCard(root + QStringLiteral("/broken")).isValid());
        QVERIFY(!ai::readCard(root + QStringLiteral("/missing")).isValid());
        QCOMPARE(ai::discoverModels({root}).size(), 4);   // invalid cards are listed with their error

        // The shipped card of MobileSAM is complete; the service lists it only with its weights.
        const ai::ModelCard sam = ai::readCard(QStringLiteral(GEOMAP_SOURCE_DIR "/models/mobilesam"));
        QVERIFY2(sam.isValid(), qPrintable(sam.error));
        QCOMPARE(sam.license, QStringLiteral("Apache-2.0"));
        QCOMPARE(sam.task, QStringLiteral("interactive-segmentation"));
        QVERIFY(!sam.filePath(QStringLiteral("encoder")).isEmpty() && !sam.filePath(QStringLiteral("decoder")).isEmpty());
        QVERIFY(QFileInfo::exists(QStringLiteral(GEOMAP_SOURCE_DIR "/models/mobilesam/LICENSE")));

        // Users' models: a card of another task is listed but not used for the click selection.
        qputenv("GEOMAP_MODELS_DIR", QDir::toNativeSeparators(root).toUtf8());
        ai::AiService service;
        bool listed = false;
        for (const QVariant &v : service.modelsForQml()) {
            const QVariantMap m = v.toMap();
            if (m.value(QStringLiteral("id")) == QLatin1String("good")) {
                listed = true;
                QCOMPARE(m.value(QStringLiteral("supported")).toBool(), false);
                QCOMPARE(m.value(QStringLiteral("chosen")).toBool(), false);
            }
        }
        QVERIFY(listed);
        qunsetenv("GEOMAP_MODELS_DIR");
    }

    // Masks to polygons: rings along pixel edges, holes, separate parts, simplification.
    void maskToPolygons()
    {
        QImage mask(100, 80, QImage::Format_Grayscale8);
        mask.fill(0);
        QPainter p(&mask);
        p.fillRect(QRect(10, 20, 50, 30), Qt::white);
        p.fillRect(QRect(30, 30, 10, 10), Qt::black);   // hole
        p.fillRect(QRect(80, 70, 2, 2), Qt::white);     // speck
        p.end();
        QString error;
        const QVector<ai::MaskPolygon> polygons = ai::maskPolygons(mask, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(polygons.size(), 2);
        const ai::MaskPolygon &big = polygons[0].area > polygons[1].area ? polygons[0] : polygons[1];
        QCOMPARE(big.area, 1400.0);
        QCOMPARE(big.outer.boundingRect(), QRectF(10, 20, 50, 30));
        QCOMPARE(big.holes.size(), 1);
        QVERIFY(big.contains(QPointF(15, 25)));
        QVERIFY(!big.contains(QPointF(35, 35)));   // in the hole
        QVERIFY(!big.contains(QPointF(70, 25)));
        QCOMPARE(ai::simplifyRing(big.outer, 0.75).size(), 4);

        // A disc: the staircase disappears, the area stays.
        QImage disc(200, 200, QImage::Format_Grayscale8);
        disc.fill(0);
        QPainter dp(&disc);
        dp.setPen(Qt::NoPen);
        dp.setBrush(Qt::white);
        dp.drawEllipse(QPointF(100, 100), 60, 60);
        dp.end();
        const QVector<ai::MaskPolygon> discs = ai::maskPolygons(disc, &error);
        QCOMPARE(discs.size(), 1);
        const QPolygonF ring = discs[0].outer, simple = ai::simplifyRing(ring, 0.75);
        QVERIFY2(simple.size() < ring.size() / 2, qPrintable(QStringLiteral("%1 → %2").arg(ring.size()).arg(simple.size())));
        double area = 0;
        for (int i = 0; i < simple.size(); ++i) {
            const QPointF a = simple[i], b = simple[(i + 1) % simple.size()];
            area += a.x() * b.y() - b.x() * a.y();
        }
        const double discArea = 3.14159265358979 * 60 * 60;
        QVERIFY2(qAbs(std::fabs(area) / 2 - discArea) / discArea < 0.02, qPrintable(QString::number(std::fabs(area) / 2)));
    }

    // The real model on a synthetic aerial scene: a click on the roof and on the pond.
    void segmentationOfASyntheticImage()
    {
        if (!ai::runtimeAvailable()) QSKIP("Built without ONNX Runtime");
        const ai::ModelCard card = segmentationModel();
        if (!card.isValid()) QSKIP(qPrintable(QStringLiteral("No segmentation model: %1").arg(card.error)));
        const QRect roof(300, 180, 200, 150);
        const QPoint pond(140, 140);
        const QImage image = scene(QSize(800, 600), roof, pond, 70);
        ai::SamSegmenter segmenter;
        QString error;
        QElapsedTimer timer;
        timer.start();
        QVERIFY2(segmenter.load(card, &error), qPrintable(error));
        const qint64 loadMs = timer.restart();
        QVERIFY2(segmenter.setImage(image, &error), qPrintable(error));
        const qint64 encodeMs = timer.restart();
        const ai::SamSegmenter::Result r = segmenter.segment({QPointF(roof.center())}, {1});
        const qint64 decodeMs = timer.elapsed();
        QVERIFY2(r.ok(), qPrintable(r.error));
        QCOMPARE(r.mask.size(), image.size());
        const double roofIou = iou(r.mask, [&](int x, int y) { return roof.contains(x, y); });
        qInfo("Load %lld ms, encoder %lld ms, decoder %lld ms; roof IoU %.3f, score %.3f", loadMs, encodeMs, decodeMs, roofIou, r.score);
        QVERIFY2(roofIou >= 0.85, qPrintable(QString::number(roofIou)));
        const ai::SamSegmenter::Result w = segmenter.segment({QPointF(pond)}, {1});
        QVERIFY2(w.ok(), qPrintable(w.error));
        const double pondIou = iou(w.mask, [&](int x, int y) { return std::hypot(x - pond.x(), y - pond.y()) <= 70; });
        qInfo("Pond IoU %.3f, score %.3f", pondIou, w.score);
        QVERIFY2(pondIou >= 0.85, qPrintable(QString::number(pondIou)));
        // A corrupted weights file is refused before ONNX Runtime sees it.
        ai::ModelCard tampered = card;
        tampered.files[0].sha256 = QString(64, QLatin1Char('0'));
        ai::SamSegmenter other;
        QVERIFY(!other.load(tampered, &error));
        QVERIFY2(error.contains(QStringLiteral("SHA-256")), qPrintable(error));
    }

    // LaMa on a field with a red car: the car is gone, the rest of the image is untouched.
    void inpaintingOfASyntheticImage()
    {
        if (!ai::runtimeAvailable()) QSKIP("Built without ONNX Runtime");
        const ai::ModelCard card = ai::AiService().inpaintingCard();
        if (!card.isValid()) QSKIP(qPrintable(QStringLiteral("No inpainting model: %1").arg(card.error)));
        const QRect car(380, 300, 44, 22);
        QImage image = scene(QSize(900, 700), QRect(), QPoint(-500, -500), 1);   // field and road only
        const QImage field = image.copy();
        QPainter(&image).fillRect(car, QColor(200, 30, 30));
        QImage mask(image.size(), QImage::Format_Grayscale8);
        mask.fill(0);
        QPainter(&mask).fillRect(car.adjusted(-3, -3, 3, 3), Qt::white);
        ai::Inpainter inpainter;
        QString error;
        QElapsedTimer timer;
        timer.start();
        QVERIFY2(inpainter.load(card, &error), qPrintable(error));
        const qint64 loadMs = timer.restart();
        const ai::Inpainter::Result r = inpainter.inpaint(image, mask);
        QVERIFY2(r.ok(), qPrintable(r.error));
        qInfo("LaMa: load %lld ms, inpaint %lld ms, windows %d (scaled %d)", loadMs, timer.elapsed(), r.windows, r.scaled);
        QCOMPARE(r.windows, 1);
        QCOMPARE(r.scaled, 0);
        // Inside: the colour of the field, not of the car.
        double red = 0, green = 0, diff = 0;
        int n = 0;
        for (int y = car.top(); y <= car.bottom(); ++y)
            for (int x = car.left(); x <= car.right(); ++x, ++n) {
                const QRgb p = r.image.pixel(x, y), f = field.pixel(x, y);
                red += qRed(p);
                green += qGreen(p);
                diff += std::abs(qRed(p) - qRed(f)) + std::abs(qGreen(p) - qGreen(f)) + std::abs(qBlue(p) - qBlue(f));
            }
        qInfo("Filled: mean R %.1f G %.1f, mean |difference| from the field %.1f", red / n, green / n, diff / n / 3);
        QVERIFY2(green / n > red / n, "the filled area is still red");
        QVERIFY2(diff / n / 3 < 30, qPrintable(QString::number(diff / n / 3)));
        // Outside the mask nothing changed.
        const QImage original = image.convertToFormat(QImage::Format_RGB888);
        for (int y = 0; y < original.height(); y += 7)
            for (int x = 0; x < original.width(); x += 7)
                if (!mask.constScanLine(y)[x]) QCOMPARE(r.image.pixel(x, y), original.pixel(x, y));
    }

    // The map tool: click → contour on the orthophoto → refine → polygon in the drawings.
    void selectionOnTheMap()
    {
        const QString project = m_dir.filePath(QStringLiteral("Проект ИИ"));
        QVERIFY(QDir().mkpath(project));
        // 60 × 60 m at 10 cm; the roof is 20 × 20 m around (500000, 5188000), EPSG:32615.
        const QRect roof(200, 200, 200, 200);
        const QImage image = scene(QSize(600, 600), roof, QPoint(90, 90), 50);
        const QString ortho = m_dir.filePath(QStringLiteral("ortho.tif"));
        const double gt[6] = {499970, 0.1, 0, 5188030, 0, -0.1};
        QVERIFY(makeOrthophoto(ortho, image, gt));

        ai::AiService aiService;
        MapService service;
        service.setAi(&aiService);
        service.setViewSize(QSizeF(800, 600));
        service.setProject(project, nullptr);
        QCOMPARE(service.addLayerFromPath(ortho), QString());
        QTRY_VERIFY_WITH_TIMEOUT([&] {
            for (const MapLayer &l : service.layers())
                if (l.source == ortho) return !l.loading;
            return false;
        }(), 30000);
        service.setView(QPointF(500000, 5188000), 0.1);
        service.setTool(QStringLiteral("aiSelect"));
        QVERIFY2(service.measurementText().contains(QStringLiteral("Щёлкните")), qPrintable(service.measurementText()));
        QVERIFY(service.aiSelectAt(QPointF(600000, 5188000)).contains(QStringLiteral("нет видимого")));
        QVERIFY(service.aiSelectAt(QPointF(500000, 5188000), false, true).contains(QStringLiteral("Сначала выделите")));
        if (!aiService.segmentationProblem().isEmpty()) QSKIP(qPrintable(aiService.segmentationProblem()));

        QSignalSpy finished(&service, &MapService::aiFinished);
        const QPointF click = service.toScreen(QPointF(500000, 5188000));
        service.pointerPressed(click, Qt::LeftButton, Qt::NoModifier);
        service.pointerReleased(click);
        QVERIFY(service.aiBusy());
        QVERIFY(!service.aiSelectAt(QPointF(500005, 5188005)).isEmpty());   // one at a time
        QVERIFY(finished.wait(120000));
        QVERIFY2(finished.last().at(0).toBool(), qPrintable(finished.last().at(1).toString()));
        QVERIFY(service.hasAiCandidate());
        VectorFeature candidate;
        candidate.kind = VectorFeature::Polygon;
        candidate.parts = service.aiCandidate();
        const double expected = 400 / (0.9996 * 0.9996);   // geodesic area of 20 × 20 m of UTM grid near the central meridian
        const double area = service.polygonArea(candidate);
        qInfo("AI contour: %lld vertices, %.2f m² (expected %.2f), score %.3f; %s", qint64(candidate.parts.first().size()), area, expected,
              service.aiScore(), qPrintable(service.aiStatus()));
        QVERIFY2(qAbs(area - expected) / expected < 0.05, qPrintable(QString::number(area)));
        QVERIFY(service.measurementText().contains(QStringLiteral("площадь")));
        QVERIFY(service.measurementDetails().contains(QStringLiteral("MobileSAM")));

        // Ctrl+click on the field: excluded; the roof stays (same image, only the decoder runs).
        const QPointF field = service.toScreen(QPointF(499980, 5188020));
        service.pointerPressed(field, Qt::LeftButton, Qt::ControlModifier);
        service.pointerReleased(field);
        QVERIFY(finished.wait(60000));
        QVERIFY2(finished.last().at(0).toBool(), qPrintable(finished.last().at(1).toString()));
        candidate.parts = service.aiCandidate();
        QVERIFY2(qAbs(service.polygonArea(candidate) - expected) / expected < 0.1, qPrintable(QString::number(service.polygonArea(candidate))));
        QVERIFY(service.measurementDetails().contains(QStringLiteral("1 исключено")));

        // Enter adds it to the drawings, undoably, with its method and the model's score.
        QVERIFY(service.keyPressed(Qt::Key_Return));
        QCOMPARE(service.drawingCount(), 1);
        QVERIFY(!service.hasAiCandidate());
        VectorFeature added;
        for (const MapLayer &l : service.layers())
            if (l.type == MapLayer::Drawings && !l.vector.features.isEmpty()) added = l.vector.features.first();
        QVERIFY(added.attributes.value(QStringLiteral("name")).toString().startsWith(QStringLiteral("ИИ-объект")));
        QVERIFY(added.attributes.value(QStringLiteral("method")).toString().contains(QStringLiteral("MobileSAM")));
        QVERIFY(added.attributes.value(QStringLiteral("ai_score")).toDouble() > 0.5);
        QCOMPARE(service.undoText(), QStringLiteral("Добавление объекта ИИ"));
        service.undo();
        QCOMPARE(service.drawingCount(), 0);
        service.redo();
        QCOMPARE(service.drawingCount(), 1);

        // Escape drops a proposal; another tool too.
        service.aiSelectAt(QPointF(499979, 5188021));
        QVERIFY(finished.wait(60000));
        service.keyPressed(Qt::Key_Escape);
        QVERIFY(!service.hasAiCandidate());

        // Saved with the project: the drawing and its area in square metres.
        service.setProject(QString(), nullptr);
        const VectorData saved = VectorData::load(QDir(project).filePath(QStringLiteral("vectors/drawings.geojson")), {}, {}, Crs::fromEpsg(32615));
        QVERIFY2(saved.valid, qPrintable(saved.error));
        QCOMPARE(saved.features.size(), 1);
        const double savedArea = saved.features.first().attributes.value(QStringLiteral("area_m2")).toDouble();
        QVERIFY2(qAbs(savedArea - expected) / expected < 0.1, qPrintable(QString::number(savedArea)));
    }

    // Removal of a car from an orthophoto: click, remove, the copy replaces the original on the
    // map, the user's file is untouched, undo and redo rewrite the window.
    void removalFromTheOrthophoto()
    {
        const QString project = m_dir.filePath(QStringLiteral("Проект правки"));
        QVERIFY(QDir().mkpath(project));
        // 90 × 70 m at 10 cm, a 4.4 × 2.2 m red car at (500000, 5188000).
        const QRect car(400, 340, 44, 22);
        QImage image = scene(QSize(900, 700), QRect(), QPoint(-500, -500), 1);
        QPainter(&image).fillRect(car, QColor(200, 30, 30));
        const QString ortho = m_dir.filePath(QStringLiteral("ortho-car.tif"));
        const double gt[6] = {499958, 0.1, 0, 5188035, 0, -0.1};
        QVERIFY(makeOrthophoto(ortho, image, gt));
        auto hash = [](const QString &path) {
            QFile f(path);
            return f.open(QIODevice::ReadOnly) ? QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256) : QByteArray();
        };
        const QByteArray originalHash = hash(ortho);

        ai::AiService aiService;
        MapService service;
        service.setAi(&aiService);
        service.setViewSize(QSizeF(800, 600));
        service.setProject(project, nullptr);
        QCOMPARE(service.addLayerFromPath(ortho), QString());
        QTRY_VERIFY_WITH_TIMEOUT(!service.layers().last().loading, 30000);
        const QString originalId = service.layerIdForSource(ortho);
        QVERIFY(service.removeFromOrthophoto().contains(QStringLiteral("Выделите объект")));
        if (!aiService.segmentationProblem().isEmpty() || !aiService.inpaintingProblem().isEmpty())
            QSKIP(qPrintable(aiService.segmentationProblem() + aiService.inpaintingProblem()));

        const QPointF carCentre(499958 + (car.center().x() + 0.5) * 0.1, 5188035 - (car.center().y() + 0.5) * 0.1);
        service.setView(carCentre, 0.05);
        QSignalSpy finished(&service, &MapService::aiFinished);
        QCOMPARE(service.aiSelectAt(carCentre), QString());
        QVERIFY(finished.wait(120000));
        QVERIFY2(finished.last().at(0).toBool(), qPrintable(finished.last().at(1).toString()));
        QCOMPARE(service.removeFromOrthophoto(0.3), QString());
        QVERIFY(service.aiBusy());
        QVERIFY(finished.wait(120000));
        QVERIFY2(finished.last().at(0).toBool(), qPrintable(finished.last().at(1).toString()));
        qInfo("%s", qPrintable(service.aiStatus()));
        QVERIFY(!service.hasAiCandidate());

        // The copy is on the map above the hidden original; the user's file did not change.
        const QString copy = QDir(project).filePath(QStringLiteral("analysis/ortho-car_ИИ-правка.tif"));
        QVERIFY(QFileInfo::exists(copy));
        QCOMPARE(hash(ortho), originalHash);
        const QString copyId = service.layerIdForSource(copy);
        QVERIFY(!copyId.isEmpty());
        for (const MapLayer &l : service.layers()) {
            if (l.id == originalId) QVERIFY(!l.visible);
            if (l.id == copyId) {
                QVERIFY(l.visible);
                QVERIFY(l.name.contains(QStringLiteral("правка ИИ")));
            }
        }
        auto carRedness = [&](const QString &path) {
            QImage rgb;
            QString error;
            if (!rasteredit::readRgb(path, car, &rgb, nullptr, &error)) return -1.0;
            double s = 0;
            for (int y = 0; y < rgb.height(); ++y)
                for (int x = 0; x < rgb.width(); ++x) s += qRed(rgb.pixel(x, y)) - qGreen(rgb.pixel(x, y));
            return s / (rgb.width() * rgb.height());
        };
        qInfo("Red − green over the car: original %.1f, edited %.1f", carRedness(ortho), carRedness(copy));
        QVERIFY(carRedness(ortho) > 100);
        QVERIFY2(carRedness(copy) < 10, qPrintable(QString::number(carRedness(copy))));
        // The overview of the copy shows the edit too.
        GDALDatasetH ds = GDALOpen(QDir::toNativeSeparators(copy).toUtf8().constData(), GA_ReadOnly);
        QVERIFY(ds);
        QVERIFY(GDALGetOverviewCount(GDALGetRasterBand(ds, 1)) >= 1);
        uchar r = 0, g = 0;
        GDALRasterBandH ov1 = GDALGetOverview(GDALGetRasterBand(ds, 1), 0), ov2 = GDALGetOverview(GDALGetRasterBand(ds, 2), 0);
        QCOMPARE(GDALRasterIO(ov1, GF_Read, car.center().x() / 2, car.center().y() / 2, 1, 1, &r, 1, 1, GDT_Byte, 0, 0), CE_None);
        QCOMPARE(GDALRasterIO(ov2, GF_Read, car.center().x() / 2, car.center().y() / 2, 1, 1, &g, 1, 1, GDT_Byte, 0, 0), CE_None);
        GDALClose(ds);
        QVERIFY2(r < g, qPrintable(QStringLiteral("overview %1/%2").arg(r).arg(g)));
        // The edit is listed with its outline and model.
        QFile log(copy + QStringLiteral(".edits.json"));
        QVERIFY(log.open(QIODevice::ReadOnly));
        const QJsonArray edits = QJsonDocument::fromJson(log.readAll()).object().value(QStringLiteral("edits")).toArray();
        log.close();
        QCOMPARE(edits.size(), 1);
        QCOMPARE(edits.first().toObject().value(QStringLiteral("model")).toString(), QStringLiteral("lama"));
        QVERIFY(edits.first().toObject().value(QStringLiteral("pixels")).toInt() >= car.width() * car.height());

        // Undo puts the car back into the copy, redo removes it again.
        QVERIFY(service.undoText().contains(QStringLiteral("ортофотоплана")));
        service.undo();
        QVERIFY(carRedness(copy) > 100);
        service.redo();
        QVERIFY(carRedness(copy) < 10);
        service.setProject(QString(), nullptr);
    }
};

QTEST_MAIN(AiTest)
#include "test_ai.moc"
