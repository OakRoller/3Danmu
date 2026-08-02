#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>
#include "bili.h"
#include "net.h"
#include "ui.h"
#include "jsonx.h"
#include "tls.h"
#include "wbi.h"
#include "md5.h"

static char s_mixin[33] = {0};
static int64_t s_mid = 0;          /* 登录用户 mid */
static char s_uname[64] = {0};
static bool s_logged_in = false;
static char s_last_err[96] = {0};

const char *bili_last_error(void) { return s_last_err; }

/* ---------- 工具 ---------- */

/* newlib 的 printf 可能不支持 %lld,64 位数字手动转字符串 */
static void i64_to_str(int64_t v, char *out) {
	char tmp[24];
	int i = 0, o = 0;
	uint64_t u = (v < 0) ? (uint64_t)(-v) : (uint64_t)v;
	do { tmp[i++] = (char)('0' + (u % 10)); u /= 10; } while (u);
	if (v < 0) out[o++] = '-';
	while (i) out[o++] = tmp[--i];
	out[o] = 0;
}

/* 去掉 <em class="keyword"> 之类的 HTML 标签 */
static void strip_html(char *s) {
	char *r = s, *w = s;
	bool in_tag = false;
	while (*r) {
		if (*r == '<') in_tag = true;
		else if (*r == '>') in_tag = false;
		else if (!in_tag) *w++ = *r;
		r++;
	}
	*w = 0;
}

/* "12:34" / "1:02:03" → 秒 */
static int parse_duration(const char *s) {
	int parts[3] = {0}, n = 0;
	const char *p = s;
	while (*p && n < 3) {
		parts[n++] = atoi(p);
		const char *c = strchr(p, ':');
		if (!c) break;
		p = c + 1;
	}
	if (n == 1) return parts[0];
	if (n == 2) return parts[0] * 60 + parts[1];
	return parts[0] * 3600 + parts[1] * 60 + parts[2];
}

/* 从 URL query 中提取参数值(不解码) */
/* 从 Set-Cookie 响应头里取一个 cookie 的值。
 * 形如 "SESSDATA=xxx; Path=/; Domain=.bilibili.com; Expires=Wed, 01-Jan-..."
 * 值到第一个 ';' 为止。逗号不能当分隔:Expires 里就有一个,
 * 而 SESSDATA 自己的逗号是 %2C,不会以裸逗号出现。 */
static bool header_cookie(const char *hdr, const char *name, char *out, size_t outlen) {
	if (!hdr || !*hdr) return false;
	size_t nl = strlen(name);
	for (const char *p = hdr; *p; p++) {
		bool boundary = (p == hdr) || p[-1] == ';' || p[-1] == ' ' ||
		                p[-1] == ',' || p[-1] == '\n';
		if (!boundary) continue;
		if (strncmp(p, name, nl) || p[nl] != '=') continue;
		const char *v = p + nl + 1;
		size_t n = strcspn(v, ";\r\n");
		if (n >= outlen) n = outlen - 1;
		memcpy(out, v, n);
		out[n] = 0;
		return n > 0;
	}
	return false;
}

/* 只列 query 的**参数名**,不带值 —— 值里可能是有效凭证,不能进日志。
 * 「url 有 166 字节但一个参数都没解析出来」时,光看长度分不清是格式变了
 * 还是解析写错了;把名字列出来一眼就知道 */
static void query_names(const char *url, char *out, size_t outlen) {
	size_t o = 0;
	out[0] = 0;
	const char *p = strchr(url, '?');
	p = p ? p + 1 : url;
	while (p && *p && o + 20 < outlen) {
		size_t n = strcspn(p, "=&");
		o += (size_t)snprintf(out + o, outlen - o, "%s%.*s",
		                      o ? "," : "", (int)n, p);
		p = strchr(p, '&');
		if (p) p++;
	}
}

static bool query_param(const char *url, const char *name, char *out, size_t outlen) {
	size_t nl = strlen(name);
	const char *p = strchr(url, '?');
	p = p ? p + 1 : url;
	while (p && *p) {
		if (!strncmp(p, name, nl) && p[nl] == '=') {
			const char *v = p + nl + 1;
			const char *e = strchr(v, '&');
			size_t n = e ? (size_t)(e - v) : strlen(v);
			if (n >= outlen) n = outlen - 1;
			memcpy(out, v, n);
			out[n] = 0;
			return true;
		}
		p = strchr(p, '&');
		if (p) p++;
	}
	return false;
}

/* GET url 并解析 JSON,校验 code==0。调用方负责 json_free + free(*body)
 * 失败时把 HTTP 状态和 B 站错误码打到下屏 console,方便真机排障 */
static Json *api_get_ref(const char *url, const char *referer, char **body_out);
static Json *api_get(const char *url, char **body_out) {
	return api_get_ref(url, NULL, body_out);
}
static Json *api_get_ref(const char *url, const char *referer, char **body_out) {
	HttpResponse res;
	s_last_err[0] = 0;
	if (net_get_ref(url, referer, &res) != 0) {
		snprintf(s_last_err, sizeof(s_last_err), "网络请求失败");
		printf("http error\n");
		return NULL;
	}
	if (res.status != 200 || !res.data) {
		snprintf(s_last_err, sizeof(s_last_err), "HTTP %d", res.status);
		printf("http status %d\n", res.status);
		net_response_free(&res);
		return NULL;
	}
	/* 大响应(如搜索)B 站会强制 gzip,先解压再解析 */
	if (res.len > 2 && (u8)res.data[0] == 0x1f && (u8)res.data[1] == 0x8b) {
		size_t cap = res.len * 10 + 65536;
		if (cap > 4u * 1024 * 1024) cap = 4u * 1024 * 1024;
		char *plain = (char *)malloc(cap);
		if (plain) {
			z_stream zs;
			memset(&zs, 0, sizeof(zs));
			if (inflateInit2(&zs, 15 + 32) == Z_OK) {
				zs.next_in = (Bytef *)res.data;
				zs.avail_in = (uInt)res.len;
				zs.next_out = (Bytef *)plain;
				zs.avail_out = (uInt)(cap - 1);
				int zr = inflate(&zs, Z_FINISH);
				size_t got = zs.total_out;
				inflateEnd(&zs);
				if ((zr == Z_STREAM_END || zr == Z_BUF_ERROR) && got > 0) {
					plain[got] = 0;
					free(res.data);
					res.data = plain;
					res.len = got;
					plain = NULL;
				}
			}
			free(plain);
		}
	}
	Json *j = json_parse(res.data, res.len);
	if (!j) {
		snprintf(s_last_err, sizeof(s_last_err), "响应解析失败");
		ui_log_ascii("bad json head: ", res.data, 24);
		net_response_free(&res);
		return NULL;
	}
	int64_t code = -1;
	if (!json_get_int(j, -1, "code", &code) || code != 0) {
		char msg[64] = {0};
		json_get_str(j, -1, "message", msg, sizeof(msg));
		snprintf(s_last_err, sizeof(s_last_err), "错误 %ld %s", (long)code, msg);
		{
			char pfx[32];
			snprintf(pfx, sizeof(pfx), "api code=%lld ", (long long)code);
			ui_log_ascii(pfx, msg, 80);
		}
		json_free(j);
		net_response_free(&res);
		return NULL;
	}
	*body_out = res.data; /* json 引用该内存,一起释放 */
	return j;
}

/* ---------- 初始化 ---------- */

static int fetch_buvid(void) {
	if (!net_get_cookie("b_nut")) { /* 与 buvid3 成对的时间戳,搜索 WAF 要查 */
		char nut[16];
		snprintf(nut, sizeof(nut), "%ld", (long)net_now());
		net_set_cookie("b_nut", nut);
		net_cookies_save_from("buvid/b_nut");
	}
	if (net_get_cookie("buvid3")) return 0;
	char *body = NULL;
	Json *j = api_get("https://api.bilibili.com/x/frontend/finger/spi", &body);
	if (!j) return -1;
	char b3[80] = {0}, b4[120] = {0};
	json_get_str(j, -1, "data.b_3", b3, sizeof(b3));
	json_get_str(j, -1, "data.b_4", b4, sizeof(b4));
	if (b3[0]) net_set_cookie("buvid3", b3);
	if (b4[0]) net_set_cookie("buvid4", b4);
	if (!net_get_cookie("b_nut")) { /* 与 buvid3 成对的时间戳 cookie,搜索 WAF 要查 */
		char nut[16];
		snprintf(nut, sizeof(nut), "%ld", (long)net_now());
		net_set_cookie("b_nut", nut);
	}
	/* 这里以前不存盘,于是新取到的 buvid3/buvid4 只活在内存里,盘上还是旧的
	 * —— 内存和文件从此不一致,排查时对不上账 */
	net_cookies_save_from("buvid/fetch");
	json_free(j);
	free(body);
	return b3[0] ? 0 : -1;
}

/* payload 里的 UA 字段,与 net.c 发出去的 User-Agent 保持一致 */
#define UA_FP "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "\
              "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"

/* ---------- Gaia 指纹:buvid_fp + ExClimbWuzhi ----------
 *
 * 背景:x/player/wbi/v2 恒 412,而同样带 WBI 签名的搜索/推荐一切正常。
 * 逐项排除过 aid 参数、具体视频页 Referer、Origin、isGaiaAvoided ——
 * **全都不是原因**;412 只跟接口路径相关。剩下的差别就是浏览器指纹:
 * 网页端除了 buvid3/4,还有一个 buvid_fp,并且要向 Gaia 网关注册一次
 * (ExClimbWuzhi)。风控严的接口会查这个,松的不查。
 *
 * buvid_fp = murmur3_x64_128(指纹 payload, seed=31) 的两个 64 位半拼接。 */

static u64 rotl64(u64 x, int r) { return (x << r) | (x >> (64 - r)); }
static u64 fmix64(u64 k) {
	k ^= k >> 33; k *= 0xff51afd7ed558ccdULL;
	k ^= k >> 33; k *= 0xc4ceb9fe1a85ec53ULL;
	k ^= k >> 33; return k;
}

static void murmur3_x64_128(const void *key, size_t len, u32 seed, u64 out[2]) {
	const u8 *data = (const u8 *)key;
	const size_t nblocks = len / 16;
	const u64 c1 = 0x87c37b91114253d5ULL, c2 = 0x4cf5ad432745937fULL;
	u64 h1 = seed, h2 = seed;
	for (size_t i = 0; i < nblocks; i++) {
		u64 k1, k2;
		memcpy(&k1, data + i * 16, 8);       /* 小端读,3DS 的 ARM11 是小端 */
		memcpy(&k2, data + i * 16 + 8, 8);
		k1 *= c1; k1 = rotl64(k1, 31); k1 *= c2; h1 ^= k1;
		h1 = rotl64(h1, 27); h1 += h2; h1 = h1 * 5 + 0x52dce729;
		k2 *= c2; k2 = rotl64(k2, 33); k2 *= c1; h2 ^= k2;
		h2 = rotl64(h2, 31); h2 += h1; h2 = h2 * 5 + 0x38495ab5;
	}
	const u8 *tail = data + nblocks * 16;
	u64 k1 = 0, k2 = 0;
	switch (len & 15) {
	case 15: k2 ^= (u64)tail[14] << 48;  /* fallthrough */
	case 14: k2 ^= (u64)tail[13] << 40;  /* fallthrough */
	case 13: k2 ^= (u64)tail[12] << 32;  /* fallthrough */
	case 12: k2 ^= (u64)tail[11] << 24;  /* fallthrough */
	case 11: k2 ^= (u64)tail[10] << 16;  /* fallthrough */
	case 10: k2 ^= (u64)tail[9] << 8;    /* fallthrough */
	case  9: k2 ^= (u64)tail[8];
	         k2 *= c2; k2 = rotl64(k2, 33); k2 *= c1; h2 ^= k2;
	         /* fallthrough */
	case  8: k1 ^= (u64)tail[7] << 56;   /* fallthrough */
	case  7: k1 ^= (u64)tail[6] << 48;   /* fallthrough */
	case  6: k1 ^= (u64)tail[5] << 40;   /* fallthrough */
	case  5: k1 ^= (u64)tail[4] << 32;   /* fallthrough */
	case  4: k1 ^= (u64)tail[3] << 24;   /* fallthrough */
	case  3: k1 ^= (u64)tail[2] << 16;   /* fallthrough */
	case  2: k1 ^= (u64)tail[1] << 8;    /* fallthrough */
	case  1: k1 ^= (u64)tail[0];
	         k1 *= c1; k1 = rotl64(k1, 31); k1 *= c2; h1 ^= k1;
	         break;
	default: break;
	}
	h1 ^= (u64)len; h2 ^= (u64)len;
	h1 += h2; h2 += h1;
	h1 = fmix64(h1); h2 = fmix64(h2);
	h1 += h2; h2 += h1;
	out[0] = h1; out[1] = h2;
}

/* u64 → 16 位小写十六进制。不能用 %llx:newlib 对 long long 支持不全,
 * 而 3DS 的 long 只有 32 位,直接 %lx 会把高位悄悄截掉 */
static void hex64(u64 v, char *out) {
	static const char *h = "0123456789abcdef";
	for (int i = 15; i >= 0; i--) { out[i] = h[v & 0xF]; v >>= 4; }
	out[16] = 0;
}

static int fetch_buvid_fp(void) {
	/* 每条返回路径都要留声。上一版这里静默 return,结果日志里
	 * 「没有 gaia 行」既可能是没跑、也可能是跳过了,分不出来 */
	if (net_get_cookie("buvid_fp")) {
		printf("gaia: fp already set, skip\n");
		return 0;
	}
	const char *b3 = net_get_cookie("buvid3");
	if (!b3) { printf("gaia: no buvid3, skip\n"); return -1; }

	/* 指纹 payload:网页端是一大坨浏览器特征。这里给一份结构合法、
	 * 内容固定的仿造值 —— 风控查的是「有没有注册过」,不是内容真假。
	 * 掺入 buvid3 让每台机器的指纹各不相同 */
	/* 时间戳不能用 %lld —— 本文件开头就记着 newlib 对 long long 支持不全,
	 * 而 3DS 的 long 只有 32 位。老老实实用 i64_to_str */
	char tsms[24];
	i64_to_str(net_now() * 1000, tsms);
	char payload[900];
	snprintf(payload, sizeof(payload),
		"{\"3064\":1,\"5062\":\"%s\",\"03bf\":\"https://www.bilibili.com/\","
		"\"39c8\":\"333.1007.fp.risk\",\"34f1\":\"\",\"d402\":\"\",\"654a\":\"\","
		"\"6e7c\":\"1920x1080\",\"3c43\":{\"2673\":0,\"5766\":24,\"6527\":0,"
		"\"7003\":1,\"807e\":1,\"b8ce\":\"%s\",\"641c\":0,\"07a4\":\"zh-CN\","
		"\"1c57\":\"not available\",\"0bd0\":8,\"748e\":[1080,1920],"
		"\"d61f\":[1040,1920],\"fc9d\":-480,\"6aa9\":\"Asia/Shanghai\","
		"\"75b8\":1,\"3b21\":1,\"8a1c\":0,\"d52f\":\"not available\","
		"\"adca\":\"Win32\",\"80c9\":[],\"13ab\":\"\",\"bfe9\":\"\","
		"\"a3c1\":[],\"6bc5\":\"Google Inc. (NVIDIA)\",\"ed31\":0,"
		"\"72bd\":0,\"fd54\":\"\",\"6c9f\":\"\"},\"54ef\":\"{}\","
		"\"8b94\":\"\",\"df35\":\"%s\",\"07a4\":\"zh-CN\",\"5f45\":null,"
		"\"db46\":0}",
		tsms, UA_FP, b3);

	u64 h[2];
	murmur3_x64_128(payload, strlen(payload), 31, h);
	char fp[40], lo[17], hi[17];
	hex64(h[0], lo);
	hex64(h[1], hi);
	snprintf(fp, sizeof(fp), "%s%s", lo, hi);
	net_set_cookie("buvid_fp", fp);

	/* 向 Gaia 网关注册。body 是 {"payload":"<转义后的 payload>"} */
	char body[2300];
	size_t o = 0;
	o += (size_t)snprintf(body + o, sizeof(body) - o, "{\"payload\":\"");
	for (const char *p = payload; *p && o < sizeof(body) - 8; p++) {
		if (*p == '"' || *p == '\\') body[o++] = '\\';
		body[o++] = *p;
	}
	snprintf(body + o, sizeof(body) - o, "\"}");

	HttpResponse res;
	int r = net_post_json(
		"https://api.bilibili.com/x/internal/gaia-gateway/ExClimbWuzhi",
		body, &res);
	if (r == 0) {
		/* HTTP 200 **不等于**注册成功:payload 不合规时网关照样回 200,
		 * 只是 body 里 code != 0。只看状态码会误判成「已经搞定了」,
		 * 然后在错误的前提上继续往下查 */
		int64_t code = -999;
		if (res.data) {
			Json *jj = json_parse(res.data, res.len);
			if (jj) { json_get_int(jj, -1, "code", &code); json_free(jj); }
		}
		printf("gaia: http %d code=%d fp=%.8s..\n",
		       res.status, (int)code, fp);
		net_response_free(&res);
	} else {
		printf("gaia: ExClimbWuzhi failed (net)\n");
	}
	net_cookies_save_from("gaia");
	return 0;
}

/* nav 接口:同时拿 WBI key 和登录态 */
static int fetch_nav(void) {
	HttpResponse res;
	if (net_get("https://api.bilibili.com/x/web-interface/nav", &res) != 0)
		return -1;
	if (res.status != 200 || !res.data) { net_response_free(&res); return -1; }
	Json *j = json_parse(res.data, res.len);
	if (!j) { net_response_free(&res); return -1; }
	/* 未登录时 code=-101,但 wbi_img 依然返回 */
	char img[256] = {0}, sub[256] = {0};
	json_get_str(j, -1, "data.wbi_img.img_url", img, sizeof(img));
	json_get_str(j, -1, "data.wbi_img.sub_url", sub, sizeof(sub));

	/* isLogin 是 JSON 布尔值(true/false),不能按整数解析 */
	char is_login[8] = {0};
	json_get_str(j, -1, "data.isLogin", is_login, sizeof(is_login));
	s_logged_in = (!strcmp(is_login, "true") || !strcmp(is_login, "1"));
	if (s_logged_in) {
		json_get_str(j, -1, "data.uname", s_uname, sizeof(s_uname));
		json_get_int(j, -1, "data.mid", &s_mid);   /* 收藏夹接口要用 */
	}
	printf("nav isLogin=%s sess=%s\n", is_login[0] ? is_login : "?",
	       net_get_cookie("SESSDATA") ? "yes" : "no");

	json_free(j);
	net_response_free(&res);

	/* 从 .../wbi/<32hex>.png 提取 key */
	char ik[64] = {0}, sk[64] = {0};
	char *p;
	if ((p = strrchr(img, '/')) && strlen(p) > 33) { memcpy(ik, p + 1, 32); ik[32] = 0; }
	if ((p = strrchr(sub, '/')) && strlen(p) > 33) { memcpy(sk, p + 1, 32); sk[32] = 0; }
	if (!ik[0] || !sk[0]) return -1;
	wbi_mixin_key(ik, sk, s_mixin);
	return 0;
}

int bili_init(void) {
	net_cookies_load();
	fetch_buvid();
	fetch_buvid_fp();  /* 指纹注册:x/player/wbi/v2 这类严风控接口要查 */
	fetch_nav();
	/* 途中任何一个响应都可能补上缺的登录 cookie(见 net.c 的 absorb),
	 * 落一次盘,免得每次启动都要重新捡一遍 */
	net_cookies_save_from("init");
	net_log_cookies("init");   /* 此刻才是「该有的都有了」 */
	ui_trace("init: logged_in=%d jct=%s", (int)s_logged_in,
	         net_get_cookie("bili_jct") ? "有" : "无");
	return 0;
}

bool bili_logged_in(void) { return s_logged_in; }
const char *bili_username(void) { return s_logged_in ? s_uname : NULL; }

void bili_logout(void) {
	ui_trace("logout: 用户主动注销,清空 cookie");
	net_clear_cookies();
	net_cookies_save_force("logout");   /* 故意清掉,绕开保险 */
	s_logged_in = false;
	s_uname[0] = 0;
	fetch_buvid();
}

/* ---------- 列表解析 ---------- */

/* 封面 URL 规整:协议相对(//i0.hdslb.com/...)补 https:,http 换 https */
/* 协议相对(//host/...)补 https:,http 换 https。
 * 必须原地 memmove,不能借道固定大小 tmp 缓冲——字幕 URL 带一长串签名
 * 参数(300+ 字节),曾被 tmp[192] 悄悄截断,请求 404,
 * UI 上就表现为"明明有字幕却说无字幕轨" */
static void fix_pic_url(char *pic, size_t n) {
	size_t len = strlen(pic);
	if (!len) return;
	if (pic[0] == '/' && pic[1] == '/') {
		if (len + 7 > n) return;           /* 放不下就别动 */
		memmove(pic + 6, pic, len + 1);
		memcpy(pic, "https:", 6);
	} else if (!strncmp(pic, "http://", 7)) {
		if (len + 2 > n) return;
		memmove(pic + 8, pic + 7, len - 7 + 1);
		memcpy(pic, "https://", 8);
	}
}

static void parse_video_item(const Json *j, int el, BiliVideo *v, bool is_search) {
	memset(v, 0, sizeof(*v));
	v->views = -1;
	json_get_str(j, el, "bvid", v->bvid, sizeof(v->bvid));
	json_get_int(j, el, "aid", &v->aid);
	if (!json_get_str(j, el, "pic", v->pic, sizeof(v->pic)) || !v->pic[0])
		json_get_str(j, el, "cover", v->pic, sizeof(v->pic));
	fix_pic_url(v->pic, sizeof(v->pic));
	json_get_str(j, el, "title", v->title, sizeof(v->title));
	strip_html(v->title);
	if (is_search) {
		json_get_str(j, el, "author", v->author, sizeof(v->author));
		char dur[24] = {0};
		if (json_get_str(j, el, "duration", dur, sizeof(dur)))
			v->duration = parse_duration(dur);
		int64_t play = -1;
		if (json_get_int(j, el, "play", &play)) v->views = play;
		v->cid = 0; /* 搜索接口不给 cid */
	} else {
		json_get_str(j, el, "owner.name", v->author, sizeof(v->author));
		int64_t d = 0, view = -1, cid = 0, np = 0;
		if (json_get_int(j, el, "duration", &d)) v->duration = (int)d;
		if (json_get_int(j, el, "stat.view", &view)) v->views = view;
		if (json_get_int(j, el, "cid", &cid)) v->cid = cid;
		/* 分 P 数:稿件类接口叫 videos。拿到了播放前就不用再问 pagelist */
		if (json_get_int(j, el, "videos", &np) && np > 0) v->pages = (int)np;
	}
}

/* 首页推荐(WBI 签名)。带 cookie 即个性化,未登录出通用推荐。
 * 没有页码概念,用 fresh_idx 换刷次数当"翻页";条目自带 cid */
/* ---------- 评论 ---------- */

/* x/v2/reply:老接口,不需要 WBI 签名,风控宽松。
 * type=1 表示"视频"这一类目,oid 就是 aid。sort=1 按热度(和网页默认一致)。
 * 未登录也能读,所以不强制要求登录态。 */
int bili_comments(int64_t aid, int page, BiliComment *out, int max, int *count) {
	*count = 0;
	if (!aid || max <= 0) return -1;
	char oid[24], pn[12];
	i64_to_str(aid, oid);
	snprintf(pn, sizeof(pn), "%d", page < 1 ? 1 : page);
	char url[192];
	snprintf(url, sizeof(url),
	         "https://api.bilibili.com/x/v2/reply?type=1&oid=%s&pn=%s&ps=20&sort=1",
	         oid, pn);
	char *body = NULL;
	Json *j = api_get(url, &body);
	if (!j) { printf("comments: %s\n", bili_last_error()); return -1; }

	int arr = json_find(j, -1, "data.replies");
	int n = json_arr_len(j, arr);
	if (n > max) n = max;
	int m = 0;
	for (int i = 0; i < n; i++) {
		int el = json_arr_at(j, arr, i);
		BiliComment *c = &out[m];
		memset(c, 0, sizeof(*c));
		if (!json_get_str(j, el, "content.message", c->text, sizeof(c->text)))
			continue;
		if (!c->text[0]) continue;
		json_get_str(j, el, "member.uname", c->user, sizeof(c->user));
		int64_t v = 0;
		if (json_get_int(j, el, "like", &v)) c->like = (int)v;
		if (json_get_int(j, el, "rcount", &v)) c->replies = (int)v;
		if (json_get_int(j, el, "ctime", &v)) c->ctime = v;
		m++;
	}
	*count = m;
	json_free(j);
	free(body);
	printf("comments: %d loaded (page %d)\n", m, page);
	return m > 0 ? 0 : -1;
}

int bili_recommend(int page, BiliVideo *out, int max, int *count) {
	*count = 0;
	if (!s_mixin[0] && fetch_nav() != 0) return -1;
	char fi[12];
	snprintf(fi, sizeof(fi), "%d", page);
	const char *keys[] = { "fresh_idx", "fresh_idx_1h", "fresh_type", "ps" };
	const char *vals[] = { fi, fi, "4", "20" };
	char query[512];
	if (wbi_sign(keys, vals, 4, s_mixin, net_now(),
	             query, sizeof(query)) != 0)
		return -1;
	char url[640];
	snprintf(url, sizeof(url),
	         "https://api.bilibili.com/x/web-interface/wbi/index/top/feed/rcmd?%s",
	         query);
	char *body = NULL;
	Json *j = api_get(url, &body);
	if (!j) return -1;
	int arr = json_find(j, -1, "data.item");
	int n = json_arr_len(j, arr);
	for (int i = 0; i < n && *count < max; i++) {
		int el = json_arr_at(j, arr, i);
		char go[16] = {0};
		json_get_str(j, el, "goto", go, sizeof(go));
		if (go[0] && strcmp(go, "av") != 0) continue;   /* 挡广告/直播卡 */
		BiliVideo *v = &out[*count];
		memset(v, 0, sizeof(*v));
		v->views = -1;
		json_get_str(j, el, "bvid", v->bvid, sizeof(v->bvid));
		json_get_int(j, el, "id", &v->aid);
		json_get_str(j, el, "pic", v->pic, sizeof(v->pic));
		json_get_str(j, el, "title", v->title, sizeof(v->title));
		fix_pic_url(v->pic, sizeof(v->pic));
		json_get_str(j, el, "owner.name", v->author, sizeof(v->author));
		int64_t d = 0, play = -1, cid = 0, np = 0;
		if (json_get_int(j, el, "duration", &d)) v->duration = (int)d;
		if (json_get_int(j, el, "stat.view", &play)) v->views = play;
		if (json_get_int(j, el, "cid", &cid)) v->cid = cid;
		if (json_get_int(j, el, "videos", &np) && np > 0) v->pages = (int)np;
		if (v->bvid[0] && v->title[0]) (*count)++;
	}
	json_free(j);
	free(body);
	return (*count > 0) ? 0 : -1;
}

/* 历史记录(需登录)。x/v2/history 简单分页,条目结构与稿件一致 */
int bili_history(int page, BiliVideo *out, int max, int *count) {
	*count = 0;
	char url[192];
	snprintf(url, sizeof(url),
	         "https://api.bilibili.com/x/v2/history?pn=%d&ps=20", page);
	char *body = NULL;
	Json *j = api_get(url, &body);
	if (!j) return -1;
	int arr = json_find(j, -1, "data");
	int n = json_arr_len(j, arr);
	for (int i = 0; i < n && *count < max; i++) {
		int el = json_arr_at(j, arr, i);
		parse_video_item(j, el, &out[*count], false);
		if (out[*count].bvid[0] && out[*count].title[0]) (*count)++;
	}
	json_free(j);
	free(body);
	return (n >= 0) ? 0 : -1;
}

/* 收藏夹(默认收藏夹,需登录)。两步:查默认夹 id → 拉内容 */
static int64_t s_fav_fid = 0;
static bool s_sub_is_ai = false;
int bili_fav(int page, BiliVideo *out, int max, int *count) {
	*count = 0;
	if (!s_mid && fetch_nav() != 0) return -1;
	if (!s_mid) return -1;
	char midstr[24], url[224];
	i64_to_str(s_mid, midstr);
	if (!s_fav_fid) {
		snprintf(url, sizeof(url),
		         "https://api.bilibili.com/x/v3/fav/folder/created/list-all?up_mid=%s",
		         midstr);
		char *body = NULL;
		Json *j = api_get(url, &body);
		if (!j) return -1;
		int arr = json_find(j, -1, "data.list");
		if (json_arr_len(j, arr) > 0)
			json_get_int(j, json_arr_at(j, arr, 0), "id", &s_fav_fid);
		json_free(j);
		free(body);
		if (!s_fav_fid) return -1;
	}
	char fidstr[24];
	i64_to_str(s_fav_fid, fidstr);
	snprintf(url, sizeof(url),
	         "https://api.bilibili.com/x/v3/fav/resource/list?"
	         "media_id=%s&pn=%d&ps=20&platform=web", fidstr, page);
	char *body = NULL;
	Json *j = api_get(url, &body);
	if (!j) return -1;
	int arr = json_find(j, -1, "data.medias");
	int n = json_arr_len(j, arr);
	for (int i = 0; i < n && *count < max; i++) {
		int el = json_arr_at(j, arr, i);
		BiliVideo *v = &out[*count];
		memset(v, 0, sizeof(*v));
		v->views = -1;
		int64_t type = 0;
		json_get_int(j, el, "type", &type);
		if (type != 2) continue;                 /* 2 = 视频稿件 */
		json_get_str(j, el, "bvid", v->bvid, sizeof(v->bvid));
		json_get_int(j, el, "id", &v->aid);
		json_get_str(j, el, "title", v->title, sizeof(v->title));
		json_get_str(j, el, "cover", v->pic, sizeof(v->pic));
		fix_pic_url(v->pic, sizeof(v->pic));
		json_get_str(j, el, "upper.name", v->author, sizeof(v->author));
		int64_t d = 0, play = -1, np = 0;
		if (json_get_int(j, el, "duration", &d)) v->duration = (int)d;
		if (json_get_int(j, el, "cnt_info.play", &play)) v->views = play;
		/* 收藏夹的条目里,分 P 数这个字段叫 page(不是 videos) */
		if (json_get_int(j, el, "page", &np) && np > 0) v->pages = (int)np;
		if (v->bvid[0] && v->title[0]) (*count)++;
	}
	json_free(j);
	free(body);
	return 0;
}

int bili_popular(int page, BiliVideo *out, int max, int *count) {
	*count = 0;
	char url[256];
	snprintf(url, sizeof(url),
	         "https://api.bilibili.com/x/web-interface/popular?ps=20&pn=%d", page);
	char *body = NULL;
	Json *j = api_get(url, &body);
	if (!j) return -1;

	int arr = json_find(j, -1, "data.list");
	int n = json_arr_len(j, arr);
	for (int i = 0; i < n && i < max; i++) {
		int el = json_arr_at(j, arr, i);
		parse_video_item(j, el, &out[*count], false);
		if (out[*count].bvid[0]) (*count)++;
	}
	json_free(j);
	free(body);
	return 0;
}

/* App 端签名:参数按 key 排序 urlencode 拼接 + appsec 求 md5 作为 sign */
#define APP_KEY "1d8b6e7d45233436"
#define APP_SEC "560c52ccd288fed045859ed18bffd973"

typedef struct { const char *k, *v; } AppKV;
static int appkv_cmp(const void *a, const void *b) {
	return strcmp(((const AppKV *)a)->k, ((const AppKV *)b)->k);
}

static int app_sign(const char **keys, const char **vals, int n,
                    char *out, size_t outlen) {
	AppKV kv[16];
	if (n > 16) return -1;
	for (int i = 0; i < n; i++) { kv[i].k = keys[i]; kv[i].v = vals[i]; }
	qsort(kv, (size_t)n, sizeof(AppKV), appkv_cmp);
	char query[1536];
	size_t o = 0;
	for (int i = 0; i < n; i++) {
		char enc[512];
		wbi_urlencode(enc, sizeof(enc), kv[i].v);
		int w = snprintf(query + o, sizeof(query) - o, "%s%s=%s",
		                 i ? "&" : "", kv[i].k, enc);
		if (w < 0 || (size_t)w >= sizeof(query) - o) return -1;
		o += (size_t)w;
	}
	char tohash[1536 + 40];
	snprintf(tohash, sizeof(tohash), "%s%s", query, APP_SEC);
	char sign[33];
	md5_hex(tohash, strlen(tohash), sign);
	int w = snprintf(out, outlen, "%s&sign=%s", query, sign);
	return (w < 0 || (size_t)w >= outlen) ? -1 : 0;
}

/* 搜索走手机 App 接口(appkey 签名),避开 web 端 WAF */
/* web 端 WBI 搜索(只搜视频)。返回的每条结果都带真正的 bvid,
 * 比 App 端从 param 拼 av 号可靠得多——App 混合列表的 param 拼出的号
 * 实测有一批查任何接口都 -404(视频在浏览器里明明能放)。
 * 早期没带 WBI 签名 + buvid 时这个接口被 WAF 挡,现在两样都有,可以走了。
 * 失败(被 WAF/风控拒)则回落 App 端接口。 */
static int search_web(const char *keyword, int page, BiliVideo *out, int max,
                      int *count) {
	if (!s_mixin[0] && fetch_nav() != 0) return -1;
	char pagestr[12];
	snprintf(pagestr, sizeof(pagestr), "%d", page);
	const char *keys[] = { "keyword", "page", "page_size", "search_type" };
	const char *vals[] = { keyword, pagestr, "20", "video" };
	char query[1600];
	if (wbi_sign(keys, vals, 4, s_mixin, net_now(),
	             query, sizeof(query)) != 0)
		return -1;
	char url[1800];
	snprintf(url, sizeof(url),
	         "https://api.bilibili.com/x/web-interface/wbi/search/type?%s", query);
	char *body = NULL;
	Json *j = api_get(url, &body);
	if (!j) return -1;
	int arr = json_find(j, -1, "data.result");
	int n = json_arr_len(j, arr);
	if (n <= 0) { json_free(j); free(body); return -1; }
	printf("web search: n=%d\n", n);
	for (int i = 0; i < n && *count < max; i++) {
		int el = json_arr_at(j, arr, i);
		BiliVideo *v = &out[*count];
		memset(v, 0, sizeof(*v));
		v->views = -1;
		v->cid = 0;
		/* type 字段应为 "video",别的类型(小卡)跳过 */
		char ty[16] = {0};
		if (json_get_str(j, el, "type", ty, sizeof(ty)) &&
		    ty[0] && strcmp(ty, "video") != 0)
			continue;
		if (!json_get_str(j, el, "bvid", v->bvid, sizeof(v->bvid)) ||
		    !v->bvid[0])
			continue;
		json_get_str(j, el, "title", v->title, sizeof(v->title));
		strip_html(v->title);   /* 命中词有 <em> 高亮标签 */
		json_get_str(j, el, "author", v->author, sizeof(v->author));
		json_get_int(j, el, "aid", &v->aid);
		json_get_str(j, el, "pic", v->pic, sizeof(v->pic));
		fix_pic_url(v->pic, sizeof(v->pic));
		int64_t play = -1;
		if (json_get_int(j, el, "play", &play)) v->views = play;
		char dur[24] = {0};
		if (json_get_str(j, el, "duration", dur, sizeof(dur)))
			v->duration = parse_duration(dur);
		if (v->title[0]) {
			printf("search[%d]: %s (web)\n", *count, v->bvid);
			(*count)++;
		}
	}
	json_free(j);
	free(body);
	return (*count > 0) ? 0 : -1;
}

int bili_search(const char *keyword, int page, BiliVideo *out, int max, int *count) {
	*count = 0;
	if (search_web(keyword, page, out, max, count) == 0)
		return 0;
	printf("web search failed, trying app search\n");

	char pagestr[12], ts[16];
	snprintf(pagestr, sizeof(pagestr), "%d", page);
	snprintf(ts, sizeof(ts), "%ld", (long)net_now());
	const char *keys[] = { "appkey", "build", "keyword", "mobi_app",
	                       "order", "platform", "pn", "ps", "ts" };
	const char *vals[] = { APP_KEY, "6840300", keyword, "android",
	                       "totalrank", "android", pagestr, "20", ts };
	char query[1600];
	if (app_sign(keys, vals, 9, query, sizeof(query)) != 0)
		return -1;

	char url[1800];
	snprintf(url, sizeof(url),
	         "https://app.bilibili.com/x/v2/search?%s", query);
	char *body = NULL;
	Json *j = api_get(url, &body);
	if (!j) return -1;

	/* App 接口的结果结构随版本变化,依次尝试已知的几种路径 */
	static const char *paths[] = {
		"data.item",           /* 新版:混合结果列表 */
		"data.items.archive",  /* 旧版:分类结果 */
		"data.result",         /* web 风格 */
		"data.items",
	};
	int arr = -1, n = -1;
	for (unsigned pi = 0; pi < sizeof(paths) / sizeof(paths[0]); pi++) {
		int a = json_find(j, -1, paths[pi]);
		int c = json_arr_len(j, a);
		if (c > 0) { arr = a; n = c; printf("search: %s n=%d\n", paths[pi], c); break; }
	}
	if (n <= 0) { /* 都不匹配:打印响应头部,便于定位真实结构 */
		ui_log_ascii("search resp: ", body, 160);
		json_free(j);
		free(body);
		return -1;
	}
	for (int i = 0; i < n && *count < max; i++) {
		int el = json_arr_at(j, arr, i);
		/* 混合结果里不只有视频:还有用户、直播间、番剧、专栏、广告……
		 * 它们的 param 是 mid/房间号/season id 之类,拼成 av 号必然查不到
		 * cid。goto 字段标记类型,只留 "av"(视频);没有 goto 字段的
		 * 结构(旧版分类接口)本身就全是视频,放行 */
		{
			char go[24] = {0};
			if (json_get_str(j, el, "goto", go, sizeof(go)) && go[0] &&
			    strcmp(go, "av") != 0) {
				printf("search: skip type=%s\n", go);
				continue;
			}
			/* linktype 是另一种标记法,形如 "av"/"live"/"bangumi" 前缀 */
			if (!go[0] && json_get_str(j, el, "linktype", go, sizeof(go)) &&
			    go[0] && strncmp(go, "av", 2) != 0) {
				printf("search: skip linktype=%s\n", go);
				continue;
			}
		}
		BiliVideo *v = &out[*count];
		memset(v, 0, sizeof(*v));
		v->views = -1;
		v->cid = 0;
		json_get_str(j, el, "title", v->title, sizeof(v->title));
		strip_html(v->title);
		if (!json_get_str(j, el, "author", v->author, sizeof(v->author)) ||
		    !v->author[0])
			json_get_str(j, el, "owner.name", v->author, sizeof(v->author));
		int64_t play = -1;
		if (json_get_int(j, el, "play", &play) ||
		    json_get_int(j, el, "stat.view", &play) ||
		    json_get_int(j, el, "cover_right_text1", &play))
			v->views = play;
		char dur[24] = {0};
		if (json_get_str(j, el, "duration", dur, sizeof(dur)))
			v->duration = parse_duration(dur);
		/* App 接口给 aid(param 字段);bvid 有则用,无则记 "av<aid>" */
		bool from_bvid = true;
		if (!json_get_str(j, el, "bvid", v->bvid, sizeof(v->bvid)) ||
		    !v->bvid[0]) {
			from_bvid = false;
			char aid[20] = {0};
			if (!json_get_str(j, el, "param", aid, sizeof(aid)) || !aid[0])
				json_get_str(j, el, "aid", aid, sizeof(aid));
			if (aid[0] && aid[0] >= '0' && aid[0] <= '9')
				snprintf(v->bvid, sizeof(v->bvid), "av%s", aid);
			else if (aid[0] == 'B')  /* param 可能直接是 bvid */
				snprintf(v->bvid, sizeof(v->bvid), "%s", aid);
		}
		if (v->bvid[0])
			printf("search[%d]: %s (%s)\n", *count, v->bvid,
			       from_bvid ? "bvid" : "param");
		/* 视频必有时长;用户/直播间/番剧卡片没有(或解析为 0)。
		 * goto 过滤漏掉的非视频条目,大多数在这一层被拦住 */
		if (v->duration <= 0 && v->views < 0) {
			printf("search: skip no-duration item\n");
			continue;
		}
		if (v->bvid[0] && v->title[0]) (*count)++;
	}
	json_free(j);
	free(body);
	return 0;
}

/* 查 cid 有三个接口,风控严格程度不同。web 端 view 对"敏感"内容 +
 * 非浏览器指纹的请求经常直接回 -404(视频明明存在,浏览器能看)。
 * 所以依次尝试:web view → pagelist(轻量、风控最松)→ App 端签名接口
 * (与搜索同一套 appkey,已验证能过) */

static int get_cid_webview(const char *bvid, int64_t *cid, int64_t *aid) {
	char url[256];
	printf("cid query id=%s\n", bvid);
	if (bvid[0] == 'a' && bvid[1] == 'v')
		snprintf(url, sizeof(url),
		         "https://api.bilibili.com/x/web-interface/view?aid=%s", bvid + 2);
	else
		snprintf(url, sizeof(url),
		         "https://api.bilibili.com/x/web-interface/view?bvid=%s", bvid);
	char *body = NULL;
	Json *j = api_get(url, &body);
	if (!j) return -1;
	bool ok = json_get_int(j, -1, "data.cid", cid);
	if (ok && aid && !*aid)
		json_get_int(j, -1, "data.aid", aid);
	json_free(j);
	free(body);
	return ok ? 0 : -1;
}

static int get_cid_pagelist(const char *bvid, int64_t *cid) {
	char url[256];
	if (bvid[0] == 'a' && bvid[1] == 'v')
		snprintf(url, sizeof(url),
		         "https://api.bilibili.com/x/player/pagelist?aid=%s", bvid + 2);
	else
		snprintf(url, sizeof(url),
		         "https://api.bilibili.com/x/player/pagelist?bvid=%s", bvid);
	printf("GET %.80s\n", url);
	char *body = NULL;
	Json *j = api_get(url, &body);
	if (!j) return -1;
	bool ok = false;
	int arr = json_find(j, -1, "data");
	if (arr >= 0 && json_arr_len(j, arr) > 0) {
		int el = json_arr_at(j, arr, 0);
		ok = json_get_int(j, el, "cid", cid);
	}
	json_free(j);
	free(body);
	return ok ? 0 : -1;
}

static int get_cid_appview(const char *bvid, int64_t *cid, int64_t *aid) {
	if (!(bvid[0] == 'a' && bvid[1] == 'v')) return -1; /* 该接口只吃 aid */
	char ts[16];
	snprintf(ts, sizeof(ts), "%ld", (long)net_now());
	const char *keys[] = { "aid", "appkey", "build", "mobi_app",
	                       "platform", "ts" };
	const char *vals[] = { bvid + 2, APP_KEY, "6840300", "android",
	                       "android", ts };
	char query[512];
	if (app_sign(keys, vals, 6, query, sizeof(query)) != 0) return -1;
	char url[640];
	snprintf(url, sizeof(url), "https://app.bilibili.com/x/v2/view?%s", query);
	char *body = NULL;
	Json *j = api_get(url, &body);
	if (!j) return -1;
	bool ok = json_get_int(j, -1, "data.cid", cid);
	if (ok && aid && !*aid)
		json_get_int(j, -1, "data.aid", aid);
	json_free(j);
	free(body);
	return ok ? 0 : -1;
}

int bili_get_cid(const char *bvid, int64_t *cid, int64_t *aid) {
	if (get_cid_webview(bvid, cid, aid) == 0) return 0;
	printf("webview cid failed, trying pagelist\n");
	if (get_cid_pagelist(bvid, cid) == 0) { printf("pagelist ok\n"); return 0; }
	printf("pagelist failed, trying app view\n");
	if (get_cid_appview(bvid, cid, aid) == 0) { printf("app view ok\n"); return 0; }
	return -1;
}

/* ---------- 分 P 列表 ----------
 *
 * 两条路都试:
 *   x/player/pagelist          —— 只返回分 P 数组,轻量、风控最松
 *   x/web-interface/view       —— data.pages[],和 cid 查询同一个接口
 *
 * 顺序和 bili_get_cid 相反(那边先 view 后 pagelist):这里要的就是
 * **整份数组**,pagelist 天生只返回它,响应小一个数量级;view 会把
 * 简介、UP 主、统计、推荐位全带上,几 P 的视频也要几十 KB。
 * 一个 200 P 的合集,view 那条路的 JSON 能到几百 KB —— 3DS 上光解析
 * 就是明显的一顿,而里面 99% 的字段这里一个都不用。
 *
 * 数组里的元素形状两边一样({cid, page, part, duration}),
 * 所以解析共用一份代码。 */
static int parse_page_array(const Json *j, int arr, BiliPage *out, int max,
                            int *count) {
	int n = json_arr_len(j, arr);
	if (n <= 0) return -1;
	if (n > max) n = max;
	int got = 0;
	for (int i = 0; i < n; i++) {
		int el = json_arr_at(j, arr, i);
		if (el < 0) continue;
		BiliPage *pg = &out[got];
		memset(pg, 0, sizeof(*pg));
		int64_t cid = 0, page = 0, dur = 0;
		if (!json_get_int(j, el, "cid", &cid) || !cid) continue;
		pg->cid = cid;
		pg->page = json_get_int(j, el, "page", &page) ? (int)page : (got + 1);
		if (json_get_int(j, el, "duration", &dur)) pg->duration = (int)dur;
		json_get_str(j, el, "part", pg->title, sizeof(pg->title));
		strip_html(pg->title);
		got++;
	}
	*count = got;
	return got > 0 ? 0 : -1;
}

int bili_pagelist(const char *bvid, BiliPage *out, int max, int *count) {
	*count = 0;
	if (!bvid || !bvid[0] || max <= 0) return -1;
	bool is_av = (bvid[0] == 'a' && bvid[1] == 'v');
	char url[256];
	char *body = NULL;
	Json *j;

	snprintf(url, sizeof(url),
	         "https://api.bilibili.com/x/player/pagelist?%s=%s",
	         is_av ? "aid" : "bvid", is_av ? bvid + 2 : bvid);
	printf("GET %.80s\n", url);
	j = api_get(url, &body);
	if (j) {
		int r = parse_page_array(j, json_find(j, -1, "data"), out, max, count);
		json_free(j);
		free(body);
		if (r == 0) {
			printf("pagelist: %d part(s)\n", *count);
			return 0;
		}
		body = NULL;
	}

	printf("pagelist failed, trying view.pages\n");
	snprintf(url, sizeof(url),
	         "https://api.bilibili.com/x/web-interface/view?%s=%s",
	         is_av ? "aid" : "bvid", is_av ? bvid + 2 : bvid);
	j = api_get(url, &body);
	if (!j) return -1;
	int r = parse_page_array(j, json_find(j, -1, "data.pages"), out, max, count);
	json_free(j);
	free(body);
	if (r == 0) printf("view.pages: %d part(s)\n", *count);
	return r;
}

/* CC 字幕:x/player/wbi/v2 拿字幕轨列表,再拉正文 JSON */
/* 上次成功的请求方案(0=还没试过)。412 只在部分方案上发生,
 * 试出来之后就固定走那一条,不再每次从头试。 */
static int s_sub_variant = 0;

int bili_subtitle_fetch(const char *bvid, int64_t aid, int64_t cid,
                        char **body_out, size_t *len_out) {
	*body_out = NULL;
	if (!bvid || !bvid[0] || !cid) return -1;
	if (!s_mixin[0] && fetch_nav() != 0) return -1;
	char cidstr[24], aidstr[24];
	i64_to_str(cid, cidstr);
	i64_to_str(aid, aidstr);
	/* 具体视频页的 Referer:风控严的接口不认站点根 */
	char ref[64];
	snprintf(ref, sizeof(ref), "https://www.bilibili.com/video/%s/", bvid);

	char url[640], query[512];
	char *body = NULL;
	bool legacy = false;
	Json *j = NULL;

	/* ---- 方案阶梯 ----
	 * 这个接口恒 412,而同样带 WBI 签名的搜索/推荐一切正常,
	 * 说明签名算法没问题,是**这一个接口的风控**另有要求。
	 * 到底缺什么无法从外部推断,所以按「最像网页端」到「最宽松」
	 * 依次尝试,每次把结果打出来,成功后记住方案号不再重试。
	 *   1 = aid + bvid,isGaiaAvoided=true,具体视频页 Referer + Origin
	 *   2 = 仅 bvid,isGaiaAvoided=false,站点根 Referer(原方案)
	 *   3 = 老接口 x/player/v2(不带 WBI,风控宽松) */
	if (s_sub_variant)
		printf("sub: locked at try%d (restart app to re-probe)\n", s_sub_variant);
	for (int v = (s_sub_variant ? s_sub_variant : 1); v <= 3 && !j; v++) {
		if (v == 1) {
			/* 网页播放器实际发的就是 aid+cid;bvid 一并带上无害。
			 * isGaiaAvoided 网页端发 true */
			const char *k[] = { "aid", "bvid", "cid",
			                    "isGaiaAvoided", "web_location" };
			const char *val[] = { aidstr, bvid, cidstr, "true", "1315873" };
			int nk = aid ? 5 : 4;          /* 没有 aid 就退化成 4 个参数 */
			const char *k2[] = { "bvid", "cid", "isGaiaAvoided", "web_location" };
			const char *v2[] = { bvid, cidstr, "true", "1315873" };
			if (wbi_sign(aid ? k : k2, aid ? val : v2, nk, s_mixin,
			             net_now(), query, sizeof(query)) != 0)
				continue;
			snprintf(url, sizeof(url),
			         "https://api.bilibili.com/x/player/wbi/v2?%s", query);
			printf("sub: try1 wbi+aid+pageref\n");
			j = api_get_ref(url, ref, &body);
		} else if (v == 2) {
			const char *k[] = { "bvid", "cid", "isGaiaAvoided", "web_location" };
			const char *val[] = { bvid, cidstr, "false", "1315873" };
			if (wbi_sign(k, val, 4, s_mixin, net_now(),
			             query, sizeof(query)) != 0)
				continue;
			snprintf(url, sizeof(url),
			         "https://api.bilibili.com/x/player/wbi/v2?%s", query);
			printf("sub: try2 wbi plain\n");
			j = api_get(url, &body);
		} else {
			snprintf(url, sizeof(url),
			         "https://api.bilibili.com/x/player/v2?bvid=%s&cid=%s",
			         bvid, cidstr);
			printf("sub: try3 legacy\n");
			j = api_get(url, &body);
			if (j) legacy = true;
		}
		if (!j) {
			printf("sub: try%d failed (%s)\n", v, bili_last_error());
			/* 记住的方案失效了(风控策略变了):忘掉它,
			 * 本次继续往后试,下次调用自然从 1 重新探。
			 * 注意别在这里改 v 回退重来——那会在方案 1 上死循环 */
			s_sub_variant = 0;
		} else {
			s_sub_variant = v;
			printf("sub: try%d OK (locked)\n", v);
		}
	}
	if (!j) {
		printf("sub: all variants failed\n");
		return -1;
	}
	{	/* 身份校验:响应里的 bvid/cid 必须与请求一致。
		 * "有时字幕是别的视频的、但 1/4 概率又是对的" —— 说明参数没错,
		 * 是响应有时对不上号,这里当场拆穿并放弃,绝不显示错的字幕。
		 *
		 * 注意校验有个静默漏洞:响应里**根本没有** bvid/cid 时,
		 * rcid=0 且 rbvid 为空,下面的 bad 判定全是假 —— 等于没校验。
		 * 老接口正是这种情况的高发地(它可以只回字幕字段)。
		 * 所以走了回落路径就要求必须能核对上,核不上一律不用:
		 * 宁可没字幕,也不能给一份别的视频的字幕。 */
		int64_t rcid = 0;
		char rbvid[24] = {0};
		json_get_int(j, -1, "data.cid", &rcid);
		json_get_str(j, -1, "data.bvid", rbvid, sizeof(rbvid));
		char rcs[24];
		i64_to_str(rcid, rcs);       /* 不能用 %ld:3DS 的 long 只有 32 位 */
		printf("sub: resp bvid=%s cid=%s%s\n",
		       rbvid[0] ? rbvid : "-", rcs, legacy ? " (legacy)" : "");
		bool bad = (rcid && rcid != cid) ||
		           (rbvid[0] && strcmp(rbvid, bvid) != 0);
		if (legacy && !rcid && !rbvid[0]) {
			printf("sub: legacy resp has no id, UNVERIFIABLE -> abort\n");
			bad = true;
		}
		if (bad) {
			printf("sub: MISMATCH! want bvid=%s cid=%s -> abort\n",
			       bvid, cidstr);
			json_free(j);
			free(body);
			return -1;
		}
	}
	char sub_url[600] = {0};
	/* 结构随版本有两种:data.subtitle.subtitles(新)/ data.subtitles(旧) */
	int arr = json_find(j, -1, "data.subtitle.subtitles");
	if (json_arr_len(j, arr) <= 0)
		arr = json_find(j, -1, "data.subtitles");
	if (json_arr_len(j, arr) <= 0)
		arr = json_find(j, -1, "data.subtitle.list");
	int n = json_arr_len(j, arr);
	char need_login[8] = {0};
	json_get_str(j, -1, "data.subtitle.need_login_subtitle", need_login,
	             sizeof(need_login));
	printf("subtitle tracks=%d need_login=%s\n", n,
	       need_login[0] ? need_login : "?");
	/* 只认中文轨:人工(zh-*)优先,没有则用 AI 中文(ai-zh)。
	 * 曾写成 "zh 开头 || 还没选到",后半句把英/日轨也抓了进来 */
	char ai_url[600] = {0};
	s_sub_is_ai = false;
	for (int i = 0; i < n; i++) {
		int el = json_arr_at(j, arr, i);
		char lan[24] = {0};
		json_get_str(j, el, "lan", lan, sizeof(lan));
		if (!strncmp(lan, "zh", 2)) {
			json_get_str(j, el, "subtitle_url", sub_url, sizeof(sub_url));
			printf("sub: pick lan=%s\n", lan);
			break;
		}
		if (!strncmp(lan, "ai-zh", 5) && !ai_url[0])
			json_get_str(j, el, "subtitle_url", ai_url, sizeof(ai_url));
	}
	if (!sub_url[0] && ai_url[0]) {
		snprintf(sub_url, sizeof(sub_url), "%s", ai_url);
		s_sub_is_ai = true;
		printf("sub: pick lan=ai-zh\n");
	}
	json_free(j);
	free(body);
	if (!sub_url[0]) {
		/* B 站已改为字幕必须登录:未登录时 subtitle_url 一律为空 */
		printf(bili_logged_in() ? "no subtitle track\n"
		                        : "subtitle needs login\n");
		return -1;
	}
	/* 【最后一道闸:URL 自身要能证明它属于本视频】
	 * 前面校验的是「元数据响应的 bvid/cid 对得上」,但真正决定内容的是
	 * subtitle_url。实测出现过元数据对得上、字幕内容却是别的视频的情况,
	 * 说明中间还有一环会串。
	 * 好在 AI 字幕(ai-zh)的 URL 路径里直接嵌着 aid/cid ——
	 * 那就直接在 URL 里找本视频的 cid,找不到就不要。
	 * 人工字幕的 URL 是内容哈希,没有 id 可查,只能放行(但会打出来)。 */
	if (strstr(sub_url, "ai_subtitle") && !strstr(sub_url, cidstr)) {
		printf("sub: URL has no cid=%s -> reject\n", cidstr);
		ui_log_ascii("bad sub url: ", sub_url, 70);
		return -1;
	}
	printf("sub url: %.60s\n", sub_url);
	fix_pic_url(sub_url, sizeof(sub_url));  /* 同样是协议相对 URL */
	ui_log_ascii("sub url: ", sub_url, 70);
	HttpResponse res;
	if (net_get(sub_url, &res) != 0) {
		printf("sub: fetch body failed (network)\n");
		return -1;
	}
	if (res.status != 200 || !res.data) {
		printf("sub: body http %d\n", res.status);
		net_response_free(&res);
		return -1;
	}
	*body_out = res.data;                    /* 所有权转移给调用方 */
	if (len_out) *len_out = res.len;
	printf("subtitle: %d bytes\n", (int)res.len);
	return 0;
}

/* 观看进度上报(x/v2/history/report):让 3DS 上看的视频出现在
 * 账号的历史记录里,手机/网页端能接着看 */
int bili_report_history(int64_t aid, int64_t cid, int progress_s) {
	const char *csrf = net_get_cookie("bili_jct");
	if (!csrf || !csrf[0] || !aid || !cid) {
		/* 每个视频只提一次,别刷屏。缺 bili_jct 是常态(见 bili_qr_poll),
		 * 静默返回的话用户只会觉得「历史记录莫名其妙不同步」 */
		static int64_t warned_cid = 0;
		if ((!csrf || !csrf[0]) && cid && cid != warned_cid) {
			warned_cid = cid;
			printf("history: 无 bili_jct,本次不上报(需手动导入)\n");
		}
		return -1;
	}
	char aidstr[24], cidstr[24];
	i64_to_str(aid, aidstr);
	i64_to_str(cid, cidstr);
	char progstr[16];
	snprintf(progstr, sizeof(progstr), "%d", progress_s);
	const char *keys[] = { "aid", "cid", "progress", "platform", "csrf" };
	const char *vals[] = { aidstr, cidstr, progstr, "web", csrf };
	HttpResponse res;
	if (net_post_fields("https://api.bilibili.com/x/v2/history/report",
	                    keys, vals, 5, &res) != 0)
		return -1;
	int rc = -1;
	if (res.status == 200 && res.data) {
		Json *j = json_parse(res.data, res.len);
		if (j) {
			int64_t code = -1;
			json_get_int(j, -1, "code", &code);
			rc = (code == 0) ? 0 : -1;
			if (rc != 0) printf("history report code=%d\n", (int)code);
			json_free(j);
		}
	}
	net_response_free(&res);
	return rc;
}

bool bili_subtitle_is_ai(void) { return s_sub_is_ai; }

/* 发弹幕(x/v2/dm/post,需登录 + csrf=bili_jct cookie) */
int bili_send_danmaku(int64_t aid, int64_t cid, int progress_ms,
                      const char *msg) {
	const char *csrf = net_get_cookie("bili_jct");
	if (!csrf || !csrf[0]) {
		/* 扫码登录拿不到 bili_jct(3DS 的 httpc 读不全 Set-Cookie,
		 * 详见 bili_qr_poll 里的说明)。这不是「没登录」,别这么写 ——
		 * 用户明明看得见自己的用户名,提示说未登录只会让他反复重登。 */
		snprintf(s_last_err, sizeof(s_last_err),
		         "发弹幕需手动导入 bili_jct,见 README");
		printf("dm post: no bili_jct (需手动导入,扫码登录拿不到)\n");
		return -1;
	}
	if (!aid || !cid || !msg || !msg[0]) {
		snprintf(s_last_err, sizeof(s_last_err), "参数缺失(aid/cid)");
		return -1;
	}
	char aidstr[24], cidstr[24], progstr[16], rndstr[24];
	i64_to_str(aid, aidstr);
	i64_to_str(cid, cidstr);
	snprintf(progstr, sizeof(progstr), "%d", progress_ms);
	/* rnd 官方是随机数,同值重复提交会被判重复弹幕 */
	snprintf(rndstr, sizeof(rndstr), "%d",
	         (int)(time(NULL) ^ (int)(cid & 0xFFFF) ^ (progress_ms << 3)));
	const char *keys[] = { "type", "oid", "msg", "aid", "progress",
	                       "color", "fontsize", "pool", "mode", "rnd",
	                       "plat", "csrf" };
	const char *vals[] = { "1", cidstr, msg, aidstr, progstr,
	                       "16777215", "25", "0", "1", rndstr,
	                       "1", csrf };
	printf("dm post: oid=%s aid=%s prog=%d len=%d\n",
	       cidstr, aidstr, progress_ms, (int)strlen(msg));
	HttpResponse res;
	if (net_post_fields("https://api.bilibili.com/x/v2/dm/post",
	                    keys, vals, 12, &res) != 0) {
		snprintf(s_last_err, sizeof(s_last_err),
		         "网络失败 step%d", g_net_last_step);
		printf("dm post: network fail\n");
		return -1;
	}
	int rc = -1;
	if (res.status != 200) {
		snprintf(s_last_err, sizeof(s_last_err), "HTTP %d", res.status);
		printf("dm post: http %d\n", res.status);
	} else if (!res.data) {
		snprintf(s_last_err, sizeof(s_last_err), "空响应");
	} else {
		ui_log_ascii("dm resp: ", res.data, 90);   /* 原始响应,最有用 */
		Json *j = json_parse(res.data, res.len);
		if (!j) {
			snprintf(s_last_err, sizeof(s_last_err), "响应解析失败");
		} else {
			int64_t code = -1;
			json_get_int(j, -1, "code", &code);
			if (code == 0) {
				rc = 0;
			} else {
				char m[80] = {0};
				json_get_str(j, -1, "message", m, sizeof(m));
				snprintf(s_last_err, sizeof(s_last_err),
				         "%ld %s", (long)code, m[0] ? m : "(无描述)");
			}
			json_free(j);
		}
	}
	net_response_free(&res);
	printf(rc == 0 ? "dm post ok\n" : "dm post FAILED\n");
	return rc;
}

/* mcdn/p2p 节点对非浏览器客户端基本连不上,要避开 */
static bool is_bad_cdn(const char *url) {
	return strstr(url, "mcdn.") || strstr(url, "szbdyd") || strstr(url, ".p2p");
}

/* 从 playurl 响应里取 durl mp4 直链,优先正规 upos CDN */
/* 【要了什么、给了什么】接口对不认识的 qn 不会报错,而是**给一个它认为
 * 最接近的档位**。我们只取 durl 的话,这种「要 240P 给 360P」完全静默 ——
 * 老机型上表现就是「明明选了 240P 却还是卡」,而设置页显示得好好的。
 * data.quality 是服务端实际给的那一档,accept_quality 是它愿意给的全部。 */
static void log_quality(Json *j, int want) {
	int64_t got = -1;
	json_get_int(j, -1, "data.quality", &got);
	if (got < 0) json_get_int(j, -1, "quality", &got);
	if (got < 0) { ui_trace("playurl: 要 qn=%d,响应里没有 quality 字段", want); return; }
	if ((int)got == want) {
		ui_trace("playurl: qn=%d 如愿", want);
		return;
	}
	/* 不一致时把可选档位也列出来 —— 直接回答「那这个视频到底有没有 240P」 */
	char acc[96] = {0};
	size_t o = 0;
	for (int i = 0; i < 8 && o + 8 < sizeof(acc); i++) {
		char path[40];
		int64_t q = -1;
		snprintf(path, sizeof(path), "data.accept_quality[%d]", i);
		if (!json_get_int(j, -1, path, &q) || q < 0) break;
		o += (size_t)snprintf(acc + o, sizeof(acc) - o, "%s%d", o ? "," : "", (int)q);
	}
	ui_trace("playurl: 要 qn=%d,给的是 %d(可选:%s)",
	         want, (int)got, acc[0] ? acc : "未知");
}

static bool extract_durl(Json *j, char *url_out, size_t urllen) {
	char cand[2048];
	/* 主地址 + 备选地址逐个试,取第一个非 p2p 的 */
	const char *paths[] = {
		"data.durl[0].url",
		"data.durl[0].backup_url[0]",
		"data.durl[0].backup_url[1]",
		/* App 端 playurl 部分版本不带 data 包装,durl 直接在根上 */
		"durl[0].url",
		"durl[0].backup_url[0]",
	};
	char first[2048] = {0};
	for (int i = 0; i < 5; i++) {
		if (!json_get_str(j, -1, paths[i], cand, sizeof(cand)) || !cand[0])
			continue;
		if (!first[0]) snprintf(first, sizeof(first), "%s", cand);
		if (!is_bad_cdn(cand)) {
			snprintf(url_out, urllen, "%s", cand);
			printf("cdn: %.48s\n", cand);
			return true;
		}
	}
	if (first[0]) { /* 全是 p2p,硬着头皮用主地址 */
		snprintf(url_out, urllen, "%s", first);
		printf("warn: p2p cdn only: %.48s\n", first);
		return true;
	}
	if (json_find(j, -1, "data.dash") >= 0)
		printf("got DASH only (no durl)\n");
	return false;
}

int bili_get_play_url(const char *bvid, int64_t cid, int qn, char *url_out, size_t urllen) {
	url_out[0] = 0;
	char cidstr[24], url[2560], qnstr[8];
	i64_to_str(cid, cidstr);
	snprintf(qnstr, sizeof(qnstr), "%d", qn);
	printf("bvid=%s cid=%s qn=%d\n", bvid, cidstr, qn);
	char *body = NULL;
	Json *j = NULL;

	/* 每一级都留声。三级方案里任何一级"没走"和"走了没成"表现一样(都是往下掉),
	 * 事后只看最终返回值分不出是哪一级出的问题,更分不出有没有真的发过请求 */
	u64 t_enter = osGetTime();

	/* 1) 标准 web 端 WBI 签名接口,fnval=1 要 durl mp4(登录后 360P/480P 可用) */
	bool is_av = (bvid[0] == 'a' && bvid[1] == 'v');
	if (!s_mixin[0]) ui_trace("playurl: 无 WBI key,先取 nav");
	if (s_mixin[0] || fetch_nav() == 0) {
		const char *keys[] = { is_av ? "avid" : "bvid", "cid", "qn", "fnval", "fnver", "fourk" };
		const char *vals[] = { is_av ? bvid + 2 : bvid, cidstr, qnstr, "1", "0", "0" };
		char query[2048];
		if (wbi_sign(keys, vals, 6, s_mixin, net_now(), query, sizeof(query)) == 0) {
			snprintf(url, sizeof(url),
			         "https://api.bilibili.com/x/player/wbi/playurl?%s", query);
			u64 t0 = osGetTime();
			j = api_get(url, &body);
			ui_trace("playurl 一级(wbi): %s %dms %s", j ? "有响应" : "失败",
			         (int)(osGetTime() - t0), j ? "" : s_last_err);
			if (j) {
				log_quality(j, qn);
				bool ok = extract_durl(j, url_out, urllen);
				json_free(j);
				free(body);
				if (ok) return 0;
				ui_trace("playurl 一级: 响应里没有可用 durl");
			} else {
				s_mixin[0] = 0; /* 签名 key 可能过期,下次重取 */
			}
		} else {
			ui_trace("playurl 一级: wbi 签名失败(跳过,未发请求)");
		}
	} else {
		ui_trace("playurl 一级: 无 WBI key 且 nav 也失败(跳过,未发请求)");
	}

	/* 2) 退回 html5 免登录接口 */
	printf("wbi playurl failed, trying html5...\n");
	snprintf(url, sizeof(url),
	         "https://api.bilibili.com/x/player/playurl?"
	         "%s=%s&cid=%s&qn=%d&type=mp4&platform=html5&high_quality=1",
	         is_av ? "avid" : "bvid", is_av ? bvid + 2 : bvid, cidstr, qn);
	printf("GET %.90s\n", url);
	body = NULL;
	{
		u64 t0 = osGetTime();
		j = api_get(url, &body);
		ui_trace("playurl 二级(html5): %s %dms %s", j ? "有响应" : "失败",
		         (int)(osGetTime() - t0), j ? "" : s_last_err);
	}
	if (j) {
		log_quality(j, qn);
		bool ok = extract_durl(j, url_out, urllen);
		json_free(j);
		free(body);
		if (ok) return 0;
		ui_trace("playurl 二级: 响应里没有可用 durl");
	}

	/* 3) App 端 appkey 签名接口:web 端风控放不过的视频走这里
	 *(与搜索/cid 兜底同一套 appkey,已验证能过)。只吃 aid,
	 * bvid 形式没有 aid 可用就到此为止 */
	if (!is_av) {
		ui_trace("playurl: BV 号,三级方案不适用,放弃(共 %dms)",
		         (int)(osGetTime() - t_enter));
		return -1;
	}
	printf("html5 playurl failed, trying app playurl...\n");
	{
		char ts[16];
		snprintf(ts, sizeof(ts), "%ld", (long)net_now());
		const char *keys[] = { "aid", "appkey", "cid", "fnval", "fnver",
		                       "mobi_app", "platform", "qn", "ts" };
		const char *vals[] = { bvid + 2, APP_KEY, cidstr, "1", "0",
		                       "android", "android", qnstr, ts };
		char query[1024];
		if (app_sign(keys, vals, 9, query, sizeof(query)) != 0) return -1;
		snprintf(url, sizeof(url),
		         "https://app.bilibili.com/x/playurl?%s", query);
		body = NULL;
		j = api_get(url, &body);
		if (!j) return -1;
		bool ok = extract_durl(j, url_out, urllen);
		json_free(j);
		free(body);
		if (ok) { printf("app playurl ok\n"); return 0; }
	}
	return -1;
}

/* ---------- 扫码登录 ---------- */

int bili_qr_generate(char *qr_url, size_t un, char *qrcode_key, size_t kn) {
	char *body = NULL;
	Json *j = api_get("https://passport.bilibili.com/x/passport-login/web/qrcode/generate", &body);
	if (!j) return -1;
	bool ok = json_get_str(j, -1, "data.url", qr_url, un) &&
	          json_get_str(j, -1, "data.qrcode_key", qrcode_key, kn);
	json_free(j);
	free(body);
	return ok ? 0 : -1;
}

/* 【试着补上 bili_jct】
 * 扫码登录只拿得到 SESSDATA:B 站一次下发五个 Set-Cookie,而 3DS 的 httpc
 * 对同名响应头只给得出一份(已确认 libctru 只有 httpcGetResponseHeader,
 * 没有枚举接口)。
 *
 * bili_jct 是 CSRF 令牌。会话里缺它时,有些页面会重新下发 —— 而我们读得到
 * 每个响应的**第一个** Set-Cookie,只要某个页面下发的第一个正好是它就成了。
 * 这是试探,成不成取决于服务端行为;失败只是白跑几个请求,不影响其它功能。
 *
 * 只在**刚登录完**跑一次:每次开机都跑的话,那几个 HTML 页面加起来
 * 几百 KB,3DS 上的启动会明显变慢,而结果多半和上次一样。 */
static void try_recover_jct(void) {
	if (net_get_cookie("bili_jct") || !net_get_cookie("SESSDATA")) return;
	/* 从轻到重:nav 是小 JSON,后两个是整页 HTML,能早停就早停 */
	static const char *const URLS[] = {
		"https://api.bilibili.com/x/web-interface/nav",
		"https://passport.bilibili.com/login",
		"https://www.bilibili.com/account/history",
	};
	for (size_t i = 0; i < sizeof(URLS) / sizeof(URLS[0]); i++) {
		HttpResponse r;
		if (net_get(URLS[i], &r) != 0) { ui_trace("jct 探测 %d: 请求失败", (int)i); continue; }
		/* 只记「第一个 cookie 叫什么」和长度 —— 值是有效凭证,不进日志 */
		const char *sc = net_last_set_cookie();
		char nm[48] = {0};
		size_t n = strcspn(sc, "=");
		if (n < sizeof(nm)) { memcpy(nm, sc, n); nm[n] = 0; }
		ui_trace("jct 探测 %d: http %d sc=%dB 首个=[%s] jct=%s",
		         (int)i, r.status, (int)strlen(sc), nm,
		         net_get_cookie("bili_jct") ? "有" : "无");
		net_response_free(&r);
		if (net_get_cookie("bili_jct")) {
			net_cookies_save_from("jct探测");
			return;
		}
	}
}

/* 【走自己的 TLS 拿全部 Set-Cookie】
 * 成功返回 0 并且 *code 有效;返回负数表示这条路没走通,
 * 调用方**必须**退回 httpc 路径 —— 那条路虽然只拿得到 SESSDATA,
 * 但至少能登上。新路子不能让事情比现在更糟。 */
static int qr_poll_tls(const char *qrcode_key, int *code) {
	if (tls_init() != 0) return -1;

	char path[384];
	snprintf(path, sizeof(path),
	         "/x/passport-login/web/qrcode/poll?qrcode_key=%s", qrcode_key);
	char ck[2048];
	net_cookie_header(ck, sizeof(ck));

	int status = 0;
	char sc[4096], body[4096];
	if (tls_get("passport.bilibili.com", path, ck, &status,
	            sc, sizeof(sc), body, sizeof(body)) != 0)
		return -2;
	if (status != 200) { ui_trace("qr(tls): http %d", status); return -3; }

	Json *j = json_parse(body, strlen(body));
	if (!j) { ui_trace("qr(tls): JSON 解析失败 body=%dB", (int)strlen(body)); return -4; }
	int64_t c = -1;
	json_get_int(j, -1, "data.code", &c);
	*code = (int)c;

	if (c == 0) {
		/* 只记有几行、每行的 cookie 名 —— 值是有效凭证 */
		int lines = sc[0] ? 1 : 0;
		for (const char *p = sc; *p; p++) if (*p == '\n') lines++;
		net_absorb_set_cookie(sc);
		ui_trace("qr(tls): 登录成功,Set-Cookie %d 行 %dB -> sess=%s jct=%s uid=%s",
		         lines, (int)strlen(sc),
		         net_get_cookie("SESSDATA")   ? "有" : "无",
		         net_get_cookie("bili_jct")   ? "有" : "无",
		         net_get_cookie("DedeUserID") ? "有" : "无");
		net_cookies_save_from("qrlogin/tls");
		fetch_nav();
		net_cookies_save_from("qrlogin/tls/nav");
	}
	json_free(j);
	return 0;
}

int bili_qr_poll(const char *qrcode_key, int *code) {
	{
		int c = -1;
		int r = qr_poll_tls(qrcode_key, &c);
		if (r == 0) { *code = c; return 0; }
		/* 每次轮询都失败会刷屏,只在第一次记一行 */
		static bool warned = false;
		if (!warned) { warned = true; ui_trace("qr: TLS 路径不可用(%d),退回 httpc", r); }
	}
	*code = -1;
	char url[384];
	snprintf(url, sizeof(url),
	         "https://passport.bilibili.com/x/passport-login/web/qrcode/poll?qrcode_key=%s",
	         qrcode_key);
	char *body = NULL;
	Json *j = api_get(url, &body);
	if (!j) return -1;

	int64_t c = -1;
	json_get_int(j, -1, "data.code", &c);
	*code = (int)c;

	if (c == 0) {
		/* 登录成功:cookie 值都在 data.url 的 query 里。
		 * 2048 不是 1024:这串是 crossDomain 跳转 URL,末尾还挂着一个
		 * 整体转义过的 gourl。截断了的话前面的 SESSDATA 可能刚好被切掉一半,
		 * 而 query_param 对「截断」和「本来就这么长」分不出来 —— 静默出错 */
		char curl[2048] = {0};
		json_get_str(j, -1, "data.url", curl, sizeof(curl));
		char val[512];
		int n_uid = 0, n_md5 = 0, n_sess = 0, n_jct = 0;
		if (query_param(curl, "DedeUserID", val, sizeof(val)))        { n_uid  = (int)strlen(val); net_set_cookie("DedeUserID", val); }
		if (query_param(curl, "DedeUserID__ckMd5", val, sizeof(val))) { n_md5  = (int)strlen(val); net_set_cookie("DedeUserID__ckMd5", val); }
		if (query_param(curl, "SESSDATA", val, sizeof(val)))          { n_sess = (int)strlen(val); net_set_cookie("SESSDATA", val); }
		if (query_param(curl, "bili_jct", val, sizeof(val)))          { n_jct  = (int)strlen(val); net_set_cookie("bili_jct", val); }
		/* 只记长度,不记值 —— 这几个是有效凭证,写进 trace.log 就等于泄露 */
		ui_trace("qr ok: url=%dB uid=%d md5=%d sess=%d jct=%d",
		         (int)strlen(curl), n_uid, n_md5, n_sess, n_jct);
		/* poll 的正文里会不会直接带着?顺手看一眼,不花钱 */
		ui_trace("qr: poll 正文 %dB 含jct=%d 含cookie_info=%d",
		         body ? (int)strlen(body) : 0,
		         (body && strstr(body, "bili_jct")) ? 1 : 0,
		         (body && strstr(body, "cookie_info")) ? 1 : 0);

		/* 【兜底】data.url 里没有就去 Set-Cookie 响应头里找。
		 * 服务端把凭证从 query 挪进响应头之后,老路径会「成功地拿到 0 个
		 * cookie」——code 还是 0、url 还在,只是参数没了,静默失效。 */
		if (!n_sess) {
			char names[200];
			query_names(curl, names, sizeof(names));
			ui_trace("qr: data.url 无凭证,参数为 [%s]", names);
			const char *sc = net_last_set_cookie();
			if (header_cookie(sc, "DedeUserID", val, sizeof(val)))        { n_uid  = (int)strlen(val); net_set_cookie("DedeUserID", val); }
			if (header_cookie(sc, "DedeUserID__ckMd5", val, sizeof(val))) { n_md5  = (int)strlen(val); net_set_cookie("DedeUserID__ckMd5", val); }
			if (header_cookie(sc, "SESSDATA", val, sizeof(val)))          { n_sess = (int)strlen(val); net_set_cookie("SESSDATA", val); }
			if (header_cookie(sc, "bili_jct", val, sizeof(val)))          { n_jct  = (int)strlen(val); net_set_cookie("bili_jct", val); }
			ui_trace("qr: Set-Cookie %dB -> uid=%d md5=%d sess=%d jct=%d",
			         (int)strlen(sc ? sc : ""), n_uid, n_md5, n_sess, n_jct);
		}
		/* 【为什么不去请求 data.url】
		 * 新版流程里 data.url 的参数是 [ticket, gourl, first_domain],
		 * 看着像「拿 ticket 去换 cookie」。实测过了:那个地址返回 200,
		 * 但**一个 Set-Cookie 都不设**,而且会跟着 gourl 把 B 站首页
		 * 整整 143KB 的 HTML 拉下来。纯浪费流量,别再加回去。
		 *
		 * bilibili.com 的 cookie 确实全在 poll 那次响应的多个 Set-Cookie 头里,
		 * 但 3DS 的 httpc 对同名响应头**只给得出一份**,我们只读得到
		 * 排在最前面的 SESSDATA。所以扫码登录能登上、但注定缺 bili_jct。
		 * 需要 csrf 的功能改由用户手动导入 bili_jct 解锁(见 README)。 */
		net_cookies_save_from("qrlogin");
		fetch_nav(); /* 刷新登录态和用户名 */
		try_recover_jct();
		/* 再存一次:nav 的响应里可能补发了 bili_jct(登录那一次的响应头
		 * 只捞得到 SESSDATA),被 absorb 收进内存了但还没落盘 */
		net_cookies_save_from("qrlogin/nav");
		ui_trace("qr ok: fetch_nav 之后 logged_in=%d jct=%s", (int)s_logged_in,
		         net_get_cookie("bili_jct") ? "有" : "无");
	}
	json_free(j);
	free(body);
	return 0;
}
