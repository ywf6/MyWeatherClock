# MyWeatherClock

这个仓库保存 STM32F407、ESP32-C3、Qt 上位机、Python 后台和 STM32 Bootloader 的源码，以及语音唤醒模型和串口依赖。适合在 Codex Cloud 中阅读、修改源码，或运行不依赖真机的检查。

仓库不包含迁移包预编译的 Qt EXE/DLL、Windows 唤醒运行库和固件镜像。启动 Qt 前需按下方说明构建；如需运行离线唤醒，请从原迁移包取回 voice-link/voice-assets/wake-runtime/。固件镜像由对应构建脚本生成。烧录、真实传感器和音频联调仍需连接开发板。

建议从 [代码阅读指南](代码阅读指南.md) 入手。下方保留原迁移包说明，其中“随包提供”的文件指原 Windows 迁移包。

# WeatherClock 迁移包

## 在新电脑上使用

1. 解压整个 `WeatherClock` 目录。Windows 64 位系统双击 `Start-WeatherClock.cmd` 即可打开 Qt 上位机。上位机的 Qt 运行文件随包提供。
2. 需要 AI 问答、录音、中文点阵或唤醒时，安装带 Tk 的 64 位 Python 3.13，并执行 `python -m pip install -r voice-link/requirements.txt`。新电脑还需在本机配置环境变量 `DASHSCOPE_API_KEY`。上位机默认使用包内语音目录，Python 路径可以在界面修改。中文点阵依赖 Windows 的 Microsoft YaHei 字体。
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

- `voice-link/qt-native/` 为 Qt 源码及可直接运行的 Windows 程序。
- `voice-link/qt_backend.py` 为 Python 中转入口，`voice-assets/` 为语音接口和离线唤醒资源。同级 `host-console/` 与 `ai-assistant/` 仅保留中转依赖的 Python 文件。
- `voice-link/VoiceFirmware/` 为外置 ESP32-C3 Arduino 源码，`firmware/ESP32-C3/` 提供应用镜像和整片镜像。
- `upstream/` 为 STM32F407 WeatherClock 工程和依赖。 `ota/` 为 Bootloader、分区定义、打包和构建脚本。
- `firmware/STM32/` 的 `weatherclock-first-install.hex` 用于 ST-Link 首次安装，`weatherclock.wcota` 用于 Qt 无线升级。这里的 STM32 固件由迁移源码重新构建，配网和心知天气凭据留空。

## 重新编译与烧录

- STM32 在 `upstream/app/wifi.h` 填写新环境的 `WIFI_SSID`、`WIFI_PASSWD` 和 `WC_SENIVERSE_KEY`。留空时，STM32 不会主动改用新的 Wi-Fi 凭据，也不会调用室外天气服务。用环境变量 `ARMCC_BIN` 指向本机 ARM Compiler 5 的 bin 目录，然后运行 `powershell -ExecutionPolicy Bypass -File ota/build.ps1`，产物在 `ota/build/`。
- ESP32-C3 准备 Arduino CLI 和 Arduino ESP32 3.3.12，运行 `powershell -ExecutionPolicy Bypass -File build-esp32c3.ps1`。外置 C3 的现有分区须核对后再写应用镜像，原联调槽地址为 `0x10000`。全新板可写整片镜像到 `0x0`，会覆盖原分区配置。两个镜像对应不同写入范围。
- Qt 准备 Qt 6.12.0 MinGW 64 位工具链、CMake 和 Ninja，运行 `powershell -ExecutionPolicy Bypass -File voice-link/qt-native/build.ps1 -QtRoot <Qt目录> -CompilerRoot <MinGW目录>`。要重新构建已解压的包，可沿用同一目录结构。

首次在 STM32 上部署 Bootloader 要用 ST-Link 写入 HEX。后续 Qt 选择 `.wcota`，按住板上 K3 再确认，开始传输后松手，等上位机确认新固件启动。Bootloader 使用板上 U32 暂存整包，当前采用单应用区。传输中断可重发，安装中断恢复已实现但仍待真机断电测试。

包内包含运行文件和工程源码。ARMCC、Arduino CLI、Qt 开发工具、Python 安装程序和私密配置需要由目标电脑准备。
