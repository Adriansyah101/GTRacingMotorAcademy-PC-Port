/* crypto.h -- the CommonCrypto shim's digests for the host's own use (crypto.c). */
#ifndef GT_CRYPTO_H
#define GT_CRYPTO_H

#include <stddef.h>

#include "arm_rt.h"

void crypto_sha256(const void *data, size_t n, u8 out[32]);    /* FIPS 180-4 */

#endif
