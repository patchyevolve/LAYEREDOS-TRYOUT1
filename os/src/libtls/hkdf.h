#ifndef TLS_HKDF_H
#define TLS_HKDF_H

#include <stdint.h>
#include <stddef.h>

void tls_hmac_sha256(const uint8_t* key, size_t key_len,
                     const uint8_t* data, size_t data_len,
                     uint8_t out[32]);

void tls_hkdf_extract(const uint8_t* salt, size_t salt_len,
                      const uint8_t* ikm, size_t ikm_len,
                      uint8_t prk[32]);

void tls_hkdf_expand(const uint8_t prk[32],
                     const uint8_t* info, size_t info_len,
                     uint8_t* out, size_t out_len);

void tls_hkdf_expand_label(const uint8_t secret[32],
                           const char* label, size_t label_len,
                           const uint8_t* ctx, size_t ctx_len,
                           uint8_t* out, size_t out_len);

#endif
