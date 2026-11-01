#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QDebug>
#include <memory>

// Exercise the host moc tool as well as target headers/libraries in SDK mode.
class Probe : public QObject {
    Q_OBJECT
};

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(R"(
import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
ApplicationWindow {
    width: 1024; height: 600
    ColumnLayout {
        Button { text: "VibraReveal" }
        Slider { from: 0.25; to: 4; value: 1 }
    }
}
)", QUrl());
    std::unique_ptr<QObject> root(component.create());
    if (!root) { qCritical() << component.errors(); return 1; }
    return root->property("width").toInt() == 1024 &&
           root->property("height").toInt() == 600 ? 0 : 2;
}

#include "main.moc"
