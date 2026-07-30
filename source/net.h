/* net: 基于 3DS httpc 服务的 HTTP 客户端 + cookie 管理 + 流式下载 */
#ifndef NET_H
#define NET_H

#include <3ds.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
	char *data;     /* malloc 分配,net_response_free 释放,总是 NUL 结尾 */
	size_t len;
	int status;     /* HTTP 状态码 */
} HttpResponse;

/* 服务器时钟:3DS 的 RTC/时区常年不准,而 WBI 签名的 wts 一旦偏离
 * 服务端窗口,风控严的接口就一路 412。每次响应的 Date 头都会用来
 * 校准,签名统一用 net_now() 而不是 time(NULL) */
int64_t net_now(void);
bool    net_time_synced(void);

Result net_init(void);
void   net_exit(void);

/* GET/POST,自动带 UA/Referer/Cookie,自动跟随最多 5 次重定向。返回 0 成功 */
int net_get(const char *url, HttpResponse *res);
/* 带自定义 Referer 的 GET(并附带 Origin/Accept-Language,更像网页端)。
 * 部分接口的风控只认具体页面的 Referer,站点根不够 */
int net_get_ref(const char *url, const char *referer, HttpResponse *res);
/* 图片专用通道:不进全局串行锁、失败不触发 httpc 自愈(避免并发线程
 * 互相重置服务)。仅用于封面等可容忍失败的小图,可多线程并发调用 */
int net_get_img(const char *url, HttpResponse *res);
/* 掐断当前在途的**图片**请求(封面)。给 APT 挂起回调用:
 * httpc 的请求没有超时也不能从外面打断,不掐的话按 HOME 要等它自己跑完。
 * API 请求不受影响 —— 那些掐掉会变成用户可见的失败。
 * 可从任意线程调用;没有在途图片请求时是空操作。 */
void net_cancel_img(void);
/* 掐断在途的视频流连接(不影响图片/API)。播放器退出前调,
 * 免得下载线程在半死连接上把 join 拖满超时 */
void net_cancel_streams(void);

/* 开始退出:掐断**所有**在途请求(含视频流),并拒绝之后的任何新请求。
 * 给 APT 的 ONEXIT 钩子用 —— 不这么做,主线程卡在同步请求里时
 * aptMainLoop() 根本轮不到,系统就停在 "Closing software"。不可逆。 */
void net_shutdown_begin(void);
bool net_is_shutting_down(void);

/* 活跃流计数(内部用):httpc 自愈时避开正在播放的流 */
void net_note_stream(int delta);
int net_post_form(const char *url, const char *body, HttpResponse *res);
/* JSON body 的 POST(Gaia 指纹注册 ExClimbWuzhi 要用) */
int net_post_json(const char *url, const char *json, HttpResponse *res);
/* POST 表单(推荐):keys/vals 传原文,由 httpc 服务负责编码。
 * 3DS 上手工拼 body + httpcAddPostDataRaw 会直接请求失败 */
int net_post_fields(const char *url, const char *const *keys,
                    const char *const *vals, int n, HttpResponse *res);
/* 最近一次请求失败的步骤:2=OpenContext 3=BeginRequest 4=状态码 6=下载 */
extern int g_net_last_step;
/* 最近一次请求跟随的重定向次数(排查「每张图为何要几百毫秒」) */
extern int g_net_last_redirects;
void net_response_free(HttpResponse *res);

/* cookie 管理(内存 + SD 卡持久化) */
void        net_set_cookie(const char *name, const char *value);
const char *net_get_cookie(const char *name);          /* 无则返回 NULL */
void        net_clear_cookies(void);
int         net_cookies_load(void);                    /* sdmc:/3ds/3danmu/cookies.txt */
int         net_cookies_save(void);
/* 打印当前 cookie 名单(排障用,由调用方挑时机) */
void        net_log_cookies(const char *tag);

/* 流式下载(播放器用),支持 Range 重定位 */
typedef struct {
	httpcContext ctx;
	bool  open;
	char  url[1024];       /* 跟随重定向后的最终 URL */
	u64   pos;             /* 当前读位置(绝对) */
	u64   size;            /* 资源总大小,0=未知 */
	bool  counted;         /* 已计入活跃流计数(net_note_stream) */
} NetStream;

int  ns_open(NetStream *s, const char *url, u64 offset);
/* 返回读取字节数;0 = EOF;<0 = 错误 */
long ns_read(NetStream *s, void *buf, size_t n);
int  ns_seek(NetStream *s, u64 pos);   /* 通过 Range 重新打开 */
/* 在当前线程重建连接(httpc 上下文有线程亲和性,跨线程复用会挂起) */
int  ns_rebind(NetStream *s);
void ns_close(NetStream *s);

#endif
