#include "controller.hpp"
#include "frame_view.hpp"
#include "backend_io.hpp"
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QtTest>
using namespace vr::touch;
class UiTest : public QObject {
    Q_OBJECT
  private slots:
    // Explicit opt-in: requires exclusive ownership of the physical camera.
    void hardwareCapture() {
        if (!qEnvironmentVariableIsSet("VR_TEST_CAMERA"))
            QSKIP("Set VR_TEST_CAMERA=1 on the board after stopping the camera service");
        const auto storage = qEnvironmentVariable("VR_TEST_STORAGE");
        QVERIFY2(!storage.isEmpty(), "Set VR_TEST_STORAGE to an eMMC directory with free space");
        EngineOptions options;
        options.storage = storage.toStdString();
        options.shader = qEnvironmentVariable("VR_TEST_SHADER_PATH", VR_TEST_SHADER).toStdString();
        options.stream.manageGadget = qEnvironmentVariableIsSet("VR_TEST_USB");
        Controller controller(options);
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("backend", &controller);
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->showFullScreen();
        QTRY_VERIFY_WITH_TIMEOUT(controller.state()["ready"].toBool(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.image(0).isNull(), 10000);
        const auto initial = controller.engine().snapshot();
        QVERIFY(!initial.format.synthetic);
        QCOMPARE(initial.format.width, 1280u);
        QCOMPARE(initial.format.height, 800u);
        QCOMPARE(initial.format.bits, 10u);
        QCOMPARE(initial.format.stride, 2560u);
        QTRY_VERIFY_WITH_TIMEOUT(controller.engine().snapshot().sequence > initial.sequence + 30, 5000);
        controller.tool("focus");
        QVERIFY(controller.state()["native"].toBool());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.overlay().isNull(), 10000);
        const auto captures = qEnvironmentVariable("VR_UI_CAPTURE_DIR");
        if (!captures.isEmpty()) {
            QDir().mkpath(captures);
            auto grab = window->contentItem()->grabToImage();
            QVERIFY(grab);
            QSignalSpy ready(grab.data(), &QQuickItemGrabResult::ready);
            QVERIFY(ready.wait(5000) || !grab->image().isNull());
            QVERIFY(grab->image().save(captures + "/camera-focus.png"));
        }
        controller.tool("record");
        controller.record();
        QTRY_COMPARE(controller.state()["recordState"].toString(), QString("recording"));
        QTest::qWait(2000);
        controller.stopRecording();
        QTRY_COMPARE_WITH_TIMEOUT(controller.state()["recordState"].toString(), QString("saved"), 15000);
        const auto recorded = controller.engine().recorder.status();
        QVERIFY2(recorded.frames >= 100, recorded.reason.c_str());
        vr::ArchiveReader archive(recorded.path);
        uint64_t count = 0;
        Frame frame;
        while (archive.read(frame))
            ++count;
        QCOMPARE(count, recorded.frames);
        controller.refreshFiles();
        QVERIFY(!controller.files().isEmpty());
        controller.openFile(0);
        QTRY_VERIFY_WITH_TIMEOUT(controller.state()["replayReady"].toBool(), 10000);
        qInfo() << "BOARD_CAMERA_RECORD_REPLAY" << recorded.frames << "frames"
                << initial.format.fps() << "fps" << QString::fromStdString(recorded.path);
        if (qEnvironmentVariableIsSet("VR_TEST_USB")) {
            controller.mode("direct");
            QCOMPARE(controller.state()["mode"].toString(), QString("direct"));
            QVERIFY(controller.state()["native"].toBool());
            QTRY_VERIFY_WITH_TIMEOUT(controller.engine().streaming.status().frames >= 120 ||
                                         controller.engine().streaming.status().state == "error", 20000);
            QVERIFY2(controller.engine().streaming.status().frames >= 120,
                     controller.engine().streaming.status().message.c_str());
            if (!captures.isEmpty()) {
                auto grab = window->contentItem()->grabToImage();
                QVERIFY(grab);
                QSignalSpy ready(grab.data(), &QQuickItemGrabResult::ready);
                QVERIFY(ready.wait(5000) || !grab->image().isNull());
                QVERIFY(grab->image().save(captures + "/camera-usb-direct.png"));
            }
            controller.mode("local");
            QTRY_VERIFY_WITH_TIMEOUT(!controller.engine().streaming.status().busy, 20000);
            QCOMPARE(QString::fromStdString(controller.engine().streaming.status().state), QString("complete"));
            qInfo() << "BOARD_USB_DIRECT_COMPLETE" << controller.engine().streaming.status().frames;
        }
    }
    void workflow() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        EngineOptions options;
        options.demo = true;
        options.width = 320;
        options.height = 200;
        options.bits = 8;
        options.demoFps = 60;
        options.storage = dir.path().toStdString();
        options.shader = qEnvironmentVariable("VR_TEST_SHADER_PATH", VR_TEST_SHADER).toStdString();
        options.stream.transport = "tcp";
        options.stream.port = 29876;
        Controller controller(options);
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("backend", &controller);
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        QCOMPARE(window->width(), 1024);
        QCOMPARE(window->height(), 600);
        QCOMPARE(QString::fromLatin1(qVersion()), QString("5.15.10"));
        QTRY_VERIFY_WITH_TIMEOUT(controller.state()["ready"].toBool(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.image(0).isNull(), 10000);
        FrameView mapping;
        mapping.setController(&controller);
        mapping.setSize(QSizeF(160, 100));
        QCOMPARE(mapping.toSource(80, 50), QPointF(160, 100));
        mapping.setNativeSize(true);
        QCOMPARE(mapping.toSource(0, 0), QPointF(80, 50));
        mapping.panBy(1000, 1000);
        QCOMPARE(mapping.toSource(0, 0), QPointF(0, 0));
        mapping.panBy(-2000, -2000);
        QCOMPARE(mapping.toSource(0, 0), QPointF(160, 100));
        auto click = [&](const char *name) {
            std::function<QQuickItem *(QQuickItem *)> find = [&](QQuickItem *parent) -> QQuickItem * {
                if (parent->objectName() == name)
                    return parent;
                for (auto child : parent->childItems())
                    if (auto found = find(child))
                        return found;
                return nullptr;
            };
            auto item = find(window->contentItem());
            if (!item)
                window->dumpObjectTree();
            QVERIFY2(item, name);
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                              item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
        };
        auto capture = [&](const QString &name) {
            const auto directory = qEnvironmentVariable("VR_UI_CAPTURE_DIR");
            if (directory.isEmpty())
                return;
            QDir().mkpath(directory);
            QTest::qWait(100);
            auto grab = window->contentItem()->grabToImage();
            QVERIFY(grab);
            QSignalSpy ready(grab.data(), &QQuickItemGrabResult::ready);
            QVERIFY(ready.wait(5000) || !grab->image().isNull());
            QVERIFY(grab->image().save(directory + "/" + name + ".png"));
        };
        click("tool_focus");
        QTRY_COMPARE(controller.state()["tool"].toString(), QString("focus"));
        QVERIFY(controller.state()["native"].toBool());
        controller.view(false);
        QVERIFY(controller.state()["native"].toBool());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.overlay().isNull(), 10000);
        capture("focus");
        click("finishFocus");
        QTRY_VERIFY(!controller.state()["native"].toBool());
        click("tool_record");
        click("recordAction");
        QTRY_COMPARE(controller.state()["recordState"].toString(), QString("recording"));
        QTest::qWait(250);
        capture("recording");
        controller.tool("focus");
        QCOMPARE(controller.state()["tool"].toString(), QString("record"));
        controller.stopRecording();
        QTRY_COMPARE_WITH_TIMEOUT(controller.state()["recordState"].toString(), QString("saved"), 10000);
        QVERIFY(controller.files().size() > 0);
        controller.tool("files");
        controller.openFile(0);
        QTRY_COMPARE(controller.state()["tool"].toString(), QString("replay"));
        QTRY_VERIFY_WITH_TIMEOUT(controller.state()["replayReady"].toBool(), 10000);
        controller.tool("select");
        controller.selectObject(100, 85, 130, 30, false);
        QTRY_VERIFY_WITH_TIMEOUT(controller.state()["objectReady"].toBool(), 15000);
        controller.confirmObject();
        controller.selectEdge(160, 96);
        QTRY_VERIFY_WITH_TIMEOUT(controller.state()["edgeReady"].toBool(), 10000);
        controller.confirmEdge();
        QCOMPARE(controller.state()["tool"].toString(), QString("replay"));
        controller.analyze();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.state()["offlineBusy"].toBool(), 30000);
        QVERIFY2(!controller.state()["offlineOutput"].toString().isEmpty(),
                 qPrintable(controller.state()["error"].toString()));
        QVERIFY(QFileInfo::exists(controller.state()["offlineOutput"].toString()));
        capture("offline");
        controller.tool("preview");
        controller.tool("select");
        controller.selectObject(100, 85, 130, 30, false);
        QTRY_VERIFY_WITH_TIMEOUT(controller.state()["objectReady"].toBool(), 10000);
        controller.confirmObject();
        controller.selectEdge(160, 96);
        QTRY_VERIFY_WITH_TIMEOUT(controller.state()["edgeReady"].toBool(), 10000);
        controller.confirmEdge();
        controller.tool("magnify");
        QTRY_VERIFY_WITH_TIMEOUT(controller.state()["spectrumReady"].toBool() ||
                                    !controller.state()["error"].toString().isEmpty(), 15000);
        QVERIFY2(controller.state()["spectrumReady"].toBool(),
                 qPrintable(controller.state()["error"].toString()));
        QVERIFY(std::abs(controller.state()["peak"].toDouble() - 12) < 0.5);
        capture("magnify");
        controller.tool("preview");
        controller.mode("direct");
        QTRY_COMPARE(controller.state()["mode"].toString(), QString("direct"));
        QVERIFY(controller.state()["native"].toBool());
        controller.tool("magnify");
        QCOMPARE(controller.state()["mode"].toString(), QString("direct"));
        controller.mode("local");
        QTRY_VERIFY_WITH_TIMEOUT(!controller.state()["streamBusy"].toBool(), 5000);
    }
};
int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle("Default");
    qmlRegisterUncreatableType<Controller>("VibraReveal", 1, 0, "Controller", "Provided");
    qmlRegisterType<FrameView>("VibraReveal", 1, 0, "FrameView");
    UiTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "ui_test.moc"
