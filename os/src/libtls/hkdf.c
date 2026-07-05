#include "hkdf.h"
#include "sha256.h"
#include <string.h>

void tls_hmac_sha256(const uint8_t* key, size_t key_len,
                     const uint8_t* data, size_t data_len,
                     uint8_t out[32])
{
    uint8_t k0[64];
    if (key_len > 64) {
        tls_sha256(key, key_len, k0);
        memset(k0+32, 0, 32);
    } else {
        memset(k0, 0, 64);
        memcpy(k0, key, key_len);
    }
    uint8_t ipad[64], opad[64];
    for (int i=0;i<64;i++) { ipad[i]=k0[i]^0x36; opad[i]=k0[i]^0x5c; }

    tls_sha256_ctx ctx;
    tls_sha256_init(&ctx);
    tls_sha256_update(&ctx, ipad, 64);
    tls_sha256_update(&ctx, data, data_len);
    uint8_t inner[32];
    tls_sha256_final(&ctx, inner);

    tls_sha256_init(&ctx);
    tls_sha256_update(&ctx, opad, 64);
    tls_sha256_update(&ctx, inner, 32);
    tls_sha256_final(&ctx, out);
}

void tls_hkdf_extract(const uint8_t* salt, size_t salt_len,
                      const uint8_t* ikm, size_t ikm_len,
                      uint8_t prk[32])
{
    tls_hmac_sha256(salt, salt_len, ikm, ikm_len, prk);
}

void tls_hkdf_expand(const uint8_t prk[32],
                     const uint8_t* info, size_t info_len,
                     uint8_t* out, size_t out_len)
{
    uint8_t t[32];
    size_t off = 0;
    for (uint8_t i=1; off < out_len; i++) {
        tls_sha256_ctx ctx;
        tls_sha256_init(&ctx);
        if (i > 1) tls_sha256_update(&ctx, t, 32);
        tls_sha256_update(&ctx, info, info_len);
        tls_sha256_update(&ctx, &i, 1);
        tls_sha256_final(&ctx, t);
        size_t copy = (out_len - off < 32) ? (out_len - off) : 32;
        memcpy(out + off, t, copy);
        off += copy;
    }
}

void tls_hkdf_expand_label(const uint8_t secret[32],
                           const char* label, size_t label_len,
                           const uint8_t* ctx, size_t ctx_len,
                           uint8_t* out, size_t out_len)
{
    /* HkdfLabel = (uint16_t length) || "tls13 " || label || (uint8_t ctx_len) || ctx */
    size_t total = 2 + 6 + label_len + 1 + ctx_len;
    uint8_t info[128];
    info[0] = (uint8_t)(out_len >> 8);
    info[1] = (uint8_t)(out_len);
    memcpy(info+2, "tls13 ", 6);
    memcpy(info+8, label, label_len);
    info[8+label_len] = (uint8_t)ctx_len;
    memcpy(info+9+label_len, ctx, ctx_len);

    tls_hkdf_expand(secret, info, total, out, out_len);
}
