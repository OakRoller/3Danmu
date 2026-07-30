/* ui: 上屏 citro2d 渲染(中文文本 / 列表 / 二维码),下屏 console */
#ifndef UI_H
#define UI_H

#include <3ds.h>
#include <citro2d.h>
#include <stdint.h>

#define UI_COL_BG     C2D_Color32(0x18, 0x18, 0x20, 0xFF)
#define UI_COL_TEXT   C2D_Color32(0xEE, 0xEE, 0xEE, 0xFF)
#define UI_COL_DIM    C2D_Color32(0x99, 0x99, 0xA5, 0xFF)
#define UI_COL_ACCENT C2D_Color32(0xFB, 0x72, 0x99, 0xFF) /* B站粉 */
#define UI_COL_SEL    C2D_Color32(0x2A, 0x2A, 0x3A, 0xFF)
#define UI_COL_WHITE  C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF)
#define UI_COL_BLACK  C2D_Color32(0x00, 0x00, 0x00, 0xFF)

void ui_init(void);   /* gfx/C3D/C2D/字体,下屏 console */
void ui_exit(void);

/* 下屏模式:false = 触控 GUI;true = 全屏调试台(秘技切换) */
void ui_bottom_debug(bool on);   /* 开/关日志页(citro2d 自绘,不切屏幕格式) */
/* 日志页(console 激活时用它画下屏)。传入本帧触摸状态;
 * 返回 true = 用户双击要求退出 */
bool ui_draw_log(bool touch_down, bool touch_held, float tx, float ty);
void ui_log_scroll(int delta);   /* 日志滚动(行) */

/* 统一日志出口:写入环形缓冲(调试台打开时重放历史)+ 台内实时显示。
 * 全工程用它代替 printf,多线程安全 */
void ui_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* 里程碑落盘:写一行到 sdmc:/3ds/3danmu/trace.log,每次都 fclose。
 *
 * 【为什么需要】整机死锁时不会有 Luma 崩溃页,内存里的调试日志也全没了 ——
 * 抠电池重启后什么线索都不剩。这个函数在关键节点各写一行并立即落盘,
 * 死机后读那个文件就知道卡在哪一步。
 * 只在里程碑处调用(一次播放十来行),不要放进逐帧的路径。 */
/* 【全工程唯一的「清晰」字号】
 *
 * 位图字体只有在 1 纹素 = 1 像素时才锐利,所以**清晰的字号只有一个**。
 * ui.c 的 eff_scale 会把这一档附近的字号吸附成精确 1.0;落在窗口外的
 * 字号仍按比例缩放,也就必然发虚。
 *
 * 推论:想让某处清晰,就得接受它和其它清晰文本**一样大**。
 * 「又要清晰又要比别处大」在单张图集上做不到 —— 要么用更大的 -s 重转
 * 字体(整体变大),要么接受发虚。所以凡是要求清晰的地方一律用这个常量,
 * 别再写 0.6 / 0.62 / 0.64 这种「差一点点」的数,那只会掉出吸附窗口。 */
#define UI_SHARP 0.52f

void ui_trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* 同步落盘版:给「写完下一句可能就没机会写了」的地方用,别放进循环 */
void ui_trace_sync(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define printf ui_printf

/* 打印到调试台,非 ASCII 字节替换成 '?'。
 * console 用系统等宽字体,没有中文字形,直接 printf 中文会花屏成乱码;
 * 凡是可能携带 UTF-8 内容的动态字符串(标题、API msg、响应片段)必须走这里 */
void ui_log_ascii(const char *prefix, const char *str, int maxlen);
bool ui_console_active(void);

/* 在同一帧内切到下屏场景(须在 ui_begin 之后、ui_end 之前调用) */
void ui_begin_bottom(void);

/* 触控按钮:绘制 + 命中判定。touched 为本帧是否有触摸按下,tx/ty 为坐标。
 * 返回 true = 本帧被点中 */
bool ui_button(float x, float y, float w, float h, const char *label,
               uint32_t bg, bool touched, float tx, float ty);

void ui_begin(void);  /* 一帧开始(清屏,场景=上屏左眼) */
void ui_begin_top_right(void);  /* 切到上屏右眼(裸眼 3D 用) */
void ui_set_3d(bool on);        /* 上屏立体模式开关 */
float ui_slider_3d(void);       /* 3D 深度滑块 0.0-1.0 */
void ui_clip(float x, float y, float w, float h);  /* 矩形裁剪开始(当前屏) */
void ui_unclip(void);                              /* 裁剪结束 */
void ui_end(void);

void ui_text(float x, float y, float scale, uint32_t color, const char *utf8);
/* 带深度的变体。citro2d 深度测试为 GEQUAL:z 大者在上,同 z 后画者胜。
 * 默认 ui_rect z=0.4、ui_text z=0.5,要盖住文字须 z>0.5 */
void ui_text_z(float x, float y, float z, float scale, uint32_t color,
               const char *utf8);
void ui_rect_z(float x, float y, float z, float w, float h, uint32_t color);
void ui_text_clipped_z(float x, float y, float z, float scale, uint32_t color,
                       const char *utf8, float maxw);
float ui_text_width(const char *utf8, float scale);
/* 指定字号下的**实际行高**(像素)。布局别再硬编码行距 ——
 * 换字体或换 mkbcfnt 的 -s 参数,行高就变了,硬编码必然叠字 */
float ui_text_height(float scale);

/* 全局字号微调(设置页里的「字号 -/+」)。
 * 换字体后各页面字号是否合适,靠肉眼在真机上定最快 —— 每猜一次就重编译、
 * 重传一次太慢。这里开一个运行时旋钮,调顺眼了把值写回 ui.c 的 UI_FONT_K。
 * 只影响整体大小,页面内的字号比例不变。 */
void  ui_font_scale_set(float k);
float ui_font_scale(void);
/* 当前倍率对应的 UI_FONT_TARGET 值(设置页显示,定稿后抄回 ui.c) */
float ui_font_target(void);
/* 字号代次:变一次加一。凡是缓存了「按当前字号量出来的像素宽度」的模块
 * (折行、截断…)都要记住它,变了就重算 */
uint32_t ui_font_gen(void);
/* 按像素宽度截断绘制(超出加 "...") */
void ui_text_clipped(float x, float y, float scale, uint32_t color,
                     const char *utf8, float maxw);
void ui_rect(float x, float y, float w, float h, uint32_t color);

/* 绘制 qrcodegen 输出的二维码(含白色安静区) */
void ui_qr(const uint8_t *qrcode, float cx, float cy, int module_px);

#endif
