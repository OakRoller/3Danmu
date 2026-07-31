/* 网络流播放器(双核版)
 *
 * 线程分工:
 *   worker 线程(New3DS core2):拉流 → 解封装 → 视频解码 → 音频解码/NDSP
 *   主线程(core0):输入处理 + 按时呈现帧 + 进度条
 * 帧通过 1 槽邮箱 + 双缓冲传递(单生产者单消费者,volatile + __dmb 同步)。
 *
 * 默认软解(可靠);按住 L 进入播放 → 尝试 MVD 硬解(实验性,尚未完全驯服)。
 */
#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <libavutil/opt.h>

#include "player.h"
#include "net.h"
#include "settings.h"
#include "ui.h"
#include "danmaku.h"
#include "ime.h"
#include "bili.h"
#include "subtitle.h"
#include "comment.h"

#define SCREEN_W 400
#define SCREEN_H 240
#define SAMPLE_RATE 32728
#define AUDIO_NBUFS 24
#define AUDIO_SAMPLES_PER_BUF 2048
#define AVIO_BUF_SIZE (128 * 1024)
/* MVD 输入暂存。一个 AU(一帧的编码数据)在 480P 下几十 KB 顶天,
 * 512KB 已是十倍余量。原来给 1MB,而线性堆本来就紧张 ——
 * 实机见过 "linear alloc failed (need 1944KB, free 152KB)" */
#define VIDEO_IN_BUF (512 * 1024)
/* 视频包队列深度。每个是 av_packet_clone 出来的堆内存(360P 关键帧
 * 十几 KB),96 深最坏要几 MB —— 而它只需要盖住「解复用领先解码」的
 * 那点抖动,真正的抗网络抖动靠 1.5MB 的环形缓冲。降到 48(约 1.5 秒)
 * 省下约一半,内存宽裕了 clone 失败丢包也就跟着少了 */
#define VQ_CAP 48
#define WORKER_STACK (160 * 1024)
#define RING_CAP (1536 * 1024)   /* 网络环形缓冲:360P 约 20+ 秒余量 */
#define DL_STACK (32 * 1024)

#ifndef MVD_STATUS_FRAMEREADY
#define MVD_STATUS_FRAMEREADY 0x17003
#endif
#ifndef MVD_STATUS_INCOMPLETEPROCESSING
#define MVD_STATUS_INCOMPLETEPROCESSING 0x17004
#endif

#define PTS_FIFO_CAP 16   /* 时间戳队列容量 */
/* 允许的最大在途深度。MVD 流水线正常只有 2-3,给到 8 意味着最坏情况下
 * 标签会偏老 8 帧(30fps 约 266ms)且无法自愈;压到 4 把最坏偏差
 * 限制在 133ms,再配合漂移自校正,基本不会跑偏 */
#define PTS_DEPTH    4

/* 上次 mvdstdExit+Init 的时刻。声明必须早于 mvd_start —— 它要初始化 */
static u64 s_mvd_reset_at = 0;

/* ---- worker 秒表(声明必须早于 mvd_start —— 它开播时要清零) ---- */
#define TICK_MS(t) ((double)(t) * 1000.0 / (double)SYSCLOCK_ARM11)
static u64 s_t_mvd, s_t_inval, s_t_copy, s_t_flush;
static int s_t_frames, s_t_calls, s_t_noframe, s_t_reports;
/* 本次播放累计出帧数(不清零)。给首帧看门狗用:
 * s_t_frames 是 prof 的窗口计数,每 150 次调用就归零,分不清
 * 「一直没出过帧」和「刚清过窗口」。 */
static volatile u32 s_frames_total = 0;
static volatile u32 s_calls_total = 0;
static u64 s_t_win0;   /* 本统计窗口的起始时刻(用来算真实出帧率) */

/* 网络环形缓冲:下载线程独占 httpc 连接持续填充,解码侧只读内存。
 * 单生产者(下载线程)单消费者(探测期主线程/播放期 worker),
 * rd/wr 为单调 u32 计数,下标取 % RING_CAP。 */
typedef struct {
	u8 *buf;
	volatile u32 rd, wr;
	volatile u64 base;           /* 计数 0 对应的流内绝对偏移 */
	volatile u64 total;          /* 资源总大小,0=未知 */
	volatile int eof, err, quit;
	volatile u64 seek_target;
	volatile int seek_req;
} NetRing;

typedef struct {
	/* 网络 & 解封装 */
	NetStream ns;
	NetRing ring;
	AVIOContext *avio;
	AVFormatContext *fmt;
	int vstream, astream;
	double fps;

	/* 视频 */
	AVBSFContext *bsf;
	AVCodecContext *vdec;
	struct SwsContext *sws;
	AVFrame *vframe;
	bool use_mvd;
	MVDSTD_Config mvd_cfg;
	u8 *mvd_in;
	/* 同一块内存在「新 FCRAM 窗口」下的虚拟地址,专门给 mvd 用。见 mvd_start */
	u8 *mvd_in_n3;
	bool mvd_first;             /* 首帧需重复送一次 */
	bool mvd_need_hdr;          /* 下一次送包时要把 SPS/PPS 拼在帧前面 */
	bool mvd_skip;              /* 上次 Process 已出帧:先排空内部队列再送新包 */
	s64 pts_fifo[16];           /* 解码顺序的时间戳队列(dts 单调) */
	int pts_head, pts_len;
	int pts_drift;              /* 累计修掉的漂移条目数(诊断用) */
	/* MVD 原始输出双缓冲(行距 = 16 对齐宽)。
	 * 曾经想省 470KB 改成单缓冲(理由:搬运已经挪进 worker 并且是同步的,
	 * MVD 不该在搬运期间回写)——真机上直接翻车:起播后解码器一帧不出,
	 * 现场是 ring 满、vq 满、mb=0。原因是**下一帧的角标要写在
	 * MVD 可能仍在收尾的那块内存上**,标记和 MVD 的写回互相踩,
	 * 出帧判定就此失灵。两面轮换才能保证「正在标记的那面没人碰」。
	 * 470KB 买一个确定性的出帧判定,值。 */
	u8 *mvd_raw[2];
	u32 mvd_wsize;              /* 工作缓冲大小(seek 重开 MVD 用) */
	u8  mvd_sps[256], mvd_pps[256];
	int mvd_sps_len, mvd_pps_len;
	u16 *vout[2];               /* 双缓冲(linear,行距 = tex_w) */
	int back;                   /* worker 正在写的缓冲下标 */
	int vw, vh, ow, oh;
	int src_w, src_h, src_stride;
	C3D_Tex tex;                /* 视频纹理(tex_w×tex_h RGB565,均为 2 的幂) */
	int tex_w, tex_h;           /* tex_w 同时是 vout/上传的行距 */
	Tex3DS_SubTexture subtex;
	bool tex_ok;

	/* 音频(worker 线程独占) */
	AVCodecContext *adec;
	SwrContext *swr;
	AVFrame *aframe;
	ndspWaveBuf wbuf[AUDIO_NBUFS];
	s16 *abuf;
	int next_wbuf;
	s16 pending[AUDIO_SAMPLES_PER_BUF * 2 * 4];
	int pending_n;
	u64 samples_done;
	bool audio_ok;              /* 整条音频链路可用(解码器 + 重采样 + NDSP) */
	/* 【和 audio_ok 分开】只表示"ndspInit 成功过,欠一次 ndspExit"。
	 * 清理的判据必须是"我有没有拿到它",不能是"整件事成没成" ——
	 * 两者不等价时就会漏释放,见 audio_exit 的说明 */
	bool ndsp_ok;
	/* 没声音时给用户看的原因。**必须区分**「这台机器缺 dspfirm」和
	 * 「这个视频本来就没音轨」——前者要用户去导一次固件,后者什么都不用做。
	 * 都显示成"无声音"的话,用户只会以为程序坏了。 */
	char audio_err[56];
	u64 start_ms, pause_t0;

	double duration;
	double cur_pts;

	/* 线程通信(volatile,单生产者单消费者) */
	volatile int quit;          /* 主线程 → worker:退出 */
	volatile int pause;         /* 主线程 → worker:暂停请求 */
	volatile int worker_done;   /* worker → 主线程:已结束 */
	volatile u32 clock_ms;      /* worker 发布的播放时钟(毫秒) */
	volatile int mb_full;       /* 邮箱:有帧待呈现 */
	volatile u32 mb_pts_ms;     /* 邮箱:帧时间戳(毫秒) */
	volatile int mb_buf;        /* 邮箱:帧所在缓冲下标 */
	volatile u32 mb_gen;        /* 邮箱:帧所属的 seek 代数 */
	volatile u32 seek_gen;      /* 每次 seek +1:旧代的帧一律不上屏 */
	volatile int ret;           /* worker 结果:0 正常 / -99 MVD 失效 / <0 错误 */
	volatile int dbg_vq;        /* 调试:视频包队列长度 */
	volatile int dbg_eof;       /* 调试:解复用已到片尾(卡顿探针要排除片尾)*/
	volatile u32 dbg_decoded;   /* 调试:已解码帧数 */
	volatile int sync_mode;     /* 0=流畅优先(少跳帧) 1=同步优先(音画对齐) */
	volatile int buffering;     /* 数据饥饿:时钟冻结,攒够缓冲再恢复 */
	volatile int net_stall;     /* 下载线程正在断线重连(值=第几次;0=正常) */
	volatile int seek_req;      /* 主线程 → worker:请求跳转 */
	volatile double seek_to;    /* 跳转目标(秒) */
	volatile double disp_pts;   /* 拖动时显示用的位置(秒) */
	bool clock_resync;          /* seek 后用首个音频帧 pts 校准时钟 */
	double seek_skip;           /* 精确 seek:丢弃此时刻之前的音视频帧(0=无) */
	/* 解码器**原地热切换**(不重开整个播放,不丢失进度)。
	 * 只在 seek 处理里执行 —— 那里本来就要冲刷解复用/解码器/音频队列,
	 * 画面也本来就要中断一下,是切换的唯一安全时机。
	 * 0=不切 1=切软解 2=切回硬解 */
	volatile int dec_switch;
	u64 sw_since;               /* 降级到软解的时刻(0=没降级过) */
	int hw_retried;             /* 本次播放已经试过切回硬解(只试一次) */
	/* 硬解后台试运行:软解照常出画,同时把同一批包也喂给 MVD,
	 * 确认它连续出帧了再把显示源切过去 —— 用户看不到任何切换过程。
	 * MVD 是硬件模块、软解是纯 CPU 的 ffmpeg,两者互不影响,可以并存 */
	int hw_trial;               /* 1 = 正在后台试 */
	int hw_trial_frames;        /* 试运行期间 MVD 出了几帧 */
	int hw_trial_pkts;          /* 试运行喂了几个包(超了还不出帧就放弃) */
	int mvd_trial_noblit;       /* 试运行期间不搬运像素:只要知道出没出帧 */
	int mvd_inited;             /* mvdstdInit 已成功(与 use_mvd 无关) */
	u64 osd_until;              /* 上屏角标显示截止时间(仅主线程使用) */
} Player;

static Player s_player;
/* 硬解降级是**每个视频重新判定**的,不是一降到底。
 * s_disable_mvd 只在「本次播放(含它的软解重试)」内有效,换个视频就清零 ——
 * MVD 卡住往往是这一条流/这一次的偶发状况,没理由让后面所有视频都陪着吃软解。
 * 但连续失败就别再试了:每次重试都要 mvdstdInit 一次,
 * 而反复初始化一个已经不正常的系统模块正是把它彻底搞崩的路径。 */
static bool s_disable_mvd = false;
static int  s_mvd_fail_streak = 0;   /* 连续几个视频硬解失败 */
#define MVD_FAIL_GIVEUP 3            /* 连续这么多次就本次运行不再试硬解 */
static int s_mvd_dbg = 0;
static bool s_pref_danmaku = true;
static bool s_pref_force_sw = false;
/* HOME 挂起请求(APT 钩子置位,渲染循环消费)。
 * 【为什么必须有】按 HOME 后应用被挂起,但**我们的线程不会自动停**:
 * 下载线程还在满速拉流、解码线程还在跑 —— Wi-Fi 和系统服务被占着,
 * HOME 菜单卡、回来之后主界面也卡(积压的包要消化)。
 * 挂起时把播放暂停,下载环满了自然停,回来由用户自己按继续。 */
static volatile int s_suspend_req = 0;
void player_notify_suspend(void) { s_suspend_req = 1; }
static int s_pref_3d = 0;           /* 裸眼 3D:0=关 1=开 */
static char s_cur_title[160];

static int s_cur_qn = 16;
static int64_t s_meta_aid = 0, s_meta_cid = 0;
static char s_meta_bvid[16] = "";
static u32 s_player_clock_ms = 0;   /* 供退出时补报进度 */
static char s_toast[128] = "";      /* 上屏浮层提示(发弹幕结果等) */
static u64  s_toast_until = 0;
static bool (*s_login_cb)(void) = NULL;

/* ---------- 分 P ----------
 * 【为什么选集在播放器**里面**】
 * 第一版把它做成开播前的独立一页(在 main.c),理由是不想往播放器主循环
 * 里再塞一个带滚动的列表 —— 那里已经有五个子页面共用同一套触控和退出路径。
 * 但那样一来,选集时上屏没有画面可留(播放器已退出、纹理已释放),
 * 而「换一集」这个动作本来就发生在看片当中,上屏理应停在暂停的那一帧。
 *
 * 所以改成和评论区同样的子页面:上屏视频保持暂停,下屏整个换成列表。
 * 播放器仍然不碰分P 的数据 —— 标签和时长由 main.c 传进来,
 * 选中后只回一个下标,重新取流还是 main.c 的事。 */
static const char *const *s_pg_labels = NULL;
static const int         *s_pg_durs   = NULL;
static int  s_pg_n   = 0;      /* 共几 P(<=1 不显示选集) */
static int  s_pg_cur = 0;      /* 当前是第几 P(下标) */
static int  s_page_pick = -1;  /* 用户选中的下标;-1 = 没选 */

void player_set_pages(const char *const *labels, const int *durations,
                      int n, int cur) {
	s_pg_labels = labels;
	s_pg_durs   = durations;
	s_pg_n      = n;
	s_pg_cur    = cur;
}

int player_take_page_pick(void) {
	int p = s_page_pick;
	s_page_pick = -1;   /* 取走即清:一次性的意图,不是状态 */
	return p;
}
static bool s_pref_sub = false;    /* CC 字幕开关 */
/* 默认中档:中档(0.52)正好落在 eff_scale 的吸附窗口里,是三档里唯一
 * 锐利的一档。小/大两档刻意取在窗口外 —— 用户选它们要的就是尺寸不同,
 * 发虚是明码标价的代价。 */
static int  s_dm_size = 1;         /* 弹幕字号 0小 1中 2大(默认中) */
static int  s_sub_size = 1;        /* 字幕字号 0小 1中 2大(默认中) */
static int  s_dm_area = 0;         /* 弹幕覆盖范围 0全屏 1半屏 2四分之一 3八分之一 */

/* ---------- 画面比例 ----------
 *
 * 上屏是 400x240(5:3)。片源按原始比例贴边居中时,16:9 的片上下各留
 * 约 12px 黑边,竖屏片更是只占中间窄窄一条。这里允许**强制**一个比例:
 * 画面被拉伸/压缩到该比例的框里,框再按「贴宽,放不下就贴高」居中。
 *
 * 【为什么是拉伸而不是裁切】裁切要改的是纹理坐标(subtex.left/right),
 * 而 3D 模式下那两个值已经被左右分屏占用了,两套逻辑叠在一起很容易
 * 画出半张脸。拉伸只改绘制时的缩放系数,和 3D、和硬解/软解都正交,
 * 中途切换也不用重开纹理 —— 这是唯一一个「随时能改、改完立刻生效」
 * 的实现方式。想要原始比例就选「自动」。 */
static const struct { const char *name; int w, h; } ASPECTS[] = {
	{ "自动", 0,  0  },   /* 片源原始比例(默认) */
	{ "16:9", 16, 9  },
	{ "9:16", 9,  16 },
	{ "4:3",  4,  3  },
	{ "1:1",  1,  1  },
	{ "3:2",  3,  2  },
	{ "4:5",  4,  5  },
};
#define ASPECT_N ((int)(sizeof(ASPECTS) / sizeof(ASPECTS[0])))
static int s_pref_aspect = 0;

/* 按当前比例设置算出画面在上屏里的目标矩形(ow x oh,居中绘制)。
 * 【必须能重复调用】设置页里改一档就现调一次,靠的就是它无副作用。 */
static void calc_output_size(Player *p) {
	int aw, ah;
	if (s_pref_aspect > 0 && s_pref_aspect < ASPECT_N) {
		aw = ASPECTS[s_pref_aspect].w;
		ah = ASPECTS[s_pref_aspect].h;
	} else {                       /* 自动:片源原始比例 */
		aw = p->vw > 0 ? p->vw : 16;
		ah = p->vh > 0 ? p->vh : 9;
	}
	int ow = SCREEN_W;
	int oh = (int)((long)ah * SCREEN_W / aw);
	if (oh > SCREEN_H) {           /* 贴宽放不下 → 改成贴高 */
		oh = SCREEN_H;
		ow = (int)((long)aw * SCREEN_H / ah);
	}
	if (ow > SCREEN_W) ow = SCREEN_W;
	if (oh > SCREEN_H) oh = SCREEN_H;
	if (ow < 2) ow = 2;
	if (oh < 2) oh = 2;
	p->ow = ow & ~1;               /* 取偶:居中偏移才落在整像素上 */
	p->oh = oh & ~1;
}

void player_set_meta(int64_t aid, int64_t cid, const char *bvid) {
	s_meta_aid = aid;
	s_meta_cid = cid;
	snprintf(s_meta_bvid, sizeof(s_meta_bvid), "%s", bvid ? bvid : "");
}
void player_set_login_cb(bool (*cb)(void)) { s_login_cb = cb; }
/* 开机时从存档恢复本模块的偏好。3D 故意不存:它按视频逐个手动开
 * (竖屏/2D 片开着 3D 只会花屏),记住上次的值弊大于利。 */
void player_prefs_init(void) {
	s_pref_sub = settings_get("sub", s_pref_sub ? 1 : 0) != 0;
	int v;
	v = settings_get("dm_size", s_dm_size);
	if (v >= 0 && v <= 2) s_dm_size = v;
	v = settings_get("sub_size", s_sub_size);
	if (v >= 0 && v <= 2) s_sub_size = v;
	v = settings_get("dm_area", s_dm_area);
	if (v >= 0 && v <= 3) s_dm_area = v;
	/* 画面比例**要存**(和 3D 相反)。3D 是逐片决定的(2D 片开着只会花屏),
	 * 比例是「我这台机器上想怎么看」——用户把 16:9 强制上之后,
	 * 下一个视频还得再点一次的话,这个设置就等于没有。 */
	v = settings_get("aspect", s_pref_aspect);
	if (v >= 0 && v < ASPECT_N) s_pref_aspect = v;
}

void player_set_prefs(bool danmaku_on, bool force_sw, int qn) {
	s_pref_danmaku = danmaku_on;
	s_pref_force_sw = force_sw;
	s_cur_qn = qn;
}

/* ---------- 下载线程 + 环形缓冲 ---------- */

static void downloader_main(void *arg) {
	Player *p = (Player *)arg;
	NetRing *r = &p->ring;
	/* httpc 上下文有线程亲和性:在本线程重建连接 */
	if (ns_rebind(&p->ns) != 0) {
		r->err = 1;
		return;
	}
	if (p->ns.size) r->total = p->ns.size;

	u64 last_read_ms = osGetTime();   /* 上次真正从 socket 读到东西的时刻 */
	int stall_count = 0;              /* 本次播放累计断线次数 */
	while (!r->quit) {
		if (r->seek_req) {
			if (ns_seek(&p->ns, r->seek_target) != 0) {
				r->err = 1;
			} else {
				r->rd = 0;
				r->wr = 0;
				r->base = r->seek_target;
				r->eof = 0;
			}
			__dmb();
			r->seek_req = 0; /* ack */
			continue;
		}
		u32 used = r->wr - r->rd;
		u32 space = RING_CAP - used;
		if (space < 4096 || r->eof || r->err) {
			/* 缓冲满了就不读 socket —— 这正是可疑之处:稳定播放时环形缓冲
			 * 长期是满的,连接一直闲着,CDN 到点就把它关了。那样的"断线"
			 * 是我们自己造成的,不是网络有问题。idle_ms 就是用来分辨这个的。 */
			svcSleepThread(2 * 1000 * 1000LL);
			continue;
		}
		u32 widx = r->wr % RING_CAP;
		u32 chunk = RING_CAP - widx;
		if (chunk > space) chunk = space;
		if (chunk > 65536) chunk = 65536;
		long n = ns_read(&p->ns, r->buf + widx, chunk);
		if (n > 0) {
			__dmb();
			r->wr += (u32)n;
			last_read_ms = osGetTime();
		} else {
			/* 【判 EOF 要先看位置,再看返回值】
			 * 原来的判据是「n==0 **且** 已到末尾」。可是拖到片尾时
			 * ns_read 往往是**报错**(n<0)而不是返回 0 —— 于是明明
			 * pos=100% 却被当成断线,拿一个超出文件长度的 Range 去重连,
			 * 服务端只会一直拒绝(实测连失败 5 次),而这期间解封装器
			 * 拿到的是残缺数据,最后喂给 MVD 一个 41 字节的 AU 把它搞崩。
			 * 已经读完了就是读完了,这跟连接出没出错无关。 */
			u64 wpos = r->base + r->wr;
			bool true_eof = (r->total && wpos >= r->total) ||
			                (n == 0 && !r->total);
			if (true_eof) {
				r->eof = 1;
			} else {
				/* 断线:后台无限重连(退避 0.5s→3s 封顶),
				 * 缓冲吃完时播放会自然停住,连上即自动续播;B 退出不受影响 */
				int attempt = 0;
				/* 【这一行是用来定性的】
				 * idle 大(几十秒)+ 缓冲当时是满的 → 是我们自己闲出来的,
				 *   服务端按空闲超时关的连接,不是网络有问题
				 * idle 小(几百毫秒)→ 真的断了,该去看 Wi-Fi
				 * n<0 是出错,n==0 是对端正常关闭 —— 后者更像空闲超时 */
				stall_count++;
				u64 idle = osGetTime() - last_read_ms;
				ui_trace("net 断开#%d: n=%ld idle=%dms 缓冲=%dKB/%dKB pos=%d%%",
				         stall_count, n, (int)idle,
				         (int)((r->wr - r->rd) / 1024), RING_CAP / 1024,
				         r->total ? (int)(wpos * 100 / r->total) : -1);
				/* net_is_shutting_down():系统正在关闭本程序。此时重连是
				 * 白费力气(所有请求都会被立刻拒绝),而这个循环最长要
				 * 3 秒一轮地转下去,退出就卡在这儿了。 */
				while (!r->quit && !p->quit && !net_is_shutting_down()) {
					p->net_stall = attempt + 1;   /* 主线程画「重连中」提示用 */
					s64 wait_ms = 500 + (s64)attempt * 500;
					if (wait_ms > 3000) wait_ms = 3000;
					/* 分段睡眠:退出信号 100ms 内响应,否则 B 键要等好几秒 */
					for (s64 slept = 0; slept < wait_ms; slept += 100) {
						if (r->quit || p->quit) break;
						svcSleepThread(100 * 1000 * 1000LL);
					}
					if (r->quit || p->quit) break;
					attempt++;
					if (attempt <= 3 || attempt % 10 == 0)
						printf("net stall, retry #%d...\n", attempt);
					u64 t0 = osGetTime();
					if (ns_seek(&p->ns, wpos) == 0) {
						ui_trace("net 重连成功: 第%d次尝试, 耗时%dms",
						         attempt, (int)(osGetTime() - t0));
						last_read_ms = osGetTime();
						break;
					}
					/* 重连**失败**才是真正要查的东西 —— 提示要显示出来,
					 * 得连续失败两次以上。只记前几次,别刷屏 */
					if (attempt <= 5)
						ui_trace("net 重连失败: 第%d次, 耗时%dms",
						         attempt, (int)(osGetTime() - t0));
				}
				p->net_stall = 0;
			}
		}
	}
}

/* ---------- AVIO(从环形缓冲读,不直接碰网络) ---------- */

static int avio_read_cb(void *opaque, uint8_t *buf, int n) {
	Player *p = (Player *)opaque;
	NetRing *r = &p->ring;
	while (r->wr == r->rd) {
		if (r->quit || p->quit) return AVERROR_EOF;  /* 退出优先 */
		if (r->err) return AVERROR(EIO);
		if (r->eof) return AVERROR_EOF;
		svcSleepThread(2 * 1000 * 1000LL);
	}
	u32 avail = r->wr - r->rd;
	u32 want = (u32)n;
	if (want > avail) want = avail;
	u32 ridx = r->rd % RING_CAP;
	u32 c = RING_CAP - ridx;
	if (c > want) c = want;
	memcpy(buf, r->buf + ridx, c);
	if (want > c) memcpy(buf + c, r->buf, want - c);
	__dmb();
	r->rd += want;
	return (int)want;
}

/* 起播阶段的 IO 统计。MP4 的 moov 索引越长的片越大,而且可能在文件尾部 ——
 * 一次跨区 seek = 断开重连 + 一次 HTTPS 握手,3DS 上单次就好几百毫秒。
 * 「长视频缓冲久」到底是握手多还是纯粹要读的字节多,靠这两个数分。 */
static int s_io_seeks = 0;
static u64 s_io_seek_ms = 0;

static int64_t avio_seek_cb(void *opaque, int64_t offset, int whence) {
	Player *p = (Player *)opaque;
	NetRing *r = &p->ring;
	if (whence & AVSEEK_SIZE)
		return r->total ? (int64_t)r->total : -1;
	whence &= ~AVSEEK_FORCE;
	u64 cur = r->base + r->rd;
	int64_t target;
	switch (whence) {
		case SEEK_SET: target = offset; break;
		case SEEK_CUR: target = (int64_t)cur + offset; break;
		case SEEK_END:
			if (!r->total) return -1;
			target = (int64_t)r->total + offset;
			break;
		default: return -1;
	}
	if (target < 0) return -1;
	/* 目标在已缓冲区间内:直接快进,零网络开销 */
	u64 wpos = r->base + r->wr;
	if ((u64)target >= cur && (u64)target <= wpos) {
		r->rd += (u32)((u64)target - cur);
		return target;
	}
	/* 否则请求下载线程重定位并等待(这一步要重连,是真正的开销所在) */
	u64 t0 = osGetTime();
	s_io_seeks++;
	r->seek_target = (u64)target;
	__dmb();
	r->seek_req = 1;
	for (int i = 0; i < 3000; i++) {   /* 最多等 6 秒,且响应退出 */
		if (!r->seek_req || r->err || r->quit || p->quit) break;
		svcSleepThread(2 * 1000 * 1000LL);
	}
	s_io_seek_ms += osGetTime() - t0;
	if (r->err || r->quit || p->quit) return -1;
	return target;
}

/* ---------- 音频(仅 worker 线程调用) ---------- */

static bool audio_init(Player *p) {
	/* ndspInit 失败几乎总是同一个原因:SD 卡上没有 /3ds/dspfirm.cdc。
	 * 那是主机的 DSP 固件,受版权保护、不能随程序分发,必须用户自己导出。
	 * 这是新用户最常撞上的一件事,别让它只在调试台里说一声。 */
	if (R_FAILED(ndspInit())) {
		snprintf(p->audio_err, sizeof(p->audio_err), "无声音:缺 dspfirm.cdc(见 README)");
		return false;
	}
	p->ndsp_ok = true;          /* 从这一刻起就欠一次 ndspExit */
	ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	ndspChnReset(0);
	ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
	ndspChnSetRate(0, (float)SAMPLE_RATE);
	ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
	p->abuf = (s16 *)linearAlloc(AUDIO_NBUFS * AUDIO_SAMPLES_PER_BUF * 2 * sizeof(s16));
	if (!p->abuf) {
		snprintf(p->audio_err, sizeof(p->audio_err), "无声音:内存不足");
		ndspExit(); p->ndsp_ok = false; return false;
	}
	memset(p->wbuf, 0, sizeof(p->wbuf));
	for (int i = 0; i < AUDIO_NBUFS; i++) {
		p->wbuf[i].data_vaddr = p->abuf + i * AUDIO_SAMPLES_PER_BUF * 2;
		p->wbuf[i].status = NDSP_WBUF_DONE;
	}
	return true;
}

/* 【判据是 ndsp_ok,不是 audio_ok】
 *
 * 曾经写成 `if (p->audio_ok)`,而 audio_ok 要等解码器、NDSP、重采样器
 * **全部**就绪才置位。于是只要 swr_init 那一步失败(50 小时的片子把线性
 * 内存耗光时就会),ndspInit 已经调过、ndspExit 却没调 ——
 * 更糟的是下面照样把 abuf 释放了,DSP 还在跑、通道 0 的 wavebuf 指向
 * 已释放的内存。表现是**从那次之后所有视频都没声音**,重启才好。
 *
 * 顺序也不能反:必须先停通道、再放缓冲。 */
static void audio_exit(Player *p) {
	if (p->ndsp_ok) {
		ndspChnWaveBufClear(0);   /* 先把队列里指向 abuf 的 wavebuf 摘掉 */
		ndspChnReset(0);
		ndspExit();
		p->ndsp_ok = false;
	}
	p->audio_ok = false;
	if (p->abuf) { linearFree(p->abuf); p->abuf = NULL; }
}

static void audio_reap(Player *p) {
	for (int i = 0; i < AUDIO_NBUFS; i++) {
		if (p->wbuf[i].status == NDSP_WBUF_DONE && p->wbuf[i].nsamples) {
			p->samples_done += p->wbuf[i].nsamples;
			p->wbuf[i].nsamples = 0;
		}
	}
}

static bool audio_have_free(Player *p) {
	return p->wbuf[p->next_wbuf].status == NDSP_WBUF_DONE ||
	       p->wbuf[p->next_wbuf].status == NDSP_WBUF_FREE;
}

static void audio_submit(Player *p) {
	if (p->pending_n < AUDIO_SAMPLES_PER_BUF || !audio_have_free(p))
		return;
	ndspWaveBuf *w = &p->wbuf[p->next_wbuf];
	s16 *dst = (s16 *)w->data_vaddr;
	memcpy(dst, p->pending, AUDIO_SAMPLES_PER_BUF * 2 * sizeof(s16));
	memmove(p->pending, p->pending + AUDIO_SAMPLES_PER_BUF * 2,
	        (size_t)(p->pending_n - AUDIO_SAMPLES_PER_BUF) * 2 * sizeof(s16));
	p->pending_n -= AUDIO_SAMPLES_PER_BUF;
	w->nsamples = AUDIO_SAMPLES_PER_BUF;
	DSP_FlushDataCache(dst, AUDIO_SAMPLES_PER_BUF * 2 * sizeof(s16));
	ndspChnWaveBufAdd(0, w);
	p->next_wbuf = (p->next_wbuf + 1) % AUDIO_NBUFS;
}

static double audio_clock(Player *p) {
	if (!p->audio_ok)
		return (double)(osGetTime() - p->start_ms) / 1000.0;
	/* 已播完整块 + 当前块内的采样进度,精度从 ~62ms 提到采样级 */
	return (double)(p->samples_done + ndspChnGetSamplePos(0)) / (double)SAMPLE_RATE;
}

static int audio_free_bufs(Player *p) {
	int n = 0;
	for (int i = 0; i < AUDIO_NBUFS; i++)
		if (p->wbuf[i].status == NDSP_WBUF_DONE || p->wbuf[i].status == NDSP_WBUF_FREE)
			n++;
	return n;
}

#define PENDING_CAP (int)(sizeof(((Player *)0)->pending) / (2 * sizeof(s16)))

/* pending 还能不能装下一帧音频(AAC 一帧最多 2048 采样) */
static bool audio_has_room(const Player *p) {
	return PENDING_CAP - p->pending_n >= 2048;
}

static void audio_feed(Player *p, AVPacket *pkt) {
	if (!p->audio_ok || !p->adec) return;
	if (avcodec_send_packet(p->adec, pkt) < 0) return;
	/* 关键:空间不足就停止取帧,让帧留在解码器内部排队。
	 * 原来无条件 receive+convert,pending 满时 swr_convert(space=0)
	 * 会把这一帧**直接丢掉** —— 音频缺一段而视频不缺,就是永久性
	 * 音画不同步(长时间缓冲/等弹幕时最容易触发) */
	while (audio_has_room(p) && avcodec_receive_frame(p->adec, p->aframe) == 0) {
		if (p->clock_resync) {
			int64_t ts = p->aframe->best_effort_timestamp;
			if (ts == AV_NOPTS_VALUE) ts = p->aframe->pts;
			if (ts != AV_NOPTS_VALUE && p->astream >= 0) {
				double t = ts * av_q2d(p->fmt->streams[p->astream]->time_base);
				/* 精确 seek:目标之前的音频帧直接丢,不进播放队列 */
				if (p->seek_skip > 0.0 && t + 0.03 < p->seek_skip)
					continue;
				p->samples_done = (u64)(t * SAMPLE_RATE);
				p->start_ms = osGetTime() - (u64)(t * 1000.0);
				/* 一起打出「本来想去哪」:seek 时是目标位置,起播时是 0。
				 * 两个数应该接近。差一大截 = 音频时间戳基准和视频对不上,
				 * 之后每一帧在主线程看来都「迟到」了那么多,追帧空转、
				 * 画面掉帧而声音完全正常——这种症状很难从现象反推,
				 * 所以在源头就记下来 */
				printf("clock sync %dms (want %dms)\n",
				       (int)(t * 1000.0), (int)(p->seek_skip * 1000.0));
			}
			p->clock_resync = false;
		}
		s16 *out = p->pending + p->pending_n * 2;
		int space = (int)(sizeof(p->pending) / (2 * sizeof(s16))) - p->pending_n;
		uint8_t *outp[1] = { (uint8_t *)out };
		int got = swr_convert(p->swr, outp, space,
		                      (const uint8_t **)p->aframe->data, p->aframe->nb_samples);
		if (got > 0) p->pending_n += got;
		audio_submit(p);
	}
}

/* ---------- 呈现(主线程,GPU 合成:视频纹理 + 弹幕 + OSD) ---------- */

#define VID_TRANSFER_FLAGS \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(1) | GX_TRANSFER_RAW_COPY(0) | \
	 GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565) | \
	 GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) | \
	 GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

static bool video_tex_init(Player *p) {
	int vw_al = (p->vw + 15) & ~15;
	int vh_al = (p->vh + 15) & ~15;
	if (vw_al > 1024 || vh_al > 1024) return false;
	/* 宽高都取 >= 对齐尺寸的 2 的幂:竖屏 360x640 → 512x1024(1MB),
	 * 横屏 854x480 → 1024x512(1MB);固定 1024 宽会浪费一倍内存 */
	int tw = 64, th = 64;
	while (tw < vw_al) tw <<= 1;
	while (th < vh_al) th <<= 1;
	p->tex_w = tw;
	p->tex_h = th;
	if (!C3D_TexInit(&p->tex, (u16)tw, (u16)th, GPU_RGB565)) {
		/* 纹理内存来自线性堆 —— 失败基本只有一个原因:线性堆不够。
		 * 把余量写进 trace,别再让下一个人从"tex init failed"开始猜 */
		ui_trace("TexInit %dx%d failed, need %dKB, linear free=%luKB",
		         tw, th, tw * th * 2 / 1024,
		         (unsigned long)(linearSpaceFree() / 1024));
		return false;
	}
	C3D_TexSetFilter(&p->tex, GPU_LINEAR, GPU_LINEAR);
	p->tex_ok = true;
	printf("tex %dx%d\n", tw, th);
	return true;
}

/* 解码帧(linear,行距 tex_w)→ 平铺纹理,PPF 引擎 DMA。
 * 缓存刷新由写入方(worker)负责,这里不再做——主线程每帧少一次
 * 几百 KB 的内核缓存操作,渲染循环才吃得住 60Hz */
static void video_upload(Player *p, int bufidx) {
	if (!p->tex_ok) return;
	int vh_al = (p->vh + 15) & ~15;
	C3D_SyncDisplayTransfer((u32 *)p->vout[bufidx],
	                        GX_BUFFER_DIM(p->tex_w, vh_al),
	                        (u32 *)p->tex.data,
	                        GX_BUFFER_DIM(p->tex_w, vh_al),
	                        VID_TRANSFER_FLAGS);
}

/* sbs:左右分屏 3D 模式。eye 0=左眼 1=右眼。
 * SBS 视频每帧 = 左半给左眼 + 右半给右眼,这里只需把纹理坐标切半;
 * 半宽画面拉伸回全宽正好复原比例(B 站 3D 片源多为半宽 SBS) */
/* xshift:会聚平移(像素)。左眼负、右眼正 = 非交叉视差,
 * 整个画面往屏幕"里"推;幅度随 3D 滑块,实现深度可调。
 * 弹幕是反方向(交叉视差),所以始终浮在视频前方 */
static void video_draw_top(Player *p, bool sbs, int eye, float xshift) {
	if (!p->tex_ok) return;
	float u0 = 0.0f, u1 = (float)p->vw / (float)p->tex_w;
	int srcw = p->vw;
	if (sbs) {
		float half = u1 * 0.5f;
		if (eye == 0) u1 = half;
		else          u0 = half;
		srcw = p->vw / 2;
		if (srcw < 1) srcw = 1;
	}
	p->subtex.width = (u16)srcw;
	p->subtex.height = (u16)p->vh;
	p->subtex.left = u0;
	p->subtex.right = u1;
	/* 纹理坐标:top 在上、bottom 在下(真机验证的正确方向) */
	float th = (float)p->tex_h;
	p->subtex.top = 1.0f;
	p->subtex.bottom = 1.0f - (float)p->vh / th;
	C2D_Image img = { &p->tex, &p->subtex };
	C2D_DrawImageAt(img, (SCREEN_W - p->ow) / 2.0f + xshift,
	                (SCREEN_H - p->oh) / 2.0f,
	                0.1f, NULL,
	                (float)p->ow / (float)srcw, (float)p->oh / (float)p->vh);
}

/* ---------- MVD 硬解(照 Core-2-Extreme/Video_player_for_3DS 的配方重写) ---------- */

#ifndef MVD_DEFAULT_WORKBUF_SIZE
#define MVD_DEFAULT_WORKBUF_SIZE 0x9006C8
#endif

/* ffmpeg level 值 → MVD 级别枚举 */
static int mvd_level_map(int lv) {
	switch (lv) {
		case 9:  return MVD_H264_LEVEL_1_0B;
		case 10: return MVD_H264_LEVEL_1_0;
		case 11: return MVD_H264_LEVEL_1_1;
		case 12: return MVD_H264_LEVEL_1_2;
		case 13: return MVD_H264_LEVEL_1_3;
		case 20: return MVD_H264_LEVEL_2_0;
		case 21: return MVD_H264_LEVEL_2_1;
		case 22: return MVD_H264_LEVEL_2_2;
		case 30: return MVD_H264_LEVEL_3_0;
		case 31: return MVD_H264_LEVEL_3_1;
		case 32: return MVD_H264_LEVEL_3_2;
		case 40: return MVD_H264_LEVEL_4_0;
		case 41: return MVD_H264_LEVEL_4_1;
		case 42: return MVD_H264_LEVEL_4_2;
		case 50: return MVD_H264_LEVEL_5_0;
		case 51: return MVD_H264_LEVEL_5_1;
		case 52: return MVD_H264_LEVEL_5_2;
		default: return 0xFF;
	}
}

/* 码流没报 level(或报了个没见过的值)时,按分辨率反推一个够用的。
 * 【为什么重要】以前这种情况直接退到 MVD_DEFAULT_WORKBUF_SIZE = 9.4MB,
 * 那是 level 5.2 的量。360P/480P 根本用不了这么多,但线性内存要不到
 * 这么大一块时 mvdstdInit 就失败 —— 于是整段视频白白走软解。 */
static int mvd_level_from_size(int w, int h) {
	int mb = ((w + 15) / 16) * ((h + 15) / 16);
	if (mb <= 396)  return MVD_H264_LEVEL_2_0;   /* ≤ 352x288 */
	if (mb <= 1620) return MVD_H264_LEVEL_3_0;   /* ≤ 720x576 */
	if (mb <= 3600) return MVD_H264_LEVEL_3_1;   /* ≤ 1280x720 */
	return MVD_H264_LEVEL_4_0;
}

/* SPS/PPS 存下来,由第一帧带着一起送(annex-b 的常规写法)。
 *
 * 【记一笔错误的推断】这里原本是单独把 SPS、PPS 各喂一次
 * mvdstdProcessVideoFrame 做"预热"。mvd 崩了之后我以为是"参数集不能单独送",
 * 就改成了现在这样。**这个理由是错的** —— devkitPro 官方 mvd 例程就是
 * 一个 NAL 一个 NAL 地喂,并且专门用 MVD_STATUS_PARAMSET 来识别参数集包。
 * 真正的原因在 exheader(内核版本 < 2.44),和喂法无关。
 *
 * 现在这个写法本身没毛病(参数集跟着 IDR 走是标准做法),就保留了,
 * 但别把它当成"修复"记在心里。 */

/* ---------- MVD 初始化(经由可放弃的线程) ----------
 *
 * 【为什么这么绕】CIA 下,对 mvd:STD 的第一次 IPC(CalculateWorkBufSize)
 * 会无限期挂起 —— svcSendSyncRequest 没有超时参数,主线程一旦进去就出不来,
 * 整机跟着锁死,只能抠电池。同一份代码 3dsx 秒回,原因至今不明
 * (ACL 大小写、服务权限都排查过)。
 * 打不过就绕:初始化放进独立线程,主线程 threadJoin 最多等 3 秒。
 * 超时就 threadDetach 丢下它(僵尸线程挂在 svc 里,16KB 栈,无害),
 * 本次运行永久走软解。线程若事后活过来,发现被放弃会自己收拾干净。 */
static volatile int s_mvdinit_state;       /* 0=进行中 1=成功 2=失败 */
static volatile int s_mvdinit_abandoned;
/* CIA 下 mvd 一旦超时就置位:本次运行不再重试(见 mvd_init_thread) */
static volatile int s_cia_mvd_dead = 0;
static u32 s_mvdinit_wsize;
static int s_mvdinit_level, s_mvdinit_w, s_mvdinit_h;

static void mvd_init_thread(void *arg) {
	(void)arg;
	/* 【关键诊断开关】svcSendSyncRequest 没有超时,但 srv 的"等服务可用"
	 * 有开关:非阻塞策略下,拿不到服务会立刻返回错误码而不是无限等。
	 * CIA 下的挂起如果发生在 srvGetServiceHandle(等 mvd:STD 会话),
	 * 这个开关会把它变成一个**可见的错误码** —— 挂起源头就现形了。
	 * 若挂起在拿到服务之后的 IPC 上,这个开关无效,3 秒超时兜底仍在。 */
	srvSetBlockingPolicy(true);   /* true = 非阻塞 */
	MVDSTD_CalculateWorkBufSizeConfig c;
	memset(&c, 0, sizeof(c));
	c.level.enable = 1;
	c.level.flag = MVD_CALC_WITH_LEVEL_FLAG_ENABLE_CALC |
	               MVD_CALC_WITH_LEVEL_FLAG_ENABLE_EXTRA_OP |
	               MVD_CALC_WITH_LEVEL_FLAG_UNK;
	c.level.level = (u8)s_mvdinit_level;
	c.width = (u32)s_mvdinit_w;
	c.height = (u32)s_mvdinit_h;

	u32 wsize = 0;
	/* 首次失败后本次运行不再重试,省掉每次播放白等 3 秒。
	 * (曾以为 CIA 天生调不动 mvd —— 实为 exheader 的 Dependency 漏了
	 *  mvd 模块,补上后正常;详见 cia/3danmu.rsf。这个短路保留:
	 *  万一别的机型/固件仍不应答,不至于每次都卡 3 秒。) */
	if (!envIsHomebrew() && s_cia_mvd_dead) {
		ui_trace("mvd: CIA known-dead, software (no retry)");
		s_mvdinit_state = 2;
		srvSetBlockingPolicy(false);
		return;
	}
	/* 【补上 mvd 依赖后,CIA 也能正常调这个 IPC】
	 * 于是不再需要"跳过 Calculate 用本地估值":本地估值只有 L3.0 有实测
	 * 锚点,L3.1(480P)全靠外推,不如让服务自己算准。
	 * 分支保留(条件恒假)只为万一 —— 若某天又出现挂起,把 0 改回
	 * !envIsHomebrew() 即可退回本地估值那条路。 */
	if (0) {
		/* 【CIA 实验路径】CalculateWorkBufSize 这个 IPC 在 CIA 下无限挂起
		 * (原因未明,3dsx 秒回)。跳过它,用本地值直接试 mvdstdInit。
		 * 取值必须**只多不少**:少给会让 mvd 越界写,整机死锁。
		 * 唯一有实测锚点的是 L3.0(640x360 实测 3626KB → 给 5MB,余量 38%)。
		 * B 站 360P/480P 的码流几乎都是 L3.0,覆盖了绝大多数情况。
		 * 更高的 level 没有锚点,理论需求可能到 9MB 开外 —— 曾经给过
		 * 7MB/默认值两档,要么赌小(危险)要么内存装不下,都不对。
		 * 没有把握就不赌:CIA 下高 level 一律软解。 */
		if (s_mvdinit_level <= MVD_H264_LEVEL_3_0) {
			wsize = 5 * 1024 * 1024;
			ui_trace("mvd: CIA, skip calc ipc, local wsize=%luKB",
			         (unsigned long)(wsize / 1024));
		} else if (s_mvdinit_level <= MVD_H264_LEVEL_3_1) {
			/* L3.1(480P):没有实机锚点,按规格外推 ——
			 * maxDpbMbs 是 L3.0 的 2.22 倍(18000/8100),
			 * 实测 L3.0 需 3626KB,x2.22 ≈ 8.1MB,给 8.5MB 留余量。
			 * 外推的风险方向只有"给少"(给多无害),而 2.22 倍取的是
			 * 规格上限比例,真实需求只会更低;下面还有 need>have 内存闸。 */
			wsize = 8704 * 1024;
			ui_trace("mvd: CIA, skip calc ipc, local wsize=%luKB (L3.1)",
			         (unsigned long)(wsize / 1024));
		} else {
			ui_trace("mvd: CIA, level too high for local sizing, software");
			s_mvdinit_state = 2;
			srvSetBlockingPolicy(false);
			return;
		}
	} else {
		ui_trace("mvd: calc bufsize (ipc)...");
		Result cr = mvdstdCalculateBufferSize(&c, &wsize);
		ui_trace("mvd: calc done %08lx wsize=%luKB",
		         (unsigned long)cr, (unsigned long)(wsize / 1024));
		if (s_mvdinit_abandoned) { srvSetBlockingPolicy(false); return; }
		if (R_FAILED(cr) || !wsize) wsize = MVD_DEFAULT_WORKBUF_SIZE;
	}

	/* 【绝不缩减工作缓冲】给系统模块小于它要求的缓冲会把它搞死
	 * (整机死锁,实测过)。不够就失败,回软解。 */
	if (wsize + 512 * 1024 > (u32)linearSpaceFree()) {
		ui_trace("mvd: not enough linear (%luKB needed)",
		         (unsigned long)(wsize / 1024));
		s_mvdinit_state = 2;
		srvSetBlockingPolicy(false);
		return;
	}

	/* 【mvd 会「忙」,而且忙是可以等的】
	 * mvdstdInit 失败过一种 0xD040xxxx 的码(实测 0xD0401834)。这个前缀拆开是
	 * level=Temporary、summary=WouldBlock —— 和 libctru 里 MVD_STATUS_BUSY
	 * (0xD0406B03)同族,意思是「现在不行,等会儿再来」,不是「不支持」。
	 * 典型成因:上一次会话没干净收尾(比如 mvd 自己崩过、或我们的进程被强杀,
	 * mvdstdExit 没跑到),硬件还挂在别人名下。
	 * 所以别一看到失败就退软解:隔 150ms 重试几次。仍然忙就说明是彻底卡住了,
	 * 那种只有重启主机能救,再等下去只是让用户干瞪眼。 */
	Result r = 0;
	for (int attempt = 0; attempt < 6; attempt++) {
		if (attempt) svcSleepThread(150ull * 1000 * 1000);
		if (s_mvdinit_abandoned) { srvSetBlockingPolicy(false); return; }
		ui_trace_sync("mvd: calling mvdstdInit wsize=%luKB try%d",
		              (unsigned long)(wsize / 1024), attempt + 1);
		r = mvdstdInit(MVDMODE_VIDEOPROCESSING, MVD_INPUT_H264,
		               MVD_OUTPUT_BGR565, wsize, NULL);
		ui_trace_sync("mvd: mvdstdInit returned %08lx", (unsigned long)r);
		if (R_SUCCEEDED(r)) break;
		/* 只对「忙」重试;其它错误再试也是一样的结果,别白白拖住播放 */
		if (((u32)r & 0xFFFF0000u) != 0xD0400000u) break;
	}
	srvSetBlockingPolicy(false);  /* 恢复默认,别影响后面 ndsp 等服务获取 */
	if (s_mvdinit_abandoned) {           /* 主线程已放弃:自己收拾干净 */
		if (R_SUCCEEDED(r)) mvdstdExit();
		return;
	}
	s_mvdinit_wsize = wsize;
	__dmb();
	s_mvdinit_state = R_FAILED(r) ? 2 : 1;
}

static bool mvd_start(Player *p, const AVCodecParameters *vpar) {
	s_mvd_dbg = 0;

	/* 宽高补齐 16(缓冲尺寸与配置必须一致) */
	int vw_al = (p->vw + 15) & ~15;
	int vh_al = (p->vh + 15) & ~15;
	size_t raw_sz = (size_t)vw_al * vh_al * 2;

	/* ---------- 1) 先占住**必需**的三块,再谈工作缓冲 ----------
	 *
	 * 【顺序很重要】以前是 mvdstdInit 先拿工作缓冲、再分配这三块。
	 * 工作缓冲按 H.264 level 算,而 level 是码流自己报的 —— 遇到报得虚高的
	 * (实测有 50 小时的片子报到 4.x),它能吃掉八九 MB,剩下的连 2MB 都不够,
	 * 于是这三块失败、整个硬解作废。
	 * 可这三块是**硬性下限**(输入暂存 + 双输出缓冲),工作缓冲反而是
	 * 「越大越好但可以少」的:小一点只是能缓存的参考帧少些。
	 * 所以先把下限占住,让工作缓冲去拿剩下的。 */
	ui_trace("mvd: alloc in/raw");
	p->mvd_in = (u8 *)linearAlloc(VIDEO_IN_BUF);
	p->mvd_raw[0] = (u8 *)linearAlloc(raw_sz);
	p->mvd_raw[1] = (u8 *)linearAlloc(raw_sz);
	if (!p->mvd_in || !p->mvd_raw[0] || !p->mvd_raw[1]) {
		ui_trace("mvd: linear alloc failed (free=%luKB)",
		         (unsigned long)(linearSpaceFree() / 1024));
		goto fail_free;
	}
	/* ---------- 地址窗口转换:libctru 少做了一步 ----------
	 *
	 * linearAlloc 给的是**旧 FCRAM 窗口**的地址(0x14xxxxxx),这在 New3DS 上
	 * 也是正常的 —— 不代表 exheader 退回了 Legacy(我一度这么误判过)。
	 * 但 mvd 是 New3DS 模块,它按**新窗口**(0x30xxxxxx)解释传进来的虚拟地址。
	 *
	 * libctru 自己清楚这件事:mvdstdInit 里工作缓冲是
	 *     MVDSTD_Initialize(osConvertOldLINEARMemToNew(workbuf), ...)
	 * 送进去的。可 mvdstdProcessVideoFrame 却是
	 *     MVDSTD_ProcessNALUnit((u32)inbuf_vaddr, osConvertVirtToPhys(inbuf_vaddr), ...)
	 * —— 物理地址转了,虚拟地址**原样透传**。于是 mvd 拿着 0x14xxxxxx 去解引用,
	 * 在自己的地址空间里那是片空地:data abort,FAR 正好等于我们这块缓冲的地址
	 * (实测两次崩溃 FAR=0x14D36300,与本行打印的 in= 完全一致)。
	 *
	 * 修法就是自己先转好再传。osConvertOldLINEARMemToNew 走的是
	 * 虚拟→物理→加 0x10000000,所以 libctru 内部再对它做 osConvertVirtToPhys
	 * 仍然得到同一个正确的物理地址,两个参数就都对了。 */
	p->mvd_in_n3 = (u8 *)osConvertOldLINEARMemToNew(p->mvd_in);
	if (!p->mvd_in_n3) p->mvd_in_n3 = p->mvd_in;   /* 不在线性堆?只能原样送 */
	ui_trace_sync("mvd: buf in=%p -> n3=%p raw0=%p",
	              (void *)p->mvd_in, (void *)p->mvd_in_n3, (void *)p->mvd_raw[0]);

	{
		int lv = mvd_level_map(vpar->level);
		if (lv == 0xFF) lv = mvd_level_from_size(p->vw, p->vh);
		s_mvdinit_level = lv;
		s_mvdinit_w = p->vw;
		s_mvdinit_h = p->vh;
		s_mvdinit_state = 0;
		s_mvdinit_abandoned = 0;
		__dmb();
		Thread t = threadCreate(mvd_init_thread, NULL, 16 * 1024, 0x30, -2, false);
		if (!t) goto fail_free;
		if (R_FAILED(threadJoin(t, 3000000000LL))) {
			/* 3 秒没回来 = 又挂在 mvd 的 svc 里了。丢下它,这辈子走软解 */
			s_mvdinit_abandoned = 1;
			__dmb();
			threadDetach(t);
			ui_trace("mvd: init TIMED OUT, software decode from now on");
			if (!envIsHomebrew()) s_cia_mvd_dead = 1;   /* CIA:别再赌了 */
			s_disable_mvd = true;
			s_mvd_fail_streak = MVD_FAIL_GIVEUP;   /* 后台复试也别再来 */
			goto fail_free;
		}
		threadFree(t);
		if (s_mvdinit_state != 1) goto fail_free;
	}
	u32 wsize = s_mvdinit_wsize;
	p->mvd_wsize = wsize;
	p->mvd_sps_len = p->mvd_pps_len = 0;

	/* 输出缓冲传真的,不传 NULL:传 NULL 的话 libctru 里
	 * physaddr_outdata0 = osConvertVirtToPhys(NULL) = 0,等于告诉硬件
	 * "往物理地址 0 写"。每帧还会用当前后台缓冲刷新这个字段,
	 * 这里只是给个合法初值。 */
	mvdstdGenerateDefaultConfig(&p->mvd_cfg, (u32)vw_al, (u32)vh_al,
	                            (u32)vw_al, (u32)vh_al, NULL,
	                            (u32 *)p->mvd_raw[0], (u32 *)p->mvd_raw[1]);
	p->src_w = p->vw;
	p->src_h = p->vh;
	p->src_stride = p->tex_w;

	/* 3) 从 avcC extradata 取出 SPS/PPS 存着(**不要**在这里单独送给 mvd,
	 *    原因见上面那段注释)。第一帧会把它们拼在前面一起送。 */
	const u8 *ed = vpar->extradata;
	if (ed && vpar->extradata_size > 11 && ed[0] == 1) {
		int sps_len = (ed[6] << 8) | ed[7];
		if (8 + sps_len <= vpar->extradata_size &&
		    sps_len > 0 && sps_len <= (int)sizeof(p->mvd_sps)) {
			memcpy(p->mvd_sps, ed + 8, (size_t)sps_len);
			p->mvd_sps_len = sps_len;
		}
		int po = 8 + sps_len + 1; /* 跳过 numPPS 字节 */
		if (po + 2 <= vpar->extradata_size) {
			int pps_len = (ed[po] << 8) | ed[po + 1];
			if (po + 2 + pps_len <= vpar->extradata_size &&
			    pps_len > 0 && pps_len <= (int)sizeof(p->mvd_pps)) {
				memcpy(p->mvd_pps, ed + po + 2, (size_t)pps_len);
				p->mvd_pps_len = pps_len;
			}
		}
	}
	ui_trace_sync("mvd: ready sps=%dB pps=%dB", p->mvd_sps_len, p->mvd_pps_len);

	p->mvd_inited = 1;
	p->mvd_need_hdr = true;
	p->mvd_first = true;
	p->mvd_skip = false;
	p->pts_head = 0;
	p->pts_len = 0;
	s_mvd_reset_at = osGetTime();   /* 开播即视作刚重开过,首个 seek 也走节流 */
	/* 秒表窗口从开播算起,否则第一条 prof 的时长会算进初始化耗时 */
	s_t_win0 = osGetTime();
	s_t_mvd = s_t_inval = s_t_copy = s_t_flush = 0;
	s_t_calls = s_t_frames = s_t_noframe = 0;
	s_t_reports = 0;
	s_frames_total = 0;
	s_calls_total = 0;
	return true;

fail_free:
	/* 任何一步失败都把已经拿到的还回去 —— 漏一次就少一次下回的机会 */
	if (p->mvd_in) { linearFree(p->mvd_in); p->mvd_in = NULL; }
	for (int i = 0; i < 2; i++)
		if (p->mvd_raw[i]) { linearFree(p->mvd_raw[i]); p->mvd_raw[i] = NULL; }
	return false;
}

/* 判据是「MVD 初始化了没有」,不是「现在用不用它」。
 * 后台试运行期间 use_mvd 还是 false(画面归软解),但 MVD 确实已经
 * mvdstdInit 过了 —— 按 use_mvd 判断会漏掉 mvdstdExit,把系统模块晾在那 */
static void mvd_stop(Player *p) {
	if (p->mvd_in) { linearFree(p->mvd_in); p->mvd_in = NULL; }
	for (int i = 0; i < 2; i++)
		if (p->mvd_raw[i]) { linearFree(p->mvd_raw[i]); p->mvd_raw[i] = NULL; }
	if (p->mvd_inited) {
		/* 【被系统关闭时跳过 mvdstdExit】
		 * libctru 的 mvdstdExit 里有一段**没有上限**的忙等:
		 *     ret = MVD_STATUS_BUSY;
		 *     while (ret == MVD_STATUS_BUSY) ret = MVDSTD_ControlFrameRendering(1);
		 * 解码器要是停在半帧上就永远转下去 —— 这正是「播放中按 HOME→X
		 * 卡在 Closing software」的位置。硬解通了之后才走得到这里,
		 * 所以以前没暴露。
		 *
		 * 进程马上要没了,会话随进程一起关,所以跳过是安全的。
		 * 代价:mvd 可能来不及归位,下次启动首次 mvdstdInit 撞上 0xD040xxxx
		 * 「忙」。那个已经有 6 次重试兜着(见 mvd_init_thread),能自愈。
		 * 【正常退出(B 键)仍然照常 mvdstdExit】—— 那时程序还要继续跑,
		 * 把系统模块晾着不管会连累下一个视频。 */
		if (net_is_shutting_down())
			ui_trace_sync("mvd: shutting down, skip mvdstdExit");
		else
			mvdstdExit();
		p->mvd_inited = 0;
	}
}

/* 角标探测:四角写 0x11,处理后任一变化 = MVD 已把帧写入输出缓冲
 *
 * 性能要点:探测只关心 4 个字节,缓存维护就只做这 4 条 cache line。
 * 早先版本每次探测都 Flush/Invalidate 整个输出缓冲(640x368x2 ≈ 470KB),
 * 而排空循环里每轮都要探测一次——单帧几百 KB 起步的缓存维护。
 * svcFlush/InvalidateProcessDataCache 是内核调用,持内存管理锁,
 * 会连带把其它核上的线程(包括跑 UI/GPU 的主线程)一起卡住,
 * 表现就是"播放时偶尔顿一下"。整帧失效只在真要读像素时做一次。 */
#define MVD_LINE 32                      /* ARM11 D-cache line */
static void mvd_mark_lines(u8 *ob, u32 osz, int W,
                           void (*op)(void *, u32)) {
	op(ob, MVD_LINE);
	op(ob + (u32)W * 2 - MVD_LINE, MVD_LINE);
	op(ob + osz - (u32)W * 2, MVD_LINE);
	op(ob + osz - MVD_LINE, MVD_LINE);
}
static void cache_flush(void *p, u32 n)  { GSPGPU_FlushDataCache(p, n); }
static void cache_inval(void *p, u32 n)  { GSPGPU_InvalidateDataCache(p, n); }

static void mvd_mark(u8 *ob, u32 osz, int W) {
	ob[0] = 0x11;
	ob[W * 2 - 1] = 0x11;
	ob[osz - W * 2] = 0x11;
	ob[osz - 1] = 0x11;
	mvd_mark_lines(ob, osz, W, cache_flush);   /* 只推这 4 条线 */
}
/* 探测前刷新这 4 条线即可(整帧数据留到真要读时再一次性失效) */
static bool mvd_marked(u8 *ob, u32 osz, int W) {
	mvd_mark_lines(ob, osz, W, cache_inval);
	return ob[0] == 0x11 && ob[W * 2 - 1] == 0x11 &&
	       ob[osz - W * 2] == 0x11 && ob[osz - 1] == 0x11;
}

/* seek 后重置 MVD:直接关掉重开。
 * 曾试过"反复 RenderVideoFrame 把内部帧排空"的做法,结果把 MVD 系统模块
 * 本身搞崩了(Luma 异常界面 Current process: mvd,svcBreak)——排空这种
 * 姿势超出了它的预期状态机。Exit+Init 是每次播放都在走的路径,已验证稳定;
 * 代价是 seek 多花几十毫秒,反正 seek 后本来就要重新缓冲。
 * 重开后 SPS/PPS 要重喂(mvd_start 时存了副本),首包也要重复送。 */
static bool mvd_reset(Player *p) {
	if (!p->use_mvd) return true;
	/* Exit 前后各留一点时间。**mvd 是系统模块,把它搞崩要整机重启**,
	 * 实测崩溃现场:Luma 报 `Current process: mvd / svcBreak`,
	 * 复现路径是「画面已经卡住 → 反复拖进度条」——
	 * 每拖一次就是一轮 Exit+Init,连着来它扛不住。
	 * 硬件可能还在 DMA 上一帧,给它一点收尾时间再拆。 */
	svcSleepThread(20 * 1000 * 1000LL);
	mvdstdExit();
	p->mvd_inited = 0;
	svcSleepThread(30 * 1000 * 1000LL);
	s_mvd_reset_at = osGetTime();
	Result r = mvdstdInit(MVDMODE_VIDEOPROCESSING, MVD_INPUT_H264,
	                      MVD_OUTPUT_BGR565, p->mvd_wsize, NULL);
	if (R_FAILED(r)) {
		printf("mvd reinit failed %08lx\n", (unsigned long)r);
		return false;
	}
	p->mvd_inited = 1;
	int vw_al = (p->vw + 15) & ~15;
	int vh_al = (p->vh + 15) & ~15;
	/* 同 mvd_start:给合法的输出缓冲初值 */
	mvdstdGenerateDefaultConfig(&p->mvd_cfg, (u32)vw_al, (u32)vh_al,
	                            (u32)vw_al, (u32)vh_al, NULL,
	                            (u32 *)p->mvd_raw[0], (u32 *)p->mvd_raw[1]);
	p->mvd_need_hdr = true;   /* 重开之后第一帧同样要带上 SPS/PPS */
	p->mvd_first = true;
	p->mvd_skip = false;
	p->pts_head = p->pts_len = 0;
	p->pts_drift = 0;
	return true;
}

/* MVD 输出(行距 W)→ 上传缓冲(行距 tex_w),顺带把写过的行推回内存。
 * 这活儿以前在主线程"呈现时"做:每帧 ~460KB memcpy + ~470KB 失效
 * + ~750KB 刷新,全压在 60Hz 的渲染循环里,一帧根本做不完 → 掉帧。
 * 现在放到 worker(core2)里做,主线程只剩一次 GPU 传输。
 * 安全性:mvd_marked 已经确认 DMA 写完了,此刻读是安全的;
 * 目标是 vout[p->back],与软解同一套双缓冲协议(发布时才翻转),
 * 主线程读的是 vout[mb_buf],不会和这里写的那面撞上。 */
/* ---------- 观看进度上报线程 ----------
 *
 * 上报是一次 HTTPS POST,在 3DS 上要几百毫秒到一秒多。
 * 它**曾经直接写在主渲染循环里**(每 15 秒一次),后果是主线程整段停摆:
 * 画面不动、弹幕也不动,而音频照放(NDSP 由 DSP 自己喂,不受主线程影响)。
 *
 * 「弹幕也会同时停」是定位这个 bug 的关键线索 —— 弹幕根本不经过解码器,
 * 它是主线程按真实时间推进的。画面和弹幕同时停,就只能是主线程被卡住,
 * 跟 MVD 一点关系都没有。之前几轮全在查解码器,方向错了。
 *
 * 现在主线程只置一个请求标志,网络请求全在这条线程里做。 */
static volatile int s_rep_req = 0, s_rep_quit = 0;
static int64_t s_rep_aid, s_rep_cid;
static int s_rep_sec;

static void reporter_main(void *arg) {
	(void)arg;
	while (!s_rep_quit) {
		if (s_rep_req) {
			bili_report_history(s_rep_aid, s_rep_cid, s_rep_sec);
			__dmb();
			s_rep_req = 0;
		}
		svcSleepThread(100 * 1000 * 1000LL);   /* 100ms 轮询,开销可忽略 */
	}
}

/* ---- 秒表 ----
 * 「视频稳定落后音频 442ms 且追不回来」= worker 每帧耗时逼近 33ms 的预算。
 * 到底耗在哪一段,靠猜已经错过两次了,直接量。 */

static void mvd_blit_to_vout(Player *p, const u8 *ob, u32 osz, int W) {
	if (!p->vout[p->back]) return;
	u64 t0 = svcGetSystemTick();
	GSPGPU_InvalidateDataCache((void *)ob, osz);   /* 整帧:唯一一次 */
	u64 t1 = svcGetSystemTick();
	u8 *dst = (u8 *)p->vout[p->back];
	int cw = (W < p->tex_w) ? W : p->tex_w;
	size_t rowb = (size_t)cw * 2;
	size_t dstp = (size_t)p->tex_w * 2, srcp = (size_t)W * 2;
	for (int y = 0; y < p->vh; y++)
		memcpy(dst + y * dstp, ob + y * srcp, rowb);
	u64 t2 = svcGetSystemTick();
	/* 只刷写过的行,别把纹理高度的补白也算进去 */
	GSPGPU_FlushDataCache(dst, (u32)(dstp * (size_t)p->vh));
	u64 t3 = svcGetSystemTick();
	s_t_inval += t1 - t0;
	s_t_copy  += t2 - t1;
	s_t_flush += t3 - t2;
}

/* 解一个 avcC 视频包。返回位:bit0=有帧输出 bit1=包已消费 */
#define MVD_GOT_FRAME 1
#define MVD_CONSUMED  2
static int mvd_decode_packet(Player *p, AVPacket *pkt) {
	u64 tk0 = svcGetSystemTick();
	u8 *ob = p->mvd_raw[p->back];
	int W = (p->vw + 15) & ~15;
	int H = (p->vh + 15) & ~15;
	u32 osz = (u32)W * (u32)H * 2;
	int ret = 0;
	bool got = false;

	mvd_mark(ob, osz, W);          /* 只刷 4 条角标 cache line */
	p->mvd_cfg.physaddr_outdata0 = osConvertVirtToPhys(ob);
	MVDSTD_SetConfig(&p->mvd_cfg);

	for (int attempt = 0; attempt < 2; attempt++) {
		if (!p->mvd_skip) {
			/* avcC(4 字节大端长度 + NAL)→ annex-b,整包一次送入 */
			u32 off = 0, so = 0;
			const u8 *d = pkt->data;
			u32 dn = (u32)pkt->size;
			/* 开播/seek 重开后的第一个访问单元前面必须带参数集,
			 * 否则解码器不知道分辨率和参考帧配置。annex-b 的常规做法。 */
			if (p->mvd_need_hdr) {
				const u8 *hs[2] = { p->mvd_sps, p->mvd_pps };
				int hl[2] = { p->mvd_sps_len, p->mvd_pps_len };
				for (int k = 0; k < 2; k++) {
					if (hl[k] <= 0 || off + 3 + (u32)hl[k] > VIDEO_IN_BUF) continue;
					p->mvd_in[off++] = 0;
					p->mvd_in[off++] = 0;
					p->mvd_in[off++] = 1;
					memcpy(p->mvd_in + off, hs[k], (size_t)hl[k]);
					off += (u32)hl[k];
				}
				p->mvd_need_hdr = false;
			}
			while (so + 4 < dn) {
				u32 sz = ((u32)d[so] << 24) | ((u32)d[so + 1] << 16) |
				         ((u32)d[so + 2] << 8) | d[so + 3];
				so += 4;
				if (!sz || so + sz > dn || off + 3 + sz > VIDEO_IN_BUF) break;
				p->mvd_in[off++] = 0;
				p->mvd_in[off++] = 0;
				p->mvd_in[off++] = 1;
				memcpy(p->mvd_in + off, d + so, sz);
				off += sz;
				so += sz;
			}
			/* 【残缺的 AU 绝不能送进 MVD】
			 * mvd 是**系统模块**,喂它半截数据不是我们崩,是它崩 ——
			 * 整机蓝屏,而且 PC 落在它自己的代码里,addr2line 都用不上。
			 * 实测现场:拖到片尾触发重连风暴,解封装器吐出一个 41 字节的
			 * 包(正常是 22~30KB),送进去当场 svcBreak。
			 *
			 * 判据是「装得下一帧吗」而不是「解析出错了吗」:上面那个
			 * while 循环对残缺数据是**静默 break**的,出不出错它都不说话,
			 * 所以只能从结果的大小上认。参数集本身约 32 字节,
			 * 一个真正的视频帧再小也不会只有几十字节。 */
			if (off < 128) {
				printf("mvd: skip runt AU %luB (truncated stream?)\n",
				       (unsigned long)off);
				/* 跳过之后解码器缺了这一段,下次必须重新带参数集,
				 * 否则它会拿着对不上的参考帧继续解 */
				p->mvd_need_hdr = true;
				break;
			}
			GSPGPU_FlushDataCache(p->mvd_in, off);
			bool first = p->mvd_first;
			/* 只在首帧同步落盘:mvd 若在这里崩,异步队列来不及写出去。
			 * 【绝不能去掉这个 if】同步写盘放进逐帧路径 = 主线程钉在 SD 卡上,
			 * 当初就是这么把播放搞卡死的。 */
			if (first) ui_trace_sync("mvd: -> frame#1 %luB", (unsigned long)off);
			Result r = mvdstdProcessVideoFrame(p->mvd_in_n3, off, 0, NULL);
			if (first) { /* 首帧要重复送一次(上游作者实测) */
				ui_trace_sync("mvd: <- frame#1 %08lx", (unsigned long)r);
				/* 重复投递也要留痕:偶发的「上屏全黑」现场,日志断在
				 * <- frame#1 之后 —— 嫌疑之一就是这次重复投递的 IPC
				 * 挂死(svcSendSyncRequest 无超时)。有了下面这行,
				 * 下次出现就能一锤定音:没有 dup 行 = 挂在这;
				 * 有 dup 行 = 往后找。 */
				Result r2 = mvdstdProcessVideoFrame(p->mvd_in_n3, off, 0, NULL);
				ui_trace_sync("mvd: <- frame#1 dup %08lx", (unsigned long)r2);
				p->mvd_first = false;
			}
			ret |= MVD_CONSUMED;
			/* 缓存时间戳(解码顺序,dts 单调)。
			 * 队列长度 = MVD 流水线深度,正常只有两三个。一旦"入队多于出队"
			 * (漏帧、seek 残留)就会越堆越长,而队首正是被取用的那个 →
			 * 画面时间戳整体落后、越 seek 越离谱。所以设硬上限,
			 * 满了丢最老的而不是停止入队,保证队首始终贴近当前包 */
			{
				s64 t = (pkt->dts != AV_NOPTS_VALUE) ? pkt->dts : pkt->pts;
				while (p->pts_len >= PTS_DEPTH) {   /* 丢最老 */
					p->pts_head = (p->pts_head + 1) % PTS_FIFO_CAP;
					p->pts_len--;
				}
				p->pts_fifo[(p->pts_head + p->pts_len) % PTS_FIFO_CAP] = t;
				p->pts_len++;
			}
			if (!mvd_marked(ob, osz, W)) {
				got = true;
				p->mvd_skip = true; /* 内部还有排队帧,下个包先排空再送 */
			}
			if (s_mvd_dbg < 16) {
				printf("proc r=%08lx got=%d\n", (unsigned long)r, got ? 1 : 0);
				s_mvd_dbg++;
			}
		}
		if (!got) {
			/* 排空渲染:非阻塞,BUSY 就重试,角标变了即有帧。
			 * 空转要让出 CPU——worker 和音频/网络线程共享核心,
			 * 死等 BUSY 会把它们饿住(听感上就是"偶尔卡一下") */
			/* 上限要小:排空本来就是「顺手看看有没有存货」,
			 * 没有就该去送下一个包,而不是在这里干等。
			 * 之前给到 4000 次 x 0.1ms = 单次排空最坏 400ms,
			 * 一个包两轮排空就能吃掉 0.8 秒——本身就够卡出人命 */
			for (int spin = 0; spin < 100; spin++) {   /* 最多约 10ms */
				Result r = mvdstdRenderVideoFrame(&p->mvd_cfg, false);
				if (!mvd_marked(ob, osz, W)) { got = true; break; }
				if (r != MVD_STATUS_BUSY) break;
				if (spin >= 4) svcSleepThread(100 * 1000LL);  /* 0.1ms */
			}
		}
		if (got) break;
		if (p->mvd_skip) { p->mvd_skip = false; continue; } /* 排空无果,这次送包 */
		break; /* 送了包也没帧:要更多数据 */
	}
	/* 计时必须覆盖**所有**调用,不能只算出帧的那些。
	 * 上一版把累加放在 if (got) 里面 —— 于是「吃了包却不吐帧」的时间
	 * 对秒表完全隐形,卡顿期整段不计入,均值自然一片祥和(6.5ms)。
	 * 测量本身有盲区,比没有测量更误导人。 */
	s_t_mvd += svcGetSystemTick() - tk0;
	s_t_calls++;
	s_calls_total++;
	if (got) {
		ret |= MVD_GOT_FRAME;
		/* 就地搬运到 vout(见 mvd_blit_to_vout 注释):
		 * 角标已证明 DMA 收尾,这里读安全,而且把重活留在 worker 核上。
		 * 后台试运行时跳过 —— 那会儿画面归软解,只需要知道 MVD 出没出帧 */
		if (!p->mvd_trial_noblit)
			mvd_blit_to_vout(p, ob, osz, W);
		s_t_frames++;
		s_frames_total++;
	} else {
		s_t_noframe++;
	}
	/* 每 150 次调用报一次。
	 * 必须带上窗口时长和实际出帧率 —— 只看 call/frm 的比例是解释不了的:
	 * 送一个包不一定立刻出帧(H.264 帧重排),排空阶段又会「出帧但没送包」,
	 * 所以调用数天然多于帧数,40/150 这种比例完全正常。
	 * 真正该看的是 **fps 跟片源帧率对不对得上** —— 对得上就是健康的,
	 * 对不上才说明解码这一路跟不上。指标要能自己解释自己。 */
	if (s_t_calls >= 150) {
		u64 wnow = osGetTime();
		u32 win = (u32)(wnow - s_t_win0);
		if (!win) win = 1;
		int fps10 = (int)((u64)s_t_frames * 10000 / win);
		s_t_win0 = wnow;
		/* 只在「头两个窗口」和「确实不正常」时打。
		 * 日志环形缓冲只有 160 行,每 5 秒一条 prof 十几分钟就能把
		 * 真正有用的历史(取流失败原因、字幕轨、卡顿现场)全冲掉 ——
		 * 常态刷屏的探针,等于把别的探针都关了。
		 * 判据:实际帧率掉到片源的 85% 以下,或单次调用均摊超 20ms */
		int want = (int)(p->fps * 10.0 * 0.85);
		bool bad = (fps10 < want) ||
		           (TICK_MS(s_t_mvd) / s_t_calls > 20.0);
		if (s_t_reports < 2 || bad) {
			s_t_reports++;
			printf("prof: %dms fps=%d.%d/%d call=%d frm=%d nofrm=%d "
			       "mvd=%d.%d inv=%d.%d cp=%d.%d fl=%d.%d\n",
			       (int)win, fps10 / 10, fps10 % 10, (int)(p->fps + 0.5),
			       s_t_calls, s_t_frames, s_t_noframe,
			       (int)(TICK_MS(s_t_mvd) / s_t_calls),
			       ((int)(TICK_MS(s_t_mvd) * 10 / s_t_calls)) % 10,
			       (int)(TICK_MS(s_t_inval) / s_t_calls),
			       ((int)(TICK_MS(s_t_inval) * 10 / s_t_calls)) % 10,
			       (int)(TICK_MS(s_t_copy) / s_t_calls),
			       ((int)(TICK_MS(s_t_copy) * 10 / s_t_calls)) % 10,
			       (int)(TICK_MS(s_t_flush) / s_t_calls),
			       ((int)(TICK_MS(s_t_flush) * 10 / s_t_calls)) % 10);
		}
		s_t_mvd = s_t_inval = s_t_copy = s_t_flush = 0;
		s_t_calls = s_t_frames = s_t_noframe = 0;
	}
	return ret;
}

/* ---------- 软解 ---------- */

static bool sw_start(Player *p) {
	const AVCodec *c = avcodec_find_decoder(AV_CODEC_ID_H264);
	if (!c) return false;
	p->vdec = avcodec_alloc_context3(c);
	if (!p->vdec) return false;
	avcodec_parameters_to_context(p->vdec, p->fmt->streams[p->vstream]->codecpar);
	p->vdec->flags2 |= AV_CODEC_FLAG2_FAST;
	p->vdec->skip_loop_filter = AVDISCARD_NONREF;
	if (avcodec_open2(p->vdec, c, NULL) < 0) return false;
	p->sws = sws_getContext(p->vw, p->vh,
	                        p->vdec->pix_fmt != AV_PIX_FMT_NONE ? p->vdec->pix_fmt : AV_PIX_FMT_YUV420P,
	                        p->vw, p->vh, AV_PIX_FMT_RGB565LE,
	                        SWS_FAST_BILINEAR, NULL, NULL, NULL);
	p->src_w = p->vw;
	p->src_h = p->vh;
	p->src_stride = p->tex_w;
	return p->sws != NULL;
}

static bool sw_decode(Player *p, AVPacket *pkt, double *pts_out) {
	if (avcodec_send_packet(p->vdec, pkt) < 0) return false;
	if (avcodec_receive_frame(p->vdec, p->vframe) != 0) return false;
	uint8_t *dst[1] = { (uint8_t *)p->vout[p->back] };
	int stride[1] = { p->tex_w * 2 };
	sws_scale(p->sws, (const uint8_t * const *)p->vframe->data, p->vframe->linesize,
	          0, p->vh, dst, stride);
	/* 缓存刷新也在 worker 做,主线程只管传输(与硬解路径对齐) */
	GSPGPU_FlushDataCache(dst[0], (u32)(p->tex_w * 2 * p->vh));
	int64_t ts = p->vframe->best_effort_timestamp;
	if (ts == AV_NOPTS_VALUE) ts = p->vframe->pts;
	*pts_out = (ts == AV_NOPTS_VALUE) ? -1.0 :
	           ts * av_q2d(p->fmt->streams[p->vstream]->time_base);
	return true;
}

/* 返回位:bit0=有帧 bit1=包已消费(MVD 可能不消费,需重新投喂) */
static int video_decode_pkt(Player *p, AVPacket *pkt, double *pts_out,
                            int *au_count, int *mvd_frames) {
	if (p->use_mvd) {
		int r = mvd_decode_packet(p, pkt);
		if (r & MVD_CONSUMED) (*au_count)++;
		if (r & MVD_GOT_FRAME) {
			(*mvd_frames)++;
			/* 【失败计数在这里清,不能等播完】
			 * 以前只在 player_play_inner 收尾处、且要求 p->use_mvd 仍为真
			 * 才清零。可是硬解中途热切软解、或者跳转时 mvd_reset 失败退回
			 * 软解,收尾时 use_mvd 都是 false —— 于是计数只增不减,攒够 3 次
			 * 就把**本次运行剩下的所有视频**钉死在软解上。
			 * 现象就是"很多视频打开就是软解"。
			 * 判据应该是"硬解这次真的出帧了",出满 30 帧(约 1 秒)即认定 */
			if (*mvd_frames == 30 && s_mvd_fail_streak) {
				printf("mvd decoding fine, fail streak reset (was %d)\n",
				       s_mvd_fail_streak);
				s_mvd_fail_streak = 0;
			}
			/* 从时间戳队列取一个(dts 单调,可直接当播放时间轴) */
			s64 t = AV_NOPTS_VALUE;
			double tb = av_q2d(p->fmt->streams[p->vstream]->time_base);
			/* 队列漂移自校正。
			 *
			 * 队列里每送一个包压一条 dts,每出一帧弹一条,理想情况下
			 * 队首正是本帧的时间戳。但「送了包却没出帧」(起播预热、
			 * seek 后 MVD 重开、丢包)会留下多余条目,此后**每一帧都被
			 * 贴上早了 N 帧的标签**,而且这个偏差永远不会自己消失。
			 *
			 * 后果比单纯的音画不同步更糟:标签总是「早就该显示了」,
			 * 主线程于是一解出来就立刻上屏,画面彻底失去按时钟出图的
			 * 节奏,变成「有多少包放多快」—— 帧间隔不均匀,看着就是
			 * 时不时顿一下(实测:吞吐只用掉 9/33ms 却照样卡)。
			 *
			 * 所以在这里主动修:队首明显落后于音频时钟就多弹几条,
			 * 把标签追上来。3 秒以上的离谱偏差仍然整队清空。 */
			if (!p->buffering && p->pts_len > 1) {
				double now = (double)p->clock_ms / 1000.0;
				int trimmed = 0;
				while (p->pts_len > 1 && trimmed < 4 &&
				       p->pts_fifo[p->pts_head] * tb < now - 0.25) {
					p->pts_head = (p->pts_head + 1) % PTS_FIFO_CAP;
					p->pts_len--;
					trimmed++;
				}
				if (trimmed) {
					p->pts_drift += trimmed;
					if (p->pts_drift <= 3 || p->pts_drift % 50 == 0)
						printf("pts drift: trimmed %d (total %d)\n",
						       trimmed, p->pts_drift);
				}
			}
			if (p->pts_len > 0) {
				t = p->pts_fifo[p->pts_head];
				p->pts_head = (p->pts_head + 1) % PTS_FIFO_CAP;
				p->pts_len--;
			}
			double tsec = (t == AV_NOPTS_VALUE) ? -1.0 : t * tb;
			/* 兜底:解码是被呈现节奏牵着走的,帧时间戳不可能离音频时钟太远。
			 * 真偏了就说明队列错位了 —— 清空重来,交给等间隔时钟接管,
			 * 免得画面一直等一个永远到不了的时间点(表现为黑屏/定格) */
			if (tsec >= 0.0 && !p->buffering) {
				double now = (double)p->clock_ms / 1000.0;
				if (tsec < now - 3.0 || tsec > now + 3.0) {
					printf("pts out of range: %d vs clock %d, resync\n",
					       (int)(tsec * 1000), (int)(now * 1000));
					p->pts_head = p->pts_len = 0;
					p->pts_drift = 0;
					tsec = -1.0;
				}
			}
			*pts_out = tsec;
		}
		return r;
	}
	bool got = sw_decode(p, pkt, pts_out);
	(*au_count)++;
	return (got ? MVD_GOT_FRAME : 0) | MVD_CONSUMED;
}

/* ---------- worker 线程:拉流 + 解码 + 音频 ---------- */

static void worker_main(void *arg) {
	Player *p = (Player *)arg;
	AVPacket *pkt = av_packet_alloc();
	static AVPacket *vq[VQ_CAP];
	int vq_head = 0, vq_len = 0;
	bool eof = false, applied_pause = false;
	int au_count = 0, mvd_frames = 0;
	int noframe_run = 0;      /* 连续多少个包没解出帧(解码侧卡顿探针) */
	int pkt_retry = 0;        /* 同一个包已重投几次(上限,防原地打转) */
	int vqfull_drops = 0;     /* 队列满时「解了就扔」发生了几次 */
	double vclock_fallback = 0.0;
	double clock_hold = 0.0;          /* 单调化用的上一次时钟 */

	p->buffering = 1;                 /* 起播先攒缓冲 */
	p->seek_skip = 0.0;
	bool waiting_danmaku = true;      /* 首次起播时顺带等弹幕就绪 */
	bool need_frame = false;          /* 暂停中 seek 后:解出一帧就停 */
	u64 dm_wait_since = osGetTime();  /* 兜底上限,见下 */
/* 等弹幕的硬上限。原来 20 秒太长:期间画面卡在"载入弹幕…",
 * 用户以为死机;而且缓冲态越久,音频缓冲越容易满(见 audio_feed) */
#define DM_WAIT_MAX 6000
	while (!p->quit) {
		/* 暂停 = 用户暂停 或 缓冲中(NDSP 调用留在本线程) */
		bool want_pause = (p->pause != 0) || (p->buffering != 0);
		if (want_pause != applied_pause) {
			applied_pause = want_pause;
			if (p->audio_ok) ndspChnSetPaused(0, applied_pause);
			if (applied_pause) p->pause_t0 = osGetTime();
			else p->start_ms += osGetTime() - p->pause_t0;
		}
		/* 暂停时也必须响应 seek(挂着不处理的话,进度条会弹回旧位置,
		 * 等恢复播放才突然跳过去);seek 后还要解出一帧上屏(need_frame),
		 * 让暂停画面立刻变成新位置的画面 */
		if (p->pause && !p->seek_req && !need_frame) {
			if (p->quit) break;
			svcSleepThread(5 * 1000 * 1000LL);
			continue;
		}
		/* 缓冲中不歇:继续读网络、填队列 */

		/* 跳转请求:ffmpeg 定位 → 冲刷解码器与队列 → 重置时钟 */
		if (p->seek_req) {
			double tgt = p->seek_to;
			int64_t ts = (int64_t)(tgt * AV_TIME_BASE);
			/* 向前跳常要重新发起 HTTP Range 定位,偶发超时;失败就重试,
			 * 静默吞掉的话时钟不会更新,表现为进度条弹回旧位置 */
			int seek_ok = -1;
			for (int att = 0; att < 3 && !p->quit; att++) {
				seek_ok = av_seek_frame(p->fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
				if (seek_ok >= 0) break;
				printf("av_seek failed (try %d)\n", att + 1);
				for (int w = 0; w < 3 && !p->quit; w++)
					svcSleepThread(100 * 1000 * 1000LL);
			}
			if (seek_ok >= 0) {
				/* 清空视频包队列(释放后置空,杜绝悬垂指针) */
				while (vq_len > 0) {
					if (vq[vq_head]) av_packet_free(&vq[vq_head]);
					vq[vq_head] = NULL;
					vq_head = (vq_head + 1) % VQ_CAP;
					vq_len--;
				}
				vq_head = 0;
				if (p->bsf) av_bsf_flush(p->bsf);   /* annex-b 过滤器也要冲刷 */
				/* 冲刷解码器 */
				if (p->vdec) avcodec_flush_buffers(p->vdec);
				if (p->adec) avcodec_flush_buffers(p->adec);
				/* 重采样器内部还压着上一位置的采样,不清的话它们会被拼到
				 * 新位置的音频前面 —— 每 seek 一次音频就整体后移一点点,
				 * 拖得越多越明显。swr_init 可重复调用,作用是复位内部状态 */
				if (p->swr) swr_init(p->swr);
				/* ---- 解码器热切换(只在这里做) ---- */
				if (p->hw_trial) {
					/* 跳转会打断包的连续性,试运行没法再算数了,收掉重来 */
					printf("hw trial aborted by seek\n");
					p->hw_trial = 0;
					p->mvd_trial_noblit = 0;
					mvd_stop(p);
				}
				if (p->dec_switch == 1) {
					mvd_stop(p);          /* 先释放,再改标志(它按旧值决定要不要 Exit) */
					p->use_mvd = false;
					p->dec_switch = 0;
					p->sw_since = osGetTime();
					if (!sw_start(p)) {
						printf("sw decoder failed too, giving up\n");
						p->ret = -1;
						break;
					}
					printf("switched to software decode @%ds\n",
					       (int)(p->clock_ms / 1000));
				}

				/* 【重开 MVD 要节流】连续拖进度条时,每次 seek 都
				 * Exit+Init 会把 mvd 系统模块搞崩(整机重启才能恢复)。
				 * 距上次重开不足 400ms 就跳过 —— 不重开的代价只是
				 * 解码器里可能残留旧帧,而那些帧会被 seek_gen 判定为
				 * 上一代、根本不会上屏,完全可以接受。 */
				if (p->use_mvd) {
					if (osGetTime() - s_mvd_reset_at < 400) {
						printf("seek: mvd reset throttled\n");
						p->mvd_skip = false;
						p->pts_head = p->pts_len = 0;
						p->pts_drift = 0;
					} else if (!mvd_reset(p)) {
						p->ret = -99;   /* MVD 起不来了:整体降级软解 */
						break;
					}
				}
				/* 丢弃已排队音频,重置时钟到目标位置。
				 * 关键:必须 ndspChnReset 让通道的采样位置计数归零——
				 * 音频时钟 = samples_done + ndspChnGetSamplePos(),只清前者
				 * 会让每次跳转都累加一次残留位置,越拖越不同步 */
				if (p->audio_ok) {
					ndspChnWaveBufClear(0);
					ndspChnReset(0);
					ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
					ndspChnSetRate(0, (float)SAMPLE_RATE);
					ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
					for (int i = 0; i < AUDIO_NBUFS; i++) {
						memset(&p->wbuf[i], 0, sizeof(p->wbuf[i]));
						p->wbuf[i].data_vaddr = p->abuf + i * AUDIO_SAMPLES_PER_BUF * 2;
						p->wbuf[i].status = NDSP_WBUF_DONE;
					}
					p->next_wbuf = 0;
					applied_pause = false; /* Reset 清了暂停态,下轮重新应用 */
				}
				p->pending_n = 0;
				pkt_retry = 0;
				noframe_run = 0;
				p->samples_done = (u64)(tgt * SAMPLE_RATE);
				p->clock_resync = true;  /* 首个"目标后"音频帧到达时校准 */
				/* 精确 seek:av_seek 落在目标前最近的关键帧(可能早好几秒),
				 * 若按老办法把时钟校准到关键帧 pts,进度条就会肉眼可见地
				 * 回跳一下再追回来。改为:关键帧→目标之间照常解码(维持
				 * 参考链)但音视频全部丢弃,时钟钉在目标位置,和官方 App
				 * 的精确跳转一致 */
				p->seek_skip = tgt;
				p->start_ms = osGetTime() - (u64)(tgt * 1000.0);
				p->mb_full = 0;
				eof = false;
				p->dbg_eof = 0;
				vclock_fallback = tgt;
				p->buffering = 1;   /* 跳转后重新攒缓冲 */
				p->seek_gen++;      /* 旧代的帧一律作废(防旧画面闪现) */
				p->clock_ms = (u32)(tgt * 1000.0);
				clock_hold = tgt;
				if (p->pause) need_frame = true; /* 暂停画面也要跳到新位置 */
			}
			__dmb();
			p->seek_req = 0;
			continue;
		}

		audio_reap(p);
		audio_submit(p);
		{	/* 音频时钟单调化:一个 wavebuf 播完到被 reap 之间,
			 * ndspChnGetSamplePos 已归零而 samples_done 还没加上去,
			 * 时钟会瞬间倒退最多 62ms → 弹幕/画面"一错一错"。
			 * 小幅倒退(<300ms)按住不放,大跳(seek/重置)才接受 */
			double c = audio_clock(p);
			if (!p->clock_resync && c < clock_hold && clock_hold - c < 0.3)
				c = clock_hold;
			clock_hold = c;
			p->clock_ms = (u32)(c * 1000.0);
		}

		/* 缓冲状态机:队列吃干 → 进入缓冲;音频攒到 3/4 且环形缓冲
		 * 有 256KB(或到文件尾)→ 恢复播放 */
		if (p->audio_ok) {
			int freeb = audio_free_bufs(p);
			u32 ring_used = p->ring.wr - p->ring.rd;
			/* 进入缓冲的判据从"一个 buf 都不剩"提前到"只剩 2 个"
			 * (≈90ms)。等到彻底吃干才冻结时钟,那期间 NDSP 已经在放
			 * 静音、而时钟还在往前跑,音画就永久错开一截;提前冻结,
			 * 时钟和实际出声的样本始终对得上 */
			if (!p->buffering && freeb >= AUDIO_NBUFS - 2 &&
			    !eof && !p->ring.eof) {
				p->buffering = 1;
				printf("buffering...\n");
			} else if (p->buffering &&
			           /* 起播时还要等弹幕就绪,否则开头几秒的弹幕会被跳过。
			            * 反正开头本来就要缓冲,两件事并行,不额外增加等待 */
			           !(waiting_danmaku && s_pref_danmaku && dm_loading() &&
			             osGetTime() - dm_wait_since < DM_WAIT_MAX) &&
			           ((freeb <= AUDIO_NBUFS / 4 &&
			             (ring_used > 256 * 1024 || p->ring.eof)) ||
			            eof || p->ring.err)) {
				waiting_danmaku = false;
				p->buffering = 0;
				printf("resume\n");
			}
		} else {
			p->buffering = 0;
		}

		/* 降级满 10 秒就在后台试着把硬解拉起来(只试一次)。
		 * 这段冷静期是给 mvd 系统模块留的 —— 刚出过问题就立刻重新初始化,
		 * 正是把它彻底搞崩的路径(见 mvd_reset 注释)。
		 * 敢压到 10 秒是因为试运行本身**不影响画面**:软解一直在出画,
		 * 万一 MVD 还是坏的,坏的也只是后台那份。 */
		if (!p->use_mvd && !p->hw_trial && p->sw_since && !p->hw_retried &&
		    !s_pref_force_sw && !p->seek_req &&
		    osGetTime() - p->sw_since > 10000) {
			p->hw_retried = 1;
			if (mvd_start(p, p->fmt->streams[p->vstream]->codecpar)) {
				p->hw_trial = 1;
				p->hw_trial_frames = 0;
				p->hw_trial_pkts = 0;
				p->mvd_trial_noblit = 1;
				printf("hw trial started (background)\n");
			} else {
				printf("hw trial: mvdstdInit failed, staying sw\n");
			}
		}

		bool did = false;

		/* 读包:音频缺粮 或 视频队列未半满 */
		p->dbg_vq = vq_len;
		/* 音频没空间时一律不读:读到的若是音频包就会被丢弃(见 audio_feed)。
		 * 原条件里 "vq_len < VQ_CAP/2" 这一支会绕过音频空间检查 */
		bool want_read = !eof && audio_has_room(p) &&
		    ((p->audio_ok && audio_free_bufs(p) > AUDIO_NBUFS / 4) ||
		     vq_len < VQ_CAP / 2);
		if (want_read) {
			int r = av_read_frame(p->fmt, pkt);
			did = true;
			if (r < 0) {
				eof = true;
				p->dbg_eof = 1;
			} else if (pkt->stream_index == p->astream) {
				audio_feed(p, pkt);
				av_packet_unref(pkt);
			} else if (pkt->stream_index == p->vstream) {
				if (vq_len == VQ_CAP) {
					/* 队列满:解掉最老的包保持参考链,不呈现。
					 * 注意:此处不能把"未消费"的包塞回队首——队列已满时
					 * 队首与队尾是同一槽位,新包会覆盖它 → 指针丢失 + 计数
					 * 越界 → 后续重复释放 → 野指针崩溃。满了就直接丢弃。 */
					AVPacket *old = vq[vq_head];
					vq[vq_head] = NULL;
					vq_head = (vq_head + 1) % VQ_CAP;
					vq_len--;
					if (old) {
						double tp;
						video_decode_pkt(p, old, &tp, &au_count, &mvd_frames);
						av_packet_free(&old);
						/* 这条路径一直没被监视过,而它每次都要花一整个
						 * 解码周期去解一帧然后**扔掉**。如果它频繁发生,
						 * worker 的解码预算就有一半在做无用功,
						 * 画面自然出不来。先看它到底跑得多勤 */
						/* 节流:头一次 + 之后每 300 次。这条路径可能
						 * 持续发生,按固定周期打会把日志刷满 */
						if (++vqfull_drops == 30 || vqfull_drops % 300 == 0)
							printf("vq full: decoded+discarded x%d\n",
							       vqfull_drops);
					}
				}
				{
					AVPacket *cp = av_packet_clone(pkt);
					if (cp) {   /* 内存紧张时 clone 会失败:丢包,绝不让
					             * NULL 进队(弹出解码就是空指针崩溃) */
						vq[(vq_head + vq_len) % VQ_CAP] = cp;
						vq_len++;
					} else {
						printf("packet clone failed (low mem), dropped\n");
					}
				}
				av_packet_unref(pkt);
			} else {
				av_packet_unref(pkt);
			}
		}

		/* 邮箱空则解下一帧 */
		if (!p->mb_full && vq_len > 0) {
			AVPacket *vp = vq[vq_head];
			vq[vq_head] = NULL;
			vq_head = (vq_head + 1) % VQ_CAP;
			vq_len--;
			if (!vp) continue;   /* 理论到不了,但空指针崩不起 */
			double tp = -1.0;
			/* ---- 硬解后台试运行 ----
			 * 软解照常出画,同时把**同一个包**也喂给 MVD,只看它出不出帧
			 * (试运行期间不搬运像素,省掉每帧几百 KB 的拷贝)。
			 * 连着出够 10 帧就认定它恢复正常,把显示源无缝切过去;
			 * 喂满 150 个包还不达标就收摊,本次播放不再试。
			 *
			 * 这样做的好处是**切换点完全不可见**:不用 seek、不中断画面,
			 * 而且「MVD 到底好没好」是**验证出来的**,不是赌出来的。 */
			if (p->hw_trial && !p->use_mvd) {
				p->hw_trial_pkts++;
				int tr = mvd_decode_packet(p, vp);
				if (!(tr & MVD_CONSUMED))      /* 上一轮在排空,再送一次 */
					tr |= mvd_decode_packet(p, vp);
				if (tr & MVD_GOT_FRAME) p->hw_trial_frames++;
				if (p->hw_trial_frames >= 10) {
					/* 验证通过:换显示源。软解此刻可以拆了 */
					p->hw_trial = 0;
					p->mvd_trial_noblit = 0;
					if (p->sws) { sws_freeContext(p->sws); p->sws = NULL; }
					if (p->vdec) avcodec_free_context(&p->vdec);
					p->use_mvd = true;
					p->sw_since = 0;
					p->pts_head = p->pts_len = 0;   /* 时间戳队列重新起算 */
					printf("hw trial OK (%d pkts), back to hardware\n",
					       p->hw_trial_pkts);
					/* continue 会跳过循环尾部的释放,这里必须自己来 */
					if (vp) av_packet_free(&vp);
					did = true;
					continue;   /* 这个包已经喂过 MVD 了,下一轮正常走硬解 */
				}
				if (p->hw_trial_pkts >= 150) {
					printf("hw trial failed (%d frames/%d pkts), staying sw\n",
					       p->hw_trial_frames, p->hw_trial_pkts);
					p->hw_trial = 0;
					p->mvd_trial_noblit = 0;
					mvd_stop(p);
				}
			}
			int dr = video_decode_pkt(p, vp, &tp, &au_count, &mvd_frames);
			/* 解码侧探针:连着这么多包一帧都不出,就是解码器卡住了。
			 * 主线程那边的 stall 只能看出「没帧」,分不清是解码器不出
			 * 还是没喂到包;这里直接在源头认定,并把包有没有被吃掉
			 * (consumed)也带上——两者组合能区分「MVD 拒收」和
			 * 「收了但不吐」 */
			/* 阈值给 8,不是 40。之前设 40 的教训:实测那次卡了 950ms、
			 * 也就重投二十几轮,**刚好够不到报警线** —— 探针的阈值高过
			 * 了要抓的现象,等于没有探针 */
			if (dr & MVD_GOT_FRAME) {
				noframe_run = 0;
			} else if (++noframe_run % 8 == 0) {
				printf("decoder no frame x%d (consumed=%d vq=%d)\n",
				       noframe_run, (dr & MVD_CONSUMED) ? 1 : 0, vq_len);
				/* 连着 60 个包(约两秒)一帧不出,说明 MVD 已经不正常了。
				 * 此时**最危险的事就是继续戳它**——用户看到画面卡住会去
				 * 拖进度条,那会触发 Exit+Init,而对一个已经异常的
				 * mvd 系统模块做这件事正是实测崩机的路径。
				 * 干脆整体降级软解重来:慢一点,但不会把系统模块搞崩。 */
				if (p->use_mvd && noframe_run >= 60 && !p->dec_switch) {
					/* 原地切软解:请求一次「跳到当前位置」的 seek,
					 * 切换动作在 seek 处理里完成。
					 * 早先是 ret=-99 整段重播 —— 那会**把进度清零**,
					 * 看到一半被拽回开头比卡一下更难受 */
					printf("mvd stuck, switching to software decode\n");
					p->dec_switch = 1;
					p->seek_to = (double)p->clock_ms / 1000.0;
					__dmb();
					p->seek_req = 1;
					noframe_run = 0;
				}
			}
			/* 包没被吃掉时退回队首下轮重投。
			 *
			 * 【重试的判据是「有没有进展」,不是「有没有吃包」】
			 * MVD_CONSUMED 只在真正送包时才置。而 mvd_skip 为真时,
			 * 这次调用是去**排空 MVD 内部已解好的帧** —— 出了帧、
			 * 但确实没吃这个包,这是完全正常的流水线行为。
			 * 第一版把这种情况也计入重试,于是 MVD 内部一旦排着 3 帧以上,
			 * 连排三次就把一个好端端的包丢了 —— 丢包断参考链,画面花屏。
			 * 是「保护机制」自己制造了它要防的故障。
			 *
			 * 所以:出了帧 = 有进展,计数清零。只有既没出帧、又没吃包
			 * 才算真的原地打转(实际上几乎不会发生,因为 attempt 循环
			 * 第二轮必定送包),留着纯粹作为兜底。 */
			if (dr & MVD_CONSUMED) {
				pkt_retry = 0;
			} else if (dr & MVD_GOT_FRAME) {
				pkt_retry = 0;              /* 排空出帧:正常,原样退回 */
				if (vq_len < VQ_CAP) {
					vq_head = (vq_head + VQ_CAP - 1) % VQ_CAP;
					vq[vq_head] = vp;
					vq_len++;
					vp = NULL;
				}
			} else if (vq_len < VQ_CAP && ++pkt_retry < 8) {
				vq_head = (vq_head + VQ_CAP - 1) % VQ_CAP;
				vq[vq_head] = vp;
				vq_len++;
				vp = NULL;
			} else if (pkt_retry >= 8) {
				printf("mvd stuck on pkt x%d, dropping\n", pkt_retry);
				pkt_retry = 0;
			}
			if (dr & MVD_GOT_FRAME) {
				if (tp < 0) {
					tp = vclock_fallback;
					vclock_fallback += 1.0 / p->fps;
				}
				/* 精确 seek:关键帧→目标之间的画面解了就丢(参考链已维持)。
				 * 注意 continue 会跳过循环尾部的 av_packet_free,这里必须自己释放 */
				if (p->seek_skip > 0.0 && tp + 0.001 < p->seek_skip) {
					if (vp) av_packet_free(&vp);
					did = true;
					continue;
				}
				p->seek_skip = 0.0;   /* 到达目标,恢复正常发布 */
				/* 发布到邮箱,翻转缓冲 */
				p->mb_pts_ms = (u32)(tp * 1000.0);
				p->mb_buf = p->back;
				p->mb_gen = p->seek_gen;
				__dmb();
				p->mb_full = 1;
				p->back ^= 1;
				p->dbg_decoded++;
				need_frame = false;   /* 暂停中 seek 要的那一帧有了 */
				/* 自适应追赶:落后超过上限才跳非参考帧,追回下限恢复完整解码。
				 * 同步优先:150/50ms;流畅优先:400/150ms(播放中按 X 切换) */
				if (!p->use_mvd && p->vdec) {
					s32 hi = p->sync_mode ? 150 : 400;
					s32 lo = p->sync_mode ? 50 : 150;
					s32 lag = (s32)p->clock_ms - (s32)p->mb_pts_ms;
					if (lag > hi) {
						p->vdec->skip_frame = AVDISCARD_NONREF;
						p->vdec->skip_loop_filter = AVDISCARD_ALL;
					} else if (lag < lo) {
						p->vdec->skip_frame = AVDISCARD_DEFAULT;
						p->vdec->skip_loop_filter = AVDISCARD_NONREF;
					}
				}
			}
			if (vp) av_packet_free(&vp);
			did = true;
			if (p->use_mvd && au_count >= 90 && mvd_frames * 10 < au_count) {
				p->ret = -99;
				break;
			}
		}

		if (eof) {
			need_frame = false;   /* 片尾解不出新帧,别为它空转 */
			audio_reap(p);
			bool drained = true;
			for (int i = 0; i < AUDIO_NBUFS; i++)
				if (p->wbuf[i].status == NDSP_WBUF_QUEUED ||
				    p->wbuf[i].status == NDSP_WBUF_PLAYING)
					drained = false;
			if (vq_len == 0 && !p->mb_full && drained) {
				p->ret = 0;
				break;
			}
		}

		if (!did)
			svcSleepThread(1 * 1000 * 1000LL); /* 无事可做,让出 CPU */
	}

	while (vq_len > 0) {
		av_packet_free(&vq[vq_head]);
		vq_head = (vq_head + 1) % VQ_CAP;
		vq_len--;
	}
	av_packet_free(&pkt);
	__dmb();
	p->worker_done = 1;
}

/* ---------- 主流程 ---------- */

static void player_cleanup(Player *p) {
	if (p->tex_ok) { C3D_TexDelete(&p->tex); p->tex_ok = false; }
	mvd_stop(p);
	if (p->sws) sws_freeContext(p->sws);
	if (p->vdec) avcodec_free_context(&p->vdec);
	if (p->adec) avcodec_free_context(&p->adec);
	if (p->bsf) av_bsf_free(&p->bsf);
	if (p->swr) swr_free(&p->swr);
	if (p->vframe) av_frame_free(&p->vframe);
	if (p->aframe) av_frame_free(&p->aframe);
	if (p->fmt) avformat_close_input(&p->fmt);
	if (p->avio) {
		av_freep(&p->avio->buffer);
		avio_context_free(&p->avio);
	}
	ns_close(&p->ns);
	audio_exit(p);
	for (int i = 0; i < 2; i++)
		if (p->vout[i]) { linearFree(p->vout[i]); p->vout[i] = NULL; }
}

/* 内部实现;软解降级会递归调用它(此时不重置 s_disable_mvd) */
static int player_play_inner(const char *url, const char *title);

int player_play(const char *url, const char *title) {
	/* 【进来先清】没清的话,上一次没被消费掉的选集结果会活到这一次,
	 * 一进播放就自己退出去换 P。s_suspend_req 刚犯过同样的错:
	 * 一个只在某处消费的标志,必须在每次进入那个上下文时归零。 */
	s_page_pick = -1;
	/* 每个新视频都重新试一次硬解。除非已经连续失败太多次 —— 那多半是
	 * mvd 系统模块本身状态不对了,继续初始化它风险大于收益 */
	if (s_mvd_fail_streak >= MVD_FAIL_GIVEUP) {
		if (!s_disable_mvd)
			printf("mvd failed %d times in a row, software only "
			       "(restart the app to retry)\n", s_mvd_fail_streak);
		s_disable_mvd = true;
	} else {
		s_disable_mvd = false;
	}
	return player_play_inner(url, title);
}

static int player_play_inner(const char *url, const char *title) {
	Player *p = &s_player;
	memset(p, 0, sizeof(*p));
	p->vstream = p->astream = -1;
	p->ret = -1;
	int ret = -1;
	Thread dl_th = NULL;

	snprintf(s_cur_title, sizeof(s_cur_title), "%s", title ? title : "");
	printf("\n");
	ui_log_ascii(">> ", title ? title : url, 60);  /* 中文标题会变 '?',正常 */
	printf("connecting...\n");
	osSetSpeedupEnable(true);
	APT_SetAppCpuTimeLimit(30);

	if (ns_open(&p->ns, url, 0) != 0) {
		printf("stream open failed\n");
		return -1;
	}

	/* 启动下载线程(独占网络连接,持续填充环形缓冲) */
	p->ring.buf = (u8 *)malloc(RING_CAP);
	if (!p->ring.buf) goto done;
	p->ring.total = p->ns.size;
	{
		/* 末尾的 -2 = 任意可用核心,**必须留着**:具体核心号能不能用
		 * 取决于进程的 AffinityMask,而 CIA 的掩码由 rsf 声明,
		 * 和 3dsx 经 Homebrew Launcher 继承来的不一样。
		 * 没有这个回退的话,掩码一收紧就整个建不出线程 —— 表现是
		 * 「列表正常、一播放就卡住」,而且卡在打印之前,日志里什么都没有。 */
		/* 【不要用核心 1】那是系统核:HOME 菜单、apt、各服务都在上面跑。
		 * 高优先级下载线程放上去会把 OS 饿着 —— 实测现象是按 HOME
		 * 进出都很慢,3dsx 和 CIA 都一样。核心 2/3 是 New3DS 的富余核,
		 * -2 兜底落回应用核 0。 */
		static const int dl_cores[] = { 2, 3, -2 };
		for (int i = 0; i < 3 && !dl_th; i++)
			dl_th = threadCreate(downloader_main, p, DL_STACK, 0x2E, dl_cores[i], false);
	}
	if (!dl_th) { printf("downloader thread failed\n"); goto done; }
	ui_trace("downloader thread up");

	printf("linear free before demux: %luKB\n",
	       (unsigned long)(linearSpaceFree() / 1024));

	uint8_t *aviobuf = (uint8_t *)av_malloc(AVIO_BUF_SIZE);
	p->avio = avio_alloc_context(aviobuf, AVIO_BUF_SIZE, 0, p,
	                             avio_read_cb, NULL, avio_seek_cb);
	if (!p->avio) goto done;

	p->fmt = avformat_alloc_context();
	p->fmt->pb = p->avio;
	p->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
	/* 【长视频起播慢的关键】ffmpeg 默认最多探测 5MB / 5 秒内容来猜格式和
	 * 参数,而这些字节都要从网络上拉。我们**已经知道**是 MP4 + H.264 + AAC,
	 * 探那么多纯属浪费:一路把带宽耗在探测上,进度条就一直在缓冲。
	 * 收紧到 256KB / 1 秒;不够的话 find_stream_info 会自己多读一点。 */
	p->fmt->probesize = 256 * 1024;
	p->fmt->max_analyze_duration = AV_TIME_BASE;   /* 1 秒 */
	s_io_seeks = 0;
	s_io_seek_ms = 0;
	u64 t_open = osGetTime();
	if (avformat_open_input(&p->fmt, NULL, NULL, NULL) < 0) {
		printf("demux open failed\n");
		goto done;
	}
	u64 t_info = osGetTime();
	if (avformat_find_stream_info(p->fmt, NULL) < 0) goto done;
	ui_trace("demux ready");
	/* 长片起播慢时先看这两个数:open 慢 = moov 索引大(时长越长越大),
	 * find_stream_info 慢 = 探测读得太多 */
	/* seek 那一项若占了大头,说明时间花在断开重连(握手),不是传输本身;
	 * 那种情况该优化的是"少跳几次",不是"下得更快" */
	printf("demux open %dms (info %dms) | %d reconnect seeks, %dms in them\n",
	       (int)(t_info - t_open), (int)(osGetTime() - t_info),
	       s_io_seeks, (int)s_io_seek_ms);

	for (unsigned i = 0; i < p->fmt->nb_streams; i++) {
		enum AVMediaType t = p->fmt->streams[i]->codecpar->codec_type;
		if (t == AVMEDIA_TYPE_VIDEO && p->vstream < 0) p->vstream = (int)i;
		if (t == AVMEDIA_TYPE_AUDIO && p->astream < 0) p->astream = (int)i;
	}
	if (p->vstream < 0) { ui_trace("play: no video stream"); goto done; }

	AVCodecParameters *vpar = p->fmt->streams[p->vstream]->codecpar;
	p->vw = vpar->width;
	p->vh = vpar->height;
	calc_output_size(p);
	/* 调试台只吃 ASCII(中文会被滤成 ?),所以打档位号不打 name */
	printf("video %dx%d -> %dx%d (aspect pref=%d)\n",
	       p->vw, p->vh, p->ow, p->oh, s_pref_aspect);
	if (p->fmt->duration > 0)
		p->duration = (double)p->fmt->duration / AV_TIME_BASE;
	p->fps = av_q2d(p->fmt->streams[p->vstream]->avg_frame_rate);
	if (p->fps <= 1.0 || p->fps > 61.0) p->fps = 30.0;

	/* 先建纹理(确定 tex_w/tex_h),vout 行距与纹理宽一致 */
	if (!video_tex_init(p)) { ui_trace("play: tex init failed"); goto done; }
	ui_trace("tex ready %dx%d", p->tex_w, p->tex_h);
	{
		/* 只需要 vh_al 行:上传时 GX_BUFFER_DIM(tex_w, vh_al),
		 * 纹理高度以下的部分从来没人读过。按 tex_h 分配等于白扔
		 * 每面几百 KB——640x360 时纹理 1024x512 而 vh_al 只有 368,
		 * 两面合计浪费近 600KB(竖屏更多) */
		int vh_al = (p->vh + 15) & ~15;
		if (vh_al > p->tex_h) vh_al = p->tex_h;
		size_t need = (size_t)p->tex_w * (size_t)vh_al * 2;
		p->vout[0] = (u16 *)linearAlloc(need);
		p->vout[1] = (u16 *)linearAlloc(need);
		/* 清零后整块刷一次:此后每帧只刷写过的行(vh 行),
		 * 底部对齐补白(vh..vh_al)靠这次初始刷新保持一致 */
		if (p->vout[0]) { memset(p->vout[0], 0, need);
		                  GSPGPU_FlushDataCache(p->vout[0], need); }
		if (p->vout[1]) { memset(p->vout[1], 0, need);
		                  GSPGPU_FlushDataCache(p->vout[1], need); }
	}
	if (!p->vout[0] || !p->vout[1]) { ui_trace("play: vout alloc failed"); goto done; }
	ui_trace("vout ready, linear free=%luKB", (unsigned long)(linearSpaceFree()/1024));
	p->vframe = av_frame_alloc();
	p->aframe = av_frame_alloc();

	if (vpar->codec_id == AV_CODEC_ID_H264) {
		const AVBitStreamFilter *f = av_bsf_get_by_name("h264_mp4toannexb");
		if (f && av_bsf_alloc(f, &p->bsf) == 0) {
			avcodec_parameters_copy(p->bsf->par_in, vpar);
			av_bsf_init(p->bsf);
		}
	}

	/* New3DS 默认 MVD 硬解;按住 L 进入 → 强制软解;硬解不出帧自动回退软解 */
	bool new3ds = false;
	APT_CheckNew3DS(&new3ds);
	hidScanInput();
	bool try_mvd = new3ds && !s_disable_mvd && !s_pref_force_sw &&
	               vpar->codec_id == AV_CODEC_ID_H264;
	ui_trace("decoder select: new3ds=%d try_mvd=%d", (int)new3ds, (int)try_mvd);
	if (try_mvd && mvd_start(p, vpar)) {
		p->use_mvd = true;
		ui_trace("decoder: MVD (hardware)");
		printf("decoder: MVD (hardware)\n");
	} else {
		/* 【一定要说明理由】"怎么又是软解"是排查时最常问的一句,
		 * 而原因有五种,光看结果分不出来 */
		const char *why = "mvdstdInit failed";
		if (!new3ds)                              why = "Old 3DS/2DS has no MVD";
		else if (s_pref_force_sw)                 why = "forced by setting";
		else if (s_disable_mvd)                   why = "mvd disabled after failures";
		else if (vpar->codec_id != AV_CODEC_ID_H264) why = "not H.264";
		if (!sw_start(p)) { ui_trace("play: sw decoder init failed"); goto done; }
		ui_trace("decoder: software - %s", why);
		printf("decoder: software h264 (dual-core) - %s\n", why);
	}

	p->audio_err[0] = 0;
	if (p->astream < 0) {
		/* 片源本身没有音轨 —— 用户什么都不用做,别引导他去折腾固件 */
		snprintf(p->audio_err, sizeof(p->audio_err), "此视频没有音轨");
	} else {
		AVCodecParameters *apar = p->fmt->streams[p->astream]->codecpar;
		const AVCodec *ac = avcodec_find_decoder(apar->codec_id);
		if (!ac) {
			snprintf(p->audio_err, sizeof(p->audio_err), "无声音:音频格式不支持");
		} else {
			p->adec = avcodec_alloc_context3(ac);
			avcodec_parameters_to_context(p->adec, apar);
			if (avcodec_open2(p->adec, ac, NULL) != 0) {
				snprintf(p->audio_err, sizeof(p->audio_err), "无声音:音频解码器打不开");
			} else if (audio_init(p)) {   /* 失败时 audio_init 自己填了原因 */
				AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
				swr_alloc_set_opts2(&p->swr,
					&out_layout, AV_SAMPLE_FMT_S16, SAMPLE_RATE,
					&p->adec->ch_layout, p->adec->sample_fmt, p->adec->sample_rate,
					0, NULL);
				if (p->swr && swr_init(p->swr) == 0)
					p->audio_ok = true;
				else
					snprintf(p->audio_err, sizeof(p->audio_err), "无声音:重采样初始化失败");
			}
		}
	}
	if (p->audio_ok) p->audio_err[0] = 0;
	ui_trace("audio %s%s%s", p->audio_ok ? "ok" : "UNAVAILABLE",
	         p->audio_err[0] ? " - " : "", p->audio_err);
	if (!p->audio_ok)
		printf("audio unavailable: %s\n", p->audio_err[0] ? p->audio_err : "?");
	/* 开播也要用首个音频帧的真实 pts 校准时钟:音频流首个 pts 未必是 0,
	 * 不校准的话弹幕(绝对时间)会整体偏移几百毫秒 */
	p->clock_resync = true;

	/* 诊断:打印时间基准(排查弹幕整体偏移用,调试台可见)。
	 * start_time / 各流 start_time 单位分别是 AV_TIME_BASE 与流 time_base */
	{
		int fs = (p->fmt->start_time == AV_NOPTS_VALUE) ? -1
		         : (int)(p->fmt->start_time * 1000 / AV_TIME_BASE);
		int as = -1, vs = -1;
		if (p->astream >= 0) {
			AVStream *st = p->fmt->streams[p->astream];
			if (st->start_time != AV_NOPTS_VALUE)
				as = (int)(st->start_time * av_q2d(st->time_base) * 1000.0);
		}
		if (p->vstream >= 0) {
			AVStream *st = p->fmt->streams[p->vstream];
			if (st->start_time != AV_NOPTS_VALUE)
				vs = (int)(st->start_time * av_q2d(st->time_base) * 1000.0);
		}
		printf("tbase: fmt=%dms audio=%dms video=%dms\n", fs, as, vs);
	}

	dm_reset();
	dm_set_size(s_dm_size);     /* 让模块与设置页显示一致 */
	dm_set_area(s_dm_area);
	sub_set_size(s_sub_size);
	s_pref_3d = 0;          /* 每个视频默认 2D,要 3D 手动开 */
	ui_set_3d(false);
	p->start_ms = osGetTime();

	/* 启动 worker:优先 New3DS 附加核心 */
	{
		Thread th = NULL;
		/* 同上:最后一档 -2 是保命的(见 dl_cores 的说明)。
		 * 这条是解码工作线程,建不出来就等于不能播 */
		static const int cores[] = { 2, 3, -2 };   /* 核心 1 是系统核,别碰 */
		for (int i = 0; i < 3 && !th; i++) {
			th = threadCreate(worker_main, p, WORKER_STACK, 0x2F, cores[i], false);
			if (th) ui_trace("worker on core %d", cores[i]);
		}
		if (!th) {
			printf("worker thread create failed\n");
			goto done;
		}

		/* 主线程:输入(按键+触控)+ GPU 合成呈现 */
		bool have_pic = false;
		int late_drop = 0;         /* 本轮已连续丢弃的迟到帧数 */
		s32 late_at_start = 0;     /* 本轮开丢时的落后量,用来判断有没有效 */
		int catchup_miss = 0;      /* 连续几轮追帧无效 */
		bool catchup_ok = true;    /* 追帧总开关(无效就自己关掉) */
		u64 last_present = 0;      /* 上次真正上屏的时刻(卡顿探针) */
		u64 stall_quiet = 0;       /* 卡顿日志节流 */
		/* 弹幕平滑时钟:worker 每轮才发布一次 clock_ms,起播时它忙于
		 * 初始化解码器/猛灌缓冲,发布间隔忽长忽短;而这里是 60fps 在画,
		 * 直接用会让弹幕一顿一顿地"错格"。改成本地按真实时间推进,
		 * 每帧向权威时钟收敛一小步,大跳(seek/缓冲)才硬对齐 */
		double dm_clock = -1.0;
		u64 dm_t_last = osGetTime();
		bool dragging = false;
		bool in_psettings = false;   /* 播放设置子页 */
		bool in_comments = false;   /* 评论区子页(视频照常播) */
		bool in_pages = false;      /* 选集子页(视频暂停,上屏留住画面) */
		/* 【选集是纯触屏页】滚动改成像素级,靠手指拖 —— 按行翻页在
		 * 上百 P 的合集里要点几十次。摇杆和十字键在这一页**不接管**:
		 * 它们在播放页是别的用途(方向键调进度、摇杆没占用),
		 * 同一个键在两个上下文做不同的事只会误触。 */
		float pg_scroll = 0.0f;     /* 列表滚动偏移(像素) */
		bool  pg_drag = false;      /* 正在拖列表 */
		bool  pg_bardrag = false;   /* 正在拖右侧滚动条 */
		float pg_touch_y0 = 0.0f;   /* 按下时的触点 y */
		float pg_scroll0 = 0.0f;    /* 按下时的滚动位置 */
		float pg_moved = 0.0f;      /* 本次触摸的最大偏移(判定点按还是拖动) */
		/* 最后一次有效触点。松手帧 hidTouchRead 拿不到坐标,只能自己留一份 */
		float pg_last_x = 0.0f, pg_last_y = 0.0f;
		int  sub_tries = 0;          /* 字幕已尝试次数(AI 字幕要等它生成) */
		u64  sub_kick_t0 = 0;
		bool want_console = false;   /* 帧外切调试台(帧内切会花屏) */
		double drag_pos = 0.0;
		/* 目标锁存:松手后进度条锁在目标位置,直到播放时钟真正追上
		 * (或超时 4 秒)才交还给时钟。挡住一切残余的短暂回跳 */
		double seek_latch = -1.0;
		u64 seek_latch_t0 = 0;
		/* 【提示的判据是「画面停没停」,不是任何内部状态】
		 * 曾经照着 p->buffering / p->net_stall 画,两个都错:
		 * 它们说的是「内部正在处理什么」,而用户在意的只有一件事 ——
		 * 画面还走不走。断线了但缓冲够用、画面照常播,屏幕中央就不该
		 * 弹任何东西;反过来画面真停了,才需要解释一句。
		 * 所以只看播放时钟有没有前进。 */
		u32 last_clock_ms_seen = 0xFFFFFFFFu;
		u64 last_clock_move = 0;

		/* 【进播放器先清挂起请求】
		 * s_suspend_req 由 APT 钩子置位,但**只有这个循环会消费它**。
		 * 在列表页按 HOME、或者启动时系统发的挂起事件,都会把它置上并一直留着,
		 * 于是下一次播放一进来就把自己暂停 —— 表现是「每次开程序后第一个视频
		 * 不自动播,第二个才正常」,而且看上去完全不像 HOME 键的问题。
		 *
		 * 挂起发生在播放开始**之前**,对这次播放毫无意义:那时根本没在播。
		 * 这和代码里另一处的教训是同一条 —— 陈旧的异步请求不能活到
		 * 它不再有意义的上下文里。 */
		s_suspend_req = 0;
		double last_clock_dbg = -1.0;
		u64 last_report = 0;         /* 观看历史上报节流 */
		/* 上报线程:主线程只置标志,网络请求不许出现在渲染循环里 */
		Thread rep_th = NULL;
		s_rep_req = 0;
		s_rep_quit = 0;
		{
			static const int rc[] = { 3, 2, -2 };
			for (int i = 0; i < 3 && !rep_th; i++)
				rep_th = threadCreate(reporter_main, NULL, 16 * 1024,
				                      0x3A, rc[i], false);
		}
		/* 主线程看门狗:一轮循环超过 120ms 就报。
		 * 「画面和弹幕同时停」说明卡的是主线程而不是解码器,
		 * 但主线程里能阻塞的地方不止一处(网络、threadJoin、GPU 同步传输),
		 * 与其一个个猜,不如让它自己报出来 */
		u64 loop_t0 = osGetTime();
		int slow_loops = 0;
		/* 首帧看门狗:开播 6 秒还一帧没出就把现场写盘(只报一次,不动手)。
		 * calls 能区分两种病:calls 冻在 1~2 = worker 卡死在某次 mvd IPC
		 * 里(那种卡是掐不掉的);calls 一直涨 = mvd 吃包不吐帧,
		 * 这种已有 noframe_run>=60 的自愈(原地切软解)兜着。 */
		u64 wd_t0 = osGetTime();
		bool wd_fired = false;
		while (aptMainLoop()) {
			if (!wd_fired && p->dbg_decoded == 0 &&
			    osGetTime() - wd_t0 > 6000) {
				wd_fired = true;
				/* stall= 是关键字段:>0 说明在断线重连,黑屏是网络;
				 * =0 且 calls 冻结,才是解码器卡死 */
				ui_trace_sync("watchdog: 0 frames in 6s (mvd=%d calls=%lu stall=%d)",
				              p->use_mvd ? 1 : 0,
				              (unsigned long)s_calls_total, p->net_stall);
			}
			{
				u64 nowl = osGetTime();
				u64 dtl = nowl - loop_t0;
				loop_t0 = nowl;
				if (dtl > 120 && ++slow_loops <= 20)
					printf("main loop blocked %dms\n", (int)dtl);
			}
			hidScanInput();
			u32 kDown = hidKeysDown();
			u32 kHeld = hidKeysHeld();
			u32 kUp = hidKeysUp();
			touchPosition tp = { 0, 0 };
			bool touched = (kDown & KEY_TOUCH) != 0;      /* 本帧刚按下 */
			bool holding = (kHeld & KEY_TOUCH) != 0;      /* 持续按住 */
			if (holding) hidTouchRead(&tp);

			if (kDown & KEY_B) {
				if (in_pages) { in_pages = false; }
				else if (in_comments) { in_comments = false; }
				else if (in_psettings) { in_psettings = false; }
				else { p->quit = 1; ret = 0; break; }
			}
			bool do_pause = (kDown & KEY_A) != 0;
			if (s_suspend_req) {          /* HOME 挂起:只暂停,不切换 */
				s_suspend_req = 0;
				if (!p->pause) do_pause = true;
			}
			bool want_dm_input = false;
			if (kDown & KEY_X) p->sync_mode = !p->sync_mode;

			/* 帧到点:上传纹理 */
			if (p->mb_full && p->mb_gen != p->seek_gen) {
				/* 上一代(seek 前)的残帧:直接作废,不上屏。
				 * 不作废的话,seek 后这帧会在新画面前闪现一下旧内容 */
				__dmb();
				p->mb_full = 0;
			}
			if (p->mb_full) {
				u32 c = p->clock_ms, fpts = p->mb_pts_ms;
				/* 正常:到点才上屏。异常:时间戳远在未来(>3s)说明它不可信,
				 * 直接上屏,绝不为了一个坏时间戳把画面停住 */
				if ((s32)(fpts - c) <= 5 || (s32)(fpts - c) > 3000) {
					/* 追帧:这帧已经迟到很多(网络抖动/解码跟不上),
					 * 上屏也只是补一张过期画面,反而把邮箱占着让 worker
					 * 停工——丢掉它,腾出邮箱去解下一帧。
					 *
					 * 但这招**只在落后是暂时的时候管用**。如果落后是结构性的
					 * (音视频时间戳基准本来就差一截、pts 队列记错),
					 * 丢多少帧都追不上,只会变成「丢 N 张放 1 张」的循环——
					 * 画面掉到几帧每秒而声音完全正常,正是实测到的
					 * 「视频偶尔卡一下、音频不卡」。所以必须能自己认输:
					 * 连续三轮丢完都没见好转,就永久关掉追帧,老老实实按序放。 */
					s32 late = (s32)(c - fpts);
					if (late > 400 && have_pic && catchup_ok && late_drop < 2) {
						if (late_drop == 0) late_at_start = late;
						late_drop++;
					} else {
						if (late_drop > 0) {
							/* 刚丢过一轮:这轮到底有没有把差距拉近? */
							if (late >= late_at_start - 50) {
								if (++catchup_miss >= 3) {
									catchup_ok = false;
									printf("catch-up not working (%dms), off\n",
									       (int)late);
								}
							} else {
								catchup_miss = 0;
							}
						}
						late_drop = 0;
						p->cur_pts = (double)fpts / 1000.0;
						/* 硬解/软解统一:worker 已把像素写进 vout[mb_buf]
						 * 并刷好缓存,主线程这里只剩一次 GPU 传输 */
						video_upload(p, p->mb_buf);
						have_pic = true;
						last_present = osGetTime();
					}
					__dmb();
					p->mb_full = 0;
				}
			}

			/* ---- 卡顿探针 ----
			 * 「画面卡一下但声音不卡」意味着音频还有余粮、时钟照常走,
			 * 是视频这一路某个环节断供了。光靠肉眼分不清是谁,
			 * 这里在画面停超过 250ms 时打一行现场快照,一眼定位:
			 *   ring 很小        → 网络没跟上(下载线程饿死了解复用)
			 *   ring 大但 vq=0   → 解复用/解码跟不上(worker 卡住)
			 *   vq 有货但 mb=0   → 解码器没出帧(MVD 排空/参考帧缺失)
			 *   mb=1 却不上屏    → 时间戳问题(帧还没到点,或追帧在丢)
			 * 每 2 秒最多一条,不会刷屏。 */
			if (p->pause || p->buffering || !have_pic || p->seek_req ||
			    seek_latch >= 0.0 || p->dbg_eof) {
				/* 暂停/缓冲/跳转/片尾期间画面本来就该停,不算卡顿。
				 * 片尾尤其要排除:最后一帧停在屏上等剩余音频放完,
				 * 现场看起来是 ring=0 vq=0 mb=0,和「真卡住」一模一样。
				 * 顺手把基准推到当下,恢复后才不会误报一条 */
				last_present = osGetTime();
			} else {
				u64 now = osGetTime();
				if (last_present && now - last_present > 250 &&
				    now - stall_quiet > 2000) {
					stall_quiet = now;
					printf("stall %dms ring=%dKB vq=%d mb=%d late=%d\n",
					       (int)(now - last_present),
					       (int)((p->ring.wr - p->ring.rd) / 1024),
					       p->dbg_vq, p->mb_full ? 1 : 0,
					       (int)((s32)p->clock_ms - (s32)p->mb_pts_ms));
				}
			}

			double clock = (double)p->clock_ms / 1000.0;
			s_player_clock_ms = p->clock_ms;

			/* 时钟追上目标(或超时)后解除锁存 */
			{	/* 解除条件必须是"时钟落到目标附近",不能是 >=:
				 * 向前跳(倒回)时旧时钟本来就大于目标,>= 会让锁存
				 * 当帧失效,残余回跳全露出来——这就是"向后跳好了、
				 * 向前跳还闪"的原因 */
				double d = clock - seek_latch;
				if (seek_latch >= 0.0 &&
				    ((d > -0.3 && d < 0.3) ||
				     osGetTime() - seek_latch_t0 > 4000))
					seek_latch = -1.0;
			}
			/* 回跳侦测:非跳转期时钟倒退超过 0.5s 属异常,打日志找根源 */
			if (last_clock_dbg >= 0.0 && !p->seek_req && seek_latch < 0.0 &&
			    clock + 0.5 < last_clock_dbg)
				printf("clock regressed %d -> %d ms\n",
				       (int)(last_clock_dbg * 1000), (int)(clock * 1000));
			last_clock_dbg = (p->seek_req || seek_latch >= 0.0) ? -1.0 : clock;

			{	/* 平滑弹幕时钟 */
				u64 now_ms = osGetTime();
				double dt = (double)(now_ms - dm_t_last) / 1000.0;
				dm_t_last = now_ms;
				if (dt < 0.0) dt = 0.0;
				if (dt > 0.25) dt = 0.25;        /* 卡顿一大下就不硬推 */
				if (p->pause || p->buffering) dt = 0.0;  /* 冻结时不走 */
				if (dm_clock < 0.0 || clock < dm_clock - 0.5 ||
				    clock > dm_clock + 1.0) {
					dm_clock = clock;            /* 首帧 / seek / 大偏差:硬对齐 */
				} else {
					dm_clock += dt;
					dm_clock += (clock - dm_clock) * 0.08;  /* 每帧收 8% */
				}
			}

			ui_begin();
			/* 3D 开启时上屏画两遍:左眼 + 右眼。弹幕加视差偏移
			 *(左 + 右 -,交叉视差 = 浮在画面前方),深度跟随 3D 滑块 */
			float slider = s_pref_3d ? ui_slider_3d() : 0.0f;
			/* 滑块 = 连续的会聚旋钮(模拟 3DS 游戏手感):
			 * 低位 ≈ 零平移(深度全由片源决定),推满 = 每眼 5px 往里推。
			 * 立体强弱本身来自片源视差,平移只是把深度范围整体进出;
			 * 推满出现轻微重影是光栅串扰 + 平移叠加的正常代价,
			 * 和官方游戏推满一样,回拉即缓解 */
			float vid_px = slider * 5.0f;
			float dm_px = slider * 3.0f;
			for (int eye = 0; eye < (s_pref_3d ? 2 : 1); eye++) {
				if (eye == 1) ui_begin_top_right();
				if (have_pic) video_draw_top(p, s_pref_3d != 0, eye,
				                             eye == 0 ? -vid_px : vid_px);
				if (s_pref_danmaku)
					dm_draw(dm_clock, eye == 0 ? dm_px : -dm_px);
				if (s_pref_sub)
					sub_draw((double)p->clock_ms / 1000.0);
				if (dm_loading() && s_pref_danmaku) {
					/* z 抬过弹幕(0.5),否则弹幕字会穿透提示框底 */
					ui_rect_z(294, 3, 0.6f, 102, 23, C2D_Color32(0, 0, 0, 0x90));
					ui_text_z(299, 5, 0.7f, UI_SHARP, UI_COL_DIM, "弹幕加载中");
				}
				/* 缓冲/重连提示。三种文案按「用户该知道什么」分级:
				 *   重连中 —— 网络断了,程序在自救,别急着退(附次数,
				 *             一直涨说明网络真有问题,该去看 Wi-Fi 了)
				 *   载入弹幕 / 缓冲中 —— 正常等待
				 * 显示条件除了 buffering 还加了「首帧还没出过」:
				 * 开播头几秒 buffering 可能尚未置位,而屏幕全黑 ——
				 * 黑屏没有任何字,和死机没法区分,这正是被报过的观感 bug。 */
				/* 【只在画面真的停了时才提示】
				 * 时钟一直在走 = 画面在播 = 用户没被打扰,哪怕后台正在
				 * 断线重连。断线本身不值得打断观看,缓冲盖得住就当没发生。
				 *
				 * 门槛分两档:起播还没出过帧时屏幕是全黑的,黑屏不出字
				 * 和死机没法区分,所以早一点;正在播的片子停一下则要等久些,
				 * 免得为几百毫秒的抖动闪一下提示。 */
				u32 cms = p->clock_ms;
				/* 暂停期间时钟本来就不走,时间戳要跟着推 ——
				 * 否则一恢复播放就"已经停了很久",立刻闪一下提示 */
				if (cms != last_clock_ms_seen || p->pause) {
					last_clock_ms_seen = cms;
					last_clock_move = osGetTime();
				}
				u32 hold_ms = (p->dbg_decoded == 0) ? 400 : 1200;
				bool frozen = !p->pause && last_clock_move &&
				              osGetTime() - last_clock_move >= hold_ms;
				if (frozen) {
					bool waiting_dm = s_pref_danmaku && dm_loading();
					static const char *dots[4] = { "", ".", "..", "..." };
					char buf[48];
					if (p->net_stall > 2)   /* 头两次闪断不惊动用户 */
						snprintf(buf, sizeof(buf), "网络中断 重连中(%d)%s",
						         p->net_stall,
						         dots[(osGetTime() / 350) % 4]);
					else
						snprintf(buf, sizeof(buf), "%s%s",
						         waiting_dm ? "载入弹幕" : "缓冲中",
						         dots[(osGetTime() / 350) % 4]);
					float tw = ui_text_width(buf, UI_SHARP);
					ui_rect_z(200 - tw / 2 - 14, 100, 0.6f, tw + 28, 36,
					          C2D_Color32(0, 0, 0, 0xA8));
					ui_text_z(200 - tw / 2, 108, 0.7f, UI_SHARP,
					          UI_COL_WHITE, buf);
				}
				if (s_toast[0] && osGetTime() < s_toast_until) {
					/* 操作结果浮层(发弹幕成败等),z 最高 */
					float tw = ui_text_width(s_toast, UI_SHARP);
					if (tw > 372) tw = 372;
					ui_rect_z(200 - tw / 2 - 8, 44, 0.75f, tw + 16, 28,
					          C2D_Color32(0, 0, 0, 0xC8));
					ui_text_clipped_z(200 - tw / 2, 48, 0.8f, UI_SHARP,
					                  UI_COL_WHITE, s_toast, 372);
				}
			}
			bool btn_touch = touched && !dragging;
			if (in_pages && !ui_console_active()) {
				/* 选集子页:上屏保持暂停的画面,下屏整个换成列表。
				 * **纯触屏**:拖动滚动、点按换 P、右侧滚动条可拖。
				 * 不接管摇杆和十字键 —— 那两个在播放页另有用途。 */
				const float ROWH = 34.0f;
				const float LX = 6.0f, LW = 296.0f;
				const float LY = 24.0f;
				const float BAR_X = 306.0f, BAR_W = 8.0f;
				float th = ui_text_height(UI_SHARP);
				/* 可视区高度由底部那两行倒推,不写死 ——
				 * 写死 170 的那一版,说明行正好落进了列表区里。
				 * 底部布局:状态条 (th+8) 高、离屏底 2;说明行在它上面 5px。 */
				float bar_h = th + 8.0f;
				float bar_y = 240.0f - bar_h - 2.0f;
				float hint_y = bar_y - th - 5.0f;
				float LH = hint_y - 4.0f - LY;
				float maxscroll = s_pg_n * ROWH - LH;
				if (maxscroll < 0) maxscroll = 0;

				/* ---- 触摸 ----
				 * 【坐标必须自己记住】hidTouchRead 只在按住期间有效,
				 * **松手那一帧 tp 已经是 (0,0)** —— 而点选正是在松手时判定的。
				 * 直接用 tp 的话,命中测试永远落在左上角,一行都点不中。 */
				if (touched) {
					pg_moved = 0.0f;
					pg_touch_y0 = tp.py;
					pg_scroll0 = pg_scroll;
					pg_last_x = tp.px;
					pg_last_y = tp.py;
					pg_bardrag = (tp.px >= BAR_X - 6 && maxscroll > 0);
					pg_drag = !pg_bardrag && tp.py >= LY && tp.py < LY + LH;
					if (pg_bardrag) {   /* 点滚动条:直接跳到该位置 */
						float rel = (tp.py - LY) / LH;
						pg_scroll = rel * maxscroll;
					}
				}
				if (holding) {
					pg_last_x = tp.px;
					pg_last_y = tp.py;
					float dy = tp.py - pg_touch_y0;
					float ady = dy < 0 ? -dy : dy;
					/* 记**最大偏移**而不是累加:累加的话按住不动时,
					 * 每帧几像素的抖动也会攒成"拖动过",于是点不动 */
					if (ady > pg_moved) pg_moved = ady;
					if (pg_bardrag) {
						float rel = (tp.py - LY) / LH;
						pg_scroll = rel * maxscroll;
					} else if (pg_drag) {
						pg_scroll = pg_scroll0 - dy;
					}
				}
				bool pg_released = (kUp & KEY_TOUCH) != 0;
				if (!holding) { pg_drag = false; pg_bardrag = false; }
				if (pg_scroll < 0) pg_scroll = 0;
				if (pg_scroll > maxscroll) pg_scroll = maxscroll;

				ui_begin_bottom();
				ui_rect(0, 0, 320, 22, UI_COL_ACCENT);
				char hdr[64];
				snprintf(hdr, sizeof(hdr), "选集  当前 P%d / 共 %d",
				         s_pg_cur + 1, s_pg_n);
				ui_text(8, (22.0f - th) / 2.0f, UI_SHARP, UI_COL_WHITE, hdr);

				/* ---- 列表 ---- */
				ui_clip(LX, LY, LW + 4.0f, LH);
				int kfirst = (int)(pg_scroll / ROWH);
				if (kfirst < 0) kfirst = 0;
				for (int i = kfirst; i < s_pg_n; i++) {
					float y = (float)(int)(LY + i * ROWH - pg_scroll);
					if (y >= LY + LH) break;
					float h = ROWH - 2.0f;
					/* 【点按 ≠ 拖动】松手时移动量还很小才算点选,
					 * 否则「滑到一半松手」会误触发换 P。 */
					bool tap = pg_released && pg_moved < 8.0f &&
					           pg_last_x >= LX && pg_last_x < LX + LW &&
					           pg_last_y >= y && pg_last_y < y + h &&
					           pg_last_y >= LY && pg_last_y < LY + LH;
					if (tap && i != s_pg_cur) {
						/* 换 P:退出播放,由 main.c 拿着下标重新取流 */
						s_page_pick = i;
						p->quit = 1;
					}
					/* 点当前这一 P:不是要换,是「就看这个」—— 关掉面板 */
					if (tap && i == s_pg_cur) in_pages = false;
					/* 【当前 P 不高亮】哪一 P 在放,底部状态条已经写着了;
					 * 列表里再标一次只是让人以为「这一行被选中了」——
					 * 而这一页里唯一的选中动作就是点按本身。 */
					ui_rect(LX, y, LW, h, UI_COL_SEL);
					/* 按下反馈:和 ui_button 同款白边。
					 * 但这里**按住期间一直显示**,而不是像按钮那样只闪一帧 ——
					 * 这一页要拖要滑,手指在屏幕上停留的时间长得多,
					 * 一帧的反馈根本看不见。移动超过阈值就撤掉:
					 * 那时已经是在拖列表,不再是要点这一行。 */
					if (holding && pg_moved < 8.0f && !pg_bardrag &&
					    pg_last_x >= LX && pg_last_x < LX + LW &&
					    pg_last_y >= y && pg_last_y < y + h &&
					    pg_last_y >= LY && pg_last_y < LY + LH) {
						ui_rect(LX, y, LW, 2, UI_COL_WHITE);
						ui_rect(LX, y + h - 2, LW, 2, UI_COL_WHITE);
						ui_rect(LX, y, 2, h, UI_COL_WHITE);
						ui_rect(LX + LW - 2, y, 2, h, UI_COL_WHITE);
					}
					float ty = y + (h - th) / 2.0f;
					float dw = 0.0f;
					if (s_pg_durs && s_pg_durs[i] > 0) {
						char db[16];
						snprintf(db, sizeof(db), "%d:%02d",
						         s_pg_durs[i] / 60, s_pg_durs[i] % 60);
						dw = ui_text_width(db, UI_SHARP);
						ui_text(LX + LW - 6.0f - dw, ty, UI_SHARP,
						        UI_COL_DIM, db);
						dw += 10.0f;
					}
					ui_text_clipped(LX + 8.0f, ty, UI_SHARP, UI_COL_WHITE,
					                s_pg_labels[i], LW - 16.0f - dw);
				}
				ui_unclip();

				/* ---- 右侧滚动条(可拖) ---- */
				if (maxscroll > 0) {
					float bh = LH * LH / (s_pg_n * ROWH);
					if (bh < 16.0f) bh = 16.0f;
					float pos = pg_scroll / maxscroll;
					ui_rect(BAR_X, LY, BAR_W, LH,
					        C2D_Color32(0x30, 0x30, 0x3C, 0xFF));
					ui_rect(BAR_X, LY + (LH - bh) * pos, BAR_W, bh,
					        pg_bardrag ? UI_COL_WHITE : UI_COL_ACCENT);
				}

				/* ---- 按键说明 + 状态条 ----
				 * 位置全部由实测行高推,别写死 y ——
				 * 写死过一次,说明行的下沿正好压在状态条的底色上。 */
				{
					ui_text(8, hint_y, UI_SHARP, UI_COL_DIM,
					        "滑动翻找   点按播放   B 返回");
					ui_rect(6, bar_y, 308, bar_h,
					        C2D_Color32(0x26, 0x26, 0x30, 0xFF));
					char sb[96];
					snprintf(sb, sizeof(sb), "正在播放:%s",
					         (s_pg_cur >= 0 && s_pg_cur < s_pg_n)
					         ? s_pg_labels[s_pg_cur] : "");
					ui_text_clipped(14, bar_y + 4.0f, UI_SHARP,
					                UI_COL_WHITE, sb, 292);
				}
			} else if (in_comments && !ui_console_active()) {
				/* 评论区子页:占满下屏,上屏视频照常播、弹幕照常飘。
				 * 触屏只管拖动滚屏,滚到底自动续下一页;关闭走 B。
				 * 下屏不放按钮 —— 按钮行会把底部提示挤远 */
				if (comment_draw(touched, (kHeld & KEY_TOUCH) != 0,
				                 tp.px, tp.py))
					in_comments = false;
			} else if (in_psettings && !ui_console_active()) { /* 播放设置子页 */
				/* 四行两列。行距 42(按钮 38 + 缝 4):比原来的三行 40+8
				 * 少占 6px,正好腾出第四行,底下还留得住两行说明。
				 * 【别再往里加按钮了】再加就得上翻页,而翻页在一个
				 * 「看片时顺手点一下」的面板上是纯负担。 */
				#define PS_Y(row) (26.0f + (row) * 42.0f)
				#define PS_H 38.0f
				ui_begin_bottom();
				ui_text(10, 4, UI_SHARP, UI_COL_TEXT, "播放设置");
				if (ui_button(10, PS_Y(0), 145, PS_H,
				              s_pref_3d ? "3D:开" : "3D:关",
				              s_pref_3d ? UI_COL_ACCENT : UI_COL_SEL,
				              btn_touch, tp.px, tp.py)) {
					s_pref_3d = !s_pref_3d;
					ui_set_3d(s_pref_3d != 0);
				}
				if (ui_button(165, PS_Y(0), 145, PS_H,
				              s_pref_sub ? "字幕:开" : "字幕:关",
				              s_pref_sub ? UI_COL_ACCENT : UI_COL_SEL,
				              btn_touch, tp.px, tp.py)) {
					s_pref_sub = !s_pref_sub;
					settings_set("sub", s_pref_sub);
					/* 开关本身只管画不画,不会产生日志——这里补一行状态,
					 * 并且"开启但没字幕"时当场重拉一次,方便排查 */
					printf("subtitle display %s (lines=%d loading=%d)\n",
					       s_pref_sub ? "ON" : "OFF",
					       sub_count(), (int)sub_loading());
					if (s_pref_sub && sub_count() == 0 && !sub_loading()) {
						if (s_meta_bvid[0] && s_meta_cid) {
							printf("subtitle reload: bvid=%s cid=%u%09u\n",
							       s_meta_bvid,
							       (unsigned)(s_meta_cid / 1000000000),
							       (unsigned)(s_meta_cid % 1000000000));
							sub_load_async(s_meta_bvid, s_meta_aid, s_meta_cid);
						} else {
							printf("subtitle reload skipped: no bvid/cid\n");
						}
					}
				}
				{
					static const char *sz[3] = { "弹幕字号:小",
					                             "弹幕字号:中",
					                             "弹幕字号:大" };
					if (ui_button(10, PS_Y(1), 145, PS_H, sz[s_dm_size],
					              UI_COL_SEL, btn_touch, tp.px, tp.py)) {
						s_dm_size = (s_dm_size + 1) % 3;
						settings_set("dm_size", s_dm_size);
						dm_set_size(s_dm_size);
						dm_set_area(s_dm_area);   /* 行数依赖字号,重算一次 */
					}
				}
				{	/* 字幕字号(占原"返回"的位置) */
					static const char *ssz[3] = { "字幕字号:小",
					                              "字幕字号:中",
					                              "字幕字号:大" };
					if (ui_button(165, PS_Y(1), 145, PS_H, ssz[s_sub_size],
					              UI_COL_SEL, btn_touch, tp.px, tp.py)) {
						s_sub_size = (s_sub_size + 1) % 3;
						settings_set("sub_size", s_sub_size);
						sub_set_size(s_sub_size);
					}
				}
				{	/* 画面比例:循环切换,当场生效(只改绘制时的缩放) */
					char ab[32];
					snprintf(ab, sizeof(ab), "画面比例:%s",
					         ASPECTS[s_pref_aspect].name);
					/* 【不按值高亮】按钮上写着当前值,高亮不提供任何额外信息,
					 * 只是让「非默认」看起来像「开启了什么」。同一行里
					 * 3D 那个是真·开关(开着会影响画面),那种才该高亮。 */
					if (ui_button(10, PS_Y(2), 145, PS_H, ab,
					              UI_COL_SEL, btn_touch, tp.px, tp.py)) {
						s_pref_aspect = (s_pref_aspect + 1) % ASPECT_N;
						settings_set("aspect", s_pref_aspect);
						calc_output_size(p);
					}
				}
				{	/* 弹幕范围:从上屏顶部往下占多少 */
					static const char *ar[4] = { "弹幕范围:全屏",
					                             "弹幕范围:1/2",
					                             "弹幕范围:1/4",
					                             "弹幕范围:1/8" };
					if (ui_button(165, PS_Y(2), 145, PS_H, ar[s_dm_area],
					              UI_COL_SEL, btn_touch, tp.px, tp.py)) {
						s_dm_area = (s_dm_area + 1) % 4;
						settings_set("dm_area", s_dm_area);
						dm_set_area(s_dm_area);
						/* 行数是范围 x 字号一起决定的,光看档位看不出
						 * 实际剩几行 —— 打出来,免得又靠数屏幕 */
						printf("danmaku area=%d -> %d rows\n",
						       s_dm_area, dm_rows());
					}
				}
				if (ui_button(10, PS_Y(3), 145, PS_H, "调试台", UI_COL_SEL,
				              btn_touch, tp.px, tp.py))
					want_console = true;
				/* 右下角:软解才有的同步/流畅;硬解时该位置放"返回" */
				if (!p->use_mvd) {
					if (ui_button(165, PS_Y(3), 145, PS_H,
					              p->sync_mode ? "软解:同步优先" : "软解:流畅优先",
					              UI_COL_SEL, btn_touch, tp.px, tp.py))
						p->sync_mode = !p->sync_mode;
				} else if (ui_button(165, PS_Y(3), 145, PS_H, "返回", UI_COL_SEL,
				                     btn_touch, tp.px, tp.py)) {
					in_psettings = false;
				}
				{	/* 字幕状态实况:排查"开了却不显示"卡在哪一步 */
					char sb[72];
					if (sub_loading())
						snprintf(sb, sizeof(sb), "字幕:加载中…");
					else if (sub_count() > 0)
						snprintf(sb, sizeof(sb), "字幕:%s %d 行",
						         bili_subtitle_is_ai() ? "AI" : "人工",
						         sub_count());
					else if (!bili_logged_in())
						snprintf(sb, sizeof(sb), "字幕:需登录后才能获取");
					else
						snprintf(sb, sizeof(sb), "字幕:本片无中文字幕轨");
					ui_text(10, PS_Y(4) - 2, UI_SHARP, UI_COL_DIM, sb);
				}
				/* 只剩两行说明的位置了(第四行按钮吃掉一行),所以这行
				 * 得把「3D 要什么片源」和「怎么退出」并成一句。
				 * 弹幕范围/画面比例不写说明:按钮上就是当前值,点一下
				 * 上屏当场变,比一行小字管用。 */
				ui_text(10, PS_Y(4) + 18, UI_SHARP, UI_COL_DIM,
				        "3D 需左右分屏片源   B 键退出设置");
				#undef PS_Y
				#undef PS_H
			} else if (ui_console_active()) {  /* 日志页(自绘) */
				if (ui_draw_log(touched, (kHeld & KEY_TOUCH) != 0,
				                tp.px, tp.py))
					ui_bottom_debug(false);
			} else { /* 下屏触控 GUI */
				ui_begin_bottom();
				ui_text_clipped(10, 4, UI_SHARP, UI_COL_TEXT, s_cur_title, 300);
				char tbuf[80];
				/* seek 已提交但 worker 还没更新时钟的几帧里,继续显示目标
				 * 位置,否则进度条会先闪回旧位置再跳到新位置 */
				double shown_pos = dragging ? drag_pos
				                 : (p->seek_req ? p->seek_to
				                 : (seek_latch >= 0.0 ? seek_latch : clock));
				int cs = (int)shown_pos, ts = (int)p->duration;
				snprintf(tbuf, sizeof(tbuf), "%02d:%02d / %02d:%02d",
				         cs / 60, cs % 60, ts / 60, ts % 60);
				ui_text(10, 26, UI_SHARP, UI_COL_DIM, tbuf);
				ui_text(140, 26, UI_SHARP, UI_COL_DIM,
				        p->use_mvd ? "硬件解码" : "软件解码");
				/* 没声音的标记跟解码方式放同一行:这行本来就是"当前这条片子
				 * 是怎么在放的"。用醒目色 —— 静音是用户第一眼就想知道原因的事 */
				if (!p->audio_ok)
					ui_text(228, 26, UI_SHARP, UI_COL_ACCENT, "无声音");

				/* ---- 可拖动进度条 ---- */
				#define BAR_X 14.0f
				#define BAR_Y 178.0f
				#define BAR_W 292.0f
				#define BAR_H 10.0f
				if (p->duration > 0.5) {
					/* 命中区放宽到上下各 14px,方便手指/触笔 */
					bool in_bar = holding && tp.px >= BAR_X - 10 &&
					              tp.px <= BAR_X + BAR_W + 10 &&
					              tp.py >= BAR_Y - 14 && tp.py <= BAR_Y + BAR_H + 14;
					if (in_bar && !dragging && touched) dragging = true;
					if (dragging && holding) {
						float rel = (tp.px - BAR_X) / BAR_W;
						if (rel < 0) rel = 0;
						if (rel > 1) rel = 1;
						drag_pos = rel * p->duration;
					}
					if (dragging && (kUp & KEY_TOUCH)) { /* 松手 → 提交跳转 */
						dragging = false;
						if (!p->seek_req) {  /* 上次跳转还没处理完就忽略 */
							p->seek_to = drag_pos;
							__dmb();
							p->seek_req = 1;
							p->cur_pts = drag_pos;
							seek_latch = drag_pos;
							seek_latch_t0 = osGetTime();
						}
					}
					double shown = dragging ? drag_pos
					             : (p->seek_req ? p->seek_to
					             : (seek_latch >= 0.0 ? seek_latch : clock));
					float fill = (float)(shown / p->duration);
					if (fill < 0) fill = 0;
					if (fill > 1) fill = 1;
					/* 轨道 + 已播部分 + 圆头把手 */
					ui_rect(BAR_X, BAR_Y, BAR_W, BAR_H, C2D_Color32(0x3A,0x3A,0x48,0xFF));
					ui_rect(BAR_X, BAR_Y, BAR_W * fill, BAR_H, UI_COL_ACCENT);
					float hx = BAR_X + BAR_W * fill;
					float hw = dragging ? 14.0f : 10.0f;
					ui_rect(hx - hw / 2, BAR_Y - 5, hw, BAR_H + 10, UI_COL_WHITE);
					if (dragging) { /* 拖动时显示目标时间 */
						char db[24];
						int ds = (int)drag_pos;
						snprintf(db, sizeof(db), "%02d:%02d", ds / 60, ds % 60);
						float tw = ui_text_width(db, UI_SHARP);
						float bx = hx - tw / 2;
						if (bx < 4) bx = 4;
						if (bx + tw > 316) bx = 316 - tw;
						ui_rect_z(bx - 4, BAR_Y - 36, 0.6f, tw + 8, 26,
						        C2D_Color32(0, 0, 0, 0xC0));
						ui_text_z(bx, BAR_Y - 33, 0.7f, UI_SHARP, UI_COL_WHITE, db);
					}
				}
				/* 多 P 视频:这个位置放「选集」而不是「返回」。
				 * 返回本来就有 B 键(下面那行提示里写着),而选集在播放中
				 * 是没有别的入口的 —— 把唯一没有替代品的功能放在按钮上。
				 * 单 P 视频照旧显示「返回」:这时选集按钮点了也没意义。 */
				if (s_pg_n > 1) {
					if (ui_button(10, 50, 96, 40, "选集", UI_COL_SEL,
					              btn_touch, tp.px, tp.py)) {
						in_pages = true;
						/* 打开时把当前这一 P 大致居中,免得还要自己滑去找 */
						pg_scroll = (float)s_pg_cur * 34.0f - 68.0f;
						if (pg_scroll < 0) pg_scroll = 0;
						pg_drag = pg_bardrag = false;
						pg_moved = 0.0f;
						/* 【不暂停】和评论区一致:上屏照常播,下屏翻列表。
						 * 双屏机器上「上面放着、下面操作」本来就是最自然的
						 * 用法,为翻个列表把视频停掉反而多此一举。 */
					}
				} else if (ui_button(10, 50, 96, 40, "返回", UI_COL_SEL,
				                     btn_touch, tp.px, tp.py)) {
					p->quit = 1; ret = 0;
				}
				if (ui_button(112, 50, 96, 40, p->pause ? "播放" : "暂停",
				              UI_COL_SEL, btn_touch, tp.px, tp.py)) do_pause = true;
				if (ui_button(214, 50, 96, 40, "设置", UI_COL_SEL,
				              btn_touch, tp.px, tp.py))
					in_psettings = true;
				/* 这一行只放得下一条。没声音优先:3D 那条是"可以更好",
				 * 静音是"东西没按预期工作",后者更需要解释 */
				if (!p->audio_ok && p->audio_err[0])
					ui_text(10, 146, UI_SHARP, UI_COL_ACCENT, p->audio_err);
				else if (s_pref_3d != 0 && s_cur_qn == 16)
					ui_text(10, 146, UI_SHARP, UI_COL_ACCENT,
					        "3D 建议切 480P 更清晰");
				if (ui_button(10, 100, 96, 40,
				              s_pref_danmaku ? "弹幕:开" : "弹幕:关",
				              UI_COL_SEL, btn_touch, tp.px, tp.py))
					s_pref_danmaku = !s_pref_danmaku;
				if (ui_button(112, 100, 96, 40, "发弹幕",
				              UI_COL_SEL, btn_touch, tp.px, tp.py))
					want_dm_input = true;
				/* 右下角:评论区。视频不暂停 —— 双屏机器上「上屏看片、
				 * 下屏看评论」本来就是最自然的用法 */
				if (ui_button(214, 100, 96, 40, "评论",
				              UI_COL_SEL, btn_touch, tp.px, tp.py)) {
					in_comments = true;
					if (s_meta_aid && comment_count() == 0 && !comment_loading())
						comment_load_async(s_meta_aid, 1);
				}
				/* 3D 的说明挪到「设置」子页去了:主覆盖层这行是常用键位,
				 * 塞进只在开 3D 时才有意义的话会挤掉真正常用的信息。 */
				ui_text(10, 204, UI_SHARP, UI_COL_DIM, "A 暂停  B 返回");
			}
			ui_end();

			/* 字幕:等播放真正跑起来(缓冲结束)再拉,且与弹幕错开 1 秒。
			 * 与取流/弹幕并发时,3DS httpc 会把响应发错对象 */
			/* 重试而不是问一次就放弃:**AI 字幕是惰性生成的**,
			 * 刚开播时查,轨道列表往往还是空的(接口如实回「没有字幕」),
			 * 过几秒服务端把它挂上去才查得到。
			 *
			 * 时间表是「距开播的绝对时刻」,前 20 秒每 2 秒问一次
			 * (生成基本都在这段时间内完成),之后逐渐拉长兜到 2 分钟。
			 * 敢这么密是因为**每次重试只有 1 个请求** —— 方案阶梯
			 * 第一次就锁定在能用的那条,不会每轮把 412 的两条重跑一遍。 */
			static const u32 delay[] = {
				1000, 2500, 4000, 6000, 8000, 10000, 12000, 14500,
				17000, 20000, 24000, 28000, 33000, 40000, 48000,
				58000, 70000, 85000, 100000, 120000
			};
			const int SUB_TRY_MAX = (int)(sizeof(delay) / sizeof(delay[0]));
			/* 没开字幕时也探满前 20 秒的密集段 —— 用户中途打开字幕时
			 * 该有的已经在手上了,不必再等一轮。之后的稀疏兜底只有
			 * 开着字幕才继续,免得对根本没字幕的视频白敲两分钟接口 */
			int sub_try_limit = s_pref_sub ? SUB_TRY_MAX : 10;
			if (!p->buffering && s_meta_bvid[0] && s_meta_cid &&
			    sub_count() == 0 && sub_tries < sub_try_limit &&
			    !sub_loading()) {
				if (!sub_kick_t0) sub_kick_t0 = osGetTime();
				else if (osGetTime() - sub_kick_t0 > delay[sub_tries] &&
				         !dm_loading()) {
					sub_set_duration(p->duration);
					/* cid 会超过 32 位,%ld 在 3DS 上会截断成假值,
					 * 之前就是被这个假日志带偏了排查方向 */
					printf("subtitle try%d: bvid=%s cid=%u%09u\n",
					       sub_tries + 1, s_meta_bvid,
					       (unsigned)(s_meta_cid / 1000000000),
					       (unsigned)(s_meta_cid % 1000000000));
					/* 只有真的启动了才算用掉一次;被推迟时不计数,
					 * 下一帧继续问 */
					if (sub_load_async(s_meta_bvid, s_meta_aid, s_meta_cid))
						sub_tries++;
				}
			}

			/* 观看进度上报:每 15 秒一次(节流,别刷接口)。
			 * 只置标志,真正的 POST 交给 reporter 线程 —— 绝不能在这里
			 * 直接发请求,那会把整个渲染循环停掉几百毫秒(见线程处注释) */
			if (s_meta_aid && s_meta_cid && !p->pause && !p->buffering) {
				u64 now = osGetTime();
				if (now - last_report > 15000 && !s_rep_req) {
					last_report = now;
					s_rep_aid = s_meta_aid;
					s_rep_cid = s_meta_cid;
					s_rep_sec = (int)(p->clock_ms / 1000);
					__dmb();
					s_rep_req = 1;
				}
			}
			if (want_console) {   /* 自绘日志页,随时可切 */
				ui_bottom_debug(true);
				want_console = false;
			}
			if (p->quit) break;
			if (want_dm_input) {
				/* 先暂停(音画都停),再走 登录 → 输入 → 发送 */
				p->pause = 1;
				svcSleepThread(60 * 1000 * 1000LL); /* 等 worker 应用暂停 */
				bool can = true;
				if (s_login_cb && !s_login_cb()) {
					printf("dm: login required\n");
					can = false;
				}
				if (can && (!s_meta_aid || !s_meta_cid)) {
					printf("dm: no aid/cid for this video\n");
					can = false;
				}
				if (can) {
					char msg[100];
					if (ime_input("发弹幕(将显示在当前进度)", NULL,
					              msg, sizeof(msg)) && msg[0]) {
						/* 显示时间前挪 1.2s:挂在"此刻"的话弹幕起点在
						 * 屏幕右缘之外,暂停中时钟不走,永远看不见 */
						double at = (double)p->clock_ms / 1000.0 - 1.2;
						if (at < 0) at = 0;
						dm_add_local(msg, at);   /* 乐观显示,成败都给看 */
						/* 成功不打扰(弹幕本身已经飞出来了就是反馈);
						 * 只有失败才需要告诉用户原因 */
						if (bili_send_danmaku(s_meta_aid, s_meta_cid,
						                      (int)p->clock_ms, msg) != 0) {
							snprintf(s_toast, sizeof(s_toast),
							         "发送失败:%s", bili_last_error());
							s_toast_until = osGetTime() + 5000;
						}
					}
				}
				/* 保持暂停,由用户决定何时继续 */
			}
			if (do_pause) {
				p->pause = !p->pause;
				/* 记一行:暂停状态莫名其妙时,光看现象分不出是用户按的、
				 * 触屏按钮点的,还是 HOME 挂起请求引起的 */
				/* u32 在 devkitARM 上是 unsigned long,要 %lu */
				ui_trace("player: %s (clock=%lums)",
				         p->pause ? "暂停" : "继续",
				         (unsigned long)p->clock_ms);
			}
			if (p->worker_done) { ret = p->ret; break; }
		}

		/* 退出顺序很重要:先置 quit 让所有等待循环解锁,
		 * 再停下载线程(AVIO 会立刻返回 EOF),最后 join worker */
		p->quit = 1;
		p->ring.quit = 1;
		__dmb();
		/* 被系统关闭时把所有等待压到 0.5 秒:此时网络已被掐死、线程都在
		 * 往外走,再按秒计地等只是把 "Closing software" 拖长。
		 * 正常退出(B 键)保持原来的宽松超时,别为了退得快而丢状态。 */
		/* 【别指望 APTHOOK_ONEXIT 一定先到】它由 APT 事件线程分发,和主线程
		 * 的 aptMainLoop() 返回 false 之间是**竞态**:实测有时钩子先跑
		 * (shutdown 已置位,清理全程毫秒级),有时主线程先到这里 ——
		 * 标志还没置,退出补报进度那个 2 秒的 POST 就照跑,后面的 bail
		 * 判断也全部落空。所以在这儿自己问一次 APT。 */
		if (aptShouldClose()) net_shutdown_begin();
		const u64 JOIN_NS = net_is_shutting_down() ? 500000000ULL : 5000000000ULL;
		if (rep_th) {           /* 先收上报线程,它可能正卡在一次 POST 上 */
			s_rep_quit = 1;
			__dmb();
			if (R_FAILED(threadJoin(rep_th,
			                        net_is_shutting_down() ? JOIN_NS : 8000000000ULL)))
				printf("reporter join timeout\n");
			threadFree(rep_th);
		}
		ui_trace_sync("exit: join worker");
		if (R_FAILED(threadJoin(th, JOIN_NS)))
			printf("worker join timeout\n");
		threadFree(th);
		if (ret == -1) ret = p->ret;
	}

done:
	if (dl_th) {
		p->quit = 1;
		p->ring.quit = 1;
		__dmb();
		/* 先掐在途的流连接再等线程:下载线程可能正卡在 CDN 的半死连接里
		 * (连着但不给数据),不掐的话这里要干等 5 秒超时。
		 * 通过登记表掐,不直接碰 p->ns —— 那个 ctx 归下载线程所有,
		 * 它随时可能在 close/reopen,直接对它发 IPC 是竞态。 */
		net_cancel_streams();
		ui_trace_sync("exit: join downloader");
		if (R_FAILED(threadJoin(dl_th,
		                        net_is_shutting_down() ? 500000000ULL
		                                               : 5000000000ULL)))
			printf("downloader join timeout\n");
		threadFree(dl_th);
	}
	ui_trace_sync("exit: comment_free");
	comment_free();   /* 评论线程可能还在跑,收掉 */
	if (p->ring.buf) { free(p->ring.buf); p->ring.buf = NULL; }
	ui_trace_sync("exit: player_cleanup");
	player_cleanup(p);
	ui_trace_sync("exit: cleanup done");
	if (ret == -99) {
		printf("MVD unusable, retrying with software decoder\n");
		s_disable_mvd = true;
		s_mvd_fail_streak++;
		/* 递归调内部实现,否则外层会把 s_disable_mvd 清掉、死循环 */
		return player_play_inner(url, title);
	}
	/* 这次是硬解跑完的:说明 MVD 好着呢,把失败计数清零,
	 * 免得早先几次偶发把后面所有视频都钉死在软解上 */
	if (p->use_mvd && s_mvd_fail_streak) {
		printf("mvd ok again, fail streak reset\n");
		s_mvd_fail_streak = 0;
	}
	/* 退出时补报一次最终进度(否则最后 15 秒内的观看不会同步)。
	 * 被系统关闭时跳过:这是个**主线程同步 POST**,而此刻网络已被封死,
	 * 它只会白跑一趟;真要是没封死,就是又一个卡住关机的地方。 */
	if (!net_is_shutting_down() &&
	    s_meta_aid && s_meta_cid && s_player_clock_ms > 0)
		bili_report_history(s_meta_aid, s_meta_cid,
		                    (int)(s_player_clock_ms / 1000));
	ui_set_3d(false);   /* 离开播放页回到 2D(列表页不需要立体) */
	ui_trace("playback end ret=%d", ret);
	printf("playback end (%d)\n", ret);
	return ret;
}
