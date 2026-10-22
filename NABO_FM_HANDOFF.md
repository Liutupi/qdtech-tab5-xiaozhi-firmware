## 2026-10-20 v1.0.16 发布交接

## 2026-10-22 v1.0.17：每日推荐重启修复

匹配设备 ELF 的 panic PC 定位为 `PsramAllocator<char>::allocate`，调用来自 `Inbox::FetchPodcasts`：音乐环形缓冲已占用 PSRAM，后台再次复制完整节目文案时分配失败并 abort。修复将完整刷新延后到停播且内存充足时，保留待刷新标记、旧缓存、音乐命令及歌曲解析；下载后和复制前再次检查，提前释放响应并减少临时复制。

基线包含用户此前已选的 26 个装饰动作与原有 4 个状态片段，本轮保持其源文件及 SD 内容。201 项宿主测试已验证通过，SDK 6.1 原生版构建成功。同功能修复版已仅写 factory `0x20000` 并通过 hash；串口记录 162.5 秒连续播放及 39 行歌词，无新的 panic/重启，用户回复“正常了”。本轮日志未独立捕捉切歌/暂停/停止，不扩大验收结论。

正式版本号镜像、启动结果、哈希及剩余测试范围统一记在 [v1.0.17 验证记录](docs/tab5-v1.0.17-validation.json)。完整原机 Flash、NVS、串口原始日志只在私有备份保存，不得上传；公开发布仅使用干净构建产物与去除本机路径的 SD 资源清单。升级程序仍为 1.2.2，在线升级只安装主程序，新增动作需另行合并 SD 包。

- NAS `stream96` 已由 Grok 部署，桌面交接文件 `/Users/tupi/Desktop/RELAY_STREAM96_HANDOFF.md`。现有 token 不变；ffmpeg 放在挂载卷 `/data/ffroot`，由 `/data/bin/ffmpeg` 包装调用，固定公网地址来自 `publicBase()`。NAS 模块接口与固件兼容，既有 `/track`、`/song`、节目和上传接口保留。
- Codex 独立线上复核播客、歌曲与实际直播：均为 MP3/96 kbps/44.1 kHz/2ch；文件 HEAD 200、非零 Range 206 字节一致、越界 416。暂停继续沿用最初的源 URL，因此命中同一转码缓存；没有重新解析过期链接导致换文件。
- 正式 v1.0.16、ESP-IDF 6.1 镜像已刷入 P4 rev 1.3，**仅写 factory 0x20000**，写后 hash 通过。镜像 12,550,400 字节，factory 余 32,512 字节，SHA-256 `2be76b9b4c2bf6b6d512085f28633cd7f72a25b75892ab150ebef4540ee795aa`；编译源码内容对应 `3d8a7345fe2ee8c693e6e8520a00f8229d508041`。
- 约 8 分钟日志覆盖 7 次文件打开、播客和两首歌曲的 3 次 Range 恢复（偏移 244193、234475、105326 字节）、独立停止/切曲、32 秒左右交替测试音完整解码、1 次共享帧池归还。台表加载 1,079 台、已有歌曲收藏加载 5 首。未见新的 panic、断言、看门狗或 brownout。大多数动画窗口每约 10 秒 75–83 帧；5 个 64 帧进度间隔超过 1.8 秒，最大 2.741 秒，不能据此声称弱网或长期完全不卡。
- 用户确认暂停续播、停止、换曲、歌词及动画“可以，都正常”，并授权先更新发布、后续问题再调试。**现场只有内置扬声器，未完成耳机/外置音箱的左右分离实听；最后的语音收尾测试按用户要求延期。** 软件与 NAS 的非对称左右测试通过，不能替代这些外部听音和语音硬件验收。双外置音箱配对/同步未实现；设备仍适配原有 24 kHz I2S 时钟，44.1 kHz 指音源。
- 200 项宿主测试、4 项本地 relay 集成测试、SDK 6.1 原生及 SDK 6.0.2 旧桌面构建均通过。正式证据见 `docs/tab5-v1.0.16-validation.json`。升级前必须部署 `/stream96` 并配置 Muse 地址；在线升级不装 SD 资源，新录音室和 catalog 需本版 SD 包，保留卡上私人内容及 favorite-songs.bin。
- 私有本阶段备份：`/Volumes/liutupi/tab5-private-backups/nabo-stereo-device-20261020-005436/`，含正式 bin/elf/map、刷机日志、运行日志与验收记录，不得上传。上一版可回退镜像仍在 `nabo-song-favorites-20261019-233833/`（`ca754f72…`）。NAS 测试音为单独文件，没有改节目列表。后续优先补外接左右实听、语音/唤醒/打断、弱网续播和长时运行；不要再重复询问已经通过的日常播放及页面验收。

以下为过程记录，涉及“未发布/候选”的历史状态以本节为准。

## 2026-10-20 v1.0.16 追加：96k 全源、真实左右声道与暂停续播（候选，未刷机/未发布）

- 用户在要求更新 GitHub/发布后追加：所有电台、每日播客、推荐和收藏歌曲统一为 MP3 CBR 96 kbps / 44.1 kHz / 双声道；加入真实左右声道后才发布，并增加播客暂停/继续与独立停止。原先“主持人讲话断续”复测已连续顺畅，166 条连续解码进度未见耗尽或停顿；没有把一次暂时消失的症状宣称为已定位修复。
- NAS 部署由用户交给 **Grok**，Codex 负责交付接口文件。可直接交付的包为 `/Users/tupi/Desktop/tab5-stream96-for-grok.zip`，同名目录包含 `stream96_routes.js`、`enable_stream96.js`、测试、部署说明、接口约定和校验文件。仓库实现位于 `tools/muse-relay/`，接口约定为 `docs/relay-audio96-api.md`。挂载工具先备份 NAS 当前 relay.js，只插入初始化和入口，不覆盖已有 song/track/上传/转录代码。**用户只答应交给 Grok，尚未确认部署成功，不能当作已上线。**
- 配套路径：LAN `/tab5/podcast/stream96?url=<编码源URL>&live=0|1`；公网 `/podcast/<现有 token>/stream96?...`。直接输出音频，文件先完整转码缓存、提供 HEAD/Range 206；直播实时转码并在断开后清理进程。默认两项并发、512 MiB 缓存、单输出 128 MiB、闲置保留至少 24 小时。服务准备中返回 503，固件最多等待 15 分钟，不绕过到原始音源。源输入、DNS/跳转、子进程与内存/磁盘边界见部署说明。
- 原生固件将全部四类来源送至该接口，并校验解码帧为 96k/44100/2ch。`OutputStereoFrames` 将交错 L/R 直接交给 ES8388，不再下混成单声道；原有语音 mono Write 路径保持兼容。左右共用增益，重采样保留两路及分数相位，避免长时间累计截断。注意：44.1 kHz 是统一音源采样率，设备目前仍适配原有 24 kHz 全双工 I2S 时钟；本次没有实现两台外置音箱的配对与同步。
- 用户暂停不更换音源 generation：保存已送入 I2S 的音频时间和 MP3 帧边界，关闭下载、释放外部音频占用；继续用 Range 从已知帧边界预热解码器，跳过已经听过的帧。独立停止/切源会使旧书签失效。直播继续接当前直播。歌词改用实际输出时钟，缓冲和暂停期间均不走时；准备/重连期间按钮仍能暂停，不会重新提交从头播放。
- 每日节目页底部改为“暂停播客/继续播客”、“暂停/继续推荐或收藏歌曲”，保留独立“停止”。实际 LVGL 渲染/点击已检查暂停、继续、连接期间再按、歌单、收藏、往期、刷新失败及长歌词；新的暂停截图在本任务视觉目录 `nabo-stereo-paused.png`。
- **200 项完整宿主测试通过，无跳过**；桌面交付包内 **4 项真实 Node/ffmpeg 集成测试通过**，覆盖逐帧 CBR 格式、左右 440/880 Hz 分离、并发文件缓存、HEAD/两处 Range 字节一致、preparing 后完成、直播断开释放、认证/IP 边界及安装备份/重复运行。宿主 Helix 用临时副本的等价整数运算代替 CPU 汇编，在多个暂停点验证恢复后的全部 PCM 与连续解码逐帧相同，没有修改 vendor 文件。
- ESP-IDF 6.1 原生候选构建通过，目录 `/Users/tupi/.cache/tab5-release-v1.0.16/source/build/`；镜像 **12,550,400 字节**，12 MiB factory 余 **32,512 字节**，SHA-256 `2be76b9b4c2bf6b6d512085f28633cd7f72a25b75892ab150ebef4540ee795aa`。ESP-IDF 6.0.2 旧桌面兼容构建也通过。改动行/新头文件格式检查与 diff 检查通过；1 MiB ota_0 仍为独立升级器，未改变布局。
- **设备仍是上一阶段已验收的收藏/48px 歌词版**，SHA-256 `ca754f7203a1031659db22f972d221c2d61f6880300a7496339ebffcf6d1c48f`。不能直接发布先前缓存的 v1.0.16 基线镜像（它没有本阶段改动），也不能拿工作树早前的 6.0.2 build 当作最新候选。现阶段没有刷设备、推 GitHub、打标签或发布 Release。
- 后续依赖：Grok 确认部署，核对实际公网/LAN、播客/歌曲 Range/CBR、真实直播及清理；再刷入最终 6.1 候选，实测左右分离、暂停较长时间后继续、独立停止/新曲、歌词/文稿、语音打断/恢复、录音/唤醒及适用 AEC、弱网重连和首页动画。全部通过后完成 v1.0.16 文档、精确源码构建与产物校验，再更新 GitHub 和发布。

## 2026-10-19 每日节目页后续：收藏歌单与大字歌词（实机验收通过）

- 已按用户反馈将中部入口改成三个固定宽度的单行标签：节目文稿 / 推荐歌单 / 收藏歌单。移除标签中的数量，避免“推荐歌单”换行截断；往期节目仍从右上进入。推荐歌曲右侧有独立收藏按钮，收藏歌单支持点播和取消收藏，点收藏不会顺带开始播放。
- 内置歌词字号从 40 px 改为 48 px（1 bpp 文楷子集，保留全文字体 owner 与回退），长句仍可换行及纵向滚动；SD 动画不变，继续共用唯一录音室与帧池。
- 新增 `nabo_song_favorites.h` / `tab5_song_favorites.{h,cc}`：SD `/tab5/nabo/favorite-songs.bin`，最多 100 首，按歌名和歌手去重，仅保存 `{title,artist,song_id}`，未知编号可为空。文件有版本头、长度限制、CRC、UTF-8/数字编号校验；专用 PSRAM 工作任务通过 fsync 临时文件及备份替换保存，支持中断恢复，最新写入失败会回滚界面到上次保存的内容。
- NAS `/song` 接口已由用户安排 Grok 部署，权威说明为 `/Users/tupi/Desktop/RELAY_SONG_API.md`（2026-10-19）。不要再 SSH 修改或部署 relay；本次固件仅使用现有接口。公网 `/podcast/<现有 token>/song`、局域网 `/tab5/podcast/song`；URL 编码 title/artist/可选 id，每次点播现取链接，复用现有播放器与歌词。推荐点播的 `/track` 结果会补存已知编号；404 提示“暂不可播”，`is_trial` 在歌名旁标明“仅试听”，不自动接受其他候选版本。程序有请求序号及音源 generation 双重过期检查。
- 已实测 NAS 正常解析、音频 HTTP Range 206/audio-mpeg、ID 不匹配的 404 `id_mismatch` 及缺少标题的 400 `bad_request`。验证输出不包含 token 或音源链接。
- 实际 LVGL 宿主渲染已核对文稿、推荐歌单、收藏、往期及空状态；点击检查覆盖收藏/取消不误播、收藏点播、试听提示随歌词更新保留、播客、刷新失败及长歌词滚动。完整宿主测试 **196 项全部通过、无跳过**；原生及旧桌面 ESP-IDF v6.0.2 构建通过，改动行格式检查、新文件格式检查及 `git diff --check` 通过。原生镜像 12,516,000 字节，12 MiB factory 剩余 66,912 字节，SHA-256 `ca754f7203a1031659db22f972d221c2d61f6880300a7496339ebffcf6d1c48f`。1 MiB `ota_0` 为既有独立升级器，构建工具对此的尺寸警告与本次目标 factory 分区无关。
- 本阶段备份在私有 `/Volumes/liutupi/tab5-private-backups/nabo-song-favorites-20261019-233833/`，含上一版已验收镜像、候选 bin/elf/map、源码快照、接口文档及检查记录，不得上传。已只写 factory 应用程序并通过 esptool 写入 hash 校验。完成 **360 秒**串口观察：`SongFavorites: load ok songs=0`、`save ok songs=1`，两首推荐歌曲解析成功、节目原声正常启动，5 次 `pool returned`；未见 panic、断言、看门狗、brownout。动画普通窗口大多约 71–81 帧/10 秒，切页窗口有较低计数。
- 用户随后明确确认：“都正常，收藏重启后还在，也能正常点播，歌词大小合适。”本阶段实机验收通过，涵盖排版、收藏操作、重启保留及收藏点播；重启保存和收藏点播以用户实际操作反馈为依据，串口观察段未覆盖这两项操作。2026-10-19 用户要求更新 GitHub 并发布新固件，接续发布为 v1.0.16。

# NABO FM 电台页交接（2026-10-19，Claude → Codex）

## Codex 接续记录（2026-10-19）

### NABO 每日节目页重排

- 用户选择第二张效果图，并确认右上大标题区域改为显示实时大字歌词。`NABO 电台` 每日节目页已改成同一午夜琥珀风格：左录音室、右侧歌词/节目标题，文稿和推荐歌单共用宽阅读区，往期入口位于右上，双播放/停止按钮固定在底部。播放播客时恢复节目标题；原声、NAS 全曲点播、顺序歌单、定时歌词、文稿音频时钟、手动滚动 8 秒和刷新失败保留内容继续有效。
- FM 和每日节目页现在共用一个 `Tab5RadioStage` 对象、reader 和首页借出的帧池，切页仅 reparent；离开两页后归还缓冲。复用 SD 卡现有 `radio-v1` 动作和道具，未写 SD 卡、NVS、分区表或升级器。大字歌词内置为 40 px、2 bpp 文楷常用字库，生成脚本为 `scripts/generate_tab5_lyric_font.py`，只编入原生版；页面持有完整字体回退的 owner。
- 修复每日节目页原先未收到定时 LRC 当前行的问题：`SetMusicLyricsWindow` 统一转发并缓存最后一行，首次打开或从别页返回可以立即显示当前歌词。播放播客时不会把音频状态提示当作歌曲歌词展示。
- **193 项完整宿主测试通过、无跳过**；最终 UI 改动另跑 4 项播客测试通过。新增真实处理函数测试覆盖两页间动画 lease、隐藏调台取消和旁白/音乐动作；已有文稿测试扩展了隐藏页延后滚动。原生和旧桌面构建均通过；实际 LVGL 渲染和点击验证歌词、播客、歌单、历史、刷新失败、空内容和长歌词。
- 已仅向 factory `0x20000` 刷入新版并通过写后校验。原生镜像 **12,496,640 字节**，12 MiB factory 余 **86,272 字节**；SHA-256 **`6f7bcb0d0a9c88ef2413738270562ed070a59b48f1a415b316555cc8d63858d6`**。后续增加内置资源时必须先检查分区余量；`ota_0` 的过小提示仍是现有 1 MiB 升级器。
- 本阶段源码快照、效果图、实际渲染图、bin/elf/map、验证记录和实机日志在私有 `/Volumes/liutupi/tab5-private-backups/nabo-podcast-redesign-20261019-230317/`；前一份已验收固件仍在 `nabo-fm-catalog-20261019-220035/xiaozhi.bin`（`6af54113…`）。均不得上传。
- 已完成 **360 秒**实机日志观察：13/13 动作包识别、帧池借用、两期节目原声、歌单第 1/2 首点播，以及 54 行/29 行定时歌词加载；动画窗口每约 10 秒 direct=72–85、lvgl=0，后段多数为 78–81。无 panic、断言、看门狗、brownout 或直写屏停用。观察段尚未出现离开节目页后的 `pool returned`；用户随后确认显示、声音和动画“都正常”，并提出推荐歌单标签换行、增加收藏歌曲及放大歌词的新要求。首版源码已本地提交 `06d8559`，未推送。


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
