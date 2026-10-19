# 给 Grok：Tab5 96k 双声道转码接口部署

本包是可运行的 Node 模块，不是要替换 NAS 上的 relay.js。保留现有 song_routes.js、节目上传、token 和已存音频。先备份现有部署目录，再在同一个 muse-relay 服务挂载新模块。

## 文件和依赖

- `stream96_routes.js`：接口实现，Node 18+，无 npm 依赖。
- `enable_stream96.js`：小范围挂载工具，先检查语法并完整备份当前 relay.js，只增加一个初始化块和一个请求入口。
- `stream96_routes.test.js`：本机集成测试，使用真实 ffmpeg/ffprobe 和左右不同的音频信号。
- `RELAY_AUDIO96_API.md`：固件端配套约定。

运行容器必须有 `ffmpeg` 和 `ffprobe`，且 `ffmpeg -encoders` 中有 `libmp3lame`。Alpine 可以安装 ffmpeg 包；请把安装加入容器的持久构建/启动配置，不能只在即将重建的容器中临时安装。不要替换已有容器镜像配置而丢失其其他依赖。

## 部署

1. 把前三个 JS 文件放进 `/data/`，与当前 relay.js 同目录。
2. 执行 `node --test /data/stream96_routes.test.js`，四个测试全部通过。测试音频、测试 token 只在独立临时目录使用，不接触现有节目数据。
3. 执行 `node /data/enable_stream96.js /data/relay.js`。它只适配当前 `const server = http.createServer(async (req, res) => {` 结构；不匹配时会停止，请手动按下面的方式接入。不要把仓库中的旧 relay.js 覆盖到 NAS。
4. 在现有环境中增加 `AUDIO96_PUBLIC_ORIGIN=https://tab5-relay.liutupi.top`。此值用于识别本服务的已认证 `/audio/<token>/<filename>.mp3`，直接从 `/data/audio/` 读取；不会开放任意私网 HTTP 请求。`tunnelUrl()` 返回的临时域名也会识别。
5. 重启 relay Node 进程，保留现有 tunnel 和数据挂载。检查原来的列表、`/track`、`/song`、上传和转录接口仍正常。

手动挂载等价于：

```js
const handleMuseStream96 = require('./stream96_routes').createStream96Routes({
  dataDir: DATA_DIR,
  token: TOKEN,
  selfOrigins: () => [tunnelUrl(), process.env.AUDIO96_PUBLIC_ORIGIN].filter(Boolean),
});
// 在 createServer 的第一段、任何通用 /podcast 分支之前：
if (await handleMuseStream96(req, res)) return;
```

接口方法/参数与约定一致，直接返回 MP3 字节。不会改动 `/song` 的返回结构，也不会把源 URL 返回给播放器作为绕过转码的后备方案。

## 实现的资源边界

- 所有输出均经 libmp3lame 转成 CBR **96,000 bit/s、44,100 Hz、2 channels**，逐帧检查输出格式；保留左右关系。当前版本统一重新编码，不做原文件快路径。
- 文件先写 `.part`，完整成功后原子改名。输出不带 ID3/Xing 头，Range 字节稳定。
- 缓存目录 `/data/audio96-cache/`，最多 256 个完成文件，完成文件与正在转换的预留合计不超过 512 MiB；每个成品最多 128 MiB、每个输入最多 256 MiB。
- 文件闲置至少保留 24 小时；正在传输的文件不会回收。缓存满且没有可回收文件时返回 503 busy，不提前删掉用户刚暂停的节目。
- 文件转换与直播合计最多 2 个进程，无无界等待队列；同一文件准备请求合并，最多同时处理 32 个接口请求。
- 文件转换最长 15 分钟，等响应最多 19 秒；未准备好返回 503 preparing，后台继续。上游闲置超时 15 秒，直播单次连接最多 6 小时，设备会自动重连。
- 直播客户端断开即取消输入并终止 ffmpeg；SIGTERM 后 2 秒仍未退出则 SIGKILL。直播暂停后继续接入当前直播，没有直播回听。
- 输入只经 Node 读取再交给 ffmpeg 的标准输入；ffmpeg 不获源 URL，也不允许从媒体内部再次访问网络。DNS 地址检查并固定、每次跳转重新检查，拒绝私网/回环/元数据/递归代理；已认证的本服务音频通过本地文件读取例外处理。
- 模块不打印 token、带签名的源 URL 或 ffmpeg stderr。部署时也请避免把接口完整 URL 输出到共享日志。

可选参数 `maxJobs/maxBytes/maxFile/maxInput/retainMs/jobMs/ffmpeg` 可在初始化中调整；不要将 retainMs 调低于 24 小时。生产不需要改测试中的 prepareMs。

## 上线验收并回传

请用现有 token 验证公网和局域网地址：

- 文件 GET 200、HEAD、两个非零 Range 206，逐字节比较 Range 与完整成品；错误范围 416。
- 播客、推荐歌曲、收藏歌曲各一条；旧接口不受影响。
- ffprobe 为 MP3/44100/2ch/96000；逐帧码率一致。左 440 Hz、右 880 Hz 测试无串换或下混。
- 一路真实电台连续 2 分钟，断开后无残留子进程；再播其他台正常。
- 错 token 和从 Cloudflare 访问 LAN 路径返回 404；私网来源被拒绝；重复准备合并、busy、preparing 和中断清理正常。

回传“接口已上线、测试结果、是否与约定有差异”即可，不贴真实 token 或签名 URL。Codex 随后做设备端联调及刷机验收，再发布 v1.0.16。
