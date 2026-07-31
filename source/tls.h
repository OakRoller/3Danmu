/* tls: 一个最小的 HTTPS GET,只为拿到**全部** Set-Cookie 响应头而存在。
 *
 * 【为什么不能用 httpc】
 * 3DS 的 httpc 服务把响应头解析在自己那边,只给出
 *   Result httpcGetResponseHeader(ctx, name, value, maxsize)
 * ——按名字取**一个**。libctru 里没有任何枚举接口(已核对头文件)。
 * B 站登录时一次下发五个 Set-Cookie,我们只读得到排在最前的 SESSDATA,
 * 需要 CSRF 的 bili_jct 永远拿不到:能登录,但发不了弹幕、上报不了历史。
 *
 * 所以扫码登录这一个请求改为自己走 soc + sslc,原始响应握在手里,
 * 想读几个头就读几个。**其余所有请求照旧走 httpc**,不动 ——
 * 那套代码是拿一堆真机踩坑换来的,没有理由为了这一个需求全盘重写。
 *
 * 【证书】3DS 内置的默认根证书是任天堂自家那几张,验不了 B 站的证书;
 * httpc 能通是因为 http 模块另有一套证书库,而那套 sslc 用不上。
 * 所以把 GlobalSign Root R3 塞进 romfs 自己加载(B 站证书链的根)。
 * 叶证书 90 天一换,但根 CA 十几年不动,这不是个脆弱的做法。
 *
 * **校验失败就失败,不降级。** 这是登录请求,宁可登不上,
 * 也不能把账号凭证送到一个验不了身份的对端。
 */
#pragma once

#include <stddef.h>
#include <stdbool.h>

/* 初始化 soc + sslc + 根证书链。失败返回负数(此时 tls_get 一律失败,
 * 调用方应当退回 httpc 路径)。可重复调用,只做一次。 */
int  tls_init(void);
void tls_exit(void);
bool tls_ready(void);

/* HTTPS GET。返回 0 成功。
 *   host/path   ——  形如 "passport.bilibili.com" / "/x/...?a=b"
 *   cookie_hdr  ——  要发出去的 Cookie 头内容,可为 NULL
 *   status      ——  HTTP 状态码
 *   setcookies  ——  **所有** Set-Cookie 行,每行一条,以 '\n' 分隔
 *   body        ——  响应正文(已解 chunked),以 NUL 结尾
 * 任一缓冲不够时截断,不算失败 —— 调用方自己判断够不够用。 */
int tls_get(const char *host, const char *path, const char *cookie_hdr,
            int *status, char *setcookies, size_t sc_n,
            char *body, size_t body_n);
