/* Compact MD5 (RFC 1321). Little-endian hosts only (x86 / ARM 3DS both OK). */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "md5.h"

static const uint32_t K[64] = {
	0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
	0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
	0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
	0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
	0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
	0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
	0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
	0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
	0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
	0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
	0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
	0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
	0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
	0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
	0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
	0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

static const uint8_t R[64] = {
	7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
	5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
	4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
	6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21,
};

#define LROT(x,c) (((x) << (c)) | ((x) >> (32 - (c))))

void md5(const void *data, size_t len, uint8_t digest[16]) {
	uint32_t h0 = 0x67452301, h1 = 0xefcdab89, h2 = 0x98badcfe, h3 = 0x10325476;

	/* padded length: message + 0x80 + zeros so that total % 64 == 56, + 8 bytes length */
	size_t new_len = ((len + 8) / 64 + 1) * 64 - 8;
	uint8_t *msg = (uint8_t *)calloc(new_len + 8, 1);
	if (!msg) { memset(digest, 0, 16); return; }
	memcpy(msg, data, len);
	msg[len] = 0x80;
	uint64_t bits = (uint64_t)len * 8;
	memcpy(msg + new_len, &bits, 8); /* little-endian */

	for (size_t off = 0; off < new_len; off += 64) {
		uint32_t w[16];
		memcpy(w, msg + off, 64); /* little-endian load */
		uint32_t a = h0, b = h1, c = h2, d = h3;
		for (int i = 0; i < 64; i++) {
			uint32_t f, g;
			if (i < 16)      { f = (b & c) | (~b & d);  g = (uint32_t)i; }
			else if (i < 32) { f = (d & b) | (~d & c);  g = (5u * i + 1) % 16; }
			else if (i < 48) { f = b ^ c ^ d;           g = (3u * i + 5) % 16; }
			else             { f = c ^ (b | ~d);        g = (7u * i) % 16; }
			uint32_t tmp = d;
			d = c;
			c = b;
			b = b + LROT(a + f + K[i] + w[g], R[i]);
			a = tmp;
		}
		h0 += a; h1 += b; h2 += c; h3 += d;
	}
	free(msg);

	memcpy(digest + 0,  &h0, 4);
	memcpy(digest + 4,  &h1, 4);
	memcpy(digest + 8,  &h2, 4);
	memcpy(digest + 12, &h3, 4);
}

void md5_hex(const void *data, size_t len, char out[33]) {
	uint8_t d[16];
	md5(data, len, d);
	for (int i = 0; i < 16; i++)
		sprintf(out + i * 2, "%02x", d[i]);
	out[32] = 0;
}
