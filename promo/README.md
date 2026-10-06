# Tab5 × Nabo 宣传片(定稿)

成片:`tab5_nabo_promo.mp4`(1920×1080,30 fps,76 秒)。

## 画面
- 设备屏幕上的每一帧都是**真实固件界面**:在电脑上用 LVGL 9.5 编译运行 `main/boards/qdtech/tab5/` 的原生界面代码
  (主页、应用页、Muse 电台、网络电台、ICU、红外、Muse 推送、SD 半身窗口场景),使用固件自带字体与 Nabo 资源,
  以及设备从 SD 卡加载的 Noto common 30 字体。硬件接口由 `hostui/shim`、`hostui/src/stubs.cc` 替身。
- Nabo 的睡觉、入睡/醒来、待机、看手机动画来自 Release `sd-nabo-v1` 的四个 `.nab` 动画包,由固件自己的 SD 场景读取器播放。
- 界面事件(点击、语音状态、播客、点歌、电台、ICU 输入等)由 `timeline.py` 生成的 `film.txt` 驱动。

## 声音
- 开场问候:设备原声 `assets/nabo/greeting.ogg`。
- Muse 电台段:2026-10-06 那期每日音乐电台的原声(阿南开场 + 《几页诗》引子),屏幕文案与歌单来自同期 `audio/radio-2026-10-06.timeline.json`,播放位置驱动逐句高亮。
- 网络电台段:6 秒真实广播录音。
- 不含 AI 合成人声;对话段只有屏幕文字和片中字幕。
- 配乐 `music.py`:120 BPM,GeneralUser GS 采样(允许商用)+ 合成层,pedalboard 混音。

## 未提交的私有素材
`audio/*.mp3`(私人节目与录音)和 `photos/official_*.png`(M5Stack 官方产品图)不在仓库中,重新生成前需自行放回。

## 重新生成
1. `hostui/`:克隆 `lvgl/lvgl`(v9.5.0)和 `78/xiaozhi-fonts`,放入 `font_noto_sans_common_30_4.bin`,`make liblvgl.a && bash build.sh`;
   把四个 `.nab` 放到 `/sdcard/tab5/nabo/`。
2. `python3 timeline.py <promo> <promo/data>`
3. `python3 music.py <GeneralUser-GS.sf2> ../assets/nabo/greeting.ogg <promo>`(生成 `music.wav` 与界面用的 `level.txt`)
4. 在 hostui 中:`./harness <promo>/film.txt 76500 film.rgb .`
5. `python3 compose.py film.rgb film.txt video.mp4`,再用 ffmpeg 合并 `music.wav`。
字体:思源黑体 Noto Sans CJK SC(OFL)与 Inter。
