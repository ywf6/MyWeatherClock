/* [阅读说明]
 * Wi-Fi 连接页：联网等待期间显示的页面，展示 Wi-Fi 图标和 SSID。
 * 在 main_init() 中 wifi_init() 之后、wifi_wait_connect() 之前调用。
 * SSID 文本水平居中显示，底部显示 "连接中" 提示。
 */
#include <stdint.h>
#include <string.h>
#include "wifi.h"
#include "ui.h"
#include "app.h"

void wifi_page_display(void)
{
    /* SSID 来源：编译时配置的 WIFI_SSID 宏 */
    static const char *ssid = WIFI_SSID;
    
    /* 计算 SSID 文本宽度并居中：每个字符宽度 = 字号/2 */
    uint16_t ssid_startx = 0;
    int ssid_len = strlen(ssid) * font20_maple_bold.size / 2;
    if (ssid_len < UI_WIDTH)
        ssid_startx = (UI_WIDTH - ssid_len + 1) / 2;
    
    /* 背景色：纯黑 */
    const uint16_t color_bg = mkcolor(0, 0, 0);
    
    /* 全屏填充黑色背景 */
    ui_fill_color(0, 0, UI_WIDTH - 1, UI_HEIGHT - 1, color_bg);
    
    /* Wi-Fi 图标 */
    ui_draw_image(30, 15, &img_wifi);
    
    /* "WiFi" 标题：青绿色，32px 粗体 */
    ui_write_string(88, 191, "WiFi", mkcolor(0, 255, 234), color_bg, &font32_maple_bold);
    
    /* SSID 名称：白色，20px 粗体，水平居中 */
    ui_write_string(ssid_startx, 231, ssid, mkcolor(255, 255, 255), color_bg, &font20_maple_bold);
    
    /* "连接中" 提示：浅蓝色，24px 粗体 */
    ui_write_string(84, 263, "连接中", mkcolor(148, 198, 255), color_bg, &font24_maple_bold);
}
