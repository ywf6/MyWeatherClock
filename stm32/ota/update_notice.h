#ifndef WC_UPDATE_NOTICE_H
#define WC_UPDATE_NOTICE_H

#include <stdbool.h>
#include <stdint.h>
#include "layout.h"

/* Existing bootloaders erase sector 3, then write only magic/size/CRC.
 * Erased or partly programmed read markers must keep the notice pending. */
static inline bool wc_update_notice_pending(const volatile uint32_t *record)
{
    return record[0] == WC_OTA_MAGIC && record[1] >= 512u &&
           record[1] <= WC_APP_MAX && record[3] != WC_NOTICE_READ;
}

#endif
