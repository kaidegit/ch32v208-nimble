/*
 * crypto_mbedtls_shim.c - software AES-128 + CMAC behind the mbedtls-style
 * API shims in config/mbedtls/{aes,cmac,cipher}.h.
 *
 * NOT yet in any CMake target: CMakeLists.txt must not be modified by this
 * agent, so the orchestrator has to add this file to the wch_ble library
 * sources (port/crypto_mbedtls_shim.c).  Until then only "nimble_core"
 * (compile-only) passes; the full firmware link would show undefined
 * mbedtls_aes_* / mbedtls_cipher_cmac symbols.
 *
 * Needed because nimble/controller/src/ble_ll_crypto.c and
 * nimble/host/src/ble_sm_alg.c unconditionally #include <mbedtls/cmac.h>
 * and use mbedtls_cipher_cmac() for SMP CMAC (f1/c1/f4/f5), plus
 * mbedtls AES-ECB in ble_sm_alg_encrypt().  BLE_SM_SC (ECDH P-256 via
 * mbedtls/ecp.h) is disabled, so no bignum/ECP implementation is needed.
 * Future optimization: route through the WCH AES hardware engine instead.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "mbedtls/aes.h"
#include "mbedtls/cmac.h"

/* ------------------------------------------------------------ AES-128 */

static const uint8_t aes_sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

static inline uint8_t
aes_xt(uint8_t x)
{
    return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0x00));
}

void
mbedtls_aes_init(mbedtls_aes_context *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->rk = ctx->buf;
}

void
mbedtls_aes_free(mbedtls_aes_context *ctx)
{
    if (ctx) {
        memset(ctx, 0, sizeof(*ctx));
    }
}

int
mbedtls_aes_setkey_enc(mbedtls_aes_context *ctx, const unsigned char *key,
                       unsigned int keybits)
{
    uint32_t *rk;
    int i;

    if (ctx == NULL || key == NULL) {
        return -1;
    }
    if (keybits != 128) {
        return MBEDTLS_ERR_AES_INVALID_KEY_LENGTH;  /* 128-bit only */
    }

    rk = ctx->buf;
    for (i = 0; i < 4; i++) {
        rk[i] = ((uint32_t)key[4 * i] << 24) | ((uint32_t)key[4 * i + 1] << 16) |
                ((uint32_t)key[4 * i + 2] << 8) | (uint32_t)key[4 * i + 3];
    }

    static const uint8_t rcon[10] = {
        0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36
    };

    for (i = 0; i < 10; i++) {
        uint32_t t = rk[3];
        uint8_t ts[4];
        uint8_t rc;

        ts[0] = aes_sbox[(t >> 16) & 0xff];
        ts[1] = aes_sbox[(t >> 8) & 0xff];
        ts[2] = aes_sbox[t & 0xff];
        ts[3] = aes_sbox[(t >> 24) & 0xff];

        rc = rcon[i];
        rk[4] = rk[0] ^ (((uint32_t)ts[0] << 24) | ((uint32_t)ts[1] << 16) |
                         ((uint32_t)ts[2] << 8) | (uint32_t)ts[3]) ^
                ((uint32_t)rc << 24);
        rk[5] = rk[1] ^ rk[4];
        rk[6] = rk[2] ^ rk[5];
        rk[7] = rk[3] ^ rk[6];
        rk += 4;
    }

    ctx->nr = 10;
    ctx->rk = ctx->buf;
    return 0;
}

static void
aes_encrypt_block(const uint32_t *rk, const uint8_t in[16], uint8_t out[16])
{
    uint32_t s0, s1, s2, s3, t0, t1, t2, t3;
    int round;

    s0 = ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
         ((uint32_t)in[2] << 8) | (uint32_t)in[3];
    s1 = ((uint32_t)in[4] << 24) | ((uint32_t)in[5] << 16) |
         ((uint32_t)in[6] << 8) | (uint32_t)in[7];
    s2 = ((uint32_t)in[8] << 24) | ((uint32_t)in[9] << 16) |
         ((uint32_t)in[10] << 8) | (uint32_t)in[11];
    s3 = ((uint32_t)in[12] << 24) | ((uint32_t)in[13] << 16) |
         ((uint32_t)in[14] << 8) | (uint32_t)in[15];

    s0 ^= rk[0]; s1 ^= rk[1]; s2 ^= rk[2]; s3 ^= rk[3];

    for (round = 1; round < 10; round++) {
        t0 = ((uint32_t)aes_sbox[(s0 >> 24) & 0xff] << 24) ^
             ((uint32_t)aes_sbox[(s1 >> 16) & 0xff] << 16) ^
             ((uint32_t)aes_sbox[(s2 >> 8) & 0xff] << 8) ^
             (uint32_t)aes_sbox[s3 & 0xff];
        t1 = ((uint32_t)aes_sbox[(s1 >> 24) & 0xff] << 24) ^
             ((uint32_t)aes_sbox[(s2 >> 16) & 0xff] << 16) ^
             ((uint32_t)aes_sbox[(s3 >> 8) & 0xff] << 8) ^
             (uint32_t)aes_sbox[s0 & 0xff];
        t2 = ((uint32_t)aes_sbox[(s2 >> 24) & 0xff] << 24) ^
             ((uint32_t)aes_sbox[(s3 >> 16) & 0xff] << 16) ^
             ((uint32_t)aes_sbox[(s0 >> 8) & 0xff] << 8) ^
             (uint32_t)aes_sbox[s1 & 0xff];
        t3 = ((uint32_t)aes_sbox[(s3 >> 24) & 0xff] << 24) ^
             ((uint32_t)aes_sbox[(s0 >> 16) & 0xff] << 16) ^
             ((uint32_t)aes_sbox[(s1 >> 8) & 0xff] << 8) ^
             (uint32_t)aes_sbox[s2 & 0xff];

        /* MixColumns, then AddRoundKey. */
        {
            uint8_t a0 = (uint8_t)(t0 >> 24), b0 = (uint8_t)(t0 >> 16),
                    c0 = (uint8_t)(t0 >> 8), d0 = (uint8_t)t0;
            uint8_t a1 = (uint8_t)(t1 >> 24), b1 = (uint8_t)(t1 >> 16),
                    c1 = (uint8_t)(t1 >> 8), d1 = (uint8_t)t1;
            uint8_t a2 = (uint8_t)(t2 >> 24), b2 = (uint8_t)(t2 >> 16),
                    c2 = (uint8_t)(t2 >> 8), d2 = (uint8_t)t2;
            uint8_t a3 = (uint8_t)(t3 >> 24), b3 = (uint8_t)(t3 >> 16),
                    c3 = (uint8_t)(t3 >> 8), d3 = (uint8_t)t3;

            /* Row0 = 2a^3b^c^d  Row1 = a^2b^3c^d
             * Row2 = a^b^2c^3d  Row3 = 3a^b^c^2d */
            s0 = ((uint32_t)(aes_xt(a0) ^ aes_xt(b0) ^ b0 ^ c0 ^ d0) << 24) |
                 ((uint32_t)(a0 ^ aes_xt(b0) ^ aes_xt(c0) ^ c0 ^ d0) << 16) |
                 ((uint32_t)(a0 ^ b0 ^ aes_xt(c0) ^ aes_xt(d0) ^ d0) << 8) |
                 (uint32_t)(aes_xt(a0) ^ a0 ^ b0 ^ c0 ^ aes_xt(d0));
            s1 = ((uint32_t)(aes_xt(a1) ^ aes_xt(b1) ^ b1 ^ c1 ^ d1) << 24) |
                 ((uint32_t)(a1 ^ aes_xt(b1) ^ aes_xt(c1) ^ c1 ^ d1) << 16) |
                 ((uint32_t)(a1 ^ b1 ^ aes_xt(c1) ^ aes_xt(d1) ^ d1) << 8) |
                 (uint32_t)(aes_xt(a1) ^ a1 ^ b1 ^ c1 ^ aes_xt(d1));
            s2 = ((uint32_t)(aes_xt(a2) ^ aes_xt(b2) ^ b2 ^ c2 ^ d2) << 24) |
                 ((uint32_t)(a2 ^ aes_xt(b2) ^ aes_xt(c2) ^ c2 ^ d2) << 16) |
                 ((uint32_t)(a2 ^ b2 ^ aes_xt(c2) ^ aes_xt(d2) ^ d2) << 8) |
                 (uint32_t)(aes_xt(a2) ^ a2 ^ b2 ^ c2 ^ aes_xt(d2));
            s3 = ((uint32_t)(aes_xt(a3) ^ aes_xt(b3) ^ b3 ^ c3 ^ d3) << 24) |
                 ((uint32_t)(a3 ^ aes_xt(b3) ^ aes_xt(c3) ^ c3 ^ d3) << 16) |
                 ((uint32_t)(a3 ^ b3 ^ aes_xt(c3) ^ aes_xt(d3) ^ d3) << 8) |
                 (uint32_t)(aes_xt(a3) ^ a3 ^ b3 ^ c3 ^ aes_xt(d3));
        }

        s0 ^= rk[4 * round];
        s1 ^= rk[4 * round + 1];
        s2 ^= rk[4 * round + 2];
        s3 ^= rk[4 * round + 3];
    }

    /* Final round: SubBytes + ShiftRows + AddRoundKey (no MixColumns). */
    t0 = ((uint32_t)aes_sbox[(s0 >> 24) & 0xff] << 24) ^
         ((uint32_t)aes_sbox[(s1 >> 16) & 0xff] << 16) ^
         ((uint32_t)aes_sbox[(s2 >> 8) & 0xff] << 8) ^
         (uint32_t)aes_sbox[s3 & 0xff];
    t1 = ((uint32_t)aes_sbox[(s1 >> 24) & 0xff] << 24) ^
         ((uint32_t)aes_sbox[(s2 >> 16) & 0xff] << 16) ^
         ((uint32_t)aes_sbox[(s3 >> 8) & 0xff] << 8) ^
         (uint32_t)aes_sbox[s0 & 0xff];
    t2 = ((uint32_t)aes_sbox[(s2 >> 24) & 0xff] << 24) ^
         ((uint32_t)aes_sbox[(s3 >> 16) & 0xff] << 16) ^
         ((uint32_t)aes_sbox[(s0 >> 8) & 0xff] << 8) ^
         (uint32_t)aes_sbox[s1 & 0xff];
    t3 = ((uint32_t)aes_sbox[(s3 >> 24) & 0xff] << 24) ^
         ((uint32_t)aes_sbox[(s0 >> 16) & 0xff] << 16) ^
         ((uint32_t)aes_sbox[(s1 >> 8) & 0xff] << 8) ^
         (uint32_t)aes_sbox[s2 & 0xff];

    t0 ^= rk[40]; t1 ^= rk[41]; t2 ^= rk[42]; t3 ^= rk[43];

    out[0]  = (uint8_t)(t0 >> 24); out[1]  = (uint8_t)(t0 >> 16);
    out[2]  = (uint8_t)(t0 >> 8);  out[3]  = (uint8_t)t0;
    out[4]  = (uint8_t)(t1 >> 24); out[5]  = (uint8_t)(t1 >> 16);
    out[6]  = (uint8_t)(t1 >> 8);  out[7]  = (uint8_t)t1;
    out[8]  = (uint8_t)(t2 >> 24); out[9]  = (uint8_t)(t2 >> 16);
    out[10] = (uint8_t)(t2 >> 8);  out[11] = (uint8_t)t2;
    out[12] = (uint8_t)(t3 >> 24); out[13] = (uint8_t)(t3 >> 16);
    out[14] = (uint8_t)(t3 >> 8);  out[15] = (uint8_t)t3;
}

int
mbedtls_aes_crypt_ecb(mbedtls_aes_context *ctx, int mode,
                      const unsigned char input[16],
                      unsigned char output[16])
{
    if (ctx == NULL || ctx->rk == NULL) {
        return -1;
    }
    if (mode != MBEDTLS_AES_ENCRYPT) {
        return -1;      /* decrypt not needed by the compiled tree */
    }
    aes_encrypt_block(ctx->rk, input, output);
    return 0;
}

/* --------------------------------------------------------------- CMAC */

const mbedtls_cipher_info_t *
mbedtls_cipher_info_from_type(unsigned int cipher_type)
{
    static const mbedtls_cipher_info_t aes_128_ecb = {
        .type = MBEDTLS_CIPHER_AES_128_ECB,
        .key_bitlen = 16,
    };

    if (cipher_type == MBEDTLS_CIPHER_AES_128_ECB) {
        return &aes_128_ecb;
    }
    return NULL;
}

/* NIST SP 800-38B CMAC with AES-128. */
int
mbedtls_cipher_cmac(const mbedtls_cipher_info_t *cipher_info,
                    const unsigned char *key, size_t keylen,
                    const unsigned char *input, size_t ilen,
                    unsigned char *output)
{
    mbedtls_aes_context ctx;
    uint8_t l[16], x[16], block[16];
    uint8_t k1[16], k2[16];
    size_t n, i, j;
    uint8_t carry;

    if (cipher_info == NULL || key == NULL || output == NULL ||
        (input == NULL && ilen != 0)) {
        return -1;
    }
    if (keylen != 16 || cipher_info->key_bitlen != 16) {
        return -1;
    }

    mbedtls_aes_init(&ctx);
    if (mbedtls_aes_setkey_enc(&ctx, key, 128) != 0) {
        mbedtls_aes_free(&ctx);
        return -1;
    }

    /* L = AES_K(0^16) */
    memset(l, 0, 16);
    aes_encrypt_block(ctx.rk, l, l);

    /* K1 = (L << 1) & (L[0] & 0x80 ? Rb : 0), Rb = 0x87 for 128-bit blocks */
    carry = (uint8_t)((l[0] & 0x80) ? 0x87 : 0x00);
    for (i = 0; i < 16; i++) {
        uint8_t next = (i < 15) ? l[i + 1] : 0;
        k1[i] = (uint8_t)((l[i] << 1) | (next >> 7));
    }
    k1[15] ^= carry;
    /* K2 = K1 << 1 with the same subkey rule */
    carry = (uint8_t)((k1[0] & 0x80) ? 0x87 : 0x00);
    for (i = 0; i < 16; i++) {
        uint8_t next = (i < 15) ? k1[i + 1] : 0;
        k2[i] = (uint8_t)((k1[i] << 1) | (next >> 7));
    }
    k2[15] ^= carry;

    memset(x, 0, 16);
    n = (ilen + 15) / 16;

    if (n == 0) {
        /* Empty message: M_last = K2 */
        for (i = 0; i < 16; i++) {
            block[i] = (uint8_t)(k2[i] ^ x[i]);
        }
        aes_encrypt_block(ctx.rk, block, output);
        mbedtls_aes_free(&ctx);
        return 0;
    }

    for (j = 0; j < n; j++) {
        size_t rem = ilen - j * 16;
        const uint8_t *m = input + j * 16;

        if (j + 1 < n) {
            for (i = 0; i < 16; i++) {
                block[i] = (uint8_t)(m[i] ^ x[i]);
            }
        } else {
            if (rem == 16) {
                for (i = 0; i < 16; i++) {
                    block[i] = (uint8_t)(m[i] ^ k1[i] ^ x[i]);
                }
            } else {
                memset(block, 0, 16);
                memcpy(block, m, rem);
                block[rem] = 0x80;
                for (i = 0; i < 16; i++) {
                    block[i] = (uint8_t)(block[i] ^ k2[i] ^ x[i]);
                }
            }
        }

        aes_encrypt_block(ctx.rk, block, x);
    }

    memcpy(output, x, 16);
    mbedtls_aes_free(&ctx);
    return 0;
}
