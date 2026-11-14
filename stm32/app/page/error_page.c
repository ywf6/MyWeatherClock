/* [阅读说明]
 * 错误页：联网失败等致命错误时显示的页面。
 * 黑底 + 错误图标 + 居中的黄色错误消息文本。
 * 调用后调用方通常会进入 while(1) 空转，不会返回。
 */
#include <stdint.h>
#include <string.h>
#include "ui.h"

void error_page_display(const char *msg)
{
    /* 背景色：纯黑 */
    const uint16_t color_bg = mkcolor(0, 0, 0);
    
    /* 全屏填充黑色背景 */
    ui_fill_color(0, 0, UI_WIDTH - 1, UI_HEIGHT - 1, color_bg);
    
    /* 错误图标 */
    ui_draw_image(40, 37, &img_error);
    
    /* 错误消息文本居中显示：黄色，20px 粗体 */
    uint16_t startx = 0;
    int len = strlen(msg) * font20_maple_bold.size / 2;
    if (len < UI_WIDTH)
        startx = (UI_WIDTH - len + 1) / 2;
    ui_write_string(startx, 245, msg, mkcolor(255, 255, 0), color_bg, &font20_maple_bold);
}
