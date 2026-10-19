# Tab5 板级目录

## Muse 电台播放

Muse 电台页分别提供“播放播客”和“播放推荐歌曲”。播客按钮只读取本期 `audio_url` 并播放原声 MP3；07:30 的文字节目尚未补上音频时，页面提示原声尚未上传。推荐歌曲按钮继续由 NAS 根据 `tracks` 的歌名和歌手获取网易云完整歌曲，按顺序播放，歌单点播、歌词和波形保留。

原生版每日节目页采用午夜琥珀配色，与网络电台共用一个 `Tab5RadioStage` 和同一套 SD 动画。左侧保留录音室，右侧上方在歌曲播放时显示歌名、歌手和 40 px 歌词，播放播客时恢复节目标题；长句可换行并手动纵向滚动。中部切换节目文稿和推荐歌单，右上“往期节目”打开历史列表，底部保留两种播放/停止按钮和状态。定时 LRC 的当前行也会送到每日节目页，关闭页面后仍缓存最新一行，再打开可立即显示；文稿仅在本期原声播放时跟随真实音频进度，手动滚动后仍暂停跟随 8 秒。

歌词字库 `qd_font_lyric_40.c` 只编入原生版，使用 2 bpp 抗锯齿、与动态文本字库相同的常用字范围，生僻字回退到页面持有的完整文本字库。无需往 SD 卡添加文件。使用 `scripts/generate_tab5_lyric_font.py <LXGWWenKai-Regular.ttf>` 和 `lv_font_conv 1.5.3` 重建；脚本校验源字体 SHA-256，许可证沿用本目录 `FONT_LICENSE_OFL.txt`。

云 VM 可在 07:35 生成完成后，按 [上传接口说明](../../../../tools/muse-relay/README.md) 给同一期补上 MP3。设备每分钟自动刷新节目，也检查已有编号的内容变化，无需重复发布节目。电台页右上角“刷新节目”可立即拉取，显示刷新中、完成或失败；失败时保留已有内容，不中断播放。节目原声播放取消尚未完成的歌曲解析请求，防止迟到的歌曲替换播客。

此目录在 XiaoZhi ESP32-P4 底座上加入原 QDTech S3 项目的桌面与服务，同时提供 Tab5 原生分辨率主界面。硬件入口是 `m5stack_tab5.cc`；`config.json` 包含 P4 rev 1.x 的旧桌面和原生版、P4 rev 3+ 的旧桌面。屏幕驱动支持 ILI9881C、ST7121、ST7123，触摸支持 GT911/ST712x。音频为 ES8388 + ES7210；SD 卡须在 C6 Wi‑Fi 的 SDIO host 初始化后使用 slot 0。

旧 `qdtech-tab5` 变体将 480 × 320 桌面以最近邻方式放在横屏中央，两侧留边。双线性全屏实验因刷新和触摸迟滞已从源码撤回。新 `qdtech-tab5-native` 变体在 `tab5_native_display.h` 里用 LVGL 原生 1280 × 720 坐标绘制 Nabo 桌面、对话和触摸按钮；局部刷新由 `esp_lvgl_port` 处理。点击主界面的“应用与设置”进入同一屏幕上的第二页，可重新配网、调节并保存亮度和音量、选择及播放网络电台；语音说“我要听广播”会打开电台页并请求播放。内置电台列表只保留经核对的不同频道，也可用 SD 卡根目录的 `radio.json` 替换。原生版“应用与设置”里现有红白机入口：将 iNES 格式的 `.nes` 文件放在 FAT 格式 SD 卡的 `/nes`、`/FC` 或 `/roms` 目录，打开游戏页后可浏览列表、启动游戏并使用屏幕按键。旧天气、相册、播客服务仍未接入原生版。中文子集由 LXGW WenKai 生成，重建脚本和 OFL 许可证见本目录及 `scripts/generate_tab5_fonts.py`。P4 rev 1.x 关闭了会在该芯片上触发非法指令的自动 ISP 管线控制器，相机设备本身仍能初始化取帧。后续按仓库根目录的 [PORTING_STATUS.md](../../../../PORTING_STATUS.md) 验收电台与其他应用。

Nabo 原画及生成说明见 [assets/nabo/README.md](../../../../assets/nabo/README.md)。固件内置主立绘、眨眼眼部覆盖图与三帧挥手姿态，SD 卡不是开机显示和日常动画的前提。图片按原尺寸呈现；待机眨眼、说话嘴型、人物周围的小光点和睡姿浮动文字只刷新局部。主页右侧上方在每日一句、历史上的今天和节日提醒之间轮播；节日、历史事件与短句共用 `tab5_daily_content.cc` 的离线数据。历史事件只显示已收录的日期，其他日期明确提示暂无收录；节日提醒支持当前日期和下一个节日倒计时。新增更多大幅动作时，先测量实机帧时间与触摸延迟，再考虑从 SD 卡加载可选动作包。

原生变体启用 `CONFIG_QDTECH_TAB5_PORTRAIT_MATTE_EXPERIMENT` 后，静态正面立绘改用预合成的 434 × 558 RGB565 资源，省去大面积透明混合；眨眼和嘴型仍用局部覆盖图。源文件是 `assets/nabo/portrait_matte_434x558_rgb565.bin`，可用 `scripts/prepare_nabo_portrait_matte.py` 从原画重新生成；构建时检查长度和 SHA-256。资源编入固件，不依赖 SD 卡。此优化只覆盖静态正面姿态，睡姿和其他动作沿用原资源。实机圆角边缘及动画观感仍需照片核对。

原生变体将空闲 Wi-Fi 策略设为 BALANCED（MIN_MODEM），以缩短家庭网络向设备推送指令时的唤醒等待；旧桌面变体维持原策略。P4 rev 1.x 实机安静待机各测 100 次、间隔 0.5 秒的局域网 ping：原 MAX_MODEM 中位数 482 ms、P95 919 ms，BALANCED 中位数 144 ms、P95 254 ms，两次均未丢包。该测量只代表该网络中的往返延迟，不等于语音、人物检测或 NAS 点歌的端到端时延；USB 电流与长期稳定性仍需实测。可在 `config.json` 中关闭 `CONFIG_QDTECH_TAB5_IDLE_BALANCED_WIFI_EXPERIMENT` 复原。

## NABO FM 电台页

原生电台页沿用 AI Passport 版 NABO FM 的“午夜琥珀”设计，按 1280 × 720 横屏重新排版：左侧是 Nabo 的直播间（ON AIR 灯、话筒、电平灯、旋钮），右侧是台名、提示或歌词条、波段标签、刻度盘和播放按钮。拖动刻度盘换台，松手吸附到最近的台，停手 0.5 秒才连接；刻度下方的左右邻台、上一台和下一台按钮都在当前波段内移动，到头后进入相邻波段。波段见下文“电台表”。页面代码在 `tab5_radio_page.cc`，直播间在 `tab5_radio_stage.cc`，Nabo 动作和电台状态的对应关系在 `nabo_radio_mood.h`（打开页面招手，开始播放戴耳机，连接中调台，音乐台点头和跟唱交替，谈话台听，停止摘耳机，连不上说哎呀，没网找信号，播放中每 60–120 秒喝一次咖啡，收藏比心）。

Nabo 的 13 个电台动作、话筒和杯子道具以及 56 px 霞鹜文楷台名字库放在 SD 卡 `/tab5/nabo/radio-v1/`，不占用 flash。动作为 451 × 395 的 NABOSD1 v3 包（LZ4 压缩的 RGB565A8，每帧裁到人物范围），循环动作按 8 fps 播放，一次性手势按约 16 fps 播放。独立任务借用隐藏首页的三个帧缓冲读帧，将人物、墙面和道具预合成为 RGB565 后直接写屏，离开电台页时归还缓冲。26 根音柱由一个控件统一绘制，避免大量脏区触发整屏重画。用 `scripts/prepare_nabo_radio_actions.py` 从 ai-passport-nabo-xiaozhi-radio 仓库 `design/nabo-radio/art/video/batch1/source` 的绿幕视频生成：

```sh
python3 scripts/prepare_nabo_radio_actions.py --clips <batch1/source> --props <art/props/source> \
    --font <fonts/LXGWWenKai-Regular.ttf> --catalog <catalog/stations.json> --output <新目录>/tab5/nabo/radio-v1
```

把输出的 `tab5` 文件夹合并复制到 SD 卡根目录。卡里没有这些文件时，直播间显示说明文字，电台照常可用；没有字库时台名改用普通文字字体。在线固件升级不会安装这些文件。2026-10-19 的实机日志已确认播放时 Nabo 接近 8 fps；用户确认动作、刻度拖动、波段、反复收藏、点歌歌词及返回首页动画均正常。直写屏即使在 DMA 立即完成时也至少让出 5 ms，后台天空和云层生成也按行分批让出 CPU，保留空闲任务运行机会；分批计算不改变画面像素。

### 电台表（SD 卡）

AI Passport 版的电台表 `design/nabo-radio/catalog/stations.json` 有 1,105 台。Tab5 只解 MP3，所以去掉 AAC 台、`.aac` 地址和只能在浏览器里播的 `health: "cors"` 台，剩 1,079 台。用下面的脚本转成约 97 KB 的二进制表，放到 SD 卡 `/tab5/nabo/radio-v1/catalog.bin`：

```sh
python3 scripts/prepare_nabo_radio_catalog.py <catalog/stations.json> --output <新目录>/catalog.bin
```

格式见 `nabo_radio_catalog.h`。开机只把头部、地区和类型表以及每台 16 字节的索引（共约 18 KB）一次读进 PSRAM；台名和地址在用到时从卡里读，最近 16 个台名有缓存。语音点台按台名或拼音首字母（如 `hbjtgb`）顺序扫一遍卡里的字符串区。卡里没有这个文件或校验失败时，退回 `/sdcard/radio.json`，再退回内置的 37 台。为了让大字号台名覆盖新台，生成字库时把同一个 `stations.json` 传给 `--catalog`；刻度下方的邻台名用 28 px 字体，其中 47 个台名有生僻字缺字。

波段依次是：收藏、最近（最近播放的 10 台）、本地（默认广东，NVS `radio_st/home` 可改）、新闻、音乐、交通、文艺、各省（按台数多的省份在前）、港澳·海外、全部。收藏和上次收听的台按稳定的台号保存（蜻蜓台用蜻蜓 ID，其余用地址哈希），换电台表后仍然对得上：收藏是 NVS `radio_fav/ids` 一条 blob，上次的台是 `radio_st/last_id`，最近列表是 `radio_st/recent`。旧固件按序号保存的 `radio_fav/fav_<n>` 和 `radio_st/last` 会在第一次启动时换算成台号；只有新收藏写入并提交成功后才删除旧收藏键，失败时保留旧键以便重试。新台表中没有的旧台号继续保存在收藏 blob 中，但该台暂时不会出现在当前波段。

2026-10-19 已在 P4 rev 1.3 上确认加载 1,079 台、35 个地区，常驻台表约 20 KB，37 个旧收藏记录迁移为 3 个收藏台。用户确认各波段、拖动换台和旧收藏正常，新增收藏重启后仍保留。SD 卡安装时对原有 17 个动作、道具和字库文件逐个校验，内容未变。

原生应用页的 ICU 数值工具及 Nabo 语音计算入口见 [ICU_CALCULATORS.md](ICU_CALCULATORS.md)。静脉泵输入中的 50 mL 指配好后的最终总液量；公式、单位与临床使用边界在该文档中列明。

## 耳机口与外放

3.5mm 输出由 ES8388 提供左右通道。应用层仍提供单声道 PCM，Tab5 codec 用固定 1 KiB 缓冲分块复制到左右 I2S 时隙，避免 ESP32-P4 的单通道配置只发送左声道。语音、播客、歌曲和 NES 两耳播放相同内容；此修复不恢复 MP3 在播放器中已混合的左右声道。Mega Drive 在独立升级程序中运行，不经过这里的音频驱动。

PI4IOE1（0x43）的 P7 为插头检测，插入时为高；P1 控制扬声器功放。开机先关闭功放，后台任务每 100 ms 采样，连续三次一致后切换：插入插头关闭外放，拔出恢复外放。I2C 失败保留已确认路由并重试，输出寄存器修改加锁，保留屏幕、触摸、相机复位和外部 5V 控制位。硬件接线见 [官方原理图](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1132/Tab5_Schematics_PDF.pdf)。

CM403 等蓝牙发射器通过模拟耳机口取音，Tab5 只能检测插头，无法获知发射器与蓝牙耳机的连接状态。发射器仍插着时，即使耳机关机，外放也保持关闭。设备验证需覆盖已插入时开机、播放中插拔、双耳出声、语音唤醒/打断、麦克风/AEC、重连，以及无插头时的外放恢复；编译与宿主测试不能替代这些实测。

2026-10-15 已在 P4 rev 1.3 原生版上用 CM403 和蓝牙耳机验证，用户确认“双耳都有声音，插拔外放正常”。唤醒/VAD/AEC、语音打断、重连和长时间运行仍需扩展验收。

## 局域网每日卡片

原生版在局域网 8080 端口提供 `/ws` WebSocket，仅接受 MCP `tools/call` 的 `self.daily.set_cards` 指令，单帧上限 2 KiB。未连接家庭 Wi-Fi 或进入配网热点时会拒绝使用。该接口仍没有身份验证，只应在可信网络使用，也不应通过路由器端口映射暴露到互联网。`scripts/push_tab5_daily.mjs` 可投递三条当日医学卡片。

## 局域网点歌推送

Tab5 在已连入家庭 Wi-Fi 时于 UDP 45678 端口接收 `play_url` JSON，配网热点期间拒绝处理。发件端应只传 `type`、`title`、`artist`、可直接播放的 `url`，以及可选的网易云十进制 `song_id`。收到歌曲 ID 后，Tab5 会先播放，再在后台按 ID 获取带时间戳的歌词；没有 ID 时按歌名和歌手查找。原有 `lyrics_json` 字段仍兼容，但不适合传整首歌词。

P4 + C6 Wi-Fi 实测总报文 1284 字节可到达，1534 字节及以上未到达；NAS 发送端应把整个 UTF-8 JSON 控制在 1200 字节以内。超过此大小的直链应改用设备 MCP 工具，不能依赖 UDP 分片。UDP 还可能在瞬时连续发送时丢包：一次发送一首歌，避免把整首歌词或多首歌同时塞进推送；对必须可靠送达的控制，优先使用已有 MCP 会话。

## 跨网段网易云点歌

v1.0.7 增加 Muse relay 音乐命令通道：NAS 上的网易云 MCP 容器将 `title`、`artist`、`url`、准确的 `song_id` 和 `continuous` 以 JSON `POST` 到同机 relay 的 `/tab5/music`；Tab5 使用已配置的 Muse inbox 地址，每约 4 秒读取 `/music/<token>`，按命令 ID 去重后播放。relay 代码见 `tools/muse-relay/relay.js`。`/tab5/music` 仅接受本机及已配置的容器网关来源，不能通过公网隧道写入；不同 Docker 网络应相应调整允许的来源。这个通道用于 NAS 和 Tab5 不在同一网段、UDP 无法到达设备的场景。公开仓库未包含用户 NAS 的网易云 MCP 容器脚本；部署时需在该容器的播放结果处理处发送上述 JSON，并确保传递真实歌曲 ID。设备端没有 ID 时仍按歌名和歌手查词，有 ID 但歌曲无定时歌词时不会拿同名其他版本的歌词替代。

连续播放只能由一侧负责。Tab5 收到 `continuous=true` 后，会在歌曲自然结束时通过小智请求下一首；NAS 网易云 MCP 若也保存私有电台的自动播放状态并按歌曲时长启动定时器，会在设备待机时自行把新歌推到 relay。接入此通道时，应关闭 **Tab5 对应账号** 的 NAS 后台定时续播和进程重启续播，保留 Tab5 的自然结束续播；其他账号可维持自己的设置。NAS 脚本不在本仓库，修改和回退路径见 `HANDOFF.md`。

### HTTPS 接收线程的内存

原生变体使用 `CONFIG_LV_USE_CUSTOM_MALLOC` 和 `tab5_lvgl_memory.cc`，将 LVGL
对象、样式、文字和图像统一分配到 PSRAM，包括小于 1KB 的分配与缩小后的
`realloc`。默认 C 库分配策略会把这类小对象放入内部 SRAM；首次打开电台和
米家中控时会挤占网络同步对象与任务的内存。此分配器不改变其他组件的全局
`malloc` 策略，不使用内部 RAM 兜底。仍需实机验证页面切换、刷新、音频和动画。

原生变体启用 `CONFIG_ESP_ML307_SSL_RX_STACK_IN_PSRAM`，将每个 TLS 接收线程的 4KB 栈分配到 PSRAM，避免界面运行后内部 SRAM 碎片化导致 Muse 音乐命令轮询无法建立连接。线程控制块仍在内部 RAM。该选项默认关闭，旧桌面和 P4 rev 3+ 变体沿用内部栈。

接收线程结束后由连接管理者使用 `vTaskDeleteWithCaps` 回收，避免线程自行删除时额外申请内部清理栈。HTTP 接收回调只交付数据，协议/MCP 副作用调度到主任务；新增回调不得直接在外部栈上写 Flash。构建成功后仍须实机检查语音点歌、停止/续播、重连、唤醒和长时间运行。

### Muse 节目文案随播放滚动

Native 电台页支持 relay 返回的 `transcript`（与 `audio_filename` 绑定的有序字幕）。
旁白段高亮全文、滚动到段落开头；音乐片段显示歌曲标题和歌手。
计时取自 `RadioService` 写入扬声器的 PCM 帧数，缓冲不前进，重播或连接重启归零。
播放推荐歌曲、切换节目或换了音频时不继续使用旧字幕。
手动拖动文案后暂停自动滚动 8 秒；没有字幕的老节目保留全文显示。

字幕来自实际生成音频的时间轴，不按字数估算。Muse 可随 MP3 使用
`upload_audio.py --speech-optimize --transcript xxx.timeline.json` 上传，或单独
`POST /podcast/transcript?episode_id=...`。接口、JSON/SRT/VTT 格式和限制见
[relay 接入说明](../../../../tools/muse-relay/README.md)。

## MD 与 NES 手柄游戏

原生版游戏页共用游戏列表，左右切换 FC/NES 和 MD。进入页面只扫描和显示列表，选择游戏后才开始运行。MD 从 `/roms/md/catalog.tsv` 读取名称，运行于现有 `ota_0` 升级程序；须同时刷入主程序和本次发布的 `tab5_updater` 1.2.2（含 MD 控制/声音及内存、启动修复）或更新版本。在线升级只更新主程序，旧升级程序须通过 USB 写入 `0xc20000`。保留 12MB 主程序、1MB `ota_0` 和现有 NVS/资产分区。

- USB HID / Sony DS4 和 Xbox360/SN30 X-input 通过同一驱动接收；X-input 使用独立的 vendor 接口轮询，不抢占通用 HID 接口。
- MD 采用三键手柄：四个方向、Start，面键左/下/右分别为 A/B/C。Select+Start 返回主程序；左上角 `EXIT` 也可触摸返回，不再在90秒后自动退出。
- MD 的 FM 与 PSG 音频混合后经 ES8388 播放，使用现有音量设置。支持 PAL/NTSC 的有界音频缓冲；声音输出任务与模拟主循环分开。
- NES 保留原按键映射，Select+Start 返回游戏列表，右上角“退出游戏”在直接输出画面时仍可触摸。列表再按 Select 返回应用。快速退出后重新启动会等旧模拟器结束再接续。音频经三帧有界队列交给独立任务，避免 DAC 写入阻塞主循环，使自动跳帧长期停留在追帧状态。

构建主程序用 `python3 scripts/build.py qdtech/tab5 --name qdtech-tab5-native`；MD 程序使用同一 ESP-IDF 6.1 环境下的 `idf.py -C updater build`。两个镜像分别属于 `factory` 和 `ota_0`，不能写到同一个分区。USB 手柄插拔、实际声音、两个模拟器反复进入/退出须用硬件验证。
