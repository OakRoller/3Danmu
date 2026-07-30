/* 触屏拼音输入法:下屏 QWERTY 键盘 + 词库候选
 *
 * 词库 romfs:/pinyin.dic 由 tools/gen_pinyin_dict.py 生成
 * (jieba 词频表 + pypinyin 标音,构建期按拼音+词频排好序)。
 * 中文模式敲拼音出候选,英文模式直通 ASCII。
 */
#ifndef IME_H
#define IME_H

#include <stdbool.h>
#include <stddef.h>

/* 初始化(加载词库)。失败返回 false,此时 ime_input 退化为纯英文键盘 */
bool ime_init(void);
void ime_exit(void);

/* 模态输入:自带主循环(占用上下屏),确认返回 true 并写 out,
 * 取消(B)返回 false。initial 为预填内容,可为 NULL */
bool ime_input(const char *hint, const char *initial, char *out, size_t outlen);

#endif
