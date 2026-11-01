#include "controller.hpp"
#include "frame_view.hpp"
#include <QCommandLineParser>
#include <QDir>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStandardPaths>
#include <cmath>
#include <csignal>
#include <iostream>
namespace {
volatile std::sig_atomic_t quitRequested = 0;
void requestQuit(int) { quitRequested = 1; }
} // namespace
int main(int argc, char **argv) {
    if (QString::fromLatin1(qVersion()) != QStringLiteral("5.15.10")) {
        std::cerr << "Require Qt runtime 5.15.10; found " << qVersion() << std::endl;
        return 2;
    }
    QGuiApplication::setAttribute(Qt::AA_EnableHighDpiScaling, false);
    QGuiApplication app(argc, argv);
    app.setApplicationName("VibraReveal");
    app.setOrganizationName("VibraReveal");
    // Stock ELF 2 Fusion/ProgressBar.qml crashes its QML compiler. The UI
    // supplies its own high-contrast styling on top of the Default controls.
    QQuickStyle::setStyle("Default");
    QCommandLineParser args;
    args.setApplicationDescription("RK3588 Qt 5.15.10 touch measurement application");
    args.addHelpOption();
    args.addOption({"demo", "Use explicit synthetic camera; no hardware access"});
    args.addOption({"windowed", "Use a window instead of full screen"});
    args.addOption({"manage-usb", "Own ffs.vri gadget sessions (reenumerates composite USB; requires root)"});
    args.addOption({"device", "V4L2 camera node", "path", "/dev/video11"});
    args.addOption({"sensor", "V4L2 sensor node", "path", "/dev/v4l-subdev2"});
    args.addOption({"width", "Source width", "pixels", "1280"});
    args.addOption({"height", "Source height", "pixels", "800"});
    args.addOption({"bits", "Valid grayscale bits (camera 8 or 10)", "bits", "10"});
    args.addOption({"demo-fps", "Synthetic source rate only", "fps", "120"});
    args.addOption({"shader", "Compiled motion shader", "path", VR_MOTION_SHADER_PATH});
    args.addOption({"encoder", "MobileSAM RKNN encoder", "path"});
    args.addOption({"decoder", "MobileSAM RKNN decoder", "path"});
    args.addOption({"storage", "RAW and offline scratch directory", "path", "/userdata/vibrareveal"});
    args.addOption({"transport", "Computer transport: bulk or tcp", "mode", "bulk"});
    args.addOption({"ffs", "Prepared FunctionFS mount", "path", "/dev/ffs-vri"});
    args.addOption(
        {"bind", "TCP interface address (no authentication; defaults to loopback)", "address", "127.0.0.1"});
    args.addOption({"port", "TCP listening port", "port", "5910"});
    args.addOption({"exit-after", "Quit after N seconds (smoke test only)", "seconds", "0"});
    args.addOption({"screenshot", "Save a screenshot before smoke-test exit", "path"});
    args.process(app);
    try {
        vr::touch::EngineOptions o;
        o.demo = args.isSet("demo");
        o.device = args.value("device").toStdString();
        o.sensor = args.value("sensor").toStdString();
        auto integer = [&](const char *name, unsigned min, unsigned max) {
            bool ok = false;
            unsigned value = args.value(name).toUInt(&ok);
            if (!ok || value < min || value > max)
                throw std::runtime_error(std::string("Invalid --") + name);
            return value;
        };
        o.width = integer("width", 8, 4096);
        o.height = integer("height", 8, 4096);
        o.bits = integer("bits", 8, 10);
        if (o.bits != 8 && o.bits != 10 || uint64_t(o.width) * o.height > 1048576)
            throw std::runtime_error("Use RAW8/RAW10, at most 1048576 pixels");
        bool ok = false;
        o.demoFps = args.value("demo-fps").toDouble(&ok);
        if (!ok || !std::isfinite(o.demoFps) || o.demoFps < 1 || o.demoFps > 1000)
            throw std::runtime_error("Invalid demo FPS");
        o.shader = args.value("shader").toStdString();
        o.encoder = args.value("encoder").toStdString();
        o.decoder = args.value("decoder").toStdString();
        o.storage = args.value("storage").toStdString();
        if (o.encoder.empty() != o.decoder.empty())
            throw std::runtime_error("Supply both MobileSAM model paths");
        o.stream.transport = args.value("transport").toStdString();
        if (o.stream.transport != "bulk" && o.stream.transport != "tcp")
            throw std::runtime_error("Invalid transport");
        o.stream.manageGadget = args.isSet("manage-usb");
        o.stream.ffs = args.value("ffs").toStdString();
        o.stream.bind = args.value("bind").toStdString();
        o.stream.port = uint16_t(integer("port", 1, 65535));
        vr::touch::Controller controller(o);
        qmlRegisterUncreatableType<vr::touch::Controller>("VibraReveal", 1, 0, "Controller",
                                                          "Provided by application");
        qmlRegisterType<vr::touch::FrameView>("VibraReveal", 1, 0, "FrameView");
        QQmlApplicationEngine qml;
        qml.rootContext()->setContextProperty("backend", &controller);
        qml.load(QUrl("qrc:/qml/Main.qml"));
        if (qml.rootObjects().isEmpty())
            return 2;
        auto window = qobject_cast<QQuickWindow *>(qml.rootObjects().first());
        if (window && !args.isSet("windowed"))
            window->showFullScreen();
        std::signal(SIGINT, requestQuit);
        std::signal(SIGTERM, requestQuit);
        QTimer signalTimer;
        QObject::connect(&signalTimer, &QTimer::timeout, &app, [&] {
            if (quitRequested) {
                controller.engine().recorder.stop("程序退出");
                if (controller.canClose())
                    app.quit();
            }
        });
        signalTimer.start(100);
        QSharedPointer<QQuickItemGrabResult> screenshot;
        const auto seconds = integer("exit-after", 0, 86400);
        if (seconds)
            QTimer::singleShot(int(seconds * 1000), &app, [&] {
                if (window && !args.value("screenshot").isEmpty()) {
                    screenshot = window->contentItem()->grabToImage();
                    if (!screenshot) {
                        std::cerr << "Screenshot request failed\n";
                        app.exit(3);
                        return;
                    }
                    QObject::connect(screenshot.data(), &QQuickItemGrabResult::ready, &app, [&] {
                        const bool saved = screenshot->image().save(args.value("screenshot"));
                        if (!saved)
                            std::cerr << "Screenshot save failed\n";
                        app.exit(saved ? 0 : 3);
                    });
                    QTimer::singleShot(5000, &app, [&] {
                        std::cerr << "Screenshot timed out\n";
                        app.exit(3);
                    });
                } else
                    app.quit();
            });
        return app.exec();
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
