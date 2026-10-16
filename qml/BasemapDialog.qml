import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Background map under the map layers: an online one (street map or satellite imagery, tiles
// downloaded as the view needs them) or a file. Every online server is used only after its terms
// are accepted; its attribution stays on the map.
Dialog {
    id: dialog
    objectName: "basemapDialog"
    title: "Подложка"
    modal: true
    anchors.centerIn: Overlay.overlay
    width: 560
    padding: 16
    standardButtons: Dialog.Close
    property var host
    property var providers: []
    property string chosen: ""
    function openFor(id) {
        providers = gis.basemapProviders()
        chosen = id || ""
        if (chosen === "")
            for (var i = 0; i < providers.length; ++i) if (providers[i].current) chosen = providers[i].id
        if (chosen === "" && providers.length > 0) chosen = providers[0].id
        accept.checked = false
        open()
    }
    function info() {
        for (var i = 0; i < providers.length; ++i) if (providers[i].id === chosen) return providers[i]
        return null
    }
    function apply() {
        var p = info()
        if (!p) return
        if (!p.accepted) {
            if (!accept.checked) return
            gis.acceptBasemapTerms(p.id)
        }
        var e = gis.setOnlineBasemap(p.id)
        if (e.length > 0) dialog.host.showError(e)
        else { dialog.host.activeView = 3; dialog.close() }
    }

    contentItem: ColumnLayout {
        spacing: 8
        Repeater {
            model: dialog.providers
            delegate: RadioButton {
                required property var modelData
                Layout.fillWidth: true
                checked: dialog.chosen === modelData.id
                text: modelData.title
                font.pixelSize: 12
                onClicked: { dialog.chosen = modelData.id; accept.checked = false }
            }
        }
        Label {
            Layout.fillWidth: true; wrapMode: Text.Wrap
            visible: dialog.info() !== null
            text: dialog.info() ? "© " + dialog.info().attribution.replace(/^© /, "") : ""
            color: Theme.muted; font.pixelSize: 11
        }
        CheckBox {
            id: accept
            Layout.fillWidth: true
            visible: dialog.info() !== null && !dialog.info().accepted
            font.pixelSize: 11
            contentItem: Label {
                leftPadding: accept.indicator.width + 6
                wrapMode: Text.Wrap
                textFormat: Text.StyledText
                text: dialog.info() ? "Принимаю <a href=\"" + dialog.info().terms + "\">условия использования</a> этого сервера" : ""
                color: Theme.text; font.pixelSize: 11
                linkColor: Theme.accentHover
                onLinkActivated: function(link) { Qt.openUrlExternally(link) }
            }
        }
        RowLayout {
            spacing: 8
            FlatButton {
                text: "Показать на карте"; emphasis: true
                enabled: dialog.info() !== null && (dialog.info().accepted || accept.checked)
                onClicked: dialog.apply()
            }
            FlatButton { text: "Из файла…"; onClicked: { dialog.close(); dialog.host.addBasemapFile() } }
            Item { Layout.fillWidth: true }
            FlatButton {
                visible: gis.hasBasemap
                text: gis.basemapVisible ? "Скрыть" : "Показать"
                onClicked: gis.setBasemapVisible(!gis.basemapVisible)
            }
        }
    }
}
