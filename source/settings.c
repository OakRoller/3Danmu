#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "settings.h"

#define SETTINGS_PATH "sdmc:/3ds/3danmu/settings.txt"
#define MAX_ENTRIES 32
#define KEY_MAX     24

static struct { char key[KEY_MAX]; int val; } s_ent[MAX_ENTRIES];
static int s_n = 0;
static int s_loaded = 0;

static int find(const char *key) {
	for (int i = 0; i < s_n; i++)
		if (!strcmp(s_ent[i].key, key)) return i;
	return -1;
}

void settings_init(void) {
	s_loaded = 1;
	FILE *f = fopen(SETTINGS_PATH, "r");
	if (!f) return;                    /* 第一次运行:没有文件很正常 */
	char line[64];
	while (fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '=');
		if (!eq || eq == line) continue;
		*eq = 0;
		/* 去掉行尾换行;key 里不允许空格,值只认整数 */
		char *k = line;
		int v = atoi(eq + 1);
		size_t kl = strlen(k);
		if (kl == 0 || kl >= KEY_MAX) continue;
		if (s_n < MAX_ENTRIES && find(k) < 0) {
			snprintf(s_ent[s_n].key, KEY_MAX, "%s", k);
			s_ent[s_n].val = v;
			s_n++;
		}
	}
	fclose(f);
	printf("settings: %d entries loaded\n", s_n);
}

int settings_get(const char *key, int def) {
	int i = find(key);
	return (i >= 0) ? s_ent[i].val : def;
}

static void save(void) {
	mkdir("sdmc:/3ds", 0777);
	mkdir("sdmc:/3ds/3danmu", 0777);
	FILE *f = fopen(SETTINGS_PATH, "w");
	if (!f) { printf("settings: save failed\n"); return; }
	for (int i = 0; i < s_n; i++)
		fprintf(f, "%s=%d\n", s_ent[i].key, s_ent[i].val);
	fclose(f);
}

void settings_set(const char *key, int val) {
	if (!s_loaded) return;             /* init 之前不该有人来设 */
	int i = find(key);
	if (i >= 0) {
		if (s_ent[i].val == val) return;   /* 没变就别磨 SD 卡 */
		s_ent[i].val = val;
	} else {
		if (s_n >= MAX_ENTRIES || strlen(key) >= KEY_MAX) return;
		snprintf(s_ent[s_n].key, KEY_MAX, "%s", key);
		s_ent[s_n].val = val;
		s_n++;
	}
	save();
}
