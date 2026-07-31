/* 3Danmu:非官方视频客户端,运行在自制系统的 3DS 上
 *
 * 操作:
 *   十字键上下   选择视频
 *   A           播放(播放中 A 暂停 / B 退出)
 *   Y           搜索
 *   X           扫码登录 / 注销
 *   L / R       上一页 / 下一页
 *   SELECT      回到热门
 *   START       退出程序
 */
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui.h"
#include "net.h"
#include "bili.h"
#include "player.h"
#include "danmaku.h"
#include "ime.h"
#include "settings.h"
#include "thumb.h"
#include "subtitle.h"
#include "tls.h"
#include "vendor/qrcodegen.h"

#define MAX_LIST 20

/* 加大主线程栈(默认 32KB,ffmpeg 解码 + 深调用链不够用) */
unsigned int __stacksize__ = 256 * 1024;

/* 【线性堆必须显式声明,且预算里要算上字体】
 * 线性堆(linearAlloc)装的是所有要给硬件看的缓冲。完整清单(实测):
 *   字体图集       8.2MB  ← C2D_FontLoad 把字形表当纹理放这里!
 *                          (曾经漏算它,14MB 的线性堆被字体吃掉大半,
 *                           MVD 连 2.4MB 输入缓冲都分不出来,硬解全灭)
 *   MVD 工作区     5~7MB
 *   MVD 输入/输出  2.4MB
 *   视频纹理+vout  2.0MB
 *   音频/封面      0.5MB
 *   合计峰值      ~20MB
 * CIA 的默认划分远小于此;.3dsx 由 Homebrew Launcher 决定(其 env 参数
 * 优先于本变量,所以这里改大不影响 3dsx)。
 * 25MB 是为了 480P 硬解:L3.1 工作区约 8.5MB,加上字体 8.2 和其余缓冲,
 * 22MB 差一口气。普通堆拿剩下的(约 64-13-25=26MB),硬解路径下 ffmpeg
 * 只做解封装,足够;软解路径帧缓冲也在堆里,实测同样够用。 */
u32 __ctru_linear_heap_size = 25 * 1024 * 1024;

typedef enum { MODE_POPULAR, MODE_RECOMMEND, MODE_SEARCH,
               MODE_HISTORY, MODE_FAV } ListMode;

static BiliVideo s_list[MAX_LIST];
static int  s_count = 0;
static int  s_sel = 0;
static float s_scroll = 0.0f;       /* 列表滚动偏移(显示值,像素) */
static float s_scroll_t = 0.0f;     /* 滚动目标值:输入写这里,显示值每帧缓动跟随 */
#define ROW_H   66.0f               /* 图文卡片行高 */
#define LIST_Y  26.0f               /* 列表区顶部(顶栏下方) */
#define LIST_H  (240.0f - LIST_Y)
static int  s_page = 1;
static ListMode s_mode = MODE_RECOMMEND;   /* 默认进推荐(和官方 App 一致) */
static int s_pending_mode = -1;   /* 登录界面里点了频道:退出后切过去 */
static int s_hl_mode = -1;        /* 高亮覆盖:点击后立刻亮新的(-1=跟随 s_mode) */
static char s_keyword[128] = {0};
static char s_status[192] = "";
/* 名字以数字开头,所以宏名不能叫 3DANMU_VERSION(C 标识符不许数字打头) */
#define APP_VERSION "1.0.2"

static bool g_danmaku = true;       /* 设置:弹幕开关 */
static int  g_qn = 16;              /* 设置:清晰度 16=360P 32=480P */
static bool g_force_sw = false;     /* 设置:强制软解 */
static bool s_in_settings = false;  /* 当前在设置页 */
static bool s_debug_ui = false;

/* 下屏 console 只支持 ASCII:log 打英文,中文状态只画在上屏 */
static char s_busy[128] = "";  /* 状态条:正在做什么 / 上次失败原因(空=空闲) */

static void set_status(const char *ui_utf8, const char *log_ascii) {
	snprintf(s_status, sizeof(s_status), "%s", ui_utf8);
	if (log_ascii) printf("%s\n", log_ascii);
}

static void load_list(void) {
	/* 【顺序要紧】先停封面,再发接口请求。
	 * 反过来的话,换页时上一页的封面线程会和新的 API 请求并发 ——
	 * 而「图片请求不与 API 请求同时在飞」正是下面放开图片并发的前提。
	 * 这个不变式靠调用顺序保证,比靠一把全局大锁便宜得多。 */
	thumb_stop();
	set_status("加载中...", "loading...");
	/* 立即画一帧提示 */
	ui_begin();
	ui_text(150, 110, UI_SHARP, UI_COL_DIM, "加载中...");
	ui_end();

	int r;
	switch (s_mode) {
	case MODE_SEARCH:
		r = bili_search(s_keyword, s_page, s_list, MAX_LIST, &s_count);
		break;
	case MODE_RECOMMEND:
		r = bili_recommend(s_page, s_list, MAX_LIST, &s_count);
		break;
	case MODE_HISTORY:
		r = bili_history(s_page, s_list, MAX_LIST, &s_count);
		break;
	case MODE_FAV:
		r = bili_fav(s_page, s_list, MAX_LIST, &s_count);
		break;
	default:
		r = bili_popular(s_page, s_list, MAX_LIST, &s_count);
		break;
	}

	s_sel = 0;
	s_scroll = 0.0f;
	s_scroll_t = 0.0f;
	if (r != 0) {
		s_count = 0;
		char sbuf[160];
		snprintf(sbuf, sizeof(sbuf), "加载失败 %s(A 重试 / B 返回)",
		         bili_last_error());
		set_status(sbuf, "load failed, A=retry B=back");
		snprintf(s_busy, sizeof(s_busy), "加载失败:%s", bili_last_error());
	} else {
		char buf[64];
		snprintf(buf, sizeof(buf), "loaded page %d, %d items", s_page, s_count);
		set_status("", buf);
		s_busy[0] = 0;      /* 加载成功:清掉上次的失败原因 */
	}
	if (s_count > 0)
		thumb_start(s_list, s_count);
}

static void fmt_meta(const BiliVideo *v, char *out, size_t n) {
	char views[32] = "";
	if (v->views >= 100000000)
		snprintf(views, sizeof(views), " %.1f亿", v->views / 1e8);
	else if (v->views >= 10000)
		snprintf(views, sizeof(views), " %.1f万", v->views / 1e4);
	else if (v->views >= 0)
		snprintf(views, sizeof(views), " %ld", (long)v->views);
	char dur[24] = "";
	if (v->duration > 0)
		snprintf(dur, sizeof(dur), "  %d:%02d", v->duration / 60, v->duration % 60);
	snprintf(out, n, "%s%s播放%s", v->author, views, dur);
}

static void draw_list(void) {
	ui_begin();
	/* 封面上传预算:静止时每帧 1 张;滚动进行中为 0——同步 GPU 传输
	 * 哪怕 1-2ms 也会在滚动时被肉眼捕捉为顿挫,滚完再补 */
	{
		float d = s_scroll - s_scroll_t;
		thumb_new_frame((d > -1.0f && d < 1.0f) ? 1 : 0);
	}
	/* 层级:citro2d 深度测试为 GEQUAL —— z 大者在上,同 z 后画者胜。
	 * 实测数据:选中高亮 z=0.4 时会盖掉缩略图(z=0.2),行文字(z=0.5)
	 * 会穿透后画的顶栏(z=0.4)。所以:高亮 0.15 < 图 0.2 < 文字 0.5
	 * < 顶栏底 0.6 < 顶栏字 0.7 */

	/* 列表:图文卡片,s_scroll 像素级滚动(左摇杆) */
	{
		int first = (int)(s_scroll / ROW_H);
		if (first < 0) first = 0;
		for (int i = first; i < s_count; i++) {
			/* 坐标取整:小数位置会让文字/边缘在像素间跳,滚动看着发抖 */
			float y = (float)(int)(LIST_Y + i * ROW_H - s_scroll);
			if (y > 240) break;
			if (i == s_sel)
				ui_rect_z(0, y, 0.15f, 400, ROW_H - 4, UI_COL_SEL);
			const C2D_Image *im = thumb_get(i);
			if (im) {
				C2D_DrawImageAt(*im, 6, y + 2, 0.2f, NULL, 1.0f, 1.0f);
			} else {
				ui_rect_z(6, y + 2, 0.18f, THUMB_W, THUMB_H,
				          C2D_Color32(0x30, 0x30, 0x3C, 0xFF));
				ui_text(6 + 38, y + 24, 0.45f, UI_COL_DIM, "…");
			}
			if (i == s_sel) {
				/* 选中行:标题过长时缓慢跑马灯(停 1.2s → 滚动 →
				 * 到尾停 1s → 回起点)。裁剪住行框,不糊到封面/顶栏 */
				static float marq = 0;
				static int marq_sel = -1;
				static u64 marq_t0 = 0;
				static float marq_tw = 0;   /* 标题宽度:换行才重算 */
				static size_t marq_len = 0; /* 换页后同下标不同视频也要重算 */
				size_t tlen = strlen(s_list[i].title);
				if (marq_sel != i || marq_len != tlen) {
					marq_sel = i;
					marq_len = tlen;
					marq = 0;
					marq_t0 = osGetTime();
					/* 量宽是完整的字形解析,别每帧都做 */
					marq_tw = ui_text_width(s_list[i].title, UI_SHARP);
				}
				float tw = marq_tw;
				const float mw = 286.0f;
				if (tw > mw) {
					float span = tw - mw + 12.0f;
					if (osGetTime() - marq_t0 > 1200) {
						marq += 0.55f;
						if (marq > span + 55.0f) {   /* 尾部停顿后回起点 */
							marq = 0;
							marq_t0 = osGetTime();
						}
					}
					float off = marq > span ? span : marq;
					ui_clip(110, y + 2, mw, 24);
					ui_text(110 - off, y + 2, UI_SHARP, UI_COL_WHITE,
					        s_list[i].title);
					ui_unclip();
				} else {
					ui_text(110, y + 2, UI_SHARP, UI_COL_WHITE,
					        s_list[i].title);
				}
			} else {
				ui_text_clipped(110, y + 2, UI_SHARP, UI_COL_TEXT,
				                s_list[i].title, 286);
			}
			char meta[128];
			fmt_meta(&s_list[i], meta, sizeof(meta));
			ui_text_clipped(110, y + 42, UI_SHARP, UI_COL_DIM, meta, 286);
		}
	}
	if (s_count == 0)
		ui_text(120, 110, UI_SHARP, UI_COL_DIM, s_status);

	/* 顶栏:z 抬到行文字(0.5)之上才能真正遮住上浮的行 */
	ui_rect_z(0, 0, 0.6f, 400, 26, UI_COL_ACCENT);
	char title[160];
	switch (s_mode) {
	case MODE_SEARCH:
		snprintf(title, sizeof(title), "搜索: %s  P%d", s_keyword, s_page);
		break;
	case MODE_RECOMMEND:
		snprintf(title, sizeof(title), "为你推荐");
		break;
	case MODE_HISTORY:
		snprintf(title, sizeof(title), "历史记录  P%d", s_page);
		break;
	case MODE_FAV:
		snprintf(title, sizeof(title), "默认收藏夹  P%d", s_page);
		break;
	default:
		snprintf(title, sizeof(title), "热门  P%d", s_page);
		break;
	}
	ui_text_clipped_z(6, 3, 0.7f, UI_SHARP, UI_COL_WHITE, title, 300);
	const char *user = bili_username();
	ui_text_clipped_z(302, 3, 0.7f, UI_SHARP, UI_COL_WHITE,
	                  user ? user : "未登录", 95);
}

typedef struct {
	bool login, settings, hist, fav, popular, recommend;
} ListActions;

static void draw_bottom_list(bool touched, float tx, float ty, ListActions *act) {
	ui_begin_bottom();
	ui_text(10, 4, UI_SHARP, UI_COL_TEXT, bili_logged_in() ? "已登录" : "未登录");
	if (s_count == 0)
		ui_text(108, 4, UI_SHARP, UI_COL_ACCENT, "加载失败?按 A 重试");
	{	/* 底部状态条:在做什么 + 总进度 */
		int done = 0, total = 0;
		bool busy = thumb_progress(&done, &total);
		ui_rect(10, 206, 300, 28, C2D_Color32(0x26, 0x26, 0x30, 0xFF));
		if (s_busy[0]) {  /* 当前动作 或 上次失败原因 */
			ui_text_clipped_z(18, 210, 0.55f, UI_SHARP, UI_COL_WHITE,
			                  s_busy, 284);
		} else if (busy && total > 0) {
			/* 只写文字,不涂进度条:封面是逐张浮现的,列表本身就是进度,
			 * 底下再来一条爬动的色块只是抢注意力 */
			char pbuf[48];
			snprintf(pbuf, sizeof(pbuf), "加载封面 %d/%d", done, total);
			ui_text_z(18, 210, 0.55f, UI_SHARP, UI_COL_WHITE, pbuf);
		} else {
			/* 空闲时兼作按键说明(别再单画一行,会和条重叠——踩过) */
			ui_text_z(18, 210, 0.55f, UI_SHARP, UI_COL_DIM,
			          s_count ? "A播放 Y搜索 ZL/ZR换频道 L/R翻页"
			                  : "等待加载(A 重试)");
		}
	}
	/* 搜索/翻页没有按钮:Y 搜索、L/R 翻页(状态条有提示)。
	 * 高亮 = s_hl_mode(点击后立刻切过去,连扫码登录期间也跟着走),
	 * 未设置时跟随实际频道 s_mode */
	int hl = (s_hl_mode >= 0) ? s_hl_mode : (int)s_mode;
	if (ui_button(10, 32, 145, 40, "推荐",
	              hl == MODE_RECOMMEND ? UI_COL_ACCENT : UI_COL_SEL,
	              touched, tx, ty)) {
		act->recommend = true;
		s_hl_mode = MODE_RECOMMEND;
	}
	if (ui_button(165, 32, 145, 40, "热门",
	              hl == MODE_POPULAR ? UI_COL_ACCENT : UI_COL_SEL,
	              touched, tx, ty)) {
		act->popular = true;
		s_hl_mode = MODE_POPULAR;
	}
	if (ui_button(10, 80, 145, 40, "历史",
	              hl == MODE_HISTORY ? UI_COL_ACCENT : UI_COL_SEL,
	              touched, tx, ty)) {
		act->hist = true;
		s_hl_mode = MODE_HISTORY;
	}
	if (ui_button(165, 80, 145, 40, "收藏",
	              hl == MODE_FAV ? UI_COL_ACCENT : UI_COL_SEL,
	              touched, tx, ty)) {
		act->fav = true;
		s_hl_mode = MODE_FAV;
	}
	if (ui_button(10, 128, 145, 40, "设置", UI_COL_SEL, touched, tx, ty))
		act->settings = true;
	if (ui_button(165, 128, 145, 40, bili_logged_in() ? "注销" : "扫码登录",
	              UI_COL_SEL, touched, tx, ty))
		act->login = true;
}

/* 设置页上屏:当前设置一览。
 * 原来只有一个大字"设置"配一行说明,空着大半屏 —— 改成把下屏各选项的
 * **当前值**同步显示出来:点一下下屏,上屏立刻反映变化,比纯装饰有用。
 * 所有纵向尺寸都由 ui_text_height 推导,改字号不会挤成一团。 */
static void draw_top_settings(void) {
	const float W = 400.0f;
	const float PAD = 16.0f;
	float th_title = ui_text_height(UI_SHARP);
	float th_row   = ui_text_height(UI_SHARP);
	float th_tip   = ui_text_height(UI_SHARP);

	/* 标题条 */
	float hh = th_title + 14.0f;
	ui_rect(0, 0, W, hh, UI_COL_ACCENT);
	ui_text(PAD, (hh - th_title) / 2.0f, UI_SHARP, UI_COL_WHITE, "设置");
	{
		const char *brand = "3Danmu v" APP_VERSION;
		float bw = ui_text_width(brand, UI_SHARP);
		ui_text(W - PAD - bw, (hh - th_tip) / 2.0f, UI_SHARP, UI_COL_WHITE, brand);
	}

	char cache[40];
	snprintf(cache, sizeof(cache), "%d MB", (int)(thumb_cache_kb() / 1024));
	struct { const char *k; const char *v; } rows[] = {
		{ "弹幕",     g_danmaku ? "开" : "关" },
		{ "清晰度",   g_qn == 32 ? "480P" : "360P" },
		{ "解码方式", g_force_sw ? "强制软解" : "自动(优先硬解)" },
		{ "账号",     bili_logged_in() ? "已登录" : "未登录" },
		{ "封面缓存", cache },
	};
	const int n = (int)(sizeof(rows) / sizeof(rows[0]));

	float rh = th_row + 6.0f;    /* 收紧一点,给底部的声明行让位 */
	float y  = hh + 10.0f;
	for (int i = 0; i < n; i++) {
		/* 隔行底色:分隔线在这个尺寸上太细,几乎看不出来,不如整行铺色 */
		if (i & 1) ui_rect(PAD, y, W - PAD * 2, rh, UI_COL_SEL);
		ui_rect(PAD, y, 3, rh, UI_COL_ACCENT);          /* 左侧色条 */
		float ty2 = y + (rh - th_row) / 2.0f;
		ui_text(PAD + 12, ty2, UI_SHARP, UI_COL_DIM, rows[i].k);
		float vw = ui_text_width(rows[i].v, UI_SHARP);     /* 值右对齐 */
		ui_text(W - PAD - 4 - vw, ty2, UI_SHARP, UI_COL_TEXT, rows[i].v);
		y += rh + 2.0f;
	}

	/* 底部两行,从下往上排 —— 上面表格的行数以后要是变了,这里不用跟着改。
	 * 版本号在右上角标题栏里已经有了(3Danmu vX.Y.Z),不重复占一行。 */
	float ly = 240.0f - th_tip - 8.0f;
	ui_text(PAD, ly, UI_SHARP, UI_COL_DIM, "下屏点按修改  /  B 返回");
	ly -= th_tip + 5.0f;
	/* 别再往这一行里塞字了:上屏可用宽度 384px,汉字在清晰档约 18px 一个,
	 * 21 个字加空格就要溢出。真要加内容就再开一行(表格行高还能收)。 */
	ui_text(PAD, ly, UI_SHARP, UI_COL_ACCENT,
	        "仅供学习交流  严禁用于商业用途");
}

static void draw_bottom_settings(bool touched, float tx, float ty) {
	ui_begin_bottom();
	ui_text(10, 5, UI_SHARP, UI_COL_TEXT, "设置");
	if (ui_button(10, 32, 145, 40, g_danmaku ? "弹幕:开" : "弹幕:关",
	              UI_COL_SEL, touched, tx, ty))
		{ g_danmaku = !g_danmaku; settings_set("danmaku", g_danmaku); }
	if (ui_button(165, 32, 145, 40, g_qn == 32 ? "清晰度:480P" : "清晰度:360P",
	              UI_COL_SEL, touched, tx, ty))
		{ g_qn = (g_qn == 32) ? 16 : 32; settings_set("qn", g_qn); }
	if (ui_button(10, 80, 145, 40, g_force_sw ? "解码:软解" : "解码:自动",
	              UI_COL_SEL, touched, tx, ty))
		{ g_force_sw = !g_force_sw; settings_set("force_sw", g_force_sw); }
	if (ui_button(165, 80, 145, 40, "调试台",
	              UI_COL_SEL, touched, tx, ty))
		s_debug_ui = true; /* 帧结束后再切,当帧内切会被 GPU 覆盖成花屏 */

	/* 封面缓存:按钮上直接显示占用,点一下清空。
	 * 大小是缓存模块自己维护的运行值,不用每帧扫盘 */
	{
		static u64 cleared_at = 0;
		char lbl[48];
		if (cleared_at && osGetTime() - cleared_at < 1500)
			snprintf(lbl, sizeof(lbl), "封面缓存:已清空");
		else
			snprintf(lbl, sizeof(lbl), "清理封面缓存 %dMB",
			         (int)(thumb_cache_kb() / 1024));
		if (ui_button(10, 128, 300, 40, lbl, UI_COL_SEL, touched, tx, ty)) {
			thumb_cache_clear();
			cleared_at = osGetTime();
		}
	}

	/* 字号 -/+ 旋钮和 TARGET 读数已撤(v1.0.0 定稿)。它们是换字体时的
	 * 标定工具,不是用户设置 —— 用户挪动 TARGET 只会把所有字挪出清晰点。
	 * 下次换字体需要重标时,从 git 历史把这一段找回来,或直接看
	 * trace.log 开机第一行的 sharp=../100(那个更准,还不用肉眼猜)。 */
	if (ui_button(10, 176, 300, 40, "返回", UI_COL_SEL, touched, tx, ty))
		s_in_settings = false;
	ui_text(10, 222, UI_SHARP, UI_COL_DIM,
	        "B 返回  /  缓存超 100MB 自动清空");
}

static void do_search(void) {
	char input[128] = {0};
	/* 自带触屏拼音输入法(词库 romfs:/pinyin.dic);
	 * 词库缺失时退化为英文键盘,不再依赖 3DS 系统键盘 */
	if (ime_input("搜索视频", s_keyword, input, sizeof(input)) && input[0]) {
		snprintf(s_keyword, sizeof(s_keyword), "%s", input);
		s_mode = MODE_SEARCH;
		s_page = 1;
		load_list();
	}
}

static void do_login(void) {
	if (bili_logged_in()) {
		bili_logout();
		set_status("已注销", "logged out");
		return;
	}

	char qurl[512], qkey[64];
	if (bili_qr_generate(qurl, sizeof(qurl), qkey, sizeof(qkey)) != 0) {
		set_status("获取二维码失败", "QR generate failed");
		return;
	}

	uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
	uint8_t tmp[qrcodegen_BUFFER_LEN_MAX];
	if (!qrcodegen_encodeText(qurl, tmp, qr, qrcodegen_Ecc_MEDIUM,
	                          1, 10, qrcodegen_Mask_AUTO, true)) {
		set_status("二维码生成失败", "QR encode failed");
		return;
	}

	printf("scan QR to log in, B to cancel\n");
	s_pending_mode = -1;   /* 清掉上次残留,否则会污染本次结果 */
	u64 last_poll = 0;
	while (aptMainLoop()) {
		/* 一帧只能 scan 一次!scan 两次会把触摸的"按下沿"吃掉
		 * (第二次 kDown 已经变 0),表现为下屏按钮完全点不动 */
		hidScanInput();
		u32 kd = hidKeysDown();
		if (kd & KEY_B) { set_status("已取消登录", "login cancelled"); return; }
		touchPosition tp = { 0, 0 };
		bool touched = (kd & KEY_TOUCH) != 0;
		if (touched) hidTouchRead(&tp);

		ui_begin();
		ui_text(96, 8, UI_SHARP, UI_COL_TEXT, "手机 B 站 App 扫码登录");
		ui_qr(qr, 200, 128, 4);
		ui_text(104, 222, UI_SHARP, UI_COL_DIM, "B 取消 / 下屏可直接切换频道");
		/* 下屏就是平时那张主页面(不另做界面):点哪个频道就切哪个,
		 * 高亮跟着走,登录随之取消 */
		ListActions act = { 0 };
		if (!ui_console_active())
			draw_bottom_list(touched, tp.px, tp.py, &act);
		ui_end();
		if (act.popular || act.recommend || act.hist || act.fav) {
			/* 历史/收藏仍需登录,登录未完成时点它们无意义 → 只接受热门/推荐 */
			if (act.popular || act.recommend) {
				s_pending_mode = act.popular ? MODE_POPULAR : MODE_RECOMMEND;
				set_status("已取消登录", "login cancelled");
				return;
			}
		}
		if (act.settings) {   /* 设置在登录页点了也当取消 */
			set_status("已取消登录", "login cancelled");
			return;
		}

		u64 now = osGetTime();
		if (now - last_poll >= 2000) {
			last_poll = now;
			int code = -1;
			if (bili_qr_poll(qkey, &code) == 0) {
				if (code == 0) {
					/* code==0 只说明「扫码这一步过了」,不代表 cookie 拿到手了。
					 * 以前这里无条件报「登录成功」,于是出问题时屏幕说成功、
					 * 顶栏说未登录 —— 提示本身在骗人,白白多查了一轮 */
					if (bili_logged_in()) {
						set_status("登录成功", "login OK");
					} else {
						set_status("登录未生效,请重试", "login failed: cookies rejected");
						printf("qr: code=0 but nav says not logged in\n");
					}
					return;
				}
				if (code == 86038) {
					/* 过期,换一张 */
					if (bili_qr_generate(qurl, sizeof(qurl), qkey, sizeof(qkey)) != 0 ||
					    !qrcodegen_encodeText(qurl, tmp, qr, qrcodegen_Ecc_MEDIUM,
					                          1, 10, qrcodegen_Mask_AUTO, true)) {
						set_status("二维码刷新失败", "QR refresh failed");
						return;
					}
					printf("QR refreshed\n");
				} else if (code == 86090) {
					printf("scanned, confirm on phone\n");
				}
			}
		}
	}
}

/* 阻塞网络操作前调用:立即画一帧,把"正在做什么"刷到状态条上 */
static void busy_frame(const char *msg) {
	snprintf(s_busy, sizeof(s_busy), "%s", msg);
	draw_list();
	if (!ui_console_active()) {
		ListActions dummy = { 0 };
		draw_bottom_list(false, 0, 0, &dummy);
	}
	ui_end();
}

/* 播放器发弹幕时的登录回调:未登录则拉起扫码流程 */
static bool login_cb(void) {
	if (!bili_logged_in())
		do_login();
	/* 播放器内登录时若在登录页点了频道,这里没人消费会留成陈旧状态,
	 * 下次点历史/收藏就会莫名跳到那个频道(高亮也就"错位")→ 直接丢弃 */
	s_pending_mode = -1;
	return bili_logged_in();
}

static void play_selected(void) {
	if (s_sel >= s_count) return;
	BiliVideo *v = &s_list[s_sel];

	/* 第一件事:停掉封面下载线程。
	 * 它们是唯一绕过 net 串行锁的请求(net_get_img),留着的话
	 * 接下来的 取cid/字幕/弹幕/取流 会与两条封面线程五路并发,
	 * 3DS httpc 在这种压力下会把响应张冠李戴 —— 实测症状就是
	 * "第一次进某视频字幕是别的视频的,退出等一会儿再进就正常"
	 * (等的那会儿封面刚好下完,并发消失) */
	thumb_stop();

	if (v->cid == 0) {
		set_status("获取视频信息...", "fetching cid...");
		busy_frame("获取视频信息...");
		if (bili_get_cid(v->bvid, &v->cid, &v->aid) != 0) {
			/* 注意:不要在这里自动移除条目!曾经这么干过,结果正常视频
			 * 也被误杀(接口对 3DS 的请求风控性 404,视频本身是好的)。
			 * 只显示原因,让用户自己决定 */
			const char *why = bili_last_error();
			char msg[128];
			snprintf(msg, sizeof(msg), "无法播放:%s",
			         (why && why[0]) ? why : "获取 cid 失败");
			set_status(msg, "get cid failed (see console)");
			snprintf(s_busy, sizeof(s_busy), "%s", msg);  /* 状态条留原因 */
			return;
		}
	}
	set_status("解析播放地址...", "resolving play url...");
	busy_frame("解析播放地址...");
	ui_trace("resolving playurl qn=%d", g_qn);
	char url[2048];
	/* 弹幕先起跑:取流那几个 API 往返是等延迟、不吃带宽的,
	 * 这段时间正好给弹幕下载用,能省下一两秒 */
	if (g_danmaku)
		dm_load_async(v->cid);
	/* 字幕不在这里拉:此刻正与 取流/弹幕 并发,3DS httpc 在多路并发下
	 * 会把响应张冠李戴(实测:字幕内容是别的视频的)。改为播放真正开始、
	 * 其它请求都收摊之后,由播放器单独去拉(player.c 里 sub_kicked) */
	sub_free();   /* 先清干净,杜绝上一个视频的残留 */
	int used_qn = g_qn;
	int r = bili_get_play_url(v->bvid, v->cid, g_qn, url, sizeof(url));
	/* 带上原因:光看 r=-1 分不出「没发请求」「请求失败」「接口拒绝」,
	 * 而这三者的修法完全不同 */
	ui_trace("playurl r=%d err=%s", r, bili_last_error());
	if (r != 0 && g_qn != 16) { /* 高清晰度拿不到就回落 360P */
		printf("qn=%d failed, fallback to 360P\n", g_qn);
		r = bili_get_play_url(v->bvid, v->cid, 16, url, sizeof(url));
		used_qn = 16;
	}
	if (r != 0) {
		{
			const char *why = bili_last_error();
			char msg[128];
			snprintf(msg, sizeof(msg), "取流失败:%s",
			         (why && why[0]) ? why : "可能需登录或视频受限");
			set_status(msg, "playurl failed");
			snprintf(s_busy, sizeof(s_busy), "%s", msg);
		}
		dm_free();   /* 弹幕已经起跑了,不播就得把线程收掉 */
		sub_free();
		return;
	}
	player_set_meta(v->aid, v->cid, v->bvid);
	player_set_prefs(g_danmaku, g_force_sw, used_qn);
	s_busy[0] = 0;
	player_play(url, v->title);
	ui_trace_sync("exit-path: dm_free");
	dm_free();
	ui_trace_sync("exit-path: sub_free");
	sub_free();
	/* 【系统正在关我们时别再启动封面下载】否则刚被掐掉的两个 loader 线程
	 * 立刻又被拉起来,接着 thumb_exit 还得把它们收一遍 —— 白白拖长
	 * "Closing software"。 */
	if (s_count > 0 && !net_is_shutting_down()) thumb_start(s_list, s_count);
	ui_trace_sync("exit-path: play_video done");
}

/* APT 事件回调(HOME/睡眠/退出)。只做置标志这类轻活,别在回调里干重活 */
static void apt_hook_cb(APT_HookType hook, void *param) {
	(void)param;
	if (hook == APTHOOK_ONSUSPEND || hook == APTHOOK_ONSLEEP) {
		player_notify_suspend();
		/* 封面下载也停一下并掐掉在途那个,否则按 HOME 要等它跑完 */
		thumb_notify_suspend(1);
	} else if (hook == APTHOOK_ONRESTORE || hook == APTHOOK_ONWAKEUP) {
		thumb_notify_suspend(0);
	} else if (hook == APTHOOK_ONEXIT) {
		/* 【HOME → X 卡在 "Closing software" 的解法】
		 * 系统要关掉我们,而 aptMainLoop() 只有主循环转起来才会返回 false。
		 * 主线程要是正卡在一个同步 HTTP 请求里(列表、播放地址都是主线程
		 * 发的,httpc 没有超时),主循环就回不来,系统只能一直等。
		 * 所以在这里把网络整个掐掉并封死:在途的立刻失败,之后的一律拒绝,
		 * 主线程于是很快回到循环、看到 false、走正常清理。
		 * 这一步不可逆 —— 无所谓,ONEXIT 之后不会再回来了。 */
		net_shutdown_begin();
		thumb_notify_suspend(1);
		player_notify_suspend();
	}
}

int main(void) {
	/* 界面本来就该跑在满速上,不必等到播视频才提。Old3DS 上是空操作。
	 *
	 * 【别顺手在这儿加 APT_SetAppCpuTimeLimit】曾经加过,理由是「启动后
	 * 到第一次播放之前界面很卡」。后来实测证明**那个理由是错的** ——
	 * 用固定计算量了一把,播放前后耗时完全一样,主频从头到尾没变过。
	 * 真凶是封面缓存扫描线程在猛敲文件系统(见 thumb.c)。
	 * 而 SetAppCpuTimeLimit 是给「应用要用系统核」用的,常驻着反而可能
	 * 挤占 HOME 菜单所在的那个核心 —— 它留在 play_stream 里就够了。 */
	osSetSpeedupEnable(true);
	ui_init();
	/* HOME/睡眠钩子:挂起时暂停播放。不挂这个钩子的话,按 HOME 后
	 * 下载和解码线程照跑,HOME 菜单卡、回来更卡 */
	{
		static aptHookCookie s_apt_cookie;
		aptHook(&s_apt_cookie, apt_hook_cb, NULL);
	}
	/* 线性内存总量在这里报一次。MVD 工作缓冲、视频输出、音频缓冲全从这里出,
	 * "linear alloc failed" 时先回头看这个数就知道是天生不够还是被吃光了。
	 * 注:.3dsx 走 Homebrew Launcher 拿到的内存通常比装成 .cia 少不少 */
	printf("linear heap free at boot: %luKB\n",
	       (unsigned long)(linearSpaceFree() / 1024));
	/* 普通堆探针:从 16MB 往下折半试 malloc,量出最大可分配块。
	 * "字体 8MB 装不装得下、ffmpeg 够不够用"直接看这个数 */
	{
		size_t probe = 16 * 1024 * 1024;
		void *pp = NULL;
		while (probe >= 64 * 1024 && !(pp = malloc(probe))) probe /= 2;
		if (pp) free(pp);
		ui_trace("boot: linear=%luKB heap-maxblock=%luKB",
		         (unsigned long)(linearSpaceFree() / 1024),
		         (unsigned long)(pp ? probe / 1024 : 0));
	}
	ime_init();   /* 加载拼音词库(约 1MB,失败则输入退化为英文) */
	player_set_login_cb(login_cb);

	/* 机型默认:New3DS 480P,老 3DS 360P + 软解(本就无 MVD) */
	{
		bool n3 = false;
		APT_CheckNew3DS(&n3);
		g_qn = n3 ? 32 : 16;
	}
	/* 存档覆盖默认值。顺序要在机型判断**之后**:清晰度的默认因机型而异,
	 * settings_get 的 def 参数要拿到正确的机型默认 */
	settings_init();
	g_danmaku = settings_get("danmaku", g_danmaku ? 1 : 0) != 0;
	g_qn      = (settings_get("qn", g_qn) == 32) ? 32 : 16;
	g_force_sw = settings_get("force_sw", g_force_sw ? 1 : 0) != 0;
	player_prefs_init();   /* 字幕开关/弹幕字号/字幕字号(存在 player.c) */

	printf("3Danmu\ninit network...\n");
	if (R_FAILED(net_init())) {
		printf("httpc init failed\n");
	} else {
		bili_init();
		printf("%s\n", bili_logged_in() ? "logged in" : "not logged in (press X)");
	}

	load_list();
	ui_bottom_debug(false); /* 启动完成,切到操作提示面板 */


	while (aptMainLoop()) {
		hidScanInput();
		u32 kDown = hidKeysDown();
		(void)0; /* kRepeat 已弃用:上下键改为 单击+长按连续滚动 */
		touchPosition tp = { 0, 0 };
		bool touched = (kDown & KEY_TOUCH) != 0;
		if (touched) hidTouchRead(&tp);

		/* 日志页只用触摸:拖动滚动、双击退出(都在 ui_draw_log 里)。
		 * 方向键/摇杆一律不接管 —— 它们在列表页是选视频用的,
		 * 让同一个键在两个场景做不同的事只会误触 */

		if (kDown & KEY_B) {
			if (s_in_settings) {
				s_in_settings = false;
			} else if (s_mode != MODE_RECOMMEND) {
				/* 从搜索/热门/历史/收藏(含失败)返回默认的推荐页 */
				s_mode = MODE_RECOMMEND;
				s_hl_mode = -1;
				s_page = 1;
				load_list();
			}
		}
		if ((kDown & KEY_START) &&
		    !(hidKeysHeld() & KEY_SELECT)) /* START+SELECT 是强制软解组合键 */
			break;
		if (!ui_console_active())
		{	/* 选择与滚动。
			 * 十字键:单击移一行(目标缓动滑入);按住超过 300ms 进入
			 * "连续滚动"——视口匀速走、选中项贴边跟随,而不是逐行连发。
			 * 逐行连发即使做了缓动也是节奏性的"平滑跳",体验仍是跳。
			 * 调试台开着时整块跳过:那会儿方向键/摇杆不该动任何东西,
			 * 日志页只用触摸(拖动滚动、双击退出) */
			static u64 hold_t0 = 0;
			if (kDown & KEY_DOWN && s_sel + 1 < s_count) {
				s_sel++;
				hold_t0 = osGetTime();
				float bot = (s_sel + 1) * ROW_H;
				if (bot > s_scroll_t + LIST_H) s_scroll_t = bot - LIST_H;
			}
			if (kDown & KEY_UP && s_sel > 0) {
				s_sel--;
				hold_t0 = osGetTime();
				float top = s_sel * ROW_H;
				if (top < s_scroll_t) s_scroll_t = top;
			}
			u32 held = hidKeysHeld();
			bool long_hold = (held & (KEY_UP | KEY_DOWN)) &&
			                 osGetTime() - hold_t0 > 300;
			if (long_hold && (held & KEY_DOWN)) {
				s_scroll_t += 4.5f;               /* 匀速下滚 */
				int last = (int)((s_scroll_t + LIST_H) / ROW_H) - 1;
				if (last >= s_count) last = s_count - 1;
				if (last > s_sel) s_sel = last;   /* 选中项贴下边缘跟随 */
			}
			if (long_hold && (held & KEY_UP)) {
				s_scroll_t -= 4.5f;               /* 匀速上滚 */
				int firstv = (int)((s_scroll_t + ROW_H - 1) / ROW_H);
				if (firstv < 0) firstv = 0;
				if (firstv < s_sel) s_sel = firstv;  /* 贴上边缘跟随 */
			}
			/* 左摇杆:匀速滚动,不动选中项 */
			circlePosition cp;
			hidCircleRead(&cp);
			if (cp.dy > 24 || cp.dy < -24)
				s_scroll_t -= (float)cp.dy * 0.055f;
			/* 夹取 + 缓动 */
			float maxs = s_count * ROW_H - LIST_H;
			if (maxs < 0) maxs = 0;
			if (s_scroll_t < 0) s_scroll_t = 0;
			if (s_scroll_t > maxs) s_scroll_t = maxs;
			s_scroll += (s_scroll_t - s_scroll) * 0.35f;
			if (s_scroll - s_scroll_t < 0.5f && s_scroll_t - s_scroll < 0.5f)
				s_scroll = s_scroll_t;
		}
		if (kDown & KEY_R) { s_page++; load_list(); }
		if (kDown & KEY_L && s_page > 1) { s_page--; load_list(); }
		if ((kDown & (KEY_ZL | KEY_ZR)) ||
		    ((kDown & KEY_SELECT) && !(hidKeysHeld() & KEY_START))) {
			/* 频道循环只含 热门↔推荐;历史/收藏走下屏独立按钮。
			 * SELECT 与 ZL/ZR 同功能,照顾没有 ZL/ZR 的老机型 */
			s_mode = (s_mode == MODE_POPULAR) ? MODE_RECOMMEND : MODE_POPULAR;
			s_hl_mode = -1;
			s_page = 1;
			load_list();
		}
		if (kDown & KEY_Y) do_search();
		if (kDown & KEY_X) do_login();
		if (kDown & KEY_A) {
			if (s_count == 0) {   /* 列表为空(如启动时没网):A = 重试 */
				printf("retrying...\n");
				bili_init();
				load_list();
			} else {
				play_selected();
				/* 【播放中 HOME→X 卡死的最后一块】播放器是从主循环这一格
				 * 里调进去的。被系统关闭时,播放器自己退得很干净(实测
				 * 全链路 150ms),但返回后**本轮循环还剩大半格没走完** ——
				 * 接下来会照常画一帧列表页,ui_end 里要等 VBlank,而退出
				 * 状态下 GSP 的 VBlank 事件可能永远不来,主线程就吊死在
				 * 那里,永远轮不到下一次 aptMainLoop() 返回 false。
				 * (日志指纹:最后一行是 exit-path: play_video done,
				 * 而 exit: thumb_exit 永远没出现。)
				 * 所以这里直接跳出主循环,别再画这最后一帧。 */
				if (net_is_shutting_down() || aptShouldClose()) {
					/* aptShouldClose 直接问 APT,不吃钩子竞态的亏 */
					net_shutdown_begin();
					ui_trace_sync("exit: bail after playback");
					break;
				}
			}
		}

		/* 同样的兜底给搜索/登录:它们内部也各有一个 aptMainLoop 子循环,
		 * 被关闭时从里面返回后,剩下的这半格循环同样不能再画帧。 */
		if (net_is_shutting_down() || aptShouldClose()) {
			net_shutdown_begin();
			ui_trace_sync("exit: bail before draw");
			break;
		}

		if (s_in_settings) {
			ui_begin();
			draw_top_settings();
			if (ui_console_active()) {
				touchPosition th;
				hidTouchRead(&th);
				bool held = (hidKeysHeld() & KEY_TOUCH) != 0;
				if (ui_draw_log(touched, held, th.px, th.py)) {
					s_debug_ui = false;
					ui_bottom_debug(false);
				}
			} else {
				draw_bottom_settings(touched, tp.px, tp.py);
			}
			ui_end();
			if (s_debug_ui && !ui_console_active())
				ui_bottom_debug(true);
			if (kDown & KEY_B) s_in_settings = false;
			continue;
		}

		ListActions act = { 0 };
		draw_list();
		if (ui_console_active()) {
			touchPosition th;
			hidTouchRead(&th);
			bool held = (hidKeysHeld() & KEY_TOUCH) != 0;
			if (ui_draw_log(touched, held, th.px, th.py)) {
				s_debug_ui = false;
				ui_bottom_debug(false);
			}
		} else {
			draw_bottom_list(touched, tp.px, tp.py, &act);
		}
		ui_end();

		if (act.login) {
			do_login();
			if (s_pending_mode >= 0) {
				s_mode = (ListMode)s_pending_mode;
				s_pending_mode = -1;
				s_page = 1;
				load_list();
			}
		}
		if (act.settings) s_in_settings = true;
		if (act.popular) {
			s_hl_mode = -1;
			if (s_mode != MODE_POPULAR) { s_mode = MODE_POPULAR; s_page = 1; load_list(); }
		}
		if (act.recommend) {
			s_hl_mode = -1;
			if (s_mode != MODE_RECOMMEND) { s_mode = MODE_RECOMMEND; s_page = 1; load_list(); }
		}
		if (act.hist || act.fav) {
			ListMode want = act.hist ? MODE_HISTORY : MODE_FAV;
			if (!bili_logged_in())
				do_login();          /* 未登录:直接拉起扫码 */
			if (s_pending_mode >= 0) {           /* 登录界面里改去别的频道 */
				s_mode = (ListMode)s_pending_mode;
				s_pending_mode = -1;
				s_page = 1;
				load_list();
			} else if (bili_logged_in()) {       /* 登录成功才切过去 */
				s_mode = want;
				s_page = 1;
				load_list();
			}
			s_hl_mode = -1;   /* 结束:高亮回落到真实频道 */
		}
	}

	/* 逐步落盘:万一还是卡在 "Closing software",trace.log 的最后一行
	 * 就指出卡在哪一步。异步日志在进程被杀时来不及写,所以用同步版。 */
	ui_trace_sync("exit: thumb_exit");
	thumb_exit();
	ui_trace_sync("exit: net_exit");
	net_exit();
	/* 放在 net_exit 之后:tls 只在扫码登录时用,退出时它一定是空闲的,
	 * 不会像 httpc 那样有请求卡在里面 */
	ui_trace_sync("exit: tls_exit");
	tls_exit();
	ui_trace_sync("exit: ime_exit");
	ime_exit();
	ui_trace_sync("exit: ui_exit");
	ui_exit();
	return 0;
}
