import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import VibraReveal 1.0
ApplicationWindow {
    id: window
    objectName: "mainWindow"
    visible: true
    width: 1024; height: 600
    minimumWidth: 1024; minimumHeight: 600
    title: "VibraReveal"
    color: "#111820"
    palette.window: "#f7f8fa"
    palette.windowText: "#151a22"
    palette.text: "#151a22"
    palette.base: "#ffffff"
    palette.button: "#e9edf2"
    palette.buttonText: "#151a22"
    palette.highlight: "#075ac5"
    palette.highlightedText: "#ffffff"
    property var s: backend.state
    property string deletePath: ""
    property string deleteName: ""
    property real deleteBytes: 0
    property point exposurePoint: Qt.point(0,0)
    property bool rangePanel: false
    property bool selectionPan: false
    property var panelPositions: ({})
    property bool liveMotionParameters: s.tool === "magnify" || (s.tool === "select" && s.fileName.length === 0)
    property bool frequencyPanel: false
    property bool showSpectrum: true
    property bool showInset: true
    property bool closingAfterSave: false
    property bool recording: s.recordState === "recording" || s.recordState === "draining"
    function bytes(n) { return n >= 1073741824 ? (n/1073741824).toFixed(2)+" GiB" : (n/1048576).toFixed(1)+" MiB" }
    function clock(n) { return (Math.floor(n/60)<10?"0":"")+Math.floor(n/60)+":"+(Math.floor(n%60)<10?"0":"")+Math.floor(n%60) }
    function setParams(a,l,h,f,focus) { backend.parameters(a,l,h,f,focus) }
    onClosing: function(close) { close.accepted = backend.canClose(); if(!close.accepted) closingAfterSave = true }
    Connections {
        target: backend
        function onChanged() {
            if (s.tool !== "select" || !s.native) selectionPan = false
            if (!s.exposureEditable) exposureTimer.stop()
            if(closingAfterSave && !recording && !s.offlineBusy && !s.streamBusy) {
                closingAfterSave = false
                Qt.callLater(function() { window.close() })
            }
        }
    }
    FrameView { id: camera; objectName: "cameraView"; anchors.fill: parent; controller: backend; nativeSize: s.native }
    MouseArea {
        id: imageTouch; objectName: "imageTouch"; preventStealing: true; anchors.fill: parent; enabled: !s.selectionBusy
        property point startPoint
        property point previousPoint
        property point sourceStart
        property bool exposureGesture: false
        property bool exposureAdjusted: false
        property real exposureStartEV: 0
        onPressed: { exposureGesture = exposureTimer.running && s.exposureEditable; exposureAdjusted = false; exposureStartEV = s.exposureEV; startPoint = Qt.point(mouse.x,mouse.y); previousPoint = startPoint; sourceStart = camera.toSource(mouse.x,mouse.y); selectionBox.visible = s.tool === "select" && s.step === 1 && !selectionPan; selectionBox.x = mouse.x; selectionBox.y = mouse.y; selectionBox.width = 0; selectionBox.height = 0 }
        onPositionChanged: {
            if (!pressed) return
            if (s.tool === "select" && s.step === 1 && !selectionPan) {
                selectionBox.x = Math.min(startPoint.x,mouse.x); selectionBox.y = Math.min(startPoint.y,mouse.y)
                selectionBox.width = Math.abs(startPoint.x-mouse.x); selectionBox.height = Math.abs(startPoint.y-mouse.y)
            } else if (exposureGesture && (exposureAdjusted || Math.abs(mouse.y-startPoint.y)>Math.max(6,Math.abs(mouse.x-startPoint.x)))) {
                exposureAdjusted = true
                backend.exposureCompensation(Math.max(-3,Math.min(3,exposureStartEV+(startPoint.y-mouse.y)/120)))
                exposureTimer.restart()
            } else if (s.native && (s.tool !== "select" || selectionPan)) camera.panBy(mouse.x-previousPoint.x,mouse.y-previousPoint.y)
            previousPoint = Qt.point(mouse.x,mouse.y)
        }
        onReleased: {
            selectionBox.visible = false
            var end = camera.toSource(mouse.x,mouse.y)
            if (s.tool === "select" && !selectionPan) {
                if (s.step === 2) backend.selectEdge(end.x,end.y)
                else backend.selectObject(sourceStart.x,sourceStart.y,end.x-sourceStart.x,end.y-sourceStart.y,Math.abs(mouse.x-startPoint.x)+Math.abs(mouse.y-startPoint.y)<12)
            } else if (s.exposureEditable && !exposureAdjusted && Math.abs(mouse.x-startPoint.x)+Math.abs(mouse.y-startPoint.y)<12) {
                exposurePoint = end
                backend.meterExposure(end.x,end.y)
                exposureTimer.restart()
            }
        }
        onCanceled: selectionBox.visible = false
    }
    Timer { id: exposureTimer; interval: 5000; repeat: false }
    Item {
        objectName: "exposureIndicator"; visible: exposureTimer.running && s.exposureEditable
        property point center: { camera.viewRevision; return camera.toView(exposurePoint.x,exposurePoint.y) }
        x: center.x-36; y: center.y-36; width: 72; height: 72
        Rectangle { anchors.fill: parent; color: "transparent"; border.width: 4; border.color: "#80201800" }
        Rectangle { anchors.fill: parent; anchors.margins: 1; color: "transparent"; border.width: 2; border.color: "#ffd60a" }
        Rectangle {
            x: 0; y: 78; width: exposureLabel.implicitWidth+16; height: 30; radius: 6; color: "#101720"
            Text { id: exposureLabel; anchors.centerIn: parent; color: "#ffd60a"; font.pixelSize: 14; text: s.exposureAuto ? "自动曝光…" : (s.exposureEV>=0?"+":"")+Number(s.exposureEV).toFixed(1)+" EV · "+Number(s.exposureMs).toFixed(2)+" ms" }
        }
    }
    Rectangle { id: selectionBox; visible: false; color: "#167cb6ff"; border.color: "#91c5ff"; border.width: 2 }
    Item {
        id: selectedRegion; objectName: "editableRegion"
        visible: s.tool === "select" && s.step === 1 && (s.objectReady || s.selectionBusy) && s.roi.width>0
        enabled: !s.selectionBusy && !selectionPan
        property bool adjusting: false
        property rect draft: Qt.rect(0,0,0,0)
        property rect region: adjusting ? draft : s.roi
        property point begin: { camera.viewRevision; return camera.toView(region.x,region.y) }
        property point end: { camera.viewRevision; return camera.toView(region.x+region.width,region.y+region.height) }
        x: begin.x; y: begin.y; width: end.x-begin.x; height: end.y-begin.y
        function submit() {
            var r=draft
            backend.selectObject(r.x,r.y,r.width,r.height,false)
            adjusting=false
        }
        Rectangle { anchors.fill: parent; color: "transparent"; border.width: 2; border.color: "#ff453a" }
        MouseArea {
            objectName: "moveRegion"; anchors.fill: parent
            property point start
            property rect original
            function update(mx,my) {
                var at=mapToItem(camera,mx,my), now=camera.toSource(at.x,at.y)
                selectedRegion.draft=Qt.rect(Math.max(0,Math.min(s.width-original.width,original.x+now.x-start.x)),Math.max(0,Math.min(s.height-original.height,original.y+now.y-start.y)),original.width,original.height)
            }
            onPressed: { var at=mapToItem(camera,mouse.x,mouse.y); start=camera.toSource(at.x,at.y); original=s.roi; selectedRegion.draft=original; selectedRegion.adjusting=true }
            onPositionChanged: if(pressed) update(mouse.x,mouse.y)
            onReleased: { update(mouse.x,mouse.y); selectedRegion.submit() }
            onCanceled: selectedRegion.adjusting=false
        }
        Repeater {
            model: [[0,0],[0.5,0],[1,0],[1,0.5],[1,1],[0.5,1],[0,1],[0,0.5]]
            Rectangle {
                x: modelData[0]*selectedRegion.width-24; y: modelData[1]*selectedRegion.height-24
                width: 48; height: 48; color: "transparent"
                Rectangle { anchors.centerIn: parent; width: 14; height: 14; radius: 7; color: "#ff453a"; border.color: "white"; border.width: 2 }
                MouseArea {
                    objectName: "resizeRegion_"+index; anchors.fill: parent
                    property rect original
                    function update(mx,my) {
                        var at=mapToItem(camera,mx,my), p=camera.toSource(at.x,at.y)
                        var left=original.x, right=left+original.width, top=original.y, bottom=top+original.height
                        if(modelData[0]===0) left=Math.max(0,Math.min(right-8,p.x))
                        if(modelData[0]===1) right=Math.min(s.width,Math.max(left+8,p.x))
                        if(modelData[1]===0) top=Math.max(0,Math.min(bottom-8,p.y))
                        if(modelData[1]===1) bottom=Math.min(s.height,Math.max(top+8,p.y))
                        selectedRegion.draft=Qt.rect(left,top,right-left,bottom-top)
                    }
                    onPressed: { original=s.roi; selectedRegion.draft=original; selectedRegion.adjusting=true }
                    onPositionChanged: if(pressed) update(mouse.x,mouse.y)
                    onReleased: { update(mouse.x,mouse.y); selectedRegion.submit() }
                    onCanceled: selectedRegion.adjusting=false
                }
            }
        }
    }

    Rectangle { anchors.top: parent.top; width: parent.width; height: 150; gradient: Gradient { GradientStop { position: 0; color: "#80000000" } GradientStop { position: 1; color: "#00000000" } } }
    Row {
        objectName: "brandLogo"; x: 20; y: 22; spacing: 4; height: 30
        Repeater {
            model: [14, 30, 22, 14]
            Rectangle { width: 5; height: modelData; radius: 2.5; color: "white"; anchors.verticalCenter: parent.verticalCenter }
        }
    }
    Text { x: 66; y: 18; text: "VibraReveal"; color: "white"; font.pixelSize: 23; font.weight: Font.DemiBold }
    Text { x: 66; y: 49; text: s.demo ? "合成相机 · 功能验证" : "RK3588 · 触屏测量"; color: "#edf3fb"; font.pixelSize: 13 }
    Row {
        objectName: "modeMenu"; anchors.right: settingsButton.left; anchors.rightMargin: 12; y: 16; spacing: 4
        TouchButton { objectName: "localMode"; glyph: "scan-line"; text: "独立测量"; checked: s.mode === "local"; onClicked: backend.mode("local") }
        TouchButton { objectName: "directMode"; glyph: "monitor"; text: "电脑直通"; checked: s.mode === "direct"; onClicked: { if(recording) switchDialog.open(); else backend.mode("direct") } }
    }
    TouchButton { id: settingsButton; objectName: "settingsButton"; glyph: "sliders-horizontal"; iconOnly: true; anchors.right: parent.right; anchors.rightMargin: 18; y: 16; width: 50; text: "相机设置"; enabled: s.mode === "local" && !recording; onClicked: backend.tool("settings") }
    Row {
        x: 18; y: 86; spacing: 8; visible: s.tool !== "files"
        Rectangle { width: formatLabel.width + 42; height: 48; radius: 10; color: "#101720"
            Rectangle { x: 12; anchors.verticalCenter: parent.verticalCenter; width: 8; height: 8; radius: 4; color: s.ready ? "#91dfb2" : "#ff8980" }
            Text { id: formatLabel; x: 28; anchors.verticalCenter: parent.verticalCenter; color: "#f4f7fc"; font.pixelSize: 15; text: (s.ready || s.fileName.length > 0) ? s.width+" × "+s.height+" · RAW"+s.bits+" · "+Number(s.fps).toFixed(1)+" fps" : "相机未就绪" }
        }
        TouchButton { objectName: "fitView"; text: "适应"; checked: !s.native; enabled: s.tool !== "focus" && s.mode !== "direct"; onClicked: backend.view(false) }
        TouchButton { objectName: "nativeView"; text: "1:1"; checked: s.native; onClicked: backend.view(true) }
        TouchButton { objectName: "selectionPan"; visible: s.tool === "select" && s.native; text: selectionPan ? "返回选择" : "移动画面"; checked: selectionPan; onClicked: selectionPan = !selectionPan }
    }
    Rectangle {
        x: 18; y: 148; width: hint.width+24; height: 38; radius: 9; color: "#101720"; visible: s.tool !== "files" && hint.text.length > 0
        Text { id: hint; anchors.centerIn: parent; color: "#f4f7fc"; font.pixelSize: 14
            text: s.mode === "direct" || s.tool === "focus" ? "峰值对焦 · 1:1 · "+Number(s.previewFps).toFixed(0)+" fps · 拖动画面" : s.tool === "select" && selectionPan ? "拖动画面 · 点击返回选择继续选区" : s.tool === "select" ? "冻结参考帧 · "+(s.step === 1 ? (s.selectionBusy?s.message:s.useNpu?"轻触物品 · NPU 自动识别":"拖动框选 · 手动模式") : "轻触对象边缘") : s.tool === "magnify" ? "本机实时计算 · 参考位置固定" : s.tool === "replay" || s.tool === "offline" ? "本地 RAW 回放与分析" : s.native ? "点对点显示 · 拖动画面" : ""
        }
    }
    Flickable {
        id: inspector; objectName: "inspector"; z: 20
        readonly property var item: panelLoader.item
        property string positionKey: s.mode === "direct" ? "direct" : s.tool
        property real safeTop: 78
        readonly property real safeBottom: dock.visible ? dock.y-12 : playbackBar.visible ? playbackBar.y-12 : window.height-18
        width: item ? item.width : 302
        height: Math.min(contentHeight,Math.max(1,safeBottom-safeTop))
        contentWidth: width
        contentHeight: item ? item.height : 0
        visible: item !== null
        clip: true
        flickableDirection: Flickable.VerticalFlick
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { policy: inspector.contentHeight>inspector.height ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff }
        x: window.width-width-18
        y: safeTop
        function constrain() {
            x = Math.max(8,Math.min(x,window.width-width-8))
            y = Math.max(8,Math.min(y,window.height-height-8))
        }
        function restorePosition() {
            if (!item) return
            var saved = panelPositions[positionKey]
            x = saved ? saved.x : window.width-width-18
            y = saved ? saved.y : (s.tool === "record" || s.tool === "settings" ? 86 : 145)
            if (saved) constrain()
            else y = Math.max(safeTop,Math.min(y,safeBottom-height))
        }
        function moveBy(dx,dy) {
            x += dx; y += dy; constrain()
            panelPositions[positionKey] = Qt.point(x,y)
        }
        onPositionKeyChanged: Qt.callLater(restorePosition)
        onHeightChanged: if (item) restorePosition()
        onWidthChanged: if (item) restorePosition()
        onSafeBottomChanged: if (item) restorePosition()
        Loader {
            id: panelLoader
            onLoaded: { inspector.contentY=0; inspector.restorePosition() }
            sourceComponent: s.mode === "direct" ? directPanel : s.tool === "focus" ? focusPanel : s.tool === "select" ? (frequencyPanel?bandPanel:selectionPanel) : s.tool === "magnify" ? (frequencyPanel?bandPanel:magnifyPanel) : s.tool === "record" ? recordingPanel : s.tool === "settings" ? settingsPanel : (s.tool === "replay" || s.tool === "offline") ? (frequencyPanel?bandPanel:rangePanel?analysisRangePanel:offlinePanel) : null
        }
    }
    Connections {
        target: inspector.item
        ignoreUnknownSignals: true
        function onDragged(dx,dy) { inspector.moveBy(dx,dy) }
    }
    onWidthChanged: inspector.constrain()
    onHeightChanged: inspector.constrain()
    Component {
        id: focusPanel
        Panel {
            title: "峰值对焦"; onClosed: backend.tool("preview")
            Text { text: "峰值灵敏度 · "+Math.round(s.focus)+"%"; color: "#151a22"; font.pixelSize: 16 }
            Slider { id: focusSlider; objectName: "focusSlider"; width: parent.width; height: 42; from: 0; to: 100; value: s.focus; onPressedChanged: if(!pressed) setParams(s.alpha,s.low,s.high,s.processingFps,value) }
            Text { text: "转动镜头对焦环，观察绿色清晰边缘。完成后恢复此前显示比例。"; width: parent.width; wrapMode: Text.Wrap; color: "#414e60"; font.pixelSize: 15 }
            TouchButton { objectName: "finishFocus"; width: parent.width; primary: true; text: "完成对焦"; onClicked: backend.tool("preview") }
        }
    }
    Component {
        id: selectionPanel
        Panel {
            contentSpacing: 7
            title: s.step === 1 ? "选择放大区域" : "选择测量边缘"
            subtitle: s.useNpu ? "NPU · MobileSAM" : "手动框选 · 本机处理"
            onClosed: backend.tool("preview")
            Row {
                visible: s.step === 1; width: parent.width; spacing: 8
                TouchButton { objectName: "npuSelection"; text: "NPU 识别"; width: (parent.width-8)/2; height: 44; checked: s.useNpu; enabled: s.hasSam; onClicked: backend.selectionNpu(true) }
                TouchButton { objectName: "manualSelection"; text: "手动框选"; width: (parent.width-8)/2; height: 44; checked: !s.useNpu; onClicked: backend.selectionNpu(false) }
            }
            Text {
                width: parent.width; wrapMode: Text.Wrap; color: "#151a22"; font.pixelSize: 15
                text: s.selectionBusy ? s.message : s.objectReady ? (s.canChooseEdge?s.selectionMethod+" · "+Math.round(s.selectionMs)+" ms":s.message) : s.useNpu ? "轻触物品或拖动框选，提取对象轮廓。" : s.hasSam ? "拖动框选测量区域。" : "NPU 模型未配置，请手动框选。"
            }
            TouchButton { objectName: "centerSelection"; visible: s.step === 1 && !s.useNpu; width: parent.width; text: "使用中心 256 × 128 区域"; enabled: !s.selectionBusy; onClicked: backend.selectObject(s.width/2-128,s.height/2-64,256,128,false) }
            Text { width: parent.width; text: s.step === 2 ? "轻触红色轮廓；红框标出视频放大区域。" : "拖动红框移动区域，拖动圆点调整大小；松手后更新边缘。"; wrapMode: Text.Wrap; color: "#414e60"; font.pixelSize: 14 }
            TouchButton { objectName: "selectionParameters"; width: parent.width; text: "放大参数 · 增益 "+Number(s.alpha).toFixed(0); enabled: !s.selectionBusy && !s.offlineBusy; onClicked: frequencyPanel = true }
            TouchButton { objectName: "magnifyRegion"; visible: s.step === 1; width: parent.width; primary: true; text: "放大整个区域"; enabled: s.objectReady && !s.selectionBusy; onClicked: backend.magnifyRegion() }
            TouchButton { objectName: "confirmSelection"; primary: s.step === 2; width: parent.width; text: s.step === 1 ? "选择边缘" : "完成选择"; enabled: !s.selectionBusy && (s.step === 1 ? s.objectReady && s.canChooseEdge : s.edgeReady); onClicked: s.step === 1 ? backend.confirmObject() : backend.confirmEdge() }
        }
    }
    Component {
        id: magnifyPanel
        Panel {
            title: "实时运动放大"; subtitle: "RK3588 · 本机计算"; onClosed: backend.tool("preview")
            Text { width: parent.width; text: s.motionEstimate; wrapMode: Text.Wrap; color: "#151a22"; font.pixelSize: 14 }
            Text { text: "相位增益 · "+Number(s.alpha).toFixed(0); color: "#151a22"; font.pixelSize: 16 }
            Slider { width: parent.width; height: 36; from: 0; to: 80; value: s.alpha; onPressedChanged: if(!pressed) setParams(value,s.low,s.high,s.processingFps,s.focus) }
            CheckBox { visible: !s.areaMode; text: "叠加频谱"; checked: showSpectrum; onToggled: showSpectrum = checked; font.pixelSize: 16; height: 44; palette.windowText: "#151a22" }
            CheckBox { text: "叠加局部放大"; checked: showInset; onToggled: showInset = checked; font.pixelSize: 16; height: 44; palette.windowText: "#151a22" }
            TouchButton { width: parent.width; glyph: "chevron-right"; iconAfter: true; text: "频段与帧率 · "+s.low+"–"+s.high+" Hz"; onClicked: frequencyPanel = true }
        }
    }
    Component {
        id: bandPanel
        Panel {
            title: liveMotionParameters ? "放大参数" : "离线放大参数"; subtitle: liveMotionParameters ? "启动前检查计算量" : "离线保持原始采样率"; onClosed: frequencyPanel = false
            // Snapshot editable values once; polling must not erase unsubmitted changes.
            Component.onCompleted: { highSlider.value=s.high; lowSlider.value=s.low; gainSlider.value=s.alpha }
            Text { text: "相位增益 · "+Number(gainSlider.value).toFixed(0); color: "#151a22"; font.pixelSize: 16 }
            Slider { id: gainSlider; objectName: "parameterGain"; width: parent.width; height: 48; from: 0; to: 80; stepSize: 1; enabled: !s.offlineBusy }
            Text { text: "下限 · "+lowSlider.value.toFixed(1)+" Hz"; color: "#151a22"; font.pixelSize: 15 }
            Slider { id: lowSlider; width: parent.width; height: 36; from: 0.5; to: Math.max(1,highSlider.value-0.5); stepSize: 0.5 }
            Text { text: "上限 · "+highSlider.value.toFixed(1)+" Hz"; color: "#151a22"; font.pixelSize: 15 }
            Slider { id: highSlider; width: parent.width; height: 36; from: 1; to: Math.max(2,s.processingFps/2-0.5); stepSize: 0.5 }
            Row { visible: liveMotionParameters; spacing: 6; Repeater { model: [30,60,120]; TouchButton { text: modelData+" fps"; width: 84; enabled: s.fps >= modelData && Math.abs(s.fps/modelData-Math.round(s.fps/modelData))<0.001; checked: Math.abs(s.processingFps-modelData)<0.01; onClicked: { setParams(gainSlider.value,Math.min(lowSlider.value,modelData/8),Math.min(highSlider.value,modelData*.4),modelData,s.focus); lowSlider.value=s.low; highSlider.value=s.high } } } }
            TouchButton { objectName: "applyMotionParameters"; width: parent.width; primary: true; text: "应用参数"; enabled: !s.offlineBusy; onClicked: { setParams(gainSlider.value,lowSlider.value,highSlider.value,s.processingFps,s.focus); frequencyPanel = false } }
        }
    }
    Component {
        id: recordingPanel
        Panel {
            Text { width: parent.width; wrapMode: Text.Wrap; text: "Bitshuffle + LZ4 · 无损压缩"+(s.recordCompressed?" · 已写入节省 "+Number(s.recordSaving).toFixed(0)+"%":""); color: "#151a22"; font.pixelSize: 14 }
            title: s.recordState === "recording" ? "正在录制 RAW" : s.recordState === "draining" ? "正在完成写入" : s.recordState === "saved" ? "录制已保存" : s.recordState === "failed" ? "录制未完成" : "RAW 录制"
            subtitle: "本机 eMMC · DDR 缓冲上限 4 GiB"; onClosed: backend.tool("preview")
            Text { text: "DDR · "+bytes(s.bufferBytes)+" / "+bytes(s.bufferBudget || 4294967296); color: "#151a22"; font.pixelSize: 16 }
            ProgressBar { width: parent.width; from: 0; to: Math.max(1,s.bufferBudget); value: s.bufferBytes; height: 8 }
            Text { text: "时长  "+clock(s.recordSeconds)+"     写入  "+bytes(s.writeRate)+"/s"; color: "#151a22"; font.pixelSize: 15 }
            Text { text: "已落盘  "+bytes(s.written); color: "#414e60"; font.pixelSize: 15 }
            Text { width: parent.width; wrapMode: Text.Wrap; text: s.recordReason || "按可用内存确定缓冲预算。余量不足时停止采集此录像，排空后才显示已保存。"; color: s.recordState === "failed" ? "#b12924" : "#414e60"; font.pixelSize: 15 }
            TouchButton { objectName: "recordAction"; glyph: s.recordState === "recording" ? "square" : "circle-dot"; width: parent.width; danger: s.recordState === "recording"; primary: !recording; text: s.recordState === "recording" ? "停止录制" : s.recordState === "draining" ? "等待剩余帧写入…" : "开始新录制"; enabled: s.recordState !== "draining" && s.ready && !s.streamBusy; onClicked: s.recordState === "recording" ? backend.stopRecording() : backend.record() }
            TouchButton { width: parent.width; visible: s.recordState === "saved" || s.recordState === "failed"; glyph: "folder-open"; text: "查看录制文件"; onClicked: backend.tool("files") }
        }
    }
    Component {
        id: directPanel
        Panel {
            title: "电脑直通"; subtitle: "电脑负责录制与处理"; closable: false
            Row {
                width: parent.width; spacing: 10
                Icon { name: s.streamState === "streaming" || s.streamState === "complete" ? "circle-check" : "unplug"; tone: s.streamState === "streaming" || s.streamState === "complete" ? "success" : "record"; anchors.verticalCenter: parent.verticalCenter }
                Text { width: parent.width-34; wrapMode: Text.Wrap; text: s.streamMessage; color: s.streamState === "error" ? "#b12924" : "#151a22"; font.pixelSize: 17 }
            }
            Text { text: s.transport.toUpperCase()+" · RAW"+s.bits+" · "+Number(s.fps).toFixed(1)+" fps"; color: "#414e60"; font.pixelSize: 15 }
            Text { text: "已传输  "+bytes(s.streamBytes)+" · "+s.streamFrames+" 帧"; color: "#151a22"; font.pixelSize: 15 }
            Text { width: parent.width; wrapMode: Text.Wrap; text: "本屏固定为峰值对焦预览。选区与叠加内容不传入电脑，也不写入 RAW。"; color: "#414e60"; font.pixelSize: 15 }
            TouchButton { visible: !s.streamBusy; width: parent.width; text: "重新等待电脑"; onClicked: backend.retryStream() }
            TouchButton { width: parent.width; text: "返回独立测量"; onClicked: backend.mode("local") }
        }
    }
    Component {
        id: settingsPanel
        Panel {
            contentSpacing: 7
            title: "相机与设备"; subtitle: "曝光与拍摄帧率"; onClosed: backend.tool("preview")
            Text { text: s.width+" × "+s.height+" · RAW"+s.bits; color: "#151a22"; font.pixelSize: 18 }
            Text { text: "曝光 "+Number(s.exposureMs).toFixed(2)+" ms · 上限 "+Number(s.exposureMaxMs).toFixed(2)+" ms"; color: "#151a22"; font.pixelSize: 15 }
            Text { text: "曝光补偿 "+(s.exposureEV>=0?"+":"")+Number(s.exposureEV).toFixed(1)+" EV"; color: "#151a22"; font.pixelSize: 15 }
            Slider { objectName: "exposureSlider"; width: parent.width; height: 44; from: -3; to: 3; stepSize: .1; value: s.exposureEV; enabled: s.exposureEditable; onMoved: backend.exposureCompensation(value) }
            TouchButton { objectName: "centerMeter"; width: parent.width; text: "中心区域自动曝光"; enabled: s.exposureEditable; onClicked: { exposurePoint=Qt.point(s.width/2,s.height/2); backend.meterExposure(exposurePoint.x,exposurePoint.y); exposureTimer.restart() } }
            Text { text: "采集帧率 · "+Number(s.fps).toFixed(1)+" fps"; color: "#151a22"; font.pixelSize: 15 }
            Row { spacing: 6; Repeater { model: [30,60,120]; TouchButton { objectName: "captureFps"+modelData; width: 84; height: 44; text: String(modelData); checked: Math.abs(s.fps-modelData)<.1; enabled: s.exposureEditable && !s.cameraBusy; onClicked: backend.captureFps(modelData) } } }
            Text { width: parent.width; text: s.cameraStatus; wrapMode: Text.Wrap; color: "#151a22"; font.pixelSize: 14 }
            Text { width: parent.width; wrapMode: Text.Wrap; text: s.hasSam ? "NPU 物品识别 · 模型已配置" : "NPU 物品识别 · 模型未配置"; color: "#151a22"; font.pixelSize: 15 }
            Text { width: parent.width; wrapMode: Text.Wrap; text: "曝光不超过单帧时长。选区、测量、录制或传输期间锁定相机参数。"; color: "#414e60"; font.pixelSize: 14 }
            TouchButton { width: parent.width; text: "完成"; primary: true; onClicked: backend.tool("preview") }
        }
    }
    Component {
        id: offlinePanel
        Panel {
            contentSpacing: 7
            title: s.tool === "offline" ? (s.offlineBusy?"正在离线分析":"离线分析结果") : "RAW 回放"; subtitle: s.fileName; onClosed: backend.tool("files")
            Text { width: parent.width; visible: s.motionEstimate.length > 0; text: s.motionEstimate; wrapMode: Text.Wrap; color: "#151a22"; font.pixelSize: 14 }
            Text { width: parent.width; text: s.message; wrapMode: Text.Wrap; color: "#414e60"; font.pixelSize: 15 }
            TouchButton { width: parent.width; text: s.tool === "offline" ? "返回 RAW 回放" : "在当前帧选择对象"; enabled: !s.offlineBusy && !s.playing && (s.tool === "offline" || s.replayReady); onClicked: s.tool === "offline" ? backend.seek(s.position) : backend.tool("select") }
            TouchButton { objectName: "offlineParameters"; width: parent.width; glyph: "chevron-right"; iconAfter: true; text: "放大参数 · 增益 "+Number(s.alpha).toFixed(0); enabled: !s.offlineBusy; onClicked: frequencyPanel = true }
            TouchButton { objectName: "analysisRangeButton"; width: parent.width; text: "分析区间 · "+Number(s.analysisStart).toFixed(2)+" s 起"; enabled: !s.offlineBusy; onClicked: rangePanel=true }
            TouchButton { objectName: "globalAnalysis"; visible: s.tool === "replay"; width: parent.width; text: "全画面放大 · 跳过选区"; enabled: !s.offlineBusy && !s.playing && s.replayReady; onClicked: backend.analyzeGlobal() }
            TouchButton { width: parent.width; primary: true; glyph: s.offlineBusy ? "x" : "chart-no-axes-combined"; text: s.offlineBusy?"取消离线分析":"开始离线分析"; enabled: s.edgeReady || s.regionReady || s.offlineBusy; onClicked: s.offlineBusy ? backend.cancelAnalysis() : backend.analyze() }
            Text { width: parent.width; visible: s.offlineOutput.length > 0; text: "结果已保存为 VRV；原始 RAW 保留"; wrapMode: Text.Wrap; color: "#13623a"; font.pixelSize: 14 }
        }
    }
    Component {
        id: analysisRangePanel
        Panel {
            title: "RAW 分析区间"; subtitle: "仅计算所选时间段"; onClosed: rangePanel=false
            Text { width: parent.width; wrapMode: Text.Wrap; text: "起点 "+Number(s.analysisStart).toFixed(3)+" s · 默认使用选取对象的帧"; color: "#151a22"; font.pixelSize: 16 }
            TouchButton { objectName: "analysisFromCurrent"; width: parent.width; text: "使用当前回放帧为起点"; enabled: !s.playing && s.replayReady; onClicked: backend.analysisRange(0,true) }
            Text { width: parent.width; wrapMode: Text.Wrap; text: "分析时长 "+Number(s.analysisSeconds>0?Math.min(s.analysisSeconds,s.analysisRemaining):s.analysisRemaining).toFixed(3)+" s"; color: "#151a22"; font.pixelSize: 18 }
            Slider { objectName: "analysisDuration"; width: parent.width; height: 44; from: 1/s.fps; to: Math.max(from,s.analysisRemaining); stepSize: 1/s.fps; value: s.analysisSeconds>0?Math.min(s.analysisSeconds,to):to; onMoved: backend.analysisRange(value) }
            Row { spacing: 8
                TouchButton { width: 82; text: "−1秒"; onClicked: backend.analysisRange(Math.max(1/s.fps,(s.analysisSeconds>0?s.analysisSeconds:s.analysisRemaining)-1)) }
                TouchButton { width: 82; text: "+1秒"; onClicked: backend.analysisRange(Math.min(s.analysisRemaining,(s.analysisSeconds>0?s.analysisSeconds:s.analysisRemaining)+1)) }
                TouchButton { width: 92; text: "至末尾"; onClicked: backend.analysisRange(0) }
            }
            Text { width: parent.width; wrapMode: Text.Wrap; text: "全画面分析从当前回放帧开始；所选时长超过剩余录像时自动截到末尾。"; color: "#414e60"; font.pixelSize: 14 }
            TouchButton { width: parent.width; primary: true; text: "完成"; onClicked: rangePanel=false }
        }
    }
    Rectangle {
        x: 18; anchors.bottom: dock.top; anchors.bottomMargin: 18; width: 326
        height: measureColumn.height+28; radius: 16; color: "#f7f8fa"
        visible: (s.tool === "magnify" || s.tool === "offline") && ((showSpectrum && !s.areaMode) || showInset)
        Column { id: measureColumn; x: 14; y: 14; width: parent.width-28; spacing: 6
            Text { visible: showSpectrum && !s.areaMode; text: s.spectrumReady ? "主峰  "+Number(s.peak).toFixed(2)+" Hz" : (s.tool === "offline" && !s.offlineBusy ? "样本不足  " : "频谱采集中  ")+s.samples+" / 256"; color: "#151a22"; font.pixelSize: 18 }
            FrameView { visible: showSpectrum && !s.areaMode; width: parent.width; height: 110; channel: 2; controller: backend }
            Text { visible: showInset; text: "局部放大 · 相位增益 "+s.alpha; color: "#414e60"; font.pixelSize: 14 }
            FrameView { visible: showInset; width: parent.width; height: 86; channel: 1; controller: backend }
        }
    }
    Rectangle {
        id: filesPanel; objectName: "filesPanel"; x: 18; y: 86; width: parent.width-36; height: Math.max(1,dock.y-12-y); radius: 18; color: "#f7f8fa"; visible: s.tool === "files"
        Text { x: 20; y: 18; text: "录制文件 · "+backend.files.length; color: "#151a22"; font.pixelSize: 22 }
        TouchButton { anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 10; text: "返回预览"; onClicked: backend.tool("preview") }
        Text { x: 20; y: 50; width: parent.width-200; text: s.fileMessage.length ? s.fileMessage : "保存在本机 · 支持回放、分析和删除"; elide: Text.ElideMiddle; color: "#414e60"; font.pixelSize: 14 }
        ListView { id: fileList; objectName: "fileList"; x: 20; y: 78; width: parent.width-40; height: parent.height-96; clip: true; model: backend.files; spacing: 8
            delegate: Rectangle { width: fileList.width; height: 86; radius: 12; color: "#e9edf2"
                Column { x: 14; anchors.verticalCenter: parent.verticalCenter; spacing: 7; width: parent.width-320
                    Text { text: modelData.name; color: "#151a22"; font.pixelSize: 16; width: parent.width; elide: Text.ElideMiddle }
                    Text { text: clock(modelData.duration)+" · "+bytes(modelData.bytes)+(modelData.compressed?" · 无损压缩":" · 原始帧")+(modelData.synthetic?" · 合成源":""); color: "#414e60"; font.pixelSize: 14 }
                }
                Row { anchors.right: parent.right; anchors.rightMargin: 12; anchors.verticalCenter: parent.verticalCenter; spacing: 10
                    TouchButton { glyph: "chart-no-axes-combined"; text: "回放 / 分析"; enabled: !s.streamBusy && !s.offlineBusy; onClicked: backend.openFile(index) }
                    TouchButton { objectName: "deleteFile_"+index; text: "删除"; danger: true; enabled: !recording && !s.streamBusy && !s.offlineBusy; onClicked: { deletePath=modelData.path; deleteName=modelData.name; deleteBytes=modelData.bytes; deleteDialog.open() } }
                }
            }
            Text { anchors.centerIn: parent; visible: fileList.count === 0; text: "尚无已完成的 RAW 录像"; color: "#414e60"; font.pixelSize: 18 }
        }
    }
    Rectangle {
        id: dock; objectName: "toolDock"; anchors.bottom: parent.bottom; anchors.bottomMargin: 18; anchors.horizontalCenter: parent.horizontalCenter
        height: 82; width: 596; radius: 20; color: "#f7f8fa"
        visible: s.mode === "local" && s.tool !== "replay" && s.tool !== "offline"
        Row { anchors.centerIn: parent; spacing: 6
            Repeater { model: [{name:"对焦辅助",tool:"focus",glyph:"focus"},{name:"对象选择",tool:"select",glyph:"scan"},{name:"实时放大",tool:"magnify",glyph:"activity"},{name:"RAW 录制",tool:"record",glyph:"circle-dot"},{name:"录制文件",tool:"files",glyph:"folder-open"}]
                TouchButton { objectName: "tool_"+modelData.tool; glyph: modelData.glyph; iconAbove: true; glyphTone: danger ? "light" : modelData.tool === "record" ? "record" : checked ? "accent" : "dark"; text: modelData.name; width: 110; height: 66; checked: s.tool === modelData.tool; danger: modelData.tool === "record" && s.recordState === "recording"; onClicked: { frequencyPanel = false; backend.tool(modelData.tool) } }
            }
        }
    }
    Rectangle {
        id: playbackBar; objectName: "playbackBar"; anchors.bottom: parent.bottom; anchors.bottomMargin: 18; x: 18; width: parent.width-36; height: 66; radius: 15; color: "#f7f8fa"; visible: s.tool === "replay" || s.tool === "offline"
        RowLayout { anchors.fill: parent; anchors.margins: 8; spacing: 12
            TouchButton { glyph: s.playing ? "pause" : "play"; text: s.playing?"暂停":"播放"; enabled: !s.offlineBusy; onClicked: backend.playPause() }
            Text { text: clock(s.position); color: "#151a22"; font.pixelSize: 16 }
            Slider { Layout.fillWidth: true; from: 0; to: s.fileDuration; value: s.position; enabled: !s.offlineBusy; onPressedChanged: if(!pressed) backend.seek(value) }
            Text { text: clock(s.fileDuration); color: "#414e60"; font.pixelSize: 16 }
            TouchButton { glyph: "folder-open"; text: "返回文件"; enabled: !s.offlineBusy; onClicked: backend.tool("files") }
        }
    }
    Rectangle {
        x: 18; y: 440; height: 46; width: recordingLabel.width+28; radius: 10; color: "#e9edf2"; visible: recording && s.mode === "local"
        Text { id: recordingLabel; anchors.centerIn: parent; text: (s.recordState === "recording"?"● RAW 录制中  ":"采集已停止 · 写入中  ")+clock(s.recordSeconds); color: "#151a22"; font.pixelSize: 15 }
    }
    Rectangle {
        id: transferBanner; objectName: "transferBanner"; x: 18; y: 192; width: 480; height: 64; radius: 12; color: "#e9edf2"; visible: s.mode === "local" && s.streamFile && (s.streamBusy || s.streamState === "complete" || s.streamState === "error")
        Text { x: 12; y: 10; width: parent.width-100; text: s.streamMessage; wrapMode: Text.Wrap; color: "#151a22"; font.pixelSize: 15 }
        TouchButton { anchors.right: parent.right; anchors.rightMargin: 8; anchors.verticalCenter: parent.verticalCenter; visible: s.streamBusy; glyph: "square"; text: "停止"; onClicked: backend.stopStream() }
    }
    Rectangle {
        objectName: "errorBanner"; x: 18; anchors.bottom: parent.bottom; anchors.bottomMargin: 108; width: parent.width-36; height: errorText.implicitHeight+24; color: "#702c32"; radius: 12; visible: s.error.length > 0
        Text { id: errorText; x: 14; y: 12; width: parent.width-28; text: s.error; color: "#fff2f1"; wrapMode: Text.Wrap; font.pixelSize: 16 }
    }
    Dialog {
        id: deleteDialog; objectName: "deleteDialog"; anchors.centerIn: parent; width: 480
        modal: true; title: "删除这段录像？"; standardButtons: Dialog.NoButton
        background: Rectangle { color: "#f7f8fa"; radius: 18 }
        header: Text { padding: 20; text: deleteDialog.title; font.pixelSize: 22; font.bold: true; color: "#151a22" }
        contentItem: Text { text: deleteName+"\n"+bytes(deleteBytes)+"\n\n将永久删除 RAW 录像及附属说明，无法撤销。"; wrapMode: Text.Wrap; color: "#151a22"; font.pixelSize: 18 }
        footer: Row {
            padding: 12; spacing: 12
            TouchButton { objectName: "cancelDelete"; width: 216; text: "取消"; onClicked: deleteDialog.reject() }
            TouchButton { objectName: "confirmDelete"; width: 216; text: "删除录像"; danger: true; onClicked: deleteDialog.accept() }
        }
        onAccepted: { backend.deleteFile(deletePath); deletePath = "" }
        onRejected: deletePath = ""
    }
    Dialog {
        id: switchDialog; anchors.centerIn: parent; width: 440; modal: true; title: "停止录制并切换？"
        standardButtons: Dialog.NoButton
        footer: Row {
            padding: 12; spacing: 12
            TouchButton { text: "取消"; onClicked: switchDialog.reject() }
            TouchButton { text: "停止并切换"; primary: true; onClicked: switchDialog.accept() }
        }
        contentItem: Text { text: "先停止采集并保存 DDR 中剩余帧，完成后进入电脑直通。"; wrapMode: Text.Wrap; color: "#202733"; font.pixelSize: 18 }
        onAccepted: backend.mode("direct")
    }
}
