import QtQuick 2.15
import QtQuick.Controls 2.15
Pane {
    id: panel
    property string title: ""
    property string subtitle: "仅在本机显示"
    property bool closable: true
    signal closed()
    width: 302
    height: contentHeight + 100
    padding: 18
    topPadding: 78
    bottomPadding: 22
    // Use Controls' content routing; a default alias to a child data list
    // crashes the stock ELF 2 Qt 5.15.10 QML compiler.
    contentItem: Column { spacing: 12 }
    background: Rectangle {
        radius: 18
        color: "#f7f8fa"
        Column {
            x: 18; y: 14; width: parent.width - 76; spacing: 3
            Text { text: panel.subtitle; color: "#414e60"; font.pixelSize: 13; width: parent.width; elide: Text.ElideRight }
            Text { text: panel.title; color: "#151a22"; font.pixelSize: 21; font.weight: Font.DemiBold }
        }
        TouchButton { anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 8; width: 50; text: "×"; visible: panel.closable; Accessible.name: "关闭面板"; onClicked: panel.closed() }
    }
}
