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

static inline uint32_t pg_md5_step(uint32_t a, uint32_t b,
	uint32_t function, uint32_t word, uint32_t constant, unsigned shift)
{
	return b + pg_md5_rotate(a + function + word + constant, shift);
}

static inline void pg_md5_block(pg_md5 *hash, const uint8_t *block)
{
	uint32_t words[16];
	uint32_t a = hash->state[0];
	uint32_t b = hash->state[1];
	uint32_t c = hash->state[2];
	uint32_t d = hash->state[3];

	for (unsigned i = 0; i < 16; i++) {
		const uint8_t *at = block + 4 * i;

		words[i] = (uint32_t)at[0] | ((uint32_t)at[1] << 8) |
			((uint32_t)at[2] << 16) | ((uint32_t)at[3] << 24);
	}
	/* Fixed rounds expose constant rotations and word indices. */
	a = pg_md5_step(a, b, (b & c) | (~b & d), words[0],
		0xd76aa478u, 7);
	d = pg_md5_step(d, a, (a & b) | (~a & c), words[1],
		0xe8c7b756u, 12);
	c = pg_md5_step(c, d, (d & a) | (~d & b), words[2],
		0x242070dbu, 17);
	b = pg_md5_step(b, c, (c & d) | (~c & a), words[3],
		0xc1bdceeeu, 22);
	a = pg_md5_step(a, b, (b & c) | (~b & d), words[4],
		0xf57c0fafu, 7);
	d = pg_md5_step(d, a, (a & b) | (~a & c), words[5],
		0x4787c62au, 12);
	c = pg_md5_step(c, d, (d & a) | (~d & b), words[6],
		0xa8304613u, 17);
	b = pg_md5_step(b, c, (c & d) | (~c & a), words[7],
		0xfd469501u, 22);
	a = pg_md5_step(a, b, (b & c) | (~b & d), words[8],
		0x698098d8u, 7);
	d = pg_md5_step(d, a, (a & b) | (~a & c), words[9],
		0x8b44f7afu, 12);
	c = pg_md5_step(c, d, (d & a) | (~d & b), words[10],
		0xffff5bb1u, 17);
	b = pg_md5_step(b, c, (c & d) | (~c & a), words[11],
		0x895cd7beu, 22);
	a = pg_md5_step(a, b, (b & c) | (~b & d), words[12],
		0x6b901122u, 7);
	d = pg_md5_step(d, a, (a & b) | (~a & c), words[13],
		0xfd987193u, 12);
	c = pg_md5_step(c, d, (d & a) | (~d & b), words[14],
		0xa679438eu, 17);
	b = pg_md5_step(b, c, (c & d) | (~c & a), words[15],
		0x49b40821u, 22);

	a = pg_md5_step(a, b, (d & b) | (~d & c), words[1],
		0xf61e2562u, 5);
	d = pg_md5_step(d, a, (c & a) | (~c & b), words[6],
		0xc040b340u, 9);
	c = pg_md5_step(c, d, (b & d) | (~b & a), words[11],
		0x265e5a51u, 14);
	b = pg_md5_step(b, c, (a & c) | (~a & d), words[0],
		0xe9b6c7aau, 20);
	a = pg_md5_step(a, b, (d & b) | (~d & c), words[5],
		0xd62f105du, 5);
	d = pg_md5_step(d, a, (c & a) | (~c & b), words[10],
		0x02441453u, 9);
	c = pg_md5_step(c, d, (b & d) | (~b & a), words[15],
		0xd8a1e681u, 14);
	b = pg_md5_step(b, c, (a & c) | (~a & d), words[4],
		0xe7d3fbc8u, 20);
	a = pg_md5_step(a, b, (d & b) | (~d & c), words[9],
		0x21e1cde6u, 5);
	d = pg_md5_step(d, a, (c & a) | (~c & b), words[14],
		0xc33707d6u, 9);
	c = pg_md5_step(c, d, (b & d) | (~b & a), words[3],
		0xf4d50d87u, 14);
	b = pg_md5_step(b, c, (a & c) | (~a & d), words[8],
		0x455a14edu, 20);
	a = pg_md5_step(a, b, (d & b) | (~d & c), words[13],
		0xa9e3e905u, 5);
	d = pg_md5_step(d, a, (c & a) | (~c & b), words[2],
		0xfcefa3f8u, 9);
	c = pg_md5_step(c, d, (b & d) | (~b & a), words[7],
		0x676f02d9u, 14);
	b = pg_md5_step(b, c, (a & c) | (~a & d), words[12],
		0x8d2a4c8au, 20);

	a = pg_md5_step(a, b, b ^ c ^ d, words[5],
		0xfffa3942u, 4);
	d = pg_md5_step(d, a, a ^ b ^ c, words[8],
		0x8771f681u, 11);
	c = pg_md5_step(c, d, d ^ a ^ b, words[11],
		0x6d9d6122u, 16);
	b = pg_md5_step(b, c, c ^ d ^ a, words[14],
		0xfde5380cu, 23);
	a = pg_md5_step(a, b, b ^ c ^ d, words[1],
		0xa4beea44u, 4);
	d = pg_md5_step(d, a, a ^ b ^ c, words[4],
		0x4bdecfa9u, 11);
	c = pg_md5_step(c, d, d ^ a ^ b, words[7],
		0xf6bb4b60u, 16);
	b = pg_md5_step(b, c, c ^ d ^ a, words[10],
		0xbebfbc70u, 23);
	a = pg_md5_step(a, b, b ^ c ^ d, words[13],
		0x289b7ec6u, 4);
	d = pg_md5_step(d, a, a ^ b ^ c, words[0],
		0xeaa127fau, 11);
	c = pg_md5_step(c, d, d ^ a ^ b, words[3],
		0xd4ef3085u, 16);
	b = pg_md5_step(b, c, c ^ d ^ a, words[6],
		0x04881d05u, 23);
	a = pg_md5_step(a, b, b ^ c ^ d, words[9],
		0xd9d4d039u, 4);
	d = pg_md5_step(d, a, a ^ b ^ c, words[12],
		0xe6db99e5u, 11);
	c = pg_md5_step(c, d, d ^ a ^ b, words[15],
		0x1fa27cf8u, 16);
	b = pg_md5_step(b, c, c ^ d ^ a, words[2],
		0xc4ac5665u, 23);

	a = pg_md5_step(a, b, c ^ (b | ~d), words[0],
		0xf4292244u, 6);
	d = pg_md5_step(d, a, b ^ (a | ~c), words[7],
		0x432aff97u, 10);
	c = pg_md5_step(c, d, a ^ (d | ~b), words[14],
		0xab9423a7u, 15);
	b = pg_md5_step(b, c, d ^ (c | ~a), words[5],
		0xfc93a039u, 21);
	a = pg_md5_step(a, b, c ^ (b | ~d), words[12],
		0x655b59c3u, 6);
	d = pg_md5_step(d, a, b ^ (a | ~c), words[3],
		0x8f0ccc92u, 10);
	c = pg_md5_step(c, d, a ^ (d | ~b), words[10],
		0xffeff47du, 15);
	b = pg_md5_step(b, c, d ^ (c | ~a), words[1],
		0x85845dd1u, 21);
	a = pg_md5_step(a, b, c ^ (b | ~d), words[8],
		0x6fa87e4fu, 6);
	d = pg_md5_step(d, a, b ^ (a | ~c), words[15],
		0xfe2ce6e0u, 10);
	c = pg_md5_step(c, d, a ^ (d | ~b), words[6],
		0xa3014314u, 15);
	b = pg_md5_step(b, c, d ^ (c | ~a), words[13],
		0x4e0811a1u, 21);
	a = pg_md5_step(a, b, c ^ (b | ~d), words[4],
		0xf7537e82u, 6);
	d = pg_md5_step(d, a, b ^ (a | ~c), words[11],
		0xbd3af235u, 10);
	c = pg_md5_step(c, d, a ^ (d | ~b), words[2],
		0x2ad7d2bbu, 15);
	b = pg_md5_step(b, c, d ^ (c | ~a), words[9],
		0xeb86d391u, 21);

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
	if (!size)
		return;
	if (hash->used) {
		size_t amount = sizeof(hash->block) - hash->used;

		if (amount > size)
			amount = size;
		memcpy(hash->block + hash->used, bytes, amount);
		hash->used += amount;
		bytes += amount;
		size -= amount;
		if (hash->used < sizeof(hash->block))
			return;
		pg_md5_block(hash, hash->block);
		hash->used = 0;
	}
	while (size >= sizeof(hash->block)) {
		pg_md5_block(hash, bytes);
		bytes += sizeof(hash->block);
		size -= sizeof(hash->block);
	}
	if (size) {
		memcpy(hash->block, bytes, size);
		hash->used = size;
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
