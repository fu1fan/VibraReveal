import QtQuick 2.15
import QtQuick.Controls 2.15
Button {
    id: control
    property bool primary: false
    property bool danger: false
    property string glyph: ""
    property bool iconAbove: false
    property bool iconOnly: false
    property bool iconAfter: false
    property string glyphTone: primary || danger ? "light" : checked ? "accent" : "dark"
    implicitHeight: 50
    implicitWidth: iconOnly ? 50 : Math.max(50, label.implicitWidth + 28 + (glyph.length && !iconAbove ? 28 : 0))
    padding: iconAbove ? 8 : 12
    font.pixelSize: 16
    focusPolicy: Qt.StrongFocus
    Accessible.name: text
    background: Rectangle {
        radius: 12
        color: control.danger ? "#b72c35" : (control.primary ? "#075ac5" : control.checked ? "#dce5f0" : "#e9edf2")
        opacity: control.enabled ? 1 : 0.45
        border.width: control.activeFocus ? 2 : 0
        border.color: "#075ac5"
    }
    contentItem: Item {
        opacity: control.enabled ? 1 : 0.65
        Icon {
            id: symbol
            name: control.glyph; tone: control.glyphTone
            width: control.iconAbove ? 22 : 20; height: width
            visible: name.length > 0
            x: control.iconOnly || control.iconAbove ? (parent.width-width)/2 : control.iconAfter ? parent.width-width : 0
            y: control.iconAbove ? (parent.height-height-6-label.implicitHeight)/2 : (parent.height-height)/2
        }
        Text {
            id: label
            visible: !control.iconOnly
            text: control.text; font: control.font
            color: control.primary || control.danger ? "#ffffff" : "#151a22"
            x: symbol.visible && !control.iconAbove && !control.iconAfter ? symbol.width+8 : 0
            y: control.iconAbove ? symbol.y+symbol.height+6 : 0
            width: parent.width-(symbol.visible && !control.iconAbove ? symbol.width+8 : 0)
            height: control.iconAbove ? implicitHeight : parent.height
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            wrapMode: Text.Wrap
        }
    }
}
