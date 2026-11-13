/* [阅读说明]
 * AHT20 温湿度传感器驱动头文件：声明初始化和测量接口。
 * AHT20 通过 I2C2（PB10=SCL, PB11=SDA, 100kHz）通信。
 * 测量流程：start_measurement -> wait_for_measurement -> read_measurement。
 */
#ifndef __AHT20_H__
#define __AHT20_H__

#include <stdbool.h>
#include <stdint.h>

/* 初始化 I2C2 外设和 AHT20 传感器（含状态检查，最多等待 100ms 就绪） */
bool aht20_init(void);

/* 触发一次温湿度测量（发送 0xAC 命令），返回是否成功发出 */
bool aht20_start_measurement(void);

/* 等待测量完成，最多等待 200ms（20 次 x 10ms），期间让出 CPU */
bool aht20_wait_for_measurement(void);

/* 读取测量结果：温度（℃）和湿度（%RH），6 字节原始数据按公式换算 */
bool aht20_read_measurement(float *temperature, float *humidity);

#endif /* __AHT20_H__ */
