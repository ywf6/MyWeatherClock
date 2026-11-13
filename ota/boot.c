/* [阅读说明]
 * STM32 裸机 Bootloader：复位 -> 检查请求/活动应用/暂存包 -> 安装或跳转，异常时等待升级命令。
 * 应用区从 0x08010000 开始，使用 U32 外置 SPI NOR 尾部暂存完整镜像；不是双应用区回滚方案。
 * CRC32 校验完整性，不提供固件签名认证；安装中断后的恢复路径已实现，仍需真机断电验证。
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "stm32f4xx.h"
#include "layout.h"

#define NOR_START  0x00f80000u
#define NOR_END    0x01000000u
#define NOR_HEADER (NOR_START + 4096u)
#define NOR_DATA   (NOR_HEADER + 4096u)
#define NOR_OWNER  0x314f4357u
#define FLASH_ERRORS (FLASH_SR_SOP | FLASH_SR_WRPERR | FLASH_SR_PGAERR | FLASH_SR_PGPERR | FLASH_SR_PGSERR)

typedef struct {
    uint32_t magic;
    uint32_t size;
    uint32_t crc;
} image_t;

extern void jump_app(uint32_t base);
static uint32_t milliseconds;
static uint8_t block[256];
static image_t download;
static uint32_t received;

static uint32_t ticks(void)
{
    if (SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk) ++milliseconds;
    return milliseconds;
}

static uint32_t crc32(uint32_t crc, const uint8_t *data, uint32_t size)
{
    while (size--) {
        unsigned bit;
        crc ^= *data++;
        for (bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return crc;
}

static bool image_size(uint32_t size)
{
    return size >= 512 && size <= WC_APP_MAX;
}

/* [阅读说明]
 * 校验栈指针落在允许 RAM 区且 8 字节对齐，并检查 Thumb 复位入口位于应用镜像内。
 */
static bool vectors_ok(const uint32_t *vectors, uint32_t size)
{
    uint32_t sp = vectors[0], reset = vectors[1];
    return !(sp & 7u) && sp > 0x20000000u && sp <= WC_COOKIE_ADDR &&
           (reset & 1u) && reset >= WC_APP_BASE + 8u && reset < WC_APP_BASE + size;
}

/* [阅读说明]
 * 检查内部 Flash 的应用记录、向量表和整包 CRC，避免跳入不完整应用。
 */
static bool active_ok(void)
{
    const image_t *record = (const image_t *)WC_APP_RECORD;
    const uint8_t *app = (const uint8_t *)WC_APP_BASE;
    return record->magic == WC_OTA_MAGIC && image_size(record->size) &&
           vectors_ok((const uint32_t *)app, record->size) &&
           ~crc32(~0u, app, record->size) == record->crc;
}

static void alternate(GPIO_TypeDef *gpio, unsigned pin, unsigned af)
{
    unsigned shift = (pin % 8u) * 4u;
    gpio->AFR[pin / 8u] = (gpio->AFR[pin / 8u] & ~(15u << shift)) | (af << shift);
    gpio->MODER = (gpio->MODER & ~(3u << (2u * pin))) | (2u << (2u * pin));
}

static void output_high(GPIO_TypeDef *gpio, unsigned pin)
{
    gpio->BSRR = 1u << pin;
    gpio->MODER = (gpio->MODER & ~(3u << (2u * pin))) | (1u << (2u * pin));
}

void SystemInit(void) {}

static void init(void)
{
    /* Reset clocks: 16 MHz HSI and no interrupts in the loader. */
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOCEN |
                    RCC_AHB1ENR_GPIODEN | RCC_AHB1ENR_GPIOEEN;
    (void)RCC->AHB1ENR;
    GPIOC->PUPDR = (GPIOC->PUPDR & ~(3u << 10)) | (1u << 10);
    GPIOD->PUPDR |= 1u << 18;
    alternate(GPIOD, 8, 7);
    alternate(GPIOD, 9, 7);
    RCC->APB1ENR |= RCC_APB1ENR_USART3EN;
    (void)RCC->APB1ENR;
    USART3->BRR = 139u;
    USART3->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;

    output_high(GPIOA, 15); /* TF card deselected. */
    output_high(GPIOE, 13); /* U31 deselected. */
    output_high(GPIOE, 14); /* U32 OTA storage. */
    alternate(GPIOA, 5, 5);
    alternate(GPIOA, 6, 5);
    alternate(GPIOA, 7, 5);
    RCC->APB2ENR |= RCC_APB2ENR_SPI1EN;
    (void)RCC->APB2ENR;
    SPI1->CR1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI | SPI_CR1_BR_1 | SPI_CR1_SPE;
    SysTick->LOAD = 15999;
    SysTick->VAL = 0;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_ENABLE_Msk;
}

static bool key_down(void) { return !(GPIOC->IDR & (1u << 5)); }

static void uart_write(const char *s)
{
    while (*s) {
        while (!(USART3->SR & USART_SR_TXE)) {}
        USART3->DR = (uint8_t)*s++;
    }
    while (!(USART3->SR & USART_SR_TC)) {}
}

static void reply(const char *body)
{
    uart_write("@WC1 ");
    uart_write(body);
    uart_write("\r\nWCEND\r\n");
}

static uint8_t spi(uint8_t byte)
{
    while (!(SPI1->SR & SPI_SR_TXE)) {}
    *(__IO uint8_t *)&SPI1->DR = byte;
    while (!(SPI1->SR & SPI_SR_RXNE)) {}
    return (uint8_t)SPI1->DR;
}

static void select_nor(void) { GPIOE->BSRR = 1u << 30; }
static void deselect_nor(void)
{
    while (SPI1->SR & SPI_SR_BSY) {}
    GPIOE->BSRR = 1u << 14;
}

static void nor_address(uint32_t address)
{
    spi((uint8_t)(address >> 16));
    spi((uint8_t)(address >> 8));
    spi((uint8_t)address);
}

static uint8_t nor_status(void)
{
    uint8_t status;
    select_nor(); spi(0x05); status = spi(0xff); deselect_nor();
    return status;
}

static bool nor_ready(uint32_t timeout)
{
    uint32_t start = ticks();
    do {
        if (!(nor_status() & 1u)) return true;
    } while (ticks() - start < timeout);
    return false;
}

static bool nor_present(void)
{
    uint32_t id;
    select_nor();
    spi(0x9f);
    id = (uint32_t)spi(0xff) << 16;
    id |= (uint32_t)spi(0xff) << 8;
    id |= spi(0xff);
    deselect_nor();
    return id == 0xef4018u && nor_ready(600);
}

static void nor_read(uint32_t address, void *destination, uint32_t count)
{
    uint8_t *out = destination;
    select_nor(); spi(0x03); nor_address(address);
    while (count--) *out++ = spi(0xff);
    deselect_nor();
}

static bool nor_enable(void)
{
    select_nor(); spi(0x06); deselect_nor();
    return (nor_status() & 2u) != 0;
}

static bool nor_blank(uint32_t address, uint32_t count)
{
    uint32_t i;
    bool blank = true;
    select_nor(); spi(0x03); nor_address(address);
    for (i = 0; i < count; ++i) {
        if (spi(0xff) != 0xff) { blank = false; break; }
    }
    deselect_nor();
    return blank;
}

static bool nor_erase(uint32_t address)
{
    if (address < NOR_HEADER || address >= NOR_END || (address & 4095u) || !nor_enable())
        return false;
    select_nor(); spi(0x20); nor_address(address); deselect_nor();
    return nor_ready(600) && nor_blank(address, 4096);
}

static bool nor_write(uint32_t address, const void *source, uint32_t count)
{
    const uint8_t *in = source;
    uint32_t i;
    if (address < NOR_START || address >= NOR_END || count > NOR_END - address) return false;
    while (count) {
        uint32_t n = 256u - (address & 255u);
        if (n > count) n = count;
        if (!nor_enable()) return false;
        select_nor(); spi(0x02); nor_address(address);
        for (i = 0; i < n; ++i) spi(in[i]);
        deselect_nor();
        if (!nor_ready(30)) return false;
        select_nor(); spi(0x03); nor_address(address);
        for (i = 0; i < n; ++i) {
            if (spi(0xff) != in[i]) { deselect_nor(); return false; }
        }
        deselect_nor();
        in += n; address += n; count -= n;
    }
    return true;
}

static bool nor_owned(void)
{
    uint32_t owner;
    nor_read(NOR_START, &owner, sizeof(owner));
    return owner == NOR_OWNER;
}

/* [阅读说明]
 * 仅在暂存区已有本项目所有权标记，或整个预留区均为空白时使用，避免覆盖未知存储数据。
 */
static bool claim_storage(void)
{
    const uint32_t owner = NOR_OWNER;
    if (nor_owned()) return true;
    return nor_blank(NOR_START, NOR_END - NOR_START) &&
           nor_write(NOR_START, &owner, sizeof(owner));
}

/* [阅读说明]
 * 从外置 NOR 分块计算候选包 CRC，并检查应用向量表，完整验证后才允许安装。
 */
static bool staged_ok(const image_t *image)
{
    uint32_t offset = 0, crc = ~0u, vectors[2];
    if (!image_size(image->size)) return false;
    nor_read(NOR_DATA, vectors, sizeof(vectors));
    if (!vectors_ok(vectors, image->size)) return false;
    while (offset < image->size) {
        uint32_t count = image->size - offset;
        if (count > sizeof(block)) count = sizeof(block);
        nor_read(NOR_DATA + offset, block, count);
        crc = crc32(crc, block, count);
        offset += count;
    }
    return ~crc == image->crc;
}

static bool flash_done(void)
{
    while (FLASH->SR & FLASH_SR_BSY) {}
    return (FLASH->SR & FLASH_ERRORS) == 0;
}

static bool flash_erase(unsigned sector)
{
    if (sector < 3 || sector > 7) return false;
    FLASH->CR = FLASH_CR_PSIZE_1 | FLASH_CR_SER | (sector << 3);
    FLASH->CR |= FLASH_CR_STRT;
    return flash_done();
}

static bool flash_word(uint32_t address, uint32_t word)
{
    FLASH->CR = FLASH_CR_PSIZE_1 | FLASH_CR_PG;
    *(__IO uint32_t *)address = word;
    return flash_done();
}

/* [阅读说明]
 * 先擦除应用记录使旧有效标记失效，再擦应用扇区并从 NOR 拷贝。
 * 完成后复核内部 Flash CRC，最后写入 size/CRC/magic；暂存包保留供中断后重新安装。
 */
static bool install(const image_t *image)
{
    uint32_t offset = 0, i;
    unsigned sector;
    bool ok;
    FLASH->ACR = 0; /* Internal Flash cache is disabled in the bootloader. */
    if (FLASH->CR & FLASH_CR_LOCK) {
        FLASH->KEYR = 0x45670123u;
        FLASH->KEYR = 0xcdef89abu;
    }
    if (FLASH->CR & FLASH_CR_LOCK) return false;
    FLASH->SR = FLASH_ERRORS | FLASH_SR_EOP;
    /* Invalidate the installed record before changing the application. */
    ok = flash_erase(3);
    for (sector = 4; sector <= 7 && ok; ++sector) ok = flash_erase(sector);
    while (ok && offset < image->size) {
        uint32_t count = image->size - offset;
        if (count > sizeof(block)) count = sizeof(block);
        memset(block, 0xff, sizeof(block));
        nor_read(NOR_DATA + offset, block, count);
        for (i = 0; i < count && ok; i += 4) {
            uint32_t word;
            memcpy(&word, block + i, sizeof(word));
            if (word != ~0u) ok = flash_word(WC_APP_BASE + offset + i, word);
        }
        offset += count;
    }
    ok = ok && ~crc32(~0u, (const uint8_t *)WC_APP_BASE, image->size) == image->crc &&
         flash_word(WC_APP_RECORD + 4, image->size) &&
         flash_word(WC_APP_RECORD + 8, image->crc) && flash_word(WC_APP_RECORD, WC_OTA_MAGIC);
    FLASH->CR = FLASH_CR_LOCK;
    return ok && active_ok();
}

/* [阅读说明]
 * 关闭 Bootloader 使用的 SysTick/SPI/USART，设置应用 VTOR，再经 jump.s 切换栈和复位入口。
 */
static void boot_app(void)
{
    SysTick->CTRL = 0;
    SysTick->VAL = 0;
    SPI1->CR1 = 0;
    USART3->CR1 = 0;
    SCB->VTOR = WC_APP_BASE;
    __DSB();
    __ISB();
    jump_app(WC_APP_BASE);
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool number(const char **p, unsigned base, uint32_t *value, char end)
{
    uint32_t result = 0;
    unsigned count = 0;
    int d;
    while ((d = hex_digit(**p)) >= 0 && (unsigned)d < base) {
        if (result > (0xffffffffu - (unsigned)d) / base) return false;
        result = result * base + (unsigned)d;
        ++*p;
        ++count;
    }
    if (!count || **p != end) return false;
    if (end) ++*p;
    *value = result;
    return true;
}

/* [阅读说明]
 * 升级事务：按住 K3 开始 OTABEGIN -> 连续偏移 OTADATA -> OTAEND 验证暂存包并复位。
 * 写 NOR 有擦除粒度和页边界限制，各块到达后逐段处理。
 */
static void handle(const char *line, bool nor)
{
    if (!strcmp(line, "@WC1 HELLO")) {
        reply("HELLO fw=wc-boot-v1 board=STM32F407 protocol=1 caps=ota_boot");
    } else if (!strncmp(line, "@WC1 OTABEGIN ", 14)) {
        uint32_t size, crc;
        const char *p = line + 14;
        download.magic = 0;
        if (!key_down()) { reply("ERR reason=HOLD_K3"); return; }
        if (!number(&p, 10, &size, ' ') || !number(&p, 16, &crc, 0) || !image_size(size)) {
            reply("ERR reason=IMAGE_SIZE"); return;
        }
        if (!nor || !claim_storage()) { reply("ERR reason=STAGING_UNAVAILABLE"); return; }
        if (!nor_erase(NOR_HEADER)) { reply("ERR reason=STAGING_ERASE"); return; }
        download.magic = WC_OTA_MAGIC;
        download.size = size;
        download.crc = crc;
        received = 0;
        reply("ACK cmd=OTABEGIN");
    } else if (!strncmp(line, "@WC1 OTADATA ", 13)) {
        const char *p = line + 13;
        uint32_t offset, count, i, addr, boundary;
        if (!download.magic || !number(&p, 10, &offset, ' ') || offset != received) {
            reply("ERR reason=IMAGE_OFFSET"); return;
        }
        count = download.size - received;
        if (count > WC_CHUNK) count = WC_CHUNK;
        if (!count || strlen(p) != count * 2u) { reply("ERR reason=CHUNK_LENGTH"); return; }
        for (i = 0; i < count; ++i) {
            int hi = hex_digit(p[i * 2]), lo = hex_digit(p[i * 2 + 1]);
            if (hi < 0 || lo < 0) { reply("ERR reason=CHUNK_HEX"); return; }
            block[i] = (uint8_t)((hi << 4) | lo);
        }
        addr = NOR_DATA + received;
        boundary = (addr + 4095u) & ~4095u;
        if ((boundary < addr + count && !nor_erase(boundary)) || !nor_write(addr, block, count)) {
            download.magic = 0;
            reply("ERR reason=STAGING_WRITE"); return;
        }
        received += count;
        reply("ACK cmd=OTADATA");
    } else if (!strcmp(line, "@WC1 OTAEND")) {
        if (!download.magic || received != download.size || !staged_ok(&download)) {
            download.magic = 0;
            reply("ERR reason=IMAGE_CRC"); return;
        }
        if (!nor_write(NOR_HEADER + 4, &download.size, 8) ||
            !nor_write(NOR_HEADER, &download.magic, 4)) {
            download.magic = 0;
            reply("ERR reason=STAGING_RECORD"); return;
        }
        reply("ACK cmd=OTAEND");
        NVIC_SystemReset();
    } else {
        reply("ERR reason=BOOT_MODE");
    }
}

/* [阅读说明]
 * 优先检查 RAM 请求和 K3；未强制留在升级模式时，若有不同且有效的暂存包则安装。
 * 安装中断导致活动应用无效时，可从保留的完整暂存包重新开始安装。
 */
int main(void)
{
    volatile uint32_t *cookie = (volatile uint32_t *)WC_COOKIE_ADDR;
    bool requested = cookie[0] == WC_BOOT_COOKIE && cookie[1] == ~WC_BOOT_COOKIE;
    bool active, nor;
    image_t candidate;
    char line[256];
    unsigned used = 0;
    bool discard = false;
    uint32_t activity;
    cookie[0] = 0; cookie[1] = 0;
    init();
    /* A held K3 on reset also lets the user replace an app that cannot start. */
    requested = requested || key_down();
    nor = nor_present();
    active = active_ok();
    if (!requested && nor && nor_owned()) {
        const image_t *record = (const image_t *)WC_APP_RECORD;
        nor_read(NOR_HEADER, &candidate, sizeof(candidate));
        if (candidate.magic == WC_OTA_MAGIC &&
            (!active || candidate.size != record->size || candidate.crc != record->crc) &&
            staged_ok(&candidate)) {
            active = install(&candidate);
        }
    }
    if (!requested && active) boot_app();
    activity = ticks();
    for (;;) {
        uint32_t sr = USART3->SR;
        uint32_t now = ticks();
        if (active && now - activity >= 20000) boot_app();
        if (sr & (USART_SR_RXNE | USART_SR_ORE | USART_SR_FE | USART_SR_NE)) {
            uint8_t ch = (uint8_t)USART3->DR;
            if (sr & (USART_SR_ORE | USART_SR_FE | USART_SR_NE)) discard = true;
            if (ch == '\n') {
                line[used] = 0;
                if (!discard && used) {
                    handle(line, nor);
                    activity = ticks();
                }
                used = 0;
                discard = false;
            } else if (ch != '\r' && !discard) {
                if (used + 1 < sizeof(line) && ch >= 32 && ch < 127) line[used++] = (char)ch;
                else discard = true;
            }
        }
    }
}
