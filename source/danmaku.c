#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "danmaku.h"
#include "net.h"
#include "ui.h"

/* 容量上限按机型给:New 3DS 内存与 GPU 都宽裕,老机型保守些。
 * 一条 DmItem 约 112 字节 → 3000 条约 330KB,HBL 下完全吃得下 */
#define DM_MAX_N3   3000
#define DM_MAX_OLD  1500
#define DM_DRAW_N3    80      /* 单帧最多绘制条数 */
#define DM_DRAW_OLD   48
#define DM_MAX_ROWS 18        /* 行数上限(小字号时) */
#define DM_LIFE_BASE 8.0f     /* 一条弹幕横穿屏幕的秒数(1.0x 档) */
#define DM_TEXTLEN  96

typedef struct {
	float t;                  /* 出现时间(秒) */
	uint32_t color;           /* C2D 颜色 */
	float w;                  /* 文本像素宽(懒计算,<0 未算) */
	uint8_t row;
	char text[DM_TEXTLEN];
} DmItem;

static DmItem *s_items = NULL;
static volatile int s_count = 0;      /* 发布后才 >0(渲染线程只读) */
static volatile int s_loading = 0;    /* 后台加载中 */
static Thread s_thread = NULL;
static int64_t s_pending_cid = 0;
static float s_scale = 0.52f; /* 字号(dm_set_size)。0.52 = 清晰档,见 ui.c eff_scale */
static float s_row_h = 16.0f;
static int   s_rows  = 14;    /* 滚动行数 = 可用高度/行高,随字号与覆盖范围变 */
/* 覆盖范围档位:0=全屏 1=1/2 2=1/4 3=1/8(从顶部往下算) */
static int   s_area  = 0;
static const float DM_AREA_FRAC[4] = { 1.0f, 0.5f, 0.25f, 0.125f };
/* 弹幕速度。存的是「横穿一屏要几秒」——**倍率越大越慢**,
 * 所以档位表是倒过来的:1.5x 快 = 只用 0.5 倍的时间。
 * 档位照搬 wiliwili(danmaku_style_speed:150/125/100/75/50,默认 100),
 * 只是把它那边的基准 12s 换成本项目实机调出来的 8s ——
 * 屏宽差三倍多,照抄绝对秒数会让弹幕在 400px 上慢得发呆。 */
static const float DM_SPEED_MUL[5] = { 1.5f, 1.25f, 1.0f, 0.75f, 0.5f };
static int   s_speed = 2;                      /* 默认 1.0x */
static float s_life  = DM_LIFE_BASE;
static int s_cursor = 0;      /* 第一个可能仍在屏上的下标 */
static double s_last_clock = 0;
static volatile int s_fresh = 0;   /* 刚发布新数据,首次绘制要对齐当前时刻 */

/* 行占用:记录每行最后一条弹幕的出现时间与宽度,用来算它此刻的右边缘。
 * 行号不再预先按 i%ROWS 轮排,而是入场时挑最空的一行 */
/* 机型相关的两个上限,首次使用时查一次 */
static int s_max = 0, s_draw_max = 0;
static void limits_init(void) {
	if (s_max) return;
	bool n3 = false;
	APT_CheckNew3DS(&n3);
	s_max      = n3 ? DM_MAX_N3  : DM_MAX_OLD;
	s_draw_max = n3 ? DM_DRAW_N3 : DM_DRAW_OLD;
}

/* 自己发的弹幕(本地即时显示,高亮色) */
#define LOCAL_MAX 8
typedef struct { float t; float w; uint8_t row; char text[DM_TEXTLEN]; } LocalDm;
static LocalDm s_local[LOCAL_MAX];
static int s_nlocal = 0;

static float s_row_t[DM_MAX_ROWS];
static float s_row_w[DM_MAX_ROWS];
static bool  s_row_used[DM_MAX_ROWS];

static void rows_clear(void) {
	for (int i = 0; i < DM_MAX_ROWS; i++) s_row_used[i] = false;
}

/* 行数 = 可用高度 / 行高。可用高度 = 上屏 234px x 覆盖范围。
 * 【为什么允许只剩 1 行】1/8 屏(约 29px)在大字号下就只装得下一行 ——
 * 那正是用户选它的本意。旧代码的下限 4 会让 1/4 和 1/8 变成同一档。 */
static void rows_recalc(void) {
	float avail = 234.0f * DM_AREA_FRAC[s_area];
	int n = (s_row_h > 1.0f) ? (int)(avail / s_row_h) : 1;
	if (n < 1) n = 1;
	if (n > DM_MAX_ROWS) n = DM_MAX_ROWS;
	s_rows = n;
}

/* 行数变了:旧行号可能已超出新行数(会画到范围外),全部作废重排 */
static void rows_reassign(void) {
	for (int i = 0; i < s_count; i++) s_items[i].row = 0xFF;
	for (int i = 0; i < s_nlocal; i++) s_local[i].row = 0xFF;
	rows_clear();
}

/* 字号 / 覆盖范围任何一边动了都走这里。
 * 【必须无条件重算一次,不能"值没变就早退"】s_rows 的静态初值 14 和
 * 234/19=12 对不上,而默认档(中号 + 全屏)恰好两边都"没变" ——
 * 早退就把这次校正一起跳过了,最底下两行画在 y=230/249,
 * 也就是**屏幕外**。落在那两行的弹幕从来没人见过。 */
static void layout_apply(void) {
	int old = s_rows;
	rows_recalc();
	if (old != s_rows) rows_reassign();
}

int dm_rows(void) { return s_rows; }

int dm_count(void) { return s_count; }
bool dm_loading(void) { return s_loading != 0; }

/* ---------- 解压 ---------- */

static uint8_t *dm_inflate(const uint8_t *in, size_t inlen, size_t *outlen) {
	limits_init();
	/* 解压后的 XML 上限:条数上限提高后,原来的 6MB 会成为新的瓶颈 */
	size_t hard = (s_max > DM_MAX_OLD ? 10u : 6u) * 1024 * 1024;
	size_t cap = inlen * 8 + 65536;
	if (cap > hard) cap = hard;
	uint8_t *buf = (uint8_t *)malloc(cap);
	if (!buf) return NULL;

	for (int mode = 0; mode < 2; mode++) {
		z_stream zs;
		memset(&zs, 0, sizeof(zs));
		/* 先按 raw deflate(B 站 dm 接口),失败再按 zlib/gzip 自动 */
		if (inflateInit2(&zs, mode == 0 ? -MAX_WBITS : 15 + 32) != Z_OK)
			continue;
		zs.next_in = (Bytef *)in;
		zs.avail_in = (uInt)inlen;
		zs.next_out = buf;
		zs.avail_out = (uInt)(cap - 1);
		int r = inflate(&zs, Z_FINISH);
		size_t got = zs.total_out;
		inflateEnd(&zs);
		if ((r == Z_STREAM_END || r == Z_BUF_ERROR) && got > 0) {
			buf[got] = 0;
			*outlen = got;
			return buf;
		}
	}
	free(buf);
	return NULL;
}

/* ---------- 解析 ---------- */

static void xml_unescape(char *s) {
	char *r = s, *w = s;
	while (*r) {
		if (*r == '&') {
			if (!strncmp(r, "&amp;", 5))  { *w++ = '&'; r += 5; continue; }
			if (!strncmp(r, "&lt;", 4))   { *w++ = '<'; r += 4; continue; }
			if (!strncmp(r, "&gt;", 4))   { *w++ = '>'; r += 4; continue; }
			if (!strncmp(r, "&quot;", 6)) { *w++ = '"'; r += 6; continue; }
			if (!strncmp(r, "&#39;", 5))  { *w++ = '\''; r += 5; continue; }
		}
		*w++ = *r++;
	}
	*w = 0;
}

static int dm_cmp(const void *a, const void *b) {
	float d = ((const DmItem *)a)->t - ((const DmItem *)b)->t;
	return (d > 0) - (d < 0);
}

static int dm_parse(const char *xml) {
	/* 先数总量,决定抽样间隔 */
	limits_init();
	int total = 0;
	for (const char *p = xml; (p = strstr(p, "<d p=\"")); p += 6)
		total++;
	if (total == 0) return 0;
	int step = (total + s_max - 1) / s_max;
	if (step < 1) step = 1;

	DmItem *items = (DmItem *)calloc((size_t)s_max, sizeof(DmItem));
	if (!items) return -1;

	int idx = 0, n = 0;
	const char *p = xml;
	while ((p = strstr(p, "<d p=\"")) && n < s_max) {
		p += 6;
		if ((idx++ % step) != 0) continue;

		/* p="time,mode,size,color,..." */
		float t = strtof(p, NULL);
		const char *c1 = strchr(p, ',');
		if (!c1) continue;
		int mode = atoi(c1 + 1);
		const char *c2 = strchr(c1 + 1, ',');
		const char *c3 = c2 ? strchr(c2 + 1, ',') : NULL;
		uint32_t rgb = 0xFFFFFF;
		if (c3) rgb = (uint32_t)strtoul(c3 + 1, NULL, 10);
		if (mode >= 7) continue;   /* 跳过高级/代码弹幕 */

		const char *end_attr = strstr(p, "\">");
		if (!end_attr) break;
		const char *txt = end_attr + 2;
		const char *end = strstr(txt, "</d>");
		if (!end) break;
		size_t len = (size_t)(end - txt);
		if (len == 0 || len >= DM_TEXTLEN) len = len >= DM_TEXTLEN ? DM_TEXTLEN - 1 : len;

		DmItem *it = &items[n];
		it->t = t;
		memcpy(it->text, txt, len);
		it->text[len] = 0;
		xml_unescape(it->text);
		if (!it->text[0]) continue;
		/* 【不透明】原来给 0xE6(90%),叠上字形本身只有半格墨的笔画,
		 * 实际落到屏幕上就更淡了。弹幕的可读性优先于「融进画面」的观感,
		 * 何况现在有描边托底,全不透明也不会显得糊在脸上。 */
		it->color = C2D_Color32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF,
		                        rgb & 0xFF, 0xFF);
		it->w = -1.0f;
		n++;
		p = end;
	}
	if (step > 1)
		printf("danmaku: %d total, sampled 1/%d\n", total, step);
	qsort(items, (size_t)n, sizeof(DmItem), dm_cmp);
	for (int i = 0; i < n; i++)
		items[i].row = 0xFF;          /* 0xFF = 尚未分配行,入场时再挑 */
	/* 原子发布:先写数组指针,内存屏障后再公布条数 */
	s_items = items;
	s_cursor = 0;
	s_last_clock = 0;
	s_fresh = 1;
	__dmb();
	s_count = n;
	return n;
}

/* ---------- 对外 ---------- */

/* 仅释放数据,不碰线程(供 dm_load 内部与 dm_free 共用) */
static void dm_clear_items(void) {
	s_count = 0;
	__dmb();
	free(s_items);
	s_items = NULL;
	s_cursor = 0;
}

int dm_load(int64_t cid) {
	dm_clear_items();
	char url[128], cidstr[24];
	{	/* i64 → str(newlib 无 %lld) */
		char tmp[24]; int i = 0, o = 0;
		uint64_t u = (uint64_t)cid;
		do { tmp[i++] = (char)('0' + u % 10); u /= 10; } while (u);
		while (i) cidstr[o++] = tmp[--i];
		cidstr[o] = 0;
	}
	snprintf(url, sizeof(url),
	         "https://api.bilibili.com/x/v1/dm/list.so?oid=%s", cidstr);

	HttpResponse res;
	if (net_get(url, &res) != 0) return -1;
	if (res.status != 200 || !res.data || res.len == 0) {
		net_response_free(&res);
		return -1;
	}

	int n;
	if (res.data[0] == '<') {          /* 未压缩 */
		n = dm_parse(res.data);
	} else {                            /* deflate */
		size_t xlen = 0;
		uint8_t *xml = dm_inflate((const uint8_t *)res.data, res.len, &xlen);
		if (!xml) { net_response_free(&res); return -1; }
		n = dm_parse((const char *)xml);
		free(xml);
	}
	net_response_free(&res);
	printf("danmaku: %d items\n", n);
	return n;
}

void dm_free(void) {
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
			printf("danmaku thread join timeout\n");
			threadDetach(s_thread);
		} else {
			threadFree(s_thread);
		}
		s_thread = NULL;
	}
	s_loading = 0;
	dm_clear_items();
}

/* ---------- 异步加载 ---------- */

static void dm_thread_main(void *arg) {
	(void)arg;
	/* 立刻开始:开头的弹幕一条都不能漏。
	 * 网络不稳时 0.3 秒连打 3 次会一秒内全烧完,整场就没弹幕了。
	 * 改为退避重试(0.5s→1s→1.5s→2s→2.5s→3s),总窗口约 10 秒,
	 * 落在播放器"等弹幕"的 20 秒上限之内 —— Wi-Fi 抖一下也能等到 */
	for (int try_ = 0; try_ < 7; try_++) {
		if (dm_load(s_pending_cid) >= 0) break;
		if (try_ == 6) { printf("danmaku gave up\n"); break; }
		printf("danmaku load failed, retry %d\n", try_ + 1);
		s64 ms = 500 * (try_ + 1);
		if (ms > 3000) ms = 3000;
		svcSleepThread(ms * 1000 * 1000LL);
	}
	__dmb();
	s_loading = 0;
}

void dm_load_async(int64_t cid) {
	s_nlocal = 0;      /* 换视频,清掉自己发的 */
	dm_free();
	s_pending_cid = cid;
	s_loading = 1;
	/* 挑空闲核心:core3(New3DS 备用)→ core1(系统核,已申请时间片)→ 任意。
	 * 优先级调低,不与解码/呈现抢 CPU */
	static const int cores[] = { 3, 2, -2 };   /* 核心 1 是系统核,别碰 */
	for (int i = 0; i < 3 && !s_thread; i++)
		s_thread = threadCreate(dm_thread_main, NULL, 32 * 1024, 0x38, cores[i], false);
	if (!s_thread) {             /* 建不了线程就同步加载 */
		dm_load(cid);
		s_loading = 0;
	}
}

void dm_add_local(const char *text, double t) {
	LocalDm *d = &s_local[s_nlocal % LOCAL_MAX];
	if (s_nlocal < LOCAL_MAX) s_nlocal++;
	d->t = (float)t;
	d->w = -1.0f;
	d->row = 0xFF;
	snprintf(d->text, sizeof(d->text), "%s", text);
}

void dm_set_size(int level) {
	/* 中 = 原生 1:1(吸附点,最清晰),小/大刻意取在吸附窗口外 ——
	 * 用户要的就是尺寸不同,不能被吸附拉回同一档。
	 *
	 * 【1.5 倍在轮廓字体上可以,在点阵字体上不行】曾经短暂换过点阵字,
	 * 那时大档必须改成精确整数倍(ui_scale_x(2)):点阵字每根笔画正好
	 * 占满整数个纹素,1.5 倍会把一半笔画整根抹掉。现在字体是 Noto
	 * (轮廓 + 抗锯齿),边缘本来就是半透明的,非整数倍只是更柔和一点,
	 * 不会掉笔画 —— 所以回到 0.78(=1.5 倍)。真换回点阵字记得改回去。
	 * 【行高向字体要,别再乘常数】常数是上一个字体的 1:1 字高,
	 * 换字体就不对了;ui_text_height 拿到的是吸附之后的真实字高。 */
	float sc = (level <= 0) ? 0.39f
	         : (level >= 2) ? 0.78f
	         : UI_SHARP;
	/* 【行高无条件重算,不能"值没变就早退"】开机时 s_scale 的静态初值
	 * 正好等于中档,于是这一句会被跳过,s_row_h 留在静态初值上 ——
	 * 和真实字高对不上,最底下几行就画到屏幕外(下面 layout_apply
	 * 的注释里记着同一个坑的另一半)。换字体后字高变了,更要每次都算。 */
	bool changed = (sc != s_scale);
	s_scale = sc;
	s_row_h = ui_text_height(sc) + 2.0f;
	if (changed) {
		/* 字号变了:宽度缓存作废,行号一定要重排(行高跟着变了) */
		for (int i = 0; i < s_count; i++) s_items[i].w = -1.0f;
		for (int i = 0; i < s_nlocal; i++) s_local[i].w = -1.0f;
		rows_recalc();
		rows_reassign();
		return;
	}
	layout_apply();
}

void dm_set_speed(int level) {
	if (level < 0) level = 0;
	if (level > 4) level = 4;
	s_speed = level;
	/* 【不清行占用、不动宽度缓存】s_row_t/s_row_w 存的是「出现时刻 + 宽度」,
	 * 右边缘是每帧拿当前 s_life 现算的 —— 换速度后这些记录照样成立。
	 * 字宽更是与速度无关,清了就是白白重做几百次字形解析。
	 * 代价只有在飞的那几条会瞬间挪一下位,下一批就正常了。 */
	s_life = DM_LIFE_BASE * DM_SPEED_MUL[level];
}

int dm_speed(void) { return s_speed; }

void dm_set_area(int level) {
	if (level < 0) level = 0;
	if (level > 3) level = 3;
	s_area = level;
	/* 只动行数,字宽不受影响 —— 别把宽度缓存也清了,那是几百次字形解析 */
	layout_apply();
}

void dm_reset(void) {
	s_cursor = 0;
	s_last_clock = 0;
	rows_clear();
	for (int i = 0; i < s_count; i++) s_items[i].row = 0xFF;
}

static void draw_local(double clock, float xoff) {
	for (int i = 0; i < s_nlocal; i++) {
		LocalDm *d = &s_local[i];
		if (clock < d->t || clock > d->t + s_life) continue;
		if (d->w < 0) d->w = ui_text_width(d->text, s_scale);
		if (d->row == 0xFF) d->row = (uint8_t)(i % s_rows);
		float prog = (float)((clock - d->t) / s_life);
		float x = 400.0f - prog * (400.0f + d->w);
		if (x + d->w < 0 || x > 400) continue;
		/* 高亮色 + 微描边感(错位重画),一眼认出是自己的 */
		ui_text(x + xoff + 1, 3.0f + d->row * s_row_h, s_scale,
		        C2D_Color32(0, 0, 0, 0xC0), d->text);
		ui_text(x + xoff, 2.0f + d->row * s_row_h, s_scale,
		        UI_COL_ACCENT, d->text);
	}
}

void dm_draw(double clock, float xoff) {
	/* 【这一路不加重】界面文字画两遍是为了补 12px 汉字的半格墨,
	 * 但弹幕单帧上限 80 条、3D 还要双眼各来一遍 —— 加倍会顶穿 citro2d
	 * 的顶点预算,而顶穿之后是**静默丢绘制**(画面残缺,且很难归因)。
	 * 弹幕本来就在动、也不用逐字细读,少这一遍看不出来。 */
	ui_text_boost(false);
	draw_local(clock, xoff);
	if (!s_items || s_count == 0) { ui_text_boost(true); return; }

	if (s_fresh) {
		/* 数据刚发布:从头重扫、清空行占用。
		 * 注意这里**不跳过**任何弹幕——即使数据是播到一半才到的,
		 * 该出现的也照样出现(挤一点也不能漏)。真正的解法是不让它迟到:
		 * 选中视频按 A 的那一刻就开始下载,并且播放器会等它就绪 */
		s_cursor = 0;
		rows_clear();
		s_last_clock = clock;
		s_fresh = 0;
	}
	if (clock < s_last_clock - 1.0) {   /* 时间回跳:重扫并清行占用 */
		s_cursor = 0;
		rows_clear();
	}
	s_last_clock = clock;

	/* 推进游标:跳过已完全滚出屏幕的 */
	while (s_cursor < s_count && s_items[s_cursor].t + s_life < clock)
		s_cursor++;

	limits_init();
	int drawn = 0;
	for (int i = s_cursor; i < s_count && drawn < s_draw_max; i++) {
		DmItem *it = &s_items[i];
		if (it->t > clock) break;
		if (it->w < 0)
			it->w = ui_text_width(it->text, s_scale);
		if (it->row == 0xFF) {
			/* 入场:挑此刻最空的一行(上一条的右边缘最靠左的那行)。
			 * 固定轮排的话,同一时刻涌进来的一批弹幕必然首尾相叠 */
			int best = 0;
			float best_edge = 1e9f;
			for (int r = 0; r < s_rows; r++) {
				float edge = -1e9f;      /* 空行:最优 */
				if (s_row_used[r]) {
					float pr = (float)((clock - s_row_t[r]) / s_life);
					edge = 400.0f - pr * (400.0f + s_row_w[r]) + s_row_w[r];
				}
				if (edge < best_edge) { best_edge = edge; best = r; }
			}
			it->row = (uint8_t)best;
			s_row_used[best] = true;
			s_row_t[best] = it->t;
			s_row_w[best] = it->w;
		}
		float prog = (float)((clock - it->t) / s_life);
		float x = 400.0f - prog * (400.0f + it->w);
		if (x + it->w < 0 || x > 400) continue;
		/* 【描边不是装饰,是弹幕能不能看清的前提】
		 * 字体是抗锯齿轮廓字,图集里九成以上的笔画像素只有半格墨 ——
		 * 界面文字靠 ui_text_boost 重画补回来,而弹幕这一路**关掉了加重**
		 * (顶点预算,见下面 ui_text_boost 的说明),于是它是全工程唯一
		 * 又淡又没有任何衬底的文字,压在亮画面上几乎透明。
		 * 错开 1px 画一遍深色再画本体:代价和加重一样是一遍顶点,
		 * 但**压在任何底色上都有对比**,比单纯加深自身管用得多。
		 * (自己发的弹幕早就这么画了,见 draw_local——这里只是补齐。) */
		float dy = 2.0f + it->row * s_row_h;
		ui_text(x + xoff + 1.0f, dy + 1.0f, s_scale,
		        C2D_Color32(0, 0, 0, 0xC0), it->text);
		ui_text(x + xoff, dy, s_scale, it->color, it->text);
		drawn++;
	}
	ui_text_boost(true);
}
