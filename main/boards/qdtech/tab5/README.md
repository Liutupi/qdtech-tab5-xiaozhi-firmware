# Tab5 板级目录

此目录在 XiaoZhi ESP32-P4 底座上加入原 QDTech S3 项目的桌面与服务，同时提供 Tab5 原生分辨率主界面。硬件入口是 `m5stack_tab5.cc`；`config.json` 包含 P4 rev 1.x 的旧桌面和原生版、P4 rev 3+ 的旧桌面。屏幕驱动支持 ILI9881C、ST7121、ST7123，触摸支持 GT911/ST712x。音频为 ES8388 + ES7210；SD 卡须在 C6 Wi‑Fi 的 SDIO host 初始化后使用 slot 0。

旧 `qdtech-tab5` 变体将 480 × 320 桌面以最近邻方式放在横屏中央，两侧留边。双线性全屏实验因刷新和触摸迟滞已从源码撤回。新 `qdtech-tab5-native` 变体在 `tab5_native_display.h` 里用 LVGL 原生 1280 × 720 坐标绘制 Nabo 桌面、对话和触摸按钮；局部刷新由 `esp_lvgl_port` 处理。点击主界面的“应用与设置”进入同一屏幕上的第二页，可重新配网、调节并保存亮度和音量、选择及播放网络电台；语音说“我要听广播”会打开电台页并请求播放。内置电台列表只保留经核对的不同频道，也可用 SD 卡根目录的 `radio.json` 替换。原生版“应用与设置”里现有红白机入口：将 iNES 格式的 `.nes` 文件放在 FAT 格式 SD 卡的 `/nes`、`/FC` 或 `/roms` 目录，打开游戏页后可浏览列表、启动游戏并使用屏幕按键。旧天气、相册、播客服务仍未接入原生版。中文子集由 LXGW WenKai 生成，重建脚本和 OFL 许可证见本目录及 `scripts/generate_tab5_fonts.py`。P4 rev 1.x 关闭了会在该芯片上触发非法指令的自动 ISP 管线控制器，相机设备本身仍能初始化取帧。后续按仓库根目录的 [PORTING_STATUS.md](../../../../PORTING_STATUS.md) 验收电台与其他应用。

Nabo 原画及生成说明见 [assets/nabo/README.md](../../../../assets/nabo/README.md)。固件内置主立绘、眨眼眼部覆盖图与三帧挥手姿态，SD 卡不是开机显示和日常动画的前提。图片按原尺寸呈现；待机眨眼、说话嘴型、人物周围的小光点和睡姿浮动文字只刷新局部。主页右侧上方在每日一句、历史上的今天和节日提醒之间轮播；节日、历史事件与短句共用 `tab5_daily_content.cc` 的离线数据。历史事件只显示已收录的日期，其他日期明确提示暂无收录；节日提醒支持当前日期和下一个节日倒计时。新增更多大幅动作时，先测量实机帧时间与触摸延迟，再考虑从 SD 卡加载可选动作包。

原生变体启用 `CONFIG_QDTECH_TAB5_PORTRAIT_MATTE_EXPERIMENT` 后，静态正面立绘改用预合成的 434 × 558 RGB565 资源，省去大面积透明混合；眨眼和嘴型仍用局部覆盖图。源文件是 `assets/nabo/portrait_matte_434x558_rgb565.bin`，可用 `scripts/prepare_nabo_portrait_matte.py` 从原画重新生成；构建时检查长度和 SHA-256。资源编入固件，不依赖 SD 卡。此优化只覆盖静态正面姿态，睡姿和其他动作沿用原资源。实机圆角边缘及动画观感仍需照片核对。

原生变体将空闲 Wi-Fi 策略设为 BALANCED（MIN_MODEM），以缩短家庭网络向设备推送指令时的唤醒等待；旧桌面变体维持原策略。P4 rev 1.x 实机安静待机各测 100 次、间隔 0.5 秒的局域网 ping：原 MAX_MODEM 中位数 482 ms、P95 919 ms，BALANCED 中位数 144 ms、P95 254 ms，两次均未丢包。该测量只代表该网络中的往返延迟，不等于语音、人物检测或 NAS 点歌的端到端时延；USB 电流与长期稳定性仍需实测。可在 `config.json` 中关闭 `CONFIG_QDTECH_TAB5_IDLE_BALANCED_WIFI_EXPERIMENT` 复原。

原生应用页的 ICU 数值工具及 Nabo 语音计算入口见 [ICU_CALCULATORS.md](ICU_CALCULATORS.md)。静脉泵输入中的 50 mL 指配好后的最终总液量；公式、单位与临床使用边界在该文档中列明。

## 局域网每日卡片

原生版在局域网 8080 端口提供 `/ws` WebSocket，仅接受 MCP `tools/call` 的 `self.daily.set_cards` 指令，单帧上限 2 KiB。未连接家庭 Wi-Fi 或进入配网热点时会拒绝使用。该接口仍没有身份验证，只应在可信网络使用，也不应通过路由器端口映射暴露到互联网。`scripts/push_tab5_daily.mjs` 可投递三条当日医学卡片。

## 局域网点歌推送

Tab5 在已连入家庭 Wi-Fi 时于 UDP 45678 端口接收 `play_url` JSON，配网热点期间拒绝处理。发件端应只传 `type`、`title`、`artist`、可直接播放的 `url`，以及可选的网易云十进制 `song_id`。收到歌曲 ID 后，Tab5 会先播放，再在后台按 ID 获取带时间戳的歌词；没有 ID 时按歌名和歌手查找。原有 `lyrics_json` 字段仍兼容，但不适合传整首歌词。

P4 + C6 Wi-Fi 实测总报文 1284 字节可到达，1534 字节及以上未到达；NAS 发送端应把整个 UTF-8 JSON 控制在 1200 字节以内。超过此大小的直链应改用设备 MCP 工具，不能依赖 UDP 分片。UDP 还可能在瞬时连续发送时丢包：一次发送一首歌，避免把整首歌词或多首歌同时塞进推送；对必须可靠送达的控制，优先使用已有 MCP 会话。

## 跨网段网易云点歌

v1.0.7 增加 Muse relay 音乐命令通道：NAS 上的网易云 MCP 容器将 `title`、`artist`、`url`、准确的 `song_id` 和 `continuous` 以 JSON `POST` 到同机 relay 的 `/tab5/music`；Tab5 使用已配置的 Muse inbox 地址，每约 4 秒读取 `/music/<token>`，按命令 ID 去重后播放。relay 代码见 `tools/muse-relay/relay.js`。`/tab5/music` 仅接受本机及已配置的容器网关来源，不能通过公网隧道写入；不同 Docker 网络应相应调整允许的来源。这个通道用于 NAS 和 Tab5 不在同一网段、UDP 无法到达设备的场景。公开仓库未包含用户 NAS 的网易云 MCP 容器脚本；部署时需在该容器的播放结果处理处发送上述 JSON，并确保传递真实歌曲 ID。设备端没有 ID 时仍按歌名和歌手查词，有 ID 但歌曲无定时歌词时不会拿同名其他版本的歌词替代。

连续播放只能由一侧负责。Tab5 收到 `continuous=true` 后，会在歌曲自然结束时通过小智请求下一首；NAS 网易云 MCP 若也保存私有电台的自动播放状态并按歌曲时长启动定时器，会在设备待机时自行把新歌推到 relay。接入此通道时，应关闭 **Tab5 对应账号** 的 NAS 后台定时续播和进程重启续播，保留 Tab5 的自然结束续播；其他账号可维持自己的设置。NAS 脚本不在本仓库，修改和回退路径见 `HANDOFF.md`。
