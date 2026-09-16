#ifndef PIGGLE_DIGEST_HPP
#define PIGGLE_DIGEST_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct pg_md5 {
	uint32_t state[4];
	uint64_t length;
	uint8_t block[64];
	size_t used;
};

static inline uint32_t pg_md5_rotate(uint32_t value, unsigned shift)
{
	return (value << shift) | (value >> (32 - shift));
}

static inline void pg_md5_block(pg_md5 *hash, const uint8_t *block)
{
	static const uint32_t constants[64] = {
		0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu,
		0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
		0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
		0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
		0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
		0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
		0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
		0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
		0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
		0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
		0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u,
		0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
		0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u,
		0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
		0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
		0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u,
	};
	static const uint8_t shifts[64] = {
		7, 12, 17, 22, 7, 12, 17, 22,
		7, 12, 17, 22, 7, 12, 17, 22,
		5, 9, 14, 20, 5, 9, 14, 20,
		5, 9, 14, 20, 5, 9, 14, 20,
		4, 11, 16, 23, 4, 11, 16, 23,
		4, 11, 16, 23, 4, 11, 16, 23,
		6, 10, 15, 21, 6, 10, 15, 21,
		6, 10, 15, 21, 6, 10, 15, 21,
	};
	uint32_t words[16];
	uint32_t a = hash->state[0];
	uint32_t b = hash->state[1];
	uint32_t c = hash->state[2];
	uint32_t d = hash->state[3];
	unsigned i;

	for (i = 0; i < 16; i++) {
		const uint8_t *at = block + 4 * i;

		words[i] = (uint32_t)at[0] | ((uint32_t)at[1] << 8) |
			((uint32_t)at[2] << 16) | ((uint32_t)at[3] << 24);
	}
	for (i = 0; i < 64; i++) {
		uint32_t function;
		unsigned index;
		uint32_t previous = d;

		if (i < 16) {
			function = (b & c) | (~b & d);
			index = i;
		} else if (i < 32) {
			function = (d & b) | (~d & c);
			index = (5 * i + 1) & 15;
		} else if (i < 48) {
			function = b ^ c ^ d;
			index = (3 * i + 5) & 15;
		} else {
			function = c ^ (b | ~d);
			index = (7 * i) & 15;
		}
		d = c;
		c = b;
		b += pg_md5_rotate(a + function + constants[i] +
			words[index], shifts[i]);
		a = previous;
	}
	hash->state[0] += a;
	hash->state[1] += b;
	hash->state[2] += c;
	hash->state[3] += d;
}

static inline void pg_md5_init(pg_md5 *hash)
{
	hash->state[0] = 0x67452301u;
	hash->state[1] = 0xefcdab89u;
	hash->state[2] = 0x98badcfeu;
	hash->state[3] = 0x10325476u;
	hash->length = 0;
	hash->used = 0;
}

static inline void pg_md5_update(pg_md5 *hash, const void *input, size_t size)
{
	const uint8_t *bytes = (const uint8_t *)input;

	hash->length += size;
	while (size) {
		size_t amount = sizeof(hash->block) - hash->used;

		if (amount > size)
			amount = size;
		memcpy(hash->block + hash->used, bytes, amount);
		hash->used += amount;
		bytes += amount;
		size -= amount;
		if (hash->used == sizeof(hash->block)) {
			pg_md5_block(hash, hash->block);
			hash->used = 0;
		}
	}
}

static inline void pg_md5_finish(pg_md5 *hash, uint8_t digest[16])
{
	uint64_t bits = hash->length * 8;
	uint8_t suffix[64] = { 0x80 };
	uint8_t length[8];
	size_t padding = hash->used < 56 ? 56 - hash->used :
		120 - hash->used;
	unsigned i;

	for (i = 0; i < 8; i++)
		length[i] = (uint8_t)(bits >> (8 * i));
	pg_md5_update(hash, suffix, padding);
	pg_md5_update(hash, length, sizeof(length));
	for (i = 0; i < 16; i++)
		digest[i] = (uint8_t)(hash->state[i / 4] >> (8 * (i % 4)));
}

#endif
