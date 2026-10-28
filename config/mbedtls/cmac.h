/*
 * mbedtls/cmac.h - compile-time shim, see config/mbedtls/cipher.h.
 * Upstream mbedtls cmac.h drags in cipher.h; the compiled NimBLE sources
 * include <mbedtls/cmac.h> and use both cmac and cipher APIs through it.
 */
#ifndef H_MBEDTLS_CMAC_SHIM_H_
#define H_MBEDTLS_CMAC_SHIM_H_

#include "cipher.h"

#endif /* H_MBEDTLS_CMAC_SHIM_H_ */
