/* [阅读说明]
 * BL24C512 EEPROM 驱动头文件：预留接口，当前实现为空。
 * BL24C512 是 512Kbit（64KB）I2C EEPROM，用于持久化存储配置数据。
 * 当前项目未使用此驱动，保留接口供后续扩展。
 */
#ifndef __BL24C512_H
#define __BL24C512_H

/* TODO: 实现 EEPROM 读写接口 */
/* 预期接口：
 *   void bl24c512_init(void);
 *   bool bl24c512_read(uint16_t addr, uint8_t *data, uint16_t len);
 *   bool bl24c512_write(uint16_t addr, const uint8_t *data, uint16_t len);
 */

#endif /* __BL24C512_H */
