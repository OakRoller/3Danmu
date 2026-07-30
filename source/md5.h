#ifndef MD5_H
#define MD5_H
#include <stddef.h>
#include <stdint.h>

void md5(const void *data, size_t len, uint8_t digest[16]);
/* out must hold 33 bytes (32 hex chars + NUL), lowercase */
void md5_hex(const void *data, size_t len, char out[33]);

#endif
