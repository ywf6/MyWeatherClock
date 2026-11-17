/* [阅读说明]
 * 屏幕驱动：SPI2 + DMA1 Stream4，负责把 UI 命令转为 ST7789 寄存器和 RGB565 像素。
 * 先读 st7789_write_gram() 和 DMA1_Stream4_IRQHandler()，理解 DMA 与任务信号量如何配合。
 * 寄存器命令仍使用轮询发送；DMA 用于像素数据发送，不能概括为 SPI 全部异步收发。
 */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "stm32f4xx.h"
#include "st7789.h"
#include "font.h"
#include "image.h"

// CLK —— PB13   (SPI2_SCK)
// MOSI —— PC3   (SPI2_MOSI)
// MISO —— PC2   (SPI2_MISO)
// CS —— PE2     (片选，低有效)
// RESET —— PE3  (硬件复位，低有效)
// DC —— PE4     (数据/命令选择：低=命令，高=数据)
// BL —— PE5     (背光控制：高=开)

#define CS_PORT     GPIOE
#define CS_PIN      GPIO_Pin_2
#define RESET_PORT  GPIOE
#define RESET_PIN   GPIO_Pin_3
#define DC_PORT     GPIOE
#define DC_PIN      GPIO_Pin_4
#define BL_PORT     GPIOE
#define BL_PIN      GPIO_Pin_5

/* DMA 像素发送完成信号量：DMA1_Stream4 TC 中断中 give，st7789_write_gram() 中 take */
static SemaphoreHandle_t write_gram_semaphore;

/* ST7789 寄存器初始化序列的前向声明 */
static void st7789_init_display(void);

/**
 * @brief 初始化屏幕控制引脚和 SPI2 复用引脚
 *
 * CS/RESET/DC/BL 配置为推挽输出（初始全置高，背光先关）；
 * PB13(SCK)/PC2(MISO)/PC3(MOSI) 配置为 SPI2 复用功能。
 */
static void st7789_io_init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct;
    GPIO_StructInit(&GPIO_InitStruct);
    
    /* 控制引脚初始状态：CS=高(不选中)，RESET=高(不复位)，DC=高(数据)，BL=低(背光关) */
    GPIO_SetBits(GPIOE, GPIO_Pin_2 | GPIO_Pin_3 | GPIO_Pin_4 | GPIO_Pin_5);
    GPIO_ResetBits(BL_PORT, BL_PIN);
    
    /* 配置 CS/RESET/DC/BL 为推挽输出 */
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_OUT;
    GPIO_InitStruct.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStruct.GPIO_Speed = GPIO_High_Speed;
    GPIO_InitStruct.GPIO_PuPd = GPIO_PuPd_NOPULL;
    GPIO_InitStruct.GPIO_Pin = GPIO_Pin_2 | GPIO_Pin_3 | GPIO_Pin_4 | GPIO_Pin_5;
    GPIO_Init(GPIOE, &GPIO_InitStruct);
    
    /* SPI2 复用引脚配置：PB13=SCK, PC2=MISO, PC3=MOSI */
    GPIO_PinAFConfig(GPIOB, GPIO_PinSource13, GPIO_AF_SPI2);
    GPIO_PinAFConfig(GPIOC, GPIO_PinSource2, GPIO_AF_SPI2);
    GPIO_PinAFConfig(GPIOC, GPIO_PinSource3, GPIO_AF_SPI2);

    /* 配置 SPI 引脚为复用推挽模式 */
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_AF;
    GPIO_InitStruct.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStruct.GPIO_Speed = GPIO_High_Speed;
    GPIO_InitStruct.GPIO_PuPd = GPIO_PuPd_NOPULL;
    GPIO_InitStruct.GPIO_Pin = GPIO_Pin_13;
    GPIO_Init(GPIOB, &GPIO_InitStruct);
    GPIO_InitStruct.GPIO_Pin = GPIO_Pin_2 | GPIO_Pin_3;
    GPIO_Init(GPIOC, &GPIO_InitStruct);
}

/**
 * @brief 初始化 SPI2 外设：主机模式、8 位数据、Mode0、软件 NSS
 *
 * 时钟分频 4（APB1/4），使能 TX DMA 请求。
 * CPOL=Low + CPHA=1Edge 即 SPI Mode0（ST7789 在上升沿采样）。
 */
static void st7789_spi_init(void)
{
    SPI_InitTypeDef SPI_InitStruct;
    SPI_StructInit(&SPI_InitStruct);
    SPI_InitStruct.SPI_Direction = SPI_Direction_2Lines_FullDuplex;  /* 双线全双工 */
    SPI_InitStruct.SPI_Mode = SPI_Mode_Master;                       /* 主机模式 */
    SPI_InitStruct.SPI_DataSize = SPI_DataSize_8b;                   /* 默认 8 位（寄存器阶段） */
    SPI_InitStruct.SPI_CPOL = SPI_CPOL_Low;                          /* 时钟空闲低电平 */
    SPI_InitStruct.SPI_CPHA = SPI_CPHA_1Edge;                        /* 第一个时钟沿采样 (Mode0) */
    SPI_InitStruct.SPI_NSS = SPI_NSS_Soft;                           /* 软件 NSS（用 CS 引脚控制） */
    SPI_InitStruct.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_4;  /* APB1(42MHz)/4 ≈ 10.5MHz */
    SPI_InitStruct.SPI_FirstBit = SPI_FirstBit_MSB;                  /* 高位先传 */
    SPI_Init(SPI2, &SPI_InitStruct);
    SPI_DMACmd(SPI2, SPI_I2S_DMAReq_Tx, ENABLE);                    /* 使能 SPI2 TX DMA 请求 */
    SPI_Cmd(SPI2, ENABLE);                                           /* 使能 SPI2 */
}

/**
 * @brief 初始化 DMA1 Stream4，用于 SPI2 TX 像素数据发送
 *
 * STM32F4 中 SPI2_TX 固定映射到 DMA1 Stream4 Channel0。
 * 配置为：内存→外设、半字(16bit)传输、地址自增（内存）、正常模式、FIFO 使能。
 * 使能传输完成 (TC) 中断，用于通知 st7789_write_gram() 发送结束。
 */
static void st7789_dma_init(void)
{
    DMA_InitTypeDef DMA_InitStruct;
    DMA_StructInit(&DMA_InitStruct);
    DMA_InitStruct.DMA_Channel = DMA_Channel_0;                            /* SPI2_TX 对应 DMA1 Stream4 Ch0 */
    DMA_InitStruct.DMA_PeripheralBaseAddr = (uint32_t)&SPI2->DR;           /* 目标地址：SPI2 数据寄存器 */
    DMA_InitStruct.DMA_DIR = DMA_DIR_MemoryToPeripheral;                   /* 方向：内存 → 外设（发送） */
    DMA_InitStruct.DMA_PeripheralInc = DMA_PeripheralInc_Disable;         /* 外设地址不自增（始终写 DR） */
    DMA_InitStruct.DMA_PeripheralDataSize = DMA_PeripheralDataSize_HalfWord; /* 外设数据宽度：半字(16bit) */
    DMA_InitStruct.DMA_MemoryDataSize = DMA_MemoryDataSize_HalfWord;       /* 内存数据宽度：半字(16bit) */
    DMA_InitStruct.DMA_Mode = DMA_Mode_Normal;                            /* 正常模式（单次传输，非循环） */
    DMA_InitStruct.DMA_Priority = DMA_Priority_High;                      /* 高优先级（屏幕刷新实时性要求高） */
    DMA_InitStruct.DMA_FIFOMode = DMA_FIFOMode_Enable;                    /* 使能 FIFO */
    DMA_InitStruct.DMA_FIFOThreshold = DMA_FIFOThreshold_Full;            /* FIFO 满阈值 */
    DMA_InitStruct.DMA_MemoryBurst = DMA_MemoryBurst_INC8;               /* 内存突发传输 8 节拍 */
    DMA_InitStruct.DMA_PeripheralBurst = DMA_PeripheralBurst_Single;     /* 外设单次突发 */
    DMA_ITConfig(DMA1_Stream4, DMA_IT_TC, ENABLE);                        /* 使能传输完成中断 */
    DMA_Init(DMA1_Stream4, &DMA_InitStruct);
}

/**
 * @brief 配置 DMA1_Stream4 中断优先级
 *
 * 抢占优先级 5、子优先级 0（与 USART1 等其他驱动中断同级）。
 */
static void st7789_int_init(void)
{
    NVIC_InitTypeDef NVIC_InitStructure;
    NVIC_InitStructure.NVIC_IRQChannel = DMA1_Stream4_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 5;   /* 抢占优先级 5 */
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;          /* 子优先级 0 */
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);
    NVIC_SetPriority(DMA1_Stream4_IRQn, 5);                     /* 统一设置优先级编码 */
}

/**
 * @brief 屏幕初始化入口：创建信号量 + 初始化各外设 + 执行 ST7789 寄存器配置
 *
 * 调用顺序：创建信号量 → SPI → DMA → NVIC → GPIO → 显示寄存器初始化
 */
void st7789_init(void)
{
    /* 创建二值信号量，用于 DMA 发送完成中断与 UI 任务同步 */
    write_gram_semaphore = xSemaphoreCreateBinary();
    configASSERT(write_gram_semaphore);    /* 确保信号量创建成功 */
    
    st7789_spi_init();
    st7789_dma_init();
    st7789_int_init();
    st7789_io_init();
    
    st7789_init_display();
}

/* [阅读说明]
 * 命令阶段拉低 DC，参数阶段拉高 DC；本函数逐字节轮询 TXE/BSY，没有使用 DMA。
 */
/**
 * @brief 向 ST7789 写入一条寄存器命令（轮询方式）
 *
 * 时序：CS 拉低 → DC 拉低发命令字节 → DC 拉高发参数字节 → CS 拉高
 * 数据宽度强制设为 8 位（与像素发送的 16 位区分）。
 *
 * @param reg    命令寄存器地址
 * @param data   参数数据数组（可为 NULL）
 * @param length 参数长度（字节数，可为 0）
 */
static void st7789_write_register(uint8_t reg, uint8_t data[], uint16_t length)
{
    SPI_DataSizeConfig(SPI2, SPI_DataSize_8b);   /* 寄存器阶段使用 8 位数据宽度 */
    
    GPIO_ResetBits(CS_PORT, CS_PIN);             /* 片选有效 */
    
    /* 发送命令字节：DC=低表示命令 */
    GPIO_ResetBits(DC_PORT, DC_PIN);
    SPI_SendData(SPI2, reg);
    while (SPI_GetFlagStatus(SPI2, SPI_FLAG_TXE) == RESET);   /* 等待发送缓冲区空 */
    while (SPI_GetFlagStatus(SPI2, SPI_FLAG_BSY) != RESET);   /* 等待移位寄存器空 */
    
    /* 发送参数数据：DC=高表示数据 */
    GPIO_SetBits(DC_PORT, DC_PIN);
    for (uint16_t i = 0; i < length; i++)
    {
        SPI_SendData(SPI2, data[i]);
        while (!SPI_GetFlagStatus(SPI2, SPI_FLAG_TXE));       /* 等待发送缓冲区空 */
    }
    while (SPI_GetFlagStatus(SPI2, SPI_FLAG_BSY) != RESET);   /* 等待最后字节移出 */
    
    GPIO_SetBits(CS_PORT, CS_PIN);              /* 片选释放 */
}

/* [阅读说明]
 * 以 16 位数据宽度发送像素，因此把字节长度除以 2，DMA 计数按半字单位计算。
 * 每块最多 65535 个半字；纯色填充固定源地址，图片发送递增源地址。
 */
/**
 * @brief 通过 DMA 发送 RGB565 像素数据到屏幕 GRAM
 *
 * SPI 切换为 16 位模式，DC 拉高（数据模式）。
 * 字节长度右移 1 得到半字数量，按 65535 半字分块（DMA NDTR 上限）。
 * 纯色填充时关闭内存地址自增（固定源），图片发送时开启自增。
 *
 * @param data         像素数据数组（RGB565，小端字节序）
 * @param length       数据长度（字节数，内部除以 2 转换为半字数）
 * @param singlecolor  true=纯色填充（源地址不自增），false=多色数据（源地址自增）
 */
static void st7789_write_gram(uint8_t data[], uint32_t length, bool singlecolor)
{
    SPI_DataSizeConfig(SPI2, SPI_DataSize_16b);   /* 像素阶段切换为 16 位数据宽度 */
    
    GPIO_ResetBits(CS_PORT, CS_PIN);              /* 片选有效 */
    GPIO_SetBits(DC_PORT, DC_PIN);                /* DC=高，数据模式 */
    
    length >>= 1;   /* 字节数 → 半字数（每个像素 2 字节） */
    
    do
    {
        /* 每块最多 65535 个半字（DMA NDTR 寄存器宽度限制） */
        uint32_t chunk_size = length < 65535 ? length : 65535;

        /* 纯色填充：关闭内存地址自增，DMA 反复读同一地址 */
        /* 图片发送：开启内存地址自增，逐像素读取 */
        /* 注意：此处操作的是 DMA_SxCR_MINC 位（内存地址自增使能位） */
        if (singlecolor) DMA1_Stream4->CR &= ~DMA_SxCR_MINC;  /* 纯色：关闭自增，反复读同一源地址 */
        else             DMA1_Stream4->CR |= DMA_SxCR_MINC;   /* 多色：开启自增，逐像素读取 */
        DMA1_Stream4->M0AR = (uint32_t)data;      /* 设置内存源地址 */
        DMA1_Stream4->NDTR = chunk_size;          /* 设置本次传输半字数 */

        DMA_Cmd(DMA1_Stream4, ENABLE);            /* 启动 DMA 传输 */
        /* [阅读说明]
         * 阻塞的是 UI 任务，DMA 仍在搬运数据，CPU 可调度其他就绪任务。
         * 函数等到 DMA 完成才继续，因此调用期间源缓冲区必须保持有效且不可被覆盖。
         */
        xSemaphoreTake(write_gram_semaphore, portMAX_DELAY);   /* 阻塞等待 DMA TC 中断唤醒 */
        
        /* 多色模式下指针前移到下一块起始位置（半字数 × 2 = 字节数） */
        if (!singlecolor)
            data += chunk_size * 2;
        length -= chunk_size;
    } while (length > 0);
    
    /* [阅读说明]
     * DMA 完成表示最后数据已搬到外设，SPI 仍可能移出最后几位；等待 BSY 清零后才能结束总线事务。
     */
    while (SPI_GetFlagStatus(SPI2, SPI_FLAG_BSY) != RESET);

    GPIO_SetBits(CS_PORT, CS_PIN);    /* 片选释放 */
}

/**
 * @brief 硬件复位 ST7789
 *
 * RESET 拉低至少 10us（此处延时 2ms 保证），拉高后等待 120ms 等待屏幕稳定。
 */
static void st7789_reset(void)
{
    GPIO_ResetBits(RESET_PORT, RESET_PIN);
    vTaskDelay(pdMS_TO_TICKS(2));    /* >10us 即可，2ms 充裕 */
    GPIO_SetBits(RESET_PORT, RESET_PIN);
    vTaskDelay(pdMS_TO_TICKS(120));  /* 等待屏幕内部初始化完成 */
}

/**
 * @brief 开关背光
 *
 * @param on true=开背光，false=关背光
 */
static void st7789_set_backlight(bool on)
{
    GPIO_WriteBit(BL_PORT, BL_PIN, on ? Bit_SET : Bit_RESET);
}

/**
 * @brief ST7789 寄存器初始化序列
 *
 * 执行硬件复位后，依次写入 ST7789 的配置寄存器：
 *   0x11 — 退出睡眠模式
 *   0x36 — MADCTL（显存方向控制，此处设为 0x00=默认方向）
 *   0x3A — COLMOD（颜色格式，0x55=RGB565 16bit）
 *   0xB7 — 电源控制
 *   0xBB — VCOM 电压设置
 *   0xC0/0xC2/0xC4 — 电源相关
 *   0xC6 — 帧率控制
 *   0xD0 — 电源序列
 *   0xD6 — 电源控制
 *   0xE0 — Gamma 正极性校准（两次写入不同序列）
 *   0x21 — 反显开（黑色背景为亮色文字）
 *   0x29 — 开显示
 * 最后清屏为黑色并打开背光。
 */
static void st7789_init_display(void)
{
    st7789_reset();
    
    st7789_write_register(0x11, NULL, 0);    /* Sleep Out：退出睡眠 */
    vTaskDelay(pdMS_TO_TICKS(5));            /* 等待睡眠退出稳定 */
    
    st7789_write_register(0x36, (uint8_t[]){0x00}, 1);    /* MADCTL：默认扫描方向 */
    st7789_write_register(0x3A, (uint8_t[]){0x55}, 1);    /* COLMOD：RGB565 */
    st7789_write_register(0xB7, (uint8_t[]){0x46}, 1);    /* 电源控制 */
    st7789_write_register(0xBB, (uint8_t[]){0x1B}, 1);    /* VCOM=1.4V */
    st7789_write_register(0xC0, (uint8_t[]){0x2C}, 1);    /* 电源控制 */
    st7789_write_register(0xC2, (uint8_t[]){0x01}, 1);    /* 电源控制 */
    st7789_write_register(0xC4, (uint8_t[]){0x20}, 1);    /* 电源控制 */
    st7789_write_register(0xC6, (uint8_t[]){0x0F}, 1);    /* 帧率 60Hz */
    st7789_write_register(0xD0, (uint8_t[]){0xA4,0xA1}, 2);  /* 电源序列 */
    st7789_write_register(0xD6, (uint8_t[]){0xA1}, 1);    /* 电源控制 */
    st7789_write_register(0xE0, (uint8_t[]){0xF0,0x00,0x06,0x04,0x05,0x05,0x31,0x44,0x48,0x36,0x12,0x12,0x2B,0x34}, 14);  /* Gamma 正极性 */
    st7789_write_register(0xE0, (uint8_t[]){0xF0,0x0B,0x0F,0x0F,0x0D,0x26,0x31,0x43,0x47,0x38,0x14,0x14,0x2C,0x32}, 14);  /* Gamma 正极性（第二次） */
    st7789_write_register(0x21, NULL, 0);    /* 反显开 */
    st7789_write_register(0x29, NULL, 0);    /* 显示开 */
    
    /* 清屏为黑色 */
    st7789_fill_color(0, 0, ST7789_WIDTH - 1, ST7789_HEIGHT - 1, 0x0000);
    st7789_set_backlight(true);    /* 打开背光 */
}

/**
 * @brief 检查矩形坐标是否在屏幕有效范围内
 *
 * @return true 坐标合法且 x1<=x2, y1<=y2
 */
static bool in_screen_range(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    if (x1 >= ST7789_WIDTH || y1 >= ST7789_HEIGHT)
        return false;
    if (x2 >= ST7789_WIDTH || y2 >= ST7789_HEIGHT)
        return false;
    if (x1 > x2 || y1 > y2)
        return false;

    return true;
}

/**
 * @brief 设置显示窗口范围并准备写 GRAM
 *
 * 先通过 0x2A 设置列地址范围 (x1~x2)，再通过 0x2B 设置行地址范围 (y1~y2)，
 * 最后发 0x2C 命令进入 GRAM 写入模式，后续数据即被写入指定窗口。
 *
 * @param x1,y1 窗口左上角坐标
 * @param x2,y2 窗口右下角坐标
 */
static void st7789_set_range_and_prepare_gram(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    /* 列地址设置：高字节在前，低字节在后 */
    st7789_write_register(0x2A, (uint8_t[]){(x1 >> 8) & 0xff, x1 & 0xff, (x2 >> 8) & 0xff, x2 & 0xff}, 4);
    /* 行地址设置：高字节在前，低字节在后 */
    st7789_write_register(0x2B, (uint8_t[]){(y1 >> 8) & 0xff, y1 & 0xff, (y2 >> 8) & 0xff, y2 & 0xff}, 4);
    /* Memory Write：进入 GRAM 写入模式 */
    st7789_write_register(0x2C, NULL, 0);
}

/* [阅读说明]
 * 参数是左上角和右下角坐标，边界都包含在填充范围内。
 */
/**
 * @brief 用纯色填充矩形区域
 *
 * @param x1,y1 左上角坐标（含）
 * @param x2,y2 右下角坐标（含）
 * @param color RGB565 颜色值
 */
void st7789_fill_color(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color)
{
    if (!in_screen_range(x1, y1, x2, y2))
        return;
    
    st7789_set_range_and_prepare_gram(x1, y1, x2, y2);
    
    /* 计算像素总数，每个像素 2 字节 */
    uint32_t pixels = (x2 - x1 + 1) * (y2 - y1 + 1);
    st7789_write_gram((uint8_t *)&color, pixels * 2, true);   /* singlecolor=true：纯色填充 */
}

/**
 * @brief 将字体点阵数据展开为 RGB565 像素并写入屏幕
 *
 * 字体为 1bpp（每像素 1 位）位图，逐像素转换为前景色或背景色。
 * 像素缓冲区为静态变量，最大支持 72×72 像素的字符。
 *
 * @param x,y       左上角坐标
 * @param width      字符宽度（像素）
 * @param height     字符高度（像素）
 * @param model      字体点阵数据（每行 (width+7)/8 字节，MSB=最左像素）
 * @param color      前景色（RGB565）
 * @param bg_color   背景色（RGB565）
 */
static void st7789_draw_font(uint16_t x, uint16_t y, uint16_t width, uint16_t height, const uint8_t *model, uint16_t color, uint16_t bg_color)
{
    uint16_t bytes_per_row = (width + 7) / 8;   /* 每行占用的字节数（向上取整） */
    
    static uint8_t buff[72 * 72 * 2];            /* RGB565 像素缓冲区，最大 72×72 像素×2 字节 */
    uint8_t *pbuf = buff;
	for (uint16_t row = 0; row < height; row++)
	{
		const uint8_t *row_data = model + row * bytes_per_row;   /* 当前行点阵数据起始 */
		for (uint16_t col = 0; col < width; col++)
		{
			/* 取当前像素位：字节内 MSB 对应最左像素 */
			uint8_t pixel = row_data[col / 8] & (1 << (7 - col % 8));
            uint16_t pixel_color = pixel ? color : bg_color;   /* 1=前景色，0=背景色 */
            /* RGB565 小端序：低字节在前 */
            *pbuf++ = pixel_color & 0xff;
            *pbuf++ = (pixel_color >> 8) & 0xff;
		}
	}
    
    st7789_set_range_and_prepare_gram(x, y, x + width - 1, y + height - 1);
    st7789_write_gram(buff, pbuf - buff, false);   /* singlecolor=false：多色数据 */
}

/**
 * @brief 从字体表中获取指定 ASCII 字符的点阵数据地址
 *
 * 支持两种字体表格式：
 *   - 有 ascii_map：通过查找映射表定位字符索引
 *   - 无 ascii_map：直接按 ch-' ' 偏移计算（ASCII 顺序排列）
 *
 * @param ch   ASCII 字符 (0x20~0x7E)
 * @param font 字体描述结构体
 * @return     点阵数据指针，未找到返回 NULL
 */
static const uint8_t *ascii_get_model(const char ch, const font_t *font)
{
    uint16_t bytes_per_row = (font->size / 2 + 7) / 8;   /* ASCII 字符宽=高/2，每行字节数 */
    uint16_t bytes_per_char = font->size * bytes_per_row; /* 每个字符占用的总字节数 */
    
    if (font->ascii_map)
    {
        /* 有映射表：遍历查找字符位置 */
        const char *map = font->ascii_map;
        do
        {
            if (*map == ch)
            {
                /* 按映射表中的偏移计算点阵地址 */
                return font->ascii_model + (map - font->ascii_map) * bytes_per_char;
            }
        } while (*(++map) != '\0');
    }
    else
    {
        /* 无映射表：直接按 ASCII 顺序计算偏移（空格 ' ' 为基准） */
        return font->ascii_model + (ch - ' ') * bytes_per_char;
    }
    
    return NULL;
}

/**
 * @brief 在屏幕上绘制单个 ASCII 字符
 *
 * @param x,y       左上角坐标
 * @param ch         ASCII 字符 (0x20~0x7E)
 * @param color      前景色
 * @param bg_color   背景色
 * @param font       字体描述结构体
 */
static void st7789_write_ascii(uint16_t x, uint16_t y, char ch, uint16_t color, uint16_t bg_color, const font_t *font)
{
    if (font == NULL)
        return;
    
    uint16_t fheight = font->size, fwidth = font->size / 2;   /* ASCII 字符宽=字号/2，高=字号 */
    if (!in_screen_range(x, y, x + fwidth - 1, y + fheight - 1))
        return;
    
    if (ch < 0x20 || ch > 0x7E)    /* 非可打印 ASCII 字符 */
        return;
    
	const uint8_t *model = ascii_get_model(ch, font);
    if (model)
        st7789_draw_font(x, y, fwidth, fheight, model, color, bg_color);
}

/**
 * @brief 在屏幕上绘制单个中文字符
 *
 * 中文字符宽=高=字号（正方形）。通过字体表中的 chinese 数组按名称查找。
 *
 * @param x,y       左上角坐标
 * @param ch         中文编码字符串（GB2312 双字节 + '\0'）
 * @param color      前景色
 * @param bg_color   背景色
 * @param font       字体描述结构体
 */
static void st7789_write_chinese(uint16_t x, uint16_t y, const char *ch, uint16_t color, uint16_t bg_color, const font_t *font)
{
    if (ch == NULL || font == NULL)
        return;

    uint16_t fheight = font->size, fwidth = font->size;   /* 中文字符宽=高=字号 */
    if (!in_screen_range(x, y, x + fwidth - 1, y + fheight - 1))
        return;
    
    /* 遍历中文字体表，按名称匹配 */
    const font_chinese_t *c = font->chinese;
    for (; c->name != NULL; c++)
    {
        if (strcmp(c->name, ch) == 0)
            break;
    }
    if (c->name == NULL)    /* 未找到该中文 */
        return;
    
    st7789_draw_font(x, y, fwidth, fheight, c->model, color, bg_color);
}

/**
 * @brief 判断字节是否为 GB2312 编码的首字节
 *
 * GB2312 首字节范围 0xA1~0xF7，双字节编码一个中文字符。
 *
 * @param ch 待判断的字节
 * @return true 是 GB2312 首字节
 */
static bool is_gb2312(char ch)
{
    return ((unsigned char)ch >= 0xA1 && (unsigned char)ch <= 0xF7);
}

//static int utf8_char_length(const char *str)
//{
//    if ((*str & 0x80) == 0) return 1; // 1 byte
//    if ((*str & 0xE0) == 0xC0) return 2; // 2 bytes
//    if ((*str & 0xF0) == 0xE0) return 3; // 3 bytes
//    if ((*str & 0xF8) == 0xF0) return 4; // 4 bytes
//    return -1; // Invalid UTF-8
//}

/**
 * @brief 在屏幕上绘制字符串（支持 ASCII 和 GB2312 中文混合）
 *
 * 逐字节扫描字符串，根据 GB2312 首字节判断区分 ASCII（1 字节）和中文（2 字节）。
 * ASCII 字符宽=字号/2，中文字符宽=字号，绘制后 x 坐标相应前移。
 *
 * @param x,y       起始左上角坐标
 * @param str        字符串（GB2312 编码）
 * @param color      前景色
 * @param bg_color   背景色
 * @param font       字体描述结构体
 */
void st7789_write_string(uint16_t x, uint16_t y, const char *str, uint16_t color, uint16_t bg_color, const font_t *font)
{
    while (*str)
    {
        // int len = utf8_char_length(*str);
        int len = is_gb2312(*str) ? 2 : 1;   /* GB2312 首字节→2 字节中文，否则 1 字节 ASCII */
        if (len <= 0)
        {
            str++;
            continue;
        }
        else if (len == 1)
        {
            /* ASCII 字符：宽=字号/2 */
            st7789_write_ascii(x, y, *str, color, bg_color, font);
            str++;
            x += font->size / 2;
        }
        else
        {
            /* 中文字符：宽=字号，需截取 2 字节 + '\0' */
            char ch[5];
            strncpy(ch, str, len);
            st7789_write_chinese(x, y, ch, color, bg_color, font);
            str += len;
            x += font->size;
        }
    }
}

/**
 * @brief 在屏幕上绘制 RGB565 图片
 *
 * @param x,y   左上角坐标
 * @param image  图片描述结构体（包含宽高和 RGB565 数据指针）
 */
void st7789_draw_image(uint16_t x, uint16_t y, const image_t *image)
{
    /* 边界检查：图片完整区域必须在屏幕内 */
    if (x >= ST7789_WIDTH || y >= ST7789_HEIGHT || 
        x + image->width - 1 >= ST7789_WIDTH || y + image->height + 1 >= ST7789_HEIGHT)
        return;
    
    st7789_set_range_and_prepare_gram(x, y, x + image->width - 1, y + image->height - 1);
    /* 图片数据为 RGB565，每像素 2 字节，singlecolor=false 开启地址自增 */
    st7789_write_gram((uint8_t *)image->data, image->width * image->height * 2, false);
}

/* [阅读说明]
 * DMA 完成中断释放信号量并请求必要的调度。
 * 现状提醒：此处 pxHigherPriorityTaskWoken 尚未显式初始化为 pdFALSE，本轮只注释，未改原逻辑。
 */
/**
 * @brief DMA1 Stream4 中断服务函数：处理 SPI2 TX 传输完成事件
 *
 * DMA 像素发送完成后触发 TC 中断，通过信号量唤醒正在 st7789_write_gram() 中阻塞等待的 UI 任务。
 * pxHigherPriorityTaskWoken 用于判断是否需要触发上下文切换。
 */
void DMA1_Stream4_IRQHandler(void)
{
    if (DMA_GetITStatus(DMA1_Stream4, DMA_IT_TCIF4) == SET)
    {
        BaseType_t pxHigherPriorityTaskWoken;
        /* 释放信号量唤醒 UI 任务，返回是否需要切换上下文 */
        xSemaphoreGiveFromISR(write_gram_semaphore, &pxHigherPriorityTaskWoken);
        /* 若唤醒了更高优先级任务，立即请求上下文切换 */
        portYIELD_FROM_ISR(pxHigherPriorityTaskWoken);
        
        DMA_ClearITPendingBit(DMA1_Stream4, DMA_IT_TCIF4);  /* 清除 DMA 传输完成中断标志 */
    }
}

/* 216 x 208, one bit per pixel, MSB first. Called only by the UI task. */
/* [阅读说明]
 * 将 216x208 的 1bpp 点阵逐行展开为 RGB565；每行仅需 432 字节临时缓冲。
 * 每字节最高位对应最左侧像素，与 Pillow 的 1 模式输出保持一致。
 */
/**
 * @brief 绘制 216×208 的 1bpp 位图（用于 AI 对话页面）
 *
 * 逐行扫描：每行 216 像素，每 8 像素占 1 字节（共 27 字节/行）。
 * 位为 1→白色 (0xFFFF)，位为 0→深蓝背景色 (0x08A4)。
 * 每行转换为 RGB565 后通过 DMA 发送，显示在 (12, 64)~(227, 271) 区域。
 *
 * @param bits 1bpp 位图数据，共 208×27=5616 字节
 */
void st7789_draw_chat_bitmap(const uint8_t *bits)
{
    uint8_t row[216 * 2];   /* 单行 RGB565 缓冲区：216 像素 × 2 字节 = 432 字节 */
    unsigned y, x;
    for (y = 0; y < 208; ++y) {
        for (x = 0; x < 216; ++x) {
            /* 取像素位：bits[y*27 + x/8] 的第 (x%8) 位，MSB 在前 */
            uint16_t color = (bits[y * 27 + x / 8] & (0x80 >> (x % 8)))
                ? 0xFFFF : 0x08A4;     /* 1=白色, 0=深蓝背景 */
            row[x * 2] = color & 0xFF;      /* RGB565 低字节 */
            row[x * 2 + 1] = color >> 8;    /* RGB565 高字节 */
        }
        /* 设置当前行的显示窗口并写入 */
        st7789_set_range_and_prepare_gram(12, 64 + y, 227, 64 + y);
        st7789_write_gram(row, sizeof(row), false);
    }
}
