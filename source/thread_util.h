#ifndef THREAD_UTIL_H
#define THREAD_UTIL_H

#include <3ds.h>
#include <stdio.h>

/* 收线程的唯一正确姿势。
 *
 * 【为什么值得单开一个头文件】threadFree 释放的是线程的栈和 Thread 结构。
 * 线程还在跑的时候释放它,那条线程接下来每一次压栈都在写已经还给 malloc
 * 的内存 —— 堆就此损坏。而症状要过很久才冒出来,通常是**后面某次
 * threadCreate 突然建不出线程**,并且在关掉软件之前一直好不了
 * (手动重试没用、重开就好,正是这个)。
 *
 * comment.c / danmaku.c / subtitle.c 里各自写过一遍这条规矩,而播放器的
 * 退出路径反倒漏了:join 超时后照样 threadFree。同一条规矩写在三个地方、
 * 漏在第四个地方 —— 那就该收成一处。
 *
 * join 超时说明线程没收回来,那就只能 threadDetach 丢下它:槽位和栈都还
 * 占着(泄漏),但内存是完整的,进程收尾时一起带走。泄漏一个线程比损坏
 * 整个堆便宜得多。 */
static inline void thread_reap(Thread *t, u64 timeout_ns, const char *who) {
	if (!t || !*t) return;
	if (R_FAILED(threadJoin(*t, timeout_ns))) {
		printf("%s join timeout, detaching\n", who ? who : "thread");
		threadDetach(*t);   /* 不能 threadFree,见上 */
	} else {
		threadFree(*t);
	}
	*t = NULL;
}

#endif
