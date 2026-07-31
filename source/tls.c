#include <3ds.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include "tls.h"
#include "ui.h"

/* soc 的缓冲要 0x1000 对齐。0x60000 对一个短请求绰绰有余;
 * 别照抄网上那些 0x100000 —— 那是给同时开几十个连接的程序用的,
 * 在 124MB 模式下也是白占。 */
#define SOC_ALIGN  0x1000
#define SOC_BUFSZ  0x60000

#define CA_PATH    "romfs:/ca/globalsign-r3.der"

static u32 *s_soc_buf   = NULL;
static bool s_soc_ok    = false;
static bool s_sslc_ok   = false;
static u32  s_chain     = 0;
static bool s_chain_ok  = false;
static bool s_tried     = false;   /* 只初始化一次,失败也不反复重试 */

bool tls_ready(void) { return s_soc_ok && s_sslc_ok && s_chain_ok; }

int tls_init(void) {
	if (s_tried) return tls_ready() ? 0 : -1;
	s_tried = true;

	s_soc_buf = (u32 *)memalign(SOC_ALIGN, SOC_BUFSZ);
	if (!s_soc_buf) { ui_trace("tls: soc 缓冲分配失败"); return -1; }
	Result rc = socInit(s_soc_buf, SOC_BUFSZ);
	if (R_FAILED(rc)) {
		ui_trace("tls: socInit 失败 %08lx", (unsigned long)rc);
		free(s_soc_buf); s_soc_buf = NULL;
		return -1;
	}
	s_soc_ok = true;

	rc = sslcInit(0);
	if (R_FAILED(rc)) { ui_trace("tls: sslcInit 失败 %08lx", (unsigned long)rc); return -1; }
	s_sslc_ok = true;

	/* 根证书:从 romfs 读 DER */
	FILE *f = fopen(CA_PATH, "rb");
	if (!f) { ui_trace("tls: 打不开 " CA_PATH); return -1; }
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n <= 0 || n > 8192) { fclose(f); ui_trace("tls: 根证书大小异常 %ld", n); return -1; }
	u8 *der = (u8 *)malloc((size_t)n);
	if (!der) { fclose(f); return -1; }
	size_t got = fread(der, 1, (size_t)n, f);
	fclose(f);
	if (got != (size_t)n) { free(der); ui_trace("tls: 根证书读取不全"); return -1; }

	rc = sslcCreateRootCertChain(&s_chain);
	if (R_FAILED(rc)) { free(der); ui_trace("tls: 建证书链失败 %08lx", (unsigned long)rc); return -1; }
	u32 certctx = 0;
	rc = sslcAddTrustedRootCA(s_chain, der, (u32)n, &certctx);
	free(der);
	if (R_FAILED(rc)) {
		ui_trace("tls: 加载根证书失败 %08lx", (unsigned long)rc);
		sslcDestroyRootCertChain(s_chain);
		return -1;
	}
	s_chain_ok = true;
	ui_trace("tls: 就绪(根证书 %ld 字节)", n);
	return 0;
}

void tls_exit(void) {
	if (s_chain_ok) { sslcDestroyRootCertChain(s_chain); s_chain_ok = false; }
	if (s_sslc_ok)  { sslcExit(); s_sslc_ok = false; }
	if (s_soc_ok)   { socExit();  s_soc_ok  = false; }
	if (s_soc_buf)  { free(s_soc_buf); s_soc_buf = NULL; }
}

/* ---------- 收响应 ---------- */

/* 等 socket 可读。0 = 可读,-1 = 超时或出错。
 * sslcRead 自己会在这个 fd 上做阻塞读,所以超时只能在**进去之前**卡。 */
static int wait_readable(int fd, int secs) {
	fd_set rf;
	FD_ZERO(&rf);
	FD_SET(fd, &rf);
	struct timeval tv = { .tv_sec = secs, .tv_usec = 0 };
	int r = select(fd + 1, &rf, NULL, NULL, &tv);
	return (r > 0 && FD_ISSET(fd, &rf)) ? 0 : -1;
}

/* 一条一条地写。sslcWrite 和 write(2) 一样,可能只写了一部分 */
static bool write_all(sslcContext *ctx, const char *p, size_t n) {
	while (n) {
		int w = sslcWrite(ctx, p, (u32)n);
		if (w <= 0) return false;
		p += w; n -= (size_t)w;
	}
	return true;
}

/* chunked 解码。为什么必须做:我们发的是 HTTP/1.1,服务端完全可以
 * 分块传输,而分块的长度行混在正文里 —— 不解码就是**静默的**内容损坏,
 * JSON 解析会莫名其妙失败,而且看上去像是别的问题。 */
static size_t dechunk(char *s, size_t n) {
	size_t r = 0, w = 0;
	while (r < n) {
		/* 读长度行 */
		size_t ls = r;
		while (r < n && s[r] != '\n') r++;
		if (r >= n) break;
		size_t len = 0;
		bool any = false;
		for (size_t i = ls; i < r; i++) {
			char c = s[i];
			int d;
			if (c >= '0' && c <= '9') d = c - '0';
			else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
			else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
			else break;              /* ';' 之后是 chunk 扩展,忽略 */
			len = len * 16 + (size_t)d;
			any = true;
		}
		r++;                          /* 跳过 '\n' */
		if (!any || len == 0) break;  /* 0 长度 = 结束 */
		if (r + len > n) len = n - r;
		memmove(s + w, s + r, len);
		w += len;
		r += len;
		while (r < n && (s[r] == '\r' || s[r] == '\n')) r++;
	}
	s[w] = 0;
	return w;
}

int tls_get(const char *host, const char *path, const char *cookie_hdr,
            int *status, char *setcookies, size_t sc_n,
            char *body, size_t body_n) {
	if (status) *status = 0;
	if (setcookies && sc_n) setcookies[0] = 0;
	if (body && body_n) body[0] = 0;
	if (!tls_ready()) return -1;

	/* ---- DNS ---- */
	struct hostent *he = gethostbyname(host);
	if (!he || !he->h_addr_list[0]) { ui_trace("tls: DNS 失败 %s", host); return -2; }

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) { ui_trace("tls: socket 失败"); return -3; }

	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port   = htons(443);
	memcpy(&sa.sin_addr, he->h_addr_list[0], sizeof(sa.sin_addr));

	/* 【超时】3DS 的 soc **没有** SO_RCVTIMEO / SO_SNDTIMEO(编不过)。
	 * 但超时不能省:这是在主线程上跑的,对端不回就是整机卡死 ——
	 * 正是前几天刚清掉的那类 bug,不能又请回来。
	 * 所以改成每次读之前先 select 一把,见 wait_readable()。 */
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		ui_trace("tls: connect 失败");
		closesocket(fd);
		return -4;
	}

	sslcContext ctx;
	Result rc = sslcCreateContext(&ctx, fd, SSLCOPT_Default, host);
	if (R_FAILED(rc)) {
		ui_trace("tls: 建 SSL 上下文失败 %08lx", (unsigned long)rc);
		closesocket(fd);
		return -5;
	}
	rc = sslcContextSetRootCertChain(&ctx, s_chain);
	if (R_FAILED(rc)) {
		ui_trace("tls: 绑定证书链失败 %08lx", (unsigned long)rc);
		sslcDestroyContext(&ctx);
		closesocket(fd);
		return -6;
	}

	int internal = 0;
	u32 out = 0;
	rc = sslcStartConnection(&ctx, &internal, &out);
	if (R_FAILED(rc)) {
		/* 这里失败最常见的原因就是证书验不过 —— 不降级,直接失败,
		 * 由调用方退回 httpc。out 是校验结果位,排障时有用 */
		ui_trace("tls: 握手失败 %08lx internal=%d verify=%08lx",
		         (unsigned long)rc, internal, (unsigned long)out);
		sslcDestroyContext(&ctx);
		closesocket(fd);
		return -7;
	}

	/* ---- 发请求 ---- */
	char req[1400];
	int rn = snprintf(req, sizeof(req),
		"GET %s HTTP/1.1\r\n"
		"Host: %s\r\n"
		"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
		"(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36\r\n"
		"Accept: application/json, text/plain, */*\r\n"
		"Referer: https://www.bilibili.com\r\n"
		/* identity:不要 gzip。我们没有解压这一步,收到压缩正文只会得到
		 * 一堆二进制,然后在 JSON 解析那里报一个毫不相干的错 */
		"Accept-Encoding: identity\r\n"
		"%s%s%s"
		"Connection: close\r\n\r\n",
		path, host,
		cookie_hdr && cookie_hdr[0] ? "Cookie: " : "",
		cookie_hdr && cookie_hdr[0] ? cookie_hdr : "",
		cookie_hdr && cookie_hdr[0] ? "\r\n" : "");
	if (rn <= 0 || rn >= (int)sizeof(req)) {
		ui_trace("tls: 请求头过长(cookie 太多?)");
		sslcDestroyContext(&ctx); closesocket(fd);
		return -8;
	}
	if (!write_all(&ctx, req, (size_t)rn)) {
		ui_trace("tls: 发送失败");
		sslcDestroyContext(&ctx); closesocket(fd);
		return -9;
	}

	/* ---- 收 ---- */
	size_t cap = 64 * 1024;
	char *buf = (char *)malloc(cap);
	if (!buf) { sslcDestroyContext(&ctx); closesocket(fd); return -10; }
	size_t total = 0;
	for (;;) {
		if (total + 1 >= cap) break;          /* 够用了,不做无限增长 */
		/* 先等可读再进 sslcRead:握手完了但对端就是不发的话,
		 * 直接读会永远挂在这儿 */
		if (wait_readable(fd, 15) != 0) {
			ui_trace("tls: 读超时(已收 %d 字节)", (int)total);
			break;
		}
		int r = sslcRead(&ctx, buf + total, (u32)(cap - total - 1), false);
		if (r <= 0) break;                    /* 0 = 对端关闭(我们要的就是这个) */
		total += (size_t)r;
	}
	buf[total] = 0;
	sslcDestroyContext(&ctx);
	closesocket(fd);

	if (total == 0) { free(buf); ui_trace("tls: 空响应"); return -11; }

	/* ---- 拆头和正文 ---- */
	char *sep = strstr(buf, "\r\n\r\n");
	if (!sep) { free(buf); ui_trace("tls: 响应头不完整"); return -12; }
	*sep = 0;
	char *bodyp = sep + 4;
	size_t bodylen = total - (size_t)(bodyp - buf);

	if (status) {
		const char *sp = strchr(buf, ' ');
		if (sp) *status = atoi(sp + 1);
	}

	/* 所有 Set-Cookie 行 —— 这一整个文件就是为了这几行存在的 */
	bool chunked = false;
	size_t sco = 0;
	for (char *line = buf; line && *line; ) {
		char *nl = strstr(line, "\r\n");
		if (nl) *nl = 0;
		if (!strncasecmp(line, "Set-Cookie:", 11)) {
			const char *v = line + 11;
			while (*v == ' ') v++;
			if (setcookies && sc_n) {
				size_t need = strlen(v) + 1;
				if (sco + need < sc_n) {
					if (sco) setcookies[sco++] = '\n';
					memcpy(setcookies + sco, v, strlen(v));
					sco += strlen(v);
					setcookies[sco] = 0;
				}
			}
		} else if (!strncasecmp(line, "Transfer-Encoding:", 18) &&
		           strstr(line, "chunked")) {
			chunked = true;
		}
		line = nl ? nl + 2 : NULL;
	}

	if (chunked) bodylen = dechunk(bodyp, bodylen);
	if (body && body_n) {
		size_t n = bodylen < body_n - 1 ? bodylen : body_n - 1;
		memcpy(body, bodyp, n);
		body[n] = 0;
	}
	free(buf);
	return 0;
}
