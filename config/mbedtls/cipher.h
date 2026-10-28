/*
 * mbedtls/cipher.h - compile-time shim for NimBLE on CH32V208.
 *
 * nimble/controller/src/ble_ll_crypto.c and nimble/host/src/ble_sm_alg.c
 * call mbedtls_cipher_cmac(mbedtls_cipher_info_from_type(
 * MBEDTLS_CIPHER_AES_128_ECB), ...) for CMAC.  The real mbedtls library is
 * not part of this build; these headers live in config/ (first on the
 * include path) and the matching implementation of the four referenced
 * entry points is provided by port/crypto_mbedtls_shim.c (software AES-128
 * + CMAC; can be swapped later for the WCH AES hardware engine).
 *
 * Only the API subset used by the compiled tree is declared.  If BLE_SM_SC
 * (Secure Connections / ECDH P-256) is ever enabled, mbedtls/ecp.h and
 * mbedtls/ecdh.h shims plus the bignum/ECP implementation will be needed
 * too - that is intentionally out of scope for v1 (MYNEWT_VAL_BLE_SM_SC=0).
 */
#ifndef H_MBEDTLS_CIPHER_SHIM_H_
#define H_MBEDTLS_CIPHER_SHIM_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MBEDTLS_CIPHER_AES_128_ECB  4   /* same id as upstream mbedtls */

typedef struct mbedtls_cipher_info_t {
    unsigned int type;           /* MBEDTLS_CIPHER_* id */
    unsigned char key_bitlen;
} mbedtls_cipher_info_t;

const mbedtls_cipher_info_t *
mbedtls_cipher_info_from_type(unsigned int cipher_type);

int mbedtls_cipher_cmac(const mbedtls_cipher_info_t *cipher_info,
                        const unsigned char *key, size_t keylen,
                        const unsigned char *input, size_t ilen,
                        unsigned char *output);

#ifdef __cplusplus
}
#endif

#endif /* H_MBEDTLS_CIPHER_SHIM_H_ */
