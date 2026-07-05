#include "tls_internal.h"
#include "hkdf.h"
#include "sha256.h"
#include "x25519.h"
#include <string.h>
#include <unistd.h>

/* Helper: hash a handshake message into the transcript */
static void hash_msg(struct tls_ctx* ctx, const uint8_t* msg, size_t len) {
    tls_sha256_update(&ctx->transcript, msg, len);
}

/* Build ClientHello and send it */
static int send_client_hello(struct tls_ctx* ctx) {
    uint8_t buf[512];
    size_t pos = 0;

    /* Handshake header (filled at end) */
    size_t hs_start = pos;
    buf[pos++] = TLS_HS_CLIENT_HELLO; /* msg_type */
    pos += 3; /* length placeholder */

    /* Legacy version: TLS 1.2 (0x0303) */
    buf[pos++] = 0x03; buf[pos++] = 0x03;

    /* Random: 32 bytes */
    memcpy(buf+pos, ctx->client_random, 32);
    pos += 32;

    /* Session ID: empty */
    buf[pos++] = 0;

    /* Cipher suites: TLS_AES_128_GCM_SHA256 (0x1301) */
    buf[pos++] = 0x00; buf[pos++] = 0x02; /* length = 2 */
    buf[pos++] = 0x13; buf[pos++] = 0x01;

    /* Compression methods: null (0x00) */
    buf[pos++] = 0x01; buf[pos++] = 0x00;

    /* Extensions */
    uint16_t ext_start = (uint16_t)pos;
    pos += 2; /* ext length placeholder */

    /* Extension 1: supported_versions (0x002b) */
    buf[pos++] = 0x00; buf[pos++] = 0x2b; /* type */
    buf[pos++] = 0x00; buf[pos++] = 0x03; /* length = 3 */
    buf[pos++] = 0x02; /* list length = 2 */
    buf[pos++] = 0x03; buf[pos++] = 0x04; /* TLS 1.3 */

    /* Extension 2: key_share (0x0033) - X25519 public key */
    buf[pos++] = 0x00; buf[pos++] = 0x33;
    buf[pos++] = 0x00; buf[pos++] = 0x2a; /* extension data length = 42 */
    buf[pos++] = 0x00; buf[pos++] = 0x28; /* client_shares length = 40 */
    buf[pos++] = 0x00; buf[pos++] = 0x1d; /* NamedGroup: x25519 */
    buf[pos++] = 0x00; buf[pos++] = 0x20; /* key_exchange length = 32 */
    /* Generate keypair */
    tls_x25519_keypair(ctx->x25519_pub, ctx->x25519_priv);
    memcpy(buf+pos, ctx->x25519_pub, 32);
    pos += 32;

    /* Extension 3: signature_algorithms (0x000d) */
    buf[pos++] = 0x00; buf[pos++] = 0x0d;
    buf[pos++] = 0x00; buf[pos++] = 0x08; /* ext data length = 8 */
    buf[pos++] = 0x00; buf[pos++] = 0x06; /* list length = 6 */
    buf[pos++] = 0x08; buf[pos++] = 0x04; /* rsa_pss_rsae_sha256 */
    buf[pos++] = 0x08; buf[pos++] = 0x07; /* ed25519 */
    buf[pos++] = 0x04; buf[pos++] = 0x01; /* ecdsa_secp256r1_sha256 */

    /* Extension 4: supported_groups (0x000a) */
    buf[pos++] = 0x00; buf[pos++] = 0x0a;
    buf[pos++] = 0x00; buf[pos++] = 0x04; /* ext data length = 4 */
    buf[pos++] = 0x00; buf[pos++] = 0x02; /* list length = 2 */
    buf[pos++] = 0x00; buf[pos++] = 0x1d; /* x25519 */

    /* Fill in extension length */
    uint16_t ext_len = (uint16_t)(pos - ext_start - 2);
    buf[ext_start] = (uint8_t)(ext_len >> 8);
    buf[ext_start+1] = (uint8_t)ext_len;

    /* Fill in handshake header length */
    uint32_t hs_len = (uint32_t)(pos - hs_start - 4);
    buf[hs_start+1] = (uint8_t)(hs_len >> 16);
    buf[hs_start+2] = (uint8_t)(hs_len >> 8);
    buf[hs_start+3] = (uint8_t)hs_len;

    /* Reset transcript and hash ClientHello */
    tls_sha256_init(&ctx->transcript);
    hash_msg(ctx, buf, pos);
    ctx->transcript_valid = 1;

    return tls_send_record(ctx, TLS_CONTENT_HANDSHAKE, buf, pos);
}

/* Parse ServerHello, extract server key_share, derive handshake secrets */
static int parse_server_hello(struct tls_ctx* ctx, const uint8_t* data, size_t len) {
    size_t pos = 0;
    if (pos+2 > len) return -1;
    uint16_t legacy_ver = ((uint16_t)data[pos]<<8)|data[pos+1]; pos+=2;
    (void)legacy_ver;

    if (pos+32 > len) return -1;
    memcpy(ctx->server_random, data+pos, 32); pos+=32;

    /* Skip session ID */
    if (pos+1 > len) return -1;
    uint8_t sid_len = data[pos++];
    pos += sid_len;

    /* Skip cipher suite (2 bytes) */
    if (pos+2 > len) return -1;
    uint16_t cs = ((uint16_t)data[pos]<<8)|data[pos+1]; pos+=2;
    if (cs != TLS_CIPHER_TLS_AES_128_GCM_SHA256) return -1;

    /* Skip compression method */
    if (pos+1 > len) return -1;
    pos++; /* null compression */

    /* Parse extensions */
    if (pos+2 > len) return -1;
    uint16_t ext_len = ((uint16_t)data[pos]<<8)|data[pos+1]; pos+=2;

    uint8_t server_pub[32] = {0};
    int found_key_share = 0;

    while (pos+4 <= len && ext_len > 0) {
        uint16_t ext_type = ((uint16_t)data[pos]<<8)|data[pos+1]; pos+=2;
        uint16_t e_len = ((uint16_t)data[pos]<<8)|data[pos+1]; pos+=2;
        ext_len -= 4;

        if (pos+e_len > len) return -1;

        if (ext_type == TLS_EXT_KEY_SHARE) {
            /* ServerKeyShare: NamedGroup(2) + key_exchange_length(2) + key */
            if (e_len < 4) return -1;
            uint16_t group = ((uint16_t)data[pos]<<8)|data[pos+1];
            uint16_t klen = ((uint16_t)data[pos+2]<<8)|data[pos+3];
            if (group != TLS_NAMED_GROUP_X25519 || klen != 32) return -1;
            if (pos+4+32 > pos+e_len) return -1;
            memcpy(server_pub, data+pos+4, 32);
            found_key_share = 1;
        }

        pos += e_len;
        ext_len -= e_len;
    }

    if (!found_key_share) return -1;
    memcpy(ctx->x25519_server_pub, server_pub, 32);

    /* Hash ServerHello */
    hash_msg(ctx, data-4, len+4); /* re-hash with TLS header (type+length) */

    /* Derive handshake secrets */
    /* early_secret = HKDF-Extract(0^32, 0^32) */
    uint8_t zero[32] = {0};
    tls_hkdf_extract(zero, 32, zero, 32, ctx->early_secret);

    /* empty_hash = SHA256("") */
    uint8_t empty_hash[32];
    tls_sha256((const uint8_t*)"", 0, empty_hash);

    /* derived_secret = HKDF-Expand-Label(early, "derived", empty_hash, 32) */
    uint8_t derived[32];
    tls_derive_secret(ctx->early_secret, "derived", empty_hash, 32, derived, 32);

    /* shared = X25519(our_priv, server_pub) */
    tls_x25519_shared(ctx->shared_secret, ctx->x25519_priv, server_pub);

    /* handshake_secret = HKDF-Extract(derived, shared) */
    tls_hkdf_extract(derived, 32, ctx->shared_secret, 32, ctx->handshake_secret);

    /* Get transcript hash up to ServerHello */
    uint8_t hs_hash[32];
    tls_sha256_ctx tmp;
    memcpy(&tmp, &ctx->transcript, sizeof(tmp));
    tls_sha256_final(&tmp, hs_hash);

    /* c_hs_traffic and s_hs_traffic */
    tls_derive_secret(ctx->handshake_secret, "c hs traffic", hs_hash, 32,
                      ctx->c_hs_traffic, 32);
    tls_derive_secret(ctx->handshake_secret, "s hs traffic", hs_hash, 32,
                      ctx->s_hs_traffic, 32);

    /* Derive handshake keys */
    tls_derive_secret(ctx->c_hs_traffic, "key", (const uint8_t*)"", 0,
                      ctx->c_hs_key, 16);
    tls_derive_secret(ctx->c_hs_traffic, "iv", (const uint8_t*)"", 0,
                      ctx->c_hs_iv, 12);
    tls_derive_secret(ctx->s_hs_traffic, "key", (const uint8_t*)"", 0,
                      ctx->s_hs_key, 16);
    tls_derive_secret(ctx->s_hs_traffic, "iv", (const uint8_t*)"", 0,
                      ctx->s_hs_iv, 12);

    /* Finished keys */
    tls_derive_secret(ctx->c_hs_traffic, "finished", (const uint8_t*)"", 0,
                      ctx->finished_key_c, 32);
    tls_derive_secret(ctx->s_hs_traffic, "finished", (const uint8_t*)"", 0,
                      ctx->finished_key_s, 32);

    /* Set epochs */
    ctx->epoch_c = 1;
    ctx->epoch_s = 1;

    return 0;
}

/* Verify server Finished message */
static int verify_server_finished(struct tls_ctx* ctx, const uint8_t* data, size_t len) {
    if (len != 32) return -1;

    /* Get transcript hash (up to but not including Finished) */
    uint8_t hs_hash[32];
    tls_sha256_ctx tmp;
    memcpy(&tmp, &ctx->transcript, sizeof(tmp));
    tls_sha256_final(&tmp, hs_hash);

    uint8_t expected[32];
    tls_hmac_sha256(ctx->finished_key_s, 32, hs_hash, 32, expected);

    if (memcmp(expected, data, 32) != 0) return -1;

    /* Hash the Finished message */
    hash_msg(ctx, data-4, len+4);
    return 0;
}

/* Send client Finished */
static int send_client_finished(struct tls_ctx* ctx) {
    /* Get transcript hash up to server Finished */
    uint8_t hs_hash[32];
    tls_sha256_ctx tmp;
    memcpy(&tmp, &ctx->transcript, sizeof(tmp));
    tls_sha256_final(&tmp, hs_hash);

    uint8_t verify_data[32];
    tls_hmac_sha256(ctx->finished_key_c, 32, hs_hash, 32, verify_data);

    uint8_t buf[36];
    buf[0] = TLS_HS_FINISHED;
    buf[1] = 0; buf[2] = 0; buf[3] = 32;
    memcpy(buf+4, verify_data, 32);

    int ret = tls_send_record(ctx, TLS_CONTENT_HANDSHAKE, buf, 36);
    if (ret < 0) return ret;

    /* Now derive master secret and application keys */
    uint8_t empty_hash[32];
    tls_sha256((const uint8_t*)"", 0, empty_hash);

    uint8_t derived2[32];
    tls_derive_secret(ctx->handshake_secret, "derived", empty_hash, 32, derived2, 32);

    uint8_t zero[32] = {0};
    tls_hkdf_extract(derived2, 32, zero, 32, ctx->master_secret);

    /* Final transcript hash (all messages including client Finished) */
    hash_msg(ctx, buf, 36);
    uint8_t final_hash[32];
    memcpy(&tmp, &ctx->transcript, sizeof(tmp));
    tls_sha256_final(&tmp, final_hash);

    tls_derive_secret(ctx->master_secret, "c ap traffic", final_hash, 32,
                      ctx->c_ap_traffic, 32);
    tls_derive_secret(ctx->master_secret, "s ap traffic", final_hash, 32,
                      ctx->s_ap_traffic, 32);

    tls_derive_secret(ctx->c_ap_traffic, "key", (const uint8_t*)"", 0,
                      ctx->c_ap_key, 16);
    tls_derive_secret(ctx->c_ap_traffic, "iv", (const uint8_t*)"", 0,
                      ctx->c_ap_iv, 12);
    tls_derive_secret(ctx->s_ap_traffic, "key", (const uint8_t*)"", 0,
                      ctx->s_ap_key, 16);
    tls_derive_secret(ctx->s_ap_traffic, "iv", (const uint8_t*)"", 0,
                      ctx->s_ap_iv, 12);

    ctx->epoch_c = 2; /* will switch to 3 on next send: actually epoch 3 */
    ctx->epoch_c = 3;
    ctx->epoch_s = 3;

    return 0;
}

/* Handle EncryptedExtensions (server -> client) */
static int parse_encrypted_extensions(struct tls_ctx* ctx, const uint8_t* data, size_t len) {
    (void)data; /* We don't need any extensions from the server */
    hash_msg(ctx, data-4, len+4);
    return 0;
}

/* Handle Certificate message (server -> client) - skip validation for now */
static int parse_certificate(struct tls_ctx* ctx, const uint8_t* data, size_t len) {
    hash_msg(ctx, data-4, len+4);
    (void)len;
    return 0;
}

/* Handle CertificateVerify (server -> client) - skip validation for now */
static int parse_certificate_verify(struct tls_ctx* ctx, const uint8_t* data, size_t len) {
    hash_msg(ctx, data-4, len+4);
    (void)len;
    return 0;
}

int tls_handshake(struct tls_ctx* ctx) {
    int ret;

    /* Step 1: Send ClientHello */
    ret = send_client_hello(ctx);
    if (ret < 0) return -1;

    /* Step 2: Receive ServerHello */
    while (1) {
        int type; uint8_t buf[4096]; size_t len;
        ret = tls_recv_record(ctx, &type, buf, &len, sizeof(buf));
        if (ret < 0) return -1;

        uint8_t hs_type = buf[0];
        uint32_t hs_len = ((uint32_t)buf[1]<<16)|((uint32_t)buf[2]<<8)|buf[3];

        if (type == TLS_CONTENT_HANDSHAKE && hs_type == TLS_HS_SERVER_HELLO) {
            /* Hash the handshake header + body */
            hash_msg(ctx, buf, hs_len+4);

            ret = parse_server_hello(ctx, buf+4, hs_len);
            if (ret < 0) return -1;
            break;
        }
        /* If we get a change_cipher_spec (legacy), skip it */
        if (type == TLS_CONTENT_CHANGECIPHERSPEC) continue;
        return -1;
    }

    /* Step 3: Receive encrypted handshake messages */
    int got_finished = 0;

    while (!got_finished) {
        int type; uint8_t buf[4096]; size_t len;
        ret = tls_recv_record(ctx, &type, buf, &len, sizeof(buf));
        if (ret < 0) return -1;

        /* Skip legacy change_cipher_spec */
        if (type == TLS_CONTENT_CHANGECIPHERSPEC) continue;

        if (type == TLS_CONTENT_HANDSHAKE) {
            uint8_t hs_type = buf[0];
            uint32_t hs_len = ((uint32_t)buf[1]<<16)|((uint32_t)buf[2]<<8)|buf[3];

            switch (hs_type) {
            case TLS_HS_ENCRYPTED_EXTENSIONS:
                ret = parse_encrypted_extensions(ctx, buf+4, hs_len);
                break;
            case TLS_HS_CERTIFICATE:
                ret = parse_certificate(ctx, buf+4, hs_len);
                break;
            case TLS_HS_CERTIFICATE_VERIFY:
                ret = parse_certificate_verify(ctx, buf+4, hs_len);
                break;
            case TLS_HS_FINISHED:
                ret = verify_server_finished(ctx, buf+4, hs_len);
                got_finished = 1;
                break;
            default:
                return -1;
            }
            if (ret < 0) return -1;
        } else {
            return -1;
        }
    }

    /* Step 4: Send Client Finished */
    ret = send_client_finished(ctx);
    if (ret < 0) return -1;

    ctx->handshake_done = 1;
    return 0;
}
