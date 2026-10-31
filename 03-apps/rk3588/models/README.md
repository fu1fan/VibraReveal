# MobileSAM 模型与运行时

采用 Rockchip Model Zoo 的 448px MobileSAM encoder / 五输入 decoder 适配。
不使用默认 1024px SAM 权重冒充这个导出布局。

来源：
- https://github.com/airockchip/rknn_model_zoo/tree/main/examples/mobilesam
- https://github.com/airockchip/MobileSAM
- https://github.com/ChaoningZhang/MobileSAM
- https://github.com/airockchip/rknn-toolkit2

模型权重、SDK headers/二进制依上游各自许可使用，不适用本仓库 MIT 的自动再许可。
仓库不内置第三方权重和 RKNN runtime；转换输出保存在独立目录。
本项目适配代码为自行实现，接口/预处理依据上述官方文档与示例核对。

从 Model Zoo 的 `model/download_model.sh` 获取 ONNX 后：

```sh
sudo apt-get install libgl1
python3 -m venv /tmp/vr-rknn-env
# CPU PyTorch 足以转换，避免安装无用 CUDA 依赖。
/tmp/vr-rknn-env/bin/pip install torch==2.4.0+cpu --index-url https://download.pytorch.org/whl/cpu
/tmp/vr-rknn-env/bin/pip install rknn-toolkit2==2.5.0 opencv-python==4.10.0.84 numpy==1.26.4
/tmp/vr-rknn-env/bin/python 03-apps/rk3588/models/convert_mobilesam.py \
  --encoder mobilesam_encoder_tiny.onnx --decoder mobilesam_decoder.onnx \
  --output-dir /tmp/mobilesam-rk3588
```

转换器固定 RK3588、默认 8 个提示 slots（`--prompt-slots` 可设置 2..64）、非量化 FP16，encoder mean/std 为 `[123.675,116.28,103.53]` /
`[58.395,57.12,57.375]`；生成 manifest 记录 Toolkit 版本、ONNX 和 RKNN SHA256。
只有两个模型都成功转换才生成 manifest；目录必须是新目录。
部署时保留 manifest，并使用与工具链匹配且经过板端验证的 runtime。
若需要隔离旧系统运行时：

```sh
LD_LIBRARY_PATH=/path/to/validated/runtime vr-select ...
```

不要直接覆盖 `/usr/lib/librknnrt.so`。上板初始化失败会明确返回 RKNN 错误，不回退 CPU 假装 NPU 成功。

本轮实际模型和运行时的哈希、源 ONNX 哈希见 [validated-manifest.json](validated-manifest.json)。
头文件与 runtime 对应上游提交 `2c723177a94fac65cbc036d722907ddbc91980cf`；
转换权重的字节身份以 manifest 为准，后续重新转换应生成新的 manifest，不能直接沿用旧哈希。
