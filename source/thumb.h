/* 列表封面缩略图:后台线程下载 + mjpeg 解码 + GPU 纹理
 *
 * 用法:列表加载完调 thumb_start(),绘制时 thumb_get(i) 拿 C2D_Image
 * (没就绪返回 NULL,画占位块);翻页/进播放器前调 thumb_stop()。
 * 需要 ffmpeg 启用 mjpeg 解码器(build-ffmpeg-3ds.sh 已加,需重编 ffmpeg);
 * 解码器缺失时安静降级为无封面。
 */
#ifndef APP_3DANMU_THUMB_H
#define APP_3DANMU_THUMB_H

#include <citro2d.h>
#include "bili.h"

#define THUMB_W 96      /* 显示尺寸(B 站缩放参数同款) */
#define THUMB_H 60

void thumb_init(void);                                /* 分配常驻缓冲 */
void thumb_exit(void);
void thumb_start(const BiliVideo *list, int count);   /* 开始加载本页封面 */
void thumb_stop(void);                                /* 停止并等线程收尾 */
/* 挂起/恢复(APT 钩子里调):挂起时停止发起新的下载,恢复后自动继续。
 * 目的是缩短按 HOME 的等待 —— 详见 thumb.c 里 s_suspend 的说明。 */
void thumb_notify_suspend(int on);
const C2D_Image *thumb_get(int idx);                  /* NULL = 未就绪/无图 */
void thumb_new_frame(int budget); /* 每帧调一次:本帧允许的 GPU 上传张数 */
/* 封面缓存(SD 卡):原始 JPEG 存盘,下次同一封面直接读盘。
 * 超过 100MB 自动清空;也可由设置页手动清 */
void thumb_cache_clear(void);        /* 同步版:只在后台线程里调 */
void thumb_cache_clear_async(void);  /* 界面上一律用这个 */
bool thumb_cache_clearing(void);
int  thumb_cache_clear_pct(void);    /* 清理进度 0~100(界面显示用) */
u32  thumb_cache_kb(void);      /* 当前缓存占用(KB) */

/* 加载进度:返回是否仍在加载,done 与 total 输出完成数与总数 */
bool thumb_progress(int *done, int *total);

#endif
