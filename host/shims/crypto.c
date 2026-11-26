#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crypto.h"
#include "rt.h"

static u32 rol(u32 x, int n) { return x << n | x >> (32 - n); }
static u32 ror(u32 x, int n) { return x >> n | x << (32 - n); }

typedef struct { u32 s[4]; u64 n; u8 buf[64]; } Md5;
static void md5_block(Md5 *m, const u8 *p)
{
    static const u32 K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
                              14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    u32 w[16];
    for (int i = 0; i < 16; i++)
        w[i] = (u32)p[4 * i] | (u32)p[4 * i + 1] << 8 | (u32)p[4 * i + 2] << 16 | (u32)p[4 * i + 3] << 24;
    u32 a = m->s[0], b = m->s[1], c = m->s[2], d = m->s[3];
    for (int i = 0; i < 64; i++) {
        u32 f;
        int g;
        if (i < 16) f = (b & c) | (~b & d), g = i;
        else if (i < 32) f = (d & b) | (~d & c), g = (5 * i + 1) & 15;
        else if (i < 48) f = b ^ c ^ d, g = (3 * i + 5) & 15;
        else f = c ^ (b | ~d), g = (7 * i) & 15;
        u32 t = d;
        d = c;
        c = b;
        b = b + rol(a + f + K[i] + w[g], R[i]);
        a = t;
    }
    m->s[0] += a, m->s[1] += b, m->s[2] += c, m->s[3] += d;
}
static void md5_init(Md5 *m)
{
    m->s[0] = 0x67452301, m->s[1] = 0xefcdab89, m->s[2] = 0x98badcfe, m->s[3] = 0x10325476;
    m->n = 0;
}
static void md5_update(Md5 *m, const u8 *p, u32 len)
{
    while (len--) {
        m->buf[m->n++ & 63] = *p++;
        if (!(m->n & 63))
            md5_block(m, m->buf);
    }
}
static void md5_final(Md5 *m, u8 *out)
{
    u64 bits = m->n * 8;
    u8 pad = 0x80;
    md5_update(m, &pad, 1);
    pad = 0;
    while ((m->n & 63) != 56)
        md5_update(m, &pad, 1);
    for (int i = 0; i < 8; i++) {
        u8 b = (u8)(bits >> (8 * i));
        md5_update(m, &b, 1);
    }
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            out[4 * i + j] = (u8)(m->s[i] >> (8 * j));
}

typedef struct { u32 s[5]; u64 n; u8 buf[64]; } Sha1;
static void sha1_block(Sha1 *h, const u8 *p)
{
    u32 w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (u32)p[4 * i] << 24 | (u32)p[4 * i + 1] << 16 | (u32)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++)
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    u32 a = h->s[0], b = h->s[1], c = h->s[2], d = h->s[3], e = h->s[4];
    for (int i = 0; i < 80; i++) {
        u32 f, k;
        if (i < 20) f = (b & c) | (~b & d), k = 0x5a827999;
        else if (i < 40) f = b ^ c ^ d, k = 0x6ed9eba1;
        else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8f1bbcdc;
        else f = b ^ c ^ d, k = 0xca62c1d6;
        u32 t = rol(a, 5) + f + e + k + w[i];
        e = d, d = c, c = rol(b, 30), b = a, a = t;
    }
    h->s[0] += a, h->s[1] += b, h->s[2] += c, h->s[3] += d, h->s[4] += e;
}
static void sha1_init(Sha1 *h)
{
    h->s[0] = 0x67452301, h->s[1] = 0xefcdab89, h->s[2] = 0x98badcfe, h->s[3] = 0x10325476, h->s[4] = 0xc3d2e1f0;
    h->n = 0;
}
static void sha1_update(Sha1 *h, const u8 *p, u32 len)
{
    while (len--) {
        h->buf[h->n++ & 63] = *p++;
        if (!(h->n & 63))
            sha1_block(h, h->buf);
    }
}
static void sha1_final(Sha1 *h, u8 *out)
{
    u64 bits = h->n * 8;
    u8 pad = 0x80;
    sha1_update(h, &pad, 1);
    pad = 0;
    while ((h->n & 63) != 56)
        sha1_update(h, &pad, 1);
    for (int i = 7; i >= 0; i--) {
        u8 b = (u8)(bits >> (8 * i));
        sha1_update(h, &b, 1);
    }
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 4; j++)
            out[4 * i + j] = (u8)(h->s[i] >> (24 - 8 * j));
}

typedef struct { u32 s[8]; u64 n; u8 buf[64]; } Sha256;
static void sha256_block(Sha256 *h, const u8 *p)
{
    static const u32 K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    u32 w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (u32)p[4 * i] << 24 | (u32)p[4 * i + 1] << 16 | (u32)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        u32 s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        u32 s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u32 a = h->s[0], b = h->s[1], c = h->s[2], d = h->s[3], e = h->s[4], f = h->s[5], g = h->s[6], k = h->s[7];
    for (int i = 0; i < 64; i++) {
        u32 t1 = k + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        u32 t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h->s[0] += a, h->s[1] += b, h->s[2] += c, h->s[3] += d, h->s[4] += e, h->s[5] += f, h->s[6] += g, h->s[7] += k;
}
static void sha256_init(Sha256 *h)
{
    static const u32 iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                              0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(h->s, iv, sizeof iv);
    h->n = 0;
}
static void sha256_update(Sha256 *h, const u8 *p, u32 len)
{
    while (len--) {
        h->buf[h->n++ & 63] = *p++;
        if (!(h->n & 63))
            sha256_block(h, h->buf);
    }
}
static void sha256_final(Sha256 *h, u8 *out)
{
    u64 bits = h->n * 8;
    u8 pad = 0x80;
    sha256_update(h, &pad, 1);
    pad = 0;
    while ((h->n & 63) != 56)
        sha256_update(h, &pad, 1);
    for (int i = 7; i >= 0; i--) {
        u8 b = (u8)(bits >> (8 * i));
        sha256_update(h, &b, 1);
    }
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 4; j++)
            out[4 * i + j] = (u8)(h->s[i] >> (24 - 8 * j));
}

void crypto_sha256(const void *data, size_t n, u8 out[32])
{
    Sha256 h;
    sha256_init(&h);
    sha256_update(&h, data, (u32)n);
    sha256_final(&h, out);
}

u32 h_CC_MD5(u32 data, u32 len, u32 md)
{
    Md5 m;
    md5_init(&m);
    md5_update(&m, GP(data), len);
    md5_final(&m, GP(md));
    return md;
}
u32 h_CC_SHA1(u32 data, u32 len, u32 md)
{
    Sha1 h;
    sha1_init(&h);
    sha1_update(&h, GP(data), len);
    sha1_final(&h, GP(md));
    return md;
}
u32 h_CC_SHA1_Init(u32 ctx) { sha1_init(GP(ctx)); return 1; }
u32 h_CC_SHA1_Update(u32 ctx, u32 data, u32 len) { sha1_update(GP(ctx), GP(data), len); return 1; }
u32 h_CC_SHA1_Final(u32 md, u32 ctx) { sha1_final(GP(ctx), GP(md)); return 1; }
u32 h_CC_SHA256(u32 data, u32 len, u32 md)
{
    Sha256 h;
    sha256_init(&h);
    sha256_update(&h, GP(data), len);
    sha256_final(&h, GP(md));
    return md;
}

static int hash_once(u32 alg, const u8 *p, u32 n, const u8 *p2, u32 n2, u8 *out)
{
    switch (alg) {
    case 0: {
        Sha1 h;
        sha1_init(&h);
        sha1_update(&h, p, n);
        sha1_update(&h, p2, n2);
        sha1_final(&h, out);
        return 20;
    }
    case 1: {
        Md5 m;
        md5_init(&m);
        md5_update(&m, p, n);
        md5_update(&m, p2, n2);
        md5_final(&m, out);
        return 16;
    }
    case 2: {
        Sha256 h;
        sha256_init(&h);
        sha256_update(&h, p, n);
        sha256_update(&h, p2, n2);
        sha256_final(&h, out);
        return 32;
    }
    }
    rt_fatal("CCHmac with algorithm %u is not supported", alg);
}
void h_CCHmac(u32 alg, u32 key, u32 keylen, u32 data, u32 len, u32 out)
{
    u8 k[64] = {0}, ipad[64], opad[64], inner[32];
    if (keylen > 64)
        hash_once(alg, GP(key), keylen, NULL, 0, k);
    else
        memcpy(k, GP(key), keylen);
    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    int n = hash_once(alg, ipad, 64, GP(data), len, inner);
    hash_once(alg, opad, 64, inner, (u32)n, GP(out));
}

static u8 sbox[256], inv_sbox[256];
static u8 xt(u8 x) { return (u8)(x << 1 ^ (x & 0x80 ? 0x1b : 0)); }
static u8 gmul(u8 a, u8 b)
{
    u8 r = 0;
    while (b) {
        if (b & 1)
            r ^= a;
        a = xt(a);
        b >>= 1;
    }
    return r;
}
static void aes_tables(void)
{
    if (sbox[0])
        return;
    u8 p = 1, q = 1;
    do {
        p = p ^ (u8)(p << 1) ^ (p & 0x80 ? 0x1b : 0);
        q ^= q << 1;
        q ^= q << 2;
        q ^= q << 4;
        if (q & 0x80)
            q ^= 0x09;
        u8 x = q ^ (u8)(q << 1 | q >> 7) ^ (u8)(q << 2 | q >> 6) ^ (u8)(q << 3 | q >> 5) ^ (u8)(q << 4 | q >> 4);
        sbox[p] = x ^ 0x63;
    } while (p != 1);
    sbox[0] = 0x63;
    for (int i = 0; i < 256; i++)
        inv_sbox[sbox[i]] = (u8)i;
}
typedef struct { u8 rk[240]; int rounds; } Aes;
static void aes_key(Aes *a, const u8 *key, int len)
{
    aes_tables();
    int nk = len / 4;
    a->rounds = nk + 6;
    memcpy(a->rk, key, (size_t)len);
    u8 rcon = 1;
    for (int i = nk; i < 4 * (a->rounds + 1); i++) {
        u8 t[4];
        memcpy(t, a->rk + 4 * (i - 1), 4);
        if (i % nk == 0) {
            u8 x = t[0];
            t[0] = sbox[t[1]] ^ rcon, t[1] = sbox[t[2]], t[2] = sbox[t[3]], t[3] = sbox[x];
            rcon = xt(rcon);
        } else if (nk > 6 && i % nk == 4) {
            for (int j = 0; j < 4; j++)
                t[j] = sbox[t[j]];
        }
        for (int j = 0; j < 4; j++)
            a->rk[4 * i + j] = a->rk[4 * (i - nk) + j] ^ t[j];
    }
}
static void aes_enc(const Aes *a, u8 *s)
{
    for (int i = 0; i < 16; i++)
        s[i] ^= a->rk[i];
    for (int r = 1; r <= a->rounds; r++) {
        u8 t[16];
        for (int i = 0; i < 16; i++)
            t[i] = sbox[s[(i + 4 * (i % 4)) % 16]];
        if (r < a->rounds)
            for (int c = 0; c < 4; c++) {
                u8 *x = t + 4 * c, a0 = x[0], a1 = x[1], a2 = x[2], a3 = x[3];
                x[0] = xt(a0) ^ xt(a1) ^ a1 ^ a2 ^ a3;
                x[1] = a0 ^ xt(a1) ^ xt(a2) ^ a2 ^ a3;
                x[2] = a0 ^ a1 ^ xt(a2) ^ xt(a3) ^ a3;
                x[3] = xt(a0) ^ a0 ^ a1 ^ a2 ^ xt(a3);
            }
        for (int i = 0; i < 16; i++)
            s[i] = t[i] ^ a->rk[16 * r + i];
    }
}
static void aes_dec(const Aes *a, u8 *s)
{
    for (int i = 0; i < 16; i++)
        s[i] ^= a->rk[16 * a->rounds + i];
    for (int r = a->rounds - 1; r >= 0; r--) {
        u8 t[16];
        for (int i = 0; i < 16; i++)
            t[(i + 4 * (i % 4)) % 16] = inv_sbox[s[i]];
        for (int i = 0; i < 16; i++)
            t[i] ^= a->rk[16 * r + i];
        if (r > 0)
            for (int c = 0; c < 4; c++) {
                u8 *x = t + 4 * c, a0 = x[0], a1 = x[1], a2 = x[2], a3 = x[3];
                x[0] = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9);
                x[1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13);
                x[2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11);
                x[3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14);
            }
        memcpy(s, t, 16);
    }
}

void I_CCCrypt(CPU *c)
{
    u32 op = c->r[0], alg = c->r[1], opt = c->r[2], key = c->r[3];
    u32 *st = GP(c->r[13]);
    u32 keylen = st[0], iv = st[1], in = st[2], inlen = st[3], out = st[4], avail = st[5], moved = st[6];
    if (alg != 0 || (keylen != 16 && keylen != 24 && keylen != 32)) {
        rt_log("[crypto] CCCrypt algorithm %u key %u bytes: not supported\n", alg, keylen);
        c->r[0] = (u32)-4305;
        return;
    }
    int pad = opt & 1, ecb = opt & 2;
    u32 n = op == 0 && pad ? (inlen / 16 + 1) * 16 : inlen;
    if (op == 0 && !pad && inlen % 16) {
        c->r[0] = (u32)-4303;
        return;
    }
    if (n > avail) {
        if (moved)
            wr32(moved, n);
        c->r[0] = (u32)-4301;
        return;
    }
    Aes a;
    aes_key(&a, GP(key), (int)keylen);
    u8 chain[16] = {0};
    if (iv && !ecb)
        memcpy(chain, GP(iv), 16);
    u8 *src = malloc(n + 16);
    memcpy(src, GP(in), inlen);
    if (op == 0 && pad)
        memset(src + inlen, (int)(n - inlen), n - inlen);
    u8 *dst = GP(out);
    for (u32 i = 0; i + 16 <= n; i += 16) {
        u8 b[16];
        memcpy(b, src + i, 16);
        if (op == 0) {
            if (!ecb)
                for (int j = 0; j < 16; j++)
                    b[j] ^= chain[j];
            aes_enc(&a, b);
            if (!ecb)
                memcpy(chain, b, 16);
        } else {
            u8 save[16];
            memcpy(save, b, 16);
            aes_dec(&a, b);
            if (!ecb) {
                for (int j = 0; j < 16; j++)
                    b[j] ^= chain[j];
                memcpy(chain, save, 16);
            }
        }
        memcpy(dst + i, b, 16);
    }
    free(src);
    if (op == 1 && pad && n >= 16) {
        u32 p = dst[n - 1];
        if (p < 1 || p > 16) {
            c->r[0] = (u32)-4304;
            return;
        }
        n -= p;
    }
    if (moved)
        wr32(moved, n);
    c->r[0] = 0;
}
