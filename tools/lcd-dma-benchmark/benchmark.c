#include <stdbool.h>

#include "FreeRTOS.h"
#include "task.h"
#include "stm32f4xx.h"
#include "st7789.h"
#include "tim_delay.h"

#define FRAME_PERIOD_MS 500u
#define FRAME_PIN GPIO_Pin_0
#define CPU_PIN GPIO_Pin_1
#define MODE_PIN GPIO_Pin_9

static TaskHandle_t benchmark_task;
static volatile bool frame_active;
static volatile bool task_running;
static bool polling;

static void write_pin(GPIO_TypeDef *port, uint16_t pin, bool high)
{
    if (high) {
        port->BSRR = pin;
    } else {
        port->BSRR = (uint32_t)pin << 16;
    }
}

/* FreeRTOS calls this after selecting the next task. */
void lcd_bench_task_switched(const void *task)
{
    task_running = task == benchmark_task;
    write_pin(GPIOB, CPU_PIN, frame_active && task_running);
}

bool lcd_bench_is_polling(void)
{
    return polling;
}

/* Include the LCD DMA completion handler in the CPU marker. */
void lcd_bench_dma_irq(bool entering)
{
    write_pin(GPIOB, CPU_PIN, frame_active && (entering || task_running));
}

static void init_markers(void)
{
    GPIO_InitTypeDef gpio;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB | RCC_AHB1Periph_GPIOE, ENABLE);
    write_pin(GPIOB, FRAME_PIN | CPU_PIN, false);
    write_pin(GPIOE, MODE_PIN, false);
    GPIO_StructInit(&gpio);
    gpio.GPIO_Mode = GPIO_Mode_OUT;
    gpio.GPIO_Pin = FRAME_PIN | CPU_PIN;
    GPIO_Init(GPIOB, &gpio);
    gpio.GPIO_Pin = MODE_PIN;
    GPIO_Init(GPIOE, &gpio);
}

static void draw_test_frames(void *argument)
{
    TickType_t next_frame;

    (void)argument;
    st7789_init();
    vTaskDelay(pdMS_TO_TICKS(1000));
    next_frame = xTaskGetTickCount();

    for (;;) {
        /* Run the same full-screen RGB565 fill in both modes. */
        write_pin(GPIOE, MODE_PIN, !polling);
        SPI_I2S_DMACmd(SPI2, SPI_I2S_DMAReq_Tx, polling ? DISABLE : ENABLE);
        vTaskDelay(pdMS_TO_TICKS(1));

        write_pin(GPIOB, FRAME_PIN, true);
        frame_active = true;
        write_pin(GPIOB, CPU_PIN, true);
        st7789_fill_color(0, 0, ST7789_WIDTH - 1, ST7789_HEIGHT - 1, 0x07E0);
        frame_active = false;
        write_pin(GPIOB, CPU_PIN, false);
        write_pin(GPIOB, FRAME_PIN, false);

        polling = !polling;
        vTaskDelayUntil(&next_frame, pdMS_TO_TICKS(FRAME_PERIOD_MS));
    }
}

void vAssertCalled(const char *file, int line)
{
    (void)file;
    (void)line;
    taskDISABLE_INTERRUPTS();
    for (;;) {
    }
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    (void)name;
    vAssertCalled(__FILE__, __LINE__);
}

void vApplicationMallocFailedHook(void)
{
    vAssertCalled(__FILE__, __LINE__);
}

int main(void)
{
    BaseType_t result;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOC | RCC_AHB1Periph_DMA1, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_SPI2, ENABLE);
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);
    init_markers();
    tim_delay_init();
    polling = true;
    result = xTaskCreate(draw_test_frames, "lcd-bench", 768, NULL, 2, &benchmark_task);
    configASSERT(result == pdPASS);
    vTaskStartScheduler();
    vAssertCalled(__FILE__, __LINE__);
    return 0;
}
