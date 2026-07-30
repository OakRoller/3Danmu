#ifndef PLAYER_H
#define PLAYER_H

#include <stdbool.h>

/* 流式播放一个 http(s) mp4 直链(h264+aac)。
 * 阻塞直到播放完或用户按 B 退出。返回 0 正常结束,<0 出错。
 * 需要:gfx 已初始化;NDSP 可用(SD 卡有 dspfirm.cdc 或已内置)。
 * New3DS 优先使用 MVD 硬件解码,失败自动退回软解。 */
int player_play(const char *url, const char *title);

/* 播放偏好(由设置页控制) */
void player_set_prefs(bool danmaku_on, bool force_sw, int qn);
/* 开机调一次:从 SD 存档恢复字幕开关和弹幕/字幕字号 */
void player_prefs_init(void);

/* 当前视频元数据(发弹幕要用 aid+cid) */
void player_set_meta(int64_t aid, int64_t cid, const char *bvid);
/* 登录回调:发弹幕时未登录则调用(阻塞直到登录流程结束),
 * 返回最终是否已登录 */
void player_set_login_cb(bool (*cb)(void));

/* APT 钩子调:HOME 挂起时暂停播放(线程不会自动停,见 player.c 的说明) */
void player_notify_suspend(void);

#endif
