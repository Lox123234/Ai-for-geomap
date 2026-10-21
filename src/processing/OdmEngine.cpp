#include "OdmEngine.h"
#include "Downloader.h"
#include "GeoTiffInfo.h"
#include "Platform.h"
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QtEndian>
#include <algorithm>
#include <cmath>

namespace {
constexpr int RecentLines = 200;
constexpr qint64 GiB = 1024LL * 1024 * 1024;
constexpr qint64 MiB = 1024LL * 1024;
const QString Native = QStringLiteral("native");

const QLocale &ru()
{
    static const QLocale locale(QLocale::Russian);
    return locale;
}
QString gigabytes(qint64 bytes) { return ru().toString(double(bytes) / GiB, 'f', 1); }
QString megabytes(qint64 bytes) { return ru().toString(double(bytes) / MiB, 'f', 1); }
// "LAS/LAZ, N точек" from the public header block, or empty if the magic is wrong.
QString describeLas(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QByteArray h = file.read(255);
    if (h.size() < 227 || !h.startsWith("LASF")) return QStringLiteral("повреждён: нет сигнатуры LASF");
    const auto *u = reinterpret_cast<const uchar *>(h.constData());
    const int major = h[24], minor = h[25];
    quint64 points = qFromLittleEndian<quint32>(u + 107);
    if (points == 0 && minor >= 4 && h.size() >= 255) points = qFromLittleEndian<quint64>(u + 247);
    return QStringLiteral("LAS %1.%2, точек: %3").arg(major).arg(minor).arg(points);
}

// ODM options GeoMap AI accepts: the structured settings of the UI and the whitelist for the
// expert field. Names and ranges follow `run.py --help` of ODM 3.6.2.
struct Option
{
    enum Kind { Flag, Choice, Integer, Number };
    QString id;            // ODM option without "--"
    QString group;         // empty: accepted in the expert field only
    QString title;
    Kind kind = Flag;
    QStringList values;    // Choice
    QStringList titles;    // Choice, same order
    double min = 0, max = 0;
    QString unit;
    QString help;
};

const QStringList Qualities = {QStringLiteral("ultra"), QStringLiteral("high"), QStringLiteral("medium"),
                               QStringLiteral("low"), QStringLiteral("lowest")};
const QStringList QualityTitles = {QStringLiteral("Сверхвысокая"), QStringLiteral("Высокая"), QStringLiteral("Средняя"),
                                   QStringLiteral("Низкая"), QStringLiteral("Наименьшая")};

const QVector<Option> &options()
{
    static const QVector<Option> list = [] {
        const QString align = QStringLiteral("Выравнивание снимков");
        const QString dense = QStringLiteral("Плотное облако точек");
        const QString model = QStringLiteral("Модель и рельеф");
        const QString ortho = QStringLiteral("Ортофотоплан");
        const QString area = QStringLiteral("Область и маски");
        const QString res = QStringLiteral("Ресурсы");
        QVector<Option> o;
        o.append({QStringLiteral("feature-quality"), align, QStringLiteral("Точность выравнивания"), Option::Choice,
                  Qualities, QualityTitles, 0, 0, {},
                  QStringLiteral("Качество поиска признаков. Выше — больше совпадений и точнее, но дольше и больше памяти.")});
        o.append({QStringLiteral("feature-type"), align, QStringLiteral("Детектор признаков"), Option::Choice,
                  {QStringLiteral("dspsift"), QStringLiteral("sift"), QStringLiteral("hahog"), QStringLiteral("akaze"), QStringLiteral("orb")},
                  {QStringLiteral("DSP-SIFT (по умолчанию ODM)"), QStringLiteral("SIFT — на видеокарте NVIDIA, если доступна"),
                   QStringLiteral("HAHOG"), QStringLiteral("AKAZE"), QStringLiteral("ORB (быстро, грубо)")}, 0, 0, {},
                  QStringLiteral("SIFT встроенный ODM считает на GPU NVIDIA (CUDA); остальные — на процессоре.")});
        o.append({QStringLiteral("min-num-features"), align, QStringLiteral("Признаков на снимок, не меньше"), Option::Integer,
                  {}, {}, 1000, 200000, {}, QStringLiteral("По умолчанию 10 000. Больше — помогает при слабом перекрытии, но медленнее.")});
        o.append({QStringLiteral("matcher-neighbors"), align, QStringLiteral("Соседей по GPS при сопоставлении"), Option::Integer,
                  {}, {}, 0, 1000, {}, QStringLiteral("0 — пары выбираются триангуляцией точек съёмки (по умолчанию).")});
        o.append({QStringLiteral("sfm-algorithm"), align, QStringLiteral("Алгоритм реконструкции"), Option::Choice,
                  {QStringLiteral("incremental"), QStringLiteral("triangulation"), QStringLiteral("planar")},
                  {QStringLiteral("Инкрементальный (по умолчанию)"), QStringLiteral("По GPS и углам камеры"),
                   QStringLiteral("Плоская сцена, надир (быстро)")}, 0, 0, {},
                  QStringLiteral("«По GPS и углам камеры» — для аэросъёмки с координатами и углами подвеса; «Плоская сцена» — для съёмки "
                                 "ровной местности строго вниз на одной высоте.")});
        o.append({QStringLiteral("gps-accuracy"), align, QStringLiteral("Точность GPS"), Option::Number, {}, {}, 0.01, 100,
                  QStringLiteral("м"), QStringLiteral("Если не задана, ODM берёт её из снимков (RTK определяется автоматически).")});
        o.append({QStringLiteral("rolling-shutter"), align, QStringLiteral("Коррекция rolling shutter"), Option::Flag, {}, {}, 0, 0, {},
                  QStringLiteral("Для камер с построчным затвором (многие DJI Mini/Air/Mavic), снимавших в движении.")});
        o.append({QStringLiteral("use-hybrid-bundle-adjustment"), align, QStringLiteral("Гибридная оптимизация"), Option::Flag,
                  {}, {}, 0, 0, {}, QStringLiteral("Ускоряет очень большие наборы (тысячи снимков).")});
        o.append({QStringLiteral("pc-quality"), dense, QStringLiteral("Качество карт глубины"), Option::Choice,
                  Qualities, QualityTitles, 0, 0, {},
                  QStringLiteral("Каждая ступень выше примерно в 4 раза дольше.")});
        o.append({QStringLiteral("pc-filter"), dense, QStringLiteral("Фильтр выбросов"), Option::Number, {}, {}, 0, 10,
                  QStringLiteral("σ"), QStringLiteral("Удаляет точки дальше N стандартных отклонений. 0 — выключен, по умолчанию 5; меньше — строже.")});
        o.append({QStringLiteral("pc-sample"), dense, QStringLiteral("Прореживание облака"), Option::Number, {}, {}, 0, 100,
                  QStringLiteral("м"), QStringLiteral("Одна точка в радиусе N м. 0 — без прореживания.")});
        o.append({QStringLiteral("pc-classify"), dense, QStringLiteral("Классифицировать точки (земля)"), Option::Flag, {}, {}, 0, 0, {},
                  QStringLiteral("Земля — фильтром SMRF; растительность, здания и машины — моделью OpenPointClass, которую ODM скачивает "
                                 "сам из интернета при первом использовании (0,6 МБ, AGPL-3.0).")});
        o.append({QStringLiteral("pc-las"), dense, QStringLiteral("Сохранить облако также в LAS"), Option::Flag});
        o.append({QStringLiteral("mesh-size"), model, QStringLiteral("Вершин в модели, не больше"), Option::Integer, {}, {},
                  10000, 50000000, {}, QStringLiteral("По умолчанию 200 000.")});
        o.append({QStringLiteral("mesh-octree-depth"), model, QStringLiteral("Детализация сетки (глубина октодерева)"),
                  Option::Integer, {}, {}, 1, 14, {}, QStringLiteral("По умолчанию 11; рекомендуется 8–12.")});
        o.append({QStringLiteral("skip-3dmodel"), model, QStringLiteral("Без полной 3D-модели (только 2D)"), Option::Flag});
        o.append({QStringLiteral("gltf"), model, QStringLiteral("Модель также в glTF (GLB)"), Option::Flag});
        o.append({QStringLiteral("3d-tiles"), model, QStringLiteral("3D-тайлы (OGC, Cesium)"), Option::Flag, {}, {}, 0, 0, {},
                  QStringLiteral("Модель и облако тайлами для веб-просмотрщиков.")});
        o.append({QStringLiteral("dsm"), model, QStringLiteral("Цифровая модель поверхности (DSM)"), Option::Flag});
        o.append({QStringLiteral("dtm"), model, QStringLiteral("Цифровая модель рельефа (DTM)"), Option::Flag});
        o.append({QStringLiteral("dem-resolution"), model, QStringLiteral("Разрешение ЦМП/ЦМР"), Option::Number, {}, {}, 0.1, 10000,
                  QStringLiteral("см/пикс"), QStringLiteral("Не точнее оценки GSD.")});
        o.append({QStringLiteral("orthophoto-resolution"), ortho, QStringLiteral("Разрешение ортофотоплана"), Option::Number, {}, {},
                  0.1, 10000, QStringLiteral("см/пикс"), QStringLiteral("Не точнее оценки GSD.")});
        o.append({QStringLiteral("orthophoto-cutline"), ortho, QStringLiteral("Линия реза по краям объектов"), Option::Flag});
        o.append({QStringLiteral("cog"), ortho, QStringLiteral("Cloud-Optimized GeoTIFF"), Option::Flag});
        o.append({QStringLiteral("build-overviews"), ortho, QStringLiteral("Обзорные уровни для GIS"), Option::Flag});
        o.append({QStringLiteral("auto-boundary"), area, QStringLiteral("Граница по точкам съёмки"), Option::Flag, {}, {}, 0, 0, {},
                  QStringLiteral("Отсекает фон и горизонт. Нужен GPS.")});
        o.append({QStringLiteral("crop"), area, QStringLiteral("Обрезка краёв"), Option::Number, {}, {}, 0, 1000, QStringLiteral("м"),
                  QStringLiteral("По умолчанию 3 м; 0 — без обрезки.")});
        o.append({QStringLiteral("sky-removal"), area, QStringLiteral("Удаление неба (ИИ ODM, экспериментально)"), Option::Flag, {}, {}, 0, 0, {},
                  QStringLiteral("ODM скачивает модель SkyRemoval (AGPL-3.0) из интернета при первом использовании. Без загрузок: маски снимков "
                                 "GeoMap AI (Правка → Маска снимка).")});
        o.append({QStringLiteral("max-concurrency"), res, QStringLiteral("Потоков"), Option::Integer, {}, {}, 1, 1024, {},
                  QStringLiteral("По умолчанию — по числу ядер и объёму памяти (около 1 ГБ на поток).")});
        o.append({QStringLiteral("split"), res, QStringLiteral("Разбиение: снимков на подмодель"), Option::Integer, {}, {}, 1, 100000, {},
                  QStringLiteral("Для больших наборов: обработка частями с последующим слиянием. Нужен GPS.")});
        o.append({QStringLiteral("optimize-disk-space"), res, QStringLiteral("Экономить место на диске"), Option::Flag, {}, {}, 0, 0, {},
                  QStringLiteral("Удаляет промежуточные файлы; задание нельзя будет возобновить с середины.")});
        o.append({QStringLiteral("no-gpu"), res, QStringLiteral("Не использовать GPU"), Option::Flag});
        // Expert field only.
        o.append({QStringLiteral("fast-orthophoto"), {}, {}, Option::Flag});
        o.append({QStringLiteral("skip-report"), {}, {}, Option::Flag});
        o.append({QStringLiteral("use-3dmesh"), {}, {}, Option::Flag});
        o.append({QStringLiteral("pc-ept"), {}, {}, Option::Flag});
        o.append({QStringLiteral("pc-skip-geometric"), {}, {}, Option::Flag});
        o.append({QStringLiteral("sfm-no-partial"), {}, {}, Option::Flag});
        o.append({QStringLiteral("texturing-single-material"), {}, {}, Option::Flag});
        o.append({QStringLiteral("ignore-gsd"), {}, {}, Option::Flag});
        o.append({QStringLiteral("split-overlap"), {}, {}, Option::Number, {}, {}, 0, 100000});
        o.append({QStringLiteral("dem-gapfill-steps"), {}, {}, Option::Integer, {}, {}, 0, 100});
        o.append({QStringLiteral("matcher-type"), {}, {}, Option::Choice,
                  {QStringLiteral("flann"), QStringLiteral("bow"), QStringLiteral("bruteforce")}});
        o.append({QStringLiteral("camera-lens"), {}, {}, Option::Choice,
                  {QStringLiteral("auto"), QStringLiteral("perspective"), QStringLiteral("brown"), QStringLiteral("fisheye"),
                   QStringLiteral("fisheye_opencv"), QStringLiteral("spherical"), QStringLiteral("equirectangular"), QStringLiteral("dual")}});
        o.append({QStringLiteral("orthophoto-compression"), {}, {}, Option::Choice,
                  {QStringLiteral("JPEG"), QStringLiteral("LZW"), QStringLiteral("PACKBITS"), QStringLiteral("DEFLATE"),
                   QStringLiteral("LZMA"), QStringLiteral("NONE")}});
        o.append({QStringLiteral("end-with"), {}, {}, Option::Choice, OdmEngine::stageIds()});
        return o;
    }();
    return list;
}

const Option *findOption(const QString &id)
{
    for (const Option &o : options()) if (o.id == id) return &o;
    return nullptr;
}

// Checks one value; returns the command-line text or an empty string with an error.
QString optionValue(const Option &o, const QVariant &value, QString *error)
{
    auto fail = [&](const QString &message) {
        if (error) *error = message;
        return QString();
    };
    if (o.kind == Option::Choice) {
        const QString v = value.toString();
        if (!o.values.contains(v)) return fail(QStringLiteral("Недопустимое значение «%1» для «--%2».").arg(v, o.id));
        return v;
    }
    bool ok = false;
    const double d = value.typeId() == QMetaType::QString ? value.toString().toDouble(&ok) : value.toDouble(&ok);
    if (!ok || !std::isfinite(d) || d < o.min || d > o.max)
        return fail(QStringLiteral("Недопустимое значение «%1» для «--%2» (допустимо %3–%4).")
                        .arg(value.toString(), o.id).arg(o.min).arg(o.max));
    if (o.kind == Option::Integer) {
        if (d != std::floor(d)) return fail(QStringLiteral("Для «--%1» нужно целое число.").arg(o.id));
        return QString::number(qint64(d));
    }
    return QString::number(d, 'g', 10);
}

QStringList removeOption(QStringList args, const QString &flag, bool valued)
{
    for (int i = args.indexOf(flag); i >= 0; i = args.indexOf(flag)) {
        args.removeAt(i);
        if (valued && i < args.size()) args.removeAt(i);
    }
    return args;
}
} // namespace

OdmEngine::OdmEngine(QObject *parent) : IProcessingEngine(parent) {}

OdmEngine::~OdmEngine()
{
    abort(m_runningJob);
    if (m_download) m_download->cancel();
}
EngineCapabilities OdmEngine::capabilities() const
{
    EngineCapabilities c;
    c.id = QStringLiteral("odm");
    c.displayName = QStringLiteral("OpenDroneMap");
    c.version = QLatin1String(Version);
    c.supportsCancel = true;
    c.supportsPause = false;
    c.outputs = {QStringLiteral("orthophoto"), QStringLiteral("dsm"), QStringLiteral("dtm"),
                 QStringLiteral("pointcloud"), QStringLiteral("mesh"), QStringLiteral("report")};
    // opendm/context.py supported_extensions in ODM 3.6.2, minus raw formats GeoMap AI does not import.
    c.inputSuffixes = {QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("png"),
                       QStringLiteral("tif"), QStringLiteral("tiff")};
    c.minimumImages = 2;   // structure from motion needs at least one overlapping pair
    return c;
}

QStringList OdmEngine::resourceWarnings(int imageCount, const ProcessingProfile &profile,
                                        const EnvironmentReport &env) const
{
    QStringList warnings;
    if (imageCount < 5)
        warnings << QStringLiteral("Снимков мало (%1): реконструкция возможна только при большом перекрытии кадров.")
                        .arg(imageCount);
    if (profile.id == QLatin1String("high") && env.memoryBytes > 0 && env.memoryBytes < 16 * GiB)
        warnings << QStringLiteral("«Высокое качество» при %1 ГБ памяти может не поместиться в память.")
                        .arg(gigabytes(env.memoryBytes));
    if (imageCount > 1000)
        warnings << QStringLiteral("Большой набор (%1 снимков): для экономии памяти можно включить разбиение на подмодели "
                                   "(«Ресурсы» → «Разбиение», например 400).").arg(imageCount);
    return warnings;
}

QVector<ProcessingProfile> OdmEngine::profiles() const
{
    // Documented in docs/PROCESSING_ODM.md; keep both in sync.
    return {
        {QStringLiteral("align"), QStringLiteral("Только выравнивание"),
         QStringLiteral("Положение камер и разреженное облако — быстро проверить снимки и качество выравнивания. "
                        "Затем задание можно продолжить с любым профилем."),
         {QStringLiteral("--feature-quality"), QStringLiteral("high"), QStringLiteral("--end-with"), QStringLiteral("opensfm")}, 1},
        {QStringLiteral("fast"), QStringLiteral("Быстрый просмотр"),
         QStringLiteral("Ортофотоплан по разреженной реконструкции, 10 см/пикс. Без плотного облака и рельефа."),
         {QStringLiteral("--fast-orthophoto"), QStringLiteral("--feature-quality"), QStringLiteral("medium"),
          QStringLiteral("--orthophoto-resolution"), QStringLiteral("10")}, 3},
        {QStringLiteral("balanced"), QStringLiteral("Сбалансированный"),
         QStringLiteral("Плотное облако среднего качества, ЦМП (DSM), ортофотоплан 5 см/пикс."),
         {QStringLiteral("--feature-quality"), QStringLiteral("high"), QStringLiteral("--pc-quality"), QStringLiteral("medium"),
          QStringLiteral("--dsm"), QStringLiteral("--orthophoto-resolution"), QStringLiteral("5"),
          QStringLiteral("--dem-resolution"), QStringLiteral("10"), QStringLiteral("--auto-boundary"),
          QStringLiteral("--pc-las")}, 8},   // LAS next to LAZ: the 3D view reads it (classes, intensity)
        {QStringLiteral("high"), QStringLiteral("Высокое качество"),
         QStringLiteral("Максимальные признаки, плотное облако высокого качества, DSM и DTM, ортофотоплан 2 см/пикс. Долго и требует много памяти."),
         {QStringLiteral("--feature-quality"), QStringLiteral("ultra"), QStringLiteral("--pc-quality"), QStringLiteral("high"),
          QStringLiteral("--dsm"), QStringLiteral("--dtm"), QStringLiteral("--orthophoto-resolution"), QStringLiteral("2"),
          QStringLiteral("--dem-resolution"), QStringLiteral("5"), QStringLiteral("--mesh-octree-depth"), QStringLiteral("12"),
          QStringLiteral("--auto-boundary"), QStringLiteral("--pc-las")}, 15},
    };
}

QStringList OdmEngine::stageIds()
{
    return {QStringLiteral("dataset"), QStringLiteral("split"), QStringLiteral("merge"), QStringLiteral("opensfm"),
            QStringLiteral("openmvs"), QStringLiteral("odm_filterpoints"), QStringLiteral("odm_meshing"),
            QStringLiteral("mvs_texturing"), QStringLiteral("odm_georeferencing"), QStringLiteral("odm_dem"),
            QStringLiteral("odm_orthophoto"), QStringLiteral("odm_report"), QStringLiteral("odm_postprocess")};
}

QString OdmEngine::stageTitle(const QString &stage) const
{
    static const QHash<QString, QString> titles = {
        {QStringLiteral("dataset"), QStringLiteral("Загрузка снимков")},
        {QStringLiteral("split"), QStringLiteral("Разбиение на подмодели")},
        {QStringLiteral("merge"), QStringLiteral("Слияние подмоделей")},
        {QStringLiteral("opensfm"), QStringLiteral("Выравнивание снимков (OpenSfM)")},
        {QStringLiteral("openmvs"), QStringLiteral("Плотное облако точек (OpenMVS)")},
        {QStringLiteral("odm_filterpoints"), QStringLiteral("Фильтрация точек")},
        {QStringLiteral("odm_meshing"), QStringLiteral("Построение сетки")},
        {QStringLiteral("mvs_texturing"), QStringLiteral("Текстурирование")},
        {QStringLiteral("odm_georeferencing"), QStringLiteral("Геопривязка")},
        {QStringLiteral("odm_dem"), QStringLiteral("Модели рельефа (DSM/DTM)")},
        {QStringLiteral("odm_orthophoto"), QStringLiteral("Ортофотоплан")},
        {QStringLiteral("odm_report"), QStringLiteral("Отчёт")},
        {QStringLiteral("odm_postprocess"), QStringLiteral("Завершение")},
    };
    return titles.value(stage, stage);
}

QVariantList OdmEngine::optionDefinitions(const EnvironmentReport &env) const
{
    QVariantList list;
    QString group;
    for (const Option &o : options()) {
        if (o.group.isEmpty()) continue;
        if (o.id == QLatin1String("no-gpu") && env.gpuName.isEmpty()) continue;
        const bool first = o.group != group;
        group = o.group;
        QVariantList choices;
        for (int i = 0; i < o.values.size(); ++i)
            choices << QVariantMap{{"value", o.values[i]}, {"title", i < o.titles.size() ? o.titles[i] : o.values[i]}};
        static const char *kinds[] = {"flag", "choice", "integer", "number"};
        QString help = o.help;
        if (o.id == QLatin1String("feature-type") && !env.gpuName.isEmpty())
            help += QStringLiteral(" Найдена видеокарта: %1.").arg(env.gpuName);
        list << QVariantMap{{"id", o.id}, {"group", o.group}, {"firstInGroup", first}, {"title", o.title},
                            {"type", QString::fromLatin1(kinds[o.kind])},
                            {"choices", choices}, {"min", o.min}, {"max", o.max}, {"unit", o.unit}, {"help", help}};
    }
    return list;
}

QString OdmEngine::stripAnsi(const QString &line)
{
    static const QRegularExpression ansi(QStringLiteral("\x1B\\[[0-9;]*[A-Za-z]"));
    QString clean = line;
    clean.remove(ansi);
    return clean;
}

QString OdmEngine::parseStage(const QString &line)
{
    static const QRegularExpression running(QStringLiteral("\\bRunning ([a-z_]+) stage\\b"));
    const QRegularExpressionMatch m = running.match(line);
    return m.hasMatch() ? m.captured(1) : QString();
}

bool OdmEngine::validateExtraArguments(const QStringList &arguments, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error) *error = message;
        return false;
    };
    for (int i = 0; i < arguments.size(); ++i) {
        const QString &a = arguments[i];
        const Option *o = a.startsWith(QLatin1String("--")) ? findOption(a.mid(2)) : nullptr;
        if (!o) {
            QStringList known;
            for (const Option &x : options()) known << QStringLiteral("--") + x.id;
            return fail(QStringLiteral("Параметр «%1» не поддерживается. Допустимы: %2.").arg(a, known.join(QStringLiteral(", "))));
        }
        if (o->kind == Option::Flag) continue;
        if (i + 1 >= arguments.size()) return fail(QStringLiteral("Для «%1» не указано значение.").arg(a));
        if (optionValue(*o, arguments[++i], error).isEmpty()) return false;
    }
    return true;
}

QStringList OdmEngine::optionArguments(const QVariantMap &values, QString *error)
{
    QStringList args;
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        const Option *o = findOption(it.key());
        if (!o || o->group.isEmpty()) {
            if (error) *error = QStringLiteral("Неизвестный параметр «%1».").arg(it.key());
            return {};
        }
        const QVariant &v = it.value();
        if (!v.isValid() || v.isNull() || (v.typeId() == QMetaType::QString && v.toString().isEmpty())) continue;
        if (o->kind == Option::Flag) {
            if (v.toBool()) args << QStringLiteral("--") + o->id;
            continue;
        }
        const QString text = optionValue(*o, v, error);
        if (text.isEmpty()) return {};
        args << QStringLiteral("--") + o->id << text;
    }
    if (error) error->clear();
    return args;
}

int OdmEngine::concurrencyFor(int cpus, qint64 memoryBytes)
{
    // ODM needs roughly 1 GB per thread (ODM help for --max-concurrency); keep 1 GB spare.
    int n = cpus > 0 ? cpus : QThread::idealThreadCount();
    const int memoryGb = int(memoryBytes / GiB);
    if (memoryGb > 1) n = qMin(n, memoryGb - 1);
    return qMax(1, n);
}

bool OdmEngine::createJob(ProcessingJob &job, const JobRequest &request, const EnvironmentReport &env, QString *error)
{
    if (!request.profile) {
        if (error) *error = QStringLiteral("Не выбран профиль обработки.");
        return false;
    }
    if (!validateExtraArguments(request.extraArguments, error)) return false;
    QString optionError;
    const QStringList structured = optionArguments(request.options, &optionError);
    if (!optionError.isEmpty()) {
        if (error) *error = optionError;
        return false;
    }
    // Later occurrences win in ODM's argument parser: profile < settings < expert field.
    QStringList args = request.profile->arguments + structured + request.extraArguments;
    QStringList notes;
    const DatasetSummary &d = request.dataset;
    if (d.images > 0 && d.withGps == 0) {
        if (args.contains(QStringLiteral("--auto-boundary"))) {
            args = removeOption(args, QStringLiteral("--auto-boundary"), false);
            notes << QStringLiteral("У снимков нет GPS: граница по точкам съёмки (--auto-boundary) отключена.");
        }
        if (args.contains(QStringLiteral("--split"))) {
            if (error) *error = QStringLiteral("Разбиение на подмодели (--split) требует GPS у снимков.");
            return false;
        }
        if (args.contains(QStringLiteral("triangulation"))) notes << QStringLiteral("Алгоритм «По GPS и углам камеры» без GPS не даст преимуществ.");
    }
    const int sift = args.lastIndexOf(QStringLiteral("--feature-type"));
    if (sift >= 0 && sift + 1 < args.size() && args[sift + 1] == QLatin1String("sift")) {
        if (env.gpuName.isEmpty())
            notes << QStringLiteral("Видеокарта NVIDIA не найдена: SIFT будет считаться на процессоре.");
        else if (!args.contains(QStringLiteral("--no-gpu")))
            notes << QStringLiteral("Признаки SIFT ищутся на видеокарте %1.").arg(env.gpuName);
    }
    if (!args.contains(QStringLiteral("--max-concurrency")))
        args << QStringLiteral("--max-concurrency") << QString::number(concurrencyFor(env.cpus, env.memoryBytes));
    job.engine = QStringLiteral("odm");
    job.engineVersion = QLatin1String(Version);
    job.backend = env.backend;
    job.arguments = args;
    job.notes = notes;
    job.stageCount = int(stageIds().size());
    return true;
}

QStringList OdmEngine::resumeArguments(const QStringList &arguments, const QString &fromStage) const
{
    QStringList args = removeOption(arguments, QStringLiteral("--end-with"), true);
    args = removeOption(args, QStringLiteral("--rerun-from"), true);
    args = removeOption(args, QStringLiteral("--rerun"), true);
    args = removeOption(args, QStringLiteral("--rerun-all"), false);
    // ODM skips a stage whose outputs exist; after a crash they may be half written, so the
    // stage that failed (or the one given) is computed again with everything after it.
    if (stageIds().contains(fromStage)) args << QStringLiteral("--rerun-from") << fromStage;
    return args;
}

QStringList OdmEngine::stepArguments(const QString &step, const ProcessingJob &job, QString *error) const
{
    const int end = int(job.arguments.indexOf(QStringLiteral("--end-with")));
    const QString endWith = end >= 0 && end + 1 < job.arguments.size() ? job.arguments[end + 1] : QString();
    auto fail = [error](const QString &message) {
        if (error) *error = message;
        return QStringList();
    };
    if (step == QLatin1String("tiles")) {
        // ODM's last stage builds them (Obj2Tiles for the model, entwine for the cloud); only it runs again.
        if (!endWith.isEmpty()) return fail(QStringLiteral("Сначала доведите задание до 3D-модели"));
        QStringList args = job.arguments;
        if (!args.contains(QStringLiteral("--3d-tiles"))) args << QStringLiteral("--3d-tiles");
        return resumeArguments(args, QStringLiteral("odm_postprocess"));
    }
    if (step == QLatin1String("dense")) {
        // An aligned job goes on to the depth maps and the dense cloud (OpenMVS) and stops there;
        // the alignment is not computed again.
        if (endWith != QLatin1String("opensfm")) return fail(QStringLiteral("Нужно задание «Только выравнивание»"));
        return resumeArguments(job.arguments, {}) << QStringLiteral("--end-with") << QStringLiteral("openmvs");
    }
    if (step == QLatin1String("continue")) {
        // The job goes on where its outputs end (after the cameras were optimised): no stage is
        // forced, ODM computes what is missing; an alignment-only job stays one.
        QStringList args = removeOption(job.arguments, QStringLiteral("--rerun-from"), true);
        args = removeOption(args, QStringLiteral("--rerun"), true);
        return removeOption(args, QStringLiteral("--rerun-all"), false);
    }
    return fail(QStringLiteral("Неизвестный шаг «%1»").arg(step));
}

QString OdmEngine::previousStage(const QString &stage) const
{
    const QStringList ids = stageIds();
    const int i = int(ids.indexOf(stage));
    return i > 0 ? ids[i - 1] : QString();
}

bool OdmEngine::retryable(int exitCode, const QString &message) const
{
    // A crash of one of ODM's programs (PDAL, OpenMVS, …) is often not repeated; running out of
    // memory or disk space, or too few photos, is — the user has to change something first.
    for (const QString &cause : {QStringLiteral("памят"), QStringLiteral("место"), QStringLiteral("Снимков недостаточно"), QStringLiteral("антивирус")})
        if (message.contains(cause)) return false;
    if (exitCode == 0) return false;
    return true;
}

// ------------------------------------------------------------ native runtime

QString OdmEngine::nativePython(const QString &root)
{
#ifdef Q_OS_WIN
    return QDir(root).filePath(QStringLiteral("venv/Scripts/python.exe"));
#else
    return QDir(root).filePath(QStringLiteral("venv/bin/python3"));
#endif
}

QString OdmEngine::managedNativeRoot()
{
    const QString base = Platform::toolSafeRoot();
    return base.isEmpty() ? QString() : QDir(base).filePath(QStringLiteral("engines/odm-") + QLatin1String(Version));
}

QString OdmEngine::registeredNativeInstall(QString *version)
{
#ifdef Q_OS_WIN
    const QString key = QStringLiteral("\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\") + QLatin1String(NativeAppId)
                        + QStringLiteral("_is1");
    const QStringList hives = {QStringLiteral("HKEY_CURRENT_USER"), QStringLiteral("HKEY_LOCAL_MACHINE")};
    for (const QString &hive : hives) {
        QSettings reg(hive + key, QSettings::Registry64Format);
        const QString location = reg.value(QStringLiteral("InstallLocation")).toString();
        if (location.isEmpty()) continue;
        if (version) *version = reg.value(QStringLiteral("DisplayVersion")).toString();
        return QDir::cleanPath(QDir::fromNativeSeparators(location));
    }
#else
    Q_UNUSED(version)
#endif
    return {};
}

QStringList OdmEngine::nativeCandidates()
{
    QStringList roots;
    const QString home = qEnvironmentVariable("GEOMAP_ODM_HOME");
    if (!home.isEmpty()) roots << home;
    roots << QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("engines/odm"));   // part of GeoMap AI
#ifdef Q_OS_WIN
    roots << managedNativeRoot();
    roots << QDir(Platform::dataDirectory()).filePath(QStringLiteral("engines/odm-") + QLatin1String(Version));
    roots << registeredNativeInstall();
    roots << QStringLiteral("C:/ODM");   // default folder of ODM's own installer
#endif
    QStringList unique;
    for (const QString &r : roots) {
        if (r.isEmpty()) continue;
        const QString clean = QDir::cleanPath(QDir::fromNativeSeparators(r));
        bool seen = false;
        for (const QString &u : unique) seen = seen || u.compare(clean, Qt::CaseInsensitive) == 0;
        if (!seen) unique << clean;
    }
    return unique;
}

QString OdmEngine::nativeProblem(const QString &root)
{
    const QDir dir(root);
    if (!dir.exists()) return QStringLiteral("папка не найдена");
#ifdef Q_OS_WIN
    const QStringList required = {QStringLiteral("run.py"), QStringLiteral("VERSION"), QStringLiteral("venv/pyvenv.cfg"),
                                  QStringLiteral("venv/Scripts/python.exe"), QStringLiteral("SuperBuild/install/bin"),
                                  QStringLiteral("opendm"), QStringLiteral("stages")};
#else
    // A native Linux build of ODM (configure.sh install): run.sh sets up its environment.
    const QStringList required = {QStringLiteral("run.sh"), QStringLiteral("run.py"), QStringLiteral("VERSION"),
                                  QStringLiteral("SuperBuild/install"), QStringLiteral("opendm"), QStringLiteral("stages")};
#endif
    for (const QString &r : required)
        if (!QFileInfo::exists(dir.filePath(r))) return QStringLiteral("нет %1").arg(QDir::toNativeSeparators(r));
    QFile versionFile(dir.filePath(QStringLiteral("VERSION")));
    const QString version = versionFile.open(QIODevice::ReadOnly) ? QString::fromLatin1(versionFile.readAll()).trimmed() : QString();
    if (!version.startsWith(QLatin1String("3.6.")))
        return QStringLiteral("версия %1, нужна 3.6.x").arg(version.isEmpty() ? QStringLiteral("неизвестна") : version);
    return {};
}

QString OdmEngine::nativeProgram(const QString &root)
{
#ifdef Q_OS_WIN
    return nativePython(root);
#else
    return QDir(root).filePath(QStringLiteral("run.sh"));
#endif
}

QProcessEnvironment OdmEngine::nativeEnvironment(const QString &root, const QString &cacheDirectory,
                                                 const QProcessEnvironment &base)
{
#ifndef Q_OS_WIN
    // run.sh of a native Linux build sets ODM's own paths.
    Q_UNUSED(root)
    QProcessEnvironment env = base;
    env.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    if (!cacheDirectory.isEmpty()) env.insert(QStringLiteral("PYTHONPYCACHEPREFIX"), cacheDirectory);
    return env;
#endif
    // Same variables as ODM's win32env.bat, without running a batch file.
    const QString r = QDir::toNativeSeparators(QDir::cleanPath(root));
    const QString bin = r + QStringLiteral("\\SuperBuild\\install\\bin");
    QProcessEnvironment env = base;
    env.insert(QStringLiteral("GDAL_DATA"), bin + QStringLiteral("\\data\\gdal"));
    env.insert(QStringLiteral("GDAL_DRIVER_PATH"), bin + QStringLiteral("\\gdalplugins"));
    env.insert(QStringLiteral("PDAL_DRIVER_PATH"), bin);
    env.insert(QStringLiteral("PROJ_LIB"), bin + QStringLiteral("\\data\\proj"));
    env.insert(QStringLiteral("PROJ_DATA"), bin + QStringLiteral("\\data\\proj"));
    env.insert(QStringLiteral("VIRTUAL_ENV"), r + QStringLiteral("\\venv"));
    env.insert(QStringLiteral("PYTHONPATH"), r + QStringLiteral("\\venv"));
    env.insert(QStringLiteral("PYENVCFG"), r + QStringLiteral("\\venv\\pyvenv.cfg"));
    env.remove(QStringLiteral("PYTHONHOME"));
    env.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));   // live log lines
    env.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
    env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    if (!cacheDirectory.isEmpty())
        env.insert(QStringLiteral("PYTHONPYCACHEPREFIX"), QDir::toNativeSeparators(cacheDirectory));
    const QString system = base.value(QStringLiteral("SystemRoot"), QStringLiteral("C:\\Windows"));
    env.insert(QStringLiteral("PATH"), QStringList{r + QStringLiteral("\\venv\\Scripts"), bin, bin + QStringLiteral("\\opensfm\\bin"),
                                                   system + QStringLiteral("\\System32"), system}.join(QLatin1Char(';')));
    return env;
}

QStringList OdmEngine::nativeArguments(const ProcessingJob &job, const QString &root, const QString &projectPath)
{
    // Inputs GeoMap AI puts into the job folder: ground control points and a camera calibration,
    // passed through the tool-safe project path the engine works in.
    QStringList inputs;
    const QDir jobDir(QDir(projectPath).filePath(job.id));
    if (QFileInfo::exists(jobDir.filePath(QStringLiteral("gcp_list.txt"))) && !job.arguments.contains(QStringLiteral("--gcp")))
        inputs << QStringLiteral("--gcp") << QDir::toNativeSeparators(jobDir.filePath(QStringLiteral("gcp_list.txt")));
    if (QFileInfo::exists(jobDir.filePath(QStringLiteral("geomap_cameras.json"))) && !job.arguments.contains(QStringLiteral("--cameras")))
        inputs << QStringLiteral("--cameras") << QDir::toNativeSeparators(jobDir.filePath(QStringLiteral("geomap_cameras.json")));
#ifdef Q_OS_WIN
    return QStringList{QStringLiteral("-X"), QStringLiteral("utf8"), QDir::toNativeSeparators(QDir(root).filePath(QStringLiteral("run.py"))),
                       QStringLiteral("--project-path"), QDir::toNativeSeparators(projectPath), job.id}
           + job.arguments + inputs;
#else
    Q_UNUSED(root)
    return QStringList{QStringLiteral("--project-path"), projectPath, job.id} + job.arguments + inputs;   // after run.sh
#endif
}

bool OdmEngine::patchNativeVirtualEnv(const QString &root, QString *error)
{
#ifndef Q_OS_WIN
    Q_UNUSED(root)
    Q_UNUSED(error)
    return true;   // the Windows build only: venv\pyvenv.cfg holds an absolute path
#endif
    const QString r = QDir::toNativeSeparators(QDir::cleanPath(root));
    const QString scripts = r + QStringLiteral("\\venv\\Scripts");
    const QString cfgPath = QDir(root).filePath(QStringLiteral("venv/pyvenv.cfg"));
    QFile cfg(cfgPath);
    QString current;
    if (cfg.open(QIODevice::ReadOnly | QIODevice::Text)) current = QString::fromUtf8(cfg.readAll());
    cfg.close();
    static const QRegularExpression homeLine(QStringLiteral("^\\s*home\\s*=\\s*(.*?)\\s*$"), QRegularExpression::MultilineOption);
    const QRegularExpressionMatch m = homeLine.match(current);
    if (m.hasMatch() && QDir::cleanPath(QDir::fromNativeSeparators(m.captured(1))).compare(
                            QDir::cleanPath(QDir::fromNativeSeparators(scripts)), Qt::CaseInsensitive) == 0)
        return true;
    auto write = [error](const QString &path, const QString &text) {
        QFile f(path);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text) && f.write(text.toUtf8()) >= 0) return true;
        if (error) *error = QStringLiteral("Папка OpenDroneMap недоступна для записи (%1). Переместите программу в папку "
                                           "пользователя или переустановите ODM.").arg(QDir::toNativeSeparators(path));
        return false;
    };
    // What ODM's winpostinstall.bat writes.
    if (!write(cfgPath, QStringLiteral("home = %1\ninclude-system-site-packages = false\n").arg(scripts))) return false;
    const QDir cv2(QDir(root).filePath(QStringLiteral("venv/Lib/site-packages/cv2")));
    if (cv2.exists()) {
        const QString bin = r + QStringLiteral("\\SuperBuild\\install\\bin");
        if (!write(cv2.filePath(QStringLiteral("config.py")),
                   QStringLiteral("BINARIES_PATHS = [r\"%1\"] + BINARIES_PATHS\n").arg(bin)))
            return false;
        const QStringList versioned = cv2.entryList({QStringLiteral("config-3.*.py")}, QDir::Files);
        for (const QString &name : versioned) {
            const QString pythonVersion = name.mid(7, name.size() - 10);   // "config-3.12.py" -> "3.12"
            if (!write(cv2.filePath(name), QStringLiteral("PYTHON_EXTENSIONS_PATHS = [r'''%1\\venv\\lib\\site-packages\\cv2\\python-%2'''] "
                                                          "+ PYTHON_EXTENSIONS_PATHS\n").arg(r, pythonVersion)))
                return false;
        }
    }
    return true;
}

QString OdmEngine::effectiveNativeRoot(const QString &root, QString *error)
{
    // ODM's tools read their data (GDAL_DATA, PROJ_LIB, plugins) through narrow C runtime calls
    // too: an installation in "C:\Users\Андрей\Загрузки\GeoMapAI" is used through an ASCII link.
    if (Platform::isToolSafePath(QDir::toNativeSeparators(QDir::cleanPath(root)))) return root;
    const QString base = Platform::toolSafeRoot();
    if (base.isEmpty()) {
        if (error) *error = QStringLiteral("Нет папки без пробелов и нелатинских символов для ссылки на OpenDroneMap.");
        return {};
    }
    const QString link = QDir(base).filePath(QStringLiteral("engines/odm-link"));
    QString linkError;
    if (!Platform::createDirectoryLink(link, root, &linkError)) {
        if (error) *error = QStringLiteral("Не удалось создать ASCII-ссылку на OpenDroneMap: %1").arg(linkError);
        return {};
    }
    return QDir::cleanPath(link);
}

QString OdmEngine::workLinkFor(const QString &jobId) const
{
    const QString base = Platform::toolSafeRoot();
    return base.isEmpty() ? QString() : QDir(base).filePath(QStringLiteral("work/") + jobId);
}

// -------------------------------------------------------- environment checks

void OdmEngine::runTool(const QString &program, const QStringList &arguments, int timeoutMs, DoneCallback done,
                        const QProcessEnvironment &environment)
{
    auto *process = new QProcess(this);
    if (!environment.isEmpty()) process->setProcessEnvironment(environment);
    process->setProcessChannelMode(QProcess::MergedChannels);
    auto *timer = new QTimer(process);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, process, [process] { process->kill(); });
    connect(process, &QProcess::finished, this, [process, done](int code, QProcess::ExitStatus status) {
        const QByteArray out = process->readAllStandardOutput();
        process->deleteLater();
        done(status == QProcess::NormalExit ? code : -1, out);
    });
    connect(process, &QProcess::errorOccurred, this, [process, done](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;   // other errors also emit finished()
        process->deleteLater();
        done(-2, {});
    });
    process->start(program, arguments);
    timer->start(timeoutMs);
}

void OdmEngine::validateEnvironment()
{
    EnvironmentReport report;
    report.checked = true;
    report.engineVersion = QLatin1String(Version);
    QString root;
    QStringList problems;
    const QStringList candidates = m_nativeRoot.isEmpty() ? nativeCandidates() : QStringList{m_nativeRoot};
    for (const QString &c : candidates) {
        const QString problem = nativeProblem(c);
        if (problem.isEmpty()) {
            root = c;
            break;
        }
        if (QFileInfo::exists(c)) problems << QStringLiteral("%1: %2").arg(QDir::toNativeSeparators(c), problem);
    }
    if (!root.isEmpty()) {
        checkNative(report, root);
        return;
    }
    report.warnings << problems;
    report.imageMissing = true;
    report.cpus = Platform::cpuCount();
    report.memoryBytes = Platform::totalMemoryBytes();
    report.summary = QStringLiteral("Встроенный OpenDroneMap не найден");
#ifdef Q_OS_WIN
    report.problems << QStringLiteral("Встроенный OpenDroneMap не найден: в папке программы нет engines\\odm. Он входит в поставку "
                                      "GeoMap AI; проекты, импорт, карта и просмотр работают и без него.");
    QString otherVersion;
    const QString other = registeredNativeInstall(&otherVersion);
    if (!other.isEmpty()) {
        report.problems << QStringLiteral("Найдена отдельная установка ODM %1 в %2, но она не подходит. Установщик ODM при "
                                          "установке удаляет другие версии, поэтому GeoMap AI её не трогает: обновите её до "
                                          "3.6.2 или удалите сами (Параметры Windows → Приложения), затем «Проверить снова».")
                               .arg(otherVersion, QDir::toNativeSeparators(other));
    } else {
        report.canInstall = true;
        report.installTitle = QStringLiteral("Восстановить OpenDroneMap");
        report.installPrompt = nativeInstallPrompt();
        report.hints << QStringLiteral("Переустановите GeoMap AI из полной поставки или нажмите «Восстановить OpenDroneMap».");
    }
#else
    report.problems << QStringLiteral("OpenDroneMap для Linux не найден. Официальной сборки для Linux нет: соберите ODM 3.6 "
                                      "(configure.sh install) и укажите его папку в переменной GEOMAP_ODM_HOME. "
                                      "Проекты, импорт, карта и просмотр работают и без него.");
#endif
    finishEnvironment(report);
}
void OdmEngine::checkNative(EnvironmentReport report, const QString &installation)
{
    QString error;
    const QString root = effectiveNativeRoot(installation, &error);
    if (root.isEmpty() || !patchNativeVirtualEnv(root, &error)) {
        report.problems << error;
        report.summary = QStringLiteral("OpenDroneMap не готов");
        finishEnvironment(report);
        return;
    }
    const QString cache = QDir(Platform::dataDirectory()).filePath(QStringLiteral("cache/odm-pycache"));
    const QProcessEnvironment env = nativeEnvironment(root, cache, QProcessEnvironment::systemEnvironment());
#ifdef Q_OS_WIN
    // Importing OpenCV and GDAL through the bundled Python proves the runtime, its DLL paths and
    // the venv configuration in under a second; ODM's own self-check (run.py --version) takes
    // ~15 s and runs after installation instead.
    const QString checkProgram = nativePython(root);
    const QStringList checkArguments{QStringLiteral("-X"), QStringLiteral("utf8"), QStringLiteral("-c"),
                                     QStringLiteral("import sys, numpy, cv2; from osgeo import gdal; print(sys.prefix)")};
#else
    const QString checkProgram = nativeProgram(root);
    const QStringList checkArguments{QStringLiteral("--version")};
#endif
    runTool(checkProgram, checkArguments, 120000, [this, report, root](int code, const QByteArray &out) mutable {
        if (code != 0) {
            report.problems << QStringLiteral("OpenDroneMap найден в %1, но не запускается (код %2): %3")
                                   .arg(QDir::toNativeSeparators(root)).arg(code)
                                   .arg(QString::fromUtf8(out).trimmed().right(300));
            report.hints << QStringLiteral("Переустановите OpenDroneMap.");
            report.summary = QStringLiteral("OpenDroneMap не запускается");
            finishEnvironment(report);
            return;
        }
        auto finish = [this, report, root](const QString &gpu) mutable {
            m_nativeRoot = root;
            report.backend = Native;
            report.gpuName = gpu;
            report.cpus = Platform::cpuCount();
            report.memoryBytes = Platform::totalMemoryBytes();
            if (report.memoryBytes > 0 && report.memoryBytes < 8 * GiB)
                report.warnings << QStringLiteral("Оперативной памяти %1 ГБ. ODM требует около 1 ГБ на поток; большие наборы снимков "
                                                  "могут не поместиться.").arg(gigabytes(report.memoryBytes));
            report.available = true;
            report.summary = QStringLiteral("Встроенный OpenDroneMap %1 · %2 потоков, %3 ГБ ОЗУ%4")
                                 .arg(QLatin1String(Version)).arg(report.cpus).arg(gigabytes(report.memoryBytes))
                                 .arg(gpu.isEmpty() ? QString() : QStringLiteral(" · GPU: ") + gpu);
            finishEnvironment(report);
        };
#ifdef Q_OS_WIN
        const QString smi = QDir(qEnvironmentVariable("SystemRoot", QStringLiteral("C:\\Windows"))).filePath(QStringLiteral("System32/nvidia-smi.exe"));
        if (QFileInfo(smi).isFile()) {
            runTool(smi, {QStringLiteral("--query-gpu=name"), QStringLiteral("--format=csv,noheader")}, 15000,
                    [finish](int smiCode, const QByteArray &names) mutable {
                finish(smiCode == 0 ? QString::fromUtf8(names).split(QLatin1Char('\n')).constFirst().trimmed() : QString());
            });
            return;
        }
#endif
        finish({});
    }, env);
}

void OdmEngine::finishEnvironment(EnvironmentReport report)
{
    m_backend = report.backend;
    m_canInstall = report.canInstall;
    m_lastReport = report;
    emit environmentChecked(report);
}
QString OdmEngine::nativeInstallPrompt()
{
    return QStringLiteral(
        "OpenDroneMap входит в полную поставку GeoMap AI. Чтобы восстановить его, GeoMap AI загрузит официальную сборку "
        "OpenDroneMap %1 для Windows с GitHub (%2 МБ, подпись OpenDroneMap, контрольная сумма проверяется) и развернёт её "
        "для текущего пользователя без прав администратора в папку\n%3\n(около 1 ГБ).\n\n"
        "OpenDroneMap распространяется по лицензии AGPL-3.0 как отдельная программа в составе GeoMap AI. "
        "Установщик ODM добавит запись «ODM» в список приложений Windows.")
        .arg(QLatin1String(Version), megabytes(NativeInstallerBytes), QDir::toNativeSeparators(managedNativeRoot()));
}

// --------------------------------------------------------------- installation

void OdmEngine::installRuntime()
{
    if (!m_canInstall) {
        emit installFinished(false, QStringLiteral("Восстанавливать нечего: сначала выполните проверку окружения."));
        return;
    }
    installNative();
}
void OdmEngine::installNative()
{
#ifndef Q_OS_WIN
    finishInstall(false, QStringLiteral("Для Linux укажите собственную сборку ODM в GEOMAP_ODM_HOME."));
#else
    if (m_download || m_installer) return;
    QString otherVersion;
    const QString other = registeredNativeInstall(&otherVersion);
    const QString target = managedNativeRoot();
    if (target.isEmpty()) {
        finishInstall(false, QStringLiteral("Не найдена папка для установки без пробелов и нелатинских символов."));
        return;
    }
    if (!other.isEmpty() && other.compare(QDir::cleanPath(target), Qt::CaseInsensitive) != 0) {
        finishInstall(false, QStringLiteral("Найдена отдельная установка ODM %1 в %2. Установщик ODM удаляет её при установке, "
                                            "поэтому GeoMap AI не будет его запускать. Обновите её до 3.6.2 или удалите сами.")
                                 .arg(otherVersion, QDir::toNativeSeparators(other)));
        return;
    }
    if (nativeProblem(target).isEmpty()) {   // already there (e.g. installed before, not yet verified)
        verifyNativeInstall(target);
        return;
    }
    m_installCancelled = false;
    const QString installer = QDir(Platform::toolSafeRoot()).filePath(QStringLiteral("downloads/ODM_Setup_") + QLatin1String(Version)
                                                                      + QStringLiteral(".exe"));
    m_download = new Downloader(this);
    connect(m_download, &Downloader::progress, this, [this](qint64 received, qint64 total) {
        const qint64 all = total > 0 ? total : NativeInstallerBytes;
        emit installProgress(QStringLiteral("Загрузка OpenDroneMap: %1 из %2 МБ").arg(megabytes(received), megabytes(all)));
    });
    connect(m_download, &Downloader::finished, this, [this](bool ok, const QString &message) {
        m_download->deleteLater();
        m_download = nullptr;
        if (!ok) {
            finishInstall(false, message);
            return;
        }
        runNativeInstaller(message);
    });
    emit installProgress(QStringLiteral("Загрузка OpenDroneMap…"));
    m_download->start(QUrl(QString::fromLatin1(NativeInstallerUrl)), installer, QByteArray(NativeInstallerSha256));
#endif
}

void OdmEngine::runNativeInstaller(const QString &installer)
{
#ifdef Q_OS_WIN
    if (m_installCancelled) {
        finishInstall(false, QStringLiteral("Установка отменена."));
        return;
    }
    const QString target = QDir::toNativeSeparators(managedNativeRoot());
    const QString logDir = QDir(Platform::dataDirectory()).filePath(QStringLiteral("logs"));
    QDir().mkpath(logDir);
    m_installer = new QProcess(this);
    m_installer->setProgram(installer);
    // Inno Setup parses its own command line: quote the values, not the whole switch.
    m_installer->setNativeArguments(QStringLiteral("/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /CURRENTUSER /NOICONS "
                                                   "/DIR=\"%1\" /LOG=\"%2\"")
                                        .arg(target, QDir::toNativeSeparators(QDir(logDir).filePath(QStringLiteral("odm-install.log")))));
    connect(m_installer, &QProcess::finished, this, [this, installer](int code, QProcess::ExitStatus status) {
        m_installer->deleteLater();
        m_installer = nullptr;
        if (status != QProcess::NormalExit || code != 0) {
            finishInstall(false, QStringLiteral("Установщик OpenDroneMap завершился с кодом %1. Журнал: %2")
                                     .arg(code).arg(QDir::toNativeSeparators(QDir(Platform::dataDirectory()).filePath(QStringLiteral("logs/odm-install.log")))));
            return;
        }
        QFile::remove(installer);
        const QString root = managedNativeRoot();
        // ODM's binaries need the VC++ runtime; its installer only adds it with admin rights.
        // GeoMap AI ships the same runtime next to its EXE: copy what is missing.
        const QDir appDir(QCoreApplication::applicationDirPath());
        const QDir bin(QDir(root).filePath(QStringLiteral("SuperBuild/install/bin")));
        for (const QString &dll : appDir.entryList({QStringLiteral("msvcp140*.dll"), QStringLiteral("vcruntime140*.dll"),
                                                    QStringLiteral("concrt140.dll"), QStringLiteral("vccorlib140.dll")}, QDir::Files))
            if (!QFileInfo::exists(bin.filePath(dll))) QFile::copy(appDir.filePath(dll), bin.filePath(dll));
        verifyNativeInstall(root);
    });
    connect(m_installer, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart || !m_installer) return;
        m_installer->deleteLater();
        m_installer = nullptr;
        finishInstall(false, QStringLiteral("Не удалось запустить установщик OpenDroneMap."));
    });
    emit installProgress(QStringLiteral("Установка OpenDroneMap (около минуты)…"));
    m_installer->start();
#else
    Q_UNUSED(installer)
#endif
}

void OdmEngine::verifyNativeInstall(const QString &root)
{
    const QString problem = nativeProblem(root);
    QString error;
    if (!problem.isEmpty() || !patchNativeVirtualEnv(root, &error)) {
        finishInstall(false, QStringLiteral("OpenDroneMap установлен не полностью: %1").arg(problem.isEmpty() ? error : problem));
        return;
    }
    emit installProgress(QStringLiteral("Проверка OpenDroneMap…"));
    const QString cache = QDir(Platform::dataDirectory()).filePath(QStringLiteral("cache/odm-pycache"));
    runTool(nativePython(root), {QStringLiteral("-X"), QStringLiteral("utf8"), QDir::toNativeSeparators(QDir(root).filePath(QStringLiteral("run.py"))),
                                 QStringLiteral("--version")},
            300000, [this, root](int code, const QByteArray &out) {
        const QString text = QString::fromUtf8(out).trimmed();
        if (code == 0 && text.contains(QLatin1String("ODM 3.6"))) {
            m_nativeRoot = root;
            finishInstall(true, QStringLiteral("OpenDroneMap %1 установлен: %2").arg(QLatin1String(Version), QDir::toNativeSeparators(root)));
        } else {
            finishInstall(false, QStringLiteral("OpenDroneMap установлен, но самопроверка не прошла (код %1): %2").arg(code).arg(text.right(400)));
        }
    }, nativeEnvironment(root, cache, QProcessEnvironment::systemEnvironment()));
}

void OdmEngine::cancelInstall()
{
    if (m_download) {
        m_installCancelled = true;
        m_download->cancel();
    } else if (m_installer) {
        m_installCancelled = true;
        emit installProgress(QStringLiteral("Установщик уже работает; дождитесь окончания, отменить его нельзя."));
    }
}

void OdmEngine::finishInstall(bool ok, const QString &message)
{
    emit installFinished(ok, message);
}

// ------------------------------------------------------------------- jobs

bool OdmEngine::start(const ProcessingJob &job, const QString &jobDirectory, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error) *error = message;
        return false;
    };
    if (m_run) return fail(QStringLiteral("Движок уже выполняет задание %1").arg(m_runningJob));
    if (!job.backend.isEmpty() && job.backend != Native)
        return fail(QStringLiteral("Задание выполнялось в Docker (GeoMap AI 0.2.0); Docker больше не используется. "
                                   "Запустите новую обработку."));
    m_runningJob = job.id;
    m_cancelRequested = false;
    m_partialLine.clear();
    m_stage.clear();
    m_stageIndex = 0;
    m_recent.clear();
    m_errors.clear();
    m_workLink.clear();

    QString program;
    QStringList arguments;
    {
        QString root = m_nativeRoot;
        if (root.isEmpty() || !nativeProblem(root).isEmpty()) {
            root.clear();
            for (const QString &c : nativeCandidates()) if (nativeProblem(c).isEmpty()) { root = c; break; }
        }
        if (root.isEmpty()) return fail(QStringLiteral("Встроенный OpenDroneMap не найден. Проверьте окружение."));
        QString rootError;
        root = effectiveNativeRoot(root, &rootError);
        if (root.isEmpty()) return fail(rootError);
        // Parts of ODM (OpenSfM, PDAL, OpenMVS) open files through narrow C runtime calls and
        // fail on spaces or non-Latin characters: give them an ASCII path to the job folder.
        // First the short (8.3) names of the same folders; a directory link only when there are
        // none: through a link, antivirus file filters (seen with Malwarebytes) make ODM's
        // renames fail ("path not found"), and the point classification of the high-quality
        // profile loses its point cloud.
        const QString dir = QDir(jobDirectory).absolutePath();
        QString projectPath = QFileInfo(dir).absolutePath();
        if (!Platform::isToolSafePath(QDir::toNativeSeparators(dir))) {
            const QString shortParent = Platform::shortPath(projectPath);
            if (Platform::isToolSafePath(QDir::toNativeSeparators(shortParent)) && Platform::isToolSafePath(job.id)) {
                projectPath = shortParent;
                emit logLine(job.id, QStringLiteral("[GeoMap] В пути к заданию есть пробелы или не латинские буквы: ODM работает с ним "
                                                    "через короткие имена папок %1").arg(QDir::toNativeSeparators(shortParent)));
            } else {
                const QString link = workLinkFor(job.id);
                QString linkError;
                if (link.isEmpty() || !Platform::createDirectoryLink(link, dir, &linkError))
                    return fail(QStringLiteral("Не удалось подготовить путь к папке задания для ODM: %1").arg(linkError));
                m_workLink = link;
                projectPath = QFileInfo(link).absolutePath();
                emit logLine(job.id, QStringLiteral("[GeoMap] В пути к заданию есть пробелы или не латинские буквы, а коротких имён "
                                                    "папок нет: ODM работает через ссылку %1").arg(QDir::toNativeSeparators(link)));
            }
        }
        program = nativeProgram(root);
        arguments = nativeArguments(job, root, projectPath);
        m_run = new QProcess(this);
        const QString cache = QDir(Platform::dataDirectory()).filePath(QStringLiteral("cache/odm-pycache"));
        m_run->setProcessEnvironment(nativeEnvironment(root, cache, QProcessEnvironment::systemEnvironment()));
        m_run->setWorkingDirectory(root);
    }
    m_run->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_run, &QProcess::readyReadStandardOutput, this, &OdmEngine::handleOutput);
    connect(m_run, &QProcess::finished, this, &OdmEngine::handleRunFinished);
    connect(m_run, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart || !m_run) return;
        const QString id = m_runningJob;
        const QString message = QStringLiteral("Не удалось запустить OpenDroneMap: %1").arg(m_run->errorString());
        m_run->deleteLater();
        m_run = nullptr;
        releaseRun();
        m_runningJob.clear();
        emit finished(id, JobOutcome::Failed, -1, message);
    });
    emit logLine(job.id, QStringLiteral("$ ") + QDir::toNativeSeparators(program) + QLatin1Char(' ') + arguments.join(QLatin1Char(' ')));
    ProcessTree::prepare(*m_run);
    m_run->start(program, arguments);
    if (m_run && m_run->processId() > 0) {
        m_tree = std::make_unique<ProcessTree>();
        if (!m_tree->attach(*m_run))
            emit logLine(job.id, QStringLiteral("[GeoMap] Предупреждение: дочерние процессы ODM не привязаны к заданию; "
                                                "при отмене они могут завершиться не сразу."));
    }
    return true;
}

void OdmEngine::handleOutput()
{
    if (!m_run) return;
    QByteArray data = m_partialLine + m_run->readAllStandardOutput();
    // ODM and PDAL also use '\r' for progress; treat it as a line break.
    data.replace('\r', '\n');
    const int last = data.lastIndexOf('\n');
    m_partialLine = last >= 0 ? data.mid(last + 1) : data;
    if (last < 0) return;
    const QList<QByteArray> lines = data.left(last).split('\n');
    for (const QByteArray &raw : lines) {
        const QString line = stripAnsi(QString::fromUtf8(raw)).trimmed();
        if (line.isEmpty()) continue;
        m_recent.append(line);
        if (m_recent.size() > RecentLines) m_recent.removeFirst();
        if (line.contains(QLatin1String("ERROR")) || line.startsWith(QLatin1String("Traceback"))
            || line.contains(QLatin1String("Exception")) || line.contains(QLatin1String("MemoryError"))
            || line.contains(QLatin1String("bad_alloc"))) {
            m_errors.append(line);
            if (m_errors.size() > 20) m_errors.removeFirst();
        }
        const QString stage = parseStage(line);
        if (!stage.isEmpty() && stage != m_stage) {
            m_stage = stage;
            m_stageIndex = int(stageIds().indexOf(stage)) + 1;
            emit stageChanged(m_runningJob, m_stage, m_stageIndex, int(stageIds().size()));
        }
        emit logLine(m_runningJob, line);
    }
}

QString OdmEngine::explainFailure(const QStringList &recent, const QStringList &errors, const QString &stage, int exitCode)
{
    // The program ODM ran last ("[INFO]    running pdal translate …") is the one that failed.
    QString command;
    for (int i = int(recent.size()) - 1; i >= 0 && command.isEmpty(); --i) {
        const int at = recent[i].indexOf(QLatin1String("running "));
        if (at >= 0 && recent[i].contains(QLatin1String("INFO"))) command = recent[i].mid(at + 8).trimmed();
    }
    QString tool = command.section(QLatin1Char(' '), 0, 0).remove(QLatin1Char('"'));
    tool = QFileInfo(tool).completeBaseName();
    const QString all = (errors + recent.mid(qMax(0, int(recent.size()) - 30))).join(QLatin1Char('\n'));
    const QString where = stage.isEmpty() ? QString() : QStringLiteral(" на этапе «%1»").arg(stage);
    if (all.contains(QLatin1String("No space left"), Qt::CaseInsensitive) || all.contains(QLatin1String("Errno 28")))
        return QStringLiteral("Обработка остановлена%1: на диске кончилось место. Освободите место и нажмите «Продолжить».").arg(where);
    if (all.contains(QLatin1String("Not enough")) && all.contains(QLatin1String("images"), Qt::CaseInsensitive))
        return QStringLiteral("Снимков недостаточно для реконструкции%1: нужно больше снимков с перекрытием 70–80 %.").arg(where);
    // ODM replaces a file by renaming another one ("[WinError 3] … 'a' -> 'b'"); when Windows or
    // an antivirus filter refuses, the file is gone and the next program cannot open it.
    const QString wide = (errors + recent).join(QLatin1Char('\n'));
    static const QRegularExpression renameFailed(QStringLiteral("\\[WinError (2|3|5|32)\\][^\\n]*' -> '"));
    if (wide.contains(renameFailed))
        return QStringLiteral("Сбой%1: Windows или антивирус (например, Malwarebytes) не дали OpenDroneMap переименовать файл "
                              "в папке задания, и он пропал. Добавьте папку проекта в исключения антивируса или храните проект "
                              "в папке с латинскими буквами в пути без пробелов, затем нажмите «Продолжить».").arg(where);
    QString what;
    if (tool == QLatin1String("pdal")) {
        what = command.contains(QLatin1String("smrf")) || command.contains(QLatin1String("classify"))
                   ? QStringLiteral("PDAL при классификации точек земли (нужна для DTM)")
                   : command.contains(QLatin1String("writers.gdal")) || command.contains(QLatin1String("dem"))
                         ? QStringLiteral("PDAL при построении модели высот")
                         : QStringLiteral("PDAL при обработке облака точек");
    } else if (tool.contains(QLatin1String("Densify"), Qt::CaseInsensitive) || tool.contains(QLatin1String("OpenMVS"), Qt::CaseInsensitive)) {
        what = QStringLiteral("OpenMVS при построении плотного облака (чаще всего — нехватка памяти)");
    } else if (tool.contains(QLatin1String("texrecon"), Qt::CaseInsensitive)) {
        what = QStringLiteral("текстурирование модели");
    } else if (tool.contains(QLatin1String("orthophoto"), Qt::CaseInsensitive)) {
        what = QStringLiteral("построение ортофотоплана ODM");
    } else if (!tool.isEmpty()) {
        what = QStringLiteral("программа %1").arg(tool);
    }
    const bool crash = all.contains(QLatin1String("strange values")) || exitCode == 139 || exitCode == 134 || exitCode == -1073741819
                       || quint32(exitCode) == 3221225477u;
    QString text = what.isEmpty() ? QStringLiteral("OpenDroneMap остановился с ошибкой%1 (код %2).").arg(where).arg(exitCode)
                                  : QStringLiteral("Сбой%1: %2%3.").arg(where, what, crash ? QStringLiteral(" завершилась аварийно") : QString());
    if (what.contains(QLatin1String("DTM")))
        text += QStringLiteral(" Нажмите «Продолжить» — повтор с этого этапа обычно проходит; если ошибка повторится, запустите обработку без DTM.");
    else if (what.contains(QStringLiteral("памят")))
        text += QStringLiteral(" Уменьшите качество облака или число потоков и нажмите «Продолжить».");
    else
        text += QStringLiteral(" Нажмите «Продолжить», чтобы повторить с этого этапа; подробности — во вкладке «Журнал».");
    return text;
}

void OdmEngine::handleRunFinished(int exitCode, QProcess::ExitStatus status)
{
    if (!m_run) return;
    if (!m_partialLine.isEmpty()) {
        m_partialLine.append('\n');
        handleOutput();
    }
    const QString id = m_runningJob;
    JobOutcome outcome = JobOutcome::Failed;
    QString message;
    const bool memory = std::any_of(m_errors.cbegin(), m_errors.cend(), [](const QString &l) {
        return l.contains(QLatin1String("MemoryError")) || l.contains(QLatin1String("bad_alloc"));
    });
    if (m_cancelRequested) {
        outcome = JobOutcome::Cancelled;
        message = QStringLiteral("Обработка отменена пользователем.");
    } else if (status == QProcess::NormalExit && exitCode == 0) {
        outcome = JobOutcome::Succeeded;
        message = QStringLiteral("OpenDroneMap завершил обработку.");
    } else if (exitCode == 137 || memory) {
        message = QStringLiteral("OpenDroneMap остановлен из-за нехватки памяти. Попробуйте профиль попроще, меньше потоков "
                                 "(«Ресурсы» → «Потоков») или разбиение на подмодели.");
    } else {
        message = explainFailure(m_recent, m_errors, stageTitle(m_stage), exitCode);
    }
    m_run->deleteLater();
    m_run = nullptr;
    releaseRun();
    m_runningJob.clear();
    emit finished(id, outcome, exitCode, message.trimmed());
}

void OdmEngine::releaseRun()
{
    m_tree.reset();   // kill-on-close ends anything the job left behind
    if (!m_workLink.isEmpty()) {
        Platform::removeDirectoryLink(m_workLink);
        m_workLink.clear();
    }
}

void OdmEngine::cancel(const QString &jobId)
{
    if (!m_run || jobId != m_runningJob || m_cancelRequested) return;
    m_cancelRequested = true;
    if (m_tree) m_tree->terminate();
    else m_run->kill();
    // finished() of the process completes the cancellation.
}

void OdmEngine::abort(const QString &jobId)
{
    if (!m_run || jobId != m_runningJob) return;
    m_run->disconnect(this);
    if (m_tree) m_tree->terminate();
    m_run->kill();
    m_run->waitForFinished(5000);
    delete m_run;
    releaseRun();
    m_runningJob.clear();
}
bool OdmEngine::isRunning(const QString &jobId) const
{
    return m_run && m_runningJob == jobId;
}

JobProgress OdmEngine::readProgress(const QString &jobId) const
{
    JobProgress p;
    if (jobId != m_runningJob) return p;
    p.stage = m_stage;
    p.stageIndex = m_stageIndex;
    p.stageCount = int(stageIds().size());
    p.recentLog = m_recent.mid(qMax(0, int(m_recent.size()) - 50));
    return p;
}

QVector<Artifact> OdmEngine::collectOutputs(const QString &jobDirectory) const
{
    struct Known { const char *kind; const char *file; };
    static const Known known[] = {
        {"orthophoto", "geomap/true_orthophoto.tif"},   // GeoMap AI's true orthophoto first: the project's orthophoto
        {"orthophoto", "odm_orthophoto/odm_orthophoto.tif"},
        {"dsm", "odm_dem/dsm.tif"},
        {"dtm", "odm_dem/dtm.tif"},
        {"pointcloud", "odm_georeferencing/odm_georeferenced_model.laz"},
        {"pointcloud", "odm_georeferencing/odm_georeferenced_model.las"},
        {"pointcloud", "opensfm/undistorted/openmvs/scene_dense_dense_filtered.ply"},   // dense cloud of an aligned job (depth maps)
        {"mesh", "odm_texturing/odm_textured_model_geo.obj"},
        {"mesh", "odm_texturing/odm_textured_model_geo.glb"},
        {"mesh", "odm_texturing_25d/odm_textured_model_geo.obj"},
        {"mesh", "geomap/texture/odm_textured_model_geo.obj"},   // the texture edited in GeoMap AI
        {"report", "odm_report/report.pdf"},
        {"report", "geomap/report.pdf"},
        {"tiles", "3d_tiles/model/tileset.json"},
        {"tiles", "3d_tiles/pointcloud/tileset.json"},
    };
    QVector<Artifact> outputs;
    const QDir dir(jobDirectory);
    for (const Known &k : known) {
        const QString file = QLatin1String(k.file);
        const QFileInfo info(dir.filePath(file));
        if (!info.isFile() || info.size() == 0) continue;
        Artifact a;
        a.kind = QLatin1String(k.kind);
        a.file = file;
        a.bytes = info.size();
        if (file.startsWith(QLatin1String("geomap/")) && file.endsWith(QLatin1String(".tif"))) {
            a.details = QStringLiteral("истинный (GeoMap AI), ") + GeoTiffInfo::read(info.filePath()).describe();
        } else if (file.endsWith(QLatin1String(".tif"))) {
            a.details = GeoTiffInfo::read(info.filePath()).describe();
            if (a.kind == QLatin1String("orthophoto")) a.details = QStringLiteral("ODM, ") + a.details;
        } else if (file.endsWith(QLatin1String(".laz")) || file.endsWith(QLatin1String(".las"))) {
            a.details = describeLas(info.filePath());
        } else if (file.endsWith(QLatin1String("tileset.json"))) {
            // The whole folder is the result: tileset.json and its tiles.
            qint64 bytes = 0;
            int count = 0;
            QDirIterator it(info.absolutePath(), QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                it.next();
                bytes += it.fileInfo().size();
                ++count;
            }
            a.bytes = bytes;
            a.details = QStringLiteral("OGC 3D Tiles, %1: %2 файлов").arg(file.contains(QLatin1String("pointcloud")) ? QStringLiteral("облако") : QStringLiteral("модель")).arg(count);
        } else if (file.endsWith(QLatin1String(".ply"))) {
            a.details = QStringLiteral("PLY, плотное облако OpenMVS");
        } else if (file.endsWith(QLatin1String(".pdf"))) {
            QFile f(info.filePath());
            a.details = f.open(QIODevice::ReadOnly) && f.read(4) == "%PDF" ? (file.startsWith(QLatin1String("geomap/")) ? QStringLiteral("PDF-отчёт GeoMap AI") : QStringLiteral("PDF-отчёт ODM"))
                                                                            : QStringLiteral("повреждён: не PDF");
        } else if (file.endsWith(QLatin1String(".glb"))) {
            QFile f(info.filePath());
            a.details = f.open(QIODevice::ReadOnly) && f.read(4) == "glTF" ? QStringLiteral("glTF (GLB), 3D-модель с текстурами")
                                                                            : QStringLiteral("повреждён: не GLB");
        } else if (file.startsWith(QLatin1String("geomap/texture/"))) {
            a.details = QStringLiteral("OBJ, текстура изменена в GeoMap AI");
        } else if (file.contains(QLatin1String("_25d"))) {
            a.details = QStringLiteral("OBJ, 2.5D-сетка с текстурами");
        } else {
            a.details = QStringLiteral("OBJ, 3D-сетка с текстурами");
        }
        outputs.append(a);
    }
    return outputs;
}

QVariantMap OdmEngine::parseQualityStats(const QByteArray &statsJson)
{
    const QJsonObject s = QJsonDocument::fromJson(statsJson).object();
    if (s.isEmpty()) return {};
    const QJsonObject rec = s.value("reconstruction_statistics").toObject();
    QVariantList rows;
    QStringList warnings;
    auto row = [&rows](const QString &label, const QString &value) { rows << QVariantMap{{"label", label}, {"value", value}}; };
    auto num = [](double v, int decimals) { return ru().toString(v, 'f', decimals); };

    const int aligned = rec.value("reconstructed_shots_count").toInt(), total = rec.value("initial_shots_count").toInt();
    if (total > 0) {
        row(QStringLiteral("Выровнено снимков"), QStringLiteral("%1 из %2").arg(aligned).arg(total));
        if (aligned < total)
            warnings << QStringLiteral("Не выровнено снимков: %1. Обычно причина — слабое перекрытие, смаз или однородная "
                                       "поверхность (вода, снег, поле).").arg(total - aligned);
    }
    const int components = rec.value("components").toInt();
    if (components > 1)
        warnings << QStringLiteral("Реконструкция состоит из %1 несвязанных частей: между группами снимков мало перекрытия.").arg(components);
    // Feature detection and matching: points found and used per photo.
    const QJsonObject features = s.value("features_statistics").toObject();
    const int detected = features.value("detected_features").toObject().value("median").toInt();
    const int used = features.value("reconstructed_features").toObject().value("median").toInt();
    if (detected > 0)
        row(QStringLiteral("Точек на снимке (медиана)"), QStringLiteral("найдено %1, сопоставлено %2").arg(ru().toString(detected), ru().toString(used)));
    if (rec.contains("reconstructed_points_count"))
        row(QStringLiteral("Связующих точек"), ru().toString(rec.value("reconstructed_points_count").toInt()));
    if (rec.contains("reprojection_error_pixels"))
        row(QStringLiteral("Ошибка репроекции (RMS)"), num(rec.value("reprojection_error_pixels").toDouble(), 2) + QStringLiteral(" пикс"));
    if (rec.contains("average_track_length"))
        row(QStringLiteral("Средняя длина трека"), num(rec.value("average_track_length").toDouble(), 2) + QStringLiteral(" снимка"));

    const QJsonObject odm = s.value("odm_processing_statistics").toObject();
    if (odm.contains("average_gsd")) row(QStringLiteral("Средний GSD"), num(odm.value("average_gsd").toDouble(), 2) + QStringLiteral(" см/пикс"));
    const double area = s.value("processing_statistics").toObject().value("area").toDouble();
    if (area > 0) row(QStringLiteral("Площадь съёмки"), area >= 10000 ? num(area / 10000, 2) + QStringLiteral(" га")
                                                                     : num(area, 0) + QStringLiteral(" м²"));

    auto errors = [&](const char *key, const QString &title) {
        const QJsonObject e = s.value(QLatin1String(key)).toObject();
        const QJsonObject rmse = e.value("error").toObject();
        if (rmse.isEmpty()) return;
        row(title + QStringLiteral(", RMSE X / Y / Z"),
            QStringLiteral("%1 / %2 / %3 м").arg(num(rmse.value("x").toDouble(), 3), num(rmse.value("y").toDouble(), 3),
                                               num(rmse.value("z").toDouble(), 3)));
        if (e.contains("ce90") && e.contains("le90"))
            row(title + QStringLiteral(", CE90 / LE90"),
                QStringLiteral("%1 / %2 м").arg(num(e.value("ce90").toDouble(), 3), num(e.value("le90").toDouble(), 3)));
    };
    if (rec.value("has_gps").toBool()) errors("gps_errors", QStringLiteral("Расхождение с GPS"));
    if (rec.value("has_gcp").toBool()) errors("gcp_errors", QStringLiteral("Ошибка по опорным точкам"));

    const QJsonObject cloud = s.value("point_cloud_statistics").toObject();
    const QJsonArray stat = cloud.value("stats").toObject().value("statistic").toArray();
    if (!stat.isEmpty())
        row(cloud.value("dense").toBool() ? QStringLiteral("Точек в плотном облаке") : QStringLiteral("Точек в облаке (разреженном)"),
            ru().toString(qint64(stat.first().toObject().value("count").toDouble())));

    const QJsonObject cameras = s.value("camera_errors").toObject();
    if (!cameras.isEmpty()) {
        const QJsonObject cam = cameras.begin().value().toObject();
        const QJsonObject init = cam.value("initial_values").toObject(), opt = cam.value("optimized_values").toObject();
        if (opt.contains("focal"))
            row(QStringLiteral("Фокус (доля ширины кадра): исходный → уточнённый"),
                QStringLiteral("%1 → %2").arg(num(init.value("focal").toDouble(), 4), num(opt.value("focal").toDouble(), 4)));
    }
    QVariantList steps;
    const QJsonObject times = s.value("processing_statistics").toObject().value("steps_times").toObject();
    for (auto it = times.begin(); it != times.end(); ++it)
        if (it.key() != QLatin1String("Total Time")) steps << QVariantMap{{"title", it.key()}, {"seconds", it.value().toDouble()}};
    return {{"rows", rows}, {"warnings", warnings}, {"steps", steps},
            {"alignedImages", aligned}, {"totalImages", total},
            {"reprojectionPx", rec.value("reprojection_error_pixels").toDouble()},
            {"gsdCm", odm.value("average_gsd").toDouble()}, {"areaM2", area}};
}

QVariantMap OdmEngine::qualityReport(const QString &jobDirectory) const
{
    // The full report after odm_report; OpenSfM's statistics for alignment-only jobs.
    for (const char *file : {"odm_report/stats.json", "opensfm/stats/stats.json"}) {
        QFile f(QDir(jobDirectory).filePath(QLatin1String(file)));
        if (f.size() > 64 * MiB || !f.open(QIODevice::ReadOnly)) continue;
        QVariantMap report = parseQualityStats(f.readAll());
        if (report.isEmpty()) continue;
        report.insert(QStringLiteral("source"), QLatin1String(file));
        return report;
    }
    return {};
}

void OdmEngine::cleanupInterrupted(const ProcessingJob &job)
{
    // The process tree died with GeoMap AI (kill-on-close); only the work link may remain.
    const QString link = workLinkFor(job.id);
    if (!link.isEmpty()) Platform::removeDirectoryLink(link);
}