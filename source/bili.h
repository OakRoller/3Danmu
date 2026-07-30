/* B 站 web API 封装 */
#ifndef BILI_H
#define BILI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
	char    bvid[16];
	char    title[200];
	char    author[64];
	int64_t cid;       /* 0 = 未知(搜索结果需再查 view 接口) */
	char    pic[192];  /* 封面图 URL(可能为空) */
	int64_t aid;       /* av 号(发弹幕要用,0 = 未知) */
	int     duration;  /* 秒,0 = 未知 */
	int64_t views;     /* -1 = 未知 */
} BiliVideo;

/* 一条评论。text 存原文,折行在绘制时按屏宽算(字体测量不是线程安全的,
 * 只能在主线程做) */
typedef struct {
	char user[40];
	/* 512 而不是 220:220 字节只装得下约 73 个汉字,B站评论经常超过,
	 * 于是正文在接口层就被砍了一截。评论区要完整显示,这里得先够大。
	 * 一页 20 条 x 512B ≈ 10KB,买得起 */
	char text[512];
	int  like;
	int  replies;      /* 楼中楼条数 */
} BiliComment;

/* 拉取评论(按热度排序)。page 从 1 开始。返回 0 成功 */
int bili_comments(int64_t aid, int page, BiliComment *out, int max, int *count);

/* 初始化:加载 cookie、获取 buvid3。net_init 必须先调用。返回 0 成功 */
int bili_init(void);

bool bili_logged_in(void);
/* 最近一次 API 错误描述(UI 显示用) */
const char *bili_last_error(void);
/* 已登录用户名(未登录返回 NULL) */
const char *bili_username(void);

/* 热门视频列表。page 从 1 开始。返回 0 成功,*count 输出条数 */
int bili_popular(int page, BiliVideo *out, int max, int *count);

/* 首页推荐(登录后个性化) */
int bili_recommend(int page, BiliVideo *out, int max, int *count);

/* 历史记录 / 默认收藏夹(均需登录) */
int bili_history(int page, BiliVideo *out, int max, int *count);
int bili_fav(int page, BiliVideo *out, int max, int *count);

/* 视频搜索(需要 WBI 签名) */
int bili_search(const char *keyword, int page, BiliVideo *out, int max, int *count);

/* 查视频 cid(播放前必须有 cid)。aid 可为 NULL;传入且为 0 时顺带回填 */
int bili_get_cid(const char *bvid, int64_t *cid, int64_t *aid);

/* 拉 CC 字幕正文 JSON(malloc,调用方 free)。挑第一条中文轨,
 * 无字幕返回 -1 */
/* 用 bvid 而不是 aid:bvid 来自列表且被播放链路验证过(能放就是对的),
 * aid 在部分列表接口里字段含义不一致,曾导致取到别的视频的字幕 */
int bili_subtitle_fetch(const char *bvid, int64_t aid, int64_t cid,
                        char **body_out,
                        size_t *len_out);
/* 最近一次取到的字幕是否为 AI 生成(ai-zh 轨) */
bool bili_subtitle_is_ai(void);

/* 上报观看进度到账号(需登录)。progress_s = 已看秒数;返回 0 成功。
 * 与官方 App 同源,记录会同步到网页端/手机端的"历史记录" */
int bili_report_history(int64_t aid, int64_t cid, int progress_s);

/* 发弹幕(需登录)。progress_ms = 视频内时间;返回 0 成功 */
int bili_send_danmaku(int64_t aid, int64_t cid, int progress_ms,
                      const char *msg);

/* 取 mp4 直链。qn:16=360P(免登录) 32=480P(需登录) */
int bili_get_play_url(const char *bvid, int64_t cid, int qn, char *url, size_t urllen);

/* 扫码登录 */
int bili_qr_generate(char *qr_url, size_t un, char *qrcode_key, size_t kn);
/* 轮询:*code 输出 0=成功(cookie 已保存) 86101=未扫码 86090=已扫码待确认 86038=过期 */
int bili_qr_poll(const char *qrcode_key, int *code);
void bili_logout(void);

#endif
