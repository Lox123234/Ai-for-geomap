# Зависимости и лицензии

Правило проекта: **перед добавлением любой зависимости** проверить её лицензию, файлы NOTICE, условия сборки и распространения и записать результат в этот документ. Источник данных о Qt ниже — SBOM-файлы установленного комплекта (`<Qt>/6.8.3/msvc2022_64/sbom/*.spdx.json`), а не память или предположения.

Собственный код GeoMap AI распространяется под MIT (`LICENSE`). Это не меняет лицензий сторонних компонентов в поставке: объединённый дистрибутив нельзя маркировать как «полностью MIT».

## 1. Компоненты в поставке (`dist/GeoMapAI`)

Qt подключается **динамически** (DLL рядом с EXE), что позволяет пользователю заменить библиотеки Qt — одно из условий LGPL.

| Компонент | Версия | Лицензия (по SBOM) | Как попадает в поставку |
| --- | --- | --- | --- |
| Qt Core, Gui, Network, OpenGL | 6.8.3 | LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only OR коммерческая | DLL, `windeployqt`; Qt Network используется напрямую с 0.2.1 (загрузка ODM по HTTPS через Schannel) |
| Qt Qml, QmlModels, QmlMeta, QmlWorkerScript | 6.8.3 | то же | DLL |
| Qt Quick, Quick Layouts, Quick Shapes, Quick Effects | 6.8.3 | то же | DLL + `qml/` |
| Qt Quick Controls 2 (Basic и прочие стили), Templates 2 | 6.8.3 | то же | DLL + `qml/` |
| Qt Quick Dialogs 2 (+ QuickImpl, Utils), Qt Labs FolderListModel | 6.8.3 | то же | DLL + `qml/` |
| Qt Svg, плагины `qsvg`, `qsvgicon` | 6.8.3 | то же | DLL, плагины |
| Qt Image Formats: `qtiff`, `qwebp`, `qtga`, `qwbmp`, `qicns` | 6.8.3 | то же | `imageformats/` |
| Платформа `qwindows`, TLS `qschannelbackend`, `qcertonlybackend`, `qnetworklistmanager`, `qtuiotouchplugin` | 6.8.3 | то же | плагины |
| Рантайм MSVC (`vcruntime140*.dll`, `msvcp140*.dll`, `concrt140.dll`, `vccorlib140.dll`) | 14.44.35112 | Microsoft, распространяемый код Visual Studio | копируется скриптом из `VC\Redist\MSVC\<версия>\x64\Microsoft.VC143.CRT` (app-local развёртывание, документированное Microsoft) |
| `D3Dcompiler_47.dll` | системная | Microsoft, распространяемый компонент Windows SDK | `windeployqt` |

Сторонний код внутри поставляемых модулей Qt (атрибуции из SBOM):

| Компонент | Версия | Лицензия | Где |
| --- | --- | --- | --- |
| libjpeg-turbo | 3.1.0 | IJG AND BSD-3-Clause | Qt Gui (JPEG) |
| libpng | 1.6.47 | Libpng AND libpng-2.0 | Qt Gui |
| zlib | 1.3.1 | Zlib | Qt Core/Gui/Network |
| FreeType | 2.13.3 | FTL OR GPL-2.0-only | Qt Gui |
| HarfBuzz | 10.4.0 | MIT | Qt Gui |
| PCRE2 | 10.45 | BSD-3-Clause с исключением PCRE2 | Qt Core |
| double-conversion, tinycbor, md4c | 3.3.0, 0.6.1, 0.5.2 | BSD-3-Clause, MIT, MIT | Qt Core/Gui |
| Unicode Character Database, CLDR | 34, v46.1 | Unicode-3.0 | Qt Core |
| libtiff | 4.7.0 | libtiff | плагин `qtiff` |
| libwebp | 1.5.0 | BSD-3-Clause | плагин `qwebp` |
| libpsl / psl-data | — | BSD-3-Clause / MPL-2.0 | Qt Network |
| ICC sRGB profile | — | LicenseRef-ICC-License | Qt Gui |
| Wintab headers | — | LicenseRef-Lcs-Telegraphics | плагин `qwindows` |

Полный перечень с хэшами — в SBOM-файлах Qt.

### GIS: GDAL, PROJ и их библиотеки (с 0.3.0)

Собираются из исходников через vcpkg (`vcpkg.json`, baseline `9e593bb18ea69cc5095e012465dcd675a822ed0d` = тег vcpkg 2026.07.29, триплет `triplets/x64-windows-geomap`: DLL, только Release, MSVC v143) и подключаются **динамически** (DLL рядом с EXE). Лицензии проверены по файлам `copyright` установленных пакетов (`build/vcpkg_installed/x64-windows-geomap/share/<пакет>/copyright`). GPL/LGPL-компонентов среди них нет. GDAL собран без сетевых драйверов (curl), без GEOS и без PDF/HDF/NetCDF: только то, что нужно для GeoTIFF/COG, VRT, JPEG/PNG и векторов GeoJSON/KML/GeoPackage.

| Компонент | Версия | Лицензия (по `copyright`) | DLL в поставке |
| --- | --- | --- | --- |
| GDAL/OGR | 3.12.4 | MIT/X11 (`LICENSE.TXT` GDAL; отдельные файлы — BSD-подобные и public domain, перечислены там же) | `gdal.dll` |
| PROJ | 9.8.1 | MIT | `proj_9.dll` |
| `proj.db` (база CRS PROJ, включает данные EPSG Geodetic Parameter Dataset, IOGP) | из PROJ 9.8.1 | данные EPSG — условия использования EPSG (распространение без изменений с указанием источника); остальное — MIT | `share\proj\proj.db` |
| libgeotiff | 1.7.4 | MIT / public domain | `geotiff.dll` |
| libtiff | 4.7.2 | libtiff (BSD-подобная) | `tiff.dll` |
| libjpeg-turbo | 3.2.0 | IJG + BSD-3-Clause + Zlib | `jpeg62.dll`, `turbojpeg.dll` |
| libpng | 1.6.58 | PNG Reference Library License v2 | `libpng16.dll` |
| zlib | 1.3.2 | Zlib | `z.dll` |
| liblzma (XZ Utils) | 5.8.3 | 0BSD | `liblzma.dll` |
| SQLite | 3.53.4 | public domain | `sqlite3.dll` |
| Expat | 2.8.2 | MIT | `libexpat.dll` |
| json-c | 0.19 | MIT | `json-c.dll` |
| Данные GDAL (`share\gdal`) | 3.12.4 | MIT | файлы данных |

Только для сборки PROJ (в поставку не входят): nlohmann-json 3.12.0 (MIT, заголовки), pkgconf (ISC).

Linux-сборка использует системные пакеты `libgdal-dev` и `libproj-dev` (Ubuntu 24.04: GDAL 3.8, PROJ 9.4); их лицензии и состав определяет дистрибутив.

### Обязанности при публикации поставки (не выполнены — блокер публикации)

Папка `dist/GeoMapAI` пока **не содержит текстов лицензий**. До любой публикации архива (этап F):

1. приложить тексты LGPL-3.0, GPL-3.0 и уведомление Qt о лицензиях;
2. приложить уведомления и лицензии сторонних компонентов из таблиц выше, включая файлы `copyright` GDAL, PROJ и их библиотек и условия EPSG для `proj.db`;
3. указать, где получить исходный код использованной версии Qt (6.8.3);
4. сохранить динамическую компоновку с Qt и не запрещать замену библиотек Qt пользователем;
5. приложить лицензию GeoMap AI (MIT) и `docs/DEPENDENCIES.md`.

## 2. Только для сборки (не поставляются)

| Инструмент | Лицензия | Примечание |
| --- | --- | --- |
| CMake | BSD-3-Clause | ≥ 3.21 |
| MSVC (Visual Studio 2022 Build Tools / Community) | Microsoft | Условия использования определяет лицензия Visual Studio |
| Инструменты Qt: moc, rcc, qmlcachegen, qmltyperegistrar, windeployqt, qsb (Qt Shader Tools) | LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0 | Исключение Qt позволяет использовать результат их работы без ограничений GPL; `qsb` компилирует шейдеры 3D-вида при сборке |
| NSIS 3.13 (`makensis`, для `-Installer`) | zlib/libpng; модуль сжатия LZMA — CPL-1.0 (файл `COPYING` дистрибутива) | Официальный архив `nsis-3.13.zip` с SourceForge (MD5 `530e3623c077cba879ed2ae6e13be362` совпал с опубликованным), распакован в `.tools\nsis-3.13` (не в git). В `setup.exe` попадает заглушка NSIS (zlib) и распаковщик LZMA; лицензии позволяют распространять установщики, в том числе коммерчески. Inno Setup не используется: текущие версии бесплатны только для некоммерческого использования |
| Git | GPL-2.0 | — |
| vcpkg (инструмент и порты) | MIT | Собирает GDAL/PROJ из исходников; на Windows подходит копия из Visual Studio (`VC\vcpkg`) или `VCPKG_ROOT` |

## 3. Только для тестов и CI (не поставляются)

| Компонент | Лицензия |
| --- | --- |
| Qt Test | LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only OR коммерческая |
| GitHub Actions: `actions/checkout`, `actions/upload-artifact`, `actions/cache` | MIT |
| `jurplel/install-qt-action` (использует aqtinstall) | MIT |

## 4. Модули Qt, требующие отдельного решения

Некоторые модули Qt распространяются только под GPL-3.0 или коммерческой лицензией (например, Qt Charts, Qt Data Visualization, Qt Graphs, Qt Quick 3D, Qt Virtual Keyboard). Подключение такого модуля меняет условия распространения всего приложения. Перед использованием — проверить лицензию в SBOM конкретной версии Qt и согласовать с владельцем проекта. В частности, ТЗ требует не выбирать Qt Quick 3D для 3D-просмотра без такой оценки (этап D).

**Решение этапа D (0.4.0): 3D-вид сделан без новых модулей.** Он рисует через Qt RHI (`QQuickRhiItem` из Qt Quick и `QRhi` из Qt GUI, подключается как `Qt6::GuiPrivate`) — это части Qt GUI и Qt Quick под LGPL-3.0, которые уже в поставке. Шейдеры компилируются инструментом сборки Qt Shader Tools (`qsb`) во время сборки; в поставку попадают только скомпилированные шейдеры (ресурсы), не сам модуль. Qt Quick 3D, VTK и Open3D не используются. Ограничение: API RHI в Qt «полупубличный» (совместимость между минорными версиями Qt не гарантирована) — версия Qt закреплена (6.8.3); при обновлении Qt 3D-вид проверяется тестом `GeoMapAIView3DRenderTest`.

## 5. Встроенный OpenDroneMap (отдельная программа в составе поставки, не линкуется)

OpenDroneMap входит в каждую поставку GeoMap AI (`engines\odm`), но остаётся **отдельной программой**: GeoMap AI не содержит его кода и не линкуется с ним, а запускает его процессом (`QProcess`, список аргументов) и читает вывод и файлы результатов. Без него работает всё, кроме фотограмметрической обработки. Docker и WSL не используются (режим Docker удалён в 0.3.2).

| Компонент | Версия | Лицензия (проверено) | Как попадает к пользователю |
| --- | --- | --- | --- |
| OpenDroneMap (ODM), официальная сборка для Windows `ODM_Setup_3.6.2.exe` | 3.6.2, SHA-256 `c9dbfabf0d066529a7b6c753fd45bf3366a0314266160cd677373bab4caff294`, подпись «OpenDroneMap» | **AGPL-3.0** (`LICENSE` в папке ODM) + сторонние лицензии в `licenses\` (OpenSfM, OpenMVS, PDAL, GDAL, Python и др.) | лежит без изменений в `engines\odm` поставки (кладёт `build-windows.ps1`); при отсутствии восстанавливается из программы загрузкой того же установщика по подтверждению пользователя |

В ТЗ указано GPL-3.0; фактическая лицензия ODM — AGPL-3.0.

**Почему ODM не встроен в код программы.** Подключение кода ODM к процессу GeoMap AI (например, через встроенный Python) сделало бы GeoMap AI производной работой AGPL-3.0: проект пришлось бы перевести с MIT на AGPL-3.0. Это решение владельца, и оно не принималось. Поставка GeoMap AI вместе с неизменённым ODM, запускаемым отдельным процессом, — объединённая поставка («mere aggregation») двух программ; лицензия GeoMap AI не меняется. Обязанности при распространении (выполняются скриптом сборки):

1. ODM распространяется без изменений, со своими `LICENSE` и `licenses\`;
2. `engines\odm\SOURCE.txt` указывает, где получить исходный код этой версии ODM (тег `v3.6.2` репозитория OpenDroneMap/ODM). При публикации архива рекомендуется дополнительно хранить копию исходников рядом, так как ссылка на чужой репозиторий может перестать работать;
3. описание поставки не должно выдавать ODM за часть GeoMap AI под MIT.

Публикация такой поставки — только после решения владельца проекта (см. ТЗ).

**ИИ-модели самого ODM (раскрытие).** Некоторые параметры ODM включают его собственные модели, которые ODM **скачивает сам при первом использовании** в `%PROGRAMDATA%\ODM\storage\models` (код `opendm/ai.py`):

| Параметр ODM | Модель | Лицензия | Статус в GeoMap AI |
| --- | --- | --- | --- |
| `--pc-classify` («Классифицировать точки») | OpenPointClass `vehicles-vegetation-buildings` (uav4geo, релиз v1.1.3, 575 КБ) | код AGPL-3.0; условия весов не указаны | **была скачана ODM 2026-10-14** при тестовой обработке с классификацией (обнаружено и раскрыто владельцу 2026-10-15). Классификация в профилях не включается без явного выбора пользователя |
| `--sky-removal` | OpenDroneMap/SkyRemoval v1.0.5 | код AGPL-3.0; условия весов не указаны | не используется; владелец не давал согласия на загрузку |
| `--bg-removal` | U²-Net из релиза ODM v2.9.0 | — | не используется |

### Локальный ИИ (с 0.5.0): ONNX Runtime и модели

GeoMap AI выполняет модели сам, в своём процессе, через ONNX Runtime; снимки никуда не отправляются. Каждая модель — папка `models\<id>` с карточкой `model.json` (задача, лицензия, авторы, источник, преобразование, файлы с SHA-256) и текстом лицензии; модель без полной карточки или с несовпадающей суммой не загружается.

| Компонент | Версия / файл | Лицензия (проверено) | Как попадает к пользователю |
| --- | --- | --- | --- |
| ONNX Runtime (Microsoft), официальный архив `onnxruntime-win-x64-1.30.0.zip` | 1.30.0, архив 78,8 МБ, SHA-256 `c6ba983baf5681af108599675d2a89c2d145512d02de28aed0bff177cd0ba949`; в поставке `onnxruntime.dll` (15,7 МБ) и `onnxruntime_providers_shared.dll` | **MIT** (`LICENSE`), уведомления — `ThirdPartyNotices.txt` (оба в `licenses\onnxruntime`) | DLL рядом с программой; только CPU |
| MobileSAM (выделение по щелчку) | кодировщик `mobile_sam_image_encoder.onnx` (28,2 МБ, SHA-256 `580f5fb6…c749`), декодер `sam_mask_decoder_multi.onnx` (16,5 МБ, `8976b90a…2ffe`) | **Apache-2.0** (MobileSAM, Chaoning Zhang и соавт.; декодер — Segment Anything, Meta AI, Apache-2.0); ONNX-экспорт официальных весов — Acly/MobileSAM, коммит `0d3b4033…` | `models\mobilesam` |
| LaMa (удаление объектов, заполнение пропусков) | `lama_fp32.onnx` (208,0 МБ, SHA-256 `1faef5301d78db7dda502fe59966957ec4b79dd64e16f03ed96913c7a4eb68d6`) | **Apache-2.0** (advimman/lama, Samsung AI Center); ONNX-экспорт big-lama — Carve/LaMa-ONNX, Apache-2.0 | `models\lama` |

Загрузка ONNX Runtime и LaMa согласована владельцем (2026-10-15). Отклонены по лицензии: CLIP OpenAI (карточка модели исключает развёрнутое использование), модели Ultralytics YOLO (AGPL-3.0).

### Тестовые данные

| Набор | Лицензия | Использование |
| --- | --- | --- |
| [Brighton Beach](https://github.com/pierotofy/drone_dataset_brighton_beach), 18 снимков | BSD-2-Clause, © 2017 Piero Toffanin | Интеграционный тест и приёмка этапа B; в репозиторий не добавляется, загружается отдельно |

## 6. Планируемые зависимости (ещё не добавлены)

Сведения предварительные и подлежат проверке по файлам LICENSE закреплённых версий при добавлении.

| Компонент | Этап | Ожидаемая лицензия | Что проверить |
| --- | --- | --- | --- |

| LASzip или PDAL (чтение LAZ) | D+ | LASzip: Apache-2.0 (с 3.x; проверить), PDAL: BSD | Размер, сборка через vcpkg, лицензии зависимостей |
| Провайдеры GPU для ONNX Runtime (DirectML, CUDA) | E+ | MIT (DirectML); CUDA — лицензия NVIDIA | Размер, распространение библиотек NVIDIA |
