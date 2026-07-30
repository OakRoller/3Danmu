#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "wbi.h"
#include "md5.h"

/* 官方 web 端混淆表(bilibili-API-collect 文档) */
static const uint8_t MIXIN_TAB[64] = {
	46, 47, 18, 2, 53, 8, 23, 32, 15, 50, 10, 31, 58, 3, 45, 35,
	27, 43, 5, 49, 33, 9, 42, 19, 29, 28, 14, 39, 12, 38, 41, 13,
	37, 48, 7, 16, 24, 55, 40, 61, 26, 17, 0, 1, 60, 51, 30, 4,
	22, 25, 54, 21, 56, 59, 6, 63, 57, 62, 11, 36, 20, 34, 44, 52
};

void wbi_mixin_key(const char *img_key, const char *sub_key, char out[33]) {
	char raw[65];
	snprintf(raw, sizeof(raw), "%s%s", img_key, sub_key);
	size_t rawlen = strlen(raw);
	int o = 0;
	for (int i = 0; i < 64 && o < 32; i++) {
		if (MIXIN_TAB[i] < rawlen)
			out[o++] = raw[MIXIN_TAB[i]];
	}
	out[o] = 0;
}

static int is_unreserved(unsigned char c) {
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
	       (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
}

void wbi_urlencode(char *dst, size_t dstlen, const char *src) {
	size_t o = 0;
	for (const unsigned char *p = (const unsigned char *)src; *p && o + 4 < dstlen; p++) {
		/* WBI 规范:过滤掉 !'()* */
		if (*p == '!' || *p == '\'' || *p == '(' || *p == ')' || *p == '*')
			continue;
		if (is_unreserved(*p)) {
			dst[o++] = (char)*p;
		} else {
			snprintf(dst + o, dstlen - o, "%%%02X", *p);
			o += 3;
		}
	}
	dst[o] = 0;
}

typedef struct { const char *k; const char *v; } KV;

static int kv_cmp(const void *a, const void *b) {
	return strcmp(((const KV *)a)->k, ((const KV *)b)->k);
}

int wbi_sign(const char **keys, const char **vals, int n,
             const char *mixin_key, int64_t ts,
             char *out, size_t outlen) {
	if (n > 30) return -1;
	KV kv[32];
	/* 手动转字符串:newlib 的 snprintf 可能不支持 %lld */
	char wts[24];
	{
		char tmp[24]; int i = 0, o = 0;
		uint64_t u = (ts < 0) ? (uint64_t)(-ts) : (uint64_t)ts;
		do { tmp[i++] = (char)('0' + (u % 10)); u /= 10; } while (u);
		if (ts < 0) wts[o++] = '-';
		while (i) wts[o++] = tmp[--i];
		wts[o] = 0;
	}
	for (int i = 0; i < n; i++) { kv[i].k = keys[i]; kv[i].v = vals[i]; }
	kv[n].k = "wts"; kv[n].v = wts;
	int total = n + 1;
	qsort(kv, (size_t)total, sizeof(KV), kv_cmp);

	char query[2048];
	size_t o = 0;
	for (int i = 0; i < total; i++) {
		char enc[512];
		wbi_urlencode(enc, sizeof(enc), kv[i].v);
		int w = snprintf(query + o, sizeof(query) - o, "%s%s=%s",
		                 i ? "&" : "", kv[i].k, enc);
		if (w < 0 || (size_t)w >= sizeof(query) - o) return -1;
		o += (size_t)w;
	}

	char to_hash[2048 + 33];
	int w = snprintf(to_hash, sizeof(to_hash), "%s%s", query, mixin_key);
	if (w < 0 || (size_t)w >= sizeof(to_hash)) return -1;

	char w_rid[33];
	md5_hex(to_hash, strlen(to_hash), w_rid);

	w = snprintf(out, outlen, "%s&w_rid=%s", query, w_rid);
	if (w < 0 || (size_t)w >= outlen) return -1;
	return 0;
}
