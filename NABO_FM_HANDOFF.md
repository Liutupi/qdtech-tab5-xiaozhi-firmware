# NABO FM 电台页交接（2026-10-19，Claude → Codex）

## Codex 接续记录（2026-10-19）

### 台表接续

- 用户确认电台页实机验收“都正常”后，已提交 `aec0d69`（`feat(tab5): add validated NABO FM radio studio`）。
- 台表分支的增量代码已导入本工作树，保留了电台页已验收的统一音柱绘制、direct blit、缓冲借还和调试日志清理。原 `claude/nabo-fm-catalog` 工作树及其暂存区均未修改。
- 已从原始 `stations.json` 重新生成并逐字节校验 `catalog.bin`：1,079 台、35 个地区、96,908 字节，常驻索引 17,806 字节。SHA-256 `6aea672aedd345685b695806deabcf9fdd0e7c727a7e9e271c0dc2ca1bfb7b6e`，原始 JSON SHA-256 `5b7ec6854a5d2d4533cdd65b0912333025030a8e8f69f7c4e7842dd397bcddce`。已写入 SD 卡 `/tab5/nabo/radio-v1/catalog.bin` 并回读验证；原有 17 个动画、道具和字库文件的 SHA-256 均保持不变，随后安全弹卡，用户已装回设备。
- 修复收藏迁移的保存失败保护：只有新 ID blob 写入及 `nvs_commit` 均成功才删除旧 `fav_<n>`。新增宿主回归覆盖打开/写入/提交失败、重试、去重及已有新格式时不重复迁移。
- 完整宿主测试 **191 项全部通过、无跳过**；格式化后另跑 21 项电台相关测试通过。新文件 clang-format、既有文件改动行格式检查及 `git diff --check` 通过。
- ESP-IDF 6.0.2 原生合并版构建通过，主程序 11,560,416 字节，factory 剩余 1,022,496 字节，SHA-256 `f9a05336d8fcb366b5e7e98cd7939f622456487ad0f289f8b6b2a640476f0731`。已备份当前 NVS（`0x9000`、16 KiB），随后仅写 `0x20000` 主程序，esptool 写后哈希验证通过。
- 本阶段 bin/elf/map、NVS 备份、SD 校验、测试和刷机日志均在私有目录 `/Volumes/liutupi/tab5-private-backups/nabo-fm-catalog-20261019-220035/`，不得上传。迁移会删除旧收藏键，若需回退到迁移前固件并恢复旧收藏，应配套使用这份 NVS 备份。
- 旧桌面 `qdtech-tab5` 变体首次链接因 `tab5_muse::CachedRelayConnection` 缺定义失败（该调用在 `aec0d69` 中已存在，而非原生 CMake 排除了 Muse 实现）。已为该缓存调用补上原生版条件编译，旧桌面直接使用原 URL；原生版 TLS/Host 缓存回归通过，旧桌面完整构建也已通过，镜像 7,185,760 字节，仅编译未刷机。
- 首个合并版实机日志确认 **1,079 台、35 地区、17,806 字节索引**，实际台表总常驻约 20 KB；37 个旧收藏记录迁移为 3 个收藏 ID，上次台号恢复成功。但本轮开机 39 秒、68 秒出现 IDLE0 看门狗告警，两次均早于 102 秒的台表加载，未重启；首次寄存器地址对应首页 `Rotate270Rgb565` / `BlitRotated`。
- 调度排查中，直写屏每帧让步 1 ms 的试验版仍在首页开机 38 秒出现 IDLE0 告警，不能视为已修复。进一步发现天空背景及云层任务一次计算约 12 秒，执行期间也需要主动让出 CPU。
- 最终调度方案：直写屏每帧至少阻塞 5 ms，保留原有 100 ms DMA 超时保护；后台天空渐变、边缘融合及云层遮罩每 8 行让步 5 ms，远景楼群按栋让步。宿主测试对比所有天色/天气下的原始与分块计算像素完全一致，并覆盖即时/延迟/丢失 DMA 完成及提交错误。最终源码的 **192 项宿主测试全部通过**，原生及旧桌面变体构建均通过。
- 当前设备已刷入最终分块调度版，仍仅写 factory 主程序，写后哈希通过。主程序 **11,560,544 字节**，factory 剩余 **1,022,368 字节**，当前 SHA-256 **`6af54113b01cdccf8e5653b9edf49741bd6b03ea8f1071c167bb23701ed6c65e`**。前期镜像和符号分别保存在 `xiaozhi-before-yield.*`、`xiaozhi-yield1.*`、`xiaozhi-yield5.*`；当前 `xiaozhi.*` 对应最终版本。
- 用户已确认台表阶段“全部正常，收藏重启后也保留”，覆盖 1,079 台、各省/新闻/音乐/交通波段、拖动换台、旧收藏迁移、新增收藏重启保存和返回首页。最终版独立首页观察 **120 秒通过**：包含两次耗时 16,481 ms、18,680 ms 的天空更新，动画持续出帧，未再出现看门狗、panic、断言、brownout 或直写屏停用告警。日志为 `runtime-final-home.log`。
- 本阶段实现、设备资源安装、构建、短时运行和用户验收均已完成，代码与本记录一同本地提交到 `claude/nabo-fm-studio`。后续仍可扩展长期待机、长时播放、语音唤醒/打断、AEC 及弱网重连测试；本轮短时观察不代表这些项目已完成。

### 电台页清理与验收

- 已在正确的 `claude/nabo-fm-studio` 工作树删除 `RenderDbg`、`InvDbg` 两段临时回调和两个专用头文件，保留每 10 秒的帧数统计；未改动音柱自绘和 direct blit 路径。
- 已同步 board README 的实际动作尺寸（451 × 395）、一次性手势帧率和借用缓冲机制。
- 完整宿主测试运行 182 项，181 通过、1 项 SDK CRC 测试因环境跳过；随后用系统 Python 设置 `IDF_PATH=/Users/tupi/esp-idf-v6.0.2` 单独补跑 SDK CRC 测试通过。`git diff --check` 与 `tab5_radio_stage.cc` 的 clang-format 检查通过。
- ESP-IDF 6.0.2 原生版增量构建通过。主程序 11,545,824 字节，12 MiB factory 分区剩余 1,037,088 字节，SHA-256 `04d25c1dfc019152d86bc9cf52a04b41490d7e5255b5d82cb077353f5ab7eca8`。构建中的 `ota_0` 过小提示仍对应原有 1 MiB 升级器分区，未修改分区布局。
- 已通过 `/dev/cu.usbmodem212401` 刷入主程序 `0x20000`，esptool 写后哈希校验通过；本次只写 factory 主程序。
- 清理前的改动快照、清理后的 bin/elf/map、测试和构建记录保存在私有目录 `/Volumes/liutupi/tab5-private-backups/nabo-fm-studio-clean-20261019-215009/`，不得上传。原始回退备份 SHA-256 已复核一致。
- 已采集清理版约 180 秒实机日志：Wi-Fi、SD 卡、首页动画正常启动，电台 13/13 动作包识别成功并借到缓冲；日志覆盖广播播放和歌曲播放，歌曲的 112 行定时歌词加载成功。循环播放窗口通常为每约 10 秒 direct=73–81、lvgl=0，一次性动作窗口达到 direct=87；广播平稳窗口 LVGL 平均约 8–14 ms，歌曲歌词刷新窗口约 12–14 ms，切页/语音切歌时另有慢帧。98 秒处出现一次 `frame 14 failed (5)`（读帧超时），之后继续出帧。观察段无 panic、断言、看门狗、brownout 或残留 RenderDbg/InvDbg 输出。
- 另观察到若干 `Headphone polling failed: ESP_ERR_TIMEOUT`，未修改耳机检测代码；用户实测声音/插拔时需留意。启动时旧 `cjk28.bin` 字库加载失败，但随后的 SD text font（2,609,092 字节）已加载成功。
- 用户已确认本轮实机验收“都正常”，覆盖播放/拖动换台、波段、反复收藏、点歌歌词以及返回首页的画面、声音和动画恢复。180 秒日志观察段未覆盖最后的返回首页操作，因此缓冲归还以用户实际确认首页动画恢复为依据。电台页已验收，下一步提交本阶段并合入台表改动。以下内容为交接时的背景记录。

## 原始交接背景（接续前状态）

Tab5 原生电台页已改成 NABO FM 横屏版：左侧是 Nabo 直播间（13 个动作从 SD 卡读），右侧是台名、提示/歌词条、波段、可拖动刻度盘和按钮。代码在本工作树，**全部未提交**。设备上刷的是本工作树最新构建（带临时调试日志）。最后一次修复（音柱改成单个对象）已在实机确认：播放中 Nabo 稳定 8 fps（每 10 秒 direct=80，lvgl=0），LVGL 平均渲染 7–10 ms，不再出现整屏重画。

## 位置

- 本工作树：`/Users/tupi/tab5-build/.claude/worktrees/nabo-fm-studio`，分支 `claude/nabo-fm-studio`，基于 `origin/main` 5fcdc59（v1.0.15）。
- **不要用 `/Users/tupi/tab5-build` 的本地 main**：它停在 v1.0.6，还带着 10 月 2 日的旧改动。
- 台表任务在另一个工作树：`/Users/tupi/tab5-build/.claude/worktrees/nabo-fm-catalog`，分支 `claude/nabo-fm-catalog`，同样基于 5fcdc59，同样未提交。见下文“台表”。
- 构建：`source /Users/tupi/esp-idf-v6.0.2/export.sh`，然后 `cmake --build build`。本工作树已经用 `python3 scripts/build.py qdtech/tab5 --name qdtech-tab5-native` 配置过。新增 `.cc` 后需先执行 `cmake -S . -B build` 重新收集板级源文件，或重新运行规范 variant 构建；仅运行增量构建不会发现新文件。直接用 `idf.py build` 会报 python 路径不一致。
- 刷机（只写主程序）：`python -m esptool --chip esp32p4 -p /dev/cu.usbmodem212401 -b 921600 write-flash 0x20000 build/xiaozhi.bin`。刷之前先确认没有串口录日志的 python 进程占着端口（`lsof /dev/cu.usbmodem212401`）。
- 刷机前的备份：`/Volumes/liutupi/tab5-private-backups/tab5-before-nabo-fm-studio-2026-10-19.bin`（v1.0.14 的 factory 分区，12 MiB，SHA-256 `c8dc7cf2244b02bccdc8bf2b009fb70e014372704fd0a6d07da99d7c1359cdfa`）。不得上传。
- 测试：`python3 -m unittest discover -s scripts/tests`，最近一次 182 项通过。必须用系统 python3（带 numpy），在 IDF 环境里跑时 NABO 相关测试会被跳过。

## 改了什么

| 文件 | 内容 |
|---|---|
| `tab5_radio_page.cc`（新） | 电台页，从 `tab5_native_apps.cc` 移出来重写。包括波段（收藏、各分类、全部）、刻度盘拖动、停手 500 ms 再连接、邻台、收藏、提示/歌词条、点歌、停止，以及从 SD 卡加载 56 px 文楷台名字库。 |
| `tab5_radio_stage.{h,cc}`（新） | 直播间。读帧任务从 SD 读 NABOSD1 v3 包。**帧缓冲向首页场景借用**，见下一节。读帧任务把墙、话筒架、Nabo、话筒、杯子合成为不透明 RGB565，再用首页同款 direct blit 直接写屏。音柱是单个对象自绘。 |
| `nabo_radio_mood.h`（新） | 电台状态到 Nabo 动作的对应，可在宿主测试：开页招手；播放戴耳机；连接中调台；音乐台点头和跟唱交替；谈话台听；停止摘耳机；出错哎呀；没网找信号；收藏比心；播放中每 60–120 秒喝一次咖啡。 |
| `tab5_sd_scene.{h,cc}`、`nabo_scene_mailbox.h` | 新增 `Tab5SdSceneBorrowPool/ReturnPool`：首页场景隐藏时把 3 个 571 KB 槽、编码缓冲和 staging 借给电台。首页读帧任务在 pump 期间持锁，借出期间不读帧。`ReclaimIdle()` 会回收取消后遗留的 Ready 帧。 |
| `tab5_native_apps.{h,cc}`、`tab5_native_display.h`、`m5stack_tab5.cc` | 新增 actions（`station_band`、`current_station`、`station_favorite`、`toggle_favorite`）和 `SetDirectBlit` 透传。 |
| `main/CMakeLists.txt` | 旧桌面变体排除两个新文件。文件是混合换行，**只能按字节修改**。 |
| `scripts/prepare_nabo_radio_actions.py`（新） | 从 AI Passport 仓库的绿幕视频生成 451×395 动作包、话筒和杯子道具，以及 `wenkai_56.bin`。 |
| 测试 | 新增 `test_nabo_radio.py`、两个 smoke 程序；`test_tab5_radio_font_sync.py` 和 `test_tab5_radio_play_intent_ui.py` 改为切 `tab5_radio_page.cc`。 |

## 内存（关键）

PSRAM 运行时只剩约 600 KB，内部 RAM 约 110 KB，小于 1 KB 的分配会落在内部 RAM。电台页**不能自己分配帧缓冲**，所以向首页场景借。动作包缩到 94%（451×395），是为了让最大一帧 RGB565A8 不超过 571 KB 槽。道具和读帧任务的 8 KB PSRAM 栈是自己分配的。

## 性能排查记录

1. 平铺图做背景加圆角面板，每次刷新约 400 ms。改为按刷新区域画小方点。
2. LVGL 软件混合 Nabo 太慢。改为在读帧任务里预合成，再用 direct blit 写屏。
3. 提示条每次状态更新都重设样式，导致重画。改为值变了才设。
4. 播放时 26 根音柱各自改高度，脏区超过 LVGL 的 32 个上限，触发整屏重画（250 ms，每秒 2–3 次）。改成单对象自绘后已在实机验证：每 10 秒 direct=80、平均渲染 7–10 ms，不再整屏重画。

## 临时调试清理（Codex 已完成）

交接时 `tab5_radio_stage.cc` 构造函数末尾的 `TEMP-DEBUG` / `RenderDbg` 慢渲染记录、`InvDbg` 整屏失效 backtrace，以及 `esp_debug_helpers.h`、`src/display/lv_display_private.h` 均已删除，已重新构建并刷机。结果见顶部接续记录。

## SD 卡

`/tab5/nabo/radio-v1/` 共 17 个文件（13 个动作 `.nab`、`mic.nab`、`mug.nab`、`wenkai_56.bin`、`manifest.json`），尺寸是 451×395，已逐个核对 SHA-256。生成命令见 board README 的“NABO FM 电台页”一节。源素材在 `https://github.com/Liutupi/ai-passport-nabo-xiaozhi-radio` 的 `design/nabo-radio/`。

## 台表原始快照（另一工作树）

`claude/nabo-fm-catalog` 已把 1,079 个 MP3 台做成 SD 卡上的 `catalog.bin`，按需读取。收藏迁移为按台 ID 存储，波段按地区和类型划分。宿主测试 190 项通过，原生构建通过。**未提交，`catalog.bin` 没拷进卡，没上机。** 这条分支和本分支都改了 `tab5_radio_page.cc` 和 `m5stack_tab5.cc`，合并时要处理冲突。建议先让本分支在实机验收、提交，再把台表分支 rebase 上来。

## 待办（按顺序）

1. 已完成：删掉临时调试代码（帧率已确认），重新构建并刷机。
2. 已完成：用户确认动作切换、刻度盘拖动和吸附、波段、反复收藏、点歌歌词及返回首页动画均正常。
3. 用户问过歌词位置：现在只有一行、约 30 px，在台名下方。用户可能想要更大的歌词区。
4. 已完成：台表改动已合入、`catalog.bin` 已写入 SD 并刷入最终版；用户确认全部台数、波段、拖动换台及收藏重启保存正常，最终首页 120 秒调度复测通过，代码本地提交。
5. 未做：定时关闭（`Doze` 动作目前不会触发）、台表抽屉和拼音搜台、夜间时钟。
