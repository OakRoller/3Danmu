/* 评论区(实现说明见 comment.h) */
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "comment.h"
#include <time.h>
#include "bili.h"
#include "net.h"
#include "ui.h"

#define CM_MAX   20      /* 一页 20 条,和接口 ps 参数一致 */
/* 单条评论的**行数上限**(不是固定行数)——真实行数按内容折出来,
 * 一条几个字就占一行,长的就占几十行,完整显示不截断。
 * 正文上限 1536 字节,一行放得下约 15 个汉字(下屏 296px / 汉字约 19px),
 * 即 45 字节左右 → 1536B 最多约 35 行。48 是留了余量的安全上界。
 * 上限存在只是为了 brk[] 定长,不是显示策略。
 * 【改这个数要连 wrap 的复杂度一起看】见下面那段说明。 */
#define CM_LINES 48
#define CM_W     296.0f  /* 正文可用宽度(下屏 320 减去左右边距和滚动条) */
/* 字号。系统字体(抗锯齿)的格子约 30px 高,所以 0.5 ≈ 15px 汉字。
 * 和 main.c / player.c 里那些 0.44~0.6 是同一个量纲,改动前先对齐它们。
 *
 * ---- 为什么最后没用点阵字体(别再走一遍) ----
 * 试过 Fusion Pixel 8/10/12px + mkbcfnt。点阵字体只有 1.0 倍绘制才不
 * 失真,而 12px 版本转出来汉字实际只有 **11 像素高** —— 用 tools/fontdump.py
 * 把字形从图集里挖出来看得很清楚:
 *     国 → 边框闭合、完全正确
 *     赢 → 底部退化成 #.#.#.# 的点阵,十七画塞不进十一行
 * 纹理里就长这样,屏幕上也长这样,**不是绘制端的问题**,是物理上限。
 * 换 -s、换过滤器、换 8px/10px 都无解(更小只会更糟)。
 * 要点阵又要笔画完整,得上 16px 字体,但那样一屏少放 40% 内容。
 *
 * 抗锯齿字体在这个尺寸反而更能认:灰阶能表达「这里有笔画」,
 * 二值点阵只能非黑即白。所以改用**轮廓字体**:自带 Noto Sans CJK 子集
 * (系统字体好看但缺字,非国行机会显示成"?")。
 * 字号的换算差异由 ui.c 的 UI_FONT_K 统一吸收,这里的数不随字体变。
 * 诊断工具留在 tools/ 下,真要重来先拿 fontdump.py 看字形再动手。 */
/* 比全局基准略大一点:评论正文是要**逐字读**的,和列表标题(扫一眼就够)
 * 不是一个用途。实机上按 0.5 排版偏小,提到 0.6 —— 大致和 ui_button 的
 * 0.7 拉开一档,又不至于一屏只放得下两条。
 * 行高、栏高、折行宽度全部由这两个数实测推导,改了不用动别处。 */
/* 正文用全局清晰档。曾经写 0.6f 想让正文比用户名那行大一点,结果
 * 掉出了 eff_scale 的吸附窗口 —— 大是大了,但整段正文都发虚,
 * 反倒是 0.52 的用户名行清清楚楚。清晰和「比别处大」只能选一个。 */
#define CM_FONT  UI_SHARP  /* 正文 */
#define CM_META  UI_SHARP  /* 用户名/赞数那一行 */

/* 发布时间。近的用相对时间(和网页端/App 一致,窄栏里也更好读),
 * 超过 30 天就写日期 —— 那时候"多少天前"已经没人在心里换算了。
 * 时区:3DS 的 time() 走的是主机本地时钟,localtime 在没设 TZ 时等同于
 * 直接把它当本地时间用,正好对上。ctime 是 UTC 秒,两边基准差一个时区
 * 偏移 —— 对"几分钟前"这种量级看不出来,而超过 30 天的一律走日期,
 * 也就顶多在跨日的那一刻差一天。为这点误差去引一套时区表不值得。 */
static void fmt_ctime(int64_t t, char *out, size_t n) {
	if (t <= 0) { out[0] = 0; return; }
	time_t now = time(NULL);
	int64_t d = (int64_t)now - t;
	if (d < 0) d = 0;                    /* 机器时钟慢了,别显示"负几分钟前" */
	if (d < 60)              snprintf(out, n, "刚刚");
	else if (d < 3600)       snprintf(out, n, "%d分钟前", (int)(d / 60));
	else if (d < 86400)      snprintf(out, n, "%d小时前", (int)(d / 3600));
	else if (d < 30 * 86400) snprintf(out, n, "%d天前", (int)(d / 86400));
	else {
		time_t tt = (time_t)t;
		struct tm tm;
		if (localtime_r(&tt, &tm))
			snprintf(out, n, "%04d-%02d-%02d",
			         tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
		else out[0] = 0;
	}
}
#define CM_TIP   UI_SHARP  /* 底部操作提示,和设置页等处同一档 */
/* 行高**不再硬编码** —— 位图字体的行高由字体和 mkbcfnt 的 -s 决定,
 * 猜一个常数换个字体就叠字(实测踩过)。每帧向字体问一次,
 * 一次字形解析而已,可以忽略 */
static float s_lh = 17.0f;   /* comment_draw 开头刷新 */
/* 顶栏/底栏/按钮的高度**同样由字体决定**,不能写死。
 * 行高变了而容器没变,就是「字叠到栏上、说明跑出屏幕」的成因 —— 
 * 布局里凡是要装文字的盒子,尺寸都得从文字反推 */
static float s_head_h = 26.0f, s_foot_h = 28.0f;

typedef struct {
	BiliComment c;
	/* 折行结果:每行在 text 里的起始字节偏移(brk[k]..brk[k+1] 是第 k 行),
	 * nlines<0 表示还没算过。
	 * **必须在主线程算** —— ui_text_width 会解析字形,用的是共享缓冲,
	 * 后台线程碰它会和渲染打架 */
	short brk[CM_LINES + 1];
	short nlines;
} CmItem;

/* 【不设条数上限】评论区是无限滚动:滚到底自动续下一页。
 *
 * 存储用**分块**而不是一整块数组:每页 20 条单独 calloc 一块,块地址一旦
 * 定下就再也不动。这样后台线程可以一边填新块、主线程一边读老块,全程免锁。
 * 换成 realloc 就不行了 —— 它会把主线程正在读的元素搬走。
 * 真正的上限是内存本身:分不到新块就停在那儿,并如实告诉用户。
 * CM_BLOCKS 只是指针表的长度(512 块 = 一万条),先撞到的一定是内存。 */
#define CM_BLOCKS 512
static CmItem *s_blk[CM_BLOCKS];
static short   s_blk_n[CM_BLOCKS];      /* 每块实际条数(只有最后一块可能不满) */
static volatile int s_nblk = 0;         /* 已发布的块数 */
static volatile int s_n = 0;            /* 已发布的总条数(只用于显示) */
static volatile int s_more = 1;         /* 服务端还有没有更多 */
static volatile int s_loading = 0;
static Thread s_thread = NULL;
static volatile uint32_t s_gen = 0;
static int s_page = 1;
static float s_scroll = 0.0f;      /* 像素偏移 */
static float s_content_h = 0.0f;   /* 上一帧算出的内容总高(夹取滚动用) */
static char s_err[64] = {0};

typedef struct { uint32_t gen; int64_t aid; int page; } CmReq;
static CmReq s_req;

int  comment_count(void) { return s_n; }
bool comment_loading(void) { return s_loading != 0; }
int  comment_page(void) { return s_page; }

/* 把控制字符换成空格。
 *
 * 【为什么必须做】评论正文常带换行(`\n`),而这两件事凑在一起会出错:
 *   - ui_text_width() 对带换行的串返回的是**最长那一行**的宽度
 *     → wrap() 以为一行放得下,不折
 *   - C2D_DrawText 遇到 `\n` 会**真的换行**
 *     → 一个"逻辑行"画成两三行,超出这条评论分配的高度,压到下一条上
 * 实测现象就是「有的评论和别的叠在一起」。
 *
 * 在数据入口处清一次最省事:后面所有量宽、折行、绘制都不必再操心。
 * 评论区只显示三行摘要,把换行压成空格不影响阅读。 */
static void flatten(char *s) {
	if (!s) return;
	for (; *s; s++)
		if ((unsigned char)*s < 0x20 || *s == 0x7F) *s = ' ';
}

static void cm_thread(void *arg) {
	CmReq req = *(const CmReq *)arg;
	BiliComment *tmp = (BiliComment *)calloc(CM_MAX, sizeof(BiliComment));
	int got = 0;
	/* 【「请求失败」和「没有评论」必须分开】两者都是 got==0,但对用户
	 * 是完全不同的两件事:一个该重试,一个什么都不用做。
	 * 原来合并成一句「没有评论 / 加载失败」,等于两边都没说清。 */
	bool req_failed = false;
	if (!tmp || bili_comments(req.aid, req.page, tmp, CM_MAX, &got) != 0) {
		got = 0;
		req_failed = true;
	}

	__dmb();
	if (req.gen != s_gen) {           /* 陈旧结果:丢掉,也不许清 loading */
		free(tmp);
		return;
	}

	/* 【新块,不动老块】老块的地址主线程可能正在读,绝不能碰。
	 * 新块填好之后再把块数推上去,主线程看到的永远是完整的块。 */
	int bi = s_nblk;
	int added = 0;
	if (tmp && got > 0 && bi < CM_BLOCKS) {
		CmItem *blk = (CmItem *)calloc((size_t)got, sizeof(CmItem));
		if (blk) {
			for (int i = 0; i < got; i++) {
				blk[i].c = tmp[i];
				flatten(blk[i].c.text);   /* 换行会撑破行高,见 flatten */
				flatten(blk[i].c.user);
				blk[i].nlines = -1;       /* 折行留给主线程算 */
			}
			s_blk[bi] = blk;
			s_blk_n[bi] = (short)got;
			added = got;
			__dmb();
			s_nblk = bi + 1;              /* 发布:这一步之后主线程才会读它 */
			s_n += got;
		} else {
			snprintf(s_err, sizeof(s_err), "内存不足,评论只能到这里");
			s_more = 0;
		}
	}
	free(tmp);

	/* 一页 20 条;不足 20 说明到底了。块表满了也不再拉 */
	if (added > 0 && s_err[0] && s_n > 0) s_err[0] = 0;
	if (got < CM_MAX || bi + 1 >= CM_BLOCKS) s_more = 0;
	if (s_n == 0) {
		/* 空评论区是常态(冷门视频、刚发布的视频),不该像出错一样冷冰冰 */
		if (req_failed)
			snprintf(s_err, sizeof(s_err), "评论没加载出来,B 退出再进来试试");
		else
			snprintf(s_err, sizeof(s_err), "还没有人评论  ( ´ ▽ ` )ﾉ");
	}
	__dmb();
	s_loading = 0;
}

bool comment_load_async(int64_t aid, int page) {
	if (page < 1) page = 1;
	/* 和字幕同样的教训:代际必须先推,再决定要不要返回。
	 * 直接 return 而不推代际的话,在途线程回来时代际还相符,
	 * 会把上一次(甚至上一个视频)的结果发布出来 */
	if (page == 1) s_gen++;
	__dmb();
	if (s_loading) return false;

	if (page == 1) {
		comment_free();               /* 换视频/重新打开:清空重来 */
		s_gen++;
		__dmb();
		s_scroll = 0.0f;
		s_more = 1;
		/* 【别拿 s_err 当加载占位】绘制那边已经按 s_loading 单独判了。
		 * 塞进来的话,「加载完但一条都没有」时 s_err 非空,
		 * 下面那句设「还没有人评论」的条件(!s_err[0])永远不成立 ——
		 * 于是空评论区**永远显示「加载中…」**。 */
		s_err[0] = 0;
	} else if (s_thread) {
		/* 续页:上一批的线程句柄还留着,先回收再建新的 */
		threadJoin(s_thread, 8000000000ULL);
		threadFree(s_thread);
		s_thread = NULL;
	}
	s_req.gen = s_gen;
	s_req.aid = aid;
	s_req.page = page;
	s_page = page;
	__dmb();
	s_loading = 1;
	static const int cores[] = { 3, 2, -2 };   /* 核心 1 是系统核,别碰 */
	for (int i = 0; i < 3 && !s_thread; i++)
		s_thread = threadCreate(cm_thread, &s_req, 32 * 1024, 0x38,
		                        cores[i], false);
	if (!s_thread) { s_loading = 0; return false; }
	return true;
}

void comment_free(void) {
	s_gen++;
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
		u64 ns = net_is_shutting_down() ? 500000000ULL : 8000000000ULL;
		if (R_FAILED(threadJoin(s_thread, ns))) {
			printf("comment thread join timeout\n");
			threadDetach(s_thread);
		} else {
			threadFree(s_thread);
		}
		s_thread = NULL;
	}
	s_loading = 0;
	s_n = 0;
	s_more = 1;
	s_page = 1;
	__dmb();
	for (int i = 0; i < CM_BLOCKS; i++) {
		free(s_blk[i]);
		s_blk[i] = NULL;
		s_blk_n[i] = 0;
	}
	s_nblk = 0;
}

/* 按屏宽折行(UTF-8 边界),结果缓存进 item。只在主线程调 */
static void wrap(CmItem *it) {
	if (it->nlines >= 0) return;
	const char *t = it->c.text;
	size_t i = 0;
	int ln = 0;
	it->brk[0] = 0;
	/* 【逐字累加宽度,而不是每加一个字重量一次整行】
	 * 原来的写法每前进一个字符就把「行首到这里」的整段重新量一遍 ——
	 * 单行 O(n²)。正文 512 字节时还看不出来(一行十几个字),
	 * 提到 1536 之后一屏 20 条要量上万次,全压在**加载完的第一帧**里
	 * (布局要算高度,所有条目都得先折行),那一帧就是肉眼可见的一顿。
	 *
	 * 位图字体没有 kerning,整串宽度就是各字形 advance 之和,所以累加
	 * 和整体测量等价。真有出入也只是某一行早/晚断一个字,不会错行。 */
	while (t[i] && ln < CM_LINES) {
		size_t start = i, last_fit = i;
		float w = 0.0f;
		while (t[i]) {
			size_t cl = 1;
			unsigned char ch = (unsigned char)t[i];
			if (ch >= 0xF0) cl = 4;
			else if (ch >= 0xE0) cl = 3;
			else if (ch >= 0xC0) cl = 2;
			char one[5];
			memcpy(one, t + i, cl);
			one[cl] = 0;
			float cw = ui_text_width(one, CM_FONT);
			if (w + cw > CM_W) break;
			w += cw;
			i += cl;
			last_fit = i;
		}
		if (last_fit == start) {         /* 一个字都放不下:硬推一个,防死循环 */
			last_fit = start + 1;
			i = last_fit;
		}
		ln++;
		it->brk[ln] = (short)last_fit;
		if (!t[last_fit]) break;
		i = last_fit;
	}
	it->nlines = (short)ln;
}

/* 续拉下一页(滚到底时自动调)。加载中/已到底则忽略 */
static void comment_load_more(void) {
	if (s_loading || !s_more || !s_req.aid) return;
	comment_load_async(s_req.aid, s_page + 1);
}

bool comment_draw(bool touch_down, bool touch_held, float tx, float ty) {
	static bool dragging = false;
	static float drag_y0 = 0, drag_s0 = 0;
	bool want_close = false;
	(void)tx;                  /* 下屏已无按钮,横坐标用不上了 */

	/* 拖动滚动。下屏不再有按钮,所以不用区分"点按"和"拖动" ——
	 * 触摸一律当拖动处理(原来的 tap/moved 判定是给按钮点击用的) */
	if (touch_down) {
		dragging = true;
		drag_y0 = ty;
		drag_s0 = s_scroll;
	}
	if (dragging && touch_held)
		s_scroll = drag_s0 - (ty - drag_y0);   /* 手指上滑 = 内容上移 */
	if (!touch_held) dragging = false;

	/* 先量字体、定尺寸,再夹取滚动 —— 反过来的话用的是上一帧的旧尺寸 */
	/* 全部取整:行高一旦是小数,第 n 行的 y 就是 n 倍的小数偏移,
	 * 越往下越偏离像素格(ui_text_z 会兜底取整,但那样行距会忽大忽小) */
	/* 字号变了,之前按旧字号算的折行断点就作废了 —— 不重算的话每行变宽
	 * 却还按老断点画,字会挤在一起。设置页调字号时实测到过 */
	{
		static uint32_t seen_gen = 0xFFFFFFFFu;
		uint32_t g = ui_font_gen();
		if (g != seen_gen) {
			seen_gen = g;
			for (int b = 0; b < s_nblk; b++)
				for (int k = 0; k < s_blk_n[b]; k++)
					s_blk[b][k].nlines = -1;
		}
	}
	s_lh = (float)(int)(ui_text_height(CM_FONT) + 1.5f);
	/* 底栏只剩一行操作提示。
	 * 原来还有「上一页/关闭评论/下一页」三个按钮,那一行 26px 正好横在
	 * 最后一条评论和提示之间 —— 「最后一行离提示很远」就是它撑的。
	 * 翻页移到 L/R 键,关闭本来 B 就能做,按钮整行去掉,正文区多出近 30px。 */
	float tip_h = (float)(int)(ui_text_height(CM_TIP) + 0.5f);
	s_foot_h = tip_h + 8.0f;
	s_head_h = s_lh + 6.0f;

	/* 滚动上限。
	 * s_content_h 是所有条目"盒子"的总高,而每个盒子尾部含 6px 分隔留白 ——
	 * 直接用 内容高-可视高 当上限,滚到底时最后一条的**盒子**底边贴住底栏,
	 * 于是**文字**底边离底栏还差那 6px,看着就是一段空白。
	 * 这里把尾部留白扣掉(留 2px 透气),最后一行就贴着提示行了。
	 *
	 * 注:正文 z=0.5、底栏 z=0.6,正文是从底栏**底下**滑过去的 ——
	 * 所以能滚多远只由这个上限决定,和遮挡无关。 */
	float maxs = s_content_h - 4.0f - (240.0f - s_head_h - s_foot_h);
	if (maxs < 0) maxs = 0;
	/* 无限滚动:快到底了就先把下一页拉回来,别等用户撞到底再等网络。
	 * 一屏的余量足够在正常滑动速度下无感衔接 */
	if (s_scroll > maxs - (240.0f - s_head_h - s_foot_h))
		comment_load_more();
	if (s_scroll < 0) s_scroll = 0;
	if (s_scroll > maxs) s_scroll = maxs;

	ui_begin_bottom();

	/* 正文区(先画,顶栏/底栏后画且 z 更高,盖住滚过界的文字) */
	float y = s_head_h - s_scroll;
	float total = 0.0f;      /* 纯内容高,不含任何起始偏移 */
	if (s_nblk > 0) {
		/* 两层循环:外层走块,内层走块内条目。块地址不会变,所以
		 * 一边有后台线程在填新块也不影响这里 */
		for (int bi = 0; bi < s_nblk; bi++) {
		for (int ci = 0; ci < s_blk_n[bi]; ci++) {
			CmItem *it = &s_blk[bi][ci];
			wrap(it);
			/* 一条 = 元信息 1 行 + 正文 n 行 + 6px 间隔 */
			float h = s_lh * ((it->nlines > 0 ? it->nlines : 1) + 1) + 6.0f;
			/* 只画落在可视区内的,滚动时才不会白算一屏外的字 */
			if (y + h > s_head_h - s_lh && y < 240.0f) {
				char meta[72], when[32];
				snprintf(meta, sizeof(meta), "%s  %d赞", it->c.user, it->c.like);
				fmt_ctime(it->c.ctime, when, sizeof(when));
				/* 时间右对齐,和用户名同色同字号:它是同一行的次要信息,
				 * 换个颜色只会让这一行看起来有两种东西。
				 * 左边那截要按剩余宽度裁,否则长用户名会顶到时间上。 */
				float tw = when[0] ? ui_text_width(when, CM_META) : 0.0f;
				ui_text_clipped_z(8, y, 0.5f, CM_META, UI_COL_ACCENT, meta,
				                  CM_W - (tw > 0.0f ? tw + 8.0f : 0.0f));
				if (when[0])
					ui_text_z(8 + CM_W - tw, y, 0.5f, CM_META,
					          UI_COL_ACCENT, when);
				for (int k = 0; k < it->nlines; k++) {
					char line[sizeof(it->c.text)];
					int a = it->brk[k], b = it->brk[k + 1];
					int len = b - a;
					if (len < 0 || len >= (int)sizeof(line)) break;
					memcpy(line, it->c.text + a, (size_t)len);
					line[len] = 0;
					ui_text_z(8, y + s_lh + k * s_lh, 0.5f, CM_FONT,
					          UI_COL_TEXT, line);
				}
				/* 分隔线 */
				ui_rect_z(8, y + h - 5.0f, 0.45f, 300, 1,
				          C2D_Color32(0x33, 0x33, 0x3E, 0xFF));
			}
			y += h;
			total += h;
		}
		}
	} else {
		ui_text_z(12, 100, 0.5f, CM_FONT, UI_COL_DIM,
		          s_loading ? "加载中…" : (s_err[0] ? s_err : "暂无评论"));
	}
	/* 列表末尾的状态行:正在续拉 / 已到底。也让滚动多出一行的余量,
	 * 用户能看到"还有没有" */
	if (s_nblk > 0) {
		const char *tail = s_loading ? "加载中…"
		                 : (s_more ? "" : "— 没有更多了 —");
		if (tail[0]) {
			float tw = ui_text_width(tail, CM_META);
			ui_text_z((320.0f - tw) / 2.0f, y + 2.0f, 0.5f, CM_META,
			          UI_COL_DIM, tail);
			total += s_lh + 4.0f;
		}
	}
	s_content_h = total;

	/* 顶栏(高度随字体) */
	ui_rect_z(0, 0, 0.6f, 320, s_head_h, UI_COL_ACCENT);
	char hdr[48];
	snprintf(hdr, sizeof(hdr), "评论  %d 条%s", s_n,
	         s_loading ? "  加载中…" : (s_more ? "" : "  已全部加载"));
	ui_text_z(8, (s_head_h - s_lh) / 2.0f, 0.7f, CM_FONT, UI_COL_WHITE, hdr);

	/* 底栏:一行操作提示,和设置页/列表页同一风格 */
	ui_rect_z(0, 240 - s_foot_h, 0.6f, 320, s_foot_h,
	          C2D_Color32(0x1A, 0x1A, 0x22, 0xFF));
	ui_text_z(8, 240.0f - s_foot_h + 4.0f, 0.7f, CM_TIP, UI_COL_DIM,
	          "B 返回  /  拖动滚屏,到底自动加载");

	return want_close;
}
