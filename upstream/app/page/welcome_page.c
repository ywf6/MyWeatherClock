/* [阅读说明]
 * 欢迎页：系统启动时第一个显示的页面，展示梅花 Logo 和产品名称。
 * 在 main_init() 中 ui_init() 之后立即调用，给用户即时视觉反馈。
 * 页面布局：黑底 -> 梅花图片(30,10) -> "梅花嵌入式"标题 -> "天气时钟" -> "loading..."
 */
#include "ui.h"

void welcome_page_display(void)
{
    /* 背景色：纯黑 */
    const uint16_t color_bg = mkcolor(0, 0, 0);
    
    /* 全屏填充黑色背景 */
    ui_fill_color(0, 0, UI_WIDTH - 1, UI_HEIGHT - 1, color_bg);
    
    /* 梅花 Logo 图片，180x180 像素 */
    ui_draw_image(30, 10, &img_meihua);
    
    /* "梅花嵌入式" 标题：粉色，32px 粗体 */
    ui_write_string(40, 205, "梅花嵌入式", mkcolor(237, 128, 147), color_bg, &font32_maple_bold);
    
    /* "天气时钟" 副标题：蓝色，32px 粗体 */
    ui_write_string(56, 233, "天气时钟", mkcolor(86, 165, 255), color_bg, &font32_maple_bold);
    
    /* "loading..." 加载提示：白色，24px 粗体 */
    ui_write_string(60, 285, "loading...", mkcolor(255, 255, 255), color_bg, &font24_maple_bold);
}
