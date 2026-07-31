#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "net.h"
#include "ui.h"     /* 日志要经 ui_printf 才会进调试台环形缓冲 */

#define UA "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"
#define REFERER "https://www.bilibili.com"
#define COOKIE_DIR  "sdmc:/3ds/3danmu"
#define COOKIE_FILE COOKIE_DIR "/cookies.txt"
/* 24 不是 16:登录后有 SESSDATA/bili_jct/DedeUserID/DedeUserID__ckMd5/sid,
 * 加上 b_nut/buvid3/buvid4/buvid_fp 就逼近 16。net_set_cookie 满了是
 * **静默丢弃**的 —— 最后加进来的(buvid_fp)正好第一个被吞 */
#define MAX_COOKIES 24
#define MAX_REDIRECTS 5
#define DL_CHUNK (64 * 1024)

/* httpcInit 的参数是"共享内存大小",而这块共享内存正是 POST 上传数据用的。
 * 传 0 时 GET 一切正常,但任何带 body 的 POST 都会在读响应头时返回
 * 0xD8A0A003(HTTP 模块 / InvalidState / 永久错误)——发弹幕、历史上报
 * 全军覆没就是这个原因。大小需 0x1000 对齐 */
#define HTTPC_SHAREDMEM_SIZE (16 * 1024)

typedef struct {
	char name[48];
	char value[512];
} Cookie;

static Cookie s_cookies[MAX_COOKIES];
static int s_ncookies = 0;

/* 最近一次请求(含重定向链)收到的 Set-Cookie 响应头原文。
 * 4096 不是 1024:登录时 B 站一次下发五个 Set-Cookie,光 SESSDATA 一条就
 * 330 字节上下,拼起来轻松过 1K。截断是**静默**的,而且被切掉的正好是排在
 * SESSDATA 后面的 bili_jct —— 表现为「能登录但发不了弹幕、上报不了历史」,
 * 看上去完全是另一个 bug。 */
static char s_setcookie[4096];

/* 短请求互斥:net_get/net_post 全部串行化。
 * 弹幕线程与主线程曾并发调用 httpc,3DS 的 httpc/sslc 对并发短连接
 * 非常脆——一旦服务态被搞坏,之后所有请求都 http error,只能重启程序。
 * 串行化损失很小(短请求最多几百 ms),换来的是服务不再被并发打坏 */
static LightLock s_req_lock;

/* ---------- 在途图片请求的「取消」入口 ----------
 *
 * 背景:封面正在下时按 HOME,要卡好几秒才弹主菜单。挂起时系统要收走无线,
 * 而 httpc 里已经发出去的请求**没有超时也没法从外面打断**,只能等它自己
 * 结束。上面的 s_req_lock 让全程只有一个短请求在途,所以卡的就是这一个。
 *
 * httpcCancelConnection 可以从**另一个线程**把它掐掉(本文件里下载出错时
 * 已经在用它清理半截响应)。所以这里登记一下当前在途的上下文,
 * APT 挂起回调就能一把取消,等待从"几秒"变成"立刻"。
 *
 * 【只登记图片请求】封面是尽力而为的,掐了下次再取即可;API 请求(登录、
 * 播放地址、评论)掐掉会变成用户可见的失败,不值当。判断依据不是 URL,
 * 而是 net_get_img 进来时置的 s_req_cancelable —— 它在 s_req_lock 里面,
 * 同一时刻只有一个请求,不会串。 */
#define NET_INFLIGHT_MAX 4
static LightLock s_cancel_lock;
static struct { httpcContext *ctx; bool img; } s_inflight[NET_INFLIGHT_MAX];
static bool s_req_cancelable = false;   /* 受 s_req_lock 保护 */
/* 一旦置位就不再发起任何新请求 —— 退出流程专用,见 net_shutdown_begin */
static volatile bool s_shutdown = false;

bool net_is_shutting_down(void) { return s_shutdown; }

static void inflight_add(httpcContext *c, bool img) {
	LightLock_Lock(&s_cancel_lock);
	for (int i = 0; i < NET_INFLIGHT_MAX; i++)
		if (!s_inflight[i].ctx) { s_inflight[i].ctx = c; s_inflight[i].img = img; break; }
	LightLock_Unlock(&s_cancel_lock);
}

static void inflight_del(httpcContext *c) {
	LightLock_Lock(&s_cancel_lock);
	for (int i = 0; i < NET_INFLIGHT_MAX; i++)
		if (s_inflight[i].ctx == c) { s_inflight[i].ctx = NULL; break; }
	LightLock_Unlock(&s_cancel_lock);
}

/* 持锁调用 httpcCancelConnection 是故意的:锁保证登记者在这期间不会把
 * 栈上/结构体里的 ctx 关掉走人,否则就是对已失效的上下文发 IPC。 */
static void cancel_inflight(bool only_img) {
	LightLock_Lock(&s_cancel_lock);
	for (int i = 0; i < NET_INFLIGHT_MAX; i++)
		if (s_inflight[i].ctx && (!only_img || s_inflight[i].img))
			httpcCancelConnection(s_inflight[i].ctx);
	LightLock_Unlock(&s_cancel_lock);
}

void net_cancel_img(void) { cancel_inflight(true); }

/* 掐掉在途的**视频流**连接(登记表里 img=false 的那些)。
 * 播放器退出时用:比等 join 超时快几个数量级。
 * 和 net_cancel_img 一样,持 s_cancel_lock 调用,不会碰到已关闭的 ctx。 */
void net_cancel_streams(void) {
	LightLock_Lock(&s_cancel_lock);
	for (int i = 0; i < NET_INFLIGHT_MAX; i++)
		if (s_inflight[i].ctx && !s_inflight[i].img)
			httpcCancelConnection(s_inflight[i].ctx);
	LightLock_Unlock(&s_cancel_lock);
}

/* 【退出/被关闭时调这个】
 * 症状:HOME → X 关闭应用,卡在 "Closing software"。
 * 原因是主线程可能正卡在同步请求里(列表、播放地址都是主线程发的),
 * 而 aptMainLoop() 只有在主循环转起来时才会返回 false —— 主线程不回来,
 * 系统就一直等。清理阶段的 threadJoin 同理。
 * httpc 请求没有超时,标志位它也不看,只能从外面掐。
 * 反正是要退了,连视频流一起掐,不必再区分。 */
void net_shutdown_begin(void) {
	s_shutdown = true;
	cancel_inflight(false);
}

int g_net_last_step = 0;   /* 最近一次请求失败在哪一步(诊断用) */
/* 最近一次请求跟随了几次重定向。http→https 的跳转会计在这里 ——
 * 那意味着一次多余往返 + 一次 TLS 握手,每张图都摊一遍就很可观 */
int g_net_last_redirects = 0;
static volatile int s_streams = 0;   /* 活跃视频流数(自愈时避开) */
void net_note_stream(int delta) { __sync_add_and_fetch(&s_streams, delta); }
/* 【有线程正在 ns_* 调用体内】和 s_streams 不是一回事:
 * s_streams 记的是「连接建立着」,而断线重连期间连接是关着的、计数为 0,
 * 可下载线程**正在 httpcOpenContext/BeginRequest 里面**。这时 API 请求
 * 一失败,httpc_reset 看 s_streams==0 就放行 httpcExit —— 把下载线程
 * 脚下的服务整个拆了,崩在 libc 的 free(实测崩溃链:
 * api_get_ref → httpc_reset → httpcExit → _free_r,数据异常写)。
 * 所以再加一层:只要有线程在流调用体内,reset 一律不做。 */
static volatile int s_ns_busy = 0;

/* 自愈:httpc 服务态坏掉后,Exit+Init 重建会话。
 * 有视频流在跑时不能做(会拔掉人家的连接) */
static void httpc_reset(void) {
	if (s_shutdown) return;               /* 都要退了,重建纯属浪费 */
	if (s_streams > 0 || s_ns_busy > 0) {
		printf("httpc sick but stream busy, skip reset\n");
		return;
	}
	printf("httpc reset...\n");
	httpcExit();
	httpcInit(HTTPC_SHAREDMEM_SIZE);
}

Result net_init(void) {
	LightLock_Init(&s_req_lock);
	LightLock_Init(&s_cancel_lock);
	return httpcInit(HTTPC_SHAREDMEM_SIZE);
}

void net_exit(void) {
	httpcExit();
}

/* ---------- cookies ---------- */

void net_set_cookie(const char *name, const char *value) {
	for (int i = 0; i < s_ncookies; i++) {
		if (!strcmp(s_cookies[i].name, name)) {
			snprintf(s_cookies[i].value, sizeof(s_cookies[i].value), "%s", value);
			return;
		}
	}
	if (s_ncookies < MAX_COOKIES) {
		snprintf(s_cookies[s_ncookies].name, sizeof(s_cookies[0].name), "%s", name);
		snprintf(s_cookies[s_ncookies].value, sizeof(s_cookies[0].value), "%s", value);
		s_ncookies++;
	}
}

const char *net_get_cookie(const char *name) {
	for (int i = 0; i < s_ncookies; i++)
		if (!strcmp(s_cookies[i].name, name))
			return s_cookies[i].value;
	return NULL;
}

void net_clear_cookies(void) { s_ncookies = 0; }

const char *net_last_set_cookie(void) { return s_setcookie; }

/* ---------- 自动吸收响应里的登录 cookie ----------
 *
 * 【为什么必须这么做】3DS 的 httpc 对**同名多个响应头只给得出一份**。
 * B 站登录时一次下发五个 Set-Cookie,我们只捞得到排在最前面的 SESSDATA,
 * bili_jct 永远拿不到 —— 表现为「能登录,但发弹幕和上报历史全失败」。
 * 一次请求捞不全,那就每次请求都捞:浏览器本来就是这么干的,
 * 只要后续任何一个响应把 bili_jct 排在第一位,我们就接住了。
 *
 * 只收白名单里这几个:别的 cookie(风控下发的一次性标记之类)收进来
 * 只会污染 cookie 头。空值和 "deleted" 也要挡 —— 那是服务端在**删** cookie,
 * 照单全收会把好好的登录态覆盖成空字符串。 */
static const char *const ABSORB[] = {
	"SESSDATA", "bili_jct", "DedeUserID", "DedeUserID__ckMd5"
};

static void absorb_from(const char *src) {
	if (!src || !src[0]) return;
	for (size_t k = 0; k < sizeof(ABSORB) / sizeof(ABSORB[0]); k++) {
		const char *name = ABSORB[k];
		size_t nl = strlen(name);
		for (const char *p = src; *p; p++) {
			bool boundary = (p == src) || p[-1] == ';' || p[-1] == ' ' ||
			                p[-1] == ',' || p[-1] == '\n';
			if (!boundary || strncmp(p, name, nl) || p[nl] != '=') continue;
			const char *v = p + nl + 1;
			size_t n = strcspn(v, ";\r\n");
			if (!n || n >= sizeof(s_cookies[0].value)) break;
			char val[sizeof(s_cookies[0].value)];
			memcpy(val, v, n);
			val[n] = 0;
			if (!strcmp(val, "deleted")) break;   /* 服务端在删,不是在设 */
			const char *old = net_get_cookie(name);
			if (old && !strcmp(old, val)) break;  /* 没变,别刷日志 */
			net_set_cookie(name, val);
			/* 只记名字和长度,值是有效凭证 */
			ui_trace("cookie 吸收: %s (%dB%s)", name, (int)n, old ? ",更新" : ",新增");
			break;
		}
	}
}

static void absorb_login_cookies(void) { absorb_from(s_setcookie); }

/* 给 tls.c 那条路用:它自己收到的**全部** Set-Cookie 行(以 '\n' 分隔)
 * 走同一套吸收逻辑,白名单和防覆盖的规则不必写两份 */
void net_absorb_set_cookie(const char *hdr) { absorb_from(hdr); }

static void build_cookie_header(char *out, size_t outlen);

void net_cookie_header(char *out, size_t outlen) { build_cookie_header(out, outlen); }

static void build_cookie_header(char *out, size_t outlen) {
	out[0] = 0;
	size_t o = 0;
	for (int i = 0; i < s_ncookies; i++) {
		int w = snprintf(out + o, outlen - o, "%s%s=%s",
		                 i ? "; " : "", s_cookies[i].name, s_cookies[i].value);
		if (w < 0 || (size_t)w >= outlen - o) break;
		o += (size_t)w;
	}
}

/* 打印当前会随请求发出的 cookie 名单。
 * 必须由调用方在「该有的 cookie 都设好之后」显式调用 ——
 * 早先挂在第一次请求上自动打印,而第一次请求发生在指纹注册之前,
 * 名单里永远不可能有 buvid_fp,白白误导了一轮 */
void net_log_cookies(const char *tag) {
	char names[320]; size_t o = 0;
	for (int i = 0; i < s_ncookies && o < sizeof(names) - 24; i++)
		o += (size_t)snprintf(names + o, sizeof(names) - o, "%s%s",
		                      i ? "," : "", s_cookies[i].name);
	names[o] = 0;
	char hdr[2048];
	build_cookie_header(hdr, sizeof(hdr));
	printf("cookies[%s] n=%d %dB: %s\n", tag ? tag : "-",
	       s_ncookies, (int)strlen(hdr), names);
}

static void cookie_names(char *out, size_t outlen);   /* 定义在下面存盘那一节 */

int net_cookies_load(void) {
	FILE *f = fopen(COOKIE_FILE, "r");
	if (!f) { ui_trace("cookie load: 打不开(文件不存在?)"); return -1; }
	char line[600];
	int n = 0;
	while (fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '=');
		if (!eq) continue;
		*eq = 0;
		char *v = eq + 1;
		char *nl = strpbrk(v, "\r\n");
		if (nl) *nl = 0;
		if (line[0] && v[0]) { net_set_cookie(line, v); n++; }
	}
	fclose(f);
	char names[140];
	cookie_names(names, sizeof(names));
	ui_trace("cookie load: n=%d [%s]", n, names);
	return 0;
}

/* ---------- 存盘 ----------
 *
 * 【为什么要带 who】排查「好好的登录态被写没了」时,只知道「文件变成了
 * 三项」毫无用处 —— 四个调用点写出来的形状差别很大,但事后从文件上
 * **看不出是谁写的**。所以每次落盘都记一行:谁写的、写了几个、都有谁。
 *
 * 【为什么要拦一道】net_cookies_save 是「把内存原样刷到盘上」,
 * 内存要是因为别处的 bug 少了 SESSDATA,这一刷就把盘上好的那份也毁了,
 * 而且**不可逆**。所以加一条单向保险:内存里没有 SESSDATA、盘上却有时,
 * 拒绝写。代价是这种情况下盘上会短暂地比内存旧 —— 那是安全的方向,
 * 下次启动照样能登上。注销是**故意**要清掉的,走 _force 绕开保险。
 */
static void cookie_names(char *out, size_t outlen) {
	size_t o = 0;
	out[0] = 0;
	for (int i = 0; i < s_ncookies && o + 24 < outlen; i++)
		o += (size_t)snprintf(out + o, outlen - o, "%s%s",
		                      i ? "," : "", s_cookies[i].name);
}

static bool file_has_sessdata(void) {
	FILE *f = fopen(COOKIE_FILE, "r");
	if (!f) return false;
	char line[600];
	bool got = false;
	while (fgets(line, sizeof(line), f)) {
		if (!strncmp(line, "SESSDATA=", 9) &&
		    line[9] && line[9] != '\r' && line[9] != '\n') { got = true; break; }
	}
	fclose(f);
	return got;
}

static int cookies_write(const char *who) {
	mkdir("sdmc:/3ds", 0777);
	mkdir(COOKIE_DIR, 0777);
	FILE *f = fopen(COOKIE_FILE, "w");
	if (!f) { ui_trace("cookie save[%s]: fopen 失败", who); return -1; }
	for (int i = 0; i < s_ncookies; i++)
		fprintf(f, "%s=%s\n", s_cookies[i].name, s_cookies[i].value);
	fclose(f);
	char names[140];
	cookie_names(names, sizeof(names));
	ui_trace("cookie save[%s]: n=%d [%s]", who, s_ncookies, names);
	return 0;
}

int net_cookies_save_from(const char *who) {
	if (!who) who = "?";
	if (!net_get_cookie("SESSDATA") && file_has_sessdata()) {
		char names[140];
		cookie_names(names, sizeof(names));
		ui_trace("cookie save[%s]: 拒绝!内存无 SESSDATA 而盘上有 n=%d [%s]",
		         who, s_ncookies, names);
		return -1;
	}
	return cookies_write(who);
}

int net_cookies_save_force(const char *who) { return cookies_write(who ? who : "?"); }

int net_cookies_save(void) { return net_cookies_save_from("?"); }

/* ---------- 请求 ---------- */

/* 本次请求的 Referer 覆盖(仅在持 s_req_lock 期间有效,故不必再加锁)。
 * B 站部分接口的风控会看「浏览器上下文」:通用的站点根 Referer 过不了,
 * 必须是具体视频页。x/player/wbi/v2 就是这类。 */
static const char *s_ref_override = NULL;
/* 本次 POST 用 application/json(同样只在持锁期间有效) */
static bool s_post_json = false;

static void add_common_headers(httpcContext *ctx) {
	httpcAddRequestHeaderField(ctx, "User-Agent", UA);
	httpcAddRequestHeaderField(ctx, "Referer",
	                           s_ref_override ? s_ref_override : REFERER);
	if (s_ref_override) {
		/* 网页端同源请求一定带 Origin;只在覆盖场景加,不影响其它接口 */
		httpcAddRequestHeaderField(ctx, "Origin", REFERER);
		httpcAddRequestHeaderField(ctx, "Accept-Language",
		                           "zh-CN,zh;q=0.9,en;q=0.8");
	}
	httpcAddRequestHeaderField(ctx, "Accept", "*/*");
	/* SESSDATA 单个就 200 多字节,buvid4 上百 —— 1024 会在拼接中途 break,
	 * 排在后面的 cookie 被悄悄截掉 */
	char cookies[2048];
	build_cookie_header(cookies, sizeof(cookies));
	if (cookies[0]) {
		Result rc = httpcAddRequestHeaderField(ctx, "Cookie", cookies);
		if (R_FAILED(rc))
			printf("! cookie hdr rc=%08lx len=%d\n", (unsigned long)rc, (int)strlen(cookies));
	}
}

/* 把相对 Location 转成绝对 URL */
static void resolve_location(const char *base, const char *loc, char *out, size_t outlen) {
	if (!strncmp(loc, "http://", 7) || !strncmp(loc, "https://", 8)) {
		snprintf(out, outlen, "%s", loc);
		return;
	}
	/* 提取 scheme://host */
	const char *p = strstr(base, "://");
	const char *host_end = p ? strchr(p + 3, '/') : NULL;
	size_t n = host_end ? (size_t)(host_end - base) : strlen(base);
	if (n >= outlen) n = outlen - 1;
	memcpy(out, base, n);
	snprintf(out + n, outlen - n, "%s", loc);
}

/* 表单值的 URL 编码(RFC3986 未保留字符原样,其余 %XX) */
static void url_encode(char *dst, size_t dn, const char *src) {
	static const char *hex = "0123456789ABCDEF";
	size_t o = 0;
	for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
		if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
		    (*p >= '0' && *p <= '9') ||
		    *p == '-' || *p == '_' || *p == '.' || *p == '~') {
			if (o + 1 >= dn) break;
			dst[o++] = (char)*p;
		} else {
			if (o + 3 >= dn) break;
			dst[o++] = '%';
			dst[o++] = hex[*p >> 4];
			dst[o++] = hex[*p & 0xF];
		}
	}
	dst[o] = 0;
}

/* ---------- 服务器时钟对齐 ----------
 *
 * WBI 签名带一个 wts 时间戳,服务端会校验它落在合理窗口内。
 * 麻烦在于 **3DS 的时钟基本靠不住**:主机 RTC 常年不准,系统里存的又是
 * 本地时间——时区没设对就直接偏几个小时。偏了之后 wts 一签就废,
 * 风控严的接口(x/player/wbi/v2)每次都还 412,风控松的却照常放行,
 * 于是现象是「别的接口都好,就字幕接口永远 412」,极难往时钟上想。
 *
 * 解法:任何一次 HTTP 响应都带 Date 头,拿它算出与本机时钟的差值,
 * 之后签名一律用 net_now()。不用额外请求,顺手就校准了。 */
static int64_t s_time_off = 0;      /* 服务器时间 - 本机时间(秒) */
static bool s_time_known = false;

/* 民用历 → 天数(Howard Hinnant 算法)。不能用 mktime:它按本地时区
 * 换算,而 Date 头是 GMT —— 那就又踩一遍时区的坑 */
static int64_t days_from_civil(int y, unsigned m, unsigned d) {
	y -= (m <= 2);
	int64_t era = (y >= 0 ? y : y - 399) / 400;
	unsigned yoe = (unsigned)(y - era * 400);
	unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
	unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
	return era * 146097 + (int64_t)doe - 719468;
}

/* "Mon, 27 Jul 2026 07:09:00 GMT" → epoch 秒;失败返回 0 */
static int64_t parse_http_date(const char *s) {
	static const char *mon = "JanFebMarAprMayJunJulAugSepOctNovDec";
	int d = 0, y = 0, h = 0, mi = 0, se = 0;
	char mname[8] = {0};
	const char *c = strchr(s, ',');
	if (c) s = c + 1;
	if (sscanf(s, " %d %7s %d %d:%d:%d", &d, mname, &y, &h, &mi, &se) != 6)
		return 0;
	const char *mp = strstr(mon, mname);
	if (!mp || y < 2000 || d < 1 || d > 31) return 0;
	unsigned m = (unsigned)((mp - mon) / 3 + 1);
	return days_from_civil(y, m, (unsigned)d) * 86400 +
	       (int64_t)h * 3600 + (int64_t)mi * 60 + se;
}

static void learn_server_time(httpcContext *ctx) {
	char date[64] = {0};
	if (R_FAILED(httpcGetResponseHeader(ctx, "Date", date, sizeof(date))) ||
	    !date[0])
		return;
	int64_t t = parse_http_date(date);
	if (!t) return;
	int64_t off = t - (int64_t)time(NULL);
	if (!s_time_known) {
		s_time_known = true;
		/* 只在偏差大到会影响签名时吭声,平时不刷屏 */
		if (off > 60 || off < -60)
			printf("clock skew %ds vs server, corrected\n", (int)off);
	}
	s_time_off = off;
}

int64_t net_now(void) { return (int64_t)time(NULL) + s_time_off; }
bool net_time_synced(void) { return s_time_known; }

/* POST 表单字段(name/value 原文,由上面的 url_encode 负责编码) */
typedef struct { const char *k, *v; } PostField;

static int do_request_ka(HTTPC_RequestMethod method, const char *url,
                         const char *body, const PostField *fields, int nf,
                         HttpResponse *res, int depth, bool keepalive);

static int do_request_ex(HTTPC_RequestMethod method, const char *url,
                         const char *body, const PostField *fields, int nf,
                         HttpResponse *res, int depth) {
	return do_request_ka(method, url, body, fields, nf, res, depth, false);
}

static int do_request(HTTPC_RequestMethod method, const char *url,
                      const char *body, HttpResponse *res, int depth) {
	return do_request_ex(method, url, body, NULL, 0, res, depth);
}

/* 关闭上下文并撤销取消登记。do_request_ka 的每一条出口都必须走这里。 */
static void close_ctx(httpcContext *ctx) {
	inflight_del(ctx);
	httpcCloseContext(ctx);
}

static int do_request_ka(HTTPC_RequestMethod method, const char *url,
                         const char *body, const PostField *fields, int nf,
                         HttpResponse *res, int depth, bool keepalive) {
	if (s_shutdown) return -1;   /* 正在退出:别再开新连接,否则清理永远等不完 */
	if (depth > MAX_REDIRECTS) return -1;
	res->data = NULL; res->len = 0; res->status = 0;
	/* 只在最外层清:跟随重定向时要保留中途那一跳设的 cookie */
	if (depth == 0) s_setcookie[0] = 0;

	httpcContext ctx;
	Result rc = httpcOpenContext(&ctx, method, url, 1);
	if (R_FAILED(rc)) return -2;
	/* 登记为「可取消」,直到下面任意一条路径把它关掉。
	 * 所有出口都走 close_ctx(),别再直接写 httpcCloseContext —— 漏一处
	 * 就会留下一个指向已失效栈变量的登记项。 */
	inflight_add(&ctx, s_req_cancelable);

	httpcSetSSLOpt(&ctx, SSLCOPT_DisableVerify); /* 3DS 根证书太旧,禁用校验 */
	/* keep-alive 分场景:
	 *   API(默认 false):关。开着时若某次响应没读干净,后一个请求会读到
	 *     前一个响应的残留 —— 实测症状是"字幕内容是别的视频的"。
	 *   图片(true):开。封面是同一主机连续 20 张,每张都重新 TLS 握手会
	 *     把系统核吃满,列表滚动明显掉帧;而图片就算串了也只是显示错封面,
	 *     且它们已被全局锁串行化。 */
	httpcSetKeepAlive(&ctx, (keepalive && method == HTTPC_METHOD_GET)
	                        ? HTTPC_KEEPALIVE_ENABLED
	                        : HTTPC_KEEPALIVE_DISABLED);
	add_common_headers(&ctx);

	void *postbuf = NULL;
	if (method == HTTPC_METHOD_POST) {
		/* 组装 x-www-form-urlencoded body。
		 * 关键:httpcAddPostDataRaw 走 3DS IPC 缓冲,指针必须是
		 * linear 内存且 32 字节对齐——栈上的 char[] 会让请求在
		 * 读响应头那一步失败(step4)。这是踩了两轮才定位到的 */
		char tmp[2560];   /* Gaia 指纹 payload 有 1KB 出头 */
		size_t len = 0;
		if (fields && nf > 0) {
			for (int i = 0; i < nf && len < sizeof(tmp) - 2; i++) {
				if (i) tmp[len++] = '&';
				len += (size_t)snprintf(tmp + len, sizeof(tmp) - len,
				                        "%s=", fields[i].k);
				char enc[512];
				url_encode(enc, sizeof(enc), fields[i].v);
				len += (size_t)snprintf(tmp + len, sizeof(tmp) - len,
				                        "%s", enc);
			}
		} else if (body) {
			len = (size_t)snprintf(tmp, sizeof(tmp), "%s", body);
		}
		if (len > 0) {
			postbuf = linearAlloc((len + 31) & ~31u);
			if (postbuf) {
				memcpy(postbuf, tmp, len);
				httpcAddRequestHeaderField(&ctx, "Content-Type",
					s_post_json ? "application/json"
					            : "application/x-www-form-urlencoded");
				httpcAddPostDataRaw(&ctx, (const u32 *)postbuf, (u32)len);
			}
		}
	}

	rc = httpcBeginRequest(&ctx);
	if (R_FAILED(rc)) {
		printf("begin rc=%08lx\n", (unsigned long)rc);
		close_ctx(&ctx);
		if (postbuf) linearFree(postbuf);
		return -3;
	}

	if (depth == 0) g_net_last_redirects = 0;
	u32 status = 0;
	/* 必须用带超时的版本:非超时版在响应稍慢时直接返回失败
	 * (POST 尤其明显,表现为 step4)。给 20 秒 */
	rc = httpcGetResponseStatusCodeTimeout(&ctx, &status,
	                                       20ULL * 1000 * 1000 * 1000);
	if (R_FAILED(rc)) {
		printf("status rc=%08lx\n", (unsigned long)rc);
		httpcCancelConnection(&ctx);
		close_ctx(&ctx);
		if (postbuf) linearFree(postbuf);
		return -4;
	}

	learn_server_time(&ctx);

	/* 【Set-Cookie 留档】扫码登录曾经只从 poll 返回的 data.url 的 query 里
	 * 取 SESSDATA —— 服务端一改返回格式,那条路就悄无声息地断了(url 还在、
	 * code 还是 0,只是参数没了)。响应头才是设 cookie 的正经渠道,这里
	 * 统一留一份,谁需要谁取。
	 * 注意:3DS 的 httpc 对同名多个响应头只给得出一份,所以这里拿到的可能
	 * 不是全部 —— 当兜底用,不能当唯一来源。 */
	/* 直接读进静态缓冲 —— 4KB 放栈上对 3DS 的线程栈太奢侈。
	 * 每一跳都读、每一跳都吸收:重定向链中途设的 cookie 同样算数。 */
	if (R_FAILED(httpcGetResponseHeader(&ctx, "Set-Cookie",
	                                    s_setcookie, sizeof(s_setcookie))))
		s_setcookie[0] = 0;
	absorb_login_cookies();

	if (status >= 301 && status <= 308 && status != 304) {
		char loc[1024] = {0};
		if (R_SUCCEEDED(httpcGetResponseHeader(&ctx, "Location", loc, sizeof(loc))) && loc[0]) {
			char next[1024];
			resolve_location(url, loc, next, sizeof(next));
			close_ctx(&ctx);
			if (postbuf) linearFree(postbuf);
			g_net_last_redirects++;
			/* 302 → GET */
			return do_request_ka(HTTPC_METHOD_GET, next, NULL, NULL, 0,
			                     res, depth + 1, keepalive);
		}
	}

	size_t cap = DL_CHUNK;
	char *buf = (char *)malloc(cap + 1);
	if (!buf) {
		close_ctx(&ctx);
		if (postbuf) linearFree(postbuf);
		return -5;
	}
	size_t total = 0;

	for (;;) {
		if (total + DL_CHUNK > cap) {
			cap *= 2;
			char *nb = (char *)realloc(buf, cap + 1);
			if (!nb) {
				free(buf);
				close_ctx(&ctx);
				if (postbuf) linearFree(postbuf);
				return -5;
			}
			buf = nb;
		}
		u32 got = 0;
		rc = httpcDownloadData(&ctx, (u8 *)buf + total, DL_CHUNK, &got);
		total += got;
		if (rc == (Result)HTTPC_RESULTCODE_DOWNLOADPENDING)
			continue;
		if (R_FAILED(rc)) {
			free(buf);
			httpcCancelConnection(&ctx);   /* 半截的响应不能留给下一个请求 */
			close_ctx(&ctx);
			if (postbuf) linearFree(postbuf);
			return -6;
		}
		break;
	}
	close_ctx(&ctx);
	if (postbuf) linearFree(postbuf);

	buf[total] = 0;
	res->data = buf;
	res->len = total;
	res->status = (int)status;
	return 0;
}

static int locked_request(HTTPC_RequestMethod m, const char *url,
                          const char *body, HttpResponse *res) {
	LightLock_Lock(&s_req_lock);
	int r = do_request(m, url, body, res, 0);
	if (r < 0) {
		printf("net fail step%d\n", -r);   /* 2=open 3=begin 4=status 6=dl */
		/* 传输层失败 → 重建 httpc 会话再试一次(自愈) */
		if (r <= -2) {
			httpc_reset();
			r = do_request(m, url, body, res, 0);
			if (r < 0) printf("net fail again step%d\n", -r);
		}
	}
	LightLock_Unlock(&s_req_lock);
	return r;
}

int net_get(const char *url, HttpResponse *res) {
	return locked_request(HTTPC_METHOD_GET, url, NULL, res);
}

/* 带自定义 Referer 的 GET(referer 为 NULL 时与 net_get 等价) */
int net_get_ref(const char *url, const char *referer, HttpResponse *res) {
	LightLock_Lock(&s_req_lock);
	s_ref_override = (referer && referer[0]) ? referer : NULL;
	int r = do_request(HTTPC_METHOD_GET, url, NULL, res, 0);
	if (r <= -2) {                       /* 传输层失败:自愈后重试一次 */
		printf("net fail step%d\n", -r);
		httpc_reset();
		r = do_request(HTTPC_METHOD_GET, url, NULL, res, 0);
	}
	s_ref_override = NULL;
	LightLock_Unlock(&s_req_lock);
	return r;
}

int net_get_img(const char *url, HttpResponse *res) {
	/* 【图片请求必须独占,别再试图并发了】
	 *
	 * 试过让图片彼此并发(API 仍独占,并且 load_list 已改成先 thumb_stop
	 * 再发接口,结构上两者不会碰面)。结果:
	 *   - 封面速度**没有明显改善** —— 说明瓶颈不在我们的串行锁,
	 *     而在 3DS 的 httpc 服务本身,它内部就是一个一个处理的;
	 *   - 字幕串台**小概率复发** —— 底层压根不支持并发短连接。
	 * 也就是说并发既没收益又有代价,没有任何理由保留。
	 *
	 * 结论:封面慢是**每请求延迟**造成的(实测 20 张共 46KB 却要 16.5 秒),
	 * 要提速只能从「减少请求数」或「查清单次为何要几百毫秒」下手,
	 * 而不是加并发路数。 */
	LightLock_Lock(&s_req_lock);
	s_req_cancelable = true;    /* 本次允许被 net_cancel_img 掐掉 */
	int r = do_request_ka(HTTPC_METHOD_GET, url, NULL, NULL, 0, res, 0, true);
	s_req_cancelable = false;
	LightLock_Unlock(&s_req_lock);
	return r;
}

int net_post_form(const char *url, const char *body, HttpResponse *res) {
	return locked_request(HTTPC_METHOD_POST, url, body, res);
}

/* JSON body 的 POST(Gaia 指纹注册要用) */
int net_post_json(const char *url, const char *json, HttpResponse *res) {
	LightLock_Lock(&s_req_lock);
	s_post_json = true;
	int r = do_request(HTTPC_METHOD_POST, url, json, res, 0);
	s_post_json = false;
	LightLock_Unlock(&s_req_lock);
	return r;
}

int net_post_fields(const char *url, const char *const *keys,
                    const char *const *vals, int n, HttpResponse *res) {
	if (n > 24) n = 24;
	PostField f[24];
	for (int i = 0; i < n; i++) { f[i].k = keys[i]; f[i].v = vals[i]; }
	LightLock_Lock(&s_req_lock);
	int r = do_request_ex(HTTPC_METHOD_POST, url, NULL, f, n, res, 0);
	if (r < 0) {
		g_net_last_step = -r;
		printf("post fail step%d\n", -r);
		httpc_reset();
		r = do_request_ex(HTTPC_METHOD_POST, url, NULL, f, n, res, 0);
		if (r < 0) { g_net_last_step = -r; printf("post fail again step%d\n", -r); }
	}
	LightLock_Unlock(&s_req_lock);
	return r;
}

void net_response_free(HttpResponse *res) {
	free(res->data);
	res->data = NULL;
	res->len = 0;
}

/* ---------- 流式下载 ---------- */

/* 关流上下文并撤销登记。ns_open_internal 的每条出口都走这里。 */
static void close_sctx(NetStream *s) {
	inflight_del(&s->ctx);
	httpcCloseContext(&s->ctx);
}

static int ns_open_internal(NetStream *s, const char *url, u64 offset, int depth) {
	if (depth > MAX_REDIRECTS) return -1;

	if (s_shutdown) return -1;
	Result rc = httpcOpenContext(&s->ctx, HTTPC_METHOD_GET, url, 1);
	if (R_FAILED(rc)) return -2;
	/* 流也登记进去:退出时要能一起掐,否则下载线程卡在 ns_read 里收不回来。
	 * img=false —— 按 HOME 时不掐流,那会打断正在看的视频。 */
	inflight_add(&s->ctx, false);
	httpcSetSSLOpt(&s->ctx, SSLCOPT_DisableVerify);
	httpcSetKeepAlive(&s->ctx, HTTPC_KEEPALIVE_ENABLED);
	add_common_headers(&s->ctx);

	/* 手动拼 Range(newlib 可能不支持 %llu),offset 为 0 时不发该头 */
	if (offset > 0) {
		char range[64] = "bytes=";
		char tmp[24];
		int i = 0, o = 6;
		u64 u = offset;
		do { tmp[i++] = (char)('0' + (u % 10)); u /= 10; } while (u);
		while (i) range[o++] = tmp[--i];
		range[o++] = '-';
		range[o] = 0;
		httpcAddRequestHeaderField(&s->ctx, "Range", range);
	}

	rc = httpcBeginRequest(&s->ctx);
	if (R_FAILED(rc)) { close_sctx(s); return -3; }

	u32 status = 0;
	/* 带超时:无超时版遇到「连上了但服务器不回话」会永远等下去,
	 * 下载线程随之卡死,B 退出只能靠 join 超时兜底。CDN 偶发这种半死
	 * 连接,15 秒足够区分「慢」和「死」;超时返回错误,上层重连接管。 */
	rc = httpcGetResponseStatusCodeTimeout(&s->ctx, &status,
	                                       15ULL * 1000 * 1000 * 1000);
	if (R_FAILED(rc)) { close_sctx(s); return -4; }

	if (status >= 301 && status <= 308 && status != 304) {
		char loc[1024] = {0};
		if (R_SUCCEEDED(httpcGetResponseHeader(&s->ctx, "Location", loc, sizeof(loc))) && loc[0]) {
			char next[1024];
			resolve_location(url, loc, next, sizeof(next));
			close_sctx(s);
			return ns_open_internal(s, next, offset, depth + 1);
		}
	}
	if (status != 200 && status != 206) {
		close_sctx(s);
		return -5;
	}

	u32 dl = 0, content = 0;
	bool have_len = R_SUCCEEDED(httpcGetDownloadSizeState(&s->ctx, &dl, &content)) && content > 0;

	if (status == 200 && offset > 0) {
		/* 服务器忽略了 Range,返回的是整个文件:手动丢弃前 offset 字节,
		 * 否则解封装读到错位数据(Invalid NAL unit / partial file)。
		 * 超过 8MB 就放弃,不然长视频会在这里下载几百 MB 假装"connecting" */
		if (offset > 8u * 1024 * 1024) {
			printf("range ignored & offset too big, abort\n");
			close_sctx(s);
			return -7;
		}
		printf("range ignored, skip %lu bytes\n", (unsigned long)offset);
		static u8 skipbuf[16384];
		u64 skip = offset;
		while (skip > 0) {
			u32 want = skip > sizeof(skipbuf) ? (u32)sizeof(skipbuf) : (u32)skip;
			u32 got = 0;
			Result r2 = httpcDownloadData(&s->ctx, skipbuf, want, &got);
			skip -= got;
			if (r2 == (Result)HTTPC_RESULTCODE_DOWNLOADPENDING) continue;
			if (R_FAILED(r2) || got == 0) { close_sctx(s); return -6; }
		}
		s->size = have_len ? content : 0;             /* 200:长度即全长 */
	} else {
		s->size = have_len ? offset + content : 0;    /* 206:长度是剩余部分 */
	}

	snprintf(s->url, sizeof(s->url), "%s", url);
	s->pos = offset;
	s->open = true;
	return 0;
}

int ns_open(NetStream *s, const char *url, u64 offset) {
	memset(s, 0, sizeof(*s));
	__sync_add_and_fetch(&s_ns_busy, 1);
	int r = ns_open_internal(s, url, offset, 0);
	if (r == 0 && !s->counted) { s->counted = true; net_note_stream(+1); }
	__sync_sub_and_fetch(&s_ns_busy, 1);
	return r;
}

static long ns_read_inner(NetStream *s, void *buf, size_t n) {
	if (!s->open) return -1;
	u32 got = 0;
	Result rc = httpcDownloadData(&s->ctx, (u8 *)buf, (u32)n, &got);
	if (rc == (Result)HTTPC_RESULTCODE_DOWNLOADPENDING || R_SUCCEEDED(rc)) {
		s->pos += got;
		if (got == 0 && R_SUCCEEDED(rc))
			return 0; /* EOF */
		return (long)got;
	}
	/* 连接断了:按当前位置重连一次 */
	printf("stream drop, reconnect @%lu\n", (unsigned long)s->pos);
	u64 pos = s->pos;
	char url[1024];
	snprintf(url, sizeof(url), "%s", s->url);
	ns_close(s);
	if (ns_open_internal(s, url, pos, 0) != 0) return -2;
	if (!s->counted) { s->counted = true; net_note_stream(+1); }
	rc = httpcDownloadData(&s->ctx, (u8 *)buf, (u32)n, &got);
	if (rc == (Result)HTTPC_RESULTCODE_DOWNLOADPENDING || R_SUCCEEDED(rc)) {
		s->pos += got;
		return (got == 0 && R_SUCCEEDED(rc)) ? 0 : (long)got;
	}
	return -2;
}

long ns_read(NetStream *s, void *buf, size_t n) {
	__sync_add_and_fetch(&s_ns_busy, 1);
	long r = ns_read_inner(s, buf, n);
	__sync_sub_and_fetch(&s_ns_busy, 1);
	return r;
}

int ns_rebind(NetStream *s) {
	u64 pos = s->pos;
	char url[1024];
	snprintf(url, sizeof(url), "%s", s->url);
	__sync_add_and_fetch(&s_ns_busy, 1);
	ns_close(s);
	int r = ns_open_internal(s, url, pos, 0);
	if (r == 0 && !s->counted) { s->counted = true; net_note_stream(+1); }
	__sync_sub_and_fetch(&s_ns_busy, 1);
	return r;
}

int ns_seek(NetStream *s, u64 pos) {
	if (s->open && pos == s->pos) return 0;
	char url[1024];
	snprintf(url, sizeof(url), "%s", s->url);
	__sync_add_and_fetch(&s_ns_busy, 1);
	ns_close(s);
	int r = ns_open_internal(s, url, pos, 0);
	if (r == 0 && !s->counted) { s->counted = true; net_note_stream(+1); }
	__sync_sub_and_fetch(&s_ns_busy, 1);
	return r;
}

void ns_close(NetStream *s) {
	if (s->open) {
		httpcCancelConnection(&s->ctx);
		close_sctx(s);
		s->open = false;
		if (s->counted) { s->counted = false; net_note_stream(-1); }
	}
}
