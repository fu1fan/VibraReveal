import QtQuick 2.15
import QtQuick.Controls 2.15
Pane {
    id: panel
    property string title: ""
    property string subtitle: "仅在本机显示"
    property bool closable: true
    signal closed()
    signal dragged(real dx, real dy)
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
        Rectangle { anchors.horizontalCenter: parent.horizontalCenter; y: 5; width: 36; height: 4; radius: 2; color: "#596677" }
        MouseArea {
            objectName: "panelDragHandle"
            x: 0; y: 0; width: parent.width-(panel.closable?66:0); height: 72
            preventStealing: true; cursorShape: Qt.SizeAllCursor
            property point previous
            onPressed: previous = mapToItem(null,mouse.x,mouse.y)
            onPositionChanged: {
                if (!pressed) return
                var at = mapToItem(null,mouse.x,mouse.y)
                panel.dragged(at.x-previous.x,at.y-previous.y)
                previous = at
            }
        }
        Column {
            x: 18; y: 14; width: parent.width - 76; spacing: 3
            Text { text: panel.subtitle; color: "#414e60"; font.pixelSize: 13; width: parent.width; elide: Text.ElideRight }
            Text { text: panel.title; color: "#151a22"; font.pixelSize: 21; font.weight: Font.DemiBold }
        }
        TouchButton { anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 8; width: 50; glyph: "x"; iconOnly: true; text: "关闭面板"; visible: panel.closable; Accessible.name: "关闭面板"; onClicked: panel.closed() }
    }
}
