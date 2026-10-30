# VibraReveal V7.3 结构设计与整机 BOM

2026-10-30 文档整理版。**几何仍为 V7.3，条件工程试装版，非制造或采购冻结。** 本版只重排装配 PDF、补齐整机 BOM 并按用途重排目录；不改变零件、孔位或装配位置。V7.2 及更早版本不改动。

## 从这里开始

- `docs/VibraReveal_V7.3_中文尺寸与装配_易读版.pdf`：15 页，正文 12 pt / 行距 20 pt，包含尺寸、安装与维护要求。
- `assembly/bom/VibraReveal_V7.3_整机完整BOM.xlsx`：61 项可筛选整机清单，含净用量、备料、确认状态与来源。
- `assembly/bom/VibraReveal_V7.3_整机完整BOM.pdf`：33 页宽松阅读版；CSV 和 JSON 与工作簿同源。
- `review/renders/`：最终实际 CAD 主效果、56 mm 正交侧视、IMU 背面焊脚浮空近景。

## 目录结构

| 目录 | 内容 | 是否可加工 |
| --- | --- | --- |
| `drawings/acrylic/` | A01–A03 亚克力 DXF（1:1 mm）+ 对应 STEP | 是 |
| `drawings/print/` | P01–P05 打印件 STL/STEP（6 个实例） | 是 |
| `drawings/templates/` | 0.3 mm IMU 绝缘膜开窗模板 DXF | 是（单独一项） |
| `assembly/` | 完整装配 STEP + BOM（xlsx/pdf/csv） | 否，装配与采购 |
| `docs/` | 易读版 PDF、设计决策记录 | 否 |
| `review/` | 渲染图与展示用剖切/近景（STEP+STL） | 否，仅供查看 |
| `qa/` | 各次静态检查、来源与渲染溯源、文件校验清单 | 否 |
| `src/` | `build.py`、参数、BOM 数据、QA/渲染/出图脚本、公开来源 STEP | 源头 |
| `build/` | 重建产物（全部实体 STL、STEP、DXF 与参数快照），**不入库** | 否 |

## 制造与复现

- `drawings/acrylic/A*.dxf`：3 张亚克力，1:1 mm。A01/A02 厚 6 mm，A03 厚 3 mm；无激光切缝补偿，先做孔径试片；圆弧以 DXF 为准。
- `drawings/print/P*.stl` / `P*.step`：6 个打印实例、5 种设计，按装配世界坐标导出。切片按受力方向重定摆放，建议 PETG/ASA 试制；禁止打印承载 1/4-20 螺纹。
- `drawings/templates/IMU_insulating_film_t0p3_TEMPLATE.dxf`：0.3 mm 绝缘膜独立 1:1 裁切模板，不混入亚克力加工任务，也不是第四张亚克力。
- `assembly/VibraReveal_V7_3_assembly.step`：完整装配。`build/stl/` 内其余模块和硬件包络用于检查/渲染，不要全部打印。
- `review/presentation/`：展示用裁切/剖视，不是制造件，不得按剖面切开实际主架。
- `src/sources/`：重建所需公开来源 STEP。来源及未确认项见 `src/sources/source_manifest.json`、`src/bom/v73_electronics.json`。
- 参数在 `src/build.py`；`build/parameters.json` 只是快照。CAD 坐标 X 左右 / Y 前 / Z 上，ELF2 局部 `X=60-u / Z=v+34.5`。任何孔、柱、灯或冷靴变更需联动复查碰撞、支承和维修。命令见下文「重建」。

## 关键尺寸与约束

| 部位 | 关键值 | 说明 |
| --- | --- | --- |
| 56 mm 基准 | PCB 前 Y54.7 → 腹板后 Y110.70 | 基准**面距**，不是净空 |
| 侧向最短距 | 风扇组件至主架 15.2、至长筋 26.4 | 简化实体最短距，非实测装配保证 |
| 主架 | 腹板 Y110.70…115.70 厚 5；底脚 90×42×5，孔 X±37 / Y81.70、103.70；顶板 Z87…91 | 相比 V6 为 5/5/4，模型体积非切片质量 |
| IMU 开窗 | 开口 X33…41、Z60.5…83.5，根部 R1.5，贯穿 Y110.70…115.70 | 孔边到开口 7.1 是名义余量，非强度证明 |
| 相机 | 孔 X±10.6 / Z65.3、78.7；M2 AF4 铜柱长 8；PCB Y124.20…125.80 | 横距 21.2 有来源，纵 13.4 为推定 |
| IMU 装配 | 中心 X30/Z72，孔 (24.8,64.35)/(24.8,79.65)；0.3 膜后 PCB Y116.00…117.60 | M2×10 + 后螺母，焊脚必须浮空 |
| 顶部冷靴 | 顶板 Z87…91、通孔 D6.8；六角槽 AF11.4、深 8（Z79…87） | 约 **9 mm 是座面下露牙，不是螺钉总长** |
| 继电器 | 中心 X55/Y60，四孔 X46.35/63.65、Y51.35/68.65 | 模块含 PCB 总高 15，四短柱 6 + 0.5 垫 |
| 灯 | VL49 66×80×30、101 g，含参考脚 Z94.6…174.6 | 灯中心 Y116.70 至三脚架中心 Y65，悬臂 51.70 |

## 已知实测与暂定值

- 实测：ELF2 PCB 厚 1.6；前散热器 12 / USB 叠层 16.4；后 M2 突出 5.4；正反包络 23.4（不含风扇）。
- 暂定：2510 风扇 5 V、25×25×10、孔距 20；散热器 40×36 仅按照片取保守占地。
- 官方件：透明盖外廓 79.5×100、四孔 D3.5 / 68×90.6、排针槽 10×83.6；A03 后 70.7 / 前 73.7，风扇前 83.7。
- 薄壁处：CSI 侧耳孔边材料 1.75、D6.4 垫片距缺口 0.30，须轻扭矩并校核公差。
- 官方 PDF 22/15 与 DXF 22.6/15.6 的差异源于混读不同开口：实际缺口 22.60/15.60、中央方孔 22，来源一致。

## 装配与维护顺序

- 松散主架上先装相机后柱螺钉、IMU 背螺母；后撑按 底脚 → 竖向螺钉 → LCD 顺序，维修反序。
- 顶部螺母：相机未装时从前方 Z73.5…79 窗口送入，上抬至 Z81.5…87 并扶住，再装相机；无独立防掉盖，拆净螺钉可能掉出。
- 继电器原位工具受阻：断线 → 卸底部 4 螺钉 → 沿 +X 移 30 → +Z 提 50 后维护；不要承诺原位拧所有螺钉。
- 灯先解锁并沿 +Y 前滑约 50 再抬起；盖维修先卸灯，再拔风扇线、移开 GPIO 外插头、松 FPC，拆四盖螺钉后 +Z 提 150。
- D6 工具孔在主架 X−34.1/Z38，仅作细杆通道，手柄与批头槽型未验证。
- IO：带灯时 4 条 GPIO 拔出/侧移路径撞灯，须先卸灯；卸灯后 10 条名义通道通过。GPIO 按 26.2 深假设，非无限前方进出。
- 三脚架座前排工具可能被背部 M.2 挡住，先装座后装板。

## 不能跳过的核对

- FPC 按用户要求列 **200 mm**，两端针数/节距/接触方向/引脚映射及实际 U 弯适配均未确认；200 mm 不代表信号或装配验证通过，不能照渲染硬折排线。
- 开发板实物版本与内存/存储 SKU、主电源功率及外设 5 V 分配、继电器触发接线、实际镜头和模块孔位仍须核实。电源资料的 12 V 3 A 与 12 V 20 W PD 差异已保留，不能据清单盲购。
- 顶部新冷靴：外廓、牙距、头下厚度、是否需拆定位销均未实测；须按真实螺母核对有效牙合及不顶底，不薄削顶板迁就短螺钉。槽只防螺母转，不等于整只冷靴防转。
- IMU：焊脚与焊锡必须浮空，绝缘膜必须按开窗模板裁切并去掉排针侧；模型的针长、焊锡和元件包络不等于实物尺寸。8 组针尾按 X36.5、Z63.11+i×2.54 假设，实际须逐点核对；不得挤压焊点或用绝缘膜掩盖接触。
- 材料强度、温升、振动、跌落与长期蠕变未验证；风扇仍经盖柱/板卡/底架传振，未隔振，风路、回流与温升未验证，不预设停扇热安全。
- 相机纵 13.4、继电器孔距、2510 孔距、镜头与 FPC 包络均为推定或照片值；标准柱不能补偿实际孔距错误。
- 模型体积与静态无干涉检查只针对简化几何，不构成强度、散热或实物装配合格结论。

## 数量与验证

独立机械计数：59 颗螺钉、24 颗螺母、22 根支柱、65 片垫片、1 张膜；另 3 张亚克力和 6 个打印实例。模块自带脚、锁轮、焊脚等不重复采购。先盘点已有件和套件自带件，净用量不是新购数量。

既有 V7.3 QA 为 225 个实体，含假设焊脚包络，未发现正体积静态相交；9 个制造实体有效/网格闭合。动态插拔和工具通道只在上述拆装顺序与假设尺寸下成立。此次目录重排以文件哈希确认几何未变化，不把旧静态检查扩展成实物认证。

## 文件完整性

`qa/SHA256SUMS.txt` 为本树校验清单（不含重建产物 `build/`）；`qa/RELEASE_MANIFEST.json` 记录几何保留与公开信息检查。未包含私人消息、用户附件照片、凭据、旧版本和检查用页面截图。

## 重建

在本目录（含 `src/`、`build/`）执行。`build/` 是临时产物，**每次运行都会被清空重写**，不要在其中手工修改。

```bash
# 几何 + QA（cadquery / ezdxf，Python 3.12）
python src/build.py                     # 全部实体写 build/，9 个制造件发布到 drawings/，装配体写 assembly/
python src/scripts/verify_all_assembly.py
python src/scripts/verify_geometry.py
python src/scripts/verify_service.py
python src/scripts/check_relay_service_final.py
python src/scripts/export_film_template.py
python src/scripts/verify_IMU_rear.py

# 渲染（Blender 4.x + Pillow）
blender -b -t 8 -P src/scripts/render_cad.py -- build/render_manifest.json --output-dir review/renders
python src/scripts/annotate_views.py && python src/scripts/annotate_imu.py

# 文档与 BOM（reportlab / PyMuPDF / ezdxf / Pillow）
python src/scripts/make_pdf_readable.py                    # → docs/
python src/bom/make_bom_pdf.py                             # → assembly/bom/
node src/bom/build_bom_workbook.mjs assembly/bom src/bom    # 需 @oai/artifact-tool 运行时

# 校验清单（文件名含空格与中文，必须 -print0；生效于任何影响交付的改动之后）
find . -path ./build -prune -o -path ./.git -prune -o -name __pycache__ -prune -o \
  -type f -print0 | sort -z | grep -zv '^./qa/SHA256SUMS.txt$' | xargs -0 sha256sum > qa/SHA256SUMS.txt
```

- QA 输出写 `qa/`；`src/sources/` 下两个公开来源 STEP 是完整重建的必需输入。
- 装配 PDF 读取 `src/docs_assets` 文本、`build/` 面板坐标、`drawings/acrylic` 的 DXF 与 `review/renders` 的 PNG；中间体 `build/body.pdf` 不是交付件。
- 注释字体需本地 Noto Sans CJK；工作簿脚本缺 `@oai/artifact-tool` 时，JSON 与 UTF-8 CSV 仍可手工维护。
- 可加工件只有 `drawings/` 下的 A*.dxf、P*.stl 与膜模板；`build/stl` 内 REF/硬件/展示几何仅供检查与渲染。
