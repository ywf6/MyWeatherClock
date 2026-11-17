# MyWeatherClock

这个仓库保存 STM32F407、ESP32-C3、Qt 上位机、Python 后台和 STM32 Bootloader 的源码，以及语音唤醒模型和串口依赖。适合在 Codex Cloud 中阅读、修改源码，或运行不依赖真机的检查。

仓库不包含迁移包预编译的 Qt EXE/DLL、Windows 唤醒运行库和固件镜像。启动 Qt 前需按下方说明构建；如需运行离线唤醒，请将原迁移包中的 `voice-link/voice-assets/wake-runtime/` 复制到本仓库的 `qt/voice-assets/wake-runtime/`。固件镜像由对应构建脚本生成。烧录、真实传感器和音频联调仍需连接开发板。

建议从 [代码阅读指南](代码阅读指南.md) 入手。下方命令对应当前源码目录。预编译镜像可在原 Windows 迁移包中找到。

ST7789 SPI 轮询与 DMA 的对照固件、逻辑分析仪采集记录和原始波形见 [LCD 刷屏测试](tools/lcd-dma-benchmark/README.md)。

## 在 Windows 上使用

1. 按下文命令构建 Qt 上位机，然后双击仓库根目录的 `Start-WeatherClock.cmd`。
2. 需要 AI 问答、录音、中文点阵或唤醒时，安装带 Tk 的 64 位 Python 3.13，并执行 `python -m pip install -r qt/requirements.txt`。新电脑还需在本机配置环境变量 `DASHSCOPE_API_KEY`。上位机默认使用包内语音目录，Python 路径可以在界面修改。中文点阵依赖 Windows 的 Microsoft YaHei 字体。
3. 外置 C3 与电脑连同一局域网，Qt 选择 Wi-Fi 并查找设备。若换了热点，用外置 C3 的 USB 串口连接上位机，在界面输入新热点并保存。迁移现有开发板时，电脑软件迁移本身不需要重刷板子。

## 接线

| 外置 ESP32-C3 | 对端 |
| --- | --- |
| GPIO0 / TX | STM32 PD9 / USART3 RX |
| GPIO1 / RX | STM32 PD8 / USART3 TX |
| GND | STM32 GND |
| GPIO4 | INMP441 SCK、MAX98357A BCLK |
| GPIO5 | INMP441 WS、MAX98357A LRC |
| GPIO6 | MAX98357A DIN |
| GPIO7 | INMP441 SD |

两板 USART3 侧为 115200、8N1，外置 C3 的电脑 USB 串口为 460800。INMP441 使用 3.3 V，L/R 接地。喇叭接功放的两根扬声器输出线，输出负端不接地。板载 ESP-AT 由 STM32 经 USART2 使用。

## 目录

- `stm32/` 保存 STM32F407 主固件、驱动和 Keil 工程，`stm32/ota/` 保存 Bootloader、分区定义和打包脚本。
- `esp32/` 是外置 ESP32-C3 的 ESP-IDF 工程，`esp32/main/voice_firmware.cpp` 保留原有 Arduino 接口和协议。原 Arduino 版镜像位于迁移包的 `firmware/ESP32-C3/`。
- `qt/` 保存电脑端代码。`qt/native/` 是 Qt 源码，`qt/qt_backend.py` 是 Python 后台入口，`qt/voice-assets/` 保存语音接口和离线唤醒资源，`qt/host-console/` 与 `qt/ai-assistant/` 保存后台复用的模块。
- 原迁移包 `firmware/STM32/` 的 `weatherclock-first-install.hex` 用于 ST-Link 首次安装，`weatherclock.wcota` 用于 Qt 无线升级。这里的 STM32 固件由迁移源码重新构建，配网和心知天气凭据留空。

## 重新编译与烧录

- STM32 在 `stm32/app/wifi.h` 填写新环境的 `WIFI_SSID`、`WIFI_PASSWD` 和 `WC_SENIVERSE_KEY`。留空时，STM32 不会主动改用新的 Wi-Fi 凭据，也不会调用室外天气服务。用环境变量 `ARMCC_BIN` 指向本机 ARM Compiler 5 的 bin 目录，然后运行 `powershell -ExecutionPolicy Bypass -File stm32/ota/build.ps1`，产物在 `stm32/ota/build/`。
- ESP32-C3 使用 VS Code 和乐鑫 ESP-IDF 扩展（`espressif.esp-idf-extension`）。在 VS Code 中打开 `esp32/` 文件夹，配置 ESP-IDF 5.5.x，目标为 `esp32c3`。在命令面板运行 `ESP-IDF: Build your Project`。首次构建会获取 Arduino ESP32 3.3.12 组件及其依赖。也可在已激活的 ESP-IDF 终端执行 `idf.py -C esp32 build`。应用镜像为 `esp32/build/weatherclock_voice.bin`。
- ESP32-C3 的 `esp32/partitions.csv` 沿用此前 4 MB 分区，`app0` 起始地址为 `0x10000`。首次烧录或需要更新引导程序时，确认连接的是外置 C3，再用 ESP-IDF 的 Flash。继续使用已部署分区时，确认当前启动槽再更新应用。项目的 USB 串口通信仍使用 UART0（460800），STM32 走 UART1（115200）。
- Qt 准备 Qt 6.12.0 MinGW 64 位工具链、CMake 和 Ninja，运行 `powershell -ExecutionPolicy Bypass -File qt/native/build.ps1 -QtRoot <Qt目录> -CompilerRoot <MinGW目录>`。产物在 `qt/native/build-auto/`。

首次在 STM32 上部署 Bootloader 要用 ST-Link 写入 HEX。后续 Qt 选择 `.wcota`，按住板上 K3 再确认，开始传输后松手，等上位机确认新固件启动。Bootloader 使用板上 U32 暂存整包，当前采用单应用区。传输中断可重发，安装中断恢复已实现但仍待真机断电测试。

ARMCC、ESP-IDF、Qt 开发工具、Python 安装程序和私密配置由目标电脑准备。

## 小智语音模式

`xiaozhi/firmware` 是外置 ESP32-C3 的小智固件，`xiaozhi/server` 是电脑端 WebSocket 服务。先用原有 C3 固件完成热点配网，小智固件复用 C3 已保存的网络。设备连接地址和令牌从 Git 忽略的 `xiaozhi/config.local.json` 读取，配置后在项目根目录执行：

```powershell
.\xiaozhi\build.ps1
.\xiaozhi\start-server.ps1 -Backend echo -BindAddress 192.168.137.1
```

将绑定地址换成 `server_url` 对应的电脑局域网地址。要使用百炼，将 `-Backend echo` 换成 `-Backend dashscope`，启动服务的终端需要有 `DASHSCOPE_API_KEY`。

按 STM32 的 K2 开始录音，再按 K2 或等到 6 秒上限结束录音。C3 播放回复，STM32 显示文字；K3 返回时钟并取消当前对话。Qt 连接小智固件后可查看设备状态、返回主页并升级 STM32。Qt 保持连接时，K2 仍可发起问答。

在运行服务的电脑打开 [小智网页控制台](http://127.0.0.1:8001/)，可查看 C3 在线状态、问答进度、最近回答和 STM32 上屏结果。问答记录仅保存在本次服务运行的内存中。默认控制台端口为 8001，可通过启动脚本的 `-DashboardPort` 更改。
