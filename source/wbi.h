/* B 站 WBI 接口签名(纯逻辑,可在 PC 上单测) */
#ifndef WBI_H
#define WBI_H
#include <stddef.h>
#include <stdint.h>

/* RFC3986 urlencode,unreserved 字符不转义 */
void wbi_urlencode(char *dst, size_t dstlen, const char *src);

/* 由 img_key + sub_key(各 32 字符)生成 mixin key(32 字符 + NUL) */
void wbi_mixin_key(const char *img_key, const char *sub_key, char out[33]);

/* 对参数做 WBI 签名。keys/vals 为 n 组参数(不含 wts / w_rid)。
 * 输出完整 query string:k1=v1&k2=v2&...&wts=...&w_rid=...(按 key 排序)
 * ts 传当前 unix 秒。返回 0 成功。 */
int wbi_sign(const char **keys, const char **vals, int n,
             const char *mixin_key, int64_t ts,
             char *out, size_t outlen);

#endif
