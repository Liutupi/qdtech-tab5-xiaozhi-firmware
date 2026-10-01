# Tab5 移植交接本

日期：2026-09-24。目标：把 [原 QDTech S3 小智固件](https://github.com/Liutupi/qdtech-s3-touch-lcd-3.5-xiaozhi-firmware)移植到 M5Stack Tab5，并优先做好屏幕清晰度、触摸响应、小智形象和相机。**当前仍为阶段版本。** 用户否定了旧桌面双线性放大全屏的体验，现另建原生分辨率主界面，实机视觉和触摸效果待验收。接手时继续处理 [屏幕适配 Issue #1](https://github.com/Liutupi/qdtech-tab5-xiaozhi-firmware/issues/1)、[文字/时间 Issue #2](https://github.com/Liutupi/qdtech-tab5-xiaozhi-firmware/issues/2) 与 [PORTING_STATUS.md](PORTING_STATUS.md) 的 P0。

## 代码基线和机器

- 本仓库基于 XiaoZhi 提交 `4632dc51f0a5ad26e08542e131e6e48da41e4ff3`；原产品代码取自提交 `d5435ca9a4f456804b280066b73a69cdf1b1dba6`。
- 开发机为 Windows PowerShell + `C:\Espressif\esp-idf-v6.0.2`。当前工程目录 `D:\tab5\qdtech-tab5-firmware`，原产品只读对照目录 `D:\tab5\qdtech-source`。路径只是本机记录，新电脑按需替换。
- 实机为 ESP32-P4 rev 1.3、16 MB flash、32 MB PSRAM、ST7121/触控固件 1、ESP32-C6 Wi‑Fi、插有 SD 卡，串口当时是 COM5。`qdtech-tab5` 构建变体适用于此机；其他机器必须重新核对芯片/面板。
- 设备原始 16 MB 完整备份仅保留在旧电脑的 `D:\tab5\backups\tab5-original-2026-09-23.bin`，**不上传 GitHub**，因为可能包含网络凭据、设备身份等个人数据。若需要恢复，须由设备所有者安全地从旧电脑复制。
- 2026-09-24 在 macOS 上另存了刷机前的完整 16 MB 备份，位于仓库外的 `/Volumes/liutupi/tab5-private-backups/tab5-before-optimization-2026-09-24.bin`，SHA-256 为 `d4cce09bb0d7ea8f2611c7b6f825018ee77ef06f9f777f7a54c9a43d777c7774`。仅供本机恢复，不上传。
- 加入本地视觉模型前，又完整备份了当前 Nabo 系统：`/Volumes/liutupi/tab5-private-backups/tab5-pre-vision-full-flash-2026-09-24.bin`（16,777,216 字节；SHA-256 `df22aa55903673e77723f41760596499b2a4a866dc8b15a3faaaae398c9ff621`）。该备份同样不得上传。
- 当前编译产物已放在 [v0.1.0 开发预发布页](https://github.com/Liutupi/qdtech-tab5-xiaozhi-firmware/releases/tag/v0.1.0-checkpoint)，供换电脑时对照；它没有刷入或实测 FC，也没有解决屏幕与文字问题。

## 新电脑的构建步骤

1. 安装 ESP-IDF 6.0.2 的 P4 工具链并激活 IDF 环境。Windows 示例：在 PowerShell 执行 `& 'C:\Espressif\esp-idf-v6.0.2\export.ps1'`。
2. `git clone https://github.com/Liutupi/qdtech-tab5-xiaozhi-firmware.git`，进入根目录。`components/nofrendo` 必须存在；其余第三方依赖由组件管理器下载。`main/idf_component.yml` 将 `esp_lvgl_port` 固定在 2.8.0，以兼容 ESP-IDF 6.0.2；2.9.0 使用此 IDF 尚不存在的 DPI 回调字段。旧开发机里有组件管理器缓存与本地的 `esp_video`/`esp_ipa` 2.5.0/2.4.0 副本，这些第三方副本和生成的 `dependencies.lock` 没有上传；首次构建会重新求解和下载依赖。
3. P4 rev 1.x 的原生小智界面执行 `python scripts/build.py qdtech/tab5 --name qdtech-tab5-native`；旧桌面回退版为 `--name qdtech-tab5`。P4 rev 3+ 的旧桌面为 `--name qdtech-tab5-p4x`，原生版尚未为 rev 3+ 单独验收。新电脑无需复制旧 `sdkconfig`。
4. 核对 `build/xiaozhi.bin` 小于 `0xc00000`（`factory` 分区）；`build/generated_assets.bin` 小于 `0x2e0000`。分区表在 `partitions/qdtech_tab5.csv`：`factory` 12 MB（主固件，偏移 `0x20000`）、`ota_0` 1 MB（SD 卡升级程序，偏移 `0xc20000`）、`assets` `0x2e0000`，16 MB 刚好用完。旧的单 `ota_0` 13 MB 布局和更早的双 OTA 布局都已作废。

## 刷机与数据保护

- 先确认芯片版本和串口，例如 `python -m esptool --chip esp32p4 --port COM5 chip-id`。设备原固件和 NVS 可能有重要信息，任何整片擦除或重刷都应先备份并取得使用者同意。
- v1.0.0 起分区表改为 `factory`（主固件）+ `ota_0`（升级程序）。NVS 与 assets 的位置未变，因此 Wi‑Fi 等设置得以保留；但首次从旧布局升级必须一次性用 USB 刷入 bootloader、分区表、主固件和升级程序（见下文“在线升级 OTA”）。不要把新分区表和旧布局的镜像混刷。
- 若设备已经在本项目分区布局上，增量代码试验可用 `idf.py -p COM5 app-flash`。本次测试的第二版使用此方式，保留了设备当前 NVS。不要把原机 flash 备份上传到公开仓库、Issue 或 Release。
- 在线升级采用 SD 卡暂存方案，不需要第二个 12 MB 应用槽：设置页 → 固件升级 会把 GitHub Release 下载到 SD 卡并校验 SHA-256，然后重启进入 `ota_0` 里的小型升级程序，由它改写 `factory` 并再次校验。详见下文。不能用原 S3 的镜像更新 Tab5。

## 重要代码入口

| 路径 | 用途 |
| --- | --- |
| `main/boards/qdtech/tab5/m5stack_tab5.cc` | 板级驱动、ST712x/ILI9881C 探测、触摸坐标、旧桌面的 `FlushScaled`，以及原生版与旧桌面的选择。 |
| `main/boards/qdtech/tab5/tab5_native_display.h` | 新原生 1280 × 720 小智主界面、局部动画、对话文字和触摸按钮。 |
| `main/boards/qdtech/tab5/tab5_vision_service.cc`, `main/boards/common/esp_video.cc` | 低频摄像头取帧、活动检测、睡眠切换与本地人体感应问候。 |
| `main/boards/qdtech/tab5/desktop_ui.cc/.h` | 从原项目移植的桌面、所有页面与字体/时间标签。仍大量采用 480 × 320 固定坐标。 |
| `main/boards/qdtech/tab5/qd_font_*.c` | 随桌面移植的中文及钟表字库；检查 glyph/cmap 与 LVGL 9.5 的兼容性。 |
| `main/boards/qdtech/tab5/time_weather_service.cc` | SNTP、天气请求以及 `DesktopUI::SetTime`。后台同步成功不代表屏幕显示成功。 |
| `main/boards/qdtech/tab5/tab5_sd.cc` | P4 SDMMC slot 0 挂载，在 C6 ESP-Hosted slot 1 已初始化后复用 host。 |
| `main/boards/qdtech/tab5/radio_service.cc`, `podcast_service.cc`, `fc_emulator_service.cc` | 从 S3 项目适配的媒体功能。实机播放尚待验证。 |
| `main/boards/qdtech/tab5/photo_service.cc` | SD 相册；扫描成功但 JPEG 解码报错。 |
| `main/boards/qdtech/tab5/tab5_ota.cc/.h`、`updater/` | v1.0.0 在线升级：主固件里的下载/校验服务与 `ota_0` 里的 SD 卡升级程序。旧桌面版的 `firmware_update_service.cc` 仅在 `qdtech-tab5` 变体中编译。 |
| `main/application.cc`, `main/audio/audio_codec.cc` | 为外部 MP3/游戏播放加入音频占用控制和 PCM 输出。 |
| `components/nofrendo/` | 原项目引入的 FC 模拟器 C 组件。其源码有独立许可证声明。 |

## 实机验证记录

第一版启动后，在串口日志中确认 ST7121 屏识别、触摸坐标/切页、C6 Wi‑Fi 连接、天气获取、SNTP 同步、SD 4-bit 挂载、ES8388/ES7210 初始化、相机传感器探测、MQTT 语音对话。连续运行超过 6 分钟未见重启。相册扫描到 63 个 JPG，但第一张 `esp_jpeg_decode` 失败。

第二版增加电台/播客及重复点击修复，构建成功，`xiaozhi.bin` 大小为 `0x6389d0`；已用 `app-flash` 刷入并启动。串口显示 37 个电台站点、服务任务和 MCP 工具注册，**没有确认实际扬声器播放效果**。之后接线 FC 的源码阶段版本也曾在 ESP-IDF 6.0.2 下编译通过，`xiaozhi.bin` 大小为 `0x64f9f0`，当时**未刷入设备、未验证 FC 实际运行**。

2026-09-24：用户发来的两张实机照片显示内置 Montserrat 的英文、日期和温度可见，中文及大时钟数字空白。三个 Tab5 自带字库的 `.bitmap_format = 1`，但原配置关闭了 `CONFIG_LV_USE_FONT_COMPRESSED`；LVGL 因而无法解码。两个 Tab5 变体现已启用压缩字库，20 px 中文标签也改用真正的 20 px 字库。P4 rev 1.x 变体关闭了 `ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER`：曾用 ESP-IDF 6.1 构建的试验镜像在摄像头探测后于 `esp_ipa_pipeline_create` 非法指令崩溃，已恢复原固件，再使用 ESP-IDF 6.0.2 重建。当前 6.0.2 镜像大小 `0x641170`，仅写入 `ota_0` 应用区；写后哈希通过，串口确认 ST7121、SC202CS、桌面、SD、相机取帧均成功，25 秒内未见重启。**新固件的文字显示仍需用户拍摄屏幕确认；FC、音频和原生横屏均未验收。**

随后用户的新照片确认大时钟和多数中文已恢复，但“智”等字仍缺，左右仍有白边，最近邻放大有明显像素感。原因是原 16/20 px 中文字库仅有 767 个非 ASCII 字形，未包含桌面源码用到的全部汉字。现用同一来源字体重新生成两个含 998 个非 ASCII 字形的子集，增加字库覆盖检查和 OFL 许可证文本。另有一版实验显示路径把旧桌面放大到完整 1280 × 720，使用双线性过滤；此版不是原生高分辨率布局，画面可能横向拉伸，需实机照片和触控、性能复验。实验版 `xiaozhi.bin` 为 `0x6508a0`，已刷入 P4 rev 1.3，写后哈希通过，30 秒串口未见崩溃，桌面与相机启动成功。实验版镜像私有备份为 `/Volumes/liutupi/tab5-private-backups/tab5-fonts-fullscreen-test-2026-09-24.bin`，SHA-256 `4c193447d7626cc1e4b782466e6be610044b02be18ed0f5c5ed8616608a07150`。

本次源码在 ESP-IDF 6.0.2 下通过 85 项宿主测试，构建 `qdtech-tab5`（`0x6508a0`）、`qdtech-tab5-p4x`（`0x6645f0`）以及受图形组件版本调整影响的 ESP32-S3 `espressif/esp32-s3-box`（`0x29dd70`）。后两个仅编译，未在对应硬件上刷机。

### 2026-09-24 原生版接续

用户后续照片 `IMG_7503/7504` 与触摸反馈确认：双线性放大全屏虽去掉留边，却让画面模糊、刷新率很低、触控不灵敏；应用页还有文字挤压裁切。该实验版已从实机撤回，并从源码撤销。撤回时先刷回私有备份 `/Volumes/liutupi/tab5-private-backups/tab5-fontfix-rev1-2026-09-24.bin`（SHA-256 `d88e3e2990bfd8cff4ec11fb12a9ce991898e29a792aaff1949ae68f1b41bf1a`），之后再刷入原生版。

新增 `qdtech-tab5-native`：LVGL 直接按 1280 × 720 绘制，端口仅旋转变化区域到物理 720 × 1280；没有旧桌面全帧双线性插值。触摸控制器的物理 720 × 1280 坐标直接传给 LVGL；LVGL 在 `lv_indev_pointer_proc` 中按屏幕 270° 旋转一次，旧 `DesktopUI::HandleTouch*` 不参与。形象用少量矢量控件绘制眼睛、嘴巴和表情，动画只改变小区域；36 px 标题字库、28 px 对话字库来自同一 LXGW WenKai 源字体。原生版不编译旧桌面及其主要应用服务，保留小智语音、Wi‑Fi、SD、摄像头底层。首版尚无旧天气、电台、相册、播客、FC 页面，后续应按用户选择逐一整合；用户说的“ICU 应用”具体含义仍待澄清。

首版 ESP-IDF 6.0.2 原生镜像 `xiaozhi.bin` 大小 `0x36c660`，应用分区剩余 47%；SHA-256 `3877deb997fc23d47f7d06461455ea6a2da87630166d66f7c45c02a192f10f51`。只写入 `ota_0` 应用分区并通过写后哈希校验，原 NVS/资源分区未动。重启日志确认 Tab5 P4 rev 1.3、ST7121、触控注册、SC202CS、Wi‑Fi、SD 挂载、相机 5 秒取 150 帧，最终进入 idle；24 秒观察未见重启。86 项宿主测试通过。用户新照片 `IMG_7505` 证实主屏已铺满，字边缘较旧放大版清楚，但大标题缺“好，我是”；用户实际触摸按钮没有反应。排查 LVGL 源码发现输入会自动随屏幕旋转，首版代码又手动旋转，造成双重旋转；此处已改成传递原始触摸坐标。标题字库也已补齐缺字。**修正版的触摸、语音按钮及动画帧率还需上机复验。**

触摸修正版镜像大小 `0x36d010`，SHA-256 `7e20c235f8e90067a3daf3f8686e23200fdbb765594a32c1d00134e4e8cf6495`，私有回退副本为 `/Volumes/liutupi/tab5-private-backups/tab5-native-touchfix-rev1-2026-09-24.bin`。已再次仅刷入 `ota_0` 并通过写后哈希校验。按钮点击时会立即把顶部状态改为“正在启动”，串口输出 `Tab5Native: Chat button clicked`，便于区分触摸命中与后续语音状态切换。当前仍在等用户实际点击确认。

最终源码通过 86 项宿主测试；ESP-IDF 6.0.2 的 `qdtech-tab5-native`、旧桌面 `qdtech-tab5` 和 P4 rev 3+ 旧桌面 `qdtech-tab5-p4x` 均编译通过。P4 rev 3+ 只验证了编译，未在该硬件上运行。旧桌面仍距应用分区上限很近，原生版有约 47% 余量，后续新功能应优先进入原生版。

独立版本的 release 名称为 `qdtech-tab5-native`，目前向服务器报告的板型仍是 `qdtech-tab5`。正式 OTA 前须先决定两种 UI 的兼容/升级策略，避免把旧桌面和原生版视作可互换更新。

### 2026-09-24 Nabo 桌面

用户提供四张透明 PNG：睡醒/站姿、挥手、五帧眼部表情，以及完整的角色设计板。原图保存在 `assets/nabo/source/`；角色设计板是视觉参考，不能当成代码指令。可复现处理脚本 `scripts/prepare_nabo_assets.py` 生成 `nabo_assets.c/.h`：434 × 618 原尺寸半身立绘、两个 312 × 125 眼部覆盖图、从角色设计板提取的两个 48 × 31 嘴型、300 × 561 挥手姿态。新原生桌面使用深蓝背景、清楚的 28/36 px 中文、真实对话状态和一个大尺寸触摸按钮。静置时每五秒眨眼，监听/说话状态下波形小区域以 20 fps 更新，说话状态按节奏切换闭口/半张/张口；按角色或对话按钮触发短暂挥手。图像常驻固件，不依赖 SD 卡。更多可选动作包的 SD 卡目录与加载原则写在 `assets/nabo/README.md`。

实机顺序：首版 Nabo 镜像仅刷 `ota_0` 后写入哈希通过，第一次进入 idle 后曾出现一次 `Load access fault`，设备自动重启；当时过滤日志没有保留完整回溯，原因未明。后续该版运行及点击未再见同一故障。加嘴型版镜像大小 `0x41ec60`，SHA-256 `dc6b893dd3c60ee821cebf0f964f5db439090de7156cda2f00dd665df4b28284`；刷入校验通过。55 秒串口观察内摄像头成功取帧、SD 挂载，按钮两次命中，设备按 idle → connecting → listening → speaking 等状态转换，没有再出现崩溃。当前设备是修正文案的第三版：`xiaozhi.bin` 大小 `0x41ec90`，应用分区仍剩 37%，SHA-256 `fe8c5398477e0f9c4b931522e95603d45141544bd4f38ab652cf62b8cc53204d`，私有回退副本 `/Volumes/liutupi/tab5-private-backups/tab5-nabo-mouth-rev3-2026-09-24.bin`。仅刷应用分区且写后哈希通过，50 秒观察无崩溃；这一版只改按钮文案及提示，仍待用户照片和动画、触摸体感反馈。

需继续确认：Nabo 在实屏上是否完整显示、眨眼与说话嘴型是否自然和流畅、触摸角色能否触发挥手、声音与对话体验是否正常。用户提到的 ICU 应用没有明确功能范围，未在桌面加入无效入口。

### 2026-09-24 摄像头人体感应

曾试过本地正脸录入与本人识别，用户实测发现 Tab5 当前摆放角度必须弯腰才能录到正脸，因此要求只判断是否有人，不再做精细人脸识别。实验中的视觉帧还暴露了两个取像问题：关闭自动 ISP 管线后，SC202CS 默认增益让室内画面过暗；按旧预览的 PPA 旋转去处理识别帧，把人的脸横过来。现改为传感器横屏方向的 320 × 240 中央裁剪，设置曝光并按画面亮度低频调整增益。触摸“测试感应”可在左侧看到实时取景，无须盲目对镜头。

最终原生版使用 `espressif/pedestrian_detect` 0.3.2 的 P4 本地模型判断人体出现；高置信度一次或普通置信度连续两次检出后触发通用问候。它**不能确认来人身份**。模型不依赖 SD 卡，固件不读取旧实验版的 `face.db`，也不会保存或上传摄像头画面。静止画面的运动网格经四点取样和连续确认后才计为活动；无活动 90 秒切到睡姿，触摸或活动唤醒。问候音改为“你好，我是 Nabo，很高兴见到你”，不再暗示已识别本人。

用户确认问候声音、画面和睡眠正常，但反馈靠近后的问候有延迟。旧镜像的串口记录显示活动唤醒到人体到达约 11 秒；已将活动后 10 秒内的检测间隔缩为 1 秒，普通待机 2 秒、睡眠 5 秒，并在高置信度检出时直接触发问候，普通检出仍需连续两次。问候播放仍受 120 秒冷却限制，避免同一人在镜头边缘反复触发。此前日志还证实无活动 90 秒进入睡姿，活动出现后唤醒。

ESP-IDF 6.0.2 最新构建 `xiaozhi.bin` 大小 `0x775e40`，SHA-256 `0beadda8a9b74f62ec8b5525a145c49c8cfb6fc81a85fe8dd9396fc665374acc`。仅写入现有 `ota_0` 应用分区，写后哈希通过；当前应用镜像私有副本为 `/Volumes/liutupi/tab5-private-backups/tab5-nabo-person-presence-fast-2026-09-24.bin`，上一版镜像和 16 MB 完整回退备份仍保存在仓库外。新镜像串口实测模型初始化后约 1.36 秒检出已在镜头中的人，3 毫秒后音频开始解码；尚未对真人走入镜头的延迟作多次实测。此前调试所用的人像串口帧仅保存在本机临时目录，交接完成后删除。

### 2026-09-24 顶部翻页时钟

按用户要求，原生桌面左上角副标题改为“土皮助手”，移除人物卡左下角的文字牌和可见的“测试感应”按钮；长按人物仍可进入感应取景测试。顶部中央新增带分瓣数字、细分割线和轻微翻折动画的时分秒时钟，只更新变化的数字。用户随后指出不需要钟面外围留白，并提供旧桌面照片作为数字风格参考；当前款取消白色外框，改为深色独立翻页卡：小时白字、分钟暖黄字、秒钟浅蓝字与浅蓝分隔点，**六位全部使用同样尺寸的 72 px 数字及翻页卡**。Montserrat Bold 数字字体单独生成，源字体哈希及 OFL 许可证保存在脚本和板级目录。右上角显示年月日、星期和运行状态；基础显示层发来的旧 `HH:MM` 状态被转为“已就绪”，避免与时钟重复。系统尚未从服务器校时时显示“等待校时”和占位数字。新中文子集补入“土”“皮”。当前镜像大小 `0x777890`，SHA-256 `e9015700bdeb2e44acc9eccaa86879265859f247e34c6a0065f7e45933b89503`，仅刷 `ota_0` 并通过写后哈希；实屏布局和时钟动画仍待用户照片验收。当前款私有镜像位于 `/Volumes/liutupi/tab5-private-backups/tab5-nabo-flip-clock-equal-2026-09-24.bin`，白底试版和初版也保存在同目录。

### 2026-09-24 原生应用页、设置与网络电台

用户要求在 Nabo 主界面之外加入“副屏”：按当前实现理解为同一块 Tab5 屏幕上的第二页，主界面“应用与设置”按钮打开应用首页，再进入设置或电台。设置页显示当前 Wi‑Fi 名称/IP 或配网热点信息，支持重新配网，并通过板级 API 调节、保存背光亮度和扬声器音量。电台页支持选台、播放/暂停/停止、上一台/下一台；选中的频道索引保存至设备设置。收到用户语音识别文字“我要听广播”等指令会打开电台页并请求播放，同时注册原生版 MCP 电台工具。电台播放时视觉服务暂停人体推理，避免影响播放和错误入睡。SD 卡根目录的 `radio.json` 仍可替换内置目录。

旧内置 37 条中有重复流地址及失效地址；重新核对后缩为 10 个不同的 MP3 频道，首选地址在开发机上逐一以 GET 获得 HTTP 200 和 `audio/mpeg`。中国之声、北京新闻/交通及部分广东频道的名称与蜻蜓 FM 页面核对，36 px 中文字体补齐这些频道名。最终镜像大小 `0x78ec50`，应用分区剩余 42%，SHA-256 `a21a5327e999c7e77db6e48f203ef228fd8263614bf614e4c0bf4b808b1621c8`；仅刷 `ota_0`，写后哈希通过，仓库外回退副本为 `/Volumes/liutupi/tab5-private-backups/tab5-nabo-apps-radio-curated-2026-09-24.bin`。86 项宿主测试通过；设备重启后至少 98 秒内持续输出相机与内存状态，未见崩溃或重启。**开发机网络可读不等于 Tab5 扬声器已通过实测**；用户尚需在屏幕上点击频道和播放，并试一次语音“我要听广播”，同时记录是否出声、是否正确跳页。

用户随后实测“打开设置后死机”。复现串口显示 `taskLVGL` 长时间占用 CPU 1，`IDLE1` 看门狗每 10 秒报警；反查符号地址停在 LVGL 的 `lv_inv_area`。原因是两个滑块注册 `LV_EVENT_ALL` 后，对绘制事件也执行 `lv_label_set_text`，在绘制期间持续要求重绘。现于回调入口只允许 `LV_EVENT_VALUE_CHANGED` 和 `LV_EVENT_RELEASED` 进入更新逻辑。修正镜像大小 `0x78ecd0`，SHA-256 `7e213fbb615516d25e2831a2342af4d774f1c3840658e9b3d42bcbbdb9e63bb4`；已仅刷 `ota_0` 并通过写后哈希，仓库外副本 `/Volumes/liutupi/tab5-private-backups/tab5-nabo-settings-slider-fix-2026-09-24.bin`。刷后持续运行观察无看门狗，但仍待用户再次打开设置、调节两个滑块的现场确认。

用户接着要求电台页更充分利用大屏并加入彩虹声波。现在左侧是选台列表、右侧是宽幅播放区，40 根彩虹声波柱在播放时按 MP3 解码得到的音频平均幅度起伏；画面任务每 100 毫秒只重绘一个 692 × 148 的声波区域，不在绘制事件中修改控件属性。停止时声波收敛，当前频道在左侧高亮。当前镜像大小 `0x78fc10`，SHA-256 `6f5b5e339fe5c35ef7bad3ba6cb5e0b2a5770cded2c462cc2dcf4825029fd185`，只刷 `ota_0` 且写后哈希通过，仓库外回退副本 `/Volumes/liutupi/tab5-private-backups/tab5-nabo-radio-rainbow-2026-09-24.bin`；86 项宿主测试通过。实机日志已记录点击不同频道、HTTP 200、持续 MP3 解码和音频输出调用，未见看门狗；仍需用户对实屏视觉、实际扬声器声响和设置滑块反馈。

## 接续工作的顺序

### 2026-09-25 网易云点歌歌词接入

用户已确认通过 NAS/网易云 MCP 点歌有声音，原先电台页没有歌词。实际运行固件来自 `/Users/tupi/tab5-build`，比主工作区新；已把其播放、音频及原生 UI 相关实现同步回主工作区，避免下一次编译退回旧功能。原生 MCP 点歌入口 `self.music.play_url` 支持带完整 LRC；新增 `self.music.set_lyrics` 可在开始播放后补发真实 LRC，保留原单句 `self.music.set_lyric`。工具说明引导小智从 NAS/网易云歌词工具获取真实歌词，不编造歌词；设备侧本身没有 NAS 凭证或歌词检索 API。歌词输入限制为 16 KiB，支持 2/3 位小数时间戳、重复时间标记和 offset；电台播放页显示歌名及前句/当前句/后句，首句到达即刷新，播放、缓冲及暂停之间保持时间轴一致。若 NAS 未返回歌词，页面会显示“等待歌词”，不会假装歌词已接入。

宿主 88 项测试通过，含新 LRC 解析检查；ESP-IDF 6.0.2 Tab5 原生版编译通过，镜像大小 `0x7a08c0`，应用分区余量 41%。已仅写入现有 `ota_0` 程序分区且写后哈希通过，新镜像 SHA-256 `0676208a6791e01652995d276e449c6cb05c4d92b5fa446e4128af1b320e8dd1`。刷后串口确认 Wi-Fi、小智 MQTT 和摄像头恢复，观察窗口未见 panic 或看门狗。刷前完整程序分区私有备份为 `/Volumes/liutupi/tab5-private-backups/tab5-before-lyrics-2026-09-25.bin`，不得上传。仍需用户再点一首已知有歌词的歌曲，确认 NAS 歌词 MCP 能返回 LRC，串口出现 `music LRC lines=`，并检查屏幕逐句显示；本次尚不能把“有声音”当成歌词链路验收。

用户随后给出 `IMG_7513.HEIC`，确认《道别是一件难事》在屏幕只剩“一 - 内”，歌词区域只剩部分占位字。串口记录表明 `self.music.play_url` 收到了完整的歌名与歌手且 MP3 正常播放，没有 `music LRC lines=`；因此是两个独立问题：原生播放页强制使用只有约 500 个扩展 glyph 的 Noto `basic` 30 px 字体，而设备资源分区已实际包含约 6845 字的 `font_noto_sans_common_30_4.bin`；点歌调用没有附 LRC。已让歌名、歌词及 Nabo 音乐文案采用已加载的主题字体，避免额外占用 flash。歌词缺失时，Tab5 先停止上一音源，在低优先级 PSRAM 任务中通过网易云网页接口按**准确歌名与歌手**查找歌曲 ID 和真实 LRC，完成或失败后再启动音频；NAS 若直接送来合格的带时间戳 LRC 则立即播放。查询限时、响应大小及 LRC 大小均受约束，严格匹配不到时不显示其他歌曲的歌词。该网页接口可用性可能随网易云服务变化，失败时仍放歌；小智仍可调用 `self.music.set_lyrics` 补发 NAS 歌词。

本修正版宿主测试通过，P4 原生固件编译通过，镜像 `0x7a19b0`，应用分区余量 41%；仅更新 `ota_0` 且刷写哈希通过，SHA-256 `ec9d9d032368da7e07bab79069c4476da918b9a57fa5ff13e6dca49e9a8e4273`。刷前当前版本的应用镜像已读回并验证哈希，私有回退文件 `/Volumes/liutupi/tab5-private-backups/tab5-before-font-lyrics-fix-2026-09-25.bin`。仍待用户重放同一首歌，核对完整歌名、逐句歌词和播放延迟；注意不得把音频 URL 或歌词正文写入公开日志。

歌词三行原高度各 33 px，小于 Noto 30 px 字体实际 43 px 行高；最终又把歌词面板扩到 132 px，并将控制按钮下移到留有底边的位置。这个最终布局版 SHA-256 `d0fe113de5c6315cfcad4976c7171aad2ef9d40202350b75c3f61d69b0481761`，镜像同为 `0x7a19b0`，已再次仅刷 `ota_0` 且写后哈希通过。上一版查歌词镜像的私有回退副本为 `/Volumes/liutupi/tab5-private-backups/tab5-lyrics-lookup-before-layout-2026-09-25.bin`。

### 2026-09-24 ICU 数值工具（本次）

原生应用页新增 ICU 数值工具：eGFR、uACR、P/F 与正式 OI、血气酸碱/AG/Winter、13 种药物静脉泵单位换算。左侧选模块，触屏数字键盘录入，右侧显示结果；Nabo 新增 `self.icu.open` 及五项计算工具。配方输入的体积定义为**配好后的最终总液量**，默认显示 50 mL，药量按 mg 或 U 明示，带 `/kg` 的单位要求体重。没有给药目标或治疗建议。公式、来源和边界详见 `main/boards/qdtech/tab5/ICU_CALCULATORS.md`。页面数据仅保留于内存，返回应用时清空。

宿主 87 项测试通过，含独立 C++ 算式/错误输入检查；ESP-IDF 6.0.2 原生版成功编译，应用镜像 `0x79c5e0`，应用分区余量 41%。已仅写入 `ota_0`，写后哈希通过；最终镜像 SHA-256 `a23f961c6f21ba3c68964811fe4775b48a36cfc9ec91b61d7f3c9c2e7f90f6f6`，仓库外回退副本 `/Volumes/liutupi/tab5-private-backups/tab5-icu-calculators-2026-09-24.bin`。刷后串口 18 秒内未见看门狗或 panic；**ICU 页面首次打开、数字键盘、结果布局、语音工具调用仍需用户实机触摸/照片验收**。已请用户用虚构的去甲肾上腺素 4 mg / 50 mL / 3 mL/h / 80 kg 示例核对 0.05 μg/kg/min。

上一个版本的原生应用页设置滑块及网络电台彩虹声波仍待用户补充实机反馈。临床投入使用前需以科室实际制剂及工作流复核所有单位和结果，特别是胺碘酮稀释液、血管加压素/胰岛素 U、同一次尿样的化验单位。用户对“50 mL 配药”是否指最终总液量的答复尚待收到；如实际是 50 mL 盐水加药液，需要改默认输入习惯或同时显示两种方式。

1. 复现并修好文字、数字、时钟缺失。优先验证显示局部刷新和字体，再重做 1280 × 720 Tab5 横屏布局。需要实机照片与串口日志一起判断，不能只看模拟器或编译输出。
2. 在新布局下核对所有可见页面的字体大小、对齐、触摸命中和每秒刷帧性能。
3. 实测音频电台/播客与 FC，然后排查 JPEG 解码；记录可复现的媒体文件类型与日志。
4. 再补齐原项目尚未移植的服务，逐个与 Tab5 硬件能力、flash/PSRAM 余量核对。不要将桌面上出现一个入口等同于功能已完成。

### 2026-09-27 主页动画与每日内容

按用户要求，原生主页新增三帧短挥手、间隔变化的眨眼、人物周围的低负担小光点，以及睡姿上浮的 `z`；说话嘴型、翻页时钟、摄像头睡醒逻辑保留。大幅角色图只在短暂挥手时切换，日常小动作只更新局部。原问候语所在区域改为深色信息卡，每 8 秒轮播每日一句、历史上的今天和节日提醒；“应用与设置”移入下方对话卡顶部。旧桌面的节日、历史和短句表抽到 `tab5_daily_content.cc` 共用，按当天日期选择；历史记录并未覆盖全年，未收录日明确提示。2026-09-27 的 NASA Dawn 发射记录与 NASA 官方资料核对，新增文案已检查 28 px 子集字库覆盖。

宿主 89 项测试通过，包含日期内容与跨年节日倒计时；ESP-IDF 6.0.2 的 `qdtech-tab5-native` 编译通过。预览三帧源图时发现最后一帧手掌被裁切，已扩展裁切并在切换时对齐人物头部。最终应用镜像 `0x80d3a0`，分区剩余 `0x4f2c60`（约 38%）。仅刷入 `ota_0`，写后哈希通过，镜像 SHA-256 为 `e6f2f418a2538ac2cf26a47c06e150625e896f3b91a1c14ee5483fd418967bea`。刷前应用区读回文件在仓库外 `/Volumes/liutupi/tab5-private-backups/tab5-pre-home-animation-2026-09-27.bin`，不可上传。最终版刷后串口观察 35 秒：相机持续活动，未见 panic、brownout 或看门狗；实屏布局、三帧挥手与触摸手感仍待用户照片或视频验收。当前空间足够，不从 SD 卡逐帧读取动画。

## 发布约束

本仓库只包含源码和可复现配置。不要提交 `build/`、`sdkconfig`、NVS/原机备份、串口日志中的网络信息、SD 私人媒体、账号令牌。根目录是 XiaoZhi MIT 许可，`components/nofrendo` 源文件声明 GNU Library General Public License v2；发布二进制前应核实所有依赖及资源素材的再分发条件。

## v1.0.0（2026-09-30）与在线升级 OTA

**分区**：`nvs 0x9000`、`otadata 0xd000`、`phy_init 0xf000`、`factory 0x20000 (12 MB，主固件)`、`ota_0 0xc20000 (1 MB，updater)`、`assets 0xd20000`。主固件当前约 10 MB，放不下两份，所以升级包先落到 SD 卡。

**流程**：

1. 设置页 → 固件升级 → 检查更新：读取 `https://api.github.com/repos/Liutupi/qdtech-tab5-xiaozhi-firmware/releases/latest`，要求存在 `qdtech-tab5-<tag>-app.bin` 与 `SHA256SUMS.txt`，版本高于当前 `PROJECT_VER`。
2. 立即升级：把安装包下载到 `/sdcard/ota/tab5-update.part`，边下边算 SHA-256，与 `SHA256SUMS.txt` 对比；再核对镜像头（ESP32-P4、项目名 `xiaozhi`、版本一致），最后写入 `tab5-update.bin` 和 `tab5-update.txt`（`sha256=`、`size=`、`version=`）。
3. 主固件调用 `esp_ota_set_boot_partition(ota_0)` 并重启；`updater/` 挂载 SD 卡，重新校验、擦写 `factory`、读回校验，成功后 `esp_ota_set_boot_partition(factory)`（会擦除 otadata），删除 SD 卡上的升级文件并重启。屏幕在这一步会黑屏约一分钟。
4. 中途断电是安全的：otadata 仍指向升级程序，SD 卡上的文件保留，下次上电自动重试。升级不会碰 NVS 和 assets。

**发布新版本**：修改根目录 `CMakeLists.txt` 的 `PROJECT_VER`，`idf.py build` 后把 `build/xiaozhi.bin` 复制为 `qdtech-tab5-vX.Y.Z-app.bin`，生成 `SHA256SUMS.txt`（`shasum -a 256`），连同 tag `vX.Y.Z` 一起发布到 GitHub Release。`ota_0` 里的升级程序只能通过 USB 更新（`updater/`，`idf.py -C updater build`，写入 `0xc20000`）。

**限制**：升级需要 SD 卡、Wi‑Fi，且设备处于空闲状态（不在对话中）。云端/MCP 下发的固件升级在 Tab5 上被拒绝。国内网络访问 GitHub 可能较慢，失败时可稍后重试，或改用 USB 刷写。

## v1.0.4 / v1.0.5 现状（2026-10-01，Claude 交接）

**两版都已在实机上测试、刷入，但尚未发布**：GitHub 上最新版仍是 v1.0.3。工作区里 v1.0.4、v1.0.5 的改动还没有 git commit；发布时照 `tab5-publish-v1.0.2.command` 复制一份，改成新版本号即可（它会 `git add -A`、打 tag、推送并建 Release）。`*.command` 和 `*.patch` 已在 `.gitignore` 里，不会被提交。

- **构建与刷机**：Mac 上的 `tab5-beta-v1.0.1.command`，其中 `VER` 必须与 `CMakeLists.txt` 里的 `PROJECT_VER` 一致（现在是 1.0.5）。它会编译、USB 刷写，再抓 100 秒串口日志检查是否崩溃（现在也会检查 `assert failed` 和反复重启）。只抓串口日志、不重启用 `tab5-serial-capture.command`（900 秒）。刷写前必须关掉占着串口的抓日志窗口。
- **Muse 推送（v1.0.4）**
  - 数据流：Muse → Cloudflare 快速隧道 → NAS 容器 `muse-relay`（项目在 NAS 共享文件夹 `docker/新建文件夹`，代码 `tools/muse-relay/`）→ Tab5 每 2 分钟通过隧道拉取 `/inbox/<token>`。NAS 与 Tab5 不在同一局域网，只能走隧道。
  - token 只存在 NAS 的 `/data/token.txt` 和 Mac 本地的 `tab5-muse-seturl.command` 里，**不要写进仓库**。
  - 隧道地址在容器重启后会变：relay 把新主机名发布到 ntfy.sh（topic 由 token 的哈希得出），Tab5 连续两次拉取失败后会自动重新查找。Muse 端则需要手动换成新的 MCP 地址（Tab5 的「Muse 推送」页底部会显示）。
  - 相关代码：`tab5_muse_inbox.*`，MCP 工具 `self.muse.inbox` / `self.muse.set_url`。
- **唤醒词（v1.0.5）**：换成 MultiNet `mn7_cn` 自定义唤醒词「ni hao na bo;ni hao xiao zhi」（你好Nabo / 你好小智），多个唤醒词用 `;` 分隔（见 `config.json` 与 Mac 的 `sdkconfig`）。模型打包在 flash 的 assets 分区里。
- **大字库移到 SD 卡（v1.0.5）**
  - `font_noto_sans_common_30_4.bin`（2.5 MB）不再打包进 assets（`DEFAULT_ASSETS_SKIP_TEXT_FONT`）。`tab5_sd_assets.*` 首次联网时把它下载到 `/sdcard/tab5/`（经 SHA-256 校验，来源是 GitHub 预发布 `sd-assets-v1` 及 ghfast/gh-proxy 镜像），之后每次开机读入 PSRAM。
  - 通过 OTA 升级、assets 分区仍带字库的旧设备，会继续使用 flash 里的字库。
  - 注意：在线升级不会改写 assets 分区，所以新唤醒词只有 USB 刷写后才生效。
- **踩过的坑**：栈放在 PSRAM 上的任务里，**不能**触碰 flash（NVS 写入、`Assets::GetInstance()` 的 mmap、OTA API），否则会触发 `esp_task_stack_is_sane_cache_disabled` 断言并反复重启。凡是涉及 flash 的操作，都要放到内部 RAM 栈的任务里做，或者通过 `Application::Schedule` 交给主任务。
- **内存**：v1.0.5 开机 45 秒后，内部 RAM 剩约 120 KB（最低 87 KB），PSRAM 约剩 4.2 MB；字库加载后 PSRAM 再少 2.6 MB。以后加 PSRAM 大户（大缓冲、模型）之前，先看 `MemDiag` 日志。
- **NAS 上的改动不在仓库里**：直接改在容器 `xiaozhi-netease-nabo` 的 `/app/xiaozhi-ws-mcp.js`（去掉了"已在播放"的拦截、把 Tab5 地址改成 192.168.88.101、加了 `continuous`），原文件的备份就在同一目录下的 `.bak-*`。如果容器按镜像重建，这些改动需要重做。

### 每日推荐连播提速（2026-10-01）
- 慢的根源：NAS 的 `play_url_arguments` 带整首 `lyrics_json`，大模型要把几千字歌词原样写进 `self.music.play_url` 调用，每首多等 20–30 秒；第二首起模型有时漏掉 `continuous`，放完就停。
- 固件（v106_c.patch）：`self.music.play_url` 不再收歌词，先出声、后台再按 song_id 查歌词；下一首请求若这一轮对话没给出歌曲，会自动重发（最多 3 次、150 秒内）；自己发起的请求所换来的歌一律按连播处理；请求期间模型调用 stop 不会结束连播。
- NAS：容器 `xiaozhi-netease-nabo` 的 `/app/xiaozhi-ws-mcp.js` 第 425/431 行改为只传 `song_id`、不传歌词（备份 `.bak-pre-nolyrics`），已重启容器。
