/* 封面缩略图加载(实现说明见 thumb.h)
 *
 * 线程分工:
 *   loader 线程:逐张 net_get(B 站图床支持 @96w_60h_1c.jpg 缩放参数,
 *   每张 5-15KB)→ ffmpeg mjpeg 解码 → sws 转 RGB565 写入常驻 staging
 *   缓冲(行距 = 纹理宽 128)→ 置 ready
 *   主线程:thumb_get() 看到 ready 才做 GPU 上传(GX 传输必须在主线程)
 *
 * 纹理 20 张 128x64 RGB565 常驻复用(每张 16KB,共 320KB linear)。
 */
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>

#include "thumb.h"
#include "net.h"
#include "ui.h"   /* printf 要经 ui_printf 才进得了调试台环形缓冲 */

#define THUMB_CACHE_DIR "sdmc:/3ds/3danmu/thumbs"
#define SLOTS    20
#define TEX_W    128
#define TEX_H    64

/* 槽状态 */
enum { TS_EMPTY = 0, TS_PENDING, TS_READY, TS_UPLOADED, TS_NONE };

static C3D_Tex s_tex[SLOTS];
static bool s_tex_ok[SLOTS];
static Tex3DS_SubTexture s_sub[SLOTS];
static C2D_Image s_img[SLOTS];
static u16 *s_stage[SLOTS];              /* linear, TEX_W*TEX_H */
static volatile u8 s_state[SLOTS];
static char s_url[SLOTS][224];
static int s_n = 0;
/* 2 路。**别再往上加了**:实测把请求改成真并发(3 路)既没有提速,
 * 又让字幕串台小概率复发 —— 瓶颈在 3DS 的 httpc 服务内部,
 * 它本来就是一个一个处理请求的,加路数只是让底层更容易出错。
 * 保留 2 路仅仅是让「解码」和「下一张的请求」重叠一点点。 */
#define NLOADERS 2
static Thread s_threads[NLOADERS];
static volatile int s_quit = 0;
/* 【按 HOME 时暂停发新请求】
 * 现象:封面正在下载时按 HOME,要卡好几秒才弹出主菜单;下载完了再按就很快。
 * 原因不在本程序 —— 挂起时系统要收走无线,而 httpc 里在途的请求没法中途
 * 取消,只能等它自己结束。多路 loader 同时在跑,就要连着等好几个。
 * 能做的是**别再往里加新的**:挂起期间线程停在发请求之前,
 * 于是最多只等当前这一个。恢复后自动继续,封面不会丢。
 * (彻底解决要能 httpcCancelConnection,得由 net_get_img 持有并暴露句柄。) */
static volatile int s_suspend = 0;

void thumb_notify_suspend(int on) {
	s_suspend = on ? 1 : 0;
	/* 光"不发新的"还不够 —— 已经发出去的那一个照样要等它跑完。
	 * httpc 请求既没有超时、也不响应任何标志位,唯一的办法是从外面
	 * 把连接掐掉。封面是尽力而为的,掐了下面会重来,不会永久少图。 */
	if (s_suspend) net_cancel_img();
}
/* 缓存扫描线程单独一套开关。
 * 不能复用 s_quit —— 那个每次切列表都会被 thumb_stop 置位,而扫描
 * 整个运行期只该做一次,被打断就永远统计不出磁盘占用。 */
static Thread s_scan_th = NULL;
static volatile int s_scan_quit = 0;
static volatile int s_next = 0;   /* 待领取的下一个槽(原子分发) */
static int s_uploads_left = 1;   /* 每帧上传预算:防多张同帧同步传输造成卡顿 */
/* 计时:整页封面到底慢在哪。req = 等网络(含锁),dec = 解码 + 缩放。
 * 两者一比就知道该不该动那把全局锁 —— 若 req 占绝对大头,
 * 说明确实卡在「请求被串行化」上;若 dec 也不小,那是 CPU 的事。 */
static volatile u32 s_t_req, s_t_dec, s_t_bytes, s_t_hit;
static u64 s_t_start;
static bool s_t_done;

static void cache_scan_kick(void);   /* 定义在下面的缓存小节里 */

void thumb_init(void) {
	mkdir("sdmc:/3ds", 0777);
	mkdir("sdmc:/3ds/3danmu", 0777);
	mkdir(THUMB_CACHE_DIR, 0777);
	cache_scan_kick();   /* 后台统计缓存占用,不阻塞任何人 */
	for (int i = 0; i < SLOTS; i++) {
		if (!s_stage[i])
			s_stage[i] = (u16 *)linearAlloc(TEX_W * TEX_H * 2);
		s_state[i] = TS_EMPTY;
	}
}

void thumb_exit(void) {
	thumb_stop();
	/* 缓存扫描线程要在这里收干净:它在做文件系统调用,而 main 返回后
	 * 运行时会把文件系统拆掉。只在 thumb_exit 收,不在 thumb_stop 收 ——
	 * 后者每次切列表都会调,扫描不该被打断 */
	s_scan_quit = 1;
	__dmb();
	if (s_scan_th) {
		if (R_FAILED(threadJoin(s_scan_th, 5000000000ULL)))
			printf("thumb: cache scan join timeout\n");
		threadFree(s_scan_th);
		s_scan_th = NULL;
	}
	for (int i = 0; i < SLOTS; i++) {
		if (s_stage[i]) { linearFree(s_stage[i]); s_stage[i] = NULL; }
		if (s_tex_ok[i]) { C3D_TexDelete(&s_tex[i]); s_tex_ok[i] = false; }
	}
}

/* ---------- SD 卡封面缓存 ----------
 *
 * 实测:单张封面 2.3KB,请求却要几百毫秒,而且 rd=0 fb=0 —— 没有重定向、
 * 没有 TLS,纯粹是 3DS 网络栈的往返延迟。加线程/加并发都无效
 * (httpc 服务内部本就串行,见 net_get_img 的注释)。
 *
 * 既然「每次往返就是这么久」改不了,那就**别再发这个请求**:
 * 把原始 JPEG 存到 SD,下次同一个封面直接读盘。SD 读 2KB 是几毫秒级,
 * 比几百毫秒的往返快两个数量级。
 *
 * 收益场景很实在:翻回上一页、看完视频退回列表、重开 App、
 * 推荐流里反复出现的那些视频 —— 全部变成秒出。
 * 首次见到的封面依然要走网络,这个没办法。
 *
 * 文件名用 URL 的 64 位 FNV 哈希(两个不同种子拼起来),
 * 碰撞概率低到可以忽略;真撞上了也只是显示错一张封面,不影响功能。 */
static void cache_path(const char *url, char *out, size_t n) {
	uint32_t h1 = 2166136261u, h2 = 0x811c9dc5u ^ 0x5bf03635u;
	for (const unsigned char *p = (const unsigned char *)url; *p; p++) {
		h1 = (h1 ^ *p) * 16777619u;
		h2 = (h2 + *p) * 2654435761u;
	}
	snprintf(out, n, THUMB_CACHE_DIR "/%08lx%08lx.jpg",
	         (unsigned long)h1, (unsigned long)h2);
}

/* 缓存总大小上限。超了就整体清空 —— 不做 LRU:
 * 要挑「最久没用的」就得记访问时间,而 3DS 上每次读写都改一次 mtime
 * 又是额外的 SD 开销。按 2.3KB/张算,100MB 能存四万多张,
 * 真到那一步说明用了很久,清空重来完全可以接受。 */
#define CACHE_MAX_BYTES (100u * 1024 * 1024)
static volatile u32 s_cache_bytes = 0;
static volatile int s_cache_scanned = 0;

/* 遍历缓存目录统计总大小。**必须在独立线程里做,绝不能挂在下载路径上**。
 *
 * 第一版是「第一次 cache_write 时顺手扫一遍」,结果:两个 loader 线程中,
 * 拿到 0 号槽的那个总是最先下载完、最先调 cache_write,于是**它被扫描
 * 卡住,另一个线程趁机把后面的图全拉完了** —— 表现为「第一张封面
 * 永远最后出来」,而且稳定复现。目录里文件越多卡得越久(要逐个 stat)。
 *
 * 教训:任何「顺手做一下」的初始化,只要耗时不确定,就不能放在
 * 会被并发路径命中的地方 —— 它会把恰好第一个到达的那条路拖垮。 */
static void cache_scan_thread(void *arg) {
	(void)arg;
	u32 total = 0;
	DIR *d = opendir(THUMB_CACHE_DIR);
	if (d) {
		struct dirent *e;
		char path[320];   /* d_name 上界 255,按接口上界给 */
		while ((e = readdir(d)) != NULL) {
			if (e->d_name[0] == '.') continue;
			if (s_scan_quit) { closedir(d); return; }   /* 退出:立刻收手 */
			snprintf(path, sizeof(path), THUMB_CACHE_DIR "/%s", e->d_name);
			struct stat st;
			if (stat(path, &st) == 0) total += (u32)st.st_size;
		}
		closedir(d);
	}
	s_cache_bytes = total;
	__dmb();
	s_cache_scanned = 2;      /* 2 = 扫完,容量上限从此生效 */
	printf("thumb cache: %dKB on disk\n", (int)(total / 1024));
}

/* 启动一次后台扫描(整个运行期只做一次) */
static void cache_scan_kick(void) {
	if (s_cache_scanned) return;
	s_cache_scanned = 1;
	static const int cores[] = { 3, 2, -2 };
	/* 【必须可 join】以前这里建的是 detached 线程,退出时没人等它 ——
	 * 按 START 退出后 main 返回、运行时把文件系统拆掉,而它还卡在
	 * readdir/stat 里,于是 data abort(读 NULL+0x44,core 3)。
	 * detached 的含义只是"不用 threadFree",不是"可以不等它结束"。 */
	for (int i = 0; i < 3 && !s_scan_th; i++)
		s_scan_th = threadCreate(cache_scan_thread, NULL, 16 * 1024, 0x3B,
		                         cores[i], false);
	if (!s_scan_th) s_cache_scanned = 2;   /* 建不了就当扫过(容量按 0 起算) */
}

void thumb_cache_clear(void) {
	DIR *d = opendir(THUMB_CACHE_DIR);
	if (d) {
		struct dirent *e;
		char path[320];       /* 同上:按 d_name 的 255 字节上界给 */
		while ((e = readdir(d)) != NULL) {
			if (e->d_name[0] == '.') continue;
			snprintf(path, sizeof(path), THUMB_CACHE_DIR "/%s", e->d_name);
			remove(path);
		}
		closedir(d);
	}
	s_cache_bytes = 0;
	s_cache_scanned = 2;   /* 刚清空,总量确定为 0 —— 是「已知」而非「扫描中」 */
	printf("thumb cache cleared\n");
}

/* 设置页每帧都会调它显示占用 —— 这里绝不能扫盘,只报当前已知值 */
u32 thumb_cache_kb(void) {
	cache_scan_kick();
	return s_cache_bytes / 1024;
}

/* 命中返回字节数(数据写进 buf),未命中返回 0 */
static size_t cache_read(const char *url, u8 *buf, size_t cap) {
	char path[128];
	cache_path(url, path, sizeof(path));
	FILE *f = fopen(path, "rb");
	if (!f) return 0;
	size_t n = fread(buf, 1, cap, f);
	fclose(f);
	return (n > 4) ? n : 0;   /* 太小说明是坏文件,当没命中 */
}

static void cache_write(const char *url, const u8 *data, size_t len) {
	if (len < 4 || len > 64 * 1024) return;
	/* 只有扫描完成后才谈容量上限。没扫完就写,顶多是这一小段时间里
	 * 不设防 —— 相比「把下载线程卡住」,这个代价可以忽略 */
	if (s_cache_scanned == 2 && s_cache_bytes + len > CACHE_MAX_BYTES) {
		printf("thumb cache full (%dMB), clearing\n",
		       (int)(s_cache_bytes / (1024 * 1024)));
		thumb_cache_clear();
	}
	char path[128];
	cache_path(url, path, sizeof(path));
	FILE *f = fopen(path, "wb");
	if (!f) return;
	if (fwrite(data, 1, len, f) == len)
		__sync_fetch_and_add(&s_cache_bytes, (u32)len);
	fclose(f);
}

/* ---------- 解码(loader 线程) ---------- */

/* 每个 loader 线程持有一套可复用的解码资源。
 *
 * 原先是**每张图**都新建 AVCodecContext + avcodec_open2 + sws_getContext,
 * 用完全部销毁。这三件事都是大块 malloc + 建表(mjpeg 的 VLC 表、
 * swscale 的缩放系数表),一张图一轮。newlib 的堆是全进程一把锁,
 * 两个 loader 同时在那儿申请释放,主线程哪怕只是路过一次分配也会被挡住,
 * 表现就是滚动中「偶尔卡一小下」。
 * 封面尺寸固定(图床按 @96w_60h 返回),上下文完全可以一直用下去。 */
typedef struct {
	AVCodecContext *c;
	AVFrame *fr;
	AVPacket *pkt;
	struct SwsContext *sws;
	int sws_w, sws_h, sws_fmt;   /* sws 建表所依据的源参数 */
	bool bad;                    /* 初始化失败过,不再重试 */
	u8 cbuf[24 * 1024];          /* 读缓存文件用(线程私有,别放栈上) */
} JpegDec;

static void jd_free(JpegDec *jd) {
	if (jd->sws) { sws_freeContext(jd->sws); jd->sws = NULL; }
	if (jd->fr) av_frame_free(&jd->fr);
	if (jd->pkt) { jd->pkt->data = NULL; jd->pkt->size = 0;
	               av_packet_free(&jd->pkt); }
	if (jd->c) avcodec_free_context(&jd->c);
}

static bool jd_init(JpegDec *jd) {
	if (jd->c) return true;
	if (jd->bad) return false;
	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
	if (!codec) {
		printf("thumb: mjpeg decoder missing (rebuild ffmpeg)\n");
		jd->bad = true;
		return false;
	}
	jd->c = avcodec_alloc_context3(codec);
	if (!jd->c || avcodec_open2(jd->c, codec, NULL) < 0) goto fail;
	jd->fr = av_frame_alloc();
	jd->pkt = av_packet_alloc();
	if (!jd->fr || !jd->pkt) goto fail;
	return true;
fail:
	jd_free(jd);
	jd->bad = true;
	return false;
}

static bool decode_jpeg_to_stage(JpegDec *jd, const u8 *data, size_t len,
                                 u16 *dst) {
	if (!jd_init(jd)) return false;
	/* 复用上下文:每张图之间必须清空内部状态,否则残留会影响下一张 */
	avcodec_flush_buffers(jd->c);
	av_frame_unref(jd->fr);
	jd->pkt->data = (u8 *)data;      /* 只读引用,不接管所有权 */
	jd->pkt->size = (int)len;
	bool ok = false;
	uint8_t *dstp[1] = { (uint8_t *)dst };   /* 提到 goto 之前声明 */
	int stride[1] = { TEX_W * 2 };
	if (avcodec_send_packet(jd->c, jd->pkt) < 0) goto out;
	if (avcodec_receive_frame(jd->c, jd->fr) < 0) goto out;

	/* 源参数变了才重建缩放表(实际上封面尺寸固定,一次都不会重建) */
	if (!jd->sws || jd->sws_w != jd->fr->width ||
	    jd->sws_h != jd->fr->height || jd->sws_fmt != jd->fr->format) {
		if (jd->sws) sws_freeContext(jd->sws);
		jd->sws = sws_getContext(
			jd->fr->width, jd->fr->height, (enum AVPixelFormat)jd->fr->format,
			THUMB_W, THUMB_H, AV_PIX_FMT_RGB565LE,
			SWS_FAST_BILINEAR, NULL, NULL, NULL);
		jd->sws_w = jd->fr->width;
		jd->sws_h = jd->fr->height;
		jd->sws_fmt = jd->fr->format;
	}
	if (!jd->sws) goto out;
	sws_scale(jd->sws, (const uint8_t * const *)jd->fr->data,
	          jd->fr->linesize, 0, jd->fr->height, dstp, stride);
	ok = true;
out:
	jd->pkt->data = NULL;
	jd->pkt->size = 0;
	return ok;
}

static void loader_main(void *arg) {
	(void)arg;
	JpegDec jd;                 /* 本线程私有,不能用 static(两个线程会打架) */
	memset(&jd, 0, sizeof(jd));
	for (;;) {
		/* 挂起期间不领新活。注意要放在领任务**之前** —— 放在后面的话
		 * 任务已经被原子取走,睡在这儿等于把它扣住不做。 */
		while (s_suspend && !s_quit && !net_is_shutting_down())
			svcSleepThread(30 * 1000 * 1000LL);
		int i = __sync_fetch_and_add(&s_next, 1);   /* 原子领任务 */
		if (i >= s_n || s_quit) break;
		if (s_state[i] != TS_PENDING) continue;
		if (!s_url[i][0]) { s_state[i] = TS_NONE; continue; }
		/* 提速关键:图床走 HTTP——3DS 上 TLS 握手要几百毫秒,20 张
		 * 串行光握手就十几秒;封面是公开内容,明文无妨。失败回落 HTTPS */
		char url[256];
		const char *u = s_url[i];
		if (!strncmp(u, "https://", 8))
			snprintf(url, sizeof(url), "http://%.215s@%dw_%dh_1c.jpg",
			         u + 8, THUMB_W, THUMB_H);
		else
			snprintf(url, sizeof(url), "%.223s@%dw_%dh_1c.jpg",
			         u, THUMB_W, THUMB_H);
		/* 先查 SD 缓存:命中就完全跳过网络 */
		u64 tq0 = osGetTime();
		size_t clen = cache_read(url, jd.cbuf, sizeof(jd.cbuf));
		if (clen) {
			bool cok = decode_jpeg_to_stage(&jd, jd.cbuf, clen, s_stage[i]);
			__sync_fetch_and_add(&s_t_hit, 1);
			__sync_fetch_and_add(&s_t_req, (u32)(osGetTime() - tq0));
			if (cok) {
				GSPGPU_FlushDataCache(s_stage[i], TEX_W * TEX_H * 2);
				__dmb();
				s_state[i] = TS_READY;
				continue;             /* 命中就到此为止,连 sleep 都不用 */
			}
			/* 缓存文件坏了:当没命中,照常走网络 */
		}

		HttpResponse res;
		bool fb = false;
		int r;
		for (;;) {
			r = net_get_img(url, &res);
			if ((r != 0 || res.status != 200) && !s_quit) {
				if (r == 0) net_response_free(&res);
				snprintf(url, sizeof(url), "%.223s@%dw_%dh_1c.jpg",
				         s_url[i], THUMB_W, THUMB_H);
				fb = true;
				r = net_get_img(url, &res);
			}
			/* 【被挂起掐断的不算失败】按 HOME 时 net_cancel_img 会让在途请求
			 * 立刻返回错误。要是就此 TS_NONE,每按一次 HOME 就永久少一张封面 ——
			 * 那等于拿"少张图"换"少等两秒",不划算。等恢复了重来一次。
			 * URL 要复原:上面的 https 回落已经把它改写过了。 */
			if (r != 0 && s_suspend && !s_quit && !net_is_shutting_down()) {
				while (s_suspend && !s_quit && !net_is_shutting_down())
					svcSleepThread(30 * 1000 * 1000LL);
				if (s_quit) break;
				const char *uu = s_url[i];
				if (!strncmp(uu, "https://", 8))
					snprintf(url, sizeof(url), "http://%.215s@%dw_%dh_1c.jpg",
					         uu + 8, THUMB_W, THUMB_H);
				else
					snprintf(url, sizeof(url), "%.223s@%dw_%dh_1c.jpg",
					         uu, THUMB_W, THUMB_H);
				fb = false;
				continue;
			}
			break;
		}
		u32 dtq = (u32)(osGetTime() - tq0);
		__sync_fetch_and_add(&s_t_req, dtq);
		/* 前 4 张打明细:单张 2.3KB 却要几百毫秒,得知道花在哪。
		 * fb=1 说明 http 那次没成、回落到了 https(那就是每张一次
		 * TLS 握手);rd 是跟随重定向的次数(http→https 也会计到这里) */
		if (i < 4)
			printf("thumb[%d] %dms st=%d fb=%d rd=%d len=%d\n", i, (int)dtq,
			       (r == 0) ? res.status : -1, fb ? 1 : 0,
			       g_net_last_redirects, (r == 0) ? (int)res.len : 0);
		if (r != 0) { s_state[i] = TS_NONE; continue; }
		__sync_fetch_and_add(&s_t_bytes, (u32)res.len);
		u64 td0 = osGetTime();
		bool ok = false;
		if (res.status == 200 && res.data && res.len > 4)
			ok = decode_jpeg_to_stage(&jd, (const u8 *)res.data, res.len,
			                          s_stage[i]);
		__sync_fetch_and_add(&s_t_dec, (u32)(osGetTime() - td0));
		if (ok) cache_write(url, (const u8 *)res.data, res.len);
		net_response_free(&res);
		if (ok) {
			GSPGPU_FlushDataCache(s_stage[i], TEX_W * TEX_H * 2);
			__dmb();
			s_state[i] = TS_READY;
		} else {
			s_state[i] = TS_NONE;
		}
		/* 从 8ms 降到 1ms。当初插这个睡眠(以及把优先级压到 0x3E)是为了
		 * 治「列表滚动卡顿」—— 后来查明真凶是 ui_text_clipped 每帧几十万次
		 * 字形解析,跟封面线程毫无关系。那个 bug 修好之后,这里的限速就是
		 * 在给一个已经不存在的问题交税,白白让封面慢好几秒。 */
		svcSleepThread(1 * 1000 * 1000LL);
	}
	jd_free(&jd);   /* 本页封面拉完才释放,不是每张一次 */
}

/* ---------- 对外(主线程) ---------- */

void thumb_start(const BiliVideo *list, int count) {
	thumb_stop();
	thumb_init();
	if (count > SLOTS) count = SLOTS;
	s_n = count;
	for (int i = 0; i < count; i++) {
		snprintf(s_url[i], sizeof(s_url[i]), "%s", list[i].pic);
		s_state[i] = TS_PENDING;
	}
	s_quit = 0;
	s_next = 0;
	s_t_req = s_t_dec = s_t_bytes = s_t_hit = 0;
	s_t_start = osGetTime();
	s_t_done = false;
	/* 封面是**纯延迟受限**:20 张共 46KB 却要 16.5 秒请求时间,
	 * 解码只占 88ms。而加并发路数无效(见 net_get_img 注释),
	 * 所以下面这两个线程实际仍是串行发请求的。 */
	/* 绝不落 core1:GSP/HID 等系统服务在那跑,解码挤过去主线程必卡;
	 * 而且列表页没申请 core1 应用时间片。New3DS 用空闲的 core2/3,
	 * 老机型回落主核(-2)低优先级,靠调度让路 */
	static const int cores[NLOADERS][2] = { { 2, -2 }, { 3, -2 } };
	/* 老机型没有 core2/3,两个 loader 都会回落到主线程那颗核上,
	 * 互相抢还连累 UI —— 那就只开一个,老老实实串行拉 */
	bool n3 = false;
	APT_CheckNew3DS(&n3);
	int nloaders = n3 ? NLOADERS : 1;
	int made = 0;
	for (int t = 0; t < NLOADERS; t++) s_threads[t] = NULL;
	for (int t = 0; t < nloaders; t++) {
		for (int c = 0; c < 2 && !s_threads[t]; c++)
			/* 0x3A:比主线程(0x30)低,但不再垫底。它们跑在 core2/3 上,
			 * 压太低只是拖慢自己,对 UI 流畅度并无帮助 */
			/* 栈从 48KB 提到 96KB:JpegDec 里带了 24KB 的缓存读缓冲,
			 * 而它是线程局部变量,48KB 会溢出 */
			s_threads[t] = threadCreate(loader_main, NULL, 96 * 1024,
			                            0x3A, cores[t][c], false);
		if (s_threads[t]) made++;
	}
	if (!made)
		for (int i = 0; i < count; i++) s_state[i] = TS_NONE;
}

void thumb_stop(void) {
	s_quit = 1;
	/* 【光置标志位收不掉线程】loader 可能正卡在 net_get_img 里,而 httpc
	 * 请求没有超时也不看任何标志 —— 它要等服务器回话或连接自己断掉,
	 * 最坏几十秒。于是 threadJoin 一起干等,表现就是**退出软件时卡住**
	 * (退出、翻页、进播放器都会调到这里)。
	 * 唯一能打断它的是从外面掐连接。
	 *
	 * 循环着掐:置 s_quit 到线程真正发出请求之间有个窗口,单掐一次可能
	 * 掐了个空,回头它又发了一个出去。join 用短超时轮询,掐到收工为止。 */
	for (int t = 0; t < NLOADERS; t++) {
		if (!s_threads[t]) continue;
		bool joined = false;
		for (int k = 0; k < 40 && !joined; k++) {      /* 最多 ~8 秒 */
			net_cancel_img();
			joined = R_SUCCEEDED(threadJoin(s_threads[t], 200000000ULL));
		}
		if (!joined) {
			/* 真收不回来就别再等了:线程只碰自己的槽位和只读的 s_url,
			 * 让它挂着比把整个程序卡死强。thumb_exit 不释放这些缓冲。 */
			printf("thumb: loader %d stuck, detaching\n", t);
			threadDetach(s_threads[t]);
			s_threads[t] = NULL;
			continue;
		}
		threadFree(s_threads[t]);
		s_threads[t] = NULL;
	}
	for (int i = 0; i < SLOTS; i++)
		if (s_state[i] == TS_PENDING) s_state[i] = TS_EMPTY;
}

void thumb_new_frame(int budget) { s_uploads_left = budget; }

bool thumb_progress(int *done, int *total) {
	int d = 0;
	for (int i = 0; i < s_n; i++)
		if (s_state[i] == TS_UPLOADED || s_state[i] == TS_READY ||
		    s_state[i] == TS_NONE)
			d++;
	if (done) *done = d;
	if (total) *total = s_n;
	/* 整页拉完报一次账。wall 是墙钟总时长,req+dec 是各线程耗时之和 ——
	 * 若 req 之和 ≈ wall,说明请求确实是一个接一个跑的(锁把并行掐了);
	 * 若 req 之和 ≈ 2*wall,说明两个线程真的重叠上了 */
	if (!s_t_done && s_n > 0 && d >= s_n) {
		s_t_done = true;
		u32 wall = (u32)(osGetTime() - s_t_start);
		printf("thumb: %d imgs in %dms (cache %d/%d, req=%d dec=%d, %dKB)\n",
		       s_n, (int)wall, (int)s_t_hit, s_n, (int)s_t_req,
		       (int)s_t_dec, (int)(s_t_bytes / 1024));
	}
	bool any = false;
	for (int t = 0; t < NLOADERS; t++) if (s_threads[t]) any = true;
	return any && d < s_n;
}

const C2D_Image *thumb_get(int idx) {
	if (idx < 0 || idx >= s_n) return NULL;
	if (s_state[idx] == TS_READY && s_uploads_left > 0) {
		s_uploads_left--;
		/* GPU 上传只能在主线程做(GX 队列非线程安全) */
		if (!s_tex_ok[idx]) {
			if (!C3D_TexInit(&s_tex[idx], TEX_W, TEX_H, GPU_RGB565))
				{ s_state[idx] = TS_NONE; return NULL; }
			C3D_TexSetFilter(&s_tex[idx], GPU_LINEAR, GPU_LINEAR);
			s_tex_ok[idx] = true;
		}
		C3D_SyncDisplayTransfer(
			(u32 *)s_stage[idx], GX_BUFFER_DIM(TEX_W, TEX_H),
			(u32 *)s_tex[idx].data, GX_BUFFER_DIM(TEX_W, TEX_H),
			/* 与 player 同款传输参数 + 同款 subtex 方向(真机验证过) */
			(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(1) |
			 GX_TRANSFER_RAW_COPY(0) |
			 GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565) |
			 GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) |
			 GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO)));
		s_sub[idx].width = THUMB_W;
		s_sub[idx].height = THUMB_H;
		s_sub[idx].left = 0.0f;
		s_sub[idx].right = (float)THUMB_W / TEX_W;
		s_sub[idx].top = 1.0f;
		s_sub[idx].bottom = 1.0f - (float)THUMB_H / TEX_H;
		s_img[idx].tex = &s_tex[idx];
		s_img[idx].subtex = &s_sub[idx];
		s_state[idx] = TS_UPLOADED;
	}
	return (s_state[idx] == TS_UPLOADED) ? &s_img[idx] : NULL;
}
