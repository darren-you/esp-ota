#pragma once
#include <stddef.h>
#include <stdint.h>
typedef int psa_status_t;
typedef struct { uint32_t sum; } psa_hash_operation_t;
#define PSA_HASH_OPERATION_INIT {0}
#define PSA_SUCCESS 0
#define PSA_ALG_SHA_256 1
psa_status_t psa_crypto_init(void);
psa_status_t psa_hash_setup(psa_hash_operation_t *operation, int algorithm);
psa_status_t psa_hash_update(psa_hash_operation_t *operation, const uint8_t *bytes, size_t length);
psa_status_t psa_hash_finish(psa_hash_operation_t *operation, uint8_t *out, size_t out_size, size_t *actual);
psa_status_t psa_hash_abort(psa_hash_operation_t *operation);
