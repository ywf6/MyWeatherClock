/* [阅读说明]
 * STM32 OTA 共用布局：Bootloader、应用、Qt 与打包脚本依赖这些常量，地址必须保持一致。
 */
#ifndef WC_OTA_LAYOUT_H
#define WC_OTA_LAYOUT_H

/* [阅读说明]
 * 应用从 0x08010000 起，到 0x08080000 之前结束，共 448 KiB；应用向量表也要重定位。
 */
#define WC_APP_BASE       0x08010000u
#define WC_APP_LIMIT      0x08080000u
#define WC_APP_MAX        (WC_APP_LIMIT - WC_APP_BASE)
/* [阅读说明]
 * 内部 Flash 0x0800c000 存放已安装应用的 magic、size、CRC32，安装时最后写入有效标记。
 */
#define WC_APP_RECORD     0x0800c000u
#define WC_OTA_MAGIC      0x31414357u
#define WC_BOOT_COOKIE    0x57434231u
/* [阅读说明]
 * RAM 顶部预留 8 字节保存进入 Bootloader 的请求及反码，应用链接布局需避免占用。
 */
#define WC_COOKIE_ADDR    0x2001fff8u
/* [阅读说明]
 * 每次传输 96 字节，转成 ASCII 十六进制后为 192 字符，适配 256 字节命令行缓冲。
 */
#define WC_CHUNK          96u

#endif
