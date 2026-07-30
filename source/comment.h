/* 评论区:后台拉取,下屏浏览(视频照常在上屏播)。
 *
 * 与字幕/弹幕同一套异步模式:后台线程取数据 + 代际计数作废旧结果。
 * **绝不在渲染循环里发网络请求** —— 那会把画面和弹幕一起冻住
 * (踩过:观看进度上报曾经就是同步发的,主线程整段停摆)。
 */
#ifndef APP_3DANMU_COMMENT_H
#define APP_3DANMU_COMMENT_H

#include <stdint.h>
#include <stdbool.h>

/* 后台拉取第 page 页(1 起)。
 * page==1 = 重新开始(清空已有内容);page>1 = **追加**到已有列表后面。
 * 调用方一般只需要调 page=1,后续续页由滚动到底自动触发。
 * 返回 false = 上一轮还在跑,本次没启动 */
bool comment_load_async(int64_t aid, int page);
void comment_free(void);
bool comment_loading(void);
int  comment_count(void);
int  comment_page(void);

/* 下屏绘制评论列表。touch_* 由调用方从 hid 读好传进来。
 * 返回 true 表示用户要关闭评论区(目前恒为 false:关闭统一走 B 键)。 */
bool comment_draw(bool touch_down, bool touch_held, float tx, float ty);

#endif
