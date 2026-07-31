/* 弹幕:拉取 + 解压 + 解析 + 顶屏滚动渲染 */
#ifndef DANMAKU_H
#define DANMAKU_H

#include <stdint.h>
#include <stdbool.h>

/* 同步加载(阻塞):按 cid 拉取弹幕。返回条数,<0 失败 */
int  dm_load(int64_t cid);

/* 异步加载:在空闲核心的后台线程里下载+解压+解析,立即返回。
 * 加载期间 dm_count() 为 0、dm_draw() 不画,完成后自动生效。 */
void dm_load_async(int64_t cid);

/* 释放(会等待后台加载线程结束) */
void dm_free(void);

/* 播放开始/跳转时重置游标 */
void dm_reset(void);

/* 弹幕字号:0=小 1=中 2=大(默认中) */
void dm_set_size(int level);

/* 弹幕覆盖范围(从上屏顶部往下算):0=全屏 1=1/2 2=1/4 3=1/8。
 * 只改可用行数,不改字号 —— 想「弹幕别挡脸」的人要的是这个,
 * 而不是把字缩小。 */
void dm_set_area(int level);
/* 当前范围档位对应的可用行数(设置页显示用) */
int  dm_rows(void);

/* 自己刚发的弹幕:立即在本地显示(高亮色) */
void dm_add_local(const char *text, double t);

/* 在当前 citro2d 上屏场景中绘制 clock(秒)时刻的弹幕。
 * xoff:横向视差偏移(像素)。3D 模式下左眼 +、右眼 -,
 * 让弹幕浮在画面前方;2D 传 0 即可 */
void dm_draw(double clock, float xoff);

int  dm_count(void);
bool dm_loading(void);   /* 后台仍在加载 */

#endif
