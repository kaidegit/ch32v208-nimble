/*
 * mbedtls/aes.h - compile-time shim, see config/mbedtls/cipher.h.
 *
 * Used unconditionally by nimble/host/src/ble_sm_alg.c (SMP legacy crypto:
 * aes-cmac based f1/c1/f4/f5 helpers) and by ble_ll_cs_drbg.c (compiled out
 * while BLE_LL_CHANNEL_SOUNDING=0).
 */
#ifndef H_MBEDTLS_AES_SHIM_H_
#define H_MBEDTLS_AES_SHIM_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MBEDTLS_AES_ENCRYPT     1
#define MBEDTLS_AES_DECRYPT     0
#define MBEDTLS_ERR_AES_INVALID_KEY_LENGTH  -0x0020

typedef struct mbedtls_aes_context {
    int nr;                      /* number of rounds (10 for AES-128) */
    uint32_t buf[44];            /* round keys, 4*(nr+1) words */
    uint32_t *rk;                /* working pointer, mirrors mbedtls */
} mbedtls_aes_context;

void mbedtls_aes_init(mbedtls_aes_context *ctx);
int mbedtls_aes_setkey_enc(mbedtls_aes_context *ctx,
                           const unsigned char *key, unsigned int keybits);
int mbedtls_aes_crypt_ecb(mbedtls_aes_context *ctx, int mode,
                          const unsigned char input[16],
                          unsigned char output[16]);
void mbedtls_aes_free(mbedtls_aes_context *ctx);

#ifdef __cplusplus
}
#endif

#endif /* H_MBEDTLS_AES_SHIM_H_ */
