# STM32F407 LCD 轮询与 DMA 对照测试

同一个 RAM 测试程序交替执行轮询和 DMA。两种模式都走现有 ST7789 驱动的显示窗口和片选逻辑，每帧填充相同的 240×320 RGB565 纯色，标称间隔 500 ms。构建脚本只在本目录生成驱动测试副本，其中初始化了原 DMA 中断的唤醒标志。

## 接线

开发板、ST-Link 和逻辑分析仪共地。逻辑分析仪接开发板上的 3.3 V 逻辑信号。GPIO 标记使用板上三颗 LED 的 MCU 侧信号线。接 MCU 引出脚或测点，依据实物核对引脚。

| 分析仪通道 | MCU 引脚 | 信号 |
| --- | --- | --- |
| GND | GND | 共地 |
| CH0 | PB13 | SPI2 SCK，观察像素传输 |
| CH1 | PB0 | Frame，高电平为一次完整刷新 |
| CH2 | PB1 | CPU，高电平为刷屏任务正在运行，或正在处理 LCD DMA 完成中断 |
| CH3 | PE9 | Mode，0 为轮询，1 为 DMA |

默认 SPI 时钟约 10.5 MHz。看 SCK 细节时可用 50 MS/s。24 MS/s 的分析仪可用 `-SlowSpi` 构建约 5.25 MHz 的版本。长时间采集可以只保留 Frame、CPU、Mode 三个通道，另采一段短的 SCK 波形。

## 构建与加载

在本目录运行。构建使用 Keil ARM Compiler 5，默认沿用项目机器上的路径，也可以用 `ARMCC_BIN` 指定工具的 bin 目录。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\run-ram.ps1 -OpenOcdRoot "<OpenOCD安装目录>"
```

使用较低 SPI 时钟时，两条命令都加 `-SlowSpi`。加载脚本使用 ST-Link/SWD。`-OpenOcdRoot` 指向包含 bin/openocd.exe 的 OpenOCD 安装目录。脚本检查 RAM 向量表，把镜像加载并校验到 `0x20000000` 后启动。复位或重新上电回到已安装固件。运行测试期间 STM32 正常时钟和语音联调会暂停。

## 捕获与分析

1. 打开逻辑分析仪，采 8～12 秒。可以用 Frame 上升沿触发。启动后屏幕会保持绿色，Mode 会在两种传输之间切换。
2. 导出数字时序 CSV。时间列用秒，逻辑列每行用 `0` 或 `1` 表示该时刻的状态。导出逐采样或只记录跳变均可。只导出 Frame、CPU、Mode 三个通道可减小文件。
3. 脚本按 Frame 的上升沿和下降沿切分并统计高电平时长。落在采集边缘的半帧会舍弃，两种模式都需要完整帧。

```powershell
python .\analyze.py .\capture.csv
python .\analyze.py .\capture.csv --frame "Channel 1" --cpu "Channel 2" --mode "Channel 3"
python .\analyze.py .\capture-markers-20261117-171623.csv --time Time --time-unit ns
```

默认时间列名是 `Time [s]`。其它表头可通过 `--time`、`--frame`、`--cpu`、`--mode` 指定。此处使用的 sigrok-cli 在 4 MS/s 时输出纳秒时间戳，因此分析原始文件时传入 `--time Time --time-unit ns`。最小格式如下，这几行只展示 CSV 结构。

```csv
Time [s],Frame,CPU,Mode
0.000000,0,0,0
0.001000,1,1,0
0.002000,0,0,0
```

输出中的 Frame ms 是每帧完整耗时，CPU ms 是 CPU 标记累计高电平的平均时长，CPU/frame 是刷新窗口内的比例，CPU/500ms 是按标称 500 ms 帧周期折算的占比。测试固件测的是指定全屏填充负载下的刷屏开销。任务运行标记也会包含少量在该任务执行区间内发生的系统中断。这些数值应连同 SPI 时钟、刷新面积和帧周期一起记录。

2026-11-17 的有效采集、接线通道和统计口径见 [真机实测记录](LCD刷屏对照_真机实测.md)。

