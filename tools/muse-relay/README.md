# Muse 播客 MP3 上传

云 VM 通过现有 relay 隧道上传原声节目，无需新增通道。保留现有 `tab5_podcast_publish` 的 `title`、`script`、`tracks` 和可选 `audio_url`，以及网易云取歌接口。

## 大文件使用分片客户端

7.4 MB MP3 在约58 KB/s的上传链路上需要超过两分钟。整文件 multipart 请求可能被代理超时中断；客户端发完所有字节并不代表 NAS 已完整保存。分片接口每512 KiB单独请求，落盘并校验后才确认；重试或 relay 进程重启后可续传，完整校验后才更新节目地址。

认证沿用现有 MCP token，不新增凭据。设置 `MUSE_RELAY_BASE` 为当前隧道根地址（不包含 `/mcp/<token>`），`MUSE_TOKEN` 为该 MCP 地址里的 token；也可直接设置现有完整 `MUSE_MCP_URL` 给 Python 客户端。

```sh
# 沿用已配置的 MUSE_RELAY_BASE、MUSE_TOKEN；下载无需安装第三方库的客户端。
curl --fail --show-error \
  -H "Authorization: Bearer $MUSE_TOKEN" \
  "$MUSE_RELAY_BASE/podcast/audio/uploads/client" \
  -o /tmp/tab5_muse_upload_audio.py

python3 /tmp/tab5_muse_upload_audio.py \
  "$HOME/workspace/podcasts/radio-2026-10-06/radio-2026-10-06-v4.mp3" \
  --episode-id 4 --speech-optimize
```

人声节目推荐加 `--speech-optimize`，生成VM需有 `ffmpeg`：客户端创建48kbps、24kHz、单声道MP3副本，上传副本，原始文件不变。约7分44秒的128kbps节目由7.4MB降到2.8MB；设备所需持续下载速度从约16KB/s降到6KB/s，也避免44.1kHz转24kHz的播放重采样。转码失败不会上传或修改节目。重复执行仍可恢复同一副本的分片会话。不加该选项时继续原样上传，适合已生成相同规格的音频。

每天07:35生成音频后，使用07:30文字发布返回的编号作为 `--episode-id`，直接为原节目补上 `audio_url`，不再重复发布。当天文件路径按日期替换。失败后用相同命令重试，会跳过已经确认的分片。会话在最近一次分片保存后保留24小时，最多8个未完成会话。

成功返回 `ok: true`、文件名、大小、SHA256、节目编号、合并耗时。为避免日志泄露访问凭据，客户端默认不打印带认证 token 的 `audio_url`；如需完整回执，使用 `--result-file /private/path/result.json` 保存为0600文件。`--rate-limit 58000` 可模拟慢速链路。

## 分片接口

所有请求都带 `Authorization: Bearer <现有relay token>`。上传索引从0开始，最大文件32 MiB；文件名以字母或数字开头，允许字母、数字、下划线、短横线和点，以 `.mp3` 结尾，最多160字符。

| 请求 | 内容 | 返回 |
| --- | --- | --- |
| `POST /podcast/audio/uploads` | JSON `{filename,size,sha256,episode_id}`，SHA256为小写64位；不指定节目编号仅存文件 | `upload_id`、`chunk_size`（524288）、`chunk_count`、`missing`、`received_bytes`、`state` |
| `GET /podcast/audio/uploads/<upload_id>` | 无 | 已收分片和缺失分片；完成后带 `result` |
| `PUT /podcast/audio/uploads/<upload_id>/chunks/<index>` | 原始二进制；`X-Chunk-SHA256`为本片SHA256；`Content-Length`为本片长度 | `ok`、索引、落盘字节数、SHA256 |
| `POST /podcast/audio/uploads/<upload_id>/complete` | JSON `{}` | 校验合并后返回 `{ok,filename,size,sha256,audio_url,episode_id,processing_ms}` |
| `GET /podcast/audio/uploads/client` | 无 | Python标准库客户端源文件 |

同样的文件名、大小、SHA256和节目编号会恢复同一会话；同一分片可安全重试；已完成的 `complete` 可再次取回成功回执。缺失分片返回409，认证错误401、输入错误400、过量数据413、并发繁忙429。并发最多两个上传写操作。每片校验并同步落盘；合并再次检查每片及整文件 SHA256 和 MP3 文件头，不做转码。

音频存放于 `/data/audio`，临时分片位于其 `.uploads` 子目录。正式文件名带内容摘要，旧音频继续可用。完整验证后原子保存并关联原节目，保留标题、文案和歌单。`GET` / `HEAD /audio/<token>/<filename>` 支持字节范围请求；relay托管地址随隧道域名变化更新，外部音频地址保持原样。

## 旧接口与维护

保留小文件旧接口 `POST /podcast/audio?episode_id=<编号>`，`multipart/form-data` 文件字段 `file`。大文件请使用分片，延长客户端超时不能取消代理的超时。

维护已有NAS部署时，先备份当前模块和 `podcasts.json`，上传 `audio_routes.js`、`audio_uploads.js`、`upload_audio.py`、`enable_audio_upload.js` 到 `/data`。执行 `node /data/enable_audio_upload.js`；脚本先检查模块依赖和语法，已有扩展时不会重写 `relay.js`，首次安装时会保留时间戳备份并合并路由。只重启 `node /data/relay.js` 由原有 supervisor 拉起，保持 cloudflared 进程和当前地址。不要覆盖已有 relay 的音乐和节目功能。

运行验证：`node --test tools/muse-relay/audio_uploads.test.js`。测试覆盖断线、重复请求、进程重启续传、校验错误、旧上传接口、音频 Range 和真实 Python 客户端。

### 逐句字幕与实际音频同步

上传客户端增加 `--transcript /path/to/spoken.json`（也支持 `.srt`、`.vtt`）：

```sh
python3 upload_audio.py radio-2026-10-06-v4.mp3 --episode-id 4 \
  --speech-optimize --transcript radio-2026-10-06-v4.srt
```

字幕必须对应真正说出的内容及其时间；设备不会按字数猜时间。音乐、停顿可保留时间空档。
48 kbps 优化不改变播放速度，不截取内容，原音频的时间标记可继续使用。
JSON 可用数组，或 `{"cues": [...]}`，例如：

```json
{"cues":[
  {"start_ms":0,"end_ms":2400,"text":"早上好，欢迎收听今天的电台。"},
  {"start_ms":3000,"end_ms":6200,"text":"接下来介绍今天的第一首歌曲。"}
]}
```

也可在音频上传成功后独立调用：

- `POST /podcast/transcript?episode_id=4`
- `Authorization: Bearer <现有 Muse token>`；`Content-Type: application/json`
- 请求：`{"audio_sha256":"<分片上传成功回执 sha256>","cues":[...]}`
- 成功：`{"ok":true,"episode_id":4,"cue_count":2}`
- `audio_sha256` 必须是设备播放副本的哈希，不能用原始高码率 MP3 的哈希。
- 每期 1–200 句，毫秒整数、时间递增且不重叠，结束时间不超过 4 小时。
  每句最多 1536 UTF-8 字节，总文本最多 9000 字节；请求最多 32 KiB。
- 音频必须已经存入本 relay；不存在节目返回 404，音频哈希不一致返回 409。
  校验失败保留原音频、原字幕和其他节目字段。
- 换音频会移除不匹配的旧字幕；重试字幕提交可安全重复。
- `/podcast/<token>` 自动返回 `transcript.audio_filename/audio_sha256/cues`。
  设备刷新后即可逐句高亮、自动滚动。缓冲期间不前进；重播和重新连接从头计时。
  手动拖动文案后暂停自动滚动 8 秒。没有字幕的期数仍显示原节目文案。

NAS 更新需同时放入 `transcript_routes.js`、`audio_routes.js`、`audio_uploads.js`、
`upload_audio.py`、`enable_audio_upload.js`，先校验模块，随后只重载 Node relay，保留 cloudflared。

Muse 原生时间轴也可以直接作为请求 JSON（秒为单位），只需补上音频哈希：

```json
{"audio_sha256":"<设备播放副本 sha256>","date":"2026-10-06","duration":464.21,
 "lines":[{"start":0.0,"end":24.19,"kind":"speech","text":"早上好，土皮……"},
          {"start":24.19,"end":41.5,"kind":"music","text":"♪《歌名》— 歌手"}]}
```

客户端的 `--transcript radio-2026-10-06.timeline.json` 也直接支持此格式。
一次提交必须使用同一种格式：`lines` 秒，或 `cues` 毫秒。
