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

### 每日推荐无声排查（2026-10-03，尚未完成实机验收）

- 用户复现时 Tab5 串口显示 `music.netease_private_fm` 已被调用，小智说“正在给你播放每日推荐”，但没有 `self.music.play_url` 或 `Tab5Udp: play_url`，随后 MQTT 会话 EOF。网易云私享 FM 返回有效歌曲和直链，NAS 日志显示向设备发过播放请求。
- NAS 容器的本地 UDP 播放包曾含 5 KB 左右的 `lyrics_json`，整包约 5.9 KB。已在容器 `/app/xiaozhi-ws-mcp.js` 删除两个播放转发中的歌词字段，改传 `song_id`，并在云端设备调用中加 `continuous`；修改前备份为 `/app/xiaozhi-ws-mcp.js.bak-20261003-playlink`。容器重启后本地包缩至约 390–405 B，语法检查通过。
- **更关键的断点是容器到局域网不通**：容器为 Docker `bridge` 网络，IP `172.17.0.4`，对 Tab5 `192.168.88.101`、NAS 宿主 `192.168.88.100`、Mac `192.168.88.149` 的 ping 均 100% 丢包。容器发出约 372 B 的真实歌曲 UDP 包，Tab5 无接收日志；同网段 Mac 发出的 103 B 包则被 Tab5 立即接收。不能把容器的 `udp.send` 成功当成设备已播放。
- NAS 的反向 JSON-RPC `tools/call self.music.play_url` 也不通：将 fire-and-forget 暂改为带 5 秒超时的 `sendRequest` 后，容器重启自动续播时明确记录 `device play_url failed tools/call timed out`。因此目前两条自动推送链都没有到达设备，用户语音播放仍未验收。
- 下一步需在不无意扩大 NAS 暴露面的前提下，为此音乐容器建立到 Tab5 的单向 LAN 发送通路（例如审查 UGREEN Docker 网络配置），或改成 Tab5 经可达的受控端点主动拉取播放指令；之后再次实测语音“播放每日推荐”并确认 Tab5 的 `Tab5Udp`、HTTP/MP3 解码与喇叭出声。不要再次仅凭 NAS 的 `sent` 日志判定成功。

### v1.0.7 测试版：音乐卡顿（2026-10-01，未发布）
- 串口证据：卡顿那首每 64 帧（1.67 s 音频）要 5–9 s，下载速度只有 3–5 KB/s。原因：对话通道关闭时把 Wi‑Fi 切回 MAX_MODEM 省电；且 HTTP 读取与 MP3 解码同步，一次慢读就让喇叭断音。
- 修复（v107_a/b/c.patch）：板级 `SetPowerSaveLevel` 在外部音频播放时把 LOW_POWER 改为 BALANCED；`radio_service.cc` 新增后台读取任务 `radio_reader`，把流写入 PSRAM 环形缓冲（音乐 384 KB，电台 128 KB），解码任务只从缓冲取数；下载完成即停止读取（有的 CDN 发完数据不关连接）。
- 实测 1.0.7a：连播 5 首无一处变慢，切歌间隔 11–15 s。

### 2026-10-02 Tab5 原生版稳定性与桌面整理

本轮以正在运行的 `/Users/tupi/tab5-build` v1.0.7 源码为准。主工作区原先落后于该版本，旧工作树已保存为 Git stash `pre-sync Tab5 development snapshot 2026-10-02`，随后快进到同一提交并同步当前源码。不要把旧 stash 直接覆盖到新板级实现。P4 rev1.3 实机继续使用 ESP-IDF 6.0.2；先前 6.1 的 ISP 路径在这块芯片上会崩溃。

- 摄像头任务的内部栈由 24 KB 降到 12 KB、优先级由 4 降到 3；对话/音乐期间暂停后台取景，触摸测试能及时处理，临时帧分配失败会重试。实测稳定时内部空闲由约 111 KB 增至约 120 KB；新任务栈最小余量约 7.8 KB。任务栈仍必须在内部 RAM，不能迁到 PSRAM。
- 本地 WebSocket 只接受 `self.daily.set_cards`，限制消息长度和 JSON 深度，并清理断连资源；每日卡片必须至少有一张完整的标题/正文。桌面收到稀疏或少于三张卡时会压紧有效卡、清除旧槽与 NVS 残留；日期跨天清空上一日摘要；轻触每日卡可翻到下一张。大对话卡的标题随临床干货/今日医学内容变化。
- WebSocket 音频帧校验版本、长度和类型；电台网络读取等待期间可及时响应停止、上一台和下一台。播放期间仍维持 Wi-Fi BALANCED，避免此前 PERFORMANCE 档与功放并行出现的欠压。
- 减少主页频繁重设相同文字和样式；右上状态点指示倾听、说话及联网状态。大幅姿态动作的刷新周期与 LVGL 显示周期一致。感应测试取景复用一个 RGB565 缓冲，退出时释放。歌词元数据更新不再反复打开电台页或重启播放。
- `qdtech-tab5-native` 配置启用 LVGL 内置 LZ4 及 `LV_BIN_DECODER_RAM_LOAD`。思考、好奇、安慰等大躯干图层改为 RGB565A8/LZ4；眨眼和鼓励动作原本就采用同一路径。这个格式减少解码时的 PSRAM 峰值，但占用更多 flash。`scripts/tests/test_nabo_image_codec.py` 用 LVGL 的 C 解码器逐块验证实际素材。
- LZ4 配置使启动 PSRAM 比先前少约 33 KB；新增好奇和安慰图层又使映射进 PSRAM 的素材增加约 52 KB。人体模型原 1920 KiB 的空闲门槛因此被卡住，最终门槛按实际素材调整为 1792 KiB，连续块门槛仍为 1280 KiB。最终版实机模型加载前有 1,889,560 B、加载后 1,286,240 B；47 秒时 PSRAM 空闲约 1.29 MB。原生固件镜像约 11.82 MB，12 MB `factory` 分区剩约 758 KB；`ota_0` 的 1 MB 分区仅装升级器，构建时对它的尺寸警告是预期的。

宿主测试 97 项通过，新增两张素材也经 LVGL 的 C 解码器逐块比对；完整原生版编译通过。2026-10-02 最终候选镜像 SHA-256 为 `9160d0bfca62d723630a1eae7aa361e0b94b4f1c735c324728661f2477271ebd`，只刷 `factory` 的 `0x20000`，NVS、assets 和 updater 未动；写后哈希通过。原厂程序分区和最终候选镜像的本机备份分别为 `/Volumes/liutupi/tab5-private-backups/tab5-factory-before-optimization-2026-10-02.bin` 与 `tab5-optimized-factory-2026-10-02.bin`，不可上传。最终版串口连续观察 135 秒：Wi-Fi 和 MQTT 已连接，人体检测持续运行，无 `NaboPose` 解码失败、panic、看门狗或 brownout；无活动 90 秒后进入睡姿。真人问候、触摸手感与实屏视觉仍需现场确认。

待办：`RadioService::PlayUrlFromTool()` 仍可能与后台 reader 并发改写静态频道目录，切台逻辑应统一序列化；本地每日卡 WebSocket 仅限局域网且没有鉴权，不能转发到公网。需要实屏确认好奇/思考/安慰动作、设置与电台触控、歌词与真实 NAS 点歌，以及真人进入镜头后的问候延迟。

### 2026-10-02 电台切歌与流缓冲清理补丁

上一节的镜像是可回退的稳定基线，并非本轮最后刷入版本。该版本在播放本机静音 MP3 期间再次下发 `play_url` 时也能复现 `tlsf_free: block already marked as free`。ELF 栈定位到 `RadioService::stop_reader()`：ESP-IDF 6.0.2 的 `vStreamBufferDeleteWithCaps()` 误用信号量删除流缓冲控制块，随后再次释放同一块内存。板级代码现在先取得该流缓冲的静态存储区与控制块，调用标准 `vStreamBufferDelete()`，再分别释放两块由 `xStreamBufferCreateWithCaps()` 分配的内存；没有改 SDK 或生成文件。

电台目录预留一个不在列表中显示的歌曲槽；歌曲 URL、选台及播放命令先进入受锁保护的待处理区，由电台任务统一改写播放状态和频道。对外查询改用状态快照；播放中收到新的 URL 会先终止旧流再建立新流。连续三次本机静音 MP3 实测：第一首首帧 245 ms，播放中切至第二首首帧 133 ms，第二首自然播放完释放资源，随后第三首首帧 111 ms 并自然播放完释放资源；整个约 125 秒串口观察未见重启、断言、panic 或看门狗。此测试只证明本机流切歌和清理路径，不代表真实网易云/NAS 链路延迟或可听音质。

三次切歌验证版 `factory` 镜像 11,827,312 B，SHA-256 `071470de5a54dd9e27c41d0953a5e1d67e4796b78ffb2a8d9eca982f12cea5a3`，仓库外备份 `/Volumes/liutupi/tab5-private-backups/tab5-radio-race-doublefree-fixed-2026-10-02.bin`，保留作回退。后续代码审查发现歌曲 URL 尚待播放任务处理时立即按“下一首/上一首”，会误启动电台；现在先取消待播放歌曲，再停止播放。设置页亮度和音量滑杆由 26 px 增至 48 px，中心位置不变，方便在 5 英寸屏上触摸。

当前刷入版 `factory` 镜像 11,827,440 B，SHA-256 `39730b92e1c2273e9e3e4339dd6de3975d0ae2b44dbfe30f9e7e8473e8bcd289`；仓库外备份 `/Volumes/liutupi/tab5-private-backups/tab5-current-factory-2026-10-02.bin`。只刷 `factory` 的 `0x20000`，写后哈希通过，NVS、assets 和 updater 未动。宿主测试 97/97 通过，P4 原生版完整编译通过，`git diff --check` 通过。当前版刷后 89 秒观察：人体检测就绪，连续两次本机静音 MP3 首帧 237 ms、62 ms，第二首自然播完并释放资源，未见重启、断言、panic、看门狗或 brownout。下一轮需要真人实屏核对新布局与触摸、真实 NAS 连播/歌词、弱网超时与快速连续点按。当前 reader 停止在网络库不返回时最多等待约 15 秒；命令队列极端饱和下的切台行为也尚未覆盖实测。

### 2026-10-02 弱网切歌、原生电台布局与 NAS 小包点歌

- `radio_service.cc` 的后台 HTTP reader 停止改为短时等待加最多 4 个延迟回收槽，网络库短暂卡住时不让电台任务无限等；读客户端及定时器清理由同一互斥锁保护。用户控制有递增序号、待处理区和有界队列兜底，过期的播放/切台命令不能推翻较新的停止命令。新歌曲接管旧流时保留外部音频焦点，避免麦克风、Wi-Fi 省电和背光在两首之间来回切换。锁顺序及过期焦点事件已独立复核。
- 采用相同的 28 秒本地静音 MP3 作弱网对照：首流发送 24 KiB 后停 35 秒，4 秒后换正常流。旧版第二首请求后依次出现 `LOW_POWER`、背光恢复、`BALANCED`、背光再变暗；新版第二首请求到首帧约 416 ms，期间没有 Wi-Fi 档位或背光来回切换。112 秒内后两首自然结束并释放资源，未见断言、panic、看门狗或重启。这个测试不代表真实 NAS 音质或可听停顿已消除。
- 电台页调整为 90 px 双行歌名、下方状态、80 px 彩虹声波、歌词卡和控制区；长歌名由 LVGL 按字符边界省略，主页和电台页不再用 96 字节栈数组截断中文。UDP 点歌只保留一条最新待处理请求和一个主任务回调，标题、歌手、URL 有字节上限，非法 URL 不入队。新 `song_id` 字段只接受 1–18 位数字字符串；收到后先播放，再用现有网易云歌词服务按 ID 查询，旧 `lyrics_json` 字段保持兼容。
- 实机 UDP 探针：1284 B JSON 到达，1534 B、1684 B、2584 B 和 5584 B 均未到达；整首歌词随一个 UDP 报文推送会在这条 P4/C6 链路上丢失。12 条小包瞬时连发只收到前 6 条，播放器正确合并已收到的请求，但 UDP 本身不能保证最新包送达。NAS 实际容器 `xiaozhi-netease-nabo:/app/xiaozhi-ws-mcp.js` 仍需改为只推 `type/title/artist/url/song_id`，整个 UTF-8 JSON 不超过 1200 B、不要附整首 `lyrics_json`。当前接收端没有请求去重，不要直接重复发送同一播放包；更可靠的控制应走已有 MCP 会话。Mac 上的 `Desktop/netease-mcp.js` 是旧副本，不能当作 NAS 运行态修改。
- 最终原生固件使用 ESP-IDF 6.0.2 完整编译，宿主测试 97/97 通过，`git diff --check` 通过。镜像 11,830,864 B，SHA-256 `3225b779edddf5a62ad4fe42c2d9e3e996738d78012092aed94d60ef40bf57a4`；仓库外备份 `/Volumes/liutupi/tab5-private-backups/tab5-songid-udp-final-2026-10-02.bin`。只写 `factory` 的 `0x20000`，写后哈希通过，NVS、assets 和 updater 未动。刷后 72 秒实测：159 B 的 `song_id` 小包被接收，静音 MP3 首帧 258 ms；合成的不存在 ID 触发按 ID 查词，随后报告没有歌词；歌曲自然结束并释放资源，未见崩溃。真实网易云歌曲歌词、NAS 容器改动、触摸手感和实屏排版仍需继续验收。

### 2026-10-02 原生页面内存与歌词卡回归

- 红白机备用 LVGL 画面的三张 960×720 RGB565 缓冲改为仅首次实际走备用显示路径时分配，共 4,147,200 B；开机不再分配未使用的 256×240 源缓冲。游戏页隐藏后，模拟器任务在分配及缩放前读取原子可见性标志，持 LVGL 锁发布时再复核。常规直通屏幕路径不需要这些缓冲；备用路径分配失败会停止游戏并显示错误。此路径已独立代码复核，但尚未实际启动 NES 验收。
- 电台歌词卡按 LVGL 实测文字高度调整标签位置：短句保持垂直居中，长句至多显示两行，超出才省略。UDP `play_url` 在配网热点期间拒收，仅已连上家庭 Wi-Fi 时处理。
- `qdtech-tab5` 旧桌面变体与 `qdtech-tab5-p4x` 已编译通过；`qdtech-tab5-native` 使用 ESP-IDF 6.0.2 完整编译通过，宿主测试 97/97 通过，`git diff --check` 通过。原生镜像 11,831,808 B，SHA-256 `978d374b971a9c88b64f84d1a162d908156042138d4d7a28df1755b981045047`，私有备份 `/Volumes/liutupi/tab5-private-backups/tab5-lazy-nes-lyric-final-2026-10-02.bin`。仅刷 `factory` 的 `0x20000` 且写后哈希通过；NVS、assets、updater 未动。`ota_0` 太小的构建提示是现有升级器分区设计的预期结果。
- 实机启动时人物检测加载模型前 PSRAM 空闲 6,238,340 B、加载后 5,627,372 B；上一镜像分别为 1,888,092 B、1,281,560 B，即节省约 4.35 MB。启动 47 秒时 PSRAM 仍有 5,632,608 B。Wi-Fi、MQTT、摄像头与人物检测就绪。
- 本机静音 MP3 回归：156 B 的 `song_id` 点歌包正常接收，首帧约 256 ms；随后 323 B 的双行测试歌词包解析为 2 条定时 LRC，切歌首帧约 91 ms，中间没有 Wi-Fi 省电档位抖动；歌曲自然结束后释放播放资源，摄像头人物检测恢复。串口未见 panic、assert、看门狗或 brownout。测试只确认解析与生命周期，不等于已目视确认两行歌词或真实 NAS 点歌。测试日志在 `/tmp/tab5_lazy_nes_lyric_smoke_20261002.log`，不要公开其中网络信息。

仍需：拿到 NAS 上正在运行的 `xiaozhi-netease-nabo:/app/xiaozhi-ws-mcp.js` 入口，把 UDP 推送压到 1200 B 内并回归一首真实歌曲的歌名/歌词；目视检查电台两行歌词、设置触摸与主页布局；实际打开 NES 检查备用路径。核心 `Application` 的后台开音频通道与断网/重置、WebSocket/MQTT 指针生命周期仍有并发竞态；下一轮应按代际号和单任务连接生命周期修复，并分别跑 WebSocket 与 MQTT 断线/慢握手交错测试，不能把本轮电台稳定性外推到所有协议连接。

连接生命周期的只读审查细节：`Application::ResetProtocol()` 可在三处后台 `OpenAudioChannel()` 仍执行时销毁 `Protocol` 及其事件组；连接成功后后台还直接改设备状态。WebSocket 的 `websocket_` 被 `Connect`/`Send` 使用时可被 `CloseAudioChannel` 重置；MQTT 定时重连可重置另一个任务正在使用的 `mqtt_`，其客户端析构与重连 timer 回调顺序也有问题。MQTT `SendAudio()` 锁外用共享的 `aes_nonce_.size()` 定缓冲大小，与本地 nonce 快照不一致；旧 hello 在同一 MQTT 连接上也可能被下一次握手误认。此审查**没有修改核心协议**，当前刷入的是上述稳定候选。安全修复需使后台工作者持有旧协议对象，协议对象有代际号，每次音频通道 Open/Close 另有代际号（防止已排队的旧断线回调把新会话置空闲），连接/重连/销毁串行化，主任务只做短时非阻塞访问；WS 和 MQTT 都要实测慢握手、断线及重置交错。现有 MQTT 服务端 hello 不回显请求 ID，严格区分同一连接的旧回应还需要协议侧约束。

### 2026-10-02 连接生命周期、组件源码覆盖与状态栏收口

- 上节列出的核心风险已在 `Application`、`Protocol`、WebSocket 和 MQTT 层修复：连接使用共享所有权与代际号；开、关、重置在后台工作者串行执行；取消握手、过期回调与被动断线不再把新会话覆盖；MQTT 定时重连及 nonce 使用本次会话快照。后台命令及 MCP 队列保持有界。仍需注意：现有 `TryWithProtocol` 的少数同步发送路径可能被底层网络发送拖慢主任务；没有后台工作者的非 Tab5 板仍走同步回退；MQTT hello 没有请求 ID，服务端若跨新连接重放旧回应，客户端无法严格识别。
- `78/esp-wifi-connect` 的保存 SSID 优先级与重复配网置顶修复，以及 `78/esp-ml307` 的 TCP/TLS 接收任务退出、WebSocket 中断阻塞发送、HTTP 被动断线及背压唤醒修复，已放入 `vendor_overrides/` 并由 `main/idf_component.yml` 的 `override_path` 引用，不再依赖 `managed_components/` 的本机修改。新增真实 socket 主机测试覆盖被动断线、阻塞发送与关闭、TLS 并发发送、HTTP 空响应和 keep-alive 复用。连接对象必须在接收回调返回后由另一任务释放，物理 `Disconnect()` 必须由单一工作者串行调用。
- 主页右上较长的连接状态改成单行省略，避免折行挤到日期。状态栏坐标与日期边界经 LVGL 9.5 API 和几何检查；仍待用户实屏照片确认视觉效果与触摸手感。
- 宿主测试执行 `99` 项（`98` 项通过、`1` 项因本机没有 LVGL 解码器跳过）；`git diff --check` 通过。ESP-IDF 6.0.2 干净构建时须包含仓库的 `sdkconfig.defaults*`：遗漏它们会错误回退到 2 MB flash/单分区及关闭 PSRAM，不能当作有效 Tab5 镜像。带完整默认配置的 `qdtech-tab5-native`、`qdtech-tab5-p4x`、`qdtech-tab5` 及代表性 ESP32-S3 `m5stack/atom-echos3r` 均编译通过；S3 未做实机测试。
- 已刷入原生 `factory` 镜像 `11,860,976 B`，SHA-256 `86816dbcdca3683763d6bad9634f2bfef7f82432ac30284450a07a4bb7cf4dab`，仓库外备份 `/Volumes/liutupi/tab5-private-backups/tab5-core-conn-layout-2026-10-02.bin`。仅写 `0x20000`，写入哈希验证通过，NVS、assets、升级器未动；上一稳定版 `/Volumes/liutupi/tab5-private-backups/tab5-lazy-nes-lyric-final-2026-10-02.bin` 保留回退。刷后 90 秒确认 Wi-Fi、MQTT、摄像头、人体检测正常启动，未见 panic、断言、看门狗或 brownout。
- 本机 243 B UDP 点歌包触发 2 行定时歌词解析；静音 MP3 首音频帧约 241 ms，播放完资源释放，约 72 秒内无崩溃或看门狗。测试源是本机 HTTP 服务，不能代替真实 NAS/网易云链路或目视歌词显示；用户实屏与设置触摸反馈仍待取得。

### 2026-10-02 出站发送、自动动作与设置快捷入口

- Tab5 的音频和控制消息现经单个有界出站工作任务发送，音频通常最多排队 16 包/16 KiB，控制消息最多 8 条/16 KiB，MCP 回复最多占 4 个控制槽。监听停止、打断和协议换代会清除过期音频，唤醒词预录音仍受原有队列上限约束；非 Tab5 板保留同步回退。`78/esp-ml307` 源码覆盖为 TCP 发送增加非阻塞分块、可取消等待和整个消息的发送期限，WebSocket 连续帧状态在发送锁内更新并核对实际发送字节。MQTT 的连接状态改为代际号快照，避免跨任务读取底层客户端对象。以上均有对应主机回归测试；真实弱网与 MQTT/WebSocket 语音交互仍要实测。
- `EspVideo::Explain()` 现在接管最近一次照片，JPEG 编码线程结束即释放旋转帧；失败路径、重复拍照和并发拍照均有清理。P4 PPA 旋转输出的声明容量改为真实 RGB565 分配量。这样在调用拍照识图后不应再长期保留约 1.84 MB 旋转帧；只做了源码测试与固件编译，尚未实机执行拍照识图。
- 主页右上通知在状态胶囊内单行显示，避免遮住每日内容卡。六位深色翻页钟的尺寸和时分秒配色保留。自动待机的好奇/思考/眨眼动作把整张头身图的大幅连续移动改为少量关键姿态，手部与小光点继续局部变化；触摸、欢迎和对话动作保持原插值。主页对话卡右上原来的“应用与设置”入口拆为并排的“应用”和“设置”，设置可一键直达。触摸命中与实屏观感仍待用户照片/操作确认。
- 实机 A/B：首版连续观察约 120 秒，自动姿态期间出现 `AFE(FEED)` 满缓冲告警 77 次；提高 Tab5 AFE 任务优先级后，第二版 135 秒告警 0 次，但自动动作窗口最慢绘制约 212 ms。第三版关键姿态 135 秒告警仍为 0；有姿态的 10 秒窗口平均姿态刷新像素由第二版约 118 万降至约 65 万，`slow40` 平均由 14.6 次降至 7.3 次，最慢动作帧约 146 ms。两次采样的自动动作窗口数量不同（5 与 4），这些数字用于定位趋势，不代表实际触摸时延。第三版内部空闲约 110 KB、PSRAM 空闲约 5.59 MB，未见 panic 或看门狗。
- 最新已刷入第四版：ESP-IDF 6.0.2 构建 `11,870,528 B`，SHA-256 `6daafb6eefe63006993968d7b2ea1b515b437dfb0258efbe50ab2a98545b36ef`，仓库外备份 `/Volumes/liutupi/tab5-private-backups/tab5-core-home-nav-v4-2026-10-02.bin`。仅写 `factory` 的 `0x20000` 且写后哈希通过，NVS、assets、升级器未动。宿主测试执行 105 项（104 项通过、1 项跳过）；另用 ESP-IDF 6.1 构建 `m5stack/atom-echos3r` 代表路径成功。第四版启动与约 80 秒待机无崩溃、看门狗或 AFE 满缓冲；本机 251 B UDP 点歌包解析 2 行定时歌词，静音 MP3 首帧 254 ms，电台页绘制正常运行，无音频告警。`ota_0` 只有 1 MB 的尺寸提示仍是当前升级器分区设计限制，完整主固件只装入 12 MB 的 `factory`。

下一步需要真人确认：实屏两入口是否易点、设置滑杆/返回是否顺畅、Nabo 自动动作是否自然、语音唤醒和实际 NAS 点歌/歌词。当前串口只能证明歌词已解析并进入电台页，不能证明文字在屏上无遮挡。还需实机走一次拍照识图和断网重连；原始串口日志在 `/tmp/tab5-core-*-20261002.log`，包含局域网及服务信息，不要上传。

### 2026-10-02 第五版：应用页六卡、隐藏时钟停刷与连接收尾

- 应用首页从四张大卡加两条小入口改为两列三行、六张同规格大触摸卡：设置、电台、ICU、NES、IR、Muse。原有导航回调保留，卡片文字限制在目标宽度内。主页六位翻页钟的外观不变；进入应用页时停止已遮住的翻页动画和数字刷新，返回主页立即同步六位当前时间。
- `Protocol` 的唤醒、开始/停止倾听、打断及 MCP 控制发送现在返回真实发送结果；Tab5 出站任务在控制消息失败时关闭本代会话并回到空闲。旧会话的迟到失败不能清除新会话队列。入站 MCP 动作要求匹配且仍打开的会话，避免断线后的迟到消息产生设备副作用。WebSocket 取消会对活跃 TCP/TLS socket 发出不等待发送锁的中断，发送有短轮询和整体期限。**WSS 同步 TLS 握手尚未发布 socket 描述符，握手期间的取消仍可能等底层调用返回。**
- 相机暂停后的恢复记录已成功重新入队的 MMAP 缓冲编号；部分 `VIDIOC_QBUF` 或 `VIDIOC_STREAMON` 失败后的重试不再重复入队前面的缓冲。电台旧 HTTP reader 超时退出后交由 core0 低优先级任务回收，释放 ring、任务栈及 HTTP 客户端；四槽上限直到清理完成才释放。该异步回收仅通过主机故障注入测试，尚需弱网实机触发并核对日志。
- 本轮宿主测试执行 111 项（110 通过、1 项按环境跳过），`git diff --check` 通过；ESP-IDF 6.0.2 的 Tab5 P4 原生版及 ESP-IDF 6.1 的 S3 `m5stack/atom-echos3r` 代表路径编译通过。第五版镜像 11,872,768 B、SHA-256 `f0441878b282ab11a9e1f0c6974342d333d3760322c6ce0273c4bb11a5df3e73`，仓库外备份 `/Volumes/liutupi/tab5-private-backups/tab5-core-home-nav-v5-2026-10-02.bin`；仅写 `factory` 的 `0x20000`，写后哈希通过，NVS、assets、升级器未动。第四版备份仍可回退。全文件 clang-format 检查会报现存未格式化代码，不能整文件自动格式化造成大量无关差异。
- 第五版开机 95 秒无 panic、看门狗、brownout 或 AFE 满缓冲。本机 28 秒静音 MP3 小包点歌首帧 263 ms、两行定时歌词解析成功、自然播放结束并释放资源。电台应用页静止时，10 秒刷新像素由第四版约 43 万降至第五版 0；动态音频期间仍有必要的页面绘制。47 秒时内部空闲 108,119 B、PSRAM 空闲 5,577,008 B，相比第四版分别少约 1.8 KB、8.6 KB；常驻界面开销较小。原始日志在 `/tmp/tab5-core-home-nav-v5-*-20261002.log`，含网络信息，不要上传。**还需用户实屏照片确认六卡排版和触摸，真人语音/WebSocket/MQTT 断线、真实 NAS 歌名歌词、拍照识图及弱网延迟回收的物理回归。**

第五版之后又在仓库的 `78__esp-ml307` 覆盖组件给 WSS 同步 TLS 建连设置 ESP-TLS 的 10 秒连接上限。ESP-IDF 6.0.2 本地 API 文档说明原先 `timeout_ms=0` 不施加应用级期限；`CancelOpen()` 在 TLS 发布 socket 以前只能等待底层握手返回，故此上限可避免无界占用连接工作者，但不是握手瞬时取消，也未单独实测 DNS 卡住的情况。此单点补丁后完整宿主测试重跑 111 项（110 通过、1 跳过），P4 和 IDF 6.1 S3 代表构建通过；最新已刷入**第六版** `factory` 镜像 11,872,768 B，SHA-256 `a6ef6aec446f75a1c8f24827f018d0c13553f64fc8a6d2262e7e97663b1d64ce`，私有备份 `/Volumes/liutupi/tab5-private-backups/tab5-core-home-nav-v6-2026-10-02.bin`。只写 `0x20000` 且写后哈希通过。刷后 80 秒开机/待机无 panic、看门狗、brownout 或 AFE 满缓冲；47 秒内部空闲 108,563 B、PSRAM 5,587,400 B。第五版的本地音频与零静止刷新数据可代表第六版未改动的电台和界面代码，但第六版尚未重复播放探针。`/tmp/tab5-core-home-nav-v6-boot-20261002.log` 为私有日志。

第六版补做了可控弱网切歌：Mac 本地服务对三条 28 秒静音 MP3 先发送 24 KiB 后停 38 秒，夹着三条正常流，每 5 秒向 Tab5 下发一次新点歌，共 6 次。设备全部收到并开始解码；三次慢流切换时旧 reader 都在约 301 ms 内退出，新流从连接开始到首音频帧依次为 243、82、66、66、90、100 ms。最终播放资源释放后内部空闲 108,155 B，与开机待机量级一致；约 105 秒内无 AFE 满缓冲、断言、崩溃或看门狗。旧 reader 退出及时，**没有触发**新增的延迟回收任务；该极端路径目前只有源码提取故障注入测试。应用页有音频活动的 10 秒窗口平均渲染约 19–21 ms，`slow40=0`；音频结束后的静止窗口刷新像素为 0。日志 `/tmp/tab5-core-home-nav-v6-weak-switch-20261002.log` 含局域网信息，不要上传。

### 2026-10-02 第七版：网络输入边界、每日卡持久化与问候重试

- 覆盖组件 `78__esp-ml307` 的 WebSocket 地址现在只在 authority 中解析端口，校验 1–65535；路径里含冒号不会误判端口，非法 authority 与 IPv6 输入会明确拒绝。入站握手头限制 8 KiB，消息在 PSRAM 配置下限制 128 KiB、非 PSRAM 配置下限制 16 KiB；未分片的未掩码消息直接使用接收缓冲，减少一次大块复制。超限或非法帧清空缓冲并中断连接，交由既有断线流程处理。MQTT 组件对分片消息校验偏移、总长、消息 ID 和首片 topic，连接换代时释放未完成消息；同样采用 128/16 KiB 上限，并把连接状态改为原子快照。主机测试使用真实组件源码，分别覆盖有无 PSRAM 的编译和异常关闭模式。Tab5 实机当前使用 MQTT，WebSocket 入站边界尚无物理协议测试。
- Muse 隐藏页不再开机生成大段内容和宽字形缓存，首次打开时才构建；每日卡在 LVGL 锁内只拷贝简短快照，然后通过 `Application::Schedule()` 在主任务写 NVS，短时间更新会合并。避免显示锁持有期间触碰 flash；跨天首次读取 NVS 仍会在显示锁内执行一次，后续可继续整理。
- 摄像头检测到有人但音频正忙时，Nabo 的问候意图会保留，下次确认帧重试；只有实际发出问候才开始 120 秒冷却。人离开会清除待问候状态。这样避免“检测成功却因忙碌丢掉问候”的竞态。真人进入镜头与真实扬声器声音尚未在第七版复核；不能只凭宿主测试宣称延迟已消除。
- 完整宿主测试执行 117 项（116 通过、1 项按环境跳过），`git diff --check` 通过。ESP-IDF 6.0.2 的 P4 原生版完整构建通过；ESP-IDF 6.1 的 S3 `m5stack/atom-echos3r` 代表路径增量构建退出码为 0，镜像 `0x21da80`，最小 app 分区 `0x2f0000`。第七版 P4 `factory` 镜像 11,876,144 B，SHA-256 `1f45e067d87490eec6e4c2f066d444993c779f02eeaddd7badb32ced6fbcd8d0`，私有备份 `/Volumes/liutupi/tab5-private-backups/tab5-core-home-nav-v7-2026-10-02.bin`。仅刷写 `factory` 的 `0x20000` 且写入哈希验证通过，NVS、assets、升级器未动；第六版备份可回退。
- 第七版刷后约 90 秒观察：MQTT 在启动约 12 秒时连回服务，摄像头和人体检测模型就绪；47 秒时内部空闲 114,307 B、PSRAM 空闲 5,578,640 B，77 秒时相机扫描期间分别为 114,367 B、5,582,868 B。未见 panic、断言、brownout、看门狗或 AFE 满缓冲。自动动作窗口仍有个别 141–182 ms 绘制帧；本轮未取得实屏照片，也未走真实 NAS 歌词、真人问候、设置滑杆、Muse 首次打开、NES 备用路径及拍照识图。第六版弱网六次切歌结果不能直接当作第七版全链路验证。原始启动日志 `/tmp/tab5-core-home-nav-v7-boot-20261002.log` 含网络信息，不要上传。

### 2026-10-02 第八版：电台中文字形与频道行高度

用户允许进一步优化布局后，检查发现电台的动态歌名、歌词和频道名在主题完整字体晚加载时可能仍用基础字形，出现缺字。`tab5_native_apps.cc/.h` 现在为这些动态标签统一选用当前主题文字字体；未就绪时先用内置 CJK 字体，进入电台页及其可见期间字体指针变化时再同步一次，隐藏页的 Tick 不做字体刷新。六个频道行的文字高度从 34 px 调至 44 px，容纳主题字体约 43 px 行高；原有 48 px 按钮触控区域不变。新增 `test_tab5_radio_font_sync.py` 提取实际字体选择、同步和 Tick 分支，验证隐藏时不更新、显示后只切换一次。

完整宿主测试执行 118 项（117 通过、1 项按环境跳过），`git diff --check` 与 ESP-IDF 6.0.2 P4 原生版构建通过。第八版 `factory` 镜像 11,876,272 B，SHA-256 `98ae5817b9dc8ad5bc4dd2b73194c290754fd2eb1673d7be9d4a381b68c402d2`，私有备份 `/Volumes/liutupi/tab5-private-backups/tab5-core-radio-font-v8-2026-10-02.bin`；仅刷 `0x20000`，写入哈希验证通过，NVS、assets、升级器未动。`ota_0` 太小的编译提示仍是现有升级器分区设计。刷后串口从运行约 34 秒开始采集至约 113 秒：相机与人体检测持续运行，90 秒无活动后进入睡姿；47 秒时内部空闲 113,635 B、PSRAM 空闲 5,582,116 B，无 panic、断言、brownout、看门狗或 AFE 满缓冲。自动动作窗口最慢绘制帧仍达约 168 ms，不能据此宣称触摸和动画已达到目标。该日志未覆盖第八版最初 34 秒，因此 MQTT 启动连接以第七版日志为依据；字体在第七版启动时已从 SD 成功加载，但第八版仍需真人目视核对频道、歌名和歌词的字形及排版。私有日志 `/tmp/tab5-core-radio-font-v8-boot-20261002.log` 不要上传。

### 2026-10-02 第八版首启电台崩溃与第九版修复

第八版用本机 269 B UDP 点歌包激活电台时立即出现 Core 1 `Instruction access fault`、`MEPC=0`。用对应 ELF 解码，返回地址落在 LVGL `lv_font_get_glyph_dsc()` 调用字体回调处。原因是隐藏电台页在构造时提前保存主题字体裸指针；约 6.5 秒后 SD 字体替换主题并释放旧字体，首次刷新频道名时调用已失效字体。该崩溃在待机日志中不会出现，故第八版**不可作为稳定回退版本**；原始故障日志 `/tmp/tab5-core-radio-font-v8-play-20261002.log` 保留本机。

第九版让电台与 Muse 的动态标签持有 `shared_ptr<LvglFont>`，字体替换时旧 owner 保留到标签全部重新绑定；电台构造时只用静态内置字体，首次打开前绑定当前主题字体。宿主回归覆盖 SD 字体晚加载、隐藏页首开及热替换、Muse 旧列表清理。歌词搜索同时收紧同名曲匹配：请求明确提供歌手时不能退到忽略歌手的模糊搜索；无歌手请求仍保留原有回退。新增姿态帧与非姿态帧分开统计，便于后续测量慢帧。完整宿主测试执行 119 项（118 通过、1 跳过），`git diff --check` 与 ESP-IDF 6.0.2 P4 构建通过。

第九版 `factory` 镜像 11,876,704 B，SHA-256 `2456dbdc485b429b1b0faf6c26a3cf1292d3f1d9dba4e7f57b508d89f487efc1`，私有备份 `/Volumes/liutupi/tab5-private-backups/tab5-core-radio-font-v9-2026-10-02.bin`。仅刷 `0x20000` 且写入哈希验证通过，NVS、assets、升级器未动。重放同一 269 B 点歌包：两条定时歌词解析成功，本机 28 秒静音 MP3 首帧约 273 ms；首次电台页渲染最慢约 202 ms，后续静止十秒窗口 0 刷新；歌曲自然结束、播放资源释放、相机恢复，约 90 秒观察未再出现崩溃、看门狗、brownout 或 AFE 满缓冲。47 秒时内部空闲 115,027 B、PSRAM 5,580,712 B；播完约 83 秒时内部空闲 110,223 B。该探针验证生命周期与字体不再触发崩溃，不能代替真实 NAS 歌词正确性或实屏目视字形。私有日志 `/tmp/tab5-core-radio-font-v9-play-20261002.log` 含局域网信息，不要上传。

后续重点：电台解码线程每 64 MP3 帧同步等待两次 LVGL 锁，动画慢帧可能拖住音频；断流从头重播时歌词时钟未归零；旧 MCP 逐行歌词可能晚到并覆盖新歌标题。第九版的统计显示自动姿态窗口仍可达到约 143 ms，电台页首次打开约 202 ms。Tab5 目前使用单个 PSRAM draw buffer，经软件旋转及 RGB565 字节交换后送 DSI；硬件 PPA 旋转值得独立 A/B，但在改动前要确认对应 `esp_lvgl_port` 初始化失败路径不会留下悬空句柄。相机的两块 1280×720 RGB24 缓冲约占 5.53 MB PSRAM，板级优先 RGB565 并保留 RGB24 回退理论可省 1.84 MB，仍须实机确认色彩、人体检测及拍照识图。

### 2026-10-02 第十版：音频状态转交显示线程

电台解码回调原先在首帧及每 64 帧同步进入两次 LVGL 显示锁，遇到慢动画帧可能阻塞音频。现在板级使用固定容量、只保留最新状态的邮箱：解码线程拷贝站名、状态和元数据后立即返回；原生界面的 50 ms Tick 在显示线程消费快照并更新歌词与播放状态。自然播放结束事件仍走原有代际校验，不能被覆盖。邮箱限制 UTF-8 字符串长度，避免动态无限排队；主机测试覆盖临时字符串寿命、快照覆盖、结束事件、连续切歌、截断与并发访问。

ESP-IDF 6.0.2 的 P4 原生版构建通过，完整宿主测试执行 122 项（121 通过、1 项按本机环境跳过）。第十版 `factory` 镜像 11,877,344 B，SHA-256 `e492deaffc1695b37169ba15c91c3d80250f34073a28fdd1ba57464adf6f4873`，私有备份 `/Volumes/liutupi/tab5-private-backups/tab5-core-radio-mailbox-v10-2026-10-02.bin`。仅刷写 `0x20000`，写后哈希通过；NVS、assets、升级器未动。重放 269 B 本机静音 MP3 点歌包：两行定时歌词解析成功，首音频帧约 257 ms，歌曲自然结束、资源释放、相机恢复，约 90 秒未见崩溃、看门狗、brownout 或 AFE 满缓冲。相对第九版 273 ms 的单次差异不能证明提速；代码变化的直接收益是解码线程不再等待显示锁。私有日志 `/tmp/tab5-core-radio-mailbox-v10-play-20261002.log` 不要公开。

### 2026-10-02 第十一版：相机 RGB565 节省 PSRAM

Tab5 原生版现在请求 RGB565 摄像头流，通用 `EspVideo` 保留 RGB24 默认值及驱动不支持 RGB565 时的回退；其他板型行为不变。实机 SC202CS 流格式为 `0x50424752`，两块 MMAP 缓冲各 1,843,200 B。与第七版同机开机日志相比，`after camera` PSRAM 空闲从 12,407,276 B 增至 14,246,700 B，净增 1,839,424 B；47 秒常驻 PSRAM 约 7.41 MB，上一版约 5.58 MB。人体检测模型加载成功并持续取帧，无人环境下每次报告 0 人；RGB565 的 RGB888 裁剪、曝光检测、预览与拍照分支经源码核对，尚无真人及颜色实屏验证。

第十一版 P4 `factory` 镜像 11,877,888 B，SHA-256 `2ee5d0b7115d81e54455a9679b30b9de0ec82bf03af70fcd600bea50eff5ec0d`，私有备份 `/Volumes/liutupi/tab5-private-backups/tab5-core-rgb565-v11-2026-10-02.bin`。只刷写 `0x20000` 且写后哈希通过，NVS、assets、升级器未动；ESP-IDF 6.1 的 `m5stack/atom-echos3r` 代表构建也通过，未上 S3 硬件。85 秒开机观察无崩溃、断言、看门狗、brownout 或 AFE 满缓冲。再次从已连 Wi-Fi 的设备点播本机 28 秒静音 MP3：两行歌词解析、播放时相机暂停、自然结束后恢复扫描且资源释放，100 秒观察未见上述故障。该次首帧约 1.04 秒，HTTP 建连本身约 1.00 秒，不能与上版一次 257 ms 样本直接比较。日志 `/tmp/tab5-core-rgb565-v11-boot-20261002.log`、`/tmp/tab5-core-rgb565-v11-play2-20261002.log` 仅留本机。

真人入镜的检出率、颜色与拍照识图、实屏字体/触摸、真实 NAS 歌词、语音及长时间弱网仍未验收。RGB565 改动后的第十一版比不可回退的第八版稳定，遇到相机颜色或识图问题可用第十版私有镜像回退。每日卡分类配色已在工作树，当前第十一版镜像尚未包含。

### 2026-10-02 第十二版：每日卡分类配色与歌词/切歌代际

主页每日内容卡在翻页时按医学摘要、金句、历史、节日、临床干货改变标题和左侧色条的静态配色，不新增逐帧刷新。旧 `self.music.show_lyric` / `set_lyric` 现在只写歌词行，不再改写当前歌名或歌手；完整 `set_lyrics` 和自动检索 LRC 也要求当前曲目标题、可用的歌手及代际匹配。未带标题的旧逐行请求会被拒绝；同名连续切歌而请求未带歌手时也会拒绝模糊匹配。旧 MCP API 无逐首唯一 token，同名同歌手的跨轮迟到包仍不能百分之百区分，真实 NAS 需再验收。

点歌提交与停止、切电台、下一台、NES 等替换音源的操作现在共用曲目锁，避免旧点歌等 LVGL 锁后把已停止的歌曲重新提交。电台自然结束/失败状态在 RadioService 的提交锁内连同源流代际一起校验；板级连播回调只在源流代际、当前曲目和播放轮次都仍匹配时发起下一首。新增交错宿主测试。默认关闭的 `CONFIG_QDTECH_TAB5_PPA_FLUSH_EXPERIMENT` 原生硬件旋转试验代码也已加入工作树，但第十二版未启用，屏幕仍走原软件旋转路径。

第十二版完整宿主测试执行 127 项（126 通过、1 项环境跳过），`git diff --check` 通过。ESP-IDF 6.0.2 的 P4 原生版与 ESP-IDF 6.1 的 S3 `m5stack/atom-echos3r` 代表路径构建成功；S3 未刷机。P4 `factory` 镜像 11,879,376 B，SHA-256 `cc35e97148d6faddc7e74c10d9ffd0d375f8ffac0578b6f328fb6d3d24504929`，私有备份 `/Volumes/liutupi/tab5-private-backups/tab5-core-layout-lyrics-v12-2026-10-02.bin`。仅刷 `0x20000` 且写后哈希验证通过，NVS、assets、升级器未动。

实机联网后连续下发两条 225 B 本机静音 MP3 小包，歌名相同、歌手分别为甲和乙，间隔约 4.2 秒。两首均开始解码，首帧分别约 90 ms 与 37 ms；第二首自然播完并释放播放资源，旧流未再触发结束覆盖。相机在播放时暂停、结束后恢复，100 秒内未见 panic、断言、看门狗、brownout 或 AFE 满缓冲。电台首次打开时一个姿态窗口最慢约 250 ms，不能据此宣称流畅度问题已解决。日志 `/tmp/tab5-core-layout-lyrics-v12-play-20261002.log` 仅留本机，不公开。

正在等待用户实屏确认每日卡配色、应用/设置入口触控、真人问候和 RGB565 取景颜色；真实 NAS 歌名/歌词、快速手触停止、语音唤醒与拍照识图也未完成物理验收。下一步可在独立受控镜像里做 PPA 旋转 A/B，必须核对输出图像与软件旋转一致、失败回退、屏幕交互和慢帧数据后才决定是否启用。

### 2026-10-02 PPA 旋转 A/B：未获得稳定流畅度收益

在仓库外独立构建目录临时开启 `CONFIG_QDTECH_TAB5_PPA_FLUSH_EXPERIMENT`，ESP-IDF 6.0.2 P4 构建第十三版 `factory` 镜像 11,881,744 B，SHA-256 `fd9e40ed58f2ac3d232118738be8bdc2aa1d81e704f2434c534428208b75dc15`，私有备份 `/Volumes/liutupi/tab5-private-backups/tab5-core-ppa-v13-experiment-2026-10-02.bin`。仅写 `0x20000`，写后哈希通过。实验功能先将四块大脏区的 PPA RGB565 输出与软件旋转逐字节比较，全部一致后才用 PPA；失败路径退回软件旋转。约 95 秒待机日志显示 10 秒内 48–121 次 PPA flush，平均旋转约 1.5–2.0 ms，零失败、零失配，未见崩溃、看门狗或 AFE 满缓冲。

但与第十二版相近的自动姿态窗口相比，`pose slow40` 仍为每 10 秒约 3–5 帧，最大姿态帧约 85–119 ms，没有稳定变快；常驻 PSRAM 还少约 72 KB。因此未把 PPA 开关放进正式 variant，已刷回并校验第十二版镜像，恢复后 48 秒正常联网、相机和人体检测就绪，未见故障。实验日志 `/tmp/tab5-core-ppa-v13-idle-20261002.log` 和回退日志 `/tmp/tab5-core-v12-restored-boot-20261002.log` 仅留本机，不要公开。源代码中的实验开关仍默认关闭。

进一步定位发现 Curious、Think、Wink 自动动作采用头身合并的大图，约 11.2–12.6 万像素。每个姿态关键帧对整图做 1–2 像素位移，就会重新合成大块透明图；这比末端旋转更可能解释慢帧。下一版仅对自动待机且无独立头部的姿态固定大图位置，手部、表情、音乐和手动动作保留原轨迹；须用相同时间窗口 A/B 核对 `idle_keys`、`pose slow40`、`pose_flushed_pixels` 及真人观感。

### 2026-10-02 第十四版：ICU 输入可信度与待机大图重绘

ICU 页的 eGFR 不再静默按男性计算，必须先点一次性别；改动数值、性别或静脉泵药物会立即清掉旧结果。语音 MCP 计算打开 ICU 页时隐藏空白手工表单，明确显示语音结果，失败时用警示色；“手动输入”才恢复表单。数值模块对计算溢出返回错误，eGFR/uACR 靠近 G/A 分层边界时增加显示精度并保留较完整的输入回显，避免屏幕数字与分层看似矛盾。pH、PaCO₂ 与 HCO₃⁻ 回算差异超过 0.15 时停止酸碱/代偿解读并提示核对标本和单位。2021 CKD-EPI 肌酐公式和 KDIGO 2024 分层仍按原单位实现；临床使用仍须核对检验单位与病人情况。

自动待机中 Curious、Think、Wink 的头身合并图不再随 1–2 像素的 body_x/body_y 位移重复整图重绘；局部手势、表情、音乐与手动动作不变，额外内存为零。`CONFIG_QDTECH_TAB5_PPA_FLUSH_EXPERIMENT` 在正式配置仍关闭。完整宿主测试 128 项（127 通过、1 项环境跳过），`git diff --check` 通过；ESP-IDF 6.0.2 P4 原生 variant 构建成功。此次更动为 Tab5 板级模块；ESP-IDF 6.1 S3 代表路径最后一次成功构建仍是第十二版。

第十四版 `factory` 镜像 11,882,080 B，SHA-256 `17ea45d4d63498606882071c9b745c4554ce4cd0ff3796c9f4961e52eda9ddd2`，仓库外备份 `/Volumes/liutupi/tab5-private-backups/tab5-core-icu-frame-v14-2026-10-02.bin`。仅写 `0x20000`，写后哈希验证通过；NVS、assets、升级器未动。从设备运行约 32 秒起连续采集 115 秒串口，此观察段无 panic、断言、看门狗、brownout 或 AFE 满缓冲；本次未捕获最初启动段。47 秒内部空闲 113,119 B、PSRAM 7,418,172 B；人体检测持续取帧，静置后进入睡姿。日志 `/tmp/tab5-core-icu-frame-v14-idle-20261002.log` 仅留本机。

相近的 10 秒自动姿态窗口中，第十三版实验镜像 `idle_keys=3–5`、姿态绘制超过 40 ms 为 3–5 帧、姿态刷新约 61–83 万像素；第十四版正式软件旋转镜像 `idle_keys=1`、姿态慢帧 1–2 帧、约 27–48 万像素。两版旋转路径不同，且动作抽样有限；第十二版软件旋转的一个窗口为 4 帧姿态慢帧、约 72 万像素，也支持改善趋势。**首页其他绘制**仍出现约 128–145 ms 的峰值，因此不能宣称整个 UI 已稳定流畅。真人目视动作自然度、ICU 页触摸与文本、设置亮度/音量、实际 NAS 歌词、摄像头真人/颜色、语音与拍照识图仍需用户和设备进一步验收。

### 2026-10-02 第十五版：ICU 键盘单位提示与慢帧分类

ICU 的数字键盘会盖住原输入卡，先前只显示数值，血气和静脉泵有误填字段/单位的风险。弹层顶部现显示当前字段及单位，数值和按键在原弹层内下移；静脉泵总药量提示随药物在 `mg` 与 `U` 间切换。扩展真实页面回调的宿主假 LVGL 测试，覆盖 eGFR 年龄、HCO₃⁻、泵药量 mg→U、清空、完成及返回。正确工作树完整宿主测试 129 项（128 通过、1 项环境跳过），`git diff --check` 与 ESP-IDF 6.0.2 P4 原生 variant 构建通过；其它板型本轮没有变动。曾误在 `/Users/tupi/tab5-build` 旧副本编辑，已按具体 hunk 撤回误改及误加测试；该旧目录原有的其它工作树改动保留。正式固件仍关闭 PPA 实验开关。

在已有 Tab5 帧统计旁新增约 160 B 固定内存的归因桶。下一次渲染帧会标记动作退场、睡醒切换、每日卡翻页，时钟翻页标记其约 180 ms 动画窗口；每 10 秒报告各类帧数、超过 40 ms 的帧数、最大耗时和对应单帧 flush 像素。不逐帧打印；同一帧可计入多类，标记是同帧相关性而非 CPU 时间的因果证明。

第十五版 `factory` 镜像 11,882,592 B，SHA-256 `0c12b54ebb83a0d076555d0a243f22cb79f2321b9c04fb8369210675dd3ff467`，仓库外备份 `/Volumes/liutupi/tab5-private-backups/tab5-core-icu-keypad-frame-v15-2026-10-02.bin`。仅刷 `0x20000` 且写后哈希验证通过，NVS、assets、升级器未动。从设备运行约 22 秒起采集 125 秒串口，观察段无 panic、断言、看门狗、brownout 或 AFE 满缓冲；人体检测持续运行并进入睡姿。47 秒内部空闲 112,179 B、PSRAM 7,413,112 B，与第十四版同量级。私有日志 `/tmp/tab5-core-icu-keypad-frame-v15-idle-20261002.log` 不要公开。

这段闲置采样中，动作退场标记帧的最大绘制时间约 95–149 ms，对应约 24–25 万刷新像素；一次睡姿切换约 128 ms、28 万像素。每日卡翻页标记帧多次超过 40 ms，最大约 52–129 ms、约 18–22 万像素；与时钟动画重叠的帧会被重复计数。单独时钟窗内也偶有慢帧，但页面翻页时刷新面积更大。尚有未标记的约 103–111 ms 帧、约 24 万像素，可能是人物动作入场；当前归因未覆盖这一事件。由此可以确定大图进退场和内容卡翻页都值得下一步优化，**不能把全部慢帧归咎于时钟或 PPA 旋转**。本版 ICU 弹层的实际手指命中、字形与遮挡，及整屏视觉节奏仍待用户实屏照片确认。

由于 ICU 页面源码也在 `qdtech-tab5` 非原生变体的源码集合中，随后用同一 ESP-IDF 6.0.2 仓库外构建目录执行该变体完整构建，退出码 0，镜像约 7.16 MB，未刷设备。此操作已把 `/Users/tupi/tab5-core-verify/build/xiaozhi.bin` 和生成的 `sdkconfig` 切换成**非原生变体**；设备仍运行第十五版原生界面，原生版的正确备份路径与 SHA-256 见上段。下一次原生构建必须用 `scripts/build.py qdtech/tab5 --name qdtech-tab5-native` 重新选择配置，不能直接拿当前 `build/xiaozhi.bin` 刷机。

针对动作退场，仓库外离线样件 `/tmp/tab5-portrait-matte-prototype/` 把原 434×618 RGB565A8 肖像的可见 434×558 区域与现有双层深蓝卡片背景预合成为 484,344 B RGB565，保留可覆盖的眨眼/嘴部叠层。原透明肖像为 804,636 B，样件的原图像素经编码后与当前 `nabo_assets.c` 对应字节完全相同；预合成可避免约 18.2 万半透明像素的逐像素混合，但退场仍须刷新约 24.2 万像素，收益尚无实机数据。样件使用 Pillow 近似圆角覆盖率，不能证明与 LVGL 的圆角抗锯齿及 1 px 边框逐像素一致；当时**未合入固件、未刷机**。后续集成与验证见下节。

### 2026-10-02 第十六至十七版：每日卡布局、Nabo 立绘与电台重连提示

主页每日内容卡更新先只刷新小卡；对话镜像等本轮 LVGL 渲染完成后再更新，并用版本/页面状态阻止迟到更新覆盖新内容。时钟与日期位置不变。电台/点歌 HTTP 流在同一首歌重连时给状态快照增加开流代际，歌词时间轴随新流重新起算。电台页的播放按钮现在依照实际的“请求播放”意图显示“暂停”或“播放”，彩虹声波仍只在解码播放中运动；断网等待恢复时点按钮不会把原本待续播的请求误当成新播放。

预合成 Nabo 静态正面立绘已作为 `CONFIG_QDTECH_TAB5_PORTRAIT_MATTE_EXPERIMENT` 板级选项加入，`qdtech-tab5-native` 变体开启，旧桌面变体关闭；图片为内置的 434×558 RGB565 共 484,344 B，眨眼和嘴型继续独立覆盖，睡姿及其他动作仍用原图。构建时按固定尺寸/SHA-256 检查图片，增量改动也触发 CMake 重新配置；资源在固件内，不要求 SD 卡。实屏圆角、边框和真人观感仍等用户照片确认。

同为约 50–120 秒的七个 10 秒待机窗口：第十六版默认头像渲染 1558 帧、超过 40 ms 为 38 帧、加权平均 9.71 ms；省内存头像实验镜像为 1578 帧/25 帧/8.77 ms；最终第十七版为 1569 帧/19 帧/9.06 ms。样本短且姿态抽样不同，不能把这个差异当成长期帧率结论。最终版人物退场四次标记峰值约 63、83、115、90 ms；仍有未标记的人物入场疑似峰值约 183–185 ms，睡姿切换约 127 ms。约 77.7 秒时最终版空闲 PSRAM 7,731,788 B、内部堆 112,635 B；默认头像版同阶段为 7,416,656 B、111,867 B。头像节省内存约 315 KB，尚未测量手指触控延迟。

最终源码完整宿主测试 139 项（138 通过、1 项环境跳过），`git diff --check` 通过；ESP-IDF 6.0.2 原生及旧桌面 `qdtech-tab5` 变体均构建成功，旧桌面镜像 7,157,856 B，仅编译未刷机，链接图不含预合成头像。第十七版 `factory` 镜像 11,563,664 B，距 12 MiB factory 分区上限尚有 1,019,248 B，SHA-256 `e6b61f6d038db786095f56a75df29c6516fdf83c2f4334f05f4db519ba4ab6ae`，仓库外备份 `/Volumes/liutupi/tab5-private-backups/tab5-core-layout-matte-radio-v17-2026-10-02.bin`。仅刷 `0x20000`，写后哈希验证通过；NVS、assets、升级器未动。串口从设备运行约 10 秒采集至约 123 秒，Wi-Fi、MQTT、摄像头、人体检测和静置睡眠均可见，未见 panic、看门狗、brownout 或 AFE 满缓冲。日志 `/tmp/tab5-core-layout-matte-radio-v17-boot-20261002.log` 仅留本机，不公开。构建目录 `/Users/tupi/tab5-core-verify` 已切换到旧桌面兼容构建；**不要把其中 `build/xiaozhi.bin` 当作当前设备原生固件刷写**，要回退请使用上面的备份。

用户尚需拍当前 Tab5 正面屏幕照片，用于核对预合成立绘的圆角、每日卡片、时钟和整体视觉；真实 NAS 歌名/歌词、设置与 ICU 手触、电台断网重连、真人问候和长时运行也仍需实机交互验收。若继续优化画面，先给未标记的大图入场帧加归因，并用相同待机窗口和照片对照，避免仅凭单次峰值切换渲染路径。

网络延迟另做了不改固件的短时对照：当前待机为 `LOW_POWER (MAX_MODEM)`，20 次间隔 0.5 秒的 ping 中位数 367.8 ms、P95 约 658.6 ms，奇偶轮次呈快慢交替；本机 28 秒静音 MP3 播放时切到 `BALANCED (MIN_MODEM)`，同时测得中位数 151.2 ms、P95 约 223.0 ms，两组均无丢包。该音频 264 ms 得到首帧，自然结束后相机恢复、Wi-Fi 回到 MAX_MODEM，未见崩溃。**播放数据流本身也会唤醒网络，不能把差异全部归因于省电配置。** 下轮应让 Tab5 在纯待机时临时采用 BALANCED 做同条件 A/B，测连接与推送延迟及 USB 电流；语音或本地摄像头问候另有自己的耗时，不能由 ping 直接推断。私有日志 `/tmp/tab5-v17-idle-ping-20261002.log`、`/tmp/tab5-v17-balanced-ping-20261002.log`、`/tmp/tab5-v17-balanced-audio-ab-20261002.log` 仅留本机。

### 2026-10-03 第十八至二十版：动作入场归因、资源 A/B 与待机网络

在原有归因桶中补上 Nabo 动作入场标记及动作编号。第十八版原压缩 Think 动作一次入场为 97.499 ms、约 24.2 万刷新像素；第十九版尝试只把 Think 躯干换成未经压缩的 RGB565A8，首轮入场为 94.654 ms、仍约 24.2 万像素，第二次 91.514 ms 与睡姿切换重叠，不能解释为稳定改善。原始资源 363,132 B，且本机开启 `SPIRAM_RODATA/XIP_FROM_PSRAM` 后，实验版在相近时点的空闲 PSRAM 比基线少约 367 KB。因此 `CONFIG_QDTECH_TAB5_RAW_THINK_TORSO_EXPERIMENT` 保持关闭；资源和生成脚本保留用于后续试验。第十八版备份 `/Volumes/liutupi/tab5-private-backups/tab5-core-pose-entry-baseline-v18-2026-10-02.bin`，第十九版实验备份 `/Volumes/liutupi/tab5-private-backups/tab5-core-raw-think-ab-v19-2026-10-02.bin`；两者均曾写入 `factory` 并经写入哈希校验，第十九版之后已恢复第十八版。日志 `/tmp/tab5-core-pose-entry-baseline-v18-idle-20261002.log`、`/tmp/tab5-core-raw-think-ab-v19-idle-20261002.log` 留本机。动作进退场和每日卡翻页仍可能出现约 90–180 ms 慢帧；还不能声称整屏动画已稳定丝滑。

随后增加只依赖 Tab5 原生版的空闲 Wi-Fi BALANCED 开关；`Application::SetExternalAudioActive()` 的省电切换统一走 Board 接口，使外部音乐结束后也遵守板级策略。普通 Wi-Fi 板的原三档映射不变，主机源码提取测试覆盖两档编译分支和自定义 Board 分派。第十八版 MAX_MODEM 的纯待机 ping 共 100/100 回包，中位数 482.0 ms、P95 919.3 ms；第二十版 MIN_MODEM 的纯待机同样 100/100 回包，中位数 143.6 ms、P95 253.6 ms。启动日志明确显示 `BALANCED (MIN_MODEM)`，Wi-Fi/MQTT、相机及睡姿运行；本机 28 秒静音 MP3 用 UDP 成功下发、完整播放、自然结束、恢复背光及相机，音频结束后仍保持 BALANCED，未见 panic、看门狗、brownout 或 AFE 满缓冲。此结果支持在 `qdtech-tab5-native` 正式配置开启 `CONFIG_QDTECH_TAB5_IDLE_BALANCED_WIFI_EXPERIMENT=y`；旧桌面变体继续关闭。此测量是局域网往返时延，尚未测 USB 电流、长期温升、语音和 NAS 端到端延迟，也没有真人画面照片。

第二十版曾写入设备 `factory`，镜像 11,563,952 B、SHA-256 `ba3e0ae8de5b8d9ad274d8e8bd3fb6514773cab350ed3b234b5c50685e7707a7`，私有备份 `/Volumes/liutupi/tab5-private-backups/tab5-core-idle-balanced-ab-v20-2026-10-03.bin`。仅写 `0x20000`，esptool 写入哈希验证通过；NVS、SD 内容和升级器未动。此镜像构建时的配置与现已更新的权威 `qdtech-tab5-native` 配置逐字一致；scratch 目录已经被旧桌面构建切换，**不要直接把 scratch 的 `build/xiaozhi.bin` 当作当前设备原生固件刷入**。宿主测试 145 项，144 通过、1 项环境跳过，`git diff --check` 通过；P4 原生版和旧桌面 `qdtech-tab5` 变体均完整构建通过，旧桌面镜像 7,157,856 B，仅编译未刷。音频完成后再次待机测 20 次 ping，20/20 回包、平均 148 ms。纯待机和音频日志分别为 `/tmp/tab5-v20-idle-balanced-boot-20261003.log`、`/tmp/tab5-v20-idle-balanced-serial-20261003.log`、`/tmp/tab5-v20-balanced-audio-smoke-20261003.log`，含局域网及服务信息，只留本机。IDF 6.1 S3 代表构建结果见后续记录。

### 2026-10-03 第二十一版：每日卡可见性门控与当前设备镜像

只读审查发现应用页盖住主页时，每日卡更新可能错误地把应用页的 `LV_EVENT_RENDER_READY` 当作小卡渲染完成，先更新隐藏的对话镜像。现在 `ShowDailyPage` 在应用页可见时记录待重放，`OnDailyRenderReady()` 不把应用页帧计作主页卡片帧，返回主页后重排当前页，真正完成一帧主页渲染再更新镜像。短开短关应用页的时序也有宿主假 LVGL 回归覆盖。预合成头像、时钟、电台等视觉设计和第二十版待机网络策略没有改变。

最终源码完整宿主测试 145 项（144 通过、1 项环境跳过），定向每日卡测试、`git diff --check`、改动区域 clang-format 检查及 ESP-IDF 6.0.2 P4 原生 variant 完整构建通过。第二十一版备份 `/Volumes/liutupi/tab5-private-backups/tab5-core-daily-gate-balanced-v21-2026-10-03.bin`，11,564,080 B，SHA-256 `99a0a9daff63947d316040c44633cc44f7da31a44f1bf5cc543e4b8a3ebe2084`，已写到设备 `factory` 的 `0x20000`，写入哈希通过；NVS、SD 内容及升级器未动。启动日志确认激活完成、空闲 `BALANCED (MIN_MODEM)`、人体检测模型就绪；90 秒无活动后 Nabo 进入睡姿。约 77.6 秒时空闲 PSRAM 7,733,884 B、内部堆 122,511 B，约 110 秒串口观察没有 panic、看门狗、brownout 或 AFE 满缓冲。刷后 20 次待机 ping 全部回包、平均约 137 ms。持续运行、实屏排版/触摸、真人摄像头问候与 NAS 歌词仍需验收；日志 `/tmp/tab5-v21-final-boot-20261003.log`、`/tmp/tab5-v21-final-idle-ping-20261003.log` 只留本机，不公开。约 35 秒时外部 Muse 隧道轮询超时，这是该外部服务的可达性问题，不影响本机主页和相机启动，但 Muse 内容暂不能仅凭本次日志认定可用。

公共 `Application` 改动另以 ESP-IDF 6.1 的 S3 `m5stack/atom-echos3r` 在独立 APFS 副本验证：原配置的源码编译、ELF 链接、bin 生成均通过，但最后 1 MiB factory 大小检查失败。该板 `config.json` 设置了 `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME` 却未显式设置 `CONFIG_PARTITION_TABLE_CUSTOM=y`；仅在临时副本补这个配置后，完整 canonical 构建通过，2.2 MiB 镜像所在 0x2f0000 app 分区尚余约 24%。未把该板分区配置修改带回权威源，以免在本次 Tab5 工作中改变其他板的 OTA 分区布局。验证日志在 `/Users/tupi/.cache/tab5-s3-verify.LHUDBv/`，S3 未刷机。

### 2026-10-03 每日推荐与点歌播放链路修复

NAS 在 `192.168.3.0/24`，Tab5 在 `192.168.88.0/24`；原网易云 MCP 容器向 Tab5 直发 UDP 无法跨网段到达，云端反向调用 `self.music.play_url` 又会超时，造成“说有歌曲但不播放”。现由 NAS 上的 `xiaozhi-netease-nabo` 将歌曲 URL、歌名、歌手、准确网易云歌曲 ID 和连续播放标志送至本机 `muse-relay`，Tab5 的 Muse 任务通过原有公网隧道每约 4 秒取一次短期有效的播放命令，在主应用任务里启动播放器。中继 POST 仅接受本机和已实测的 NAS 容器网关来源，公网访问仍拒绝；中继源码为 `tools/muse-relay/relay.js`，NAS 部署文件 `/data/relay.js`。NAS 网易云 MCP 的部署脚本位于容器 `/app/xiaozhi-ws-mcp.js`，其 `/app` 是 NAS 共享文件夹 `docker/xiaozhi-mcp-services/netease-music-nabo` 的挂载；部署前备份 `.bak-20261003-relay`。中继备份为 `/data/relay.js.bak-20261003-music`。这些 NAS 修改还没有同步到公开仓库中的脚本文件，后续维护不要用旧容器脚本覆盖。

歌词错配的原因是 NAS MCP 在解析工具结果时漏传 `song_id`，设备只得按歌名搜索。NAS 部署脚本已补传 ID；设备在收到有效 ID 但该曲没有定时 LRC 时不再回退到同名的其他录音，避免显示错歌词。无 ID 的点歌仍按歌名和歌手严格匹配。最终 ESP-IDF 6.0.2 原生版镜像 11,565,904 B，SHA-256 `ee38e7776e92cdd94b494333f4b73b1003209c270fdacb5204fffa08fb26711c`；只刷 `factory` 的 `0x20000`，写后哈希验证通过，NVS、assets、updater 和 SD 未动。刷前主程序备份 `/Volumes/liutupi/tab5-private-backups/tab5-factory-before-optimization-2026-10-02.bin` 仍可回退，不得上传。实机串口 `/tmp/tab5-music-final-v2-serial-20261003.log` 记录每日推荐《Men On The Moon》从 NAS 队列到 Tab5：音源 HTTP 200、44.1 kHz 立体声 MP3 持续解码、准确 ID 对应的 51 行定时歌词加载到页面，观察段未见 panic、看门狗或 brownout。实际扬声器听感和歌词视觉位置仍等用户现场确认。NAS 自动续播和用户语音点歌应分别再做一次实际体验确认。

### 2026-10-03 待机突然播放：NAS 重复连播计时器

用户报告无新任务时突然放歌。NAS `xiaozhi-netease-nabo` 日志直接记录 `private fm autoplay next fetching`、`music queued to relay`、`private fm autoplay scheduled`，说明该容器在后台按歌曲时长自行续播；Tab5 固件收到 `continuous=true` 后也会在歌曲自然结束时请求下一首，两端重复负责连播。Nabo 容器还将 `privateFmAutoplay=true` 保存到 `/app/private-fm-autoplay-<账号哈希>.json`，进程重启后会恢复旧会话；检查时该状态已经持续约一小时。relay 的歌曲命令本身只保留约 90 秒，长期待机后的新歌是 NAS 定时器新投递的，不是 relay 长期保存的旧 URL。

已在 NAS 挂载脚本 `/app/xiaozhi-ws-mcp.js` 添加按账号标记的 `tab5ManagedPlayback`：带 `/app/tab5-managed-<账号哈希>` 标记的 Nabo 账号在载入旧状态和处理新的私有电台请求时都不启动 NAS 自动续播，其他账号仍沿用旧逻辑。Nabo 的保存状态已改为 `enabled=false`，只重启了 `xiaozhi-netease-nabo` 容器。脚本和状态文件分别备份为同目录 `.bak-20261003-no-autoplay`；`node --check` 通过，容器重新运行并连接小智，复核 `enabled=false` 且 relay `/tab5/music` 返回空命令。修复发生在 NAS 共享目录，**不在本仓库的固件或 relay 源码中**；重建 NAS 服务时须保留这项门控。未主动下达播放命令做音频回归，以免再次打扰用户；下一次自然使用应确认语音点歌及一首歌结束后的设备端续播。
