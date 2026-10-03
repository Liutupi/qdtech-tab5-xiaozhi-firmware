# 小智桌面固件：QDTech S3 → M5Stack Tab5 移植

这是面向 M5Stack Tab5 的开发中固件。`qdtech-tab5-native` 变体直接按 1280 × 720 绘制 Nabo 主界面、时分秒翻页时钟与应用页；支持配网、亮度、音量、网络电台、音乐歌词、ICU 计算器和人体感应。原 `qdtech-tab5` 变体保留旧桌面。当前公开版本为 [v1.0.7](https://github.com/Liutupi/qdtech-tab5-xiaozhi-firmware/releases/tag/v1.0.7)；各功能的实机验证程度见 [移植状态](PORTING_STATUS.md) 和 [交接本](HANDOFF.md)。

## 固件与在线升级

设置页的 **固件升级** 会检查 GitHub 最新 Release，把主固件下载到 SD 卡并校验 SHA-256，然后由 `ota_0` 里的升级程序改写主固件；Wi‑Fi 等 NVS 设置不会被在线升级覆盖。原理与限制见 [HANDOFF.md](HANDOFF.md#v1002026-09-30与在线升级-ota)。

首次从旧分区布局升级需要一次 USB 刷写（分区表、主固件和升级程序）；已有兼容分区布局的设备可在设置页升级。v1.0.7 的 NAS 网易云点歌路径还要求 NAS 上的 Muse relay 和网易云 MCP 容器按 [板级说明](main/boards/qdtech/tab5/README.md) 配置；单独刷固件不会建立 NAS 服务。

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

烧录前阅读 [交接本的刷机与数据保护部分](HANDOFF.md#刷机与数据保护)。

P4 rev 1.x 的应用、资源、bootloader、分区表、升级程序及 SHA-256 清单见 [最新 Release](https://github.com/Liutupi/qdtech-tab5-xiaozhi-firmware/releases/latest)。其他芯片版本请按对应变体从源码构建。

## 文档

- [HANDOFF.md](HANDOFF.md)：环境、代码入口、构建、已验证结果、继续工作的顺序。
- [PORTING_STATUS.md](PORTING_STATUS.md)：逐项状态、实机问题及可执行的后续计划。
- [Tab5 板级说明](main/boards/qdtech/tab5/README.md)。
