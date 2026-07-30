/* CC 字幕(实现说明见 subtitle.h)
 * 正文格式:{"body":[{"from":1.2,"to":3.4,"content":"..."}]},按时间有序 */
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "subtitle.h"
#include "bili.h"
#include "jsonx.h"
#include "net.h"
#include "ui.h"

#define SUB_MAX     800
#define SUB_TEXTLEN 120

typedef struct {
	float from, to;
	float w;                 /* 像素宽,懒计算 */
	short cut;               /* 折行位置(字节偏移),-1=未算 */
	char text[SUB_TEXTLEN];
} SubLine;

static SubLine *s_lines = NULL;
static volatile int s_n = 0;
static volatile int s_loading = 0;
static Thread s_thread = NULL;
/* 请求参数随线程参数传递,不走全局——全局变量在快速换视频时
 * 可能被下一次调用覆盖,线程读到的就是别的视频的 id */
typedef struct { u32 gen; char bvid[16]; int64_t aid, cid; } SubReq;
static SubReq s_req;
static int s_cursor = 0;
static double s_last_clock = 0;
static float s_scale = 0.52f;  /* 字号(sub_set_size)。0.52 = 清晰档 */
/* 代际:每次 sub_load_async +1。旧线程若 join 超时仍在跑,回来时代际
 * 已经变了,它的结果必须丢弃——否则会把上一个视频的字幕"复活"到
 * 当前视频上(实测现象:开字幕出现的是刚才看的那个视频的字幕) */
static volatile u32 s_gen = 0;
static double s_duration = 0;    /* 视频时长(秒),0=未知 */

void sub_set_duration(double seconds) { s_duration = seconds; }

void sub_set_size(int level) {
	/* 档位设计同弹幕(见 danmaku.c dm_set_size 的说明):中档 0.52 走清晰
	 * 吸附,小/大取在窗口外保住尺寸差异 */
	float sc = (level <= 0) ? 0.39f : (level >= 2) ? 0.78f : 0.52f;
	if (sc == s_scale) return;
	s_scale = sc;
	for (int i = 0; i < s_n; i++) {                      /* 宽度/折行缓存作废 */
		s_lines[i].w = -1.0f;
		s_lines[i].cut = -1;
	}
}

bool sub_available(void) { return s_n > 0; }
bool sub_loading(void) { return s_loading != 0; }
int  sub_count(void) { return s_n; }

static void sub_clear_items(void) {
	s_n = 0;
	__dmb();
	free(s_lines);
	s_lines = NULL;
	s_cursor = 0;
}

static void sub_thread_main(void *arg) {
	SubReq req = *(const SubReq *)arg;   /* 立刻取值,之后不再碰全局 */
	u32 mygen = req.gen;
	char *body = NULL;
	size_t blen = 0;
	if (bili_subtitle_fetch(req.bvid, req.aid, req.cid, &body, &blen) == 0 && body) {
		Json *j = json_parse(body, blen);
		if (!j) printf("sub: body JSON parse failed (%d bytes)\n", (int)blen);
		if (j) {
			int arr = json_find(j, -1, "body");
			int n = json_arr_len(j, arr);
			if (n > SUB_MAX) n = SUB_MAX;
			SubLine *ls = (SubLine *)calloc((size_t)(n > 0 ? n : 1),
			                                sizeof(SubLine));
			int m = 0;
			for (int i = 0; i < n && ls; i++) {
				int el = json_arr_at(j, arr, i);
				double f = 0, t = 0;
				if (!json_get_num(j, el, "from", &f) ||
				    !json_get_num(j, el, "to", &t))
					continue;
				SubLine *L = &ls[m];
				L->from = (float)f;
				L->to = (float)t;
				L->w = -1.0f;
				L->cut = -1;      /* calloc 出来是 0,那会被当成"已算好" */
				if (!json_get_str(j, el, "content", L->text,
				                  sizeof(L->text)) || !L->text[0])
					continue;
				m++;
			}
			/* 合理性校验:最后一句都超出视频时长一大截,
			 * 说明这份字幕根本不属于本视频(实测踩过"字幕串台") */
			if (m > 0 && s_duration > 1.0 &&
			    ls[m - 1].from > s_duration + 60.0) {
				printf("sub: REJECT last=%ds > duration=%ds\n",
				       (int)ls[m - 1].from, (int)s_duration);
				m = 0;
			}
			if (m > 0 && mygen == s_gen) {   /* 无锁发布(代际相符才算数) */
				s_lines = ls;
				s_cursor = 0;
				__dmb();
				s_n = m;
				printf("subtitle: %d lines\n", m);
				/* 打前 3 行的时间戳 + 文本:时间戳不合理 = 时间轴问题,
				 * 时间戳正常但文不对题 = 内容问题,一眼可分 */
				for (int k = 0; k < 3 && k < m; k++) {
					char pfx[32];
					snprintf(pfx, sizeof(pfx), "sub[%d] %d-%ds: ", k,
					         (int)ls[k].from, (int)ls[k].to);
					ui_log_ascii(pfx, ls[k].text, 40);
				}
			} else if (m > 0) {
				printf("sub: stale result discarded (gen %lu != %lu)\n",
				       (unsigned long)mygen, (unsigned long)s_gen);
				free(ls);
			} else {
				printf("sub: parsed 0 usable lines (arr=%d)\n", n);
				free(ls);
			}
			json_free(j);
		}
		free(body);
	}
	__dmb();
	if (mygen == s_gen) s_loading = 0;   /* 陈旧线程不许清 loading 标志 */
}

bool sub_load_async(const char *bvid, int64_t aid, int64_t cid) {
	/* 【代际必须先加,再考虑要不要返回】
	 * 这个函数会被**渲染循环**调用,所以不能在这里 threadJoin 等待
	 * (sub_free 的 join 超时 10 秒,会把画面和弹幕一起冻住)。
	 * 但「不等」不等于「什么都不做」:上一轮线程还在跑时直接 return,
	 * 代际没变,它回来时 mygen == s_gen 就会**把上一个视频的字幕
	 * 发布到当前视频上** —— 正是「字幕串视频」。
	 * 先把代际推掉再返回,在途线程的结果一律作废。 */
	s_gen++;
	__dmb();
	if (s_loading) {
		printf("sub: prev load still running, deferring\n");
		return false;        /* 下一帧再来问,绝不在这里等 */
	}
	sub_free();
	s_gen++;                 /* 换视频:之前那轮的结果一律作废 */
	__dmb();
	s_req.gen = s_gen;
	snprintf(s_req.bvid, sizeof(s_req.bvid), "%s", bvid ? bvid : "");
	s_req.aid = aid;
	s_req.cid = cid;
	__dmb();
	s_loading = 1;
	static const int cores[] = { 3, 2, -2 };   /* 核心 1 是系统核,别碰 */
	for (int i = 0; i < 3 && !s_thread; i++)
		s_thread = threadCreate(sub_thread_main, &s_req, 32 * 1024,
		                        0x38, cores[i], false);
	if (!s_thread) { s_loading = 0; return false; }  /* 建不了线程就算了 */
	return true;
}

void sub_free(void) {
	s_gen++;                 /* 让所有在途线程的结果失效 */
	__dmb();
	/* 【退出路径上的 join 超时要分场景】
	 * 这三个后台线程(弹幕/字幕/评论)只在播放时存在 —— 这正是
	 * 「只有播放中按 HOME→X 才卡」的一大块原因:原本每个最多等 8~10 秒,
	 * 三个加起来能到半分钟,系统就一直停在 "Closing software"。
	 * 系统要关我们时网络已被 net_shutdown_begin 封死、线程都在往外走,
	 * 再按秒等没有意义,压到 0.5 秒。
	 *
	 * 【超时后不能 threadFree】线程还在跑,释放它的 Thread 结构就是
	 * use-after-free。只能 threadDetach 丢下它,让进程收尾时一起带走。 */
	if (s_thread) {
		u64 ns = net_is_shutting_down() ? 500000000ULL : 10000000000ULL;
		if (R_FAILED(threadJoin(s_thread, ns))) {
			printf("subtitle thread join timeout\n");
			threadDetach(s_thread);
		} else {
			threadFree(s_thread);
		}
		s_thread = NULL;
	}
	s_loading = 0;
	sub_clear_items();
}

void sub_draw(double clock) {
	if (!s_lines || s_n == 0) return;
	if (clock < s_last_clock - 1.0) s_cursor = 0;   /* 回跳重扫 */
	s_last_clock = clock;
	while (s_cursor < s_n && s_lines[s_cursor].to < clock)
		s_cursor++;
	if (s_cursor >= s_n) return;
	SubLine *L = &s_lines[s_cursor];
	if (clock < L->from) return;
	if (L->w < 0) L->w = ui_text_width(L->text, s_scale);

	const float MAXW = 384.0f;
	float lh = 23.0f * (s_scale / 0.52f);    /* 行高随字号(按吸附后的实际字高) */
	char l1[SUB_TEXTLEN], l2[SUB_TEXTLEN];
	l1[0] = l2[0] = 0;

	if (L->w <= MAXW) {
		snprintf(l1, sizeof(l1), "%s", L->text);
	} else {
		/* 超宽:折成两行。折行位置只算一次并缓存在 L->cut——
		 * 这段逐字加宽的循环每个字符都要量一次宽度(= 一次完整字形解析),
		 * 一句 40 字的字幕就是 40 次,3D 模式双眼再翻倍,
		 * 而同一句字幕会连续显示好几秒(几百帧)。每帧重算纯属浪费。 */
		if (L->cut < 0) {
			size_t i = 0, cut = 0;
			char tmp[SUB_TEXTLEN];
			while (L->text[i]) {
				size_t cl = 1;                    /* 本字符字节数 */
				unsigned char c = (unsigned char)L->text[i];
				if (c >= 0xF0) cl = 4;
				else if (c >= 0xE0) cl = 3;
				else if (c >= 0xC0) cl = 2;
				if (i + cl > sizeof(tmp) - 1) break;
				memcpy(tmp, L->text, i + cl);
				tmp[i + cl] = 0;
				if (ui_text_width(tmp, s_scale) > MAXW) break;
				i += cl;
				if (L->text[i] == ' ') cut = i;   /* 记住最后一个空格 */
			}
			if (cut == 0 || i - cut > 24) cut = i;   /* 没有合适空格就硬断 */
			if (cut == 0) cut = i ? i : 1;
			L->cut = (short)cut;
		}
		size_t cut = (size_t)L->cut;
		memcpy(l1, L->text, cut);
		l1[cut] = 0;
		const char *rest = L->text + cut;
		while (*rest == ' ') rest++;
		snprintf(l2, sizeof(l2), "%s", rest);
	}

	/* 底部居中(两行时整体上移一行);z 抬过弹幕(0.5),可读性优先 */
	int nlines = l2[0] ? 2 : 1;
	float y = 238.0f - lh * nlines;
	const char *lines[2] = { l1, l2 };
	for (int k = 0; k < nlines; k++) {
		float w = ui_text_width(lines[k], s_scale);
		if (w > MAXW) w = MAXW;
		float x = (400.0f - w) / 2.0f;
		float ly = y + k * lh;
		ui_rect_z(x - 6, ly, 0.6f, w + 12, lh, C2D_Color32(0, 0, 0, 0xA0));
		ui_text_clipped_z(x, ly + 2, 0.7f, s_scale, UI_COL_WHITE,
		                  lines[k], MAXW);
	}
}
