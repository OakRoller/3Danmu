/* 触屏拼音输入法(实现说明见 ime.h)
 *
 * 词库检索:条目已按 (拼音升序, 词频降序) 排好,
 * 前缀匹配 = 二分找区间起点后顺序扫描,天然按可用性排序。
 * 候选构成:
 *   1) 拼音以整个输入串开头的词(完整消耗输入)
 *   2) 拼音恰等于输入前 L 个字母的词,L 从长到短(部分消耗,剩余继续拼)
 */
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ime.h"
#include "ui.h"

#define PY_MAX     24     /* 拼音缓冲上限 */
#define CAND_MAX   72     /* 一次检索的候选上限(64 * 8B 在栈上,无压力) */
#define CAND_PAGE  8      /* 每页最多画几个(实际能画几个由宽度决定) */
#define OUT_MAX    120

/* ---------- 词库 ---------- */

static u8  *s_blob = NULL;
static u32 *s_off  = NULL;
static u32  s_count = 0;

static const char *ent_py(u32 i, int *len) {
	const u8 *e = s_blob + s_off[i];
	*len = e[0];
	return (const char *)e + 2;
}
static const char *ent_word(u32 i, int *len) {
	const u8 *e = s_blob + s_off[i];
	*len = e[1];
	return (const char *)e + 2 + e[0];
}

bool ime_init(void) {
	if (s_blob) return true;
	FILE *f = fopen("romfs:/pinyin.dic", "rb");
	if (!f) { printf("ime: no pinyin.dic\n"); return false; }
	char magic[4];
	if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "PYD1", 4)) {
		fclose(f);
		return false;
	}
	if (fread(&s_count, 4, 1, f) != 1 || s_count == 0 || s_count > 200000) {
		fclose(f);
		return false;
	}
	s_off = (u32 *)malloc(s_count * 4);
	if (!s_off || fread(s_off, 4, s_count, f) != s_count) goto fail;
	long pos = ftell(f);
	fseek(f, 0, SEEK_END);
	long blob_sz = ftell(f) - pos;
	fseek(f, pos, SEEK_SET);
	s_blob = (u8 *)malloc((size_t)blob_sz);
	if (!s_blob || fread(s_blob, 1, (size_t)blob_sz, f) != (size_t)blob_sz)
		goto fail;
	fclose(f);
	printf("ime: %lu entries\n", (unsigned long)s_count);
	return true;
fail:
	fclose(f);
	free(s_off); s_off = NULL;
	free(s_blob); s_blob = NULL;
	printf("ime: dict load failed\n");
	return false;
}

void ime_exit(void) {
	free(s_off); s_off = NULL;
	free(s_blob); s_blob = NULL;
	s_count = 0;
}

/* 首个 pinyin >= key 的下标(二分) */
static u32 lower_bound(const char *key, int keylen) {
	u32 lo = 0, hi = s_count;
	while (lo < hi) {
		u32 mid = (lo + hi) / 2;
		int pl;
		const char *p = ent_py(mid, &pl);
		int c = memcmp(p, key, (size_t)(pl < keylen ? pl : keylen));
		if (c < 0 || (c == 0 && pl < keylen)) lo = mid + 1;
		else hi = mid;
	}
	return lo;
}

typedef struct {
	u32 idx;          /* 词库下标 */
	u8  consume;      /* 选中后消耗的拼音字母数 */
} Cand;

static int collect(const char *py, int pylen, Cand *out) {
	int n = 0;
	if (!s_blob || pylen <= 0) return 0;
	/* 1) 整串前缀匹配。留给第 2 阶段的名额要够:pylen 每多一个字母,
	 * 第 2 阶段就多试一档 L,名额不足时长拼音的首字候选会被挤没。 */
	int reserve = 24;
	for (u32 i = lower_bound(py, pylen); i < s_count && n < CAND_MAX - reserve; i++) {
		int pl;
		const char *p = ent_py(i, &pl);
		if (pl < pylen || memcmp(p, py, (size_t)pylen) != 0) break;
		out[n].idx = i;
		out[n].consume = (u8)pylen;
		n++;
	}
	/* 2) 部分匹配:L 从长到短,只收拼音恰等于前缀的。
	 *
	 * 【每档配额不能一律给 4】原来写死 got < 4,于是输入多音节拼音时
	 * (比如 "nihao" 而词库里没这个词),第一个字的候选只剩 4 个 ——
	 * 用户看到的就是「拼音一长,首字反而选不着了」。
	 * 而**第一个真正匹配上的 L 就是用户最可能想要的那一档**
	 * (最长的完整音节前缀),它该拿走大部分名额;更短的档次纯属兜底。
	 * 所以:首个有结果的档给 16 个,之后每档 3 个。 */
	bool first_hit = true;
	for (int L = pylen - 1; L >= 1 && n < CAND_MAX; L--) {
		int got = 0;
		int quota = first_hit ? 16 : 3;
		for (u32 i = lower_bound(py, L); i < s_count && got < quota && n < CAND_MAX; i++) {
			int pl;
			const char *p = ent_py(i, &pl);
			if (pl != L || memcmp(p, py, (size_t)L) != 0) break;
			out[n].idx = i;
			out[n].consume = (u8)L;
			n++; got++;
		}
		if (got) first_hit = false;
	}
	return n;
}

/* ---------- 键盘 UI ---------- */

typedef struct { float x, y, w, h; char label[8]; char ch; } Key;

/* 键面(下屏 320x240):数字排常驻顶部,下面三排 字母/符号 可切换 */
#define KEY_H 32.0f             /* 键高(小键对触屏校准偏移最敏感,尽量大) */
/* 列间距 31.7 但键宽只给 30:留 1.7px 缝隙。
 * 缝隙若小于 1px(比如宽 31),GPU 取整后有些相邻键会贴在一起看不见缝 */
#define KEY_DX 31.7f            /* 列间距 */
#define KEY_W  30.0f
#define KEY_Y0 68.0f            /* 数字排 y */
#define KEY_ROW 34.0f           /* 行间距 */
#define KEY_YL (KEY_Y0 + KEY_ROW)     /* 字母/符号区起始 y */
static Key s_numkeys[12];
static int s_nnum = 0;
static Key s_keys[32];      /* 字母键面(中/英共用) */
static int s_nkeys = 0;
static Key s_symkeys[32];   /* 符号键面 */
static int s_nsym = 0;

static void add_key_to(Key *arr, int *n, float x, float y, float w,
                       char ch) {
	Key *k = &arr[(*n)++];
	k->x = x; k->y = y; k->w = w; k->h = KEY_H;
	k->label[0] = ch; k->label[1] = 0;
	k->ch = ch;
}

static void build_row(Key *arr, int *n, const char *chars, float x0, float y) {
	for (int i = 0; chars[i]; i++)
		add_key_to(arr, n, x0 + i * KEY_DX, y, KEY_W, chars[i]);
}

static void build_keys(void) {
	if (s_nkeys) return;
	build_row(s_numkeys, &s_nnum, "1234567890", 2, KEY_Y0);
	build_row(s_keys, &s_nkeys, "qwertyuiop", 2, KEY_YL);
	build_row(s_keys, &s_nkeys, "asdfghjkl", 18, KEY_YL + KEY_ROW);
	build_row(s_keys, &s_nkeys, "zxcvbnm", 34, KEY_YL + 2 * KEY_ROW);
	/* 符号键面(替换字母区三排) */
	build_row(s_symkeys, &s_nsym, "-_.,:;!?@~", 2, KEY_YL);
	build_row(s_symkeys, &s_nsym, "#%&*()+=/\\", 2, KEY_YL + KEY_ROW);
	build_row(s_symkeys, &s_nsym, "'\"<>[]{}|^", 2, KEY_YL + 2 * KEY_ROW);
}

/* 触点 → 键字符:严格按每个键的真实矩形判定(绘制用的就是这套矩形)。
 * 曾用"网格换算"做命中,但 (tx-x0)/DX 对键左侧空白会算出负数并被截断成
 * 第 0 列 —— 于是 z/a/q 左边的空白也被判成 z/a/q。严格矩形不会外溢:
 * 点在键与键之间的 1.7px 缝隙上直接不响应(缝隙比笔尖细,无感) */
static char key_at(float tx, float ty, bool sym) {
	const Key *sets[2];
	int counts[2];
	sets[0] = s_numkeys;              counts[0] = s_nnum;
	sets[1] = sym ? s_symkeys : s_keys;
	counts[1] = sym ? s_nsym : s_nkeys;
	for (int g = 0; g < 2; g++) {
		for (int i = 0; i < counts[g]; i++) {
			const Key *k = &sets[g][i];
			if (tx >= k->x && tx < k->x + k->w &&
			    ty >= k->y && ty < k->y + k->h)
				return k->ch;
		}
	}
	return 0;
}

/* utf8 尾删一个字符 */
static void utf8_pop(char *s) {
	size_t n = strlen(s);
	while (n > 0 && (s[--n] & 0xC0) == 0x80) ;
	s[n] = 0;
}

bool ime_input(const char *hint, const char *initial, char *out, size_t outlen) {
	build_keys();
	bool has_dict = (s_blob != NULL);
	bool en = !has_dict;   /* 中/英(只影响字母区) */
	bool sym = false;      /* 符:字母区切成符号键面 */
	#define IS_CN (!en && !sym)
	char text[OUT_MAX] = {0};        /* 已上屏文本 */
	char py[PY_MAX + 1] = {0};       /* 拼音组合区 */
	int pylen = 0;
	Cand cands[CAND_MAX];
	int ncand = 0;
	/* 【翻页不能用 页码 x 每页条数】候选按钮宽度随词长变化,一页能塞几个
	 * 是画的时候才知道的。按固定步长翻页,遇到宽词(那页只画得下 3 个)
	 * 就会把剩下的 5 个直接跳过 —— 它们永远选不到。
	 * 所以记「本页起点」和「本页实际画了几个」,下一页从起点+已画开始。
	 * 往回翻需要历史起点,存一个小栈(候选最多 72 个,32 层够深)。 */
	int cbase = 0;          /* 本页第一个候选的下标 */
	int cshown = 0;         /* 本页实际画出来的个数(绘制时填) */
	int cstack[32], cdepth = 0;
	if (initial) snprintf(text, sizeof(text), "%s", initial);
	bool confirmed = false, done = false;

	while (aptMainLoop() && !done) {
		hidScanInput();
		u32 kDown = hidKeysDown();
		u32 kHeld = hidKeysHeld();
		touchPosition tp;
		hidTouchRead(&tp);
		bool touched = (kDown & KEY_TOUCH) != 0;
		float tx = tp.px, ty = tp.py;

		/* ---- 实体键 ---- */
		if (kDown & KEY_B) {
			if (pylen > 0) { pylen = 0; py[0] = 0; ncand = 0; cbase = cdepth = cshown = 0; }
			else { done = true; }                    /* 无拼音时 B = 取消 */
		}
		if (kDown & KEY_START) { confirmed = true; done = true; }
		if ((kDown & KEY_L) && cdepth > 0) cbase = cstack[--cdepth];
		if ((kDown & KEY_R) && cshown > 0 && cbase + cshown < ncand) {
			if (cdepth < (int)(sizeof(cstack) / sizeof(cstack[0])))
				cstack[cdepth++] = cbase;
			cbase += cshown;
		}
		(void)kHeld;

		/* ---- 绘制 + 触控 ---- */
		ui_begin();
		/* 上屏:提示 + 当前文本(大字预览) */
		ui_text(10, 8, UI_SHARP, UI_COL_DIM, hint ? hint : "输入");
		/* 【这里曾经写死 0.85】当时的理由是:按那版字体的标定(sharp≈0.87)
		 * 0.85 落在吸附窗口里、会被吸到精确 1.0,所以又大又锐。
		 * 但字体后来重转过,UI_SHARP 现在是 0.52 —— 0.85 早就不在窗口里了,
		 * 于是变成 1.6 倍放大,正在输入的字反而是全屏最糊的。
		 * 教训:把「当前标定值」抄成字面量,标定一变它就成了错的,
		 * 而且**不会报错,只会变糊**。跟着 UI_SHARP 走,别再写死。 */
		ui_text(10, 38, UI_SHARP, UI_COL_TEXT, text[0] ? text : "…");
		if (pylen)
			ui_text(10, 84, UI_SHARP, UI_COL_ACCENT, py);
		ui_text(10, 202, UI_SHARP, UI_COL_DIM,
		        IS_CN ? "空格=选第一候选   B 清拼音   L/R 翻候选页"
		              : (sym ? "符号:点按直接上屏" : "英文:字母直接上屏"));
		ui_text(10, 222, UI_SHARP, UI_COL_DIM, "START 或 下屏[完成] 确认输入");

		ui_begin_bottom();
		/* 文本行 */
		ui_rect(0, 0, 320, 28, C2D_Color32(0x26, 0x26, 0x30, 0xFF));
		{
			char line[OUT_MAX + PY_MAX + 2];
			snprintf(line, sizeof(line), "%s%s", text, py);
			ui_text_clipped(6, 2, UI_SHARP, UI_COL_TEXT, line, 308);
		}
		/* 候选栏(仅中文模式) */
		if (IS_CN && ncand > 0) {
			float cx = 24;
			int base = cbase;
			int drawn = 0;
			if (ui_button(0, 28, 22, 34, "<", UI_COL_SEL, touched, tx, ty) &&
			    cdepth > 0) cbase = cstack[--cdepth];
			for (int i = 0; i < CAND_PAGE && base + i < ncand; i++) {
				int wl;
				const char *w = ent_word(cands[base + i].idx, &wl);
				char buf[28];
				int c = wl < 27 ? wl : 27;
				memcpy(buf, w, (size_t)c); buf[c] = 0;
				/* 宽度必须按 ui_button 的实际绘制字号算 —— 它画的是 UI_SHARP。
				 * (这里曾写死 0.7,而按钮早已改成 UI_SHARP:量宽和绘制不是
				 * 同一个数,候选按钮就会忽宽忽窄。) */
				float bw = ui_text_width(buf, UI_SHARP) + 18;
				if (bw < 42) bw = 42;
				if (cx + bw > 296) break;
				if (ui_button(cx, 28, bw, 34, buf, UI_COL_SEL, touched, tx, ty)) {
					/* 选中:上屏 + 消耗拼音 */
					if (strlen(text) + (size_t)c < sizeof(text) - 1)
						strcat(text, buf);
					int cons = cands[base + i].consume;
					memmove(py, py + cons, (size_t)(pylen - cons + 1));
					pylen -= cons;
					ncand = pylen ? collect(py, pylen, cands) : 0;
					cbase = 0; cdepth = 0; cshown = 0;
				}
				cx += bw + 4;
				drawn++;
			}
			cshown = drawn;      /* 给下一帧的 R / > 用 */
			if (ui_button(296, 28, 22, 34, ">", UI_COL_SEL, touched, tx, ty) &&
			    drawn > 0 && base + drawn < ncand) {
				if (cdepth < (int)(sizeof(cstack) / sizeof(cstack[0])))
					cstack[cdepth++] = base;
				cbase = base + drawn;
			}
			/* 【页码提示已去掉】曾经在这里画 "1-6/48"。
			 * 本意是让用户知道后面还有候选,但 < > 两个翻页键本身就说明了
			 * 这件事,而具体数字对「挑一个字」这个动作没有任何用 ——
			 * 它只是占着候选行下方的位置,还多一行要读的东西。 */
		}
		/* 键面自绘:ui_button 的 0.7 字号会从 30px 的小键里溢出去,
		 * 看着就"和触控区对不上"。这里用 0.58 并按住高亮命中键 */
		{
			bool held_touch = (hidKeysHeld() & KEY_TOUCH) != 0;
			char hot = held_touch ? key_at(tx, ty, sym) : 0;
			Key *set = sym ? s_symkeys : s_keys;
			int nset = sym ? s_nsym : s_nkeys;
			for (int pass = 0; pass < 2; pass++) {
				Key *arr = pass ? set : s_numkeys;
				int cnt = pass ? nset : s_nnum;
				for (int i = 0; i < cnt; i++) {
					Key *k = &arr[i];
					bool on = (hot && k->ch == hot);
					ui_rect(k->x, k->y, k->w, k->h,
					        on ? UI_COL_ACCENT : UI_COL_SEL);
					ui_rect(k->x, k->y + k->h - 2, k->w, 2,
					        C2D_Color32(0, 0, 0, 0x60));
					float tw = ui_text_width(k->label, UI_SHARP);
					float th = ui_text_height(UI_SHARP);
					/* 垂直居中按实测字高算,别写死 -13.5:那是给旧字号调的,
					 * 字体一换就偏 */
					ui_text(k->x + (k->w - tw) / 2, k->y + (k->h - th) / 2,
					        UI_SHARP, UI_COL_WHITE, k->label);
				}
			}
		}
		/* 触控命中:按网格直接换算,键与键之间没有死区。
		 * 逐键小矩形命中时,点在 2px 缝隙上会漏,30px 的小键上
		 * 体感就是"按钮和触控区域对不上" */
		if (touched) {
			char ch = key_at(tx, ty, sym);
			if (ch >= '0' && ch <= '9') {
				if (strlen(text) + pylen < sizeof(text) - 2) {
					/* 拼音挂着时先清,避免数字插进语序 */
					if (pylen) { pylen = 0; py[0] = 0; ncand = 0; cbase = cdepth = cshown = 0; }
					size_t n = strlen(text);
					text[n] = ch; text[n + 1] = 0;
				}
			} else if (ch) {
				if (IS_CN && !sym) {
					if (pylen < PY_MAX) {
						py[pylen++] = ch; py[pylen] = 0;
						ncand = collect(py, pylen, cands);
						cbase = cdepth = cshown = 0;
					}
				} else if (strlen(text) < sizeof(text) - 2) {
					size_t n = strlen(text);
					text[n] = ch; text[n + 1] = 0;
				}
			}
		}
		/* 底排功能键:[中/英] [符] [空格] [删除] [完成] */
		{
			float y = KEY_YL + 3 * KEY_ROW;   /* = 204,行高 30 到 234 */
			const float FH = 30.0f;
			if (ui_button(2, y, 48, FH, en ? "英" : "中",
			              UI_COL_ACCENT, touched, tx, ty)) {
				if (has_dict) {          /* 无词库时锁定英文 */
					en = !en;
					pylen = 0; py[0] = 0; ncand = 0; cbase = cdepth = cshown = 0;
				}
			}
			if (ui_button(54, y, 40, FH, "符",
			              sym ? UI_COL_ACCENT : UI_COL_SEL, touched, tx, ty)) {
				sym = !sym;
				pylen = 0; py[0] = 0; ncand = 0; cbase = cdepth = cshown = 0;
			}
			if (ui_button(98, y, 88, FH, "空格", UI_COL_SEL, touched, tx, ty)) {
				if (IS_CN && pylen && ncand) {   /* 中文态空格 = 选第一候选 */
					int wl;
					const char *w = ent_word(cands[0].idx, &wl);
					if (strlen(text) + (size_t)wl < sizeof(text) - 1)
						strncat(text, w, (size_t)wl);
					int cons = cands[0].consume;
					memmove(py, py + cons, (size_t)(pylen - cons + 1));
					pylen -= cons;
					ncand = pylen ? collect(py, pylen, cands) : 0;
					cbase = cdepth = cshown = 0;
				} else if (strlen(text) < sizeof(text) - 2) {
					strcat(text, " ");
				}
			}
			if (ui_button(190, y, 60, FH, "删除", UI_COL_SEL, touched, tx, ty)) {
				if (pylen) {
					py[--pylen] = 0;
					ncand = pylen ? collect(py, pylen, cands) : 0;
					cbase = cdepth = cshown = 0;
				} else if (text[0]) {
					utf8_pop(text);
				}
			}
			if (ui_button(254, y, 64, FH, "完成", UI_COL_ACCENT, touched, tx, ty)) {
				confirmed = true; done = true;
			}
		}
		ui_end();
	}

	/* 确认时拼音区还有残留字母:当英文直接拼上(比如想搜英文词) */
	if (confirmed && pylen && strlen(text) + (size_t)pylen < sizeof(text))
		strcat(text, py);
	#undef IS_CN

	if (confirmed && text[0]) {
		snprintf(out, outlen, "%s", text);
		return true;
	}
	return false;
}
