/* [阅读说明]
 * USART1 本地调试控制台：PA9/PA10，115200。
 * console_write() 提供 DMA2 Stream7 发送，但 printf 在 board.c 的 fputc 中仍走轮询。
 * USART1 接收回调由 host_console_start() 注册，用于接收本地 @WC1 命令。
 */
#include <stdint.h>
#include <string.h>
#include "FreeRTOS.h"
#include "semphr.h"
#include "stm32f4xx.h"
#include "console.h"

/* DMA 异步发送完成信号量：DMA2_Stream7 传输完成中断中 give，console_write() 中 take */
static SemaphoreHandle_t write_async_semaphore;

/* 串口接收回调函数指针：每收到一个字节在中断中调用一次，由 host_console_start() 注册 */
static console_received_func_t received_func;

/**
 * @brief 配置 USART1 对应的 GPIO 引脚（PA9=TX, PA10=RX）为复用功能
 *
 * 将 PA9、PA10 映射到 USART1 复用功能，设置为高速推挽复用模式。
 */
static void console_io_init(void)
{
    /* 将 PA9、PA10 引脚复用功能选为 USART1 (AF7) */
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource9, GPIO_AF_USART1);
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource10, GPIO_AF_USART1);
    
    GPIO_InitTypeDef GPIO_InitStructure;
    GPIO_StructInit(&GPIO_InitStructure);
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF;               /* 复用功能模式 */
    GPIO_InitStructure.GPIO_Speed = GPIO_High_Speed;           /* 高速，适应 115200 波特率 */
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_9 | GPIO_Pin_10;   /* PA9(TX) + PA10(RX) */
    GPIO_Init(GPIOA, &GPIO_InitStructure);
}

/**
 * @brief 初始化 USART1 外设：115200, 8N1, 收发双模式
 *
 * 配置波特率 115200、无校验、1 停止位、8 位数据宽度；
 * 开启 RXNE 接收非空中断；使能 USART1 的 TX DMA 请求。
 */
static void console_usart_init(void)
{
    USART_InitTypeDef USART_InitStructure;
    USART_StructInit(&USART_InitStructure);

    USART_InitStructure.USART_BaudRate = 115200u;                          /* 波特率 115200 */
    USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None; /* 无硬件流控 */
    USART_InitStructure.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;        /* 同时收发 */
    USART_InitStructure.USART_Parity = USART_Parity_No;                   /* 无校验位 */
    USART_InitStructure.USART_StopBits = USART_StopBits_1;                /* 1 个停止位 */
    USART_InitStructure.USART_WordLength = USART_WordLength_8b;            /* 8 位数据 */
    
    USART_Init(USART1, &USART_InitStructure);
    USART_ITConfig(USART1, USART_IT_RXNE, ENABLE);   /* 使能接收非空中断，用于逐字节接收 */
    USART_DMACmd(USART1, USART_DMAReq_Tx, ENABLE);   /* 使能发送 DMA 请求，配合 DMA2_Stream7 */
    USART_Cmd(USART1, ENABLE);                        /* 使能 USART1 */
}

/**
 * @brief 初始化 DMA2 Stream7，用于 USART1 TX 发送
 *
 * STM32F4 中 USART1_TX 固定映射到 DMA2 Stream7 Channel4。
 * 配置为：内存→外设、字节传输、地址自增（内存）、正常模式、FIFO 使能。
 * 并使能传输完成 (TC) 中断，用于通知 console_write() 发送结束。
 */
static void console_dma_init(void)
{
    DMA_InitTypeDef DMA_InitStruct;
    DMA_StructInit(&DMA_InitStruct);
    DMA_InitStruct.DMA_Channel = DMA_Channel_4;                           /* USART1_TX 对应 DMA2 Stream7 Ch4 */
    DMA_InitStruct.DMA_PeripheralBaseAddr = (uint32_t)&USART1->DR;        /* 目标地址：USART1 数据寄存器 */
    DMA_InitStruct.DMA_DIR = DMA_DIR_MemoryToPeripheral;                  /* 方向：内存 → 外设（发送） */
    DMA_InitStruct.DMA_PeripheralInc = DMA_PeripheralInc_Disable;        /* 外设地址不自增（始终写 DR） */
    DMA_InitStruct.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte; /* 外设数据宽度：字节 */
    DMA_InitStruct.DMA_MemoryInc = DMA_MemoryInc_Enable;                 /* 内存地址自增（逐字节取） */
    DMA_InitStruct.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;         /* 内存数据宽度：字节 */
    DMA_InitStruct.DMA_Mode = DMA_Mode_Normal;                           /* 正常模式（单次传输，非循环） */
    DMA_InitStruct.DMA_Priority = DMA_Priority_Low;                      /* 低优先级 */
    DMA_InitStruct.DMA_FIFOMode = DMA_FIFOMode_Enable;                   /* 使能 FIFO */
    DMA_InitStruct.DMA_FIFOThreshold = DMA_FIFOThreshold_Full;           /* FIFO 满阈值 */
    DMA_InitStruct.DMA_MemoryBurst = DMA_MemoryBurst_INC8;              /* 内存突发传输 8 节拍 */
    DMA_InitStruct.DMA_PeripheralBurst = DMA_PeripheralBurst_Single;    /* 外设单次突发 */
    DMA_ITConfig(DMA2_Stream7, DMA_IT_TC, ENABLE);                       /* 使能传输完成中断 */
    DMA_Init(DMA2_Stream7, &DMA_InitStruct);
}

/**
 * @brief 配置 NVIC 中断：USART1 和 DMA2_Stream7
 *
 * 两者均设为抢占优先级 5、子优先级 0（数值较大=优先级较低，不抢占高优先级任务）。
 */
static void console_int_init(void)
{
    NVIC_InitTypeDef NVIC_InitStructure;
    /* USART1 中断：用于接收字节数据 */
    NVIC_InitStructure.NVIC_IRQChannel = USART1_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 5;   /* 抢占优先级 5 */
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;          /* 子优先级 0 */
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);
    NVIC_SetPriority(USART1_IRQn, 5);                           /* 统一设置优先级编码 */
    
    /* DMA2_Stream7 中断：用于发送完成通知 */
    NVIC_InitStructure.NVIC_IRQChannel = DMA2_Stream7_IRQn;
    NVIC_Init(&NVIC_InitStructure);
    NVIC_SetPriority(DMA2_Stream7_IRQn, 5);
}

/**
 * @brief 控制台初始化入口：创建信号量并初始化各外设
 *
 * 调用顺序：创建信号量 → USART1 → DMA → NVIC → GPIO
 */
void console_init(void)
{
    /* 创建二值信号量，用于 DMA 发送完成中断与发送任务同步 */
    write_async_semaphore = xSemaphoreCreateBinary();
    configASSERT(write_async_semaphore);    /* 确保信号量创建成功 */
    
    console_usart_init();
    console_dma_init();
    console_int_init();
    console_io_init();
}

/* [阅读说明]
 * 启动 DMA 并阻塞等待发送完成信号量，最后等待串口 TC；期间调用方缓冲区必须保持有效。
 */
/**
 * @brief 通过 DMA 异步发送字符串
 *
 * 流程：
 *   1. 计算字符串长度，按 65535 字节分块（DMA NDTR 最大 65535）
 *   2. 设置 DMA 内存地址 (M0AR) 和传输数量 (NDTR)
 *   3. 启动 DMA，阻塞等待传输完成信号量
 *   4. 全部数据 DMA 完成后，再轮询 USART1 TC 标志确保最后字节移出
 *
 * @note 调用期间 str 缓冲区必须保持有效（DMA 异步读取）
 * @note 信号量初始无 give，首次调用 xSemaphoreTake 将阻塞直到第一次 DMA TC 中断
 */
void console_write(const char str[])
{
    uint32_t len = strlen(str);
    do
    {
        /* 每块最多 65535 字节（DMA NDTR 寄存器宽度限制） */
        uint32_t chunk_size = len < 65535 ? len : 65535;

        DMA2_Stream7->M0AR = (uint32_t)str;          /* 设置内存源地址 */
        DMA2_Stream7->NDTR = chunk_size;             /* 设置本次传输数据量 */

        DMA_Cmd(DMA2_Stream7, ENABLE);               /* 启动 DMA 传输 */
        xSemaphoreTake(write_async_semaphore, portMAX_DELAY);  /* 阻塞等待 TC 中断唤醒 */
        
        str += chunk_size;   /* 指针前移到下一块 */
        len -= chunk_size;   /* 更新剩余长度 */
    } while (len > 0);
    
    /* 等待 USART1 发送完成标志（最后字节完全移出移位寄存器） */
    while (USART_GetFlagStatus(USART1, USART_FLAG_TC) == RESET);
    USART_ClearFlag(USART1, USART_FLAG_TC);
}

/**
 * @brief 注册串口接收回调函数
 *
 * @param func 接收回调函数指针，每收到一个字节在中断上下文中调用
 */
void console_received_register(console_received_func_t func)
{
    received_func = func;
}

/* [阅读说明]
 * 收到字节后调用已注册回调，回调同样运行在中断上下文，应只做轻量接收处理。
 */
/**
 * @brief USART1 中断服务函数：处理接收数据
 *
 * 当 RXNE（接收缓冲区非空）标志置位时，读取接收到的字节并调用已注册的回调函数。
 * 回调运行在中断上下文，应只做轻量处理（如存入缓冲区）。
 */
void USART1_IRQHandler(void)
{
    if (USART_GetITStatus(USART1, USART_IT_RXNE) == SET)
    {
        if (received_func != NULL)
        {
            uint8_t data = USART_ReceiveData(USART1);   /* 读取接收到的字节（同时清除 RXNE） */
            received_func(data);                         /* 调用上层回调处理该字节 */
        }
        
        USART_ClearITPendingBit(USART1, USART_IT_RXNE); /* 清除接收中断挂起标志 */
    }
}

/* [阅读说明]
 * 完成中断唤醒发送任务；此处唤醒标志尚未显式初始化为 pdFALSE，后续修复应单独进行。
 */
/**
 * @brief DMA2 Stream7 中断服务函数：处理传输完成事件
 *
 * DMA 发送完成后触发 TC 中断，通过信号量唤醒正在 console_write() 中阻塞等待的任务。
 * pxHigherPriorityTaskWoken 用于判断是否需要触发上下文切换。
 */
void DMA2_Stream7_IRQHandler(void)
{
    if (DMA_GetITStatus(DMA2_Stream7, DMA_IT_TCIF7) == SET)
    {
        BaseType_t pxHigherPriorityTaskWoken;
        /* 释放信号量唤醒发送任务，返回是否需要切换上下文 */
        xSemaphoreGiveFromISR(write_async_semaphore, &pxHigherPriorityTaskWoken);
        /* 若唤醒了更高优先级任务，立即请求上下文切换 */
        portYIELD_FROM_ISR(pxHigherPriorityTaskWoken);
        
        DMA_ClearITPendingBit(DMA2_Stream7, DMA_IT_TCIF7);  /* 清除 DMA 传输完成中断标志 */
    }
}
