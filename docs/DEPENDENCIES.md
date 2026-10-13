# Зависимости и лицензии

Правило проекта: **перед добавлением любой зависимости** проверить её лицензию, файлы NOTICE, условия сборки и распространения и записать результат в этот документ. Источник данных о Qt ниже — SBOM-файлы установленного комплекта (`<Qt>/6.8.3/msvc2022_64/sbom/*.spdx.json`), а не память или предположения.

Собственный код GeoMap AI распространяется под MIT (`LICENSE`). Это не меняет лицензий сторонних компонентов в поставке: объединённый дистрибутив нельзя маркировать как «полностью MIT».

## 1. Компоненты в поставке (`dist/GeoMapAI`)

Qt подключается **динамически** (DLL рядом с EXE), что позволяет пользователю заменить библиотеки Qt — одно из условий LGPL.

| Компонент | Версия | Лицензия (по SBOM) | Как попадает в поставку |
| --- | --- | --- | --- |
| Qt Core, Gui, Network, OpenGL | 6.8.3 | LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only OR коммерческая | DLL, `windeployqt` |
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

### Обязанности при публикации поставки (не выполнены — блокер публикации)

Папка `dist/GeoMapAI` пока **не содержит текстов лицензий**. До любой публикации архива (этап F):

1. приложить тексты LGPL-3.0, GPL-3.0 и уведомление Qt о лицензиях;
2. приложить уведомления и лицензии сторонних компонентов из таблиц выше;
3. указать, где получить исходный код использованной версии Qt (6.8.3);
4. сохранить динамическую компоновку с Qt и не запрещать замену библиотек Qt пользователем;
5. приложить лицензию GeoMap AI (MIT) и `docs/DEPENDENCIES.md`.

## 2. Только для сборки (не поставляются)

| Инструмент | Лицензия | Примечание |
| --- | --- | --- |
| CMake | BSD-3-Clause | ≥ 3.21 |
| MSVC (Visual Studio 2022 Build Tools / Community) | Microsoft | Условия использования определяет лицензия Visual Studio |
| Инструменты Qt: moc, rcc, qmlcachegen, qmltyperegistrar, windeployqt | LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0 | Исключение Qt позволяет использовать результат их работы без ограничений GPL |
| Git | GPL-2.0 | — |

## 3. Только для тестов и CI (не поставляются)

| Компонент | Лицензия |
| --- | --- |
| Qt Test | LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only OR коммерческая |
| GitHub Actions: `actions/checkout`, `actions/upload-artifact` | MIT |
| `jurplel/install-qt-action` (использует aqtinstall) | MIT |

## 4. Модули Qt, требующие отдельного решения

Некоторые модули Qt распространяются только под GPL-3.0 или коммерческой лицензией (например, Qt Charts, Qt Data Visualization, Qt Graphs, Qt Quick 3D, Qt Virtual Keyboard). Подключение такого модуля меняет условия распространения всего приложения. Перед использованием — проверить лицензию в SBOM конкретной версии Qt и согласовать с владельцем проекта. В частности, ТЗ требует не выбирать Qt Quick 3D для 3D-просмотра без такой оценки (этап D).

## 5. Планируемые зависимости (ещё не добавлены)

Сведения предварительные и подлежат проверке по файлам LICENSE закреплённых версий при добавлении.

| Компонент | Этап | Ожидаемая лицензия | Что проверить |
| --- | --- | --- | --- |
| OpenDroneMap (ODM) | B | AGPL-3.0 по репозиторию OpenDroneMap/ODM (в ТЗ указано GPL-3.0 — уточнить) | Используется как отдельный процесс/контейнер, не линкуется; образ закрепить по тегу/digest |
| Docker / Docker Desktop | B | Лицензия Docker Desktop зависит от типа использования | Не обязателен для запуска GeoMap AI |
| GDAL / OGR | C | MIT | Транзитивные зависимости (PROJ, GEOS, libtiff, curl и др.) |
| PROJ | C | MIT | База EPSG (условия использования данных) |
| SQLite | A–F | Public domain | — |
| VTK или Open3D | D | BSD-3-Clause / MIT | Упаковка, размер, драйверы |
| ONNX Runtime | E | MIT | Провайдеры GPU и их лицензии |
| Модели сегментации | E | Разные | Карточка модели: лицензия весов и датасета обучения |
