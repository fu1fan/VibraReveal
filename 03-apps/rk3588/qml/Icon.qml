import QtQuick 2.15
Image {
    property string name: ""
    property string tone: "dark"
    width: 24
    height: 24
    source: name.length ? "qrc:/qml/icons/"+name+"-"+tone+".svg" : ""
    sourceSize: Qt.size(width, height)
    fillMode: Image.PreserveAspectFit
    // Decorative; the surrounding control supplies its accessible name.
    Accessible.ignored: true
}
