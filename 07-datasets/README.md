# 07-datasets · 样本与数据索引

用于算法验证的小型样本数据与索引，不存放原始大文件。

## 约定

- 只提交小型样本或经过裁剪的数据
- 每个数据集附带索引文件：拍摄条件、参数、帧率、场景说明
- 记录校验值（如 SHA-256），保证样本与实验记录可对应
- 大体积原始采集数据保存在仓库外（CNB附件），通过索引引用

## RK3588 OV9281 RAW10 原始视频

两段原始采集数据以无损 7z 压缩包保存为 CNB 提交附件：

- [下载 RK3588_RAW10_lossless.7z](https://cnb.cool/fu1fan/VibraReveal/-/commit-assets/download/b157b0c319424751ac2bcd041c9e655e931c2399/RK3588_RAW10_lossless.7z)（2,501,418,289 字节，约 2.50 GB）
- [数据格式与原始文件校验值](https://cnb.cool/fu1fan/VibraReveal/-/commit-assets/download/b157b0c319424751ac2bcd041c9e655e931c2399/RAW_METADATA.json)
- [附件 SHA-256 清单](https://cnb.cool/fu1fan/VibraReveal/-/commit-assets/download/b157b0c319424751ac2bcd041c9e655e931c2399/SHA256SUMS)
- [解压校验脚本](https://cnb.cool/fu1fan/VibraReveal/-/commit-assets/download/b157b0c319424751ac2bcd041c9e655e931c2399/verify_raw_archive.py) · [数据说明](https://cnb.cool/fu1fan/VibraReveal/-/commit-assets/download/b157b0c319424751ac2bcd041c9e655e931c2399/RAW_README.md)

压缩包包含 `e10hz.raw` 和 `exp220.raw`，各 1,810 帧、3,706,880,000 字节；每帧 1280 × 800，按行排列，以小端 `uint16` 保存，低 10 位有效。历史处理使用 120 fps 参数，采集帧率未通过时间戳实测；模拟 60 fps 时应隔帧抽取。

压缩包 SHA-256：`e9cb68c04fc146e07689471486b779f33b38ad6bfd6b5eacf1542d605d092855`。

下载压缩包、`RAW_METADATA.json` 和 `verify_raw_archive.py` 到同一目录，安装 7-Zip 后运行 `python verify_raw_archive.py`，可流式解压并按元数据校验两份原始 RAW 的大小与 SHA-256。Windows 若未将 7-Zip 加入 PATH，可使用 `python verify_raw_archive.py --sevenzip "C:\Program Files\7-Zip\7z.exe"`。
