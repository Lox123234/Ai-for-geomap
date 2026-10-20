import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

// The right panel of the georeferencing: the accuracy, the transformation, the links with their
// errors, the selected link's coordinates, link files and the GeoTIFF — ArcMap's Link Table and
// Georeferencing toolbar in one short column.
Rectangle {
    id: panel
    property var host
    property var imagePane            // GeorefWorkspace: its view zooms to a link
    color: Theme.panel

    component Cross: Item {
        property color tint
        implicitWidth: 11; implicitHeight: 11
        Rectangle { anchors.centerIn: parent; width: 11; height: 2; color: parent.tint }
        Rectangle { anchors.centerIn: parent; width: 2; height: 11; color: parent.tint }
    }

    function fmt(v, d) { return v === undefined || v < 0 ? "—" : georef.formatNumber(v, d) }
    readonly property int mapDecimals: georef.units === "°" ? 7 : 3
    readonly property var current: georef.selectedLink >= 0 && georef.selectedLink < georef.links.length ? georef.links[georef.selectedLink] : null
    function apply(field, text) {
        var e = georef.setLinkValue(panel.current.index, field, text)
        if (e.length) panel.host.showNotice(e)
    }
    function show(index) {
        georef.zoomToLink(index)
        if (panel.imagePane) panel.imagePane.view.zoomToLink(index)
    }

    AppFileDialog {
        id: saveLinksDialog
        kind: "georef"
        title: "Сохранить связи"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "txt"
        nameFilters: ["Связи ArcMap (*.txt)", "Все файлы (*)"]
        onAccepted: { var e = georef.saveLinks(selectedFile); if (e.length) panel.host.showNotice(e) }
    }
    AppFileDialog {
        id: loadLinksDialog
        kind: "georef"
        title: "Загрузить связи"
        nameFilters: ["Связи (*.txt *.csv)", "Все файлы (*)"]
        onAccepted: { var e = georef.loadLinks(selectedFile); if (e.length) panel.host.showNotice(e) }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Theme.panelHeader
            color: Theme.toolbar
            SectionTitle { anchors.verticalCenter: parent.verticalCenter; x: 10; text: "ПРИВЯЗКА" }
            FlatButton {
                objectName: "georefClose"
                anchors.right: parent.right; anchors.rightMargin: 6; anchors.verticalCenter: parent.verticalCenter
                compact: true; text: "Готово"
                onClicked: georef.close()
                ToolTip.visible: hovered; ToolTip.delay: 500
                ToolTip.text: "Закрыть привязку. Связи сохранятся в проекте"
            }
        }
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth
            clip: true
            ColumnLayout {
                width: parent.width
                spacing: 8
                Item { implicitHeight: 2 }
                // --- The image.
                ColumnLayout {
                    Layout.fillWidth: true; Layout.leftMargin: 12; Layout.rightMargin: 12
                    spacing: 1
                    Label {
                        Layout.fillWidth: true
                        text: georef.imageName
                        elide: Text.ElideMiddle
                        color: Theme.text; font.pixelSize: 13; font.weight: Font.DemiBold
                    }
                    Label {
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        text: (georef.imageWidth > 0 ? georef.imageWidth + " × " + georef.imageHeight : "") + (georef.placement.length ? " · " + georef.placement : "")
                        color: Theme.faint; font.pixelSize: 11
                    }
                }
                // --- Accuracy: the number first.
                Rectangle {
                    objectName: "georefAccuracy"
                    Layout.fillWidth: true; Layout.leftMargin: 10; Layout.rightMargin: 10
                    implicitHeight: accuracy.implicitHeight + 16
                    radius: 6
                    color: georef.fitted ? "#1A3AD07A" : "#1AF5C542"
                    border.color: georef.fitted ? "#553AD07A" : "#55F5C542"
                    ColumnLayout {
                        id: accuracy
                        anchors.left: parent.left; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: 12; anchors.rightMargin: 12
                        spacing: 0
                        RowLayout {
                            visible: georef.fitted
                            spacing: 8
                            Label { text: "СКО"; color: Theme.muted; font.pixelSize: 11; Layout.alignment: Qt.AlignBottom; bottomPadding: 4 }
                            Label { text: panel.fmt(georef.rmse, panel.mapDecimals) + " " + georef.units; color: Theme.good; font.pixelSize: 22; font.weight: Font.DemiBold }
                            Item { Layout.fillWidth: true }
                            Label {
                                text: panel.fmt(georef.inverseRmse, 1) + " пикс."
                                color: Theme.muted; font.pixelSize: 12; Layout.alignment: Qt.AlignBottom; bottomPadding: 4
                                ToolTip.visible: invHover.hovered; ToolTip.delay: 400; ToolTip.text: "Та же ошибка в пикселях снимка"
                                HoverHandler { id: invHover }
                            }
                        }
                        Label {
                            objectName: "georefFit"
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: georef.fitted ? georef.fitText.split("\n").slice(1).join("\n") : georef.fitText
                            visible: text.length > 0
                            color: georef.fitted ? Theme.faint : "#F5C542"
                            font.pixelSize: georef.fitted ? 11 : 13
                            font.weight: georef.fitted ? Font.Normal : Font.DemiBold
                        }
                    }
                }
                // --- Transformation and display.
                ComboBox {
                    id: methodBox
                    objectName: "georefMethod"
                    Layout.fillWidth: true; Layout.leftMargin: 10; Layout.rightMargin: 10
                    implicitHeight: 30
                    model: georef.methods
                    textRole: "title"; valueRole: "value"
                    currentIndex: Math.max(0, indexOfValue(georef.method))
                    onActivated: georef.method = currentValue
                    font.pixelSize: 12
                    ToolTip.visible: hovered; ToolTip.delay: 600
                    ToolTip.text: "Аффинное — для сканов и ортофото (от 3 связей)\nПроективное — наклонный снимок (от 4)\nПолиномы — изогнутые карты (от 6 и 10)"
                }
                RowLayout {
                    Layout.fillWidth: true; Layout.leftMargin: 6; Layout.rightMargin: 10
                    spacing: 2
                    CheckBox {
                        objectName: "georefAuto"
                        text: "Авто"
                        checked: georef.autoAdjust
                        onToggled: georef.autoAdjust = checked
                        font.pixelSize: 12
                        ToolTip.visible: hovered; ToolTip.delay: 600
                        ToolTip.text: "Снимок встаёт по связям сразу"
                    }
                    FlatButton { compact: true; visible: !georef.autoAdjust; text: "Применить"; onClicked: georef.applyNow() }
                    CheckBox {
                        text: "Снимок"
                        checked: georef.showImage
                        onToggled: georef.showImage = checked
                        font.pixelSize: 12
                        ToolTip.visible: hovered; ToolTip.delay: 600
                        ToolTip.text: "Показывать снимок на карте"
                    }
                    Slider {
                        Layout.fillWidth: true
                        enabled: georef.showImage
                        from: 0.1; to: 1; value: georef.opacity
                        onMoved: georef.opacity = value
                        ToolTip.visible: hovered; ToolTip.delay: 600
                        ToolTip.text: "Непрозрачность снимка: " + Math.round(georef.opacity * 100) + " %"
                    }
                }

                // --- Links.
                RowLayout {
                    Layout.fillWidth: true; Layout.leftMargin: 10; Layout.rightMargin: 10; Layout.topMargin: 2
                    SectionTitle { text: "СВЯЗИ" }
                    Label { text: georef.linkCount; color: Theme.muted; font.pixelSize: 11 }
                    Item { Layout.fillWidth: true }
                    Label { text: "ошибка, " + georef.units; color: Theme.faint; font.pixelSize: 10; Layout.preferredWidth: 70; horizontalAlignment: Text.AlignRight }
                    Label { text: "пикс."; color: Theme.faint; font.pixelSize: 10; Layout.preferredWidth: 44; horizontalAlignment: Text.AlignRight }
                    Item { Layout.preferredWidth: 22 }
                }
                Label {
                    visible: georef.linkCount === 0
                    Layout.fillWidth: true; Layout.leftMargin: 12; Layout.rightMargin: 12
                    wrapMode: Text.Wrap
                    text: "Точка на снимке, затем — это место на карте"
                    color: Theme.faint; font.pixelSize: 12
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    Repeater {
                        model: georef.links
                        delegate: Rectangle {
                            required property var modelData
                            Layout.fillWidth: true
                            implicitHeight: 28
                            color: modelData.index === georef.selectedLink ? Theme.accentSoft : rowMouse.containsMouse ? Theme.hover : "transparent"
                            MouseArea {
                                id: rowMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: georef.selectedLink = modelData.index
                                onDoubleClicked: panel.show(modelData.index)
                            }
                            RowLayout {
                                anchors.fill: parent; anchors.leftMargin: 6; anchors.rightMargin: 10
                                spacing: 2
                                CheckBox {
                                    implicitHeight: 24
                                    checked: modelData.enabled
                                    onToggled: georef.setLinkEnabled(modelData.index, checked)
                                    ToolTip.visible: hovered; ToolTip.delay: 600
                                    ToolTip.text: "Учитывать связь"
                                }
                                Label {
                                    text: modelData.index + 1
                                    color: modelData.enabled ? Theme.text : Theme.faint
                                    font.pixelSize: 12; font.weight: Font.DemiBold
                                    Layout.fillWidth: true
                                }
                                Label {
                                    Layout.preferredWidth: 70; horizontalAlignment: Text.AlignRight; font.pixelSize: 12
                                    text: panel.fmt(modelData.residual, panel.mapDecimals)
                                    color: modelData.worst ? Theme.bad : modelData.enabled ? Theme.text : Theme.faint
                                    font.weight: modelData.worst ? Font.DemiBold : Font.Normal
                                    ToolTip.visible: modelData.worst && worstHover.hovered; ToolTip.delay: 300
                                    ToolTip.text: "Самая большая ошибка — проверьте эту точку"
                                    HoverHandler { id: worstHover }
                                }
                                Label {
                                    Layout.preferredWidth: 44; horizontalAlignment: Text.AlignRight; font.pixelSize: 11
                                    text: panel.fmt(modelData.inverse, 1)
                                    color: Theme.faint
                                }
                                ToolButton {
                                    Layout.preferredWidth: 22; implicitHeight: 22
                                    text: "✕"; font.pixelSize: 11
                                    onClicked: georef.removeLink(modelData.index)
                                    ToolTip.visible: hovered; ToolTip.delay: 500; ToolTip.text: "Удалить"
                                }
                            }
                        }
                    }
                }

                // --- The selected link: coordinates to type in.
                GridLayout {
                    visible: panel.current !== null
                    Layout.fillWidth: true; Layout.leftMargin: 12; Layout.rightMargin: 10
                    columns: 3; columnSpacing: 6; rowSpacing: 4
                    Label { text: "Снимок"; color: Theme.muted; font.pixelSize: 11 }
                    TextField {
                        objectName: "georefIx"
                        Layout.fillWidth: true; implicitHeight: 28; font.pixelSize: 11
                        text: panel.current ? georef.formatNumber(panel.current.ix, 1) : ""
                        onEditingFinished: if (panel.current) panel.apply("ix", text)
                    }
                    TextField {
                        Layout.fillWidth: true; implicitHeight: 28; font.pixelSize: 11
                        text: panel.current ? georef.formatNumber(panel.current.iy, 1) : ""
                        onEditingFinished: if (panel.current) panel.apply("iy", text)
                    }
                    Label {
                        text: "Карта"; color: Theme.muted; font.pixelSize: 11
                        ToolTip.visible: mapHover.hovered; ToolTip.delay: 400; ToolTip.text: gis.crsLabel
                        HoverHandler { id: mapHover }
                    }
                    TextField {
                        objectName: "georefMx"
                        Layout.fillWidth: true; implicitHeight: 28; font.pixelSize: 11
                        text: panel.current ? georef.formatNumber(panel.current.mx, panel.mapDecimals) : ""
                        onEditingFinished: if (panel.current) panel.apply("mx", text)
                    }
                    TextField {
                        Layout.fillWidth: true; implicitHeight: 28; font.pixelSize: 11
                        text: panel.current ? georef.formatNumber(panel.current.my, panel.mapDecimals) : ""
                        onEditingFinished: if (panel.current) panel.apply("my", text)
                    }
                    Item { implicitWidth: 1 }
                    FlatButton { compact: true; Layout.fillWidth: true; text: "Показать"; onClicked: panel.show(panel.current.index) }
                    FlatButton { compact: true; Layout.fillWidth: true; caution: true; text: "Удалить"; onClicked: georef.removeLink(panel.current.index) }
                }
                Flow {
                    Layout.fillWidth: true; Layout.leftMargin: 10; Layout.rightMargin: 10
                    spacing: 6
                    FlatButton { compact: true; visible: georef.pending; text: "Отменить точку"; onClicked: georef.cancelPending() }
                    FlatButton { compact: true; text: "Загрузить…"; onClicked: loadLinksDialog.open()
                                 ToolTip.visible: hovered; ToolTip.delay: 500; ToolTip.text: "Связи из файла (ArcMap, txt)" }
                    FlatButton { compact: true; text: "Сохранить…"; enabled: georef.linkCount > 0; onClicked: saveLinksDialog.open()
                                 ToolTip.visible: hovered; ToolTip.delay: 500; ToolTip.text: "Связи в файл (формат ArcMap)" }
                    FlatButton { compact: true; caution: true; text: "Очистить"; enabled: georef.linkCount > 0; onClicked: georef.clearLinks() }
                }

                // --- Result.
                Rectangle { Layout.fillWidth: true; Layout.leftMargin: 10; Layout.rightMargin: 10; implicitHeight: 1; color: Theme.line }
                RowLayout {
                    Layout.fillWidth: true; Layout.leftMargin: 12; Layout.rightMargin: 10
                    Label { text: "Пиксель, " + georef.units; color: Theme.muted; font.pixelSize: 11 }
                    TextField {
                        id: resolutionField
                        objectName: "georefResolution"
                        Layout.fillWidth: true; implicitHeight: 28; font.pixelSize: 11
                        placeholderText: "авто"
                        validator: DoubleValidator { bottom: 0; locale: "ru_RU" }
                        ToolTip.visible: hovered; ToolTip.delay: 600; ToolTip.text: "Пусто — как у снимка"
                    }
                }
                FlatButton {
                    objectName: "georefRectify"
                    Layout.fillWidth: true; Layout.leftMargin: 10; Layout.rightMargin: 10
                    emphasis: true
                    text: georef.busy ? "Создание… " + Math.round(georef.progress * 100) + " %" : "Создать GeoTIFF"
                    enabled: georef.fitted && !georef.busy
                    onClicked: {
                        var r = resolutionField.text.length > 0 ? Number.fromLocaleString(Qt.locale("ru_RU"), resolutionField.text) : 0
                        var e = georef.rectify(isNaN(r) ? 0 : r)
                        if (e.length) panel.host.showNotice(e)
                    }
                    ToolTip.visible: hovered; ToolTip.delay: 600
                    ToolTip.text: "Новый файл в папке проекта, сразу на карте. Исходный не меняется"
                }
                ProgressBar {
                    visible: georef.busy
                    Layout.fillWidth: true; Layout.leftMargin: 10; Layout.rightMargin: 10
                    value: georef.progress
                }
                FlatButton { visible: georef.busy; compact: true; caution: true; text: "Отменить"; Layout.leftMargin: 10; onClicked: georef.cancel() }

                // --- Legend.
                RowLayout {
                    Layout.leftMargin: 12; Layout.topMargin: 2
                    spacing: 5
                    Cross { tint: "#3CDC5A" }
                    Label { text: "снимок"; color: Theme.muted; font.pixelSize: 11; rightPadding: 6 }
                    Cross { tint: "#FF3C3C" }
                    Label { text: "карта"; color: Theme.muted; font.pixelSize: 11; rightPadding: 6 }
                    Rectangle { implicitWidth: 14; implicitHeight: 2; color: "#3C8CFF" }
                    Label { text: "ошибка"; color: Theme.muted; font.pixelSize: 11 }
                }
                Item { implicitHeight: 8 }
            }
        }
    }
}
