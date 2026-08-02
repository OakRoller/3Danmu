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
	/* 分 P 数。0 = 未知(接口没给),1 = 确定只有一 P。
	 * 【为什么值得存这一个 int】播放前要不要发 pagelist 请求就看它:
	 * 3DS 上一次网络往返几百毫秒,而绝大多数视频只有一 P。
	 * 列表接口给了这个数就别再问一遍。 */
	int     pages;
} BiliVideo;

/* 一条评论。text 存原文,折行在绘制时按屏宽算(字体测量不是线程安全的,
 * 只能在主线程做) */
typedef struct {
	char user[40];
	/* 1536 而不是 512:512 字节 ≈ 170 个汉字,而 B 站的长评轻松超过 ——
	 * 于是正文在**接口层**就被砍了一截,后面再怎么折行都补不回来。
	 * (这个数已经从 220 提到 512 一次了,当时按"73 个汉字不够"算的,
	 *  但没往上再想一步:真正写长评的人根本不止一两百字。)
	 * 1536B ≈ 512 个汉字,覆盖绝大多数;一页 20 条 ≈ 30KB,仍然买得起。
	 * 再往上就要连折行的开销一起重新算了(见 comment.c 的 wrap)。 */
	char text[1536];
	int  like;
	int  replies;      /* 楼中楼条数 */
	int64_t ctime;     /* 发布时间(unix 秒),0 = 接口没给 */
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

/* 一个分 P。多 P 视频(合集/番外/课程)每一 P 有自己的 cid,
 * **弹幕、字幕、进度上报全都按 cid 走** —— 只有 cid 换对了才是真的换了一集 */
typedef struct {
	int64_t cid;
	int     page;       /* P 序号,从 1 开始 */
	int     duration;   /* 秒,0 = 未知 */
	char    title[96];  /* part 字段;为空时调用方显示 "P%d" */
} BiliPage;

/* 取分 P 列表。返回 0 成功,*count 输出条数(单 P 视频返回 1 条)。
 * 单 P 视频也走这条路,调用方不必分两种情况写。 */
int bili_pagelist(const char *bvid, BiliPage *out, int max, int *count);

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
