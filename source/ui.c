#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include "ui.h"
#include "thread_util.h"
#undef printf      /* ui.c 内部要用真 printf 写控制台,不能再绕回 ui_printf
                    * (否则重放历史会把历史再写一遍进环形缓冲) */
#include "vendor/qrcodegen.h"

static C3D_RenderTarget *s_top;
static C3D_RenderTarget *s_top_r;   /* 右眼(裸眼 3D) */
static C3D_RenderTarget *s_bot;
static C2D_Font s_font;          /* NULL = 系统默认字体 */
/* 字形纹理采样。自带的 Noto 和系统字体都是**轮廓字体**(抗锯齿),
 * 本来就是给缩放用的,LINEAR 正确。
 * NEAREST 只在「1.0 倍绘制的点阵字体」下才有意义,而点阵那条路已经验证
 * 走不通(见下面 UI_USE_ROMFS_FONT 的说明),不要再改。 */
#define UI_FONT_FILTER GPU_LINEAR
/* 1 = 用 romfs:/font.bcfnt(Noto Sans CJK 子集);0 = 用 3DS 中文系统字体。
 * 用自带字体的唯一理由:**系统字体缺字**,非国行机尤其严重,会显示成"?"。
 * 文件不存在时自动回退,不会崩。 */
#define UI_USE_ROMFS_FONT 1
/* ---------- 字号标定 ----------
 *
 * 屏幕上的字号 = 各页面的字号常量 x s_font_k x 字体行高
 *
 * 前两项是定的(页面常量早就调好了),所以**换字体时唯一要保住的量是
 * 「s_font_k x 行高」这个乘积** —— 保住它,屏幕上大小分毫不变;
 * 而行高越大(mkbcfnt -s 转得越大),同样的显示尺寸下纹理细节越多,越锐利。
 *
 * 这就是 UI_FONT_TARGET:它是那个乘积,实机调出来的。
 * 于是 s_font_k 由**开机实测行高**反算,换 -s 不用改任何代码。
 *
 * 【要更锐利就把字体转大,不要动这个数】
 * 动它只会改变大小;发虚是纹理不够大造成的,只能靠加大 -s 解决。
 *
 * 【TARGET 不能跨字体沿用】它以**行高**为基准,而"汉字占行高的比例"
 * 因字体而异:混着日韩字形的字体行高虚高,同一个 TARGET 换到纯中文
 * 子集上就整体放大(实测放大 33%,还因为拉伸而发虚)。
 * **每换一次字体(哪怕只是重新裁剪)都要用设置页的旋钮重新标定。**
 * 定稿方式:设置页「字号 -/+」调顺眼,把底下显示的 TARGET 抄回这里。 */
#define UI_FONT_TARGET 33.3f   /* 新字体实机标定:0.52 档正好落在原生 1:1 */
/* 万一行高量不出来时的兜底倍率 */
#define UI_FONT_K 0.85f
static float s_font_k = 1.0f;   /* 实际生效值,ui_init 里按加载结果设定 */

/* 字号吸附。实机两次独立标定得到同一个结论:清晰点 = 字形原生 1:1。
 * (TARGET 24.7 时按钮 0.70 档最清晰、33.3 时正文 0.52 档最清晰,
 *  两者的有效倍率都 ≈0.96 —— 都在逼近 1:1。)
 *
 * 【必须返回精确 1.0,不是"吸附到 0.52 再乘 k"】
 * 0.52xk 只是 ≈1.0(实测 0.96~1.0007 之间飘)。首字符有 floor 取整兜底,
 * 但**串内后续字符的位置 = 起点 + 字距x有效倍率** —— 倍率和 1 差
 * 千分之几,小数偏移也会逐字累积,越往后越糊。列表副标(带数字,字多)
 * 就是这么漏网的。返回精确 1.0 后字距是整数,每个字都落在整像素上。
 *
 * 【窗口必须相对 s_font_k,不能写死区间】
 * 上一版无条件返回 1.0,在自带字体下(k≈1.92)碰巧接近正确,但回退到
 * 系统字体时(k=1.0)连 0.44 的小字也被拉成 1.0 倍 —— 满屏字都变大。
 * 正确做法:吸附到「最接近 1.0 的那个档」,即 1/k 附近的常量;
 * 有效倍率取精确 1.0(整像素字距),但只在原始字号确实落在那一档时。 */
static float eff_scale(float s) {
	float snap = (s_font_k > 0.01f) ? (1.0f / s_font_k) : 1.0f;
	/* 窗口 = 该档 ±22%:常用字号吸进来,弹幕小/大档和大标题留在窗外 */
	if (s > snap * 0.78f && s < snap * 1.22f) return 1.0f;
	return s * s_font_k;
}
static float s_font_lh = 0.0f;  /* 开机实测的字体行高(反算 TARGET 用) */
/* 字号变更的代次。折行/截断这类**按像素宽度算出来的缓存**必须跟着它作废,
 * 否则字号变了还按旧断点画,就会挤在一起(评论区实测踩过) */
static uint32_t s_font_gen = 0;
/* 绘制用字形缓冲:每帧 ui_begin 清一次,本帧所有要画的字都从这里分配 */
static C2D_TextBuf s_textbuf;
/* 量宽专用缓冲:测量也要 TextFontParse,也要占字形槽,但测完就没用了。
 * 以前和绘制共用一个缓冲 → 测量把槽吃光 → 后面真要画的字 parse 失败。
 * 这是"弹幕一多画面就变形"的根源(详见 ui_text_z 里的说明)。
 * 单独开一个小的,测完立刻清,永远不会涨。 */
static C2D_TextBuf s_measbuf;
static bool s_console_mode = false; /* 默认 GUI,秘技/设置进调试台 */
static float s_scr_w = 400.0f;      /* 当前场景屏宽(裁剪换算用) */

/* ---------- 日志环形缓冲 ----------
 * 调试台是按需打开的,打开前的日志(取流失败原因、字幕轨数量…)
 * 才是最需要看的。所以全工程统一走 ui_printf:先进环形缓冲,
 * 打开调试台时把历史重放出来。多线程会调,加锁。 */
/* 400 不是 160:启动阶段的日志(指纹注册、cookie 名单、取流失败原因)
 * 恰恰最关键,而它们最早被冲掉 —— 排查时反复出现「那行应该打了但没看到」,
 * 其实是滚没了。400 行 x 60 字节 = 24KB,买得起 */
#define LOG_LINES 400
/* 换行宽度按**屏幕能显示多少**来定,不是按缓冲区多大。
 * 早先存 110 字节一行,而下屏只画得下约一半 —— 超出部分被
 * ui_text_clipped 截成「...」,右半截根本看不见。排查那几轮里反复出现的
 * 「这行应该打了却没看到」,有一部分就是这么来的。
 *
 * 后来改成写死 54,换成像素字体后又不对了(每字更宽)。所以现在**实测**:
 * 每帧量一次 ASCII 字宽,反算一行能放多少字节。常数在这里永远会过期。 */
#define LOG_COLS  72   /* 单行存储上限(含多字节溢出余量 + 结尾 0) */
static int s_log_wrap = 54;   /* 首帧前的初值,之后按实测更新 */
/* 日志页字号。这一个数控制整页:行高、一屏行数、折行长度都由它推导。
 * 跟全局清晰档走 —— 日志本来就是最需要看清的地方,而且行数、折行长度
 * 都是按实测行高/字宽算的,字变大只是每屏少几行,不会错位。 */
#define LOG_FONT UI_SHARP
static char s_log[LOG_LINES][LOG_COLS];
static int  s_log_head = 0, s_log_n = 0;
/* 一屏行数在绘制时按实测行高算,这里只给个上限 */
#define LOG_VIEW_MAX 16
static int s_log_view = 11;           /* 实际值每帧更新(滚动夹取要用) */
static int  s_log_scroll = 0;
static char s_log_cur[LOG_COLS];      /* 尚未遇到 \n 的半行 */
static int  s_log_curlen = 0;
static LightLock s_log_lock;
static bool s_log_ready = false;

static void log_push(const char *line) {
	snprintf(s_log[s_log_head], LOG_COLS, "%s", line);
	s_log_head = (s_log_head + 1) % LOG_LINES;
	if (s_log_n < LOG_LINES) s_log_n++;
}

void ui_printf(const char *fmt, ...) {
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (!s_log_ready) return;
	LightLock_Lock(&s_log_lock);
	for (const char *p = buf; *p; p++) {
		/* 只在 UTF-8 字符边界处折行:从半个汉字中间断开会画出乱码方块。
		 * 代价是一行最多溢出 3 字节,LOG_COLS 已经留好余量 */
		bool cont = ((unsigned char)*p & 0xC0) == 0x80;
		if (*p == '\n' || (s_log_curlen >= s_log_wrap && !cont)) {
			s_log_cur[s_log_curlen] = 0;
			log_push(s_log_cur);
			s_log_curlen = 0;
			if (*p != '\n') s_log_cur[s_log_curlen++] = *p;
		} else if (*p != '\r' && s_log_curlen < LOG_COLS - 1) {
			s_log_cur[s_log_curlen++] = *p;
		}
	}
	LightLock_Unlock(&s_log_lock);
}

/* ---------- 里程碑落盘(经由后台线程) ----------
 *
 * 【主线程绝不直接写 SD】ui_trace 曾经在调用线程里直接 fopen/fclose,
 * 结果 CIA 下播放启动阶段主线程就卡死在这次写入里 —— 整机跟着锁死。
 * trace 两次卡死点之间只有纯算术,凶手就是这次 SD 写入本身。
 * 改为:调用方只把行推进内存队列(拿一把轻锁,微秒级),
 * 由低优先级线程负责真正写盘。SD 层再怎么挂,也只挂那个线程。 */
#define TQ_N 32
static char s_tq[TQ_N][192];
static volatile int s_tq_head = 0, s_tq_tail = 0;
static LightLock s_tq_lock;
static Thread s_tq_thread = NULL;
static volatile int s_tq_quit = 0;

static void trace_writer_main(void *arg) {
	(void)arg;
	while (!s_tq_quit || s_tq_head != s_tq_tail) {
		while (s_tq_head != s_tq_tail) {
			FILE *f = fopen("sdmc:/3ds/3danmu/trace.log", "a");
			if (f) {
				fseek(f, 0, SEEK_END);
				if (ftell(f) > 64 * 1024) {   /* 别让它无限长 */
					fclose(f);
					f = fopen("sdmc:/3ds/3danmu/trace.log", "w");
				}
			}
			if (!f) break;                    /* SD 不可用:本轮放弃,行留在队里 */
			fprintf(f, "%s\n", s_tq[s_tq_tail]);
			fclose(f);                        /* 每行落盘:死机前写到哪算哪 */
			s_tq_tail = (s_tq_tail + 1) % TQ_N;
		}
		svcSleepThread(50 * 1000 * 1000LL);
	}
}

/* 【崩溃前那几行日志的专用通道】
 * 普通 ui_trace 是「丢进队列,后台线程 50ms 后落盘」——整机死机或系统模块
 * svcBreak 时,机器在 50ms 内就没了,最关键的最后几行永远写不出去。
 * (实测:mvd 崩了两次,trace.log 里连 mvd 这一段都没有。)
 *
 * 所以给硬解初始化这几个里程碑单开一条同步通道:当场 fopen/fclose 落盘。
 * **只能用在低频位置** —— 当初把 ui_trace 整个做成同步的,逐帧调用直接
 * 把主线程钉死在 SD 卡上,表现是播放一开就卡死。每个视频三五次没问题,
 * 放进解码循环就会重蹈覆辙。 */
void ui_trace_sync(const char *fmt, ...) {
	char buf[160];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	ui_printf("%s\n", buf);
	FILE *f = fopen("sdmc:/3ds/3danmu/trace.log", "a");
	if (!f) return;
	fprintf(f, "[%llu] %s\n", (unsigned long long)osGetTime(), buf);
	fclose(f);
}

void ui_trace(const char *fmt, ...) {
	char buf[160];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	ui_printf("%s\n", buf);                  /* 调试台也留一份 */
	LightLock_Lock(&s_tq_lock);
	int next = (s_tq_head + 1) % TQ_N;
	if (next != s_tq_tail) {                  /* 队满就丢最新的,绝不阻塞 */
		snprintf(s_tq[s_tq_head], sizeof(s_tq[0]), "[%llu] %s",
		         (unsigned long long)osGetTime(), buf);
		s_tq_head = next;
	}
	LightLock_Unlock(&s_tq_lock);
}

void ui_init(void) {
	LightLock_Init(&s_log_lock);
	LightLock_Init(&s_tq_lock);
	s_log_ready = true;
	/* trace 写盘线程:低优先级,任意可用核 */
	s_tq_thread = threadCreate(trace_writer_main, NULL, 16 * 1024, 0x3C, -2, false);
	gfxInitDefault();
	/* 注意:全程不 consoleInit——它会把下屏改成 RGB565 单缓冲,
	 * 与 GUI 的 BGR8 双缓冲打架(退出后主页闪烁)。日志页改为自绘。 */

	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
	C2D_Prepare();
	s_top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	s_top_r = C2D_CreateScreenTarget(GFX_TOP, GFX_RIGHT);
	s_bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
	/* 预算:弹幕单帧上限 80 条 x 十几个字,3D 模式双眼各画一遍,
	 * 再加界面文字 —— 6144 足够,极端情况(全是长弹幕)也只是
	 * 少画几条(ui_text_z 有 NULL 保护),不会再变成画面损坏。
	 * 测量已改用独立缓冲,不再挤占这里;每字形约 40 字节,
	 * 从 8192 降到 6144 省下约 80KB */
	s_textbuf = C2D_TextBufNew(6144);
	s_measbuf = C2D_TextBufNew(512);

	/* 字体优先级:romfs 自带 bcfnt > 中文系统字体 > 台版 > 默认(日版,覆盖大部分常用汉字) */
	{
		/* romfs 挂载结果落盘。CIA 满屏"?"时先看这行:
		 * 挂载失败 = CIA 里根本没打进 romfs(makerom 的 RomFs 配置问题);
		 * 挂载成功而字体仍加载失败 = 文件本身的问题。两种修法完全不同 */
		Result rr = romfsInit();
		ui_trace("romfs mount %08lx", (unsigned long)rr);
	}
	/* romfs 里的是 **Noto Sans CJK SC 子集**(轮廓字体),不是点阵字体。
	 * 点阵那条路试过,不通:12px 点阵转出来汉字只有 11 像素高,复杂字
	 * (赢、蘸)在纹理里就已经退化成点阵 —— 不是绘制问题,是物理上限。
	 * 详见 comment.c 里 CM_FONT 上面那段和 tools/fontdump.py。
	 * 生成方式见 README「中文字体」一节(mkbcfnt -s 9 + bcfnt_fix.py)。 */
#if UI_USE_ROMFS_FONT
	s_font = C2D_FontLoad("romfs:/font.bcfnt");
	/* 落盘:字体没加载上的话满屏"?",而原因(文件缺失/内存不够)只有这里知道 */
	ui_trace("font: romfs %s", s_font ? "loaded" : "LOAD FAILED");
	if (!s_font) {
		/* C2D_FontLoad 失败不给原因,手动探一下把三种情况分开:
		 *   fopen 失败        → romfs 镜像里没有这个文件(打包路径问题)
		 *   size/magic 不对   → 文件进去了但内容坏了
		 *   都正常            → C2D_FontLoad 内部分配失败(那才是内存问题) */
		FILE *ff = fopen("romfs:/font.bcfnt", "rb");
		if (!ff) {
			ui_trace("font probe: fopen FAILED (file not in romfs)");
		} else {
			unsigned char m4[4] = {0};
			fseek(ff, 0, SEEK_END);
			long sz = ftell(ff);
			fseek(ff, 0, SEEK_SET);
			fread(m4, 1, 4, ff);
			fclose(ff);
			ui_trace("font probe: size=%ldKB magic=%c%c%c%c linear=%luKB",
			         sz / 1024, m4[0], m4[1], m4[2], m4[3],
			         (unsigned long)(linearSpaceFree() / 1024));
		}
		printf("romfs font missing, fall back to system font\n");
	}
#endif
	if (!s_font) s_font = C2D_FontLoadSystem(CFG_REGION_CHN);
	if (!s_font) s_font = C2D_FontLoadSystem(CFG_REGION_TWN);
	/* s_font == NULL 时 C2D_TextFontParse 会用共享系统字体 */

	/* 抗锯齿字体 + 非整数倍绘制 → LINEAR。理由见 UI_FONT_FILTER 处。 */
	if (s_font)
		C2D_FontSetFilter(s_font, UI_FONT_FILTER, UI_FONT_FILTER);

#if UI_USE_ROMFS_FONT
	/* 自带字体:按实测行高反算倍率(见 UI_FONT_TARGET)。
	 * 回退到系统字体时不做这一步 —— 各页面的字号常量本来就是按它调的,
	 * s_font_k 保持 1.0 才是对的。 */
	if (s_font) {
		float lh = ui_text_height(1.0f);      /* 此时 s_font_k 还是 1.0 */
		s_font_lh = lh;
		s_font_k = (lh > 1.0f) ? (UI_FONT_TARGET / lh) : UI_FONT_K;
		/* 换字体后先看这行:行高变大而 k 相应变小 = 对了(同样大小,更锐利) */
		/* 落进 trace.log:1/k 就是唯一那个清晰档,UI_SHARP 必须贴着它。
		 * 换字体后先看这行,别再靠猜哪个字号锐利。 */
		ui_trace("font: lineheight=%d k=%d/100 sharp=%d/100 (UI_SHARP=%d/100)",
		         (int)(lh + 0.5f), (int)(s_font_k * 100.0f + 0.5f),
		         (int)(100.0f / s_font_k + 0.5f), (int)(UI_SHARP * 100.0f + 0.5f));
		/* 【点对点自检】UI_SHARP 必须被 eff_scale 吸附成精确 1.0,否则
		 * 全工程的界面文字都在做非整数倍缩放 —— 那是「整屏发虚」而不是
		 * 「这一处发虚」,肉眼很难归因,但这里一行就能判死。
		 * 换字体/换 mkbcfnt 的 -s 之后先看这行:出现 NOT 1:1 就说明
		 * UI_SHARP 掉出了吸附窗口,照上一行的 sharp=xx/100 改回去。 */
		if (eff_scale(UI_SHARP) != 1.0f)
			ui_trace("font: WARNING UI_SHARP NOT 1:1 (eff=%d/100) "
			         "-> set UI_SHARP=%d/100",
			         (int)(eff_scale(UI_SHARP) * 100.0f + 0.5f),
			         (int)(100.0f / s_font_k + 0.5f));
		else
			ui_trace("font: UI_SHARP is 1:1 (point-to-point)");
	}
#endif
}

void ui_exit(void) {
	if (s_tq_thread) {                 /* 先让写盘线程把队列清完 */
		s_tq_quit = 1;
		thread_reap(&s_tq_thread, 3000000000LL, "trace writer");
	}
	if (s_font) C2D_FontFree(s_font);
	C2D_TextBufDelete(s_measbuf);
	C2D_TextBufDelete(s_textbuf);
	C2D_Fini();
	C3D_Fini();
	romfsExit();
	gfxExit();
}

void ui_begin(void) {
	C2D_TextBufClear(s_textbuf);
	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
	C2D_TargetClear(s_top, UI_COL_BG);
	C2D_SceneBegin(s_top);
	s_scr_w = 400.0f;
}

void ui_end(void) {
	C3D_FrameEnd(0);
}

/* 裸眼 3D:开/关上屏立体模式(2DS 上调用无害,只是没效果) */
void ui_set_3d(bool on) {
	gfxSet3D(on);
}

/* 切到右眼目标(须在 ui_begin 之后、ui_begin_bottom 之前) */
void ui_begin_top_right(void) {
	C2D_TargetClear(s_top_r, UI_COL_BG);
	C2D_SceneBegin(s_top_r);
	s_scr_w = 400.0f;
}

/* 矩形裁剪(跑马灯等用)。3DS 帧缓冲相对屏幕旋转 90°,
 * scissor 坐标须按 (240-y-h, W-x-w, 240-y, W-x) 换算。
 * citro2d 是攒批渲染,改 GPU 状态前必须 C2D_Flush 把已攒的画完 */
void ui_clip(float x, float y, float w, float h) {
	C2D_Flush();
	float x2 = x + w, y2 = y + h;
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x2 > s_scr_w) x2 = s_scr_w;
	if (y2 > 240) y2 = 240;
	C3D_SetScissor(GPU_SCISSOR_NORMAL,
	               (u32)(240.0f - y2), (u32)(s_scr_w - x2),
	               (u32)(240.0f - y), (u32)(s_scr_w - x));
}

void ui_unclip(void) {
	C2D_Flush();
	C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
}

float ui_slider_3d(void) {
	return osGet3DSliderState();
}

/* 文字加重开关(见 ui.h 的说明)。默认开:界面文字全部受益;
 * 弹幕那一路自己关掉,免得顶穿顶点预算 */
static bool s_boost = true;
void ui_text_boost(bool on) { s_boost = on; }

/* 画一行字。
 *
 * 【必须检查 C2D_TextFontParse 的返回值】字形缓冲满时它返回 NULL,
 * 而栈上的 C2D_Text 还是未初始化的垃圾——把垃圾交给 C2D_DrawText,
 * 它会拿随机的 begin/end 下标和随机的 sheet 指针去画,
 * 结果就是屏幕上出现乱七八糟的方块/拉伸色带,也就是实测到的
 * "弹幕一多画面就变形"。缓冲满时正确的行为是这行字不画,
 * 而不是把渲染搞崩。 */
void ui_text_z(float x, float y, float z, float scale, uint32_t color,
               const char *utf8) {
	C2D_Text t;
	if (!utf8 || !utf8[0]) return;
	scale = eff_scale(scale);               /* 吸附+补偿,见 eff_scale */
	if (!C2D_TextFontParse(&t, s_font, s_textbuf, utf8)) return;
	C2D_TextOptimize(&t);
	/* 【坐标必须取整】纹理里的字形再锐利,只要落在半像素位置上,
	 * GPU 的双线性采样就会把每个黑像素摊成两个 50% 灰 —— 而且
	 * **二值位图偏半像素比抗锯齿位图更糊**:后者本来就是为被重采样
	 * 设计的,前者不是。实测现象:换上像素字体后反而不如原来清楚,
	 * 根源就是行高算出来是 17.33 这种小数,每行都偏了。
	 * 在这里统一取整,调用方就不必个个操心。 */
	float px = floorf(x + 0.5f), py = floorf(y + 0.5f);
	C2D_DrawText(&t, C2D_WithColor, px, py, z, scale, scale, color);
	/* 加重:同一份解析结果再画一遍(位置、字号完全一致 —— 挪半个像素
	 * 就成了描边/重影,那是另一回事,只会更糊)。同 z 后画者胜,
	 * 深度测试 GEQUAL 通过,等效于把 alpha 再混一次。 */
	if (s_boost)
		C2D_DrawText(&t, C2D_WithColor, px, py, z, scale, scale, color);
}

static void clip_cache_reset(void);   /* 定义在截断缓存那一段 */

/* 设置页的字号旋钮。上下限防手滑:0.6 以下看不清,2.5 以上一屏放不下几个字 */
void ui_font_scale_set(float k) {
	if (k < 0.6f) k = 0.6f;
	if (k > 2.5f) k = 2.5f;
	if (k == s_font_k) return;
	s_font_k = k;
	s_font_gen++;
	/* 截断缓存的 key 里只有调用方传的 scale,没有这个全局倍率 ——
	 * 不清的话字号变了还用旧的省略号位置。整片作废最省事,反正一帧就重建。
	 * (缓存本体声明在本函数之后,故走前置声明) */
	clip_cache_reset();
}

float ui_font_scale(void) { return s_font_k; }

/* 当前倍率换算成 UI_FONT_TARGET 该写多少 —— 设置页直接显示这个数,
 * 调顺眼之后照抄进 ui.c 即可,不用再从行高手算 */
float ui_font_target(void) { return s_font_k * s_font_lh; }

uint32_t ui_font_gen(void) { return s_font_gen; }

void ui_text(float x, float y, float scale, uint32_t color, const char *utf8) {
	ui_text_z(x, y, 0.5f, scale, color, utf8);
}

/* 量宽走独立缓冲并即时回收:测量不占绘制预算,也不会累积 */
float ui_text_width(const char *utf8, float scale) {
	C2D_Text t;
	if (!utf8 || !utf8[0]) return 0.0f;
	C2D_TextBufClear(s_measbuf);
	if (!C2D_TextFontParse(&t, s_font, s_measbuf, utf8)) return 0.0f;
	scale = eff_scale(scale);               /* 必须和 ui_text_z 一致,否则布局错位 */
	float w = 0, h = 0;
	C2D_TextGetDimensions(&t, scale, scale, &w, &h);
	return w;
}

/* 量一行字的高度。位图字体的行高由字体本身决定(和 mkbcfnt 的 -s 有关),
 * 不是「字号 x 某个常数」—— 硬编码行距换个字体就叠字,实测踩过。
 * 取样串带上汉字和西文升降部,拿到的才是真正的行高 */
float ui_text_height(float scale) {
	C2D_Text t;
	scale = eff_scale(scale);               /* 同上 */
	C2D_TextBufClear(s_measbuf);
	if (!C2D_TextFontParse(&t, s_font, s_measbuf, "Ag中"))
		return 16.0f * scale;      /* 兜底 */
	float w = 0, h = 0;
	C2D_TextGetDimensions(&t, scale, scale, &w, &h);
	return h;
}

/* ---------- 截断文本(带结果缓存) ----------
 *
 * 列表每帧每行要画标题 + 元信息两串,都要"超宽就加省略号"。
 * 旧写法:每次砍掉一个字符,重新量一遍整串宽度 —— 一个 40 字的标题
 * 要量 20 多遍,每量一遍就是一次完整的字形解析。四行可见 x 两串
 * x 每帧 60 次 = 每秒几十万次字形查表,滚动必然掉帧。
 * 而且这些测量以前和绘制共用字形缓冲,把槽吃光后连正经要画的字
 * 都 parse 失败 —— 画面变形和滚动卡顿其实是同一个病根。
 *
 * 现在:①二分查找截断点(约 8 次测量);②结果按内容缓存,
 * 同一串在后续帧直接命中,一次测量都不用做。 */
/* 容量必须盖得住「一屏能滚过的所有串」,否则滚动时新行进场不断把
 * 旧行挤出去,回滚又要重算——表现就是滚动中偶尔顿一下。
 * 一页 20 条 x(标题 + 元信息)= 40 串,加顶栏/按钮等界面文字,
 * 取 64 才有富余。一条约 220 字节,64 条 ~14KB,不心疼 */
#define TCLIP_N 64
typedef struct {
	const char *src;       /* 原串地址,只作 key,不解引用 */
	uint32_t hash;         /* 内容哈希:同地址换内容也能识破 */
	float scale, maxw;
	char out[208];
} ClipEnt;
static ClipEnt s_clip[TCLIP_N];
static int s_clip_next = 0;

static void clip_cache_reset(void) {
	memset(s_clip, 0, sizeof(s_clip));
	s_clip_next = 0;
}

static uint32_t str_hash(const char *s) {
	uint32_t h = 2166136261u;
	while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
	return h;
}

void ui_text_clipped_z(float x, float y, float z, float scale, uint32_t color,
                       const char *utf8, float maxw) {
	if (!utf8 || !utf8[0]) return;
	uint32_t h = str_hash(utf8);
	for (int i = 0; i < TCLIP_N; i++) {
		const ClipEnt *e = &s_clip[i];
		if (e->src == utf8 && e->hash == h &&
		    e->scale == scale && e->maxw == maxw) {
			ui_text_z(x, y, z, scale, color, e->out);
			return;
		}
	}

	char buf[208];
	snprintf(buf, sizeof(buf), "%s", utf8);
	if (ui_text_width(buf, scale) > maxw) {
		/* 二分:找最长的、留得下省略号的前缀(始终落在 UTF-8 字符边界) */
		size_t lo = 0, hi = strlen(buf);
		float lim = maxw - 12.0f;
		while (lo < hi) {
			size_t mid = (lo + hi + 1) / 2;
			while (mid > lo && ((unsigned char)buf[mid] & 0xC0) == 0x80)
				mid--;                       /* 退回字符起始字节 */
			if (mid == lo) break;            /* 再分不下去了 */
			char save = buf[mid];
			buf[mid] = 0;
			float w = ui_text_width(buf, scale);
			buf[mid] = save;
			if (w <= lim) lo = mid;
			else          hi = mid - 1;
		}
		buf[lo] = 0;
		strncat(buf, "...", sizeof(buf) - strlen(buf) - 1);
	}

	ClipEnt *e = &s_clip[s_clip_next];
	s_clip_next = (s_clip_next + 1) % TCLIP_N;
	e->src = utf8;
	e->hash = h;
	e->scale = scale;
	e->maxw = maxw;
	snprintf(e->out, sizeof(e->out), "%s", buf);
	ui_text_z(x, y, z, scale, color, buf);
}

void ui_text_clipped(float x, float y, float scale, uint32_t color,
                     const char *utf8, float maxw) {
	ui_text_clipped_z(x, y, 0.5f, scale, color, utf8, maxw);
}

void ui_rect(float x, float y, float w, float h, uint32_t color) {
	C2D_DrawRectSolid(x, y, 0.4f, w, h, color);
}

void ui_rect_z(float x, float y, float z, float w, float h, uint32_t color) {
	C2D_DrawRectSolid(x, y, z, w, h, color);
}

void ui_log_ascii(const char *prefix, const char *str, int maxlen) {
	char buf[161];
	if (maxlen > (int)sizeof(buf) - 1 || maxlen <= 0)
		maxlen = (int)sizeof(buf) - 1;
	int o = 0;
	for (const unsigned char *r = (const unsigned char *)str;
	     *r && o < maxlen; r++) {
		if (*r >= 0x20 && *r < 0x7F) buf[o++] = (char)*r;
		else if (*r >= 0x80) {
			if (o == 0 || buf[o - 1] != '?') buf[o++] = '?'; /* 连续多字节合并 */
		}
		/* 控制字符直接丢弃 */
	}
	buf[o] = 0;
	printf("%s%s\n", prefix ? prefix : "", buf);
}

void ui_bottom_debug(bool on) {
	/* 日志台改为 citro2d 自绘(不再用 3DS console):
	 * console 会把下屏切成 RGB565 单缓冲,退出后另一个后台缓冲还留着
	 * 旧内容 → 与 GPU 双缓冲打架,表现为主页闪烁(封面上传那几帧尤其明显)。
	 * 自绘就没有任何格式切换,顺带还能显示中文。 */
	s_console_mode = on;
	if (on) s_log_scroll = 0;
}

void ui_log_scroll(int delta) {
	s_log_scroll += delta;
	if (s_log_scroll < 0) s_log_scroll = 0;
	int maxs = s_log_n - s_log_view;
	if (maxs < 0) maxs = 0;
	if (s_log_scroll > maxs) s_log_scroll = maxs;
}

/* 日志页:字号 0.5,触摸拖动滚动,双击退出。
 * 返回 true 表示"请求退出"(双击),调用方据此关掉日志页 */
bool ui_draw_log(bool touch_down, bool touch_held, float tx, float ty) {
	static bool dragging = false;
	static float drag_y0 = 0;
	static float drag_scroll0 = 0;
	static u64 last_tap = 0;
	static float last_tap_y = 0;
	bool want_exit = false;
	(void)tx;

	/* 先量字体:下面的拖动换算、一屏行数、折行长度全依赖它。
	 * 位图字体的行高由字体本身决定,写死常数换个字体就叠字 */
	float lh = (float)(int)(ui_text_height(LOG_FONT) + 1.5f);   /* 取整,同上 */
	{	/* 顺便按实测字宽更新折行长度(下次 ui_printf 生效)。
		 * 写死字节数的话,换字体后右半截又会被 ui_text_clipped 截掉 */
		float w10 = ui_text_width("MMMMMMMMMM", LOG_FONT);
		if (w10 > 1.0f) {
			int n = (int)(312.0f * 10.0f / w10);
			if (n < 16) n = 16;
			if (n > LOG_COLS - 5) n = LOG_COLS - 5;
			s_log_wrap = n;
		}
	}
	/* 顶栏/底栏高度也随字体 —— 写死 24 的话,字一变大就顶出栏外 */
	float bar = lh + 6.0f;
	float body_top = bar;
	float body_bot = 240.0f - bar;
	int view = (int)((body_bot - body_top) / lh);
	if (view < 1) view = 1;
	if (view > LOG_VIEW_MAX) view = LOG_VIEW_MAX;
	s_log_view = view;

	if (touch_down) {
		u64 now = osGetTime();
		/* 双击退出:两次点按间隔 <400ms 且位置接近 */
		if (now - last_tap < 400 &&
		    (ty - last_tap_y < 24.0f && last_tap_y - ty < 24.0f))
			want_exit = true;
		last_tap = now;
		last_tap_y = ty;
		dragging = true;
		drag_y0 = ty;
		drag_scroll0 = (float)s_log_scroll;
	}
	if (dragging && touch_held) {
		/* 手指下拉 = 看更早的日志(内容跟手向下走) */
		float d = (ty - drag_y0) / lh;
		int ns = (int)(drag_scroll0 + d);
		int maxs = s_log_n - s_log_view;
		if (maxs < 0) maxs = 0;
		if (ns < 0) ns = 0;
		if (ns > maxs) ns = maxs;
		s_log_scroll = ns;
	}
	if (!touch_held) dragging = false;

	ui_begin_bottom();
	ui_rect(0, 0, 320, bar, UI_COL_ACCENT);
	char hdr[64];
	snprintf(hdr, sizeof(hdr), "调试日志 %d 行  第%d行起",
	         s_log_n, s_log_n - s_log_scroll);
	ui_text(6, (bar - lh) / 2.0f, LOG_FONT, UI_COL_WHITE, hdr);
	LightLock_Lock(&s_log_lock);
	int shown = s_log_n < view ? s_log_n : view;
	for (int i = 0; i < shown; i++) {
		int back = shown - i + s_log_scroll;
		int idx = (s_log_head - back + LOG_LINES * 2) % LOG_LINES;
		/* 1.0 倍绘制:位图字体只有原生尺寸才锐利(详见 comment.c 的说明) */
		ui_text_clipped(4, body_top + i * lh, LOG_FONT, UI_COL_TEXT,
		                s_log[idx], 312);
	}
	LightLock_Unlock(&s_log_lock);
	/* 滚动条 */
	if (s_log_n > s_log_view) {
		float frac = (float)s_log_view / (float)s_log_n;
		float pos = (float)(s_log_n - s_log_scroll - s_log_view) /
		            (float)(s_log_n - s_log_view);
		float bh = (body_bot - body_top) * frac;
		if (bh < 12) bh = 12;
		float track = body_bot - body_top;
		if (bh > track) bh = track;
		ui_rect(314, body_top, 4, track, C2D_Color32(0x30, 0x30, 0x3C, 0xFF));
		ui_rect(314, body_top + (track - bh) * pos, 4, bh, UI_COL_ACCENT);
	}
	/* 底部提示行:位置和高度都跟着字体走,否则字会顶出屏幕 */
	ui_rect(0, body_bot, 320, 240.0f - body_bot,
	        C2D_Color32(0x1A, 0x1A, 0x22, 0xFF));
	ui_text(6, body_bot + (bar - lh) / 2.0f, LOG_FONT, UI_COL_DIM,
	        "拖动滚动   双击退出");
	return want_exit;
}

bool ui_console_active(void) { return s_console_mode; }

void ui_begin_bottom(void) {
	C2D_TargetClear(s_bot, UI_COL_BG);
	C2D_SceneBegin(s_bot);
	s_scr_w = 320.0f;
}

bool ui_button(float x, float y, float w, float h, const char *label,
               uint32_t bg, bool touched, float tx, float ty) {
	bool hit = touched && tx >= x && tx < x + w && ty >= y && ty < y + h;
	/* 按下反馈不用 ACCENT:ACCENT 表示"当前页",两者同色会在点击那一帧
	 * 出现"两个都亮"。改为压暗底色 + 白边,语义不冲突 */
	ui_rect(x, y, w, h, bg);
	if (hit) {
		ui_rect(x, y, w, 2, UI_COL_WHITE);
		ui_rect(x, y + h - 2, w, 2, UI_COL_WHITE);
		ui_rect(x, y, 2, h, UI_COL_WHITE);
		ui_rect(x + w - 2, y, 2, h, UI_COL_WHITE);
	}
	ui_rect(x, y + h - 2, w, 2, C2D_Color32(0, 0, 0, 0x60)); /* 底边阴影 */
	/* 按钮文字走全局清晰档(UI_SHARP)。从 0.7 降下来后和正文同尺寸 ——
	 * 层次感由底色和边框承担,清晰优先。三处必须是同一个数:
	 * 量宽、量高、绘制任何一个对不上,居中就会偏。 */
	float tw = ui_text_width(label, UI_SHARP);
	float th = ui_text_height(UI_SHARP);
	ui_text(x + (w - tw) / 2, y + (h - th) / 2, UI_SHARP,
	        UI_COL_WHITE, label);
	return hit;
}

void ui_qr(const uint8_t *qrcode, float cx, float cy, int module_px) {
	int size = qrcodegen_getSize(qrcode);
	float px = (float)module_px;
	float total = (size + 8) * px; /* 每边 4 模块安静区 */
	float x0 = cx - total / 2, y0 = cy - total / 2;
	ui_rect(x0, y0, total, total, UI_COL_WHITE);
	float off = 4 * px;
	for (int y = 0; y < size; y++)
		for (int x = 0; x < size; x++)
			if (qrcodegen_getModule(qrcode, x, y))
				ui_rect(x0 + off + x * px, y0 + off + y * px, px, px, UI_COL_BLACK);
}

/* ---------- 设置列表 + 说明 ----------
 * 触屏为主:拖动滚动、点按修改、右侧滚动条可拖。不接管方向键 ——
 * 它们在别的页面另有用途,同一个键在两个上下文做不同的事只会误触。
 * 状态放这里而不是让调用方保管:同一时刻只有一页列表活着,
 * 多传五个参数换一点纯度不划算。 */
static float s_ls_scroll = 0.0f;
static bool  s_ls_drag = false, s_ls_bar = false;
static float s_ls_y0 = 0.0f, s_ls_s0 = 0.0f, s_ls_moved = 0.0f;
static float s_ls_lx = 0.0f, s_ls_ly = 0.0f;   /* 最后一次有效触点 */
static int   s_ls_focus = -1;

void ui_list_reset(void) {
	s_ls_scroll = 0.0f;
	s_ls_drag = s_ls_bar = false;
	s_ls_moved = 0.0f;
	s_ls_focus = -1;
}

int ui_list_focus(void) { return s_ls_focus; }

int ui_list_draw(const char *title, const UiRow *rows, int n,
                 bool touched, bool holding, bool released,
                 float tx, float ty) {
	const float LX = 6.0f, LW = 296.0f, LY = 24.0f;
	const float BAR_X = 306.0f, BAR_W = 8.0f;
	float th = ui_text_height(UI_SHARP);
	float rowh = th + 12.0f;                  /* 行高跟着字号走,别写死 */
	float hint_y = 240.0f - th - 6.0f;
	float LH = hint_y - 6.0f - LY;
	float maxs = (float)n * rowh - LH;
	if (maxs < 0) maxs = 0;
	int picked = -1;

	/* 【坐标要自己记】hidTouchRead 只在按住期间有效,松手那一帧是 (0,0),
	 * 而点选恰恰在松手时判定 —— 直接用 tx/ty 会永远命中左上角。 */
	if (touched) {
		s_ls_moved = 0.0f;
		s_ls_y0 = ty; s_ls_s0 = s_ls_scroll;
		s_ls_lx = tx; s_ls_ly = ty;
		s_ls_bar = (tx >= BAR_X - 6 && maxs > 0);
		s_ls_drag = !s_ls_bar && ty >= LY && ty < LY + LH;
		if (s_ls_bar) s_ls_scroll = (ty - LY) / LH * maxs;
	}
	if (holding) {
		s_ls_lx = tx; s_ls_ly = ty;
		float dy = ty - s_ls_y0, ady = dy < 0 ? -dy : dy;
		if (ady > s_ls_moved) s_ls_moved = ady;   /* 记最大偏移,不是累加 */
		if (s_ls_bar) s_ls_scroll = (ty - LY) / LH * maxs;
		else if (s_ls_drag) s_ls_scroll = s_ls_s0 - dy;
	}
	if (!holding) { s_ls_drag = s_ls_bar = false; }
	if (s_ls_scroll < 0) s_ls_scroll = 0;
	if (s_ls_scroll > maxs) s_ls_scroll = maxs;

	ui_begin_bottom();
	ui_rect(0, 0, 320, 22, UI_COL_ACCENT);
	ui_text(8, (22.0f - th) / 2.0f, UI_SHARP, UI_COL_WHITE, title);

	ui_clip(LX, LY, LW + 4.0f, LH);
	int first = (int)(s_ls_scroll / rowh);
	if (first < 0) first = 0;
	for (int i = first; i < n; i++) {
		float y = (float)(int)(LY + i * rowh - s_ls_scroll);
		if (y >= LY + LH) break;
		float h = rowh - 2.0f;
		bool inrow = s_ls_lx >= LX && s_ls_lx < LX + LW &&
		             s_ls_ly >= y && s_ls_ly < y + h &&
		             s_ls_ly >= LY && s_ls_ly < LY + LH;
		/* 按下就更新高亮:上屏的说明要立刻跟上,不能等松手 */
		if (touched && inrow) s_ls_focus = i;
		if (released && inrow && s_ls_moved < 8.0f) picked = i;
		ui_rect(LX, y, LW, h, (i == s_ls_focus) ? UI_COL_SEL
		                                        : C2D_Color32(0x22,0x22,0x2C,0xFF));
		/* 按住反馈:和 ui_button 同款白边,但按住期间一直显示 ——
		 * 这一页要拖要滑,一帧的反馈根本看不见 */
		if (holding && inrow && s_ls_moved < 8.0f && !s_ls_bar) {
			ui_rect(LX, y, LW, 2, UI_COL_WHITE);
			ui_rect(LX, y + h - 2, LW, 2, UI_COL_WHITE);
			ui_rect(LX, y, 2, h, UI_COL_WHITE);
			ui_rect(LX + LW - 2, y, 2, h, UI_COL_WHITE);
		}
		float ty2 = y + (h - th) / 2.0f;
		ui_text(LX + 10, ty2, UI_SHARP, UI_COL_TEXT, rows[i].name);
		/* 动作项(value 为 NULL)右侧什么都不画。
		 * 原来画一个 "›" —— 字体是子集化过的,那个字形不在里面,
		 * 渲染出来是个问号。**子集字体里只能用确定收录了的字符**。 */
		if (rows[i].value) {
			float vw = ui_text_width(rows[i].value, UI_SHARP);
			ui_text(LX + LW - 10 - vw, ty2, UI_SHARP, UI_COL_ACCENT,
			        rows[i].value);
		}
	}
	ui_unclip();

	if (maxs > 0) {
		float bh = LH * LH / ((float)n * rowh);
		if (bh < 16.0f) bh = 16.0f;
		float pos = s_ls_scroll / maxs;
		ui_rect(BAR_X, LY, BAR_W, LH, C2D_Color32(0x30,0x30,0x3C,0xFF));
		ui_rect(BAR_X, LY + (LH - bh) * pos, BAR_W, bh,
		        s_ls_bar ? UI_COL_WHITE : UI_COL_ACCENT);
	}
	ui_text(8, hint_y, UI_SHARP, UI_COL_DIM, "滑动翻找   点按修改   B 返回");
	return picked;
}

void ui_help_draw(const char *title, const char *body, const char *footer1,
                  const char *footer2) {
	/* 【z 必须抬到弹幕之上】这一页是画在**正在播放的画面上**的,
	 * 而弹幕走的是 ui_text 的默认 0.5。顶栏用 ui_rect 是 0.4,
	 * 于是弹幕从标题栏里穿过去 —— 看着就像弹幕那层透明底盖住了条。
	 * 说明正文同理。0.8/0.85 高过上屏所有既有图层(弹幕 0.5、
	 * 「弹幕加载中」0.6/0.7)。 */
	const float ZBAR = 0.8f, ZTXT = 0.85f;
	const float PAD = 16.0f;
	float th = ui_text_height(UI_SHARP);
	float hh = th + 14.0f;
	ui_rect_z(0, 0, ZBAR, 400, hh, UI_COL_ACCENT);
	ui_text_z(PAD, (hh - th) / 2.0f, ZTXT, UI_SHARP, UI_COL_WHITE,
	          title ? title : "");
	/* 正文按 \n 逐行画。这里不做自动折行 —— 见 ui.h 里的说明 */
	float y = hh + 14.0f;
	if (body) {
		char line[160];
		const char *p = body;
		while (*p && y < 240.0f - th * 3.0f) {
			const char *nl = strchr(p, '\n');
			size_t len = nl ? (size_t)(nl - p) : strlen(p);
			if (len >= sizeof(line)) len = sizeof(line) - 1;
			memcpy(line, p, len);
			line[len] = 0;
			ui_text_z(PAD, y, ZTXT, UI_SHARP, UI_COL_TEXT, line);
			y += th + 4.0f;
			if (!nl) break;
			p = nl + 1;
		}
	}
	/* 底部两行从下往上排,表格行数变了也不用跟着改 */
	float ly = 240.0f - th - 8.0f;
	if (footer2) { ui_text_z(PAD, ly, ZTXT, UI_SHARP, UI_COL_DIM, footer2);
	               ly -= th + 4.0f; }
	if (footer1) ui_text_z(PAD, ly, ZTXT, UI_SHARP, UI_COL_ACCENT, footer1);
}
