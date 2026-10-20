import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

// Survey control of the project (gcp.json): ground control and check points with their positions
// on the photos, the accuracy of the newest reconstruction at those points, the cameras and the
// calibration reused from an earlier job, and scale bars between markers.
Dialog {
    id: dialog
    objectName: "surveyDialog"
    title: "Опорные точки, камеры и контроль точности"
    modal: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(880, Overlay.overlay ? Overlay.overlay.width - 40 : 880)
    height: Math.min(620, Overlay.overlay ? Overlay.overlay.height - 40 : 620)
    padding: 14
    standardButtons: Dialog.Close
    property var host
    property var accuracyResult: ({})
    property var calibrations: []
    property var cameraGroups: []

    function fmt(v, d) { return Number(v).toLocaleString(Qt.locale("ru_RU"), 'f', d) }
    function num(text) { return Number(String(text).replace(/\s/g, "").replace(",", ".")) }
    function openTab(index) {
        tabs.currentIndex = index
        refresh()
        open()
    }
    function refresh() {
        if (tabs.currentIndex === 1) accuracyResult = survey.accuracy("")
        if (tabs.currentIndex === 2) { calibrations = survey.jobCalibrations(); cameraGroups = survey.cameras() }
    }
    Connections {
        target: survey
        function onChanged() { if (dialog.visible && tabs.currentIndex === 2) { dialog.calibrations = survey.jobCalibrations(); dialog.cameraGroups = survey.cameras() } }
    }

    contentItem: ColumnLayout {
        spacing: 8
        TabBar {
            id: tabs
            Layout.fillWidth: true
            onCurrentIndexChanged: dialog.refresh()
            TabButton { text: "Опорные точки (" + survey.points.length + ")" }
            TabButton { text: "Точность" }
            TabButton { text: "Камеры и калибровка" }
            TabButton { text: "Масштабные линейки" }
        }
        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: tabs.currentIndex

            // ---------------------------------------------------------------- points
            ColumnLayout {
                spacing: 8
                RowLayout {
                    spacing: 8
                    Label { text: "Система координат точек:"; color: Theme.muted; font.pixelSize: 11 }
                    Label { text: survey.crsLabel.length > 0 ? survey.crsLabel : "не задана"; color: survey.crsLabel.length > 0 ? Theme.text : Theme.warn; font.pixelSize: 11; Layout.fillWidth: true; elide: Text.ElideRight }
                    TextField { id: crsField; placeholderText: "EPSG:32637"; implicitWidth: 120; selectByMouse: true; font.pixelSize: 11 }
                    FlatButton {
                        text: "Задать"; compact: true; enabled: crsField.text.length > 0
                        onClicked: { var e = survey.setCrs(crsField.text); if (e.length > 0) dialog.host.showError(e); else crsField.text = "" }
                    }
                }
                RowLayout {
                    spacing: 8
                    FlatButton { text: "Импорт координат…"; compact: true; emphasis: survey.points.length === 0; onClicked: importDialog.open() }
                    FlatButton {
                        text: "Добавить точку"; compact: true; enabled: survey.crsLabel.length > 0
                        onClicked: survey.addPoint("", 0, 0, 0)
                    }
                    FlatButton {
                        text: "Отметить на снимках"; compact: true; enabled: survey.points.length > 0 && projects.photoCount > 0
                        onClicked: { dialog.close(); dialog.host.runCommand("project.gcpTag") }
                    }
                    FlatButton { text: "Экспорт gcp_list.txt…"; compact: true; enabled: survey.controlCount > 0; onClicked: gcpListFile.open() }
                    Item { Layout.fillWidth: true }
                    Label { text: "опорных " + survey.controlCount + " · контрольных " + survey.checkCount; color: Theme.muted; font.pixelSize: 11 }
                }
                ListView {
                    id: pointList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 3
                    model: survey.points
                    ScrollBar.vertical: ScrollBar {}
                    delegate: Rectangle {
                        required property var modelData
                        width: ListView.view.width - 12
                        height: pointRow.implicitHeight + 10
                        radius: 5
                        color: Theme.field
                        border.color: modelData.check ? "#3a6ea5" : Theme.line
                        RowLayout {
                            id: pointRow
                            anchors.fill: parent; anchors.margins: 5
                            spacing: 6
                            TextField { implicitWidth: 110; text: modelData.name; selectByMouse: true; font.pixelSize: 12; onEditingFinished: survey.renamePoint(modelData.id, text) }
                            Repeater {
                                model: [["X", "x", 3], ["Y", "y", 3], ["Z", "z", 3]]
                                delegate: TextField {
                                    required property var modelData
                                    property var point: parent.parent.modelData
                                    implicitWidth: 120; selectByMouse: true; font.pixelSize: 11
                                    placeholderText: modelData[0]
                                    text: dialog.fmt(point[modelData[1]], modelData[2])
                                    onEditingFinished: {
                                        var v = dialog.num(text)
                                        if (!isFinite(v)) { text = dialog.fmt(point[modelData[1]], modelData[2]); return }
                                        var p = { x: point.x, y: point.y, z: point.z }
                                        p[modelData[1]] = v
                                        survey.setCoordinates(point.id, p.x, p.y, p.z)
                                    }
                                }
                            }
                            CheckBox {
                                text: "контрольная"; checked: modelData.check; font.pixelSize: 11
                                onToggled: survey.setCheck(modelData.id, checked)
                                ToolTip.visible: hovered; ToolTip.delay: 600
                                ToolTip.text: "Контрольная точка не передаётся движку: по ней независимо оценивается точность"
                            }
                            Label {
                                Layout.fillWidth: true
                                text: "снимков: " + modelData.photos
                                color: modelData.check || modelData.photos >= 3 ? Theme.muted : Theme.warn; font.pixelSize: 11
                            }
                            FlatButton { text: "Удалить"; compact: true; caution: true; onClicked: survey.removePoint(modelData.id) }
                        }
                    }
                    Label {
                        anchors.centerIn: parent
                        visible: survey.points.length === 0
                        width: parent.width - 60
                        wrapMode: Text.Wrap; horizontalAlignment: Text.AlignHCenter
                        text: "Точек нет — импортируйте каталог координат"
                        color: Theme.faint
                    }
                }
            }

            // ---------------------------------------------------------------- accuracy
            ColumnLayout {
                spacing: 8
                RowLayout {
                    spacing: 8
                    Label {
                        Layout.fillWidth: true; wrapMode: Text.Wrap
                        text: dialog.accuracyResult.error ? dialog.accuracyResult.error
                              : "Задание " + (dialog.accuracyResult.job || "") + " · " + (dialog.accuracyResult.crs || "")
                                + (dialog.accuracyResult.gcpUsed ? " · привязано по опорным точкам" : " · привязано по GPS снимков (опорные точки не использовались)")
                        color: dialog.accuracyResult.error ? Theme.warn : Theme.text; font.pixelSize: 11
                    }
                    FlatButton { text: "Пересчитать"; compact: true; onClicked: dialog.accuracyResult = survey.accuracy("") }
                    FlatButton { text: "Экспорт CSV…"; compact: true; enabled: !dialog.accuracyResult.error; onClicked: accuracyFile.open() }
                }
                Repeater {
                    model: [["control", "Опорные точки"], ["check", "Контрольные точки"]]
                    delegate: Label {
                        required property var modelData
                        property var m: dialog.accuracyResult[modelData[0]] || ({})
                        visible: (m.count || 0) > 0
                        text: modelData[1] + " (" + (m.count || 0) + "): СКО X " + dialog.fmt(m.rmseX || 0, 3) + " м, Y " + dialog.fmt(m.rmseY || 0, 3)
                              + " м, Z " + dialog.fmt(m.rmseZ || 0, 3) + " м, плановая " + dialog.fmt(m.rmseXY || 0, 3) + " м, общая " + dialog.fmt(m.rmse || 0, 3) + " м"
                        color: modelData[0] === "check" ? Theme.good : Theme.text; font.pixelSize: 12; font.bold: true
                    }
                }
                RowLayout {
                    spacing: 0
                    Repeater {
                        model: [["Точка", 130], ["Тип", 100], ["Снимков", 70], ["dX, м", 80], ["dY, м", 80], ["dZ, м", 80], ["Ошибка, м", 90], ["На снимках, пикс", 120]]
                        delegate: Label { required property var modelData; text: modelData[0]; Layout.preferredWidth: modelData[1]; color: Theme.muted; font.pixelSize: 11; font.bold: true }
                    }
                }
                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: dialog.accuracyResult.rows || []
                    ScrollBar.vertical: ScrollBar {}
                    delegate: RowLayout {
                        required property var modelData
                        spacing: 0
                        Label { text: modelData.name; Layout.preferredWidth: 130; elide: Text.ElideRight; color: Theme.text; font.pixelSize: 11 }
                        Label { text: modelData.check ? "контрольная" : "опорная"; Layout.preferredWidth: 100; color: modelData.check ? Theme.good : Theme.text; font.pixelSize: 11 }
                        Label { text: modelData.photos; Layout.preferredWidth: 70; color: modelData.photos >= 2 ? Theme.text : Theme.warn; font.pixelSize: 11 }
                        Repeater {
                            model: [["dx", 80, 3], ["dy", 80, 3], ["dz", 80, 3], ["d", 90, 3], ["reprojection", 120, 2]]
                            delegate: Label {
                                required property var modelData
                                property var row: parent.modelData
                                text: row[modelData[0]] !== undefined ? dialog.fmt(row[modelData[0]], modelData[2]) : (modelData[0] === "dx" ? "нужно ≥ 2 снимков" : "")
                                Layout.preferredWidth: modelData[1]; color: Theme.text; font.pixelSize: 11
                            }
                        }
                    }
                }
            }

            // ---------------------------------------------------------------- cameras
            ColumnLayout {
                spacing: 8
                Label { text: "КАМЕРЫ СНИМКОВ"; color: Theme.muted; font.pixelSize: 10; font.bold: true; font.letterSpacing: 1.2 }
                Repeater {
                    model: dialog.cameraGroups
                    delegate: Label {
                        required property var modelData
                        text: modelData.model + " · " + modelData.width + "×" + modelData.height + " · снимков: " + modelData.count
                              + (modelData.calibrated ? " · калибровка проекта подходит" : "")
                        color: Theme.text; font.pixelSize: 12
                    }
                }
                RowLayout {
                    Label {
                        Layout.fillWidth: true; wrapMode: Text.Wrap
                        text: survey.calibration.length > 0
                              ? "Следующие обработки начинают с калибровки задания " + (survey.calibration === "?" ? "(файл calibration/cameras.json)" : survey.calibration) + " и уточняют её."
                              : "Каждая обработка вычисляет калибровку камеры заново (самокалибровка)."
                        color: survey.calibration.length > 0 ? Theme.good : Theme.muted; font.pixelSize: 11
                    }
                    FlatButton { text: "Сбросить"; compact: true; caution: true; visible: survey.calibration.length > 0; onClicked: survey.clearCalibration() }
                }
                Label { text: "КАЛИБРОВКИ ЗАВЕРШЁННЫХ ЗАДАНИЙ"; color: Theme.muted; font.pixelSize: 10; font.bold: true; font.letterSpacing: 1.2; Layout.topMargin: 6 }
                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 4
                    model: dialog.calibrations
                    ScrollBar.vertical: ScrollBar {}
                    delegate: Rectangle {
                        required property var modelData
                        width: ListView.view.width - 12
                        height: calColumn.implicitHeight + 12
                        radius: 5
                        color: Theme.field
                        border.color: modelData.used ? Theme.accent : Theme.line
                        RowLayout {
                            anchors.fill: parent; anchors.margins: 6
                            ColumnLayout {
                                id: calColumn
                                Layout.fillWidth: true
                                spacing: 2
                                Label { text: "Задание " + modelData.job + " · " + modelData.date; color: Theme.text; font.pixelSize: 12; font.bold: true }
                                Repeater {
                                    model: modelData.cameras
                                    delegate: Label {
                                        required property var modelData
                                        Layout.fillWidth: true; wrapMode: Text.Wrap
                                        text: modelData.id + ": f = " + dialog.fmt(modelData.focalPx, 1) + " пикс"
                                              + (modelData.k1 !== undefined ? ", k1 " + dialog.fmt(modelData.k1, 5) : "")
                                              + (modelData.k2 !== undefined ? ", k2 " + dialog.fmt(modelData.k2, 5) : "")
                                              + (modelData.k3 !== undefined ? ", k3 " + dialog.fmt(modelData.k3, 5) : "")
                                              + (modelData.p1 !== undefined ? ", p1 " + dialog.fmt(modelData.p1, 5) : "")
                                              + (modelData.p2 !== undefined ? ", p2 " + dialog.fmt(modelData.p2, 5) : "")
                                        color: Theme.muted; font.pixelSize: 11
                                    }
                                }
                            }
                            FlatButton {
                                text: modelData.used ? "Используется" : "Использовать"; compact: true; enabled: !modelData.used
                                onClicked: { var e = survey.useCalibration(modelData.job); if (e.length > 0) dialog.host.showError(e) }
                            }
                        }
                    }
                    Label {
                        anchors.centerIn: parent
                        visible: dialog.calibrations.length === 0
                        width: parent.width - 60
                        wrapMode: Text.Wrap; horizontalAlignment: Text.AlignHCenter
                        text: "Калибровок пока нет: они появляются после выравнивания снимков (cameras.json задания)."
                        color: Theme.faint
                    }
                }
            }

            // ---------------------------------------------------------------- scale bars
            ColumnLayout {
                spacing: 8
                RowLayout {
                    spacing: 8
                    ComboBox { id: markerA; implicitWidth: 170; model: gis.markers; textRole: "name"; valueRole: "id"; font.pixelSize: 11 }
                    Label { text: "—"; color: Theme.muted }
                    ComboBox { id: markerB; implicitWidth: 170; model: gis.markers; textRole: "name"; valueRole: "id"; currentIndex: Math.min(1, gis.markers.length - 1); font.pixelSize: 11 }
                    TextField { id: barDistance; implicitWidth: 110; placeholderText: "расстояние, м"; selectByMouse: true; font.pixelSize: 11 }
                    FlatButton {
                        text: "Добавить"; compact: true; enabled: gis.markers.length >= 2 && barDistance.text.length > 0
                        onClicked: {
                            var e = survey.addScaleBar(markerA.currentValue, markerB.currentValue, dialog.num(barDistance.text))
                            if (e.length > 0) dialog.host.showError(e); else barDistance.text = ""
                        }
                    }
                    Item { Layout.fillWidth: true }
                }
                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 3
                    model: survey.scaleBars
                    ScrollBar.vertical: ScrollBar {}
                    delegate: Rectangle {
                        required property var modelData
                        width: ListView.view.width - 12
                        height: 34
                        radius: 5
                        color: Theme.field
                        border.color: Theme.line
                        RowLayout {
                            anchors.fill: parent; anchors.margins: 6
                            Label { text: modelData.a + " — " + modelData.b; Layout.preferredWidth: 220; elide: Text.ElideRight; color: Theme.text; font.pixelSize: 12 }
                            Label { text: "на местности " + dialog.fmt(modelData.distance, 3) + " м"; Layout.preferredWidth: 160; color: Theme.muted; font.pixelSize: 11 }
                            Label {
                                Layout.fillWidth: true
                                text: modelData.measured !== undefined
                                      ? "на карте " + dialog.fmt(modelData.measured, 3) + " м · ошибка " + (modelData.error >= 0 ? "+" : "") + dialog.fmt(modelData.error, 3)
                                        + " м (" + dialog.fmt(100 * modelData.error / modelData.distance, 2) + " %)"
                                      : "маркер удалён"
                                color: modelData.measured === undefined ? Theme.warn
                                       : Math.abs(modelData.error / modelData.distance) < 0.005 ? Theme.good : Theme.warn
                                font.pixelSize: 11
                            }
                            FlatButton { text: "Удалить"; compact: true; caution: true; onClicked: survey.removeScaleBar(modelData.id) }
                        }
                    }
                    Label {
                        anchors.centerIn: parent
                        visible: survey.scaleBars.length === 0
                        width: parent.width - 60
                        wrapMode: Text.Wrap; horizontalAlignment: Text.AlignHCenter
                        text: gis.markers.length < 2
                              ? "Поставьте два маркера на концах измеренного отрезка"
                              : "Выберите два маркера и введите расстояние между ними, измеренное на местности."
                        color: Theme.faint
                    }
                }
            }
        }
    }

    Dialog {
        id: importDialog
        title: "Импорт опорных точек"
        modal: true
        anchors.centerIn: Overlay.overlay
        width: 460
        standardButtons: Dialog.Cancel
        contentItem: ColumnLayout {
            spacing: 8
            Label {
                Layout.fillWidth: true; wrapMode: Text.Wrap
                text: "CSV или TXT: имя, X, Y, Z — или gcp_list.txt OpenDroneMap"
                color: Theme.text; font.pixelSize: 11
            }
            RowLayout {
                Label { text: "Система координат файла:"; color: Theme.muted; font.pixelSize: 11 }
                TextField { id: importCrs; Layout.fillWidth: true; placeholderText: survey.crsLabel.length > 0 ? "как у точек проекта" : "EPSG:32637"; selectByMouse: true; font.pixelSize: 11 }
            }
            CheckBox {
                id: northFirst
                text: "Сначала северная координата (как в геодезии)"
                font.pixelSize: 11
            }
            FlatButton { text: "Выбрать файл…"; emphasis: true; onClicked: pointsFile.open() }
        }
    }
    AppFileDialog {
        id: pointsFile
        kind: "import"
        title: "Файл опорных точек"
        nameFilters: ["Координаты (*.csv *.txt)", "Все файлы (*)"]
        onAccepted: {
            var r = survey.importFile(selectedFile, importCrs.text, northFirst.checked)
            importDialog.close()
            if (r.error && r.error.length > 0) { dialog.host.showError(r.error); return }
            dialog.host.flash("Импортировано точек: " + r.added + (r.observations > 0 ? ", отметок на снимках: " + r.observations : "")
                              + (r.warning ? ". " + r.warning : ""))
        }
    }
    AppFileDialog {
        id: gcpListFile
        kind: "export"
        title: "Сохранить gcp_list.txt"
        fileMode: FileDialog.SaveFile
        nameFilters: ["gcp_list (*.txt)"]
        defaultSuffix: "txt"
        onAccepted: {
            var e = survey.exportGcpList(selectedFile)
            if (e.length > 0) dialog.host.showError(e)
            else dialog.host.flash("Сохранено: " + gis.displayPath(selectedFile))
        }
    }
    AppFileDialog {
        id: accuracyFile
        kind: "export"
        title: "Сохранить ведомость точности"
        fileMode: FileDialog.SaveFile
        nameFilters: ["CSV (*.csv)"]
        defaultSuffix: "csv"
        onAccepted: {
            var e = survey.exportAccuracy(selectedFile, dialog.accuracyResult.job || "")
            if (e.length > 0) dialog.host.showError(e)
            else dialog.host.flash("Сохранено: " + gis.displayPath(selectedFile))
        }
    }
}
