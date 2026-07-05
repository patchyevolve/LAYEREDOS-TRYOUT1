#ifndef TLS_INTERNAL_H
#define TLS_INTERNAL_H

#include <stdint.h>
#include "sha256.h"

#define TLS_CONTENT_CHANGECIPHERSPEC 20
#define TLS_CONTENT_ALERT            21
#define TLS_CONTENT_HANDSHAKE        22
#define TLS_CONTENT_APPLICATIONDATA  23

#define TLS_HS_CLIENT_HELLO           1
#define TLS_HS_SERVER_HELLO           2
#define TLS_HS_ENCRYPTED_EXTENSIONS   8
#define TLS_HS_CERTIFICATE           11
#define TLS_HS_CERTIFICATE_VERIFY    15
#define TLS_HS_FINISHED              20

#define TLS_CIPHER_TLS_AES_128_GCM_SHA256 0x1301

#define TLS_EXT_SUPPORTED_VERSIONS  0x002b
#define TLS_EXT_KEY_SHARE           0x0033
#define TLS_EXT_SIGNATURE_ALGORITHMS 0x000d
#define TLS_EXT_SUPPORTED_GROUPS    0x000a

#define TLS_NAMED_GROUP_X25519      0x001d

#define TLS_SIG_ED25519             0x0807
#define TLS_SIG_RSA_PSS_RSAE_SHA256 0x0804

struct tls_ctx {
    int sockfd;
    int handshake_done;

    uint8_t client_random[32];
    uint8_t server_random[32];

    uint8_t x25519_priv[32];
    uint8_t x25519_pub[32];
    uint8_t x25519_server_pub[32];
    uint8_t shared_secret[32];

    uint8_t early_secret[32];
    uint8_t handshake_secret[32];
    uint8_t master_secret[32];

    uint8_t c_hs_traffic[32];
    uint8_t s_hs_traffic[32];
    uint8_t c_hs_key[16], c_hs_iv[12];
    uint8_t s_hs_key[16], s_hs_iv[12];

    uint8_t c_ap_traffic[32];
    uint8_t s_ap_traffic[32];
    uint8_t c_ap_key[16], c_ap_iv[12];
    uint8_t s_ap_key[16], s_ap_iv[12];

    uint8_t finished_key_c[32];
    uint8_t finished_key_s[32];

    uint64_t seq_c;
    uint64_t seq_s;

    int epoch_c;
    int epoch_s;

    tls_sha256_ctx transcript;
    int transcript_valid;

    uint8_t recv_buf[16384];
    int recv_len;
    int recv_pos;
};

int tls_send_record(struct tls_ctx* ctx, int type, const uint8_t* data, size_t len);
int tls_recv_record(struct tls_ctx* ctx, int* type, uint8_t* buf, size_t* len, size_t cap);
int tls_encrypt(struct tls_ctx* ctx, int epoch, const uint8_t* plain, size_t plain_len,
                uint8_t type, uint8_t* out, size_t* out_len);
int tls_decrypt(struct tls_ctx* ctx, int epoch, const uint8_t* in, size_t in_len,
                uint8_t* type, uint8_t* out, size_t* out_len);
void tls_derive_secret(const uint8_t* secret, const char* label,
                       const uint8_t* hash, size_t hash_len,
                       uint8_t* out, size_t out_len);
int tls_handshake(struct tls_ctx* ctx);

#endif
