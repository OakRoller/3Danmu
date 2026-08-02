#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define JSMN_STATIC
#include "vendor/jsmn.h"
#include "jsonx.h"

struct Json {
	const char *js;
	jsmntok_t  *tok;
	int         ntok;
};

Json *json_parse(const char *text, size_t len) {
	jsmn_parser p;
	jsmn_init(&p);
	int n = jsmn_parse(&p, text, len, NULL, 0);
	if (n <= 0) return NULL;

	Json *j = (Json *)malloc(sizeof(Json));
	if (!j) return NULL;
	j->tok = (jsmntok_t *)malloc(sizeof(jsmntok_t) * n);
	if (!j->tok) { free(j); return NULL; }

	jsmn_init(&p);
	j->ntok = jsmn_parse(&p, text, len, j->tok, n);
	if (j->ntok <= 0) { free(j->tok); free(j); return NULL; }
	j->js = text;
	return j;
}

void json_free(Json *j) {
	if (!j) return;
	free(j->tok);
	free(j);
}

/* 返回 token t 的下一个兄弟节点下标(跳过整个子树) */
static int tok_skip(const Json *j, int t) {
	int end = j->tok[t].end;
	int i = t + 1;
	while (i < j->ntok && j->tok[i].start < end)
		i++;
	return i;
}

static bool tok_key_eq(const Json *j, int t, const char *key, size_t keylen) {
	const jsmntok_t *k = &j->tok[t];
	if (k->type != JSMN_STRING) return false;
	if ((size_t)(k->end - k->start) != keylen) return false;
	return memcmp(j->js + k->start, key, keylen) == 0;
}

/* 在对象 obj 中找键,返回值 token 下标 */
static int obj_get(const Json *j, int obj, const char *key, size_t keylen) {
	if (obj < 0 || obj >= j->ntok || j->tok[obj].type != JSMN_OBJECT) return -1;
	int i = obj + 1;
	int pairs = j->tok[obj].size;
	for (int p = 0; p < pairs && i < j->ntok; p++) {
		int val = i + 1;
		if (tok_key_eq(j, i, key, keylen))
			return val;
		i = tok_skip(j, val);
	}
	return -1;
}

int json_arr_len(const Json *j, int tok) {
	if (tok < 0 || tok >= j->ntok || j->tok[tok].type != JSMN_ARRAY) return -1;
	return j->tok[tok].size;
}

int json_arr_at(const Json *j, int tok, int idx) {
	if (tok < 0 || tok >= j->ntok || j->tok[tok].type != JSMN_ARRAY) return -1;
	if (idx < 0 || idx >= j->tok[tok].size) return -1;
	int i = tok + 1;
	for (int e = 0; e < idx; e++)
		i = tok_skip(j, i);
	return i;
}

int json_find(const Json *j, int root, const char *path) {
	int cur = (root < 0) ? 0 : root;
	if (!path || !*path) return cur;
	const char *p = path;
	while (*p) {
		if (*p == '.') { p++; continue; }
		if (*p == '[') {
			char *endp;
			long idx = strtol(p + 1, &endp, 10);
			if (*endp != ']') return -1;
			cur = json_arr_at(j, cur, (int)idx);
			if (cur < 0) return -1;
			p = endp + 1;
		} else {
			const char *seg = p;
			while (*p && *p != '.' && *p != '[') p++;
			cur = obj_get(j, cur, seg, (size_t)(p - seg));
			if (cur < 0) return -1;
		}
	}
	return cur;
}

/* 把 codepoint 编码为 UTF-8,返回写入字节数 */
static int utf8_encode(uint32_t cp, char *out) {
	if (cp < 0x80) { out[0] = (char)cp; return 1; }
	if (cp < 0x800) {
		out[0] = (char)(0xC0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = (char)(0xE0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	out[0] = (char)(0xF0 | (cp >> 18));
	out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
	out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
	out[3] = (char)(0x80 | (cp & 0x3F));
	return 4;
}

static uint32_t hex4(const char *s) {
	uint32_t v = 0;
	for (int i = 0; i < 4; i++) {
		char c = s[i];
		v <<= 4;
		if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
		else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
	}
	return v;
}

/* 反转义 JSON 字符串 src[0..n) 到 out。
 * 注意:单字节只需要 1 字节空间,\uXXXX 最多 4 字节,按需检查,
 * 不能一刀切留 5 字节余量(会把短缓冲区的最后几个字符截掉)。 */
static void unescape(const char *src, size_t n, char *out, size_t outlen) {
	size_t o = 0;
	for (size_t i = 0; i < n; ) {
		char c = src[i];
		if (c != '\\') {
			if (o + 1 >= outlen) break;
			out[o++] = c;
			i++;
			continue;
		}
		if (i + 1 >= n) break;
		char e = src[i + 1];
		i += 2;
		if (e == 'u') {
			if (i + 4 > n) break;
			uint32_t cp = hex4(src + i);
			i += 4;
			/* surrogate pair */
			if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= n &&
			    src[i] == '\\' && src[i + 1] == 'u') {
				uint32_t lo = hex4(src + i + 2);
				if (lo >= 0xDC00 && lo <= 0xDFFF) {
					cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
					i += 6;
				}
			}
			char enc[4];
			int len = utf8_encode(cp, enc);
			if (o + (size_t)len >= outlen) break;
			memcpy(out + o, enc, (size_t)len);
			o += (size_t)len;
			continue;
		}
		if (o + 1 >= outlen) break;
		switch (e) {
			case 'n': out[o++] = '\n'; break;
			case 't': out[o++] = '\t'; break;
			case 'r': out[o++] = '\r'; break;
			case 'b': out[o++] = '\b'; break;
			case 'f': out[o++] = '\f'; break;
			case '"': out[o++] = '"';  break;
			case '\\': out[o++] = '\\'; break;
			case '/': out[o++] = '/';  break;
			default:  out[o++] = e;   break;
		}
	}
	/* 【截断要落在字符边界上】上面按字节拷,装不下就 break —— 一个汉字
	 * 三字节,正好卡在中间就留下半个字符,屏幕上是个乱码方块。
	 * 这里把结尾不完整的那一截退掉。标题、用户名、弹幕全经过这儿。 */
	while (o > 0 && (out[o - 1] & 0xC0) == 0x80) {
		/* 往前找到首字节,看这串本该多长 */
		size_t st = o - 1;
		while (st > 0 && (out[st] & 0xC0) == 0x80) st--;
		unsigned char lead = (unsigned char)out[st];
		size_t need = (lead >= 0xF0) ? 4 : (lead >= 0xE0) ? 3 :
		              (lead >= 0xC0) ? 2 : 1;
		if (o - st >= need) break;      /* 完整,不动 */
		o = st;                          /* 半个字符:整个退掉 */
	}
	out[o] = 0;
}

bool json_get_str(const Json *j, int root, const char *path, char *out, size_t outlen) {
	if (!out || outlen == 0) return false;
	out[0] = 0;
	int t = json_find(j, root, path);
	if (t < 0) return false;
	const jsmntok_t *tk = &j->tok[t];
	if (tk->type != JSMN_STRING && tk->type != JSMN_PRIMITIVE) return false;
	unescape(j->js + tk->start, (size_t)(tk->end - tk->start), out, outlen);
	return true;
}

bool json_get_num(const Json *j, int root, const char *path, double *out) {
	int t = json_find(j, root, path);
	if (t < 0) return false;
	const jsmntok_t *tk = &j->tok[t];
	if (tk->type != JSMN_PRIMITIVE && tk->type != JSMN_STRING) return false;
	char buf[40];
	size_t n = (size_t)(tk->end - tk->start);
	if (n >= sizeof(buf)) n = sizeof(buf) - 1;
	memcpy(buf, j->js + tk->start, n);
	buf[n] = 0;
	char *endp;
	double v = strtod(buf, &endp);
	if (endp == buf) return false;
	*out = v;
	return true;
}

bool json_get_int(const Json *j, int root, const char *path, int64_t *out) {
	int t = json_find(j, root, path);
	if (t < 0) return false;
	const jsmntok_t *tk = &j->tok[t];
	if (tk->type != JSMN_PRIMITIVE && tk->type != JSMN_STRING) return false;
	char buf[32];
	size_t n = (size_t)(tk->end - tk->start);
	if (n >= sizeof(buf)) n = sizeof(buf) - 1;
	memcpy(buf, j->js + tk->start, n);
	buf[n] = 0;
	char *endp;
	int64_t v = strtoll(buf, &endp, 10);
	if (endp == buf) return false;
	*out = v;
	return true;
}
