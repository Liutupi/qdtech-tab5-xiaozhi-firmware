# NABO 动画 v2 验证记录

日期：2026-10-01。素材使用内置 imagegen 生成，原始形象文件保持完整；生成提示词见 `source/animation-v2-prompts.md`。

## 已完成

- 八帧挥手，100 ms 节奏，1.3 秒结束；延迟回调跳到当前帧，无补帧循环。
- 八种反应：开心、眨眼、点赞、思考、好奇、倾听、安慰、听歌。
- 短按轮换动作，400 ms 内双击显示开心，18–25 秒待机反应；长按沿用感应测试。
- 倾听、用户文本和支持的情绪消息触发姿势；语音回复使用原有小块口型。睡眠与相机预览取消姿势。
- 16 张运行帧均为 300×561，鞋底基线统一；新增 PNG 按 RGB565 屏幕色彩精度准备，透明度保留。新增帧共 1,859,945 字节，比压缩前少 1,148,101 字节。
- 图像缓存上限 8 MiB，且不超过初始化时空闲 PSRAM 的一半。

## 检查结果

| 检查 | 结果 |
| --- | --- |
| `/Volumes/liutupi/tab5` 宿主测试 | 90/90 通过 |
| `/Users/tupi/tab5-build` 宿主测试 | 90/92 通过；两项既有配置断言失败，见下文 |
| 新姿势控制器测试 | 时间边界、延迟回调、过期、取消、动作替换通过 |
| C++ 格式 | 新文件和本次修改范围通过 clang-format 检查 |
| 16 张运行素材 | 数量、尺寸、共同鞋底基线及两个目录生成结果一致 |
| ESP-IDF 6.1，qdtech-tab5-native | 编译和链接成功，factory 分区容纳成功 |

实际构建使用 `/Users/tupi/esp-idf-v6.1`（工具报告 v6.1-dirty，子模块状态为已有环境状态）。标准构建入口已配置目标；外置 SDK 因 `._aes.c` 等 macOS 元数据文件被当成源码而失败，改用本机 SDK 和独立构建目录验证。

```sh
source /Users/tupi/esp-idf-v6.1/export.sh
idf.py -B build-nabo-v2 -DIDF_TARGET=esp32p4 -DBOARD_NAME=qdtech-tab5-native build
```

固件：`/Users/tupi/tab5-build/build-nabo-v2/xiaozhi.bin`

大小：11,341,440 字节（10.82 MiB）。factory 分区 12 MiB，剩余 1,241,472 字节（1.18 MiB）。`ota_0` 是既有 1 MiB 独立更新器分区，因此构建工具提示它装不下主程序；主程序运行于 factory。

SHA-256：`5331949eeb2dfa80b4c7eb348f6aaa0b65c9bff037a864db1f560e4e2c04568a`

构建副本中既有失败项（本轮未修改相应 config.json）：

- `test_qdtech_tab5_enables_decoder_for_its_compressed_fonts`
- `test_qdtech_tab5_rev1_disables_incompatible_isp_pipeline`

## 仍需真机确认

本轮没有烧录设备。GIF 为素材时序预览，不代表硬件帧率。真机需确认首次 PNG 解码和反复挥手时的流畅度、边缘及位置、双击和长按、倾听→思考→回复→休息切换，以及音乐/语音/相机并行时的音频连续性和 PSRAM。当前构建副本 UI 定时器 40 ms，旧项目快照 50 ms；两者挥手均按 100 ms 时间线推进。

该构建路径后续已更新为连续手臂 v3；以上大小及 SHA-256 是 v2 当时记录，当前结果见 `validation-v3.md`。
