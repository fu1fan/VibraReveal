import QtQuick 2.15
import QtQuick.Controls 2.15
Button {
    id: control
    property bool primary: false
    property bool danger: false
    implicitHeight: 50
    implicitWidth: Math.max(50, label.implicitWidth + 28)
    padding: 12
    font.pixelSize: 16
    focusPolicy: Qt.StrongFocus
    background: Rectangle {
        radius: 12
        color: control.danger ? "#b72c35" : (control.primary ? "#075ac5" : control.checked ? "#dce5f0" : "#e9edf2")
        opacity: control.enabled ? 1 : 0.45
        border.width: control.activeFocus ? 2 : 0
        border.color: "#075ac5"
    }
    contentItem: Text {
        id: label
        text: control.text
        font: control.font
        color: control.primary || control.danger ? "#ffffff" : "#151a22"
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        wrapMode: Text.Wrap
        opacity: control.enabled ? 1 : 0.65
    }
}
