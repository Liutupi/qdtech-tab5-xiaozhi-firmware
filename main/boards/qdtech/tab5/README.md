# Tab5 板级目录

此目录在 XiaoZhi ESP32-P4 底座上加入原 QDTech S3 项目的桌面与服务。硬件入口是 `m5stack_tab5.cc`；配置由 `config.json` 选出两个 P4 芯片版本。屏幕驱动支持 ILI9881C、ST7121、ST7123，触摸支持 GT911/ST712x。音频为 ES8388 + ES7210；SD 卡须在 C6 Wi‑Fi 的 SDIO host 初始化后使用 slot 0，不能让照片、播客或模拟器重新初始化整个 host。

当前的 `QdtechTab5Display` 仅把原项目 480 × 320 画面经软件放大旋转到 Tab5 物理 720 × 1280 屏。用户已经确认实际效果不合适，且文字、数字、时间缺失。请先按仓库根目录的 [PORTING_STATUS.md](../../../../PORTING_STATUS.md) 解决这些 P0 问题，再扩展功能。
