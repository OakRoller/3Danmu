/* settings: 用户设置的 SD 卡持久化(sdmc:/3ds/3danmu/settings.txt)
 *
 * 只存整数,key=value 一行一条。设计取舍:
 *   - settings_set() 当场落盘 —— 文件几十字节,写盘 1~2ms,而改设置
 *     必然是主线程上的用户点击,不会打扰播放;换来的是**怎么退出都不丢**
 *     (强杀、崩溃、抠电池都无所谓),也就不用在退出路径上再挂一个
 *     "记得保存"的钩子 —— 那条路上的等待我们刚清理干净,别再往上加东西。
 *   - 未知的 key 原样保留:老版本读到新版本的文件时不会把人家的设置删了。
 */
#ifndef APP_3DANMU_SETTINGS_H
#define APP_3DANMU_SETTINGS_H

void settings_init(void);                       /* 开机读一次 */
int  settings_get(const char *key, int def);    /* 没有该项则返回 def */
void settings_set(const char *key, int val);    /* 更新并立即落盘 */

#endif
