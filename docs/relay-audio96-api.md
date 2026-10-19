# Tab5 全源 96 kbps 音频接口约定（v1）

供 NAS muse-relay 的维护者实施。Tab5 v1.0.16 将把网络电台、每日播客、推荐歌曲和收藏歌曲的播放请求全部送到此接口。现有 /track、/song、节目列表和上传接口保持兼容。先备份部署目录；不要把用户 token、源音频签名 URL 或私有配置提交到公开仓库。

## 地址和认证

- 局域网：GET /tab5/podcast/stream96?url=<编码后的音源URL>&live=0
- 公网：GET /podcast/<现有 Muse token>/stream96?url=<编码后的音源URL>&live=0
- token 与现有 /podcast/<token>/track、/song 相同，仍放在路径中。
- LAN 接口不需 token，但应沿用现有来源限制，从 Cloudflare 进入 /tab5 路径必须返回 404。
- 错误路径或 token 返回 404。

这是直接返回 MP3 字节的接口，不返回一个需要固件再次解析的 JSON 音源链接。成功不应重定向到上游原始音频，因为那样无法保证统一格式。

## 参数

- url：必填，完整 http/https 音源 URL，调用方按 UTF-8/RFC3986 编码一次。最长 2048 字节。可能是网易云签名直链、现有 /audio/<token>/<filename>.mp3、或直播 MP3 地址。
- live：必填，只允许 0 或 1。1 表示网络直播电台；0 表示可缓存、可定位的完整播客/歌曲。
- 上游链接会过期：文件缓存可按 URL 的安全摘要或已有曲目/节目身份定位，但不得把过期的上游签名 URL 当成以后重新播放的唯一凭据。
- 拒绝接口自身的 stream96 URL，防止递归代理。

## 统一的输出格式

- MP3，MPEG-1 Layer III。
- 固定码率 CBR 96,000 bit/s（整个双声道文件合计 96 kbps，不是每个声道各 96 kbps）。
- 采样率 44,100 Hz，2 个声道。
- 必须保留原始左右声道关系，不做左右混音、交换、反相或只取一侧。原音源为单声道时可以复制到左右两路；这不能凭空生成原本不存在的立体声信息。
- ffmpeg 编码参数可用：-c:a libmp3lame -b:a 96k -ar 44100 -ac 2。不要使用 -q:a 或其他 VBR 参数。
- 已确认为 MP3/CBR 96k/44.1kHz/双声道的文件可以直接复制字节，避免重复有损编码；不能仅凭一个平均码率字段认定为 CBR。

## live=0：完整文件缓存、Range 和暂停后继续

Tab5 暂停时保留已输出的位置并关闭下载连接；继续时通过 Range 从原位置恢复，带少量 MP3 解码预热。因此此模式必须给出稳定、可定位的 MP3 文件。

1. 在 NAS 上将完整源文件转为上述格式，写临时文件，完成后校验并原子提交缓存。不能把未完成文件作为完整成品返回。
2. 首个请求可等待准备，但头部响应最长 20 秒。超过时返回 503 JSON，code=preparing，Retry-After: 2；后台缓存任务继续。同一来源后续请求复用任务，不能反复启动 ffmpeg。
3. 完成后 GET 无 Range 返回 200、Content-Type: audio/mpeg、准确的 Content-Length、Accept-Ranges: bytes。
4. 支持 GET 的标准单区间 Range: bytes=N-、bytes=N-M，返回 206、准确的 Content-Range 与 Content-Length；无效范围返回 416。
5. 必须支持 HEAD，返回与 GET 相同的格式和大小头部，不发送音频体。HEAD 使用相同查询参数。
6. 同一条流暂停期间，要保证缓存不会被回收；建议文件闲置保留至少 24 小时，并为缓存设置总大小/文件数上限，正在播放的项目不得被清理。
7. 流关闭或客户端断开不得留下无界内存缓冲。缓存准备任务可以继续，但必须有超时和并发数量限制。

## live=1：实时直播转码

1. 尽快返回 200、Content-Type: audio/mpeg，然后连续输出 96k/44.1kHz/双声道 MP3。
2. 不要虚构 Content-Length；可使用 chunked。Accept-Ranges: none，不支持文件定位。
3. 设置有限的并发转码数量和启动超时。客户端断开后关闭输入连接并终止相关 ffmpeg 进程；不能泄漏子进程。
4. 直播暂停不承诺无限时移缓存。固件继续时可以重新接入当前直播。文件模式的播客/歌曲必须保留原位置，不能按直播方式从头重播。

## 建议的响应头

- Content-Type: audio/mpeg
- X-Audio-Codec: mp3
- X-Audio-Bitrate: 96000
- X-Audio-Sample-Rate: 44100
- X-Audio-Channels: 2
- X-Audio-Seekable: 1（文件）或 0（直播）
- Cache-Control: private, no-store（面向公网响应；NAS 自己的受控文件缓存不受此限制）

## 安全与边界

- 只允许 http/https 音源，拒绝 URL 内嵌用户名/密码及 file/ftp/udp 等协议。
- 这是带认证的音频转换接口，不能成为公开的任意内网代理。校验 DNS 解析结果和每次重定向，禁止回环、链路本地、云元数据及其他私网来源；唯一可明确放行的内网来源是 relay 自己的已认证 /audio/<同一token>/<filename>.mp3。
- 推荐由 Node 校验并读取上游，把字节通过 pipe:0 交给 ffmpeg，避免 ffmpeg 自行跟随未校验地址。源 URL 和 token 不应出现在子进程参数日志里。
- 使用 spawn 参数数组，不使用 shell 拼接。
- 缓存文件名使用安全摘要，不使用歌名或 URL 直接作为路径。
- 为文件转换、上游请求、磁盘缓存和并发转码设置明确上限。失败清理临时文件。
- 网易云上游请求可复用当前 /track、/song 成功音源的 User-Agent/Referer 等必要头部。

## 错误响应

除 token/路径错误可用纯文本 404，其余错误建议 {ok:false,code:...}：

- 400 bad_request：缺参数、非法 URL/live 值。
- 403 source_forbidden：来源未获允许。
- 404 source_not_found：上游文件/电台不存在。
- 416 range_not_satisfiable：文件 Range 无效。
- 502 upstream_error：下载或解码失败。
- 503 preparing：文件正在准备，后台工作继续，Retry-After: 2。
- 503 busy：达到转码并发限制，Retry-After: 2。
- 504 timeout：后台转换任务超时。

固件不会在转码不可用时静默绕过到原始高码率音源；会显示准备/失败并允许重试。

## 部署后交接给 Codex 的最少信息

1. 确认上述 LAN/公网两条路径已经上线，现有 token 不变。
2. 一首原始立体声歌曲和一集播客：200、真实 Content-Length，两个非零 Range 均为 206，取到的字节与完整成品对应位置一致。
3. 用 ffprobe 核对 MP3、44,100 Hz、2 channels；用逐帧包长/码率验证 CBR 96k（44.1kHz 下 MP3 帧长交替为 313/314 字节属正常填充）。
4. 一路直播连续转码至少 2 分钟，客户端关闭后确认没有残留进程。
5. 用左右不同的测试信号验证没有下混：例如左 440Hz、右 880Hz；不能只看 channels=2。
6. 验证无 token、非法来源、递归 URL、503 preparing、超出并发、Range 越界和取消后的清理。
7. 给出接口是否完全按本文实现、缓存保留/容量/并发限制及差异。不要在交接消息中粘贴真实 token 或带签名音源 URL。
