/* jsonx: 轻量 JSON 访问层(基于 jsmn),支持路径查询 "data.list[3].title" */
#ifndef JSONX_H
#define JSONX_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct Json Json;

/* 解析 JSON 文本。返回 NULL 表示失败。text 在 Json 生命周期内必须保持有效。 */
Json *json_parse(const char *text, size_t len);
void  json_free(Json *j);

/* 按路径查找 token 下标。root=-1 表示从根开始。找不到返回 -1。
 * 路径语法: "data.list[3].owner.name" */
int json_find(const Json *j, int root, const char *path);

/* 取字符串(自动反转义 \uXXXX/\n 等,输出 UTF-8)。成功返回 true。 */
bool json_get_str(const Json *j, int root, const char *path, char *out, size_t outlen);
/* 取整数(支持字符串包裹的数字)。 */
bool json_get_int(const Json *j, int root, const char *path, int64_t *out);
/* 取浮点数(字幕时间戳等)。 */
bool json_get_num(const Json *j, int root, const char *path, double *out);

/* 数组操作:tok 必须是数组 token 的下标 */
int json_arr_len(const Json *j, int tok);
int json_arr_at(const Json *j, int tok, int idx);   /* 返回第 idx 个元素的 token 下标 */

#endif
