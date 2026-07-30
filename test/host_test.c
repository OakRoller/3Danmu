/* PC 端单元测试:md5 / wbi / jsonx(与 3DS 无关的纯逻辑) */
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "../source/md5.h"
#include "../source/wbi.h"
#include "../source/jsonx.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
	if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
	else printf("ok: %s\n", msg); } while (0)

int main(void) {
	/* --- md5 --- */
	char h[33];
	md5_hex("", 0, h);
	CHECK(!strcmp(h, "d41d8cd98f00b204e9800998ecf8427e"), "md5(empty)");
	md5_hex("abc", 3, h);
	CHECK(!strcmp(h, "900150983cd24fb0d6963f7d28e17f72"), "md5(abc)");
	md5_hex("The quick brown fox jumps over the lazy dog", 43, h);
	CHECK(!strcmp(h, "9e107d9d372bb6826bd81d3542a419d6"), "md5(fox)");
	/* >64 bytes, cross-block */
	char many_a[81];
	memset(many_a, 'a', sizeof(many_a));
	md5_hex(many_a, 81, h); /* 跨块边界 */
	CHECK(!strcmp(h, "986e6938ed767a8ae9530eef54bfe5f1"), "md5(81*a)");

	/* --- wbi mixin key(bilibili-API-collect 文档示例) --- */
	char mixin[33];
	wbi_mixin_key("7cd084941338484aae1ad9425b84077c", "4932caff0ff746eab6f01bf08b70ac45", mixin);
	CHECK(!strcmp(mixin, "ea1db124af3c7062474693fa704f4ff8"), "wbi mixin key");

	/* --- wbi sign(与 python 参考实现对拍,见 run_tests.sh) --- */
	const char *keys[] = { "foo", "bar", "zab" };
	const char *vals[] = { "one one four", "五一四", "1919810" };
	char signed_q[2048];
	int r = wbi_sign(keys, vals, 3, mixin, 1702204169, signed_q, sizeof(signed_q));
	CHECK(r == 0, "wbi_sign returns 0");
	/* 与 python: urllib.parse.urlencode(sorted, quote_via=quote)+md5 对拍得到 */
	CHECK(!strcmp(signed_q,
	      "bar=%E4%BA%94%E4%B8%80%E5%9B%9B&foo=one%20one%20four&wts=1702204169"
	      "&zab=1919810&w_rid=993fe49bb52e4183d188d4712eed9862"),
	      "wbi_sign matches python reference");

	/* --- jsonx --- */
	const char *js = "{\"code\":0,\"data\":{\"list\":[{\"bvid\":\"BV1xx411c7mD\",\"title\":\"\\u4f60\\u597d hello\",\"owner\":{\"name\":\"UP\\u4e3b\"},\"stat\":{\"view\":123456},\"cid\":789},{\"bvid\":\"BV2\",\"title\":\"second\"}],\"wbi_img\":{\"img_url\":\"https://i0.hdslb.com/bfs/wbi/abc123.png\"}}}";
	Json *j = json_parse(js, strlen(js));
	CHECK(j != NULL, "json_parse");
	int64_t code = -1;
	CHECK(json_get_int(j, -1, "code", &code) && code == 0, "json code==0");
	char buf[256];
	CHECK(json_get_str(j, -1, "data.list[0].bvid", buf, sizeof(buf)) &&
	      !strcmp(buf, "BV1xx411c7mD"), "json bvid");
	CHECK(json_get_str(j, -1, "data.list[0].title", buf, sizeof(buf)) &&
	      !strcmp(buf, "你好 hello"), "json title unescape CJK");
	CHECK(json_get_str(j, -1, "data.list[0].owner.name", buf, sizeof(buf)) &&
	      !strcmp(buf, "UP主"), "json nested owner.name");
	int64_t view = 0;
	CHECK(json_get_int(j, -1, "data.list[0].stat.view", &view) && view == 123456, "json stat.view");
	int arr = json_find(j, -1, "data.list");
	CHECK(json_arr_len(j, arr) == 2, "json arr len");
	int el1 = json_arr_at(j, arr, 1);
	CHECK(json_get_str(j, el1, "title", buf, sizeof(buf)) && !strcmp(buf, "second"),
	      "json arr_at + relative path");
	CHECK(json_get_str(j, -1, "data.wbi_img.img_url", buf, sizeof(buf)) &&
	      strstr(buf, "abc123.png") != NULL, "json wbi_img url");
	CHECK(json_find(j, -1, "data.nothing") == -1, "json missing key");
	/* 回归:12 字符 bvid 存入 16 字节缓冲不能被截断(曾因余量检查过严丢最后一位) */
	char bv16[16];
	CHECK(json_get_str(j, -1, "data.list[0].bvid", bv16, sizeof(bv16)) &&
	      strlen(bv16) == 12, "json exact-fit no truncation");
	char tiny[5]; /* "true" 存入 5 字节缓冲 */
	Json *jb = json_parse("{\"isLogin\":true}", 16);
	CHECK(jb && json_get_str(jb, -1, "isLogin", tiny, sizeof(tiny)) &&
	      !strcmp(tiny, "true"), "json bool primitive full read");
	json_free(jb);
	json_free(j);

	printf(failures ? "\n%d FAILURES\n" : "\nALL PASS\n", failures);
	return failures ? 1 : 0;
}
