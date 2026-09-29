# 小智桌面固件：QDTech S3 → M5Stack Tab5 移植

这是一个**开发中的公开阶段存档**。Tab5 已能启动小智、连接 Wi‑Fi、进行语音对话并初始化摄像头。用户实测确认：把原 480 × 320 桌面双线性放大到全屏会让刷新和触摸明显变慢，因此该实验已撤回。`qdtech-tab5-native` 变体直接按 1280 × 720 绘制 Nabo 主界面与时分秒时钟，并加入第二页的配网、亮度、音量和网络电台。原 `qdtech-tab5` 变体仍可回退到 480 × 320 桌面。主界面铺满屏幕、触摸、对话、人体感应与时间日期已经在设备上验证；新应用页和电台声音仍需实测。详见 [交接本](HANDOFF.md) 和 [问题与计划](PORTING_STATUS.md)。

## v1.0.0 与在线升级

v1.0.0 是当前烧录在开发机上的进度快照，包含 `qdtech-tab5-native` 原生 1280 × 720 主界面、Nabo 形象与对话、天气/日期、网络电台与音乐歌词、FC 红白机与 USB 手柄、红外遥控、ICU 计算器、摄像头人体感应等模块；各模块的实机验证程度以 [PORTING_STATUS.md](PORTING_STATUS.md) 为准。设置页新增 **固件升级**：检查 GitHub 最新 Release，把安装包下载到 SD 卡并校验 SHA-256，然后由 `ota_0` 里的小型升级程序改写主固件，断电可重试，Wi‑Fi 等设置不受影响。原理与限制见 [HANDOFF.md](HANDOFF.md#v1002026-09-30与在线升级-ota)。

首次从旧版本升级到 v1.0.0 需要一次 USB 刷写（新分区表 + 主固件 + `updater/`）；之后可以直接在设备上升级。

## 来源与硬件

- 产品来源：[Liutupi/qdtech-s3-touch-lcd-3.5-xiaozhi-firmware](https://github.com/Liutupi/qdtech-s3-touch-lcd-3.5-xiaozhi-firmware)，取自提交 `d5435ca9a4f456804b280066b73a69cdf1b1dba6`。
- 小智底座：提交 `4632dc51f0a5ad26e08542e131e6e48da41e4ff3`。本仓库保留其 Git 历史和根目录 MIT 许可证。
- 硬件：[M5Stack Tab5](https://docs.m5stack.com/zh_CN/core/Tab5)，ESP32-P4 + ESP32-C6、720 × 1280 MIPI 屏、ES8388/ES7210 音频、SD 卡。已在一台 P4 v1.3、ST7121 屏设备上测试。
- `components/nofrendo` 源于上述 QDTech 项目，文件头声明 GNU Library General Public License v2；其他第三方组件由 ESP-IDF 组件管理器取得，各自许可证以组件内容为准。

## 从新电脑继续开发

安装 **ESP-IDF 6.0.2**（包含 ESP32-P4 工具链），克隆本仓库后在已激活 IDF 的终端运行：

```sh
python scripts/build.py qdtech/tab5 --name qdtech-tab5-native
```

`qdtech-tab5` 面向 P4 rev 1.x；`qdtech-tab5-p4x` 面向 rev 3 或更新芯片。先运行 `esptool.py --chip esp32p4 --port <串口> chip_id` 确认硬件版本。构建脚本使用 `main/boards/qdtech/tab5/config.json`、`sdkconfig.defaults*` 和 `partitions/qdtech_tab5.csv`。第一次构建需联网下载组件。不要将本地 `sdkconfig`、`build/`、设备备份、Wi‑Fi 凭据或 NVS 数据加入公开仓库。

烧录前阅读 [交接本的刷机与数据保护部分](HANDOFF.md#刷机与数据保护)。本次阶段存档的 FC 模拟器尚未在设备上验证；电台和播客也尚待播放实测。

编译产物保存在 [v0.1.0 阶段预发布页](https://github.com/Liutupi/qdtech-tab5-xiaozhi-firmware/releases/tag/v0.1.0-checkpoint)，包含应用、资源、bootloader、分区表和 SHA-256 清单。它是 **P4 rev 1.x 的未刷机开发快照**，存在上述显示问题；继续开发请以源码为准。

## 文档

- [HANDOFF.md](HANDOFF.md)：环境、代码入口、构建、已验证结果、继续工作的顺序。
- [PORTING_STATUS.md](PORTING_STATUS.md)：逐项状态、实机问题及可执行的后续计划。
- [Tab5 板级说明](main/boards/qdtech/tab5/README.md)。
