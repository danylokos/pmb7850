#include <string.h>
#include "emu_engine.h"

#if (defined(__x86_64__) || defined(__i386__)) && \
    (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
#define EMU_SHA256_X86_SHA 1
#else
#define EMU_SHA256_X86_SHA 0
#endif

typedef struct {
    uint32_t h[8];
    uint64_t bits;
    uint8_t block[64];
    size_t used;
} emu_sha256_ctx_t;

#if EMU_SHA256_X86_SHA
static const uint32_t emu_sha256_sha_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
};
#endif

static uint32_t emu_rotr(uint32_t v, unsigned n) {
    return (v >> n) | (v << (32u - n));
}

static void emu_sha256_block(emu_sha256_ctx_t *ctx, const uint8_t *data) {
    static const uint32_t k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
    };
    uint32_t w[64];
    for (unsigned i = 0; i < 16; i++)
        w[i] = ((uint32_t)data[i * 4] << 24) |
               ((uint32_t)data[i * 4 + 1] << 16) |
               ((uint32_t)data[i * 4 + 2] << 8) | data[i * 4 + 3];
    for (unsigned i = 16; i < 64; i++) {
        uint32_t s0 = emu_rotr(w[i-15],7) ^ emu_rotr(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = emu_rotr(w[i-2],17) ^ emu_rotr(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=ctx->h[0], b=ctx->h[1], c=ctx->h[2], d=ctx->h[3];
    uint32_t e=ctx->h[4], f=ctx->h[5], g=ctx->h[6], h=ctx->h[7];
    for (unsigned i = 0; i < 64; i++) {
        uint32_t s1=emu_rotr(e,6)^emu_rotr(e,11)^emu_rotr(e,25);
        uint32_t ch=(e&f)^((~e)&g), t1=h+s1+ch+k[i]+w[i];
        uint32_t s0=emu_rotr(a,2)^emu_rotr(a,13)^emu_rotr(a,22);
        uint32_t maj=(a&b)^(a&c)^(b&c), t2=s0+maj;
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    ctx->h[0]+=a; ctx->h[1]+=b; ctx->h[2]+=c; ctx->h[3]+=d;
    ctx->h[4]+=e; ctx->h[5]+=f; ctx->h[6]+=g; ctx->h[7]+=h;
}

#if EMU_SHA256_X86_SHA
__attribute__((target("sha,ssse3,sse4.1"), optimize("unroll-loops")))
static void emu_sha256_block_x86_sha(emu_sha256_ctx_t *ctx,
                                     const uint8_t *data) {
    const __m128i mask = _mm_set_epi8(
        12,13,14,15, 8,9,10,11, 4,5,6,7, 0,1,2,3);
    __m128i state0 = _mm_loadu_si128((const __m128i *)&ctx->h[0]);
    __m128i state1 = _mm_loadu_si128((const __m128i *)&ctx->h[4]);
    __m128i tmp = _mm_shuffle_epi32(state0, 0xB1);
    state1 = _mm_shuffle_epi32(state1, 0x1B);
    state0 = _mm_alignr_epi8(tmp, state1, 8);
    state1 = _mm_blend_epi16(state1, tmp, 0xF0);
    const __m128i saved0 = state0;
    const __m128i saved1 = state1;
    __m128i messages[4];

    for (unsigned group = 0; group < 16; group++) {
        unsigned index = group & 3u;
        if (group < 4) {
            messages[index] = _mm_shuffle_epi8(
                _mm_loadu_si128((const __m128i *)(data + group * 16u)),
                mask);
        } else {
            messages[index] = _mm_sha256msg1_epu32(
                messages[index], messages[(index + 1u) & 3u]);
            tmp = _mm_alignr_epi8(messages[(index + 3u) & 3u],
                                  messages[(index + 2u) & 3u], 4);
            messages[index] = _mm_add_epi32(messages[index], tmp);
            messages[index] = _mm_sha256msg2_epu32(
                messages[index], messages[(index + 3u) & 3u]);
        }
        tmp = _mm_add_epi32(
            messages[index],
            _mm_loadu_si128(
                (const __m128i *)&emu_sha256_sha_k[group * 4u]));
        state1 = _mm_sha256rnds2_epu32(state1, state0, tmp);
        tmp = _mm_shuffle_epi32(tmp, 0x0E);
        state0 = _mm_sha256rnds2_epu32(state0, state1, tmp);
    }
    state0 = _mm_add_epi32(state0, saved0);
    state1 = _mm_add_epi32(state1, saved1);
    tmp = _mm_shuffle_epi32(state0, 0x1B);
    state1 = _mm_shuffle_epi32(state1, 0xB1);
    state0 = _mm_blend_epi16(tmp, state1, 0xF0);
    state1 = _mm_alignr_epi8(state1, tmp, 8);
    _mm_storeu_si128((__m128i *)&ctx->h[0], state0);
    _mm_storeu_si128((__m128i *)&ctx->h[4], state1);
}
#endif

typedef void (*emu_sha256_block_fn)(emu_sha256_ctx_t *, const uint8_t *);

static void emu_sha256_update(emu_sha256_ctx_t *ctx, const uint8_t *data,
                              size_t size, emu_sha256_block_fn block) {
    ctx->bits += (uint64_t)size * 8u;
    while (size) {
        if (!ctx->used && size >= sizeof ctx->block) {
            block(ctx, data);
            data += sizeof ctx->block;
            size -= sizeof ctx->block;
            continue;
        }
        size_t take = 64u - ctx->used;
        if (take > size) take = size;
        memcpy(ctx->block + ctx->used, data, take);
        ctx->used += take; data += take; size -= take;
        if (ctx->used == 64u) {
            block(ctx, ctx->block);
            ctx->used = 0;
        }
    }
}

void emu_sha256(const void *data, size_t size, uint8_t digest[32]) {
    emu_sha256_ctx_t ctx = {
        .h={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
            0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19}
    };
    emu_sha256_block_fn block = emu_sha256_block;
#if EMU_SHA256_X86_SHA
    if (__builtin_cpu_supports("sha")) block = emu_sha256_block_x86_sha;
#endif
    emu_sha256_update(&ctx, data, size, block);
    ctx.block[ctx.used++] = 0x80;
    if (ctx.used > 56) {
        memset(ctx.block + ctx.used, 0, 64 - ctx.used);
        block(&ctx, ctx.block);
        ctx.used = 0;
    }
    memset(ctx.block + ctx.used, 0, 56 - ctx.used);
    for (unsigned i = 0; i < 8; i++)
        ctx.block[63-i] = (uint8_t)(ctx.bits >> (i * 8));
    block(&ctx, ctx.block);
    for (unsigned i = 0; i < 8; i++) {
        digest[i*4]=(uint8_t)(ctx.h[i]>>24);
        digest[i*4+1]=(uint8_t)(ctx.h[i]>>16);
        digest[i*4+2]=(uint8_t)(ctx.h[i]>>8);
        digest[i*4+3]=(uint8_t)ctx.h[i];
    }
}

void emu_sha256_hex(const uint8_t digest[32], char text[65]) {
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; i++) {
        text[i*2] = digits[digest[i] >> 4];
        text[i*2+1] = digits[digest[i] & 15];
    }
    text[64] = 0;
}
