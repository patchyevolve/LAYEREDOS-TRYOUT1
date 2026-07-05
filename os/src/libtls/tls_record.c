#include "tls_internal.h"
#include "hkdf.h"
#include "aes.h"
#include <string.h>

void tls_derive_secret(const uint8_t* secret, const char* label,
                       const uint8_t* hash, size_t hash_len,
                       uint8_t* out, size_t out_len)
{
    tls_hkdf_expand_label(secret, label, strlen(label), hash, hash_len, out, out_len);
}

static void tls_aead_nonce(const uint8_t iv[12], uint64_t seq, uint8_t nonce[12]) {
    for (int i=0;i<12;i++) {
        uint8_t byte = (i<4) ? 0 : (uint8_t)(seq >> (8*(11-i)));
        nonce[i] = iv[i] ^ byte;
    }
}

int tls_encrypt(struct tls_ctx* ctx, int epoch, const uint8_t* plain, size_t plain_len,
                uint8_t type, uint8_t* out, size_t* out_len)
{
    const uint8_t *key, *iv;
    uint64_t seq;
    if (epoch == 2) {
        key = ctx->c_hs_key; iv = ctx->c_hs_iv; seq = ctx->seq_c;
    } else if (epoch == 3) {
        key = ctx->c_ap_key; iv = ctx->c_ap_iv; seq = ctx->seq_c;
    } else return -1;

    uint8_t nonce[12];
    tls_aead_nonce(iv, seq, nonce);

    /* TLS inner plaintext: append content type */
    uint8_t inner[16384];
    memcpy(inner, plain, plain_len);
    inner[plain_len] = type;

    /* Additional data: record header (5 bytes) */
    uint8_t aad[5];
    aad[0]=TLS_CONTENT_APPLICATIONDATA;
    aad[1]=0x03; aad[2]=0x03; /* TLS 1.2 legacy version */
    uint16_t ct_len = (uint16_t)(plain_len + 1 + 16); /* + type + tag */
    aad[3]=(uint8_t)(ct_len>>8); aad[4]=(uint8_t)ct_len;

    tls_aes128_gcm_encrypt(key, nonce, aad, 5, inner, plain_len+1, out+5, out);

    /* Build record header */
    out[0]=TLS_CONTENT_APPLICATIONDATA;
    out[1]=0x03; out[2]=0x03;
    out[3]=(uint8_t)((plain_len+1+16)>>8);
    out[4]=(uint8_t)(plain_len+1+16);
    *out_len = plain_len + 1 + 16 + 5;

    return 0;
}

int tls_decrypt(struct tls_ctx* ctx, int epoch, const uint8_t* in, size_t in_len,
                uint8_t* type, uint8_t* out, size_t* out_len)
{
    const uint8_t *key, *iv;
    uint64_t seq;
    if (epoch == 2) {
        key = ctx->s_hs_key; iv = ctx->s_hs_iv; seq = ctx->seq_s;
    } else if (epoch == 3) {
        key = ctx->s_ap_key; iv = ctx->s_ap_iv; seq = ctx->seq_s;
    } else return -1;

    if (in_len < 5+16) return -1;

    uint8_t nonce[12];
    tls_aead_nonce(iv, seq, nonce);

    size_t ct_len = in_len - 5 - 16;
    uint8_t aad[5];
    aad[0]=TLS_CONTENT_APPLICATIONDATA;
    aad[1]=0x03; aad[2]=0x03;
    aad[3]=(uint8_t)((ct_len+16)>>8); aad[4]=(uint8_t)(ct_len+16);

    uint8_t inner[16384];
    int ret = tls_aes128_gcm_decrypt(key, nonce, aad, 5,
                                     in+5, ct_len, in+5+ct_len, inner);
    if (ret < 0) return -1;

    *out_len = ct_len - 1;
    *type = inner[ct_len - 1];
    memcpy(out, inner, *out_len);
    return 0;
}

int tls_send_record(struct tls_ctx* ctx, int type, const uint8_t* data, size_t len) {
    uint8_t buf[16384];
    size_t total;

    if (ctx->epoch_c == 0) {
        buf[0]=(uint8_t)type;
        buf[1]=0x03; buf[2]=0x03;
        buf[3]=(uint8_t)(len>>8); buf[4]=(uint8_t)len;
        memcpy(buf+5, data, len);
        total = len + 5;
    } else {
        int epoch = (ctx->epoch_c == 1) ? 2 : ctx->epoch_c;
        if (tls_encrypt(ctx, epoch, data, len, (uint8_t)type, buf, &total) < 0)
            return -1;
    }

    long ret;
    asm volatile("int $0x80":"=a"(ret):"a"(43),"D"((long)ctx->sockfd),
                 "S"((long)buf),"d"((long)total):"memory");
    if (ret > 0) ctx->seq_c++;
    return (int)ret;
}

int tls_recv_record(struct tls_ctx* ctx, int* type, uint8_t* buf, size_t* len, size_t cap) {
    /* Try to consume from receive buffer first */
    if (ctx->recv_len > ctx->recv_pos) {
        size_t avail = ctx->recv_len - ctx->recv_pos;
        if (avail < 5) { ctx->recv_len=ctx->recv_pos=0; }
        else {
            size_t rlen = ((size_t)ctx->recv_buf[ctx->recv_pos+3]<<8) |
                          ctx->recv_buf[ctx->recv_pos+4];
            if (avail >= (size_t)(5 + rlen)) {
                *type = ctx->recv_buf[ctx->recv_pos];
                size_t total = 5 + rlen;
                size_t start = ctx->recv_pos;
                ctx->recv_pos += total;

                if (ctx->epoch_s == 0) {
                    memcpy(buf, ctx->recv_buf+start+5, rlen);
                    *len = rlen;
                } else {
                    int epoch = (ctx->epoch_s == 1) ? 2 : ctx->epoch_s;
                    if (tls_decrypt(ctx, epoch, ctx->recv_buf+start, total,
                                    (uint8_t*)type, buf, len) < 0)
                        return -1;
                }
                ctx->seq_s++;
                return 0;
            }
        }
    }

    /* Read more data from socket */
    long ret;
    asm volatile("int $0x80":"=a"(ret):"a"(44),"D"((long)ctx->sockfd),
                 "S"((long)ctx->recv_buf),"d"((long)sizeof(ctx->recv_buf)):);
    if (ret <= 0) return -1;
    ctx->recv_len = (int)ret;
    ctx->recv_pos = 0;

    /* Now parse from buffer */
    if (ctx->recv_len < 5) return -1;
    size_t rlen = ((size_t)ctx->recv_buf[3]<<8) | ctx->recv_buf[4];
    if ((size_t)ctx->recv_len < 5 + rlen) return -1;

    *type = ctx->recv_buf[0];
    ctx->recv_pos = 5 + (int)rlen;

    if (ctx->epoch_s == 0) {
        memcpy(buf, ctx->recv_buf+5, rlen);
        *len = rlen;
    } else {
        int epoch = (ctx->epoch_s == 1) ? 2 : ctx->epoch_s;
        if (tls_decrypt(ctx, epoch, ctx->recv_buf, (size_t)(5+rlen),
                        (uint8_t*)type, buf, len) < 0)
            return -1;
    }
    ctx->seq_s++;
    return 0;
}
