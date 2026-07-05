#ifndef TLS_SHA256_H
#define TLS_SHA256_H

#include <stdint.h>
#include <stddef.h>

#define TLS_SHA256_DIGEST_SIZE 32
#define TLS_SHA256_BLOCK_SIZE  64

typedef struct {
    uint8_t  data[64];
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[8];
} tls_sha256_ctx;

void tls_sha256_init(tls_sha256_ctx* ctx);
void tls_sha256_update(tls_sha256_ctx* ctx, const uint8_t* data, size_t len);
void tls_sha256_final(tls_sha256_ctx* ctx, uint8_t hash[32]);
void tls_sha256(const uint8_t* data, size_t len, uint8_t hash[32]);

#endif
