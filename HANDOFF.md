# Tab5 移植交接本

日期：2026-09-24。目标：把 [原 QDTech S3 小智固件](https://github.com/Liutupi/qdtech-s3-touch-lcd-3.5-xiaozhi-firmware)移植到 M5Stack Tab5。**当前为阶段版本，屏幕和字体问题未解决，原项目功能尚未全部实现。** 新电脑接手时先处理 [屏幕适配 Issue #1](https://github.com/Liutupi/qdtech-tab5-xiaozhi-firmware/issues/1)、[文字/时间 Issue #2](https://github.com/Liutupi/qdtech-tab5-xiaozhi-firmware/issues/2) 与 [PORTING_STATUS.md](PORTING_STATUS.md) 的 P0，再验证其余模块。

## 代码基线和机器

- 本仓库基于 XiaoZhi 提交 `4632dc51f0a5ad26e08542e131e6e48da41e4ff3`；原产品代码取自提交 `d5435ca9a4f456804b280066b73a69cdf1b1dba6`。
- 开发机为 Windows PowerShell + `C:\Espressif\esp-idf-v6.0.2`。当前工程目录 `D:\tab5\qdtech-tab5-firmware`，原产品只读对照目录 `D:\tab5\qdtech-source`。路径只是本机记录，新电脑按需替换。
- 实机为 ESP32-P4 rev 1.3、16 MB flash、32 MB PSRAM、ST7121/触控固件 1、ESP32-C6 Wi‑Fi、插有 SD 卡，串口当时是 COM5。`qdtech-tab5` 构建变体适用于此机；其他机器必须重新核对芯片/面板。
- 设备原始 16 MB 完整备份仅保留在旧电脑的 `D:\tab5\backups\tab5-original-2026-09-23.bin`，**不上传 GitHub**，因为可能包含网络凭据、设备身份等个人数据。若需要恢复，须由设备所有者安全地从旧电脑复制。
- 当前编译产物已放在 [v0.1.0 开发预发布页](https://github.com/Liutupi/qdtech-tab5-xiaozhi-firmware/releases/tag/v0.1.0-checkpoint)，供换电脑时对照；它没有刷入或实测 FC，也没有解决屏幕与文字问题。

## 新电脑的构建步骤

1. 安装 ESP-IDF 6.0.2 的 P4 工具链并激活 IDF 环境。Windows 示例：在 PowerShell 执行 `& 'C:\Espressif\esp-idf-v6.0.2\export.ps1'`。
2. `git clone https://github.com/Liutupi/qdtech-tab5-xiaozhi-firmware.git`，进入根目录。`components/nofrendo` 必须存在；其余第三方依赖由组件管理器下载。旧开发机里有组件管理器缓存与本地的 `esp_video`/`esp_ipa` 2.5.0/2.4.0 副本，这些第三方副本和生成的 `dependencies.lock` 没有上传；首次构建会重新求解和下载依赖。
3. 执行 `python scripts/build.py qdtech/tab5 --name qdtech-tab5`。这个脚本按 `main/boards/qdtech/tab5/config.json` 建立 P4 rev 1.x 配置并编译。P4 rev 3+ 使用 `--name qdtech-tab5-p4x`。本机曾用 `idf.py build` 在已有配置上增量编译，直接复制旧 `sdkconfig` 到新电脑并非必要。
4. 核对 `build/xiaozhi.bin` 的大小必须低于 `0x680000`；`build/generated_assets.bin` 必须低于 `0x2e0000`。分区表在 `partitions/qdtech_tab5.csv`：两个应用分区各 `0x680000`，资源分区 `0x2e0000`，16 MB 刚好用完。

## 刷机与数据保护

- 先确认芯片版本和串口，例如 `python -m esptool --chip esp32p4 --port COM5 chip-id`。设备原固件和 NVS 可能有重要信息，任何整片擦除或重刷都应先备份并取得使用者同意。
- 首次换分区表需烧录 bootloader、partition table、app、assets，可能使现有 OTA/NVS 状态改变。`idf.py -p COM5 flash` 仅在确认备份和变体后使用。
- 若设备已经在本项目分区布局上，增量代码试验可用 `idf.py -p COM5 app-flash`。本次测试的第二版使用此方式，保留了设备当前 NVS。不要把原机 flash 备份上传到公开仓库、Issue 或 Release。
- 固件 OTA 只允许 `qdtech-tab5-` 前缀且需 SHA256SUMS 校验；在发布正式兼容固件前，宁可保持 OTA 不可用，也不能用原 S3 的镜像更新 Tab5。

## 重要代码入口

| 路径 | 用途 |
| --- | --- |
| `main/boards/qdtech/tab5/m5stack_tab5.cc` | 板级驱动、ST712x/ILI9881C 探测、触摸、`QdtechTab5Display` 和软件放大旋转的 `FlushScaled`、各产品服务的启动。显示缺字与比例问题从这里查起。 |
| `main/boards/qdtech/tab5/desktop_ui.cc/.h` | 从原项目移植的桌面、所有页面与字体/时间标签。仍大量采用 480 × 320 固定坐标。 |
| `main/boards/qdtech/tab5/qd_font_*.c` | 随桌面移植的中文及钟表字库；检查 glyph/cmap 与 LVGL 9.5 的兼容性。 |
| `main/boards/qdtech/tab5/time_weather_service.cc` | SNTP、天气请求以及 `DesktopUI::SetTime`。后台同步成功不代表屏幕显示成功。 |
| `main/boards/qdtech/tab5/tab5_sd.cc` | P4 SDMMC slot 0 挂载，在 C6 ESP-Hosted slot 1 已初始化后复用 host。 |
| `main/boards/qdtech/tab5/radio_service.cc`, `podcast_service.cc`, `fc_emulator_service.cc` | 从 S3 项目适配的媒体功能。实机播放尚待验证。 |
| `main/boards/qdtech/tab5/photo_service.cc` | SD 相册；扫描成功但 JPEG 解码报错。 |
| `main/boards/qdtech/tab5/firmware_update_service.cc`, `main/ota.cc` | Tab5 专属 OTA 资产筛选及 SHA-256 验证。 |
| `main/application.cc`, `main/audio/audio_codec.cc` | 为外部 MP3/游戏播放加入音频占用控制和 PCM 输出。 |
| `components/nofrendo/` | 原项目引入的 FC 模拟器 C 组件。其源码有独立许可证声明。 |

## 实机验证记录

第一版启动后，在串口日志中确认 ST7121 屏识别、触摸坐标/切页、C6 Wi‑Fi 连接、天气获取、SNTP 同步、SD 4-bit 挂载、ES8388/ES7210 初始化、相机传感器探测、MQTT 语音对话。连续运行超过 6 分钟未见重启。相册扫描到 63 个 JPG，但第一张 `esp_jpeg_decode` 失败。

第二版增加电台/播客及重复点击修复，构建成功，`xiaozhi.bin` 大小为 `0x6389d0`；已用 `app-flash` 刷入并启动。串口显示 37 个电台站点、服务任务和 MCP 工具注册，**没有确认实际扬声器播放效果**。之后接线 FC 的源码阶段版本也已在 ESP-IDF 6.0.2 下编译通过，`xiaozhi.bin` 大小为 `0x64f9f0`，距离 app 分区上限只有 `0x30610`（约 193 KiB），**未刷入设备、未验证 FC 实际运行**。设备目前仍运行第二版。用户随后指出界面比例/清晰度和文字、数字、时间显示问题，优先级提升为 P0。

## 接续工作的顺序

1. 复现并修好文字、数字、时钟缺失。优先验证显示局部刷新和字体，再重做 1280 × 720 Tab5 横屏布局。需要实机照片与串口日志一起判断，不能只看模拟器或编译输出。
2. 在新布局下核对所有可见页面的字体大小、对齐、触摸命中和每秒刷帧性能。
3. 实测音频电台/播客与 FC，然后排查 JPEG 解码；记录可复现的媒体文件类型与日志。
4. 再补齐原项目尚未移植的服务，逐个与 Tab5 硬件能力、flash/PSRAM 余量核对。不要将桌面上出现一个入口等同于功能已完成。

## 发布约束

本仓库只包含源码和可复现配置。不要提交 `build/`、`sdkconfig`、NVS/原机备份、串口日志中的网络信息、SD 私人媒体、账号令牌。根目录是 XiaoZhi MIT 许可，`components/nofrendo` 源文件声明 GNU Library General Public License v2；发布二进制前应核实所有依赖及资源素材的再分发条件。
