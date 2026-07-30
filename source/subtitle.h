/* CC 字幕:后台拉取 B 站字幕轨(优先中文),播放时在画面底部渲染。
 * 与弹幕同模式:异步加载 + 无锁发布;没有字幕轨时静默不显示。 */
#ifndef SUBTITLE_H
#define SUBTITLE_H

#include <stdint.h>
#include <stdbool.h>

/* 后台线程拉取+解析。aid 也要传:风控最严的那个字幕接口按网页端的
 * 姿势发才不会 412,而网页端发的是 aid+cid */
/* 返回 false = 本次没启动(上一轮还在跑),调用方应稍后重试,
 * 不能当作已经拉过了 */
bool sub_load_async(const char *bvid, int64_t aid, int64_t cid);
/* 告知视频时长(秒):用来识别"字幕明显不属于本视频"并丢弃 */
void sub_set_duration(double seconds);
void sub_free(void);                            /* 释放(join 线程) */
bool sub_available(void);                       /* 已加载且有内容 */
bool sub_loading(void);                         /* 后台仍在拉取 */
int  sub_count(void);                           /* 行数(0=无字幕轨) */

/* 字幕字号:0=小 1=中 2=大(默认中) */
void sub_set_size(int level);

/* 在当前上屏场景绘制 clock(秒)时刻的字幕行(底部居中,带底色) */
void sub_draw(double clock);

#endif
