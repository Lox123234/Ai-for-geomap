import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Обработка → Оптимизация камер: the newest aligned job bundled again with the project's control
// points; its results are then built again.
Dialog {
    id: dialog
    objectName: "optimizeDialog"
    title: "Оптимизация камер"
    modal: true
    anchors.centerIn: Overlay.overlay
    width: 440
    padding: 18
    property var host
    property string jobId: ""
    onOpened: jobId = processing.newestJob("aligned")

    contentItem: ColumnLayout {
        spacing: 10
        Label {
            Layout.fillWidth: true
            text: dialog.jobId.length > 0 ? "Задание " + dialog.jobId : "Нет выровненного задания"
            color: Theme.text; font.pixelSize: 13; font.weight: Font.DemiBold
        }
        RowLayout {
            spacing: 6
            Label { text: "Опорных точек"; color: Theme.muted; font.pixelSize: 12 }
            Label {
                text: survey.controlCount
                color: survey.controlCount >= 3 ? Theme.good : Theme.warn; font.pixelSize: 12; font.weight: Font.DemiBold
                ToolTip.visible: gcpHover.hovered; ToolTip.delay: 400
                ToolTip.text: "Учитываются точки, отмеченные минимум на 3 снимках задания (нужно не меньше 3 таких точек)"
                HoverHandler { id: gcpHover }
            }
        }
        CheckBox {
            id: refine
            objectName: "optimizeRefine"
            text: "Уточнить калибровку камер"
            checked: true
            font.pixelSize: 12
            ToolTip.visible: hovered; ToolTip.delay: 500
            ToolTip.text: "Фокус, главная точка и дисторсия уточняются вместе с положениями камер"
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: survey.controlCount >= 3 ? "Результаты задания будут пересобраны" : "Без опорных точек — уточнение по связующим точкам и GPS. Результаты будут пересобраны"
            color: Theme.faint; font.pixelSize: 11
        }
        RowLayout {
            visible: processing.optimizing
            spacing: 8
            BusyIndicator { running: processing.optimizing; implicitWidth: 22; implicitHeight: 22 }
            Label { text: "Уравнивание…"; color: Theme.text; font.pixelSize: 12 }
        }
    }
    footer: DialogButtonBox {
        FlatButton {
            objectName: "optimizeStart"
            text: "Оптимизировать"; emphasis: true
            enabled: dialog.jobId.length > 0 && !processing.optimizing
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
        }
        FlatButton { text: "Отмена"; DialogButtonBox.buttonRole: DialogButtonBox.RejectRole }
    }
    onAccepted: {
        var e = processing.optimizeCameras(dialog.jobId, refine.checked)
        if (e.length > 0) dialog.host.showError(e)
        else dialog.host.flash("Оптимизация камер…")
    }
    Connections {
        target: processing
        function onOptimizeFinished(jobId, ok, message) {
            if (ok) dialog.host.flash("Камеры оптимизированы: " + message)
            else dialog.host.showError("Оптимизация камер: " + message)
        }
    }
}
