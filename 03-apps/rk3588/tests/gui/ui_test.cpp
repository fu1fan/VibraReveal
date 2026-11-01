#include "controller.hpp"
#include "frame_view.hpp"
#include "focus_peaking.hpp"
#include "vulkan_motion.hpp"
#include "backend_io.hpp"
#include "motion_budget.hpp"
#include "sensor_controls.hpp"
#include <cstring>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
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
    void panelMenuClearance() {
        QTemporaryDir dir;
        EngineOptions options; options.demo=true; options.width=320; options.height=200;
        options.bits=8; options.demoFps=60; options.storage=dir.path().toStdString();
        options.shader=qEnvironmentVariable("VR_TEST_SHADER_PATH",VR_TEST_SHADER).toStdString();
        Controller controller(options);
        QQmlApplicationEngine engine; engine.rootContext()->setContextProperty("backend",&controller);
        engine.load(QUrl("qrc:/qml/Main.qml")); QVERIFY(!engine.rootObjects().isEmpty());
        auto window=qobject_cast<QQuickWindow *>(engine.rootObjects().first()); QVERIFY(window); window->showFullScreen();
        QTRY_VERIFY_WITH_TIMEOUT(controller.state()["ready"].toBool(),5000);
        auto panel=window->findChild<QQuickItem *>("inspector"); QVERIFY(panel);
        auto check=[&](QQuickItem *item) {
            QTest::qWait(100);
            const QRectF box(item->mapToScene(QPointF()),item->size());
            QVERIFY(box.top()>=78); QVERIFY(box.bottom()<=window->height()-8);
            for(const auto name : {"toolDock","playbackBar","modeMenu","settingsButton"}) {
                auto menu=window->findChild<QQuickItem *>(name); QVERIFY(menu);
                if(menu->isVisible()) QVERIFY2(!box.intersects(QRectF(menu->mapToScene(QPointF()),menu->size())),name);
            }
        };
        for(const auto &tool : {"focus","select","record","settings"}) { controller.tool(tool); check(panel); }
        controller.tool("select"); QVERIFY(window->setProperty("frequencyPanel",true)); check(panel);
        QVERIFY(panel->property("contentHeight").toDouble()>panel->height());
        auto device=QTest::createTouchDevice();
        const QPoint from=panel->mapToScene(QPointF(8,panel->height()-30)).toPoint();
        const QPoint to=from-QPoint(0,180); const QPointF before(panel->x(),panel->y());
        QTest::touchEvent(window,device).press(0,from,window);
        for(int i=1;i<=6;++i) { QTest::touchEvent(window,device).move(0,from+(to-from)*i/6,window); QTest::qWait(30); }
        QTest::touchEvent(window,device).release(0,to,window); QTest::qWait(300);
        QVERIFY(panel->property("contentY").toDouble()>0); QCOMPARE(QPointF(panel->x(),panel->y()),before);
        const auto captures=qEnvironmentVariable("VR_UI_CAPTURE_DIR");
        if(!captures.isEmpty()) {
            QDir().mkpath(captures); auto grab=window->contentItem()->grabToImage(); QVERIFY(grab);
            QSignalSpy ready(grab.data(),&QQuickItemGrabResult::ready); QVERIFY(ready.wait(5000)||!grab->image().isNull());
            QVERIFY(grab->image().save(captures+"/panel-scroll-clearance.png"));
        }
        QVERIFY(window->setProperty("frequencyPanel",false));
        controller.selectObject(40,70,64,64,false);
        QTRY_VERIFY_WITH_TIMEOUT(controller.state()["objectReady"].toBool(),5000);
        controller.magnifyRegion(); QTRY_VERIFY_WITH_TIMEOUT(!controller.image(1).isNull(),10000); check(panel);
        controller.tool("preview"); controller.record();
        QTRY_VERIFY_WITH_TIMEOUT(controller.engine().recorder.status().frames>=8,5000);
        controller.stopRecording(); QTRY_COMPARE_WITH_TIMEOUT(controller.state()["recordState"].toString(),QString("saved"),5000); check(panel);
        controller.tool("files"); check(window->findChild<QQuickItem *>("filesPanel"));
        controller.openFile(0); QTRY_VERIFY_WITH_TIMEOUT(controller.state()["replayReady"].toBool(),5000); check(panel);
        QVERIFY(window->setProperty("frequencyPanel",true)); check(panel);
        QVERIFY(window->setProperty("frequencyPanel",false)); QVERIFY(window->setProperty("rangePanel",true)); check(panel);
    }
    void gainControls() {
        QTemporaryDir dir;
        EngineOptions options; options.demo=true; options.width=160; options.height=120;
        options.bits=8; options.demoFps=60; options.storage=dir.path().toStdString();
        options.shader=qEnvironmentVariable("VR_TEST_SHADER_PATH",VR_TEST_SHADER).toStdString();
        Controller controller(options);
        QQmlApplicationEngine engine; engine.rootContext()->setContextProperty("backend",&controller);
        engine.load(QUrl("qrc:/qml/Main.qml")); QVERIFY(!engine.rootObjects().isEmpty());
        auto window=qobject_cast<QQuickWindow *>(engine.rootObjects().first()); QVERIFY(window); window->showFullScreen();
        QTRY_VERIFY_WITH_TIMEOUT(controller.state()["ready"].toBool(),5000);
        std::function<QQuickItem *(QQuickItem *,const QString &)> find = [&](QQuickItem *parent,const QString &name) -> QQuickItem * {
            if(parent->objectName()==name) return parent;
            for(auto child:parent->childItems()) if(auto found=find(child,name)) return found;
            return nullptr;
        };
        auto click=[&](const QString &name) {
            auto item=find(window->contentItem(),name); QVERIFY2(item,qPrintable(name)); QVERIFY(item->isVisible()); QVERIFY(item->isEnabled());
            auto panel=find(window->contentItem(),"inspector");
            if(panel && panel->isAncestorOf(item)) {
                const auto top=item->mapToItem(panel,QPointF()).y();
                const double offset=panel->property("contentY").toDouble();
                if(top<0 || top+item->height()>panel->height())
                    panel->setProperty("contentY",std::clamp(offset+top+item->height()-panel->height(),0.,std::max(0.,panel->property("contentHeight").toDouble()-panel->height())));
                QTest::qWait(50);
            }
            QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,item->mapToScene(QPointF(item->width()/2,item->height()/2)).toPoint());
            QTest::qWait(50);
        };
        auto dragGain=[&](double fraction) {
            auto slider=find(window->contentItem(),"parameterGain"); QVERIFY(slider); QVERIFY(slider->height()>=48);
            const auto start=slider->mapToScene(QPointF(slider->width()/2,slider->height()/2)).toPoint();
            const auto end=slider->mapToScene(QPointF(fraction*(slider->width()-1),slider->height()/2)).toPoint();
            QTest::mousePress(window,Qt::LeftButton,Qt::NoModifier,start);
            QTest::mouseMove(window,end,80); QTest::mouseRelease(window,Qt::LeftButton,Qt::NoModifier,end);
        };
        auto capture=[&](const QString &name) {
            const auto output=qEnvironmentVariable("VR_UI_CAPTURE_DIR"); if(output.isEmpty()) return;
            QDir().mkpath(output); QTest::qWait(150); auto grab=window->contentItem()->grabToImage(); QVERIFY(grab);
            QSignalSpy ready(grab.data(),&QQuickItemGrabResult::ready); QVERIFY(ready.wait(5000)||!grab->image().isNull());
            QVERIFY(grab->image().save(output+"/"+name+".png"));
        };
        controller.tool("select"); click("selectionParameters"); dragGain(1);
        QCOMPARE(find(window->contentItem(),"parameterGain")->property("value").toDouble(),80.);
        capture("gain-before-live"); click("applyMotionParameters");
        QCOMPARE(controller.engine().snapshot().alpha,80.);
        controller.selectObject(32,30,64,64,false);
        QTRY_VERIFY_WITH_TIMEOUT(controller.state()["objectReady"].toBool(),5000);
        controller.magnifyRegion(); QTRY_VERIFY_WITH_TIMEOUT(!controller.image(1).isNull(),10000);
        QCOMPARE(controller.engine().snapshot().alpha,80.);
        controller.tool("preview"); controller.record();
        QTRY_VERIFY_WITH_TIMEOUT(controller.engine().recorder.status().frames>=12,5000);
        controller.stopRecording(); QTRY_COMPARE_WITH_TIMEOUT(controller.state()["recordState"].toString(),QString("saved"),5000);
        controller.tool("files"); controller.openFile(0);
        QTRY_VERIFY_WITH_TIMEOUT(controller.state()["replayReady"].toBool(),5000);
        click("offlineParameters");
        QCOMPARE(find(window->contentItem(),"parameterGain")->property("value").toDouble(),80.);
        dragGain(0); capture("gain-before-offline"); click("applyMotionParameters");
        QCOMPARE(controller.engine().snapshot().alpha,0.);
        // The RAW object-selection route must expose the same control.
        controller.tool("select"); click("selectionParameters"); click("applyMotionParameters");
        controller.seek(0); QTRY_VERIFY_WITH_TIMEOUT(controller.state()["replayReady"].toBool(),5000);
        controller.analysisRange(4./60); controller.analyzeGlobal();
        QVERIFY(controller.state()["offlineBusy"].toBool());
        auto button=find(window->contentItem(),"offlineParameters"); QVERIFY(button); QVERIFY(!button->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.state()["offlineBusy"].toBool(),30000);
        const auto path=controller.state()["offlineOutput"].toString();
        QVERIFY2(!path.isEmpty(),qPrintable(controller.state()["error"].toString()));
        QFile output(path); QVERIFY(output.open(QIODevice::ReadOnly));
        vr::ArchiveReader source(controller.files()[0].toMap()["path"].toString().toStdString()); Frame raw;
        // Zero gain must reach the actual offline algorithm, preserving source pixels.
        for(int i=0;i<4;++i) {
            QVERIFY(source.read(raw)); QCOMPARE(output.read(40).size(),40);
            auto pixels=output.read(raw.width*raw.height*4); QCOMPARE(pixels.size(),int(raw.width*raw.height*4));
            for(unsigned y=0;y<raw.height;++y) for(unsigned x=0;x<raw.width;++x) {
                float value; std::memcpy(&value,pixels.constData()+4*(y*raw.width+x),4);
                QVERIFY(std::abs(value-float(raw.data[y*raw.stride+x])/255.f)<.0001f);
            }
        }
        QVERIFY(output.atEnd());
        auto panel=find(window->contentItem(),"inspector"); QVERIFY(panel); QVERIFY(panel->height()<=584);
    }
    void regionAndClip() {
        QTemporaryDir dir;
        EngineOptions options; options.demo=true; options.width=320; options.height=200;
        options.bits=8; options.demoFps=60; options.storage=dir.path().toStdString();
        options.shader=qEnvironmentVariable("VR_TEST_SHADER_PATH",VR_TEST_SHADER).toStdString();
        Controller controller(options);
        QQmlApplicationEngine engine; engine.rootContext()->setContextProperty("backend",&controller);
        engine.load(QUrl("qrc:/qml/Main.qml")); QVERIFY(!engine.rootObjects().isEmpty());
        auto window=qobject_cast<QQuickWindow *>(engine.rootObjects().first()); QVERIFY(window); window->showFullScreen();
        QTRY_VERIFY_WITH_TIMEOUT(controller.state()["ready"].toBool(),5000);
        controller.tool("select"); controller.selectObject(32,70,100,64,false);
        QTRY_VERIFY_WITH_TIMEOUT(controller.state()["objectReady"].toBool(),5000);
        auto camera=window->findChild<FrameView *>("cameraView"); QVERIFY(camera);
        auto region=window->findChild<QQuickItem *>("editableRegion"); QVERIFY(region);
        auto drag=[&](const QPoint &from,const QPoint &to) {
            QTest::mousePress(window,Qt::LeftButton,Qt::NoModifier,from);
            QTest::mouseMove(window,(from+to)/2,60);
            QTest::mouseMove(window,to,60);
            QVERIFY(region->property("adjusting").toBool());
            const auto requested=region->property("draft").toRectF().toAlignedRect();
            QTest::mouseRelease(window,Qt::LeftButton,Qt::NoModifier,to);
            // The submitted geometry must be visible before async processing ends.
            QCOMPARE(controller.state()["roi"].toRect(),requested);
            QCOMPARE(region->property("region").toRectF().toAlignedRect(),requested);
        };
        drag(camera->toView(82,102).toPoint(),camera->toView(92,107).toPoint());
        QTRY_VERIFY_WITH_TIMEOUT(controller.state()["objectReady"].toBool(),5000);
        auto moved=controller.state()["roi"].toRect();
        QVERIFY(std::abs(moved.x()-42)<=1); QVERIFY(std::abs(moved.y()-75)<=1);
        drag(camera->toView(moved.x()+moved.width(),moved.y()+moved.height()).toPoint(),camera->toView(moved.x()+moved.width()+10,moved.y()+moved.height()+5).toPoint());
        QTRY_VERIFY_WITH_TIMEOUT(controller.state()["objectReady"].toBool(),5000);
        auto resized=controller.state()["roi"].toRect(); QVERIFY(resized.width()>moved.width()); QVERIFY(resized.height()>moved.height());
        QVERIFY(controller.state()["canChooseEdge"].toBool()); QVERIFY(!controller.engine().snapshot().overlay.isNull());
        auto capture=[&](const QString &name) {
            auto output=qEnvironmentVariable("VR_UI_CAPTURE_DIR"); if(output.isEmpty()) return;
            QDir().mkpath(output); QTest::qWait(150); auto grab=window->contentItem()->grabToImage(); QVERIFY(grab);
            QSignalSpy ready(grab.data(),&QQuickItemGrabResult::ready); QVERIFY(ready.wait(5000)||!grab->image().isNull()); QVERIFY(grab->image().save(output+"/"+name+".png"));
        };
        capture("region-edit");
        controller.magnifyRegion();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.image(1).isNull(),10000);
        QCOMPARE(controller.image(1).size(),resized.size()); QVERIFY(controller.state()["areaMode"].toBool()); QVERIFY(!controller.state()["spectrumReady"].toBool());
        controller.tool("preview"); controller.tool("select"); controller.selectObject(280,50,24,24,false);
        QTRY_VERIFY_WITH_TIMEOUT(controller.state()["objectReady"].toBool(),5000);
        QVERIFY(!controller.state()["canChooseEdge"].toBool());
        controller.magnifyRegion(); QTRY_VERIFY_WITH_TIMEOUT(!controller.image(1).isNull(),5000);
        QCOMPARE(controller.image(1).size(),QSize(24,24));
        controller.tool("preview"); controller.record();
        QTRY_VERIFY_WITH_TIMEOUT(controller.engine().recorder.status().frames>=48,5000);
        controller.stopRecording(); QTRY_COMPARE_WITH_TIMEOUT(controller.state()["recordState"].toString(),QString("saved"),5000);
        controller.tool("files"); controller.openFile(0);
        QTRY_VERIFY_WITH_TIMEOUT(controller.state()["replayReady"].toBool(),5000);
        const auto path=controller.files()[0].toMap()["path"].toString();
        std::vector<Frame> source; vr::ArchiveReader archive(path.toStdString()); Frame f;
        while(archive.read(f)) source.push_back(f);
        controller.seek(10./60); QTRY_COMPARE(controller.engine().snapshot().replayIndex,uint64_t(10));
        controller.tool("select"); controller.selectObject(32,70,100,64,false);
        QTRY_VERIFY_WITH_TIMEOUT(controller.state()["objectReady"].toBool(),5000);
        controller.magnifyRegion(); QCOMPARE(controller.state()["analysisStart"].toDouble(),10./60);
        controller.analysisRange(.2); controller.analyze();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.state()["offlineBusy"].toBool(),30000);
        auto verifyOutput=[&](unsigned first,unsigned count,unsigned width,unsigned height) {
            const auto output=controller.state()["offlineOutput"].toString();
            QVERIFY2(!output.isEmpty(),qPrintable(controller.state()["error"].toString()));
            QFile file(output); QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(file.size(),qint64(count)*(40+width*height*4));
            for(unsigned i=0;i<count;++i) {
                const auto h=file.read(40); QCOMPARE(h.left(4),QByteArray("VRV1"));
                const auto p=reinterpret_cast<const uint8_t *>(h.constData());
                QCOMPARE(vr::getLE(p+4,4),uint64_t(width)); QCOMPARE(vr::getLE(p+8,4),uint64_t(height));
                QCOMPARE(vr::getLE(p+16,8),source[first+i].sequence); QCOMPARE(vr::getLE(p+24,8),source[first+i].timestamp);
                QVERIFY(file.seek(file.pos()+width*height*4));
            }
        };
        verifyOutput(10,12,100,64); capture("region-clip-result");
        controller.seek(20./60); QTRY_COMPARE(controller.engine().snapshot().replayIndex,uint64_t(20));
        controller.analysisRange(.15); controller.analyzeGlobal();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.state()["offlineBusy"].toBool(),30000);
        verifyOutput(20,9,320,200); QVERIFY(!controller.state()["spectrumReady"].toBool());
        controller.seek((source.size()-4)/60.); QTRY_COMPARE(controller.engine().snapshot().replayIndex,uint64_t(source.size()-4));
        QTRY_VERIFY_WITH_TIMEOUT(controller.state()["replayReady"].toBool(),5000);
        controller.analyzeGlobal(); QTRY_VERIFY_WITH_TIMEOUT(!controller.state()["offlineBusy"].toBool(),30000);
        QVERIFY2(controller.state()["error"].toString().isEmpty(),qPrintable(controller.state()["error"].toString()));
        verifyOutput(source.size()-4,4,320,200);
        controller.seek((source.size()-1)/60.); QTRY_COMPARE(controller.engine().snapshot().replayIndex,uint64_t(source.size()-1));
        QTRY_VERIFY_WITH_TIMEOUT(controller.state()["replayReady"].toBool(),5000);
        controller.analyzeGlobal(); QVERIFY(!controller.state()["offlineBusy"].toBool());
        QVERIFY(controller.state()["error"].toString().contains("两帧"));
        controller.seek(20./60); QTRY_VERIFY_WITH_TIMEOUT(controller.state()["replayReady"].toBool(),5000);
        QVERIFY(window->setProperty("rangePanel",true)); capture("analysis-range");
        auto inspector=window->findChild<QQuickItem *>("inspector"); QVERIFY(inspector); QVERIFY(inspector->height()<=584);
        QVERIFY(window->setProperty("rangePanel",false)); capture("raw-analysis-options");
        // Corruption outside the selected interval must still prevent publication.
        QFile corrupt(path); QVERIFY(corrupt.open(QIODevice::ReadWrite)); QVERIFY(corrupt.seek(corrupt.size()-81));
        char value; QCOMPARE(corrupt.read(&value,1),qint64(1)); value^=1;
        QVERIFY(corrupt.seek(corrupt.size()-81)); QCOMPARE(corrupt.write(&value,1),qint64(1)); corrupt.close();
        controller.analyzeGlobal(); QTRY_VERIFY_WITH_TIMEOUT(!controller.state()["offlineBusy"].toBool(),30000);
        QVERIFY(controller.state()["offlineOutput"].toString().isEmpty()); QVERIFY(controller.state()["error"].toString().contains("CRC") || controller.state()["error"].toString().contains("Bitshuffle"));
    }

    void hardwareCompression() {
        if(!qEnvironmentVariableIsSet("VR_TEST_CAMERA")) QSKIP("Requires exclusive camera access");
        // Explicit eMMC directory: /tmp is tmpfs on the ELF 2 board.
        const auto storage=qEnvironmentVariable("VR_TEST_STORAGE");
        QVERIFY(!storage.isEmpty()); QVERIFY(QDir().mkpath(storage));
        QTemporaryDir dir(storage+"/compression-XXXXXX"); QVERIFY(dir.isValid());
        EngineOptions options; options.storage=dir.path().toStdString();
        options.shader=qEnvironmentVariable("VR_TEST_SHADER_PATH",VR_TEST_SHADER).toStdString();
        Controller controller(options);
        QQmlApplicationEngine engine; engine.rootContext()->setContextProperty("backend",&controller);
        engine.load(QUrl("qrc:/qml/Main.qml")); QVERIFY(!engine.rootObjects().isEmpty());
        auto window=qobject_cast<QQuickWindow *>(engine.rootObjects().first()); QVERIFY(window); window->showFullScreen();
        QTRY_VERIFY_WITH_TIMEOUT(controller.state()["exposureEditable"].toBool(),10000);
        const double originalFps=controller.state()["fps"].toDouble();
        controller.captureFps(120);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.state()["cameraBusy"].toBool(),10000);
        QVERIFY2(controller.state()["error"].toString().isEmpty(),qPrintable(controller.state()["error"].toString()));
        QTest::qWait(500);
        controller.tool("record"); controller.record();
        uint64_t peakBuffer=0;
        QElapsedTimer clock; clock.start();
        while(clock.elapsed()<30000) {
            QTest::qWait(200);
            const auto status=controller.engine().recorder.status();
            peakBuffer=std::max(peakBuffer,status.buffered);
            if(status.state!=RecordingStatus::Recording) break;
        }
        const auto during=controller.engine().recorder.status();
        controller.stopRecording();
        QTRY_COMPARE_WITH_TIMEOUT(controller.state()["recordState"].toString(),QString("saved"),30000);
        const auto result=controller.engine().recorder.status();
        controller.captureFps(originalFps);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.state()["cameraBusy"].toBool(),10000);
        QCOMPARE(during.state,RecordingStatus::Recording);
        QVERIFY2(result.frames>=3500,result.reason.c_str());
        QVERIFY(result.compressed); QCOMPARE(result.processed,result.payload);
        vr::ArchiveReader reader(result.path); Frame frame; uint64_t count=0;
        while(reader.read(frame)) ++count;
        QCOMPARE(count,result.frames); QCOMPARE(reader.frameCount(),count);
        const double measured=double(count)/result.seconds;
        QVERIFY(std::abs(measured-120)<1);
        qInfo()<<"BOARD_COMPRESSION"<<"frames"<<count<<"fps"<<measured<<"raw_bytes"<<result.payload
               <<"stored_payload_bytes"<<result.written<<"saved_percent"<<100.*(1.-double(result.written)/result.payload)
               <<"peak_buffer_bytes"<<peakBuffer<<"stop_reason"<<QString::fromStdString(result.reason);
        const auto fixture=qEnvironmentVariable("VR_TEST_COMPRESSION_FIXTURE");
        if(!fixture.isEmpty()) {
            Recording subset; Format format{reader.width,reader.height,reader.stride,reader.bits,reader.fpsNum,reader.fpsDen,false};
            subset.start(fixture.toStdString(),format);
            for(unsigned i=0;i<8;++i) { auto f=std::make_shared<Frame>(); reader.readAt(i,*f); QVERIFY(subset.submit(f)); }
            subset.stop(); subset.wait(); QCOMPARE(subset.status().state,RecordingStatus::Saved);
        }
        const auto output=qEnvironmentVariable("VR_UI_CAPTURE_DIR");
        if(!output.isEmpty()) {
            QDir().mkpath(output); QTest::qWait(200);
            auto grab=window->contentItem()->grabToImage(); QVERIFY(grab);
            QSignalSpy ready(grab.data(),&QQuickItemGrabResult::ready); QVERIFY(ready.wait(5000));
            QVERIFY(grab->saveToFile(output+"/compression-saved.png"));
        }
        controller.tool("files"); controller.openFile(0);
        QTRY_VERIFY_WITH_TIMEOUT(controller.state()["replayReady"].toBool(),5000);
        controller.seek(20); QTRY_COMPARE_WITH_TIMEOUT(controller.engine().snapshot().replayIndex,uint64_t(2400),5000);
        QTRY_VERIFY_WITH_TIMEOUT(controller.state()["replayReady"].toBool(),5000);
        controller.seek(1); QTRY_COMPARE_WITH_TIMEOUT(controller.engine().snapshot().replayIndex,uint64_t(120),5000);
        QTRY_VERIFY_WITH_TIMEOUT(controller.state()["replayReady"].toBool(),5000);
    }

    void hardwareRegionClip() {
        if(!qEnvironmentVariableIsSet("VR_TEST_CAMERA")) QSKIP("Requires exclusive camera access");
        QTemporaryDir dir;
        EngineOptions options; options.storage=dir.path().toStdString();
        options.shader=qEnvironmentVariable("VR_TEST_SHADER_PATH",VR_TEST_SHADER).toStdString();
        Controller controller(options);
        QTRY_VERIFY_WITH_TIMEOUT(controller.state()["ready"].toBool(),10000);
        controller.record(); QTRY_VERIFY_WITH_TIMEOUT(controller.engine().recorder.status().frames>=12,5000);
        controller.stopRecording(); QTRY_COMPARE_WITH_TIMEOUT(controller.state()["recordState"].toString(),QString("saved"),10000);
        controller.tool("files"); controller.openFile(0);
        QTRY_VERIFY_WITH_TIMEOUT(controller.state()["replayReady"].toBool(),5000);
        const double fps=controller.state()["fps"].toDouble();
        controller.seek(2/fps); QTRY_COMPARE(controller.engine().snapshot().replayIndex,uint64_t(2));
        QTRY_VERIFY_WITH_TIMEOUT(controller.state()["replayReady"].toBool(),5000);
        const auto identity=controller.engine().snapshot().referenceSequence;
        controller.analysisRange(6/fps); controller.analyzeGlobal();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.state()["offlineBusy"].toBool(),60000);
        const auto output=controller.state()["offlineOutput"].toString();
        QVERIFY2(!output.isEmpty(),qPrintable(controller.state()["error"].toString()));
        QFile file(output); QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.size(),qint64(6)*(40+1280*800*4));
        const auto h=file.read(40); QCOMPARE(vr::getLE(reinterpret_cast<const uint8_t *>(h.constData())+16,8),identity);
        qInfo()<<"BOARD_GLOBAL_CLIP"<<"start_index"<<2<<"frames"<<6<<"dimensions"<<controller.state()["roi"]<<"fps"<<fps;
    }
    void recordingDeletion() {
        QTemporaryDir dir, outside;
        EngineOptions options; options.demo=true; options.width=320; options.height=200;
        options.bits=8; options.demoFps=30; options.storage=dir.path().toStdString();
        options.shader=qEnvironmentVariable("VR_TEST_SHADER_PATH",VR_TEST_SHADER).toStdString();
        Controller controller(options);
        QQmlApplicationEngine engine; engine.rootContext()->setContextProperty("backend",&controller);
        engine.load(QUrl("qrc:/qml/Main.qml")); QVERIFY(!engine.rootObjects().isEmpty());
        auto window=qobject_cast<QQuickWindow *>(engine.rootObjects().first()); QVERIFY(window);
        window->showFullScreen();
        QTRY_VERIFY_WITH_TIMEOUT(controller.state()["ready"].toBool(),5000);
        controller.record();
        QTRY_VERIFY_WITH_TIMEOUT(controller.engine().recorder.status().frames>=3,5000);
        controller.stopRecording();
        QTRY_COMPARE_WITH_TIMEOUT(controller.state()["recordState"].toString(),QString("saved"),5000);
        controller.tool("files"); QCOMPARE(controller.files().size(),1);
        const auto path=controller.files()[0].toMap()["path"].toString();
        QVERIFY(QFileInfo::exists(path+".json"));
        const auto other=outside.path()+"/outside.vri";
        QVERIFY(QFile::copy(path,other));
        QVERIFY(QFile::link(other,dir.path()+"/link.vri"));
        controller.refreshFiles(); QCOMPARE(controller.files().size(),1);
        controller.deleteFile(other); QVERIFY(QFileInfo::exists(other));
        controller.deleteFile(dir.path()+"/link.vri"); QVERIFY(QFileInfo::exists(other));
        controller.clearError();
        std::function<QQuickItem *(QQuickItem *,const QString &)> find = [&](QQuickItem *parent,const QString &name) -> QQuickItem * {
            if(parent->objectName()==name) return parent;
            for(auto child:parent->childItems()) if(auto found=find(child,name)) return found;
            return nullptr;
        };
        auto click=[&](const QString &name) {
            auto item=find(window->contentItem(),name); QVERIFY2(item,qPrintable(name));
            QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,item->mapToScene(QPointF(item->width()/2,item->height()/2)).toPoint());
        };
        auto capture=[&](const QString &name) {
            const auto output=qEnvironmentVariable("VR_UI_CAPTURE_DIR"); if(output.isEmpty()) return;
            QDir().mkpath(output); QTest::qWait(150);
            auto grab=window->contentItem()->grabToImage(); QVERIFY(grab);
            QSignalSpy ready(grab.data(),&QQuickItemGrabResult::ready);
            QVERIFY(ready.wait(5000)||!grab->image().isNull()); QVERIFY(grab->image().save(output+"/"+name+".png"));
        };
        QTest::qWait(150); capture("files-delete-menu");
        click("deleteFile_0"); QTest::qWait(250); capture("files-delete-confirm");
        click("cancelDelete"); QTest::qWait(250); QVERIFY(QFileInfo::exists(path));
        click("deleteFile_0"); QTest::qWait(250);
        // Refresh/reorder while confirming must not delete whichever file moves into row zero.
        const auto copy=dir.path()+"/copy.vri"; QVERIFY(QFile::copy(path,copy));
        controller.refreshFiles(); QCOMPARE(controller.files().size(),2);
        click("confirmDelete"); QTRY_VERIFY(!QFileInfo::exists(path));
        QVERIFY(!QFileInfo::exists(path+".json")); QVERIFY(QFileInfo::exists(copy));
        QCOMPARE(controller.files().size(),1);
        controller.openFile(0); QTRY_COMPARE(controller.state()["tool"].toString(),QString("replay"));
        controller.tool("files"); controller.deleteFile(copy);
        QVERIFY(!QFileInfo::exists(copy)); QCOMPARE(controller.state()["fileName"].toString(),QString());
        QCOMPARE(controller.files().size(),0); capture("files-delete-empty");
        QVERIFY(QFile::copy(other,copy)); controller.refreshFiles();
        controller.record(); QTRY_VERIFY_WITH_TIMEOUT(controller.engine().recorder.status().frames>=3,5000);
        controller.deleteFile(copy); QVERIFY(QFileInfo::exists(copy));
        controller.stopRecording();
        QTRY_COMPARE_WITH_TIMEOUT(controller.state()["recordState"].toString(),QString("saved"),5000);
    }
    void exposureInteraction() {
        SensorSettings limits;
        QCOMPARE(boundedExposure(limits,1e20),limits.maximum);
        QCOMPARE(boundedExposure(limits,-100),limits.minimum);
        QVERIFY_EXCEPTION_THROWN(boundedExposure(limits,NAN),std::runtime_error);
        Frame uniform; uniform.width=65; uniform.height=65; uniform.bits=10; uniform.stride=136;
        uniform.data.resize(uniform.stride*uniform.height);
        for(unsigned y=0;y<65;++y) for(unsigned x=0;x<65;++x) {
            uniform.data[y*uniform.stride+x*2]=0; uniform.data[y*uniform.stride+x*2+1]=2;
        }
        QVERIFY(std::abs(meteredBrightness(uniform,0,0)-512./1023)<1e-6);
        QTemporaryDir dir;
        EngineOptions options; options.demo=true; options.width=320; options.height=200;
        options.bits=8; options.demoFps=30; options.storage=dir.path().toStdString();
        options.shader=qEnvironmentVariable("VR_TEST_SHADER_PATH",VR_TEST_SHADER).toStdString();
        Controller controller(options);
        QQmlApplicationEngine engine; engine.rootContext()->setContextProperty("backend",&controller);
        engine.load(QUrl("qrc:/qml/Main.qml")); QVERIFY(!engine.rootObjects().isEmpty());
        auto window=qobject_cast<QQuickWindow *>(engine.rootObjects().first()); QVERIFY(window);
        window->showFullScreen();
        QTRY_VERIFY_WITH_TIMEOUT(controller.state()["exposureEditable"].toBool(),5000);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.image(0).isNull(),5000);
        auto camera=window->findChild<FrameView *>("cameraView"); QVERIFY(camera);
        auto indicator=window->findChild<QQuickItem *>("exposureIndicator"); QVERIFY(indicator);
        const double original=controller.state()["exposureMs"].toDouble();
        QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,camera->toView(70,100).toPoint());
        QTRY_VERIFY(indicator->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.state()["cameraBusy"].toBool(),5000);
        QVERIFY(controller.state()["exposureMs"].toDouble()<original);
        const double metered=controller.state()["exposureMs"].toDouble();
        QTest::mousePress(window,Qt::LeftButton,Qt::NoModifier,QPoint(500,350));
        QTest::mouseMove(window,QPoint(500,270),80);
        QTest::mouseRelease(window,Qt::LeftButton,Qt::NoModifier,QPoint(500,270));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.state()["cameraBusy"].toBool(),5000);
        QVERIFY(controller.state()["exposureEV"].toDouble()>.5);
        QVERIFY(controller.state()["exposureMs"].toDouble()>metered);
        auto capture=[&](const QString &name) {
            const auto dir=qEnvironmentVariable("VR_UI_CAPTURE_DIR"); if(dir.isEmpty()) return;
            QDir().mkpath(dir); QTest::qWait(100);
            auto grab=window->contentItem()->grabToImage(); QVERIFY(grab);
            QSignalSpy ready(grab.data(),&QQuickItemGrabResult::ready);
            QVERIFY(ready.wait(5000)||!grab->image().isNull()); QVERIFY(grab->image().save(dir+"/"+name+".png"));
        };
        capture("exposure-yellow");
        QTRY_VERIFY_WITH_TIMEOUT(!indicator->isVisible(),6500);
        controller.tool("settings");
        QTest::qWait(100);
        auto inspector=window->findChild<QQuickItem *>("inspector"); QVERIFY(inspector);
        QVERIFY2(inspector->height()<=584,qPrintable(QString::number(inspector->height())));
        for(double fps : {120.,60.,30.}) {
            controller.captureFps(fps);
            QTRY_VERIFY_WITH_TIMEOUT(!controller.state()["cameraBusy"].toBool(),5000);
            QCOMPARE(controller.state()["fps"].toDouble(),fps);
            controller.exposureCompensation(3);
            QTRY_VERIFY_WITH_TIMEOUT(!controller.state()["cameraBusy"].toBool(),5000);
            QVERIFY(controller.state()["exposureMs"].toDouble()<=1000/fps);
            QVERIFY(controller.state()["exposureMs"].toDouble()<=controller.state()["exposureMaxMs"].toDouble());
        }
        capture("exposure-settings");
        controller.record();
        QTRY_COMPARE(controller.state()["recordState"].toString(),QString("recording"));
        const double locked=controller.state()["exposureMs"].toDouble();
        controller.captureFps(120); controller.meterExposure(70,100);
        QVERIFY(!controller.state()["exposureEditable"].toBool());
        QCOMPARE(controller.state()["exposureMs"].toDouble(),locked);
        QCOMPARE(controller.state()["fps"].toDouble(),30.0);
        QTRY_VERIFY_WITH_TIMEOUT(controller.engine().recorder.status().frames>=3,3000);
        controller.stopRecording();
        QTRY_COMPARE_WITH_TIMEOUT(controller.state()["recordState"].toString(),QString("saved"),5000);
        controller.tool("select");
        controller.meterExposure(70,100);
        QVERIFY(!controller.state()["exposureEditable"].toBool());
        QCOMPARE(controller.state()["exposureMs"].toDouble(),locked);
    }
    void hardwareExposure() {
        if(!qEnvironmentVariableIsSet("VR_TEST_CAMERA")) QSKIP("Requires exclusive camera access");
        EngineOptions options; options.storage=qEnvironmentVariable("VR_TEST_STORAGE").toStdString();
        options.shader=qEnvironmentVariable("VR_TEST_SHADER_PATH",VR_TEST_SHADER).toStdString();
        Controller controller(options);
        QTRY_VERIFY_WITH_TIMEOUT(controller.state()["exposureEditable"].toBool(),10000);
        SensorControls sensor(options.sensor);
        for(double fps : {30.,60.,120.}) {
            controller.captureFps(fps);
            QTRY_VERIFY_WITH_TIMEOUT(!controller.state()["cameraBusy"].toBool(),10000);
            QVERIFY2(controller.state()["error"].toString().isEmpty(),qPrintable(controller.state()["error"].toString()));
            QVERIFY(std::abs(controller.state()["fps"].toDouble()-fps)<.01);
            controller.exposureCompensation(3);
            QTRY_VERIFY_WITH_TIMEOUT(!controller.state()["cameraBusy"].toBool(),5000);
            auto actual=sensor.read(800);
            QVERIFY(actual.exposure<=actual.maximum);
            QVERIFY(actual.exposureMs()<=1000/actual.fps());
            QTest::qWait(150);
            auto begin=controller.engine().snapshot(); QTest::qWait(1000); auto end=controller.engine().snapshot();
            const double measured=double(end.sequence-begin.sequence)*1e9/(end.timestamp-begin.timestamp);
            QVERIFY2(std::abs(measured-fps)<fps*.03,qPrintable(QString::number(measured)));
            qInfo()<<"BOARD_EXPOSURE_FPS"<<fps<<"actual"<<measured<<"exposure_ms"<<actual.exposureMs()<<"limit_ms"<<1000/actual.fps();
        }
        controller.meterExposure(640,400);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.state()["cameraBusy"].toBool(),5000);
        QVERIFY2(controller.state()["error"].toString().isEmpty(),qPrintable(controller.state()["error"].toString()));
        qInfo()<<"BOARD_AUTO_EXPOSURE"<<controller.state()["exposureMs"]<<controller.state()["cameraStatus"];
    }
    void motionAdmission() {
        using namespace vr;
        constexpr uint64_t ample=8ULL*1024*1024*1024;
        MotionConfig c; c.width=64; c.height=64; c.fps=60; c.low=8; c.high=25;
        auto small=estimateMotionLoad(c,1280,800,2560);
        QCOMPARE(motionLimit(small,true,ample),MotionLimit::None);
        QCOMPARE(motionLimit(small,true,motionReserve+small.memoryBytes-1),MotionLimit::Memory);
        QCOMPARE(motionLimit(small,true,motionReserve+small.memoryBytes),MotionLimit::None);
        c.width=256; c.height=256; c.fps=120;
        auto large=estimateMotionLoad(c,1280,800,2560);
        QCOMPARE(motionLimit(large,true,ample),MotionLimit::Compute);
        c.fps=30; c.low=3; c.high=12;
        QCOMPARE(motionLimit(estimateMotionLoad(c,1280,800,2560),true,ample),MotionLimit::None);
        c.width=1280; c.height=800; c.fps=120;
        QCOMPARE(motionLimit(estimateMotionLoad(c,1280,800,2560),true,ample),MotionLimit::Compute);
        c.width=31; c.height=23; c.fps=30; c.realtime=false;
        auto offline=estimateMotionLoad(c,320,200,320,10);
        // 31x23 + 15x11 + 7x5 = 913 phase samples, nine padding frames per end.
        QCOMPARE(offline.scratchBytes,uint64_t(10*(64000+16+3*913*4)+18*913*4));
        QCOMPARE(offline.outputBytes,uint64_t(10*(40+31*23*4)));
        QCOMPARE(motionLimit(offline,false,ample,ample),MotionLimit::None);
        QCOMPARE(motionLimit(offline,false,ample,motionReserve+offline.scratchBytes+offline.outputBytes-1),MotionLimit::Disk);
        QVERIFY_EXCEPTION_THROWN(estimateMotionLoad(c,320,200,320,UINT64_MAX),MotionError);
        auto huge=estimateMotionLoad(c,320,200,320,10000000);
        QCOMPARE(motionLimit(huge,false,ample,UINT64_MAX),MotionLimit::Scratch);

        QTemporaryDir dir;
        EngineOptions options;
        options.demo=true; options.width=320; options.height=200; options.bits=8; options.demoFps=240;
        options.storage=dir.path().toStdString();
        options.shader=qEnvironmentVariable("VR_TEST_SHADER_PATH", VR_TEST_SHADER).toStdString();
        Controller controller(options);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.image(0).isNull(),5000);
        // Enter through the real button path: select first, then auto-start.
        controller.tool("magnify");
        controller.selectObject(100,85,130,30,false);
        QTRY_VERIFY_WITH_TIMEOUT(controller.state()["objectReady"].toBool(),10000);
        controller.confirmObject(); controller.selectEdge(160,96);
        QTRY_VERIFY_WITH_TIMEOUT(controller.state()["edgeReady"].toBool(),10000);
        controller.parameters(20,8,25,240,65);
        controller.confirmEdge();
        QCOMPARE(controller.state()["tool"].toString(),QString("select"));
        QCOMPARE(controller.engine().snapshot().tool,QString("select"));
        QVERIFY(controller.state()["error"].toString().contains("RK3588"));
        QVERIFY(controller.image(1).isNull());
        QCOMPARE(controller.state()["samples"].toInt(),0);
        QVERIFY(controller.state()["edgeReady"].toBool());
        qInfo() << "MOTION_REJECTED_BEFORE_START" << controller.state()["error"];
        controller.parameters(20,8,25,60,65);
        controller.confirmEdge();
        QCOMPARE(controller.state()["tool"].toString(),QString("magnify"));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.image(1).isNull(),10000);
        const auto acceptedEstimate=controller.state()["motionEstimate"].toString();
        // Increasing load while running must not restart with rejected parameters.
        controller.parameters(20,8,25,240,65);
        QCOMPARE(controller.state()["processingFps"].toDouble(),60.0);
        QCOMPARE(controller.engine().snapshot().processingFps,60.0);
        QCOMPARE(controller.state()["motionEstimate"].toString(),acceptedEstimate);
        QVERIFY(controller.state()["error"].toString().contains("RK3588"));
        controller.tool("preview");
    }
    void draggablePanelsAndPan() {
        QTemporaryDir dir;
        EngineOptions options;
        options.demo = true; options.width = 1280; options.height = 800; options.bits = 8;
        options.demoFps = 30; options.storage = dir.path().toStdString();
        options.shader = qEnvironmentVariable("VR_TEST_SHADER_PATH", VR_TEST_SHADER).toStdString();
        options.stream.transport = "tcp"; options.stream.port = 29879;
        Controller controller(options);
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("backend", &controller);
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->showFullScreen();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.image(0).isNull(), 10000);
        auto camera = window->findChild<FrameView *>("cameraView");
        auto inspector = window->findChild<QQuickItem *>("inspector");
        QVERIFY(camera); QVERIFY(inspector);
        std::function<QQuickItem *(QQuickItem *, const QString &)> find =
            [&](QQuickItem *parent, const QString &name) -> QQuickItem * {
                if (parent->objectName() == name) return parent;
                for (auto child : parent->childItems())
                    if (auto found = find(child,name)) return found;
                return nullptr;
            };
        auto touch = QTest::createTouchDevice();
        auto drag = [&](QPoint from, QPoint to) {
            QTest::touchEvent(window,touch).press(0,from,window);
            QTest::qWait(30);
            for (int i=1; i<=5; ++i) {
                QTest::touchEvent(window,touch).move(0,from+(to-from)*i/5,window);
                QTest::qWait(30);
            }
            QTest::touchEvent(window,touch).release(0,to,window);
            QTest::qWait(100);
        };
        auto movePanel = [&] {
            QTest::qWait(100);
            // Every new tool starts between the top and bottom menus.
            QVERIFY(inspector->y()>=inspector->property("safeTop").toDouble());
            QVERIFY(inspector->y()+inspector->height()<=inspector->property("safeBottom").toDouble()+.01);
            auto handle = find(inspector,"panelDragHandle");
            QVERIFY(handle);
            const QPointF before(inspector->x(),inspector->y());
            const auto sourceBefore = camera->toSource(512,300);
            const QPoint from = handle->mapToScene(QPointF(100,40)).toPoint();
            drag(from,from+QPoint(-350,40));
            QVERIFY(inspector->x() < before.x()-200);
            QCOMPARE(camera->toSource(512,300), sourceBefore);
            const auto retained = QPointF(inspector->x(),inspector->y());
            QTest::qWait(200);
            QCOMPARE(QPointF(inspector->x(),inspector->y()),retained);
            QVERIFY(inspector->x()>=8 && inspector->y()>=8);
            QVERIFY(inspector->x()+inspector->width() <= window->width()-8);
            QVERIFY(inspector->y()+inspector->height() <= window->height()-8);
        };
        controller.view(true);
        QTRY_VERIFY(camera->nativeSize());
        const auto initial = camera->toSource(512,300);
        drag(QPoint(500,300),QPoint(580,340));
        QVERIFY(camera->toSource(512,300).x() < initial.x()-50);
        const auto panned = camera->toSource(512,300);
        QTest::qWait(250); // Multiple state refreshes must not reset the offset.
        QCOMPARE(camera->toSource(512,300),panned);
        camera->setNativeSize(true); // Same-value setter must also preserve it.
        QCOMPARE(camera->toSource(512,300),panned);
        controller.view(false);
        QTRY_VERIFY(!camera->nativeSize());
        const auto fitted = camera->toSource(512,300);
        drag(QPoint(500,300),QPoint(580,340));
        QCOMPARE(camera->toSource(512,300),fitted);
        controller.tool("focus");
        movePanel();
        const auto focusPosition = QPointF(inspector->x(),inspector->y());
        // A slider gesture must not move its panel.
        auto slider = find(inspector,"focusSlider");
        QVERIFY(slider);
        const auto start = slider->mapToScene(QPointF(30,slider->height()/2)).toPoint();
        drag(start,start+QPoint(120,0));
        QCOMPARE(QPointF(inspector->x(),inspector->y()),focusPosition);
        controller.tool("preview"); controller.tool("focus");
        QTest::qWait(100);
        QCOMPARE(QPointF(inspector->x(),inspector->y()),focusPosition);
        controller.tool("preview"); controller.view(true); controller.tool("select");
        movePanel();
        controller.selectObject(400,360,480,80,false);
        QTRY_VERIFY_WITH_TIMEOUT(controller.state()["objectReady"].toBool(),10000);
        const auto roi = controller.state()["roi"].toRect();
        auto panButton = find(window->contentItem(),"selectionPan");
        QVERIFY(panButton);
        QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,
                          panButton->mapToScene(QPointF(panButton->width()/2,panButton->height()/2)).toPoint());
        QVERIFY(window->property("selectionPan").toBool());
        const auto selectionBefore = camera->toSource(512,300);
        drag(QPoint(850,300),QPoint(910,340));
        QVERIFY(camera->toSource(512,300).x() < selectionBefore.x()-30);
        QCOMPARE(controller.state()["roi"].toRect(),roi);
        QVERIFY(controller.state()["objectReady"].toBool());
        controller.confirmObject(); controller.selectEdge(640,383);
        QTRY_VERIFY_WITH_TIMEOUT(controller.state()["edgeReady"].toBool(),10000);
        controller.confirmEdge(); controller.tool("magnify");
        movePanel();
        controller.tool("record"); movePanel();
        controller.tool("preview"); controller.mode("direct"); movePanel();
        const auto directBefore = camera->toSource(512,300);
        drag(QPoint(850,300),QPoint(790,260));
        QVERIFY(camera->toSource(512,300).x() > directBefore.x()+30);
        QImage directImage(1024,600,QImage::Format_RGB32);
        directImage.fill(Qt::black);
        { QPainter painter(&directImage); camera->paint(&painter); }
        for (int y=0; y<directImage.height(); ++y)
            for (int x=0; x<directImage.width(); ++x)
                QVERIFY(directImage.pixel(x,y) != qRgb(165,29,24)); // no old ROI label
        const auto captures=qEnvironmentVariable("VR_UI_CAPTURE_DIR");
        if (!captures.isEmpty()) {
            QDir().mkpath(captures);
            auto grab=window->contentItem()->grabToImage();
            QVERIFY(grab);
            QSignalSpy ready(grab.data(), &QQuickItemGrabResult::ready);
            QVERIFY(ready.wait(5000) || !grab->image().isNull());
            QVERIFY(grab->image().save(captures+"/draggable-direct.png"));
        }
        controller.mode("local");
        QTRY_VERIFY_WITH_TIMEOUT(!controller.state()["streamBusy"].toBool(),5000);
    }
    void hardwareNpu() {
        if (!qEnvironmentVariableIsSet("VR_TEST_NPU") || !qEnvironmentVariableIsSet("VR_TEST_CAMERA"))
            QSKIP("Requires exclusive camera access and deployed RKNN models");
        EngineOptions options;
        options.storage = qEnvironmentVariable("VR_TEST_STORAGE").toStdString();
        options.encoder = qEnvironmentVariable("VR_TEST_ENCODER").toStdString();
        options.decoder = qEnvironmentVariable("VR_TEST_DECODER").toStdString();
        options.rknnRuntime = qEnvironmentVariable("VR_TEST_RKNN_RUNTIME").toStdString();
        options.shader = qEnvironmentVariable("VR_TEST_SHADER_PATH", VR_TEST_SHADER).toStdString();
        Controller controller(options);
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("backend", &controller);
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto window=qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->showFullScreen();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.image(0).isNull(), 10000);
        controller.tool("select");
        controller.selectObject(320,200,640,400,false);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.state()["selectionBusy"].toBool(), 15000);
        QVERIFY2(controller.state()["objectReady"].toBool(), qPrintable(controller.state()["error"].toString()));
        QCOMPARE(controller.state()["selectionMethod"].toString(), QString("NPU 物品识别"));
        qInfo() << "BOARD_CAMERA_NPU" << controller.state()["selectionMs"] << "ms"
                << controller.state()["roi"] << controller.engine().snapshot().referenceSequence;
        const auto captures=qEnvironmentVariable("VR_UI_CAPTURE_DIR");
        if (!captures.isEmpty()) {
            QDir().mkpath(captures);
            auto grab=window->contentItem()->grabToImage();
            QVERIFY(grab);
            QSignalSpy ready(grab.data(), &QQuickItemGrabResult::ready);
            QVERIFY(ready.wait(5000) || !grab->image().isNull());
            QVERIFY(grab->image().save(captures+"/npu-camera.png"));
        }
    }
    void npuFailureRecovery() {
#ifndef VR_TOUCH_RKNN
        QSKIP("RKNN backend disabled");
#else
        QTemporaryDir dir;
        EngineOptions options;
        options.demo = true; options.width = 320; options.height = 200; options.bits = 8;
        options.demoFps = 30; options.storage = dir.path().toStdString();
        options.shader = qEnvironmentVariable("VR_TEST_SHADER_PATH", VR_TEST_SHADER).toStdString();
        options.encoder = "/missing/encoder.rknn"; options.decoder = "/missing/decoder.rknn";
        options.rknnRuntime = "/missing/runtime.so";
        Controller controller(options);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.image(0).isNull(), 5000);
        controller.tool("select");
        controller.selectObject(160, 100, 0, 0, true);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.state()["selectionBusy"].toBool(), 5000);
        QVERIFY(controller.state()["error"].toString().contains("NPU"));
        QVERIFY(!controller.state()["objectReady"].toBool());
        QVERIFY(controller.state()["useNpu"].toBool()); // no silent fallback
        controller.selectionNpu(false);
        controller.selectObject(100, 85, 130, 30, false);
        QTRY_VERIFY_WITH_TIMEOUT(controller.state()["objectReady"].toBool(), 10000);
        QCOMPARE(controller.state()["selectionMethod"].toString(), QString("矩形区域"));
        QVERIFY(controller.state()["error"].toString().isEmpty());
        controller.selectionNpu(true);
        controller.selectObject(160, 100, 0, 0, true);
        controller.tool("preview");
        QTest::qWait(200);
        QVERIFY(!controller.state()["selectionBusy"].toBool());
        QVERIFY(!controller.state()["objectReady"].toBool());
        QVERIFY(controller.overlay().isNull());
#endif
    }
    void npuWorkflow() {
        if (!qEnvironmentVariableIsSet("VR_TEST_NPU"))
            QSKIP("Requires RK3588 NPU and deployed model pair");
        QTemporaryDir dir;
        QVERIFY(QFile::copy(qEnvironmentVariable("VR_TEST_NPU_FIXTURE"), dir.path()+"/object.vri"));
        EngineOptions options;
        options.demo = true; options.width = 320; options.height = 200; options.bits = 8;
        options.demoFps = 30; options.storage = dir.path().toStdString();
        options.encoder = qEnvironmentVariable("VR_TEST_ENCODER").toStdString();
        options.decoder = qEnvironmentVariable("VR_TEST_DECODER").toStdString();
        options.rknnRuntime = qEnvironmentVariable("VR_TEST_RKNN_RUNTIME").toStdString();
        options.shader = qEnvironmentVariable("VR_TEST_SHADER_PATH", VR_TEST_SHADER).toStdString();
        Controller controller(options);
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("backend", &controller);
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->showFullScreen();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.image(0).isNull(), 5000);
        QVERIFY(controller.state()["hasSam"].toBool());
        controller.refreshFiles();
        controller.openFile(0);
        QTRY_VERIFY_WITH_TIMEOUT(controller.state()["replayReady"].toBool(), 5000);
        controller.tool("select");
        auto camera = window->findChild<FrameView *>("cameraView");
        QVERIFY(camera);
        const auto tap = camera->mapToScene(camera->toView(370, 210)).toPoint();
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, tap);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.state()["selectionBusy"].toBool(), 15000);
        QVERIFY2(controller.state()["objectReady"].toBool(), qPrintable(controller.state()["error"].toString()));
        QCOMPARE(controller.state()["selectionMethod"].toString(), QString("NPU 物品识别"));
        auto overlay = controller.overlay();
        int filled = 0;
        QPoint edge(-1,-1);
        for (int y=1; y<overlay.height()-1; ++y)
            for (int x=1; x<overlay.width()-1; ++x) {
                const auto pixel = overlay.pixel(x,y);
                if (qAlpha(pixel)==45) ++filled;
                if (pixel==qRgb(255,69,58) && edge.x()<0) edge=QPoint(x,y);
            }
        QVERIFY(filled>100);
        QVERIFY(edge.x()>=0);
        qInfo() << "BOARD_NPU_POINT" << controller.state()["selectionMs"] << "ms" << filled << "mask pixels";
        const auto captures=qEnvironmentVariable("VR_UI_CAPTURE_DIR");
        if (!captures.isEmpty()) {
            QDir().mkpath(captures);
            auto grab=window->contentItem()->grabToImage();
            QVERIFY(grab);
            QSignalSpy ready(grab.data(), &QQuickItemGrabResult::ready);
            QVERIFY(ready.wait(5000) || !grab->image().isNull());
            QVERIFY(grab->image().save(captures+"/npu-object.png"));
        }
        controller.selectObject(190,70,270,210,false);
        QCOMPARE(controller.state()["roi"].toRect(),QRect(190,70,270,210));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.state()["selectionBusy"].toBool(), 10000);
        QVERIFY2(controller.state()["objectReady"].toBool(), qPrintable(controller.state()["error"].toString()));
        qInfo() << "BOARD_NPU_CACHED_BOX" << controller.state()["selectionMs"] << "ms";
        auto region=window->findChild<QQuickItem *>("editableRegion"); QVERIFY(region);
        for(bool resize : {false,true}) {
            const auto old=controller.state()["roi"].toRect();
            const QPointF source=resize ? QPointF(old.x()+old.width(),old.y()+old.height()) : QPointF(old.center());
            const auto from=camera->mapToScene(camera->toView(source.x(),source.y())).toPoint();
            const auto to=from+QPoint(8,5);
            QTest::mousePress(window,Qt::LeftButton,Qt::NoModifier,from);
            QTest::mouseMove(window,to,60);
            QVERIFY(region->property("adjusting").toBool());
            const auto requested=region->property("draft").toRectF().toAlignedRect();
            QTest::mouseRelease(window,Qt::LeftButton,Qt::NoModifier,to);
            QCOMPARE(controller.state()["roi"].toRect(),requested);
            int busySamples=0; QElapsedTimer deadline; deadline.start();
            while(controller.state()["selectionBusy"].toBool() && deadline.elapsed()<15000) {
                QCOMPARE(region->property("region").toRectF().toAlignedRect(),requested);
                ++busySamples; QTest::qWait(5);
            }
            QVERIFY(busySamples>0); QVERIFY(controller.state()["objectReady"].toBool());
            QCOMPARE(controller.state()["roi"].toRect(),requested);
            qInfo()<<"NPU_ROI_STABLE"<<(resize?"resize":"move")<<requested<<"busy_samples"<<busySamples;
        }
        const QRect objectRoi = controller.state()["roi"].toRect();
        controller.confirmObject();
        controller.selectEdge(370,210);
        QTRY_VERIFY_WITH_TIMEOUT(controller.state()["edgeReady"].toBool(), 5000);
        const QRect edgeRoi = controller.state()["roi"].toRect();
        QVERIFY(edgeRoi.contains(QPoint(370,210)));
        QVERIFY(edgeRoi.width()*edgeRoi.height() < objectRoi.width()*objectRoi.height());
        QVERIFY(QRect(QPoint(0,0), controller.image(0).size()).contains(edgeRoi));
        QVERIFY(!controller.state()["selectionBusy"].toBool());
        qInfo() << "EDGE_MAGNIFICATION_ROI" << edgeRoi;
        if (!captures.isEmpty()) {
            QTest::qWait(100);
            auto grab=window->contentItem()->grabToImage();
            QVERIFY(grab);
            QSignalSpy ready(grab.data(), &QQuickItemGrabResult::ready);
            QVERIFY(ready.wait(5000) || !grab->image().isNull());
            QVERIFY(grab->image().save(captures+"/npu-edge-region.png"));
        }
        // A failed re-selection must not keep a stale ready region.
        controller.selectEdge(-1000,-1000);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.state()["selectionBusy"].toBool(), 5000);
        QVERIFY(!controller.state()["edgeReady"].toBool());
        QVERIFY(!controller.state()["error"].toString().isEmpty());
        controller.selectEdge(370,210);
        QTRY_VERIFY_WITH_TIMEOUT(controller.state()["edgeReady"].toBool(), 5000);
        QCOMPARE(controller.state()["roi"].toRect(), edgeRoi);
        QVERIFY(controller.state()["error"].toString().isEmpty());
        controller.confirmEdge();
        QCOMPARE(controller.state()["tool"].toString(), QString("replay"));
        controller.tool("select");
        controller.selectionNpu(false);
        QVERIFY(!controller.state()["useNpu"].toBool());
        QVERIFY(!controller.state()["objectReady"].toBool());
    }
    void focusPeakingParity() {
        vr::MotionConfig config;
        config.width = 128; config.height = 96;
        config.analysisOnly = true; config.realtime = false;
        vr::VulkanMotion reference(config, qEnvironmentVariable("VR_TEST_SHADER_PATH", VR_TEST_SHADER).toStdString());
        FocusPeaking focus;
        for (unsigned bits : {8u, 10u, 12u, 16u}) {
            Frame f;
            f.width = config.width; f.height = config.height; f.bits = bits;
            f.stride = f.width*(bits == 8 ? 1 : 2)+16;
            f.data.assign(size_t(f.stride)*f.height, 0xcc);
            unsigned seed = 431;
            for (unsigned y=0; y<f.height; ++y)
                for (unsigned x=0; x<f.width; ++x) {
                    seed = seed*1664525u+1013904223u;
                    const unsigned value = seed >> (32-bits);
                    auto p = f.data.data()+size_t(y)*f.stride+x*(bits == 8 ? 1 : 2);
                    p[0]=value;
                    if (bits != 8) p[1]=value>>8;
                }
            vr::EdgeConfig edge;
            edge.threshold = .04f;
            const auto expected = reference.analyze(f, edge);
            const auto actual = focus.render(f, edge.threshold);
            int differences = 0, peaks = 0;
            for (unsigned y=0; y<f.height; ++y)
                for (unsigned x=0; x<f.width; ++x) {
                    const bool lit = qAlpha(actual.pixel(x,y)) != 0;
                    peaks += lit;
                    differences += lit != bool(expected.peaks[size_t(y)*f.width+x]);
                }
            qInfo() << "FOCUS_PARITY" << bits << "bits mismatches" << differences << "of" << f.width*f.height;
            QVERIFY(peaks > 100);
            // Only floating-point tie cases may differ between CPU and GPU.
            QVERIFY(differences < int(f.width*f.height/1000));
            std::fill(f.data.begin(), f.data.end(), 0);
            const auto flat = focus.render(f, edge.threshold);
            for (unsigned y=0; y<f.height; ++y)
                for (unsigned x=0; x<f.width; ++x)
                    QCOMPARE(qAlpha(flat.pixel(x,y)), 0);
        }
    }
    void hardwarePreview() {
        if (!qEnvironmentVariableIsSet("VR_TEST_CAMERA"))
            QSKIP("Requires exclusive physical camera access");
        EngineOptions options;
        options.storage = qEnvironmentVariable("VR_TEST_STORAGE").toStdString();
        options.shader = qEnvironmentVariable("VR_TEST_SHADER_PATH", VR_TEST_SHADER).toStdString();
        options.stream.manageGadget = true;
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
        for (const auto &tool : {QString("preview"), QString("focus")}) {
            controller.tool(tool);
            if (tool == "focus")
                QTRY_VERIFY_WITH_TIMEOUT(!controller.overlay().isNull(), 15000);
            QTest::qWait(1000);
            QSignalSpy frames(&controller, &Controller::frameChanged);
            QSignalSpy swaps(window, &QQuickWindow::frameSwapped);
            auto start = controller.engine().snapshot();
            QElapsedTimer timer;
            timer.start();
            QTest::qWait(3000);
            const double seconds = timer.elapsed() / 1000.;
            auto end = controller.engine().snapshot();
            qInfo() << "BOARD_PREVIEW_PERFORMANCE" << tool
                    << "computed_fps" << (end.displayRevision-start.displayRevision)/seconds
                    << "delivered_fps" << frames.count()/seconds
                    << "swapped_fps" << swaps.count()/seconds;
            QVERIFY2(frames.count()/seconds >= 30., "Live preview fell below 30 unique frames/s");
            const auto captures = qEnvironmentVariable("VR_UI_CAPTURE_DIR");
            if (!captures.isEmpty()) {
                QDir().mkpath(captures);
                auto grab = window->contentItem()->grabToImage();
                QVERIFY(grab);
                QSignalSpy ready(grab.data(), &QQuickItemGrabResult::ready);
                QVERIFY(ready.wait(5000) || !grab->image().isNull());
                QVERIFY(grab->image().save(captures + "/camera-" + tool + ".png"));
            }
        }
        controller.tool("preview");
        // No receiver: leaving direct mode must not become a sticky error.
        controller.mode("direct");
        QTRY_COMPARE(controller.state()["streamState"].toString(), QString("waiting"));
        QTest::qWait(500);
        controller.mode("local");
        QTRY_VERIFY_WITH_TIMEOUT(!controller.state()["streamBusy"].toBool(), 5000);
        qInfo() << "BOARD_USB_CANCEL" << controller.state()["streamState"]
                << controller.state()["streamMessage"];
        QCOMPARE(controller.state()["streamState"].toString(), QString("cancelled"));
        QVERIFY(controller.state()["error"].toString().isEmpty());
        auto banner = window->findChild<QQuickItem *>("transferBanner");
        QVERIFY(banner);
        QVERIFY(!banner->isVisible());
    }
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
        QCOMPARE(controller.image(1).size(), controller.state()["roi"].toRect().size());
        capture("magnify");
        controller.tool("preview");
        controller.mode("direct");
        QTRY_COMPARE(controller.state()["mode"].toString(), QString("direct"));
        QVERIFY(controller.state()["native"].toBool());
        controller.tool("magnify");
        QCOMPARE(controller.state()["mode"].toString(), QString("direct"));
        controller.mode("local");
        QTRY_VERIFY_WITH_TIMEOUT(!controller.state()["streamBusy"].toBool(), 5000);
        QCOMPARE(controller.state()["streamState"].toString(), QString("cancelled"));
        QVERIFY(!controller.state()["streamFile"].toBool());
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
