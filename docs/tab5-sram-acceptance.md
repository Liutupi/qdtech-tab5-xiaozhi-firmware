# Tab5 内部 SRAM 耗尽：原因、补丁和实机验收

没有刷机，也没有在设备上复测。点歌发虚、每日推荐卡顿若来自 NAS 码流，不在这次修改里。

## 基线

运行固件不是干净的 `main`。它是 `ef10924` 上的本地脏树，日志里的应用版本是 1.0.19，ELF SHA256 前缀 `73a19d25a`，编译时间 Nov 8 2026 17:20:04。

第一次会话里曾把这 28 个文件提交为基线，但虚拟机在推送前被重置，那个提交不在远端。随后的补丁只基于 `ef10924`。源码包重新附上之后，在隔离目录里用 `ef10924` 加上这 28 个文件重建脏树：`committed-reference` 里的 HTTP 源文件与 `ef10924` 逐字节相同，没有当成改动。`git apply --check` 通过，套用后这 28 个文件的 SHA256 不变。

`ef10924` 和本补丁的 `CMakeLists.txt` 都是 `PROJECT_VER "1.0.18"`。设备日志是 1.0.19。重新附上的源码包里没有 `CMakeLists.txt`，也没有把版本写成 1.0.19。git status 里另有未纳入该包的 `.audio-fix-park-20261108-171939/CMakeLists.txt`，这里看不到它的内容，所以不改版本号。`3cae881`（v1.0.19）才把仓库版本写成 1.0.19；`8c5bb82` 写成 1.0.20。

远端 `main` 的 `8c5bb82`（keep NABO push history in PSRAM）只处理了历史字符串，没有处理 HTTP 失败时的 `abort`。本分支从 `ef10924` 拉出，吸收了那次字符串迁移，并补上失败路径。

## 因果关系

47.8 秒的内部堆摘要约 3155 个已分配块，平均一百多字节。其中 100KB 的 DMA 保留池有 1297 块、最大空闲块为 0，DMA 空闲 287 字节。PSRAM 当时还有约 1.3MB。

时间对得上：36.6 秒内部空闲 139KB；41.5 秒 `NaboPush: history: 226 item(s)`；46.6 秒空闲 7KB，之后两分钟维持在 5–9KB。SD 在此之前能读，此后 `allocate_dma_buf` 返回 `ESP_ERR_NO_MEM`。`mbedtls_ssl_setup` 返回 `-0x008D`。

`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=1024` 让不超过 1024 字节的普通 `malloc` 使用 `MALLOC_CAP_INTERNAL`，因此会进入 DMA 保留池。226 条推送各有多段 16–1024 字节的 `std::string`，`PublishLocked()` 再复制一整份。这批块的数量和尺寸与堆摘要一致，而且加载之后空闲不再回到 139KB，所以是留下来的历史字符串，不是一次临时的 cJSON 尖峰。cJSON 在这块板上已经优先使用 PSRAM。

最后一次 `ntfy.sh` 建 socket 失败后，`HttpClient` 在打印 `HTTP connection closed` 之前进入 `std::mutex::lock`。锁失败会构造 `std::system_error` 并调用 `std::error_code::message()`，未启用异常时 `__wrap___cxa_allocate_exception` 直接 `abort()`。`mcause=2` 是这个 `abort`，不是另一条非法指令。打开串口的 USB 复位不是这次崩溃。历史计数 122→123，本窗口只新增 1 次。

这次抓包没有点歌。用户另外说每日推送的歌经常放不了、「播放每日推荐」很卡。内部堆已经掉到 7KB 时，TLS 和 DMA 都会失败，播放缓冲也会被拖垮，这和「放不了 / 卡」相容。NAS 码流是否另有问题，留给正在进行的 NAS 调查；这里不改 NAS、中继或播放缓冲大小。

## 修改

- 推送条目、标记和历史文件缓冲使用已有的 PSRAM 分配器。标记直接从 cJSON 插入，不再先攒最多 4000 个内部 `std::string`。
- 新建 TCP/TLS 前，内部空闲小于 32KB 或最大块小于 8KB 时，`HttpClient::Open` 返回 `network_unavailable`，不调用 `mbedtls_ssl_setup`。原有轮询间隔之后会再试。音频流仍走原来的 `esp_http_client`。
- `Close` 和连接失败不再调用会抛异常的 `std::mutex::lock`。失败日志用 `NetworkError::Format` 写到栈缓冲区。
- 历史加载前后各打一行 `MemDiag`，方便实机对照。

没有打开 C++ 异常，也没有关掉每日推送、home 轮询、SD 或音频。

## 宿主测试

```sh
python3 -m unittest \
  scripts.tests.test_tab5_nabo_push \
  scripts.tests.test_tab5_low_memory_http \
  scripts.tests.test_tab5_podcast_memory -v
```

`test_history_payload_uses_psram_allocator` 灌入 226 条中等长度字段，要求 PSRAM 分配器计数超过 60KB，且不超过 1024 字节的 `operator new` 合计小于 8KB。`test_gate_failed_lock_and_error_text` 用抓包里的 7275 / 3840 拒绝连接，用 139263 / 98304 恢复允许，错误检测互斥锁第二次加锁返回失败而不中止，并且 `Format` 不调用 `operator new` 就得到 `TLS handshake failed (tls_failed, native=32791)`。

## 套到原来的脏树并构建

云端没有 `/Users/tupi/esp-idf-v6.1` 那份 `v6.1-dirty`，所以没有新的 ELF/BIN，也不能和设备上的 `73a19d25…` 对比。在原来的 Mac 上：

```sh
cd /Users/tupi/tab5-nabo-push   # 仍应是 ef10924，并保留未提交文件
git diff ef10924d09a8f50d51dd41a725cb4786d8283c30 <fix-sha> | git apply --check
git diff ef10924d09a8f50d51dd41a725cb4786d8283c30 <fix-sha> | git apply
source /Users/tupi/esp-idf-v6.1/export.sh
python3 scripts/build.py qdtech/tab5 --name qdtech-tab5-native
```

`git apply --check` 若报已修改文件冲突，不要重置工作区。本次没有改 `sdkconfig` 的 `ALWAYSINTERNAL`。

## 实机要看的数

不要把刷写后第一次 USB 复位当成 PANIC。home、不点歌，至少看到 `NaboPush: history:` 和 `MemDiag: after NABO history` 之后 3 分钟：

- 内部 free 保持在 32KB 以上，不能再停在 5–9KB。加载前的对照是约 139KB。
- 内部最大块大于 8KB。不能再出现 `allocate_dma_buf` / `ESP_ERR_NO_MEM` 后场景一直 static fallback。
- 空闲轮询不再连续 `mbedtls_ssl_setup returned -0x008D`。允许 `defer connect`，内存回来后下一次轮询应能再连。
- 失败时仍有 `HTTP connection closed`，`Tab5Diag` 的 PANIC 次数不增加。
- 再播每日推荐：失败只能回到现有错误提示，不能在 `HttpClient` / `__cxa_allocate_exception` 重启；停止后内部 free 回到播放前同一量级。卡顿若仍在、而内部 free 和 DMA 正常，就不是这次 SRAM 问题，交给 NAS 调查。

`minimal sram` 记住开机后的最低水位（AFE 创建时到过约 14KB）。验收看历史加载之后的 free 和 largest。

## 回退

```sh
git checkout ef10924d09a8f50d51dd41a725cb4786d8283c30 -- \
  main/boards/qdtech/tab5/tab5_nabo_fortune_view.cc \
  main/boards/qdtech/tab5/tab5_nabo_push.cc \
  main/boards/qdtech/tab5/tab5_nabo_push.h \
  main/boards/qdtech/tab5/tab5_nabo_push_page.cc \
  main/boards/qdtech/tab5/tab5_nabo_remind.cc \
  main/boards/qdtech/tab5/tab5_nabo_remind.h \
  main/boards/qdtech/tab5/tab5_podcast_model.h \
  scripts/tests/test_tab5_nabo_push.py \
  vendor_overrides/78__esp-ml307/include/network_error.h \
  vendor_overrides/78__esp-ml307/src/network_error.cc \
  vendor_overrides/78__esp-ml307/src/http_client.cc \
  vendor_overrides/78__esp-ml307/src/esp/esp_ssl.cc
rm -f vendor_overrides/78__esp-ml307/include/heap_policy.h \
      vendor_overrides/78__esp-ml307/include/nothrow_lock.h \
      docs/tab5-sram-acceptance.md \
      scripts/tests/test_tab5_low_memory_http.py
```

未改分区、NVS 和资源。若已刷机，用刷写前的 factory 镜像写回 `0x20000`。本次没有新 ELF。

## 尚未证实

- 没有编出固件，没有新 ELF 的 SHA256。
- 宿主测试没有跑 FreeRTOS、mbedtls 或 SDMMC。
- `condition_variable::wait` 仍可能在锁失败时抛异常。抓到的是 `Close` 里的 `mutex::lock`。
- 其它仍调用 `ToString()` 的路径在内部堆耗尽时还会分配。
- 其它不超过 1024 字节的 `malloc` 仍优先内部 SRAM。没有改 `ALWAYSINTERNAL`，避免把必须留在内部的小 DMA 缓冲挪走。
- PSRAM 分配失败时现有分配器仍会 `abort()`。抓包里 PSRAM 还有约 1.3MB。
- 不能说设备上已经修好，也不能说每日推荐的卡顿已经排除。
