# 09-tools · 构建与辅助工具

支撑构建、部署和日常研发的脚本与工具。

## 计划覆盖

- RK3588 应用交叉编译与打包脚本
- 部署、升级与回滚脚本
- Mac 应用构建辅助
- 数据转换、样本裁剪与校验值生成
- 开发环境检查（Vulkan、Metal 工具链）

## 当前状态

`check-usb-link.py` 在 macOS 只读检查图像目的地址是否实际路由到 Apple NCM 网卡，防止误用 Wi-Fi 路径作为 USB 性能结果。

应用交叉编译及制品身份生成见 `10-env/build-image-transport.sh` 和 CNB `api_trigger_image_transport`。

其中交叉编译工具链、ELF2 增量构建与开发环境自检已由 [10-env](../10-env/README.md) 提供（`elf2-build`、`elf2-env`、`elf2-sysroot`），本目录只补充围绕它们的打包与部署脚本，避免重复实现。
