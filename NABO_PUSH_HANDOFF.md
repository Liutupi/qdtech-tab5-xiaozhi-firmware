# NABO 每日推送 交接（2026-10-22，Claude → Codex）

分支 `feat/nabo-push`，基于 v1.0.17（`9bdaf28`）。本机编译副本 `~/tab5-nabo-push`（ESP-IDF 6.1，`python3 scripts/build.py qdtech/tab5 --name qdtech-tab5-native`，之后可用 `cmake --build build` 增量编译）。不要在 `/Volumes/liutupi` 外接卷上编译：`._*` 文件会弄坏组件哈希。用户的 `/Volumes/liutupi/tab5` 是 10/3 的旧检出，里面有用户自己未提交的 `HANDOFF.md`、`scripts/push_tab5_daily.mjs` 改动，不要切分支、不要动这两个文件；只在那里同步本地分支引用。

设计和约定以 `docs/nabo-daily-push.md` 为准（Grok 也按它推送）。设计画布：claude.ai artifact `A3PTCP89f2p7Wz5Dp4x5HU`（v2 页）。

## 已完成

| 部分 | 状态 |
|---|---|
| 中转站 `tools/muse-relay/nabo_routes.js` + `enable_nabo.js` + 测试 | 完成；10/10 通过。**Grok 已部署到 NAS**，钩子排在电台扩展 `handleNabo` 之前，只认 `POST /nabo/<relay token>/push` 和 `GET /nabo/<relay token>/inbox`，其余放行。Grok 已推过真实内容（rev 10）。 |
| 数据层 `tab5_nabo_push.{h,cc}` | 完成；按 rev 增量同步，SD `/sdcard/tab5/nabo/push.json` 存 30 天历史和本地标记（已读/完成/学会/收藏/收藏的日子），往日回顾在设备上生成。宿主测试通过。 |
| 推送页 `tab5_nabo_push_page.{h,cc}`、`tab5_nabo_fortune_view.cc`、`tab5_nabo_ui.h` | 完成；替换了旧 Muse 消息列表（入口仍是应用页「NABO 每日推送」）。运势 9 个模块标签、学习、待办、回顾。已在宿主 LVGL 渲染核对。 |
| 拉取 `Inbox::PollNabo`（`tab5_muse_inbox.cc`） | 完成；随 Muse 收件箱每 60 秒拉一次，只在配置了隧道 URL 时工作（LAN-only 没有 token）。 |
| 腾 flash：SD 场景构建不再引用 375 KB 旧睡姿图（`tab5_native_display.h`） | 完成；`test_tab5_portrait_matte.py` 已补断言。 |
| **已刷机**：`1840c8a` 只写 factory，写后哈希通过 | 用户确认推送页显示正常、能收到推送。150 s + 300 s 串口无 panic/看门狗。备份：`/Volumes/liutupi/tab5-private-backups/nabo-push-20261022-224612/`（`factory-before.bin` = v1.0.17 `f4def716…`、`nvs-before.bin`、新镜像与 ELF、日志；私有，不得上传）。 |

## 本次最后一个提交（未刷机）

主页提醒 + 推送页左下角小 NABO，代码写完、编译通过（镜像 12,282,592 B，factory 余 300,320 B），**没有刷机，没有实机验证**：

- `tab5_nabo_remind.{h,cc}`：`PickReminder()` 按优先级取一条：待办开始前 15 分钟到过点 1 小时 → 未读高优先级推送 → 6:00–12:00 未读运势 → 20:00–23:00 没学完 → 周日 21:00 一周回顾。宿主测试 `test_home_reminder_priority_and_windows` 通过。
- `nabo_action_library.h` 加 `ActionLibrary::Request(id, now)`；`tab5_sd_scene.h` 加 `RequestAction()`：下一个中性帧优先播点播动作，60 秒内没播成就作废，阻塞条件与空闲动作相同。
- `tab5_native_display.h`：`UpdateNaboReminder()` 每秒在主页评估；有提醒时每日卡片换成提醒（橙色标题、隐藏轮播圆点），点卡片 → `apps_->OpenMuseBoard(board)` 并把这条记为已处理；没提醒时恢复轮播。动作插播：非 23:00–7:00、不在对话/播放/睡眠、同一条一次、两次间隔 ≥10 分钟。
- `tab5_native_apps.{h,cc}`：`OpenMuseBoard(int)`；推送页 `SelectBoard()`。
- 小 NABO：`assets/nabo/push-avatar-source.png` → `scripts/prepare_nabo_push_avatar.py` → `main/boards/qdtech/tab5/nabo_push_avatar.c`（134×172 RGB565A8，69 KB），推送页左下 208×208 卡片里上下浮动，下方小字跟随最新未读推送的 mood。

## 待办（按顺序）

1. **修 `scripts/tests/test_tab5_daily_mirror.py`**（当前唯一失败，共 202 项、3 项跳过）：它截取 `ShowDailyPage` 放进测试工具编译，现在函数里多了 `if (nabo_reminder_.active) { ShowNaboReminderCard(); return; }`。在测试工具的假类里补 `struct { bool active = false; } nabo_reminder_;` 和空的 `void ShowNaboReminderCard() {}` 即可；最好再加一条断言：`active=true` 时不改标题/正文、不排队镜像。另外核对一下跳过数从 1 变 3 是不是本机环境原因。
2. 跑完整宿主测试 `python3 -m unittest discover -s scripts/tests`，`git diff --check`。本机没装 `clang-format`，新文件未格式化。
3. 备份后只写 factory（端口与工牌共用：先 `esptool --after hard-reset chip-id` 确认 MAC `e8:f6:0a:e2:d7:e0`，**不要用 `--after no-reset`**），抓 150 s 串口。
4. 实机验证：推送页左下小 NABO 的浮动和文字；让 Grok 推一条 15 分钟后开始、`priority=high` 的待办，看主页卡片和 NABO 插播（日志 `nabo reminder … action A07`）；点卡片跳到待办板块；高优先级推送；早上运势。
5. 文档未覆盖的小项：高优先级提示音没做（设计里有，可选）；`/tab5/nabo/inbox` 局域网无 token 路由没做；没插 SD 卡时推送历史不保存。
6. 全部通过后按惯例发版（更新 `PROJECT_VER`、Release 产物、`HANDOFF.md`、验证记录）。
