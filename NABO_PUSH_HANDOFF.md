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

## Codex 接续记录（2026-10-22 23:26，上海时间）

以上待办 1–3 已完成；实屏及白天动作验收仍待用户反馈，尚未发版。

- 开发位置仍为 `/Users/tupi/tab5-nabo-push`。外接卷当前检出的是旧 `main`；没有切换它，也没有改动用户未提交的 `HANDOFF.md` 和 `scripts/push_tab5_daily.mjs`。
- 修复 `test_tab5_daily_mirror.py`：截取范围排除新的提醒实现，补假提醒成员和空的显示方法，以及 `<cstring>`；新增提醒有效时不覆盖标题/正文、不排队镜像、不改对话面板的断言。没有改运行时代码。
- ESP-IDF 6.1 环境完整宿主测试：202 项，199 通过、3 项因 IDF Python 环境缺少 numpy 跳过。使用隔离环境读取本机已有 numpy 后，`test_nabo_radio.py` 的 4 项全部通过（其中 3 项正是前述跳过项），**合计 202 个不同测试全部通过，无未验证跳过项**。SDK CRC 测试已在设置 `IDF_PATH` 的完整运行中通过。中转站 `nabo_routes.test.js` 10/10 通过，`git diff --check` 通过。
- ESP-IDF `v6.1-dirty` 下规范构建 `python3 scripts/build.py qdtech/tab5 --name qdtech-tab5-native` 成功。镜像仍为 12,282,592 B，factory 剩余 300,320 B。应用版本保持 `1.0.17`，功能源码仍为 `df21a4e`。
- 刷前再次核对 MAC `e8:f6:0a:e2:d7:e0`，整片 16 MiB Flash、factory、NVS 均已私有备份；刷前 factory 的有效镜像与 `1840c8a` 备份逐字节一致。新备份目录：`/Volumes/liutupi/tab5-private-backups/nabo-reminder-codex-20261022/`。内含 `backup-manifest.json`、新 BIN/ELF、构建/测试/刷机日志和 `device-validation.json`；不得上传。
- 只写 `factory` 的 `0x20000`，使用 `--after hard-reset`，写后哈希校验通过。新 BIN SHA-256：`d15065c09a0fac1d3b3227b29b63a206827b792b36bd571b0d023665cfbb19e1`；ELF SHA-256：`7f3e308870c05ec0236b3075586b88e7ae801e8fdc0536f9d0c17ccc9294eec9`。150.3 秒串口观察确认应用版本和 ELF 标识、进入 idle，未见 panic、断言、看门狗或 brownout。
- 已通过 Grok Bot 的现有「Nabo 推送」对话请求真实验收待办。Grok 返回 `ok:true`、`rev:11`：`nabo-codex-reminder-test-20261022232530`，标题 `23:40 主页提醒验收（测试）`，`ts=2026-10-22T23:25:30+08:00`，`source=nabo/todo`、`priority=high`、`mood=alert`。设备运行约 140 秒时串口确认同步到 `rev 11`。未撤回这条验收待办，原有推送保留。

剩余验收与发版条件：

1. 用户目视确认主页提醒卡片、点卡片进入待办板块、小 NABO 上下浮动及 mood 小字。若先显示原有 `23:00 夜班交接查房`，点开再回主页即可让下一条待办提醒出现，不必勾选完成真实待办。
2. 本次验收在 23:00–7:00 夜间窗口，按约定不插播；A07 实际播放、同条一次/10 分钟间隔和对话/音乐/睡眠抑制仍需白天实机验收。高优先级推送和早间运势的实屏验收也待补，宿主选择逻辑测试已通过。
3. 上述通过后再更新版本、制作 Release 产物及正式验证记录。当前没有创建 tag 或 Release，版本未递增。不要把本次启动和同步成功当作 UI/动作已经通过验收。
