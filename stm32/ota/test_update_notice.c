/* Host-side persistence and legacy-loader compatibility checks.
 * Run: gcc -std=c99 -Wall -Wextra -Werror ota/test_update_notice.c -o <test.exe> */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "update_notice.h"

int main(void)
{
    uint32_t record[4] = {WC_OTA_MAGIC, 188396u, 0x933d9f63u, UINT32_MAX};
    uint32_t rebooted[4], image_header[3];
    assert(wc_update_notice_pending(record)); /* Legacy bootloader install. */
    memcpy(rebooted, record, sizeof(record));
    assert(wc_update_notice_pending(rebooted)); /* Reset before K3. */
    memcpy(image_header, record, sizeof(image_header));
    record[3] &= WC_NOTICE_READ; /* One-way Flash programming; no erase. */
    assert(!wc_update_notice_pending(record));
    assert(memcmp(record, image_header, sizeof(image_header)) == 0);
    memcpy(rebooted, record, sizeof(record));
    assert(!wc_update_notice_pending(rebooted)); /* Power cycle after K3. */
    record[3] = UINT32_MAX; /* Next OTA install erases the record sector. */
    ++record[2];
    assert(wc_update_notice_pending(record));
    record[3] = 0x00ff0000u; /* Interrupted acknowledgement can be retried. */
    assert(wc_update_notice_pending(record));
    record[0] = UINT32_MAX;
    assert(!wc_update_notice_pending(record));
    record[0] = WC_OTA_MAGIC;
    record[1] = 511u;
    assert(!wc_update_notice_pending(record));
    record[1] = WC_APP_MAX + 1u;
    assert(!wc_update_notice_pending(record));
    puts("OTA notice: persistence, interrupted write, image isolation and bounds passed");
    return 0;
}
