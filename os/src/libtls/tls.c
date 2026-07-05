#include "tls.h"
#include "tls_internal.h"
#include "hkdf.h"
#include "sha256.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

tls_ctx_t* tls_new(int sockfd) {
    struct tls_ctx* ctx = (struct tls_ctx*)malloc(sizeof(struct tls_ctx));
    if (!ctx) return NULL;
        memset(ctx, 0, sizeof(struct tls_ctx));
    ctx->sockfd = sockfd;
    ctx->epoch_c = 0;
    ctx->epoch_s = 0;
    ctx->seq_c = 0;
    ctx->seq_s = 0;
    ctx->handshake_done = 0;
    ctx->recv_len = 0;
    ctx->recv_pos = 0;

    /* Generate client random */
    long ret;
    asm volatile("int $0x80":"=a"(ret):"a"(63),"D"((long)ctx->client_random),
                 "S"(32),"d"(0):"memory");

    return ctx;
}

int tls_connect(struct tls_ctx* ctx) {
    if (!ctx) return -1;
    return tls_handshake(ctx);
}

ssize_t tls_send(struct tls_ctx* ctx, const void* buf, size_t len) {
    if (!ctx || !ctx->handshake_done) return -1;
    long ret = tls_send_record(ctx, TLS_CONTENT_APPLICATIONDATA,
                               (const uint8_t*)buf, len);
    return (ret > 0) ? (ssize_t)len : -1;
}

ssize_t tls_recv(struct tls_ctx* ctx, void* buf, size_t len) {
    if (!ctx || !ctx->handshake_done) return -1;

    int type;
    size_t rlen = 0;
    int ret = tls_recv_record(ctx, &type, (uint8_t*)buf, &rlen, len);
    if (ret < 0) {
        errno = ECONNRESET;
        return -1;
    }
    if (type == TLS_CONTENT_ALERT) {
        errno = ECONNRESET;
        return -1;
    }
    return (ssize_t)rlen;
}

int tls_close(struct tls_ctx* ctx) {
    if (!ctx) return -1;
    /* Send close_notify alert */
    uint8_t alert[2] = {1, 0}; /* warning, close_notify */
    int ret = tls_send_record(ctx, TLS_CONTENT_ALERT, alert, 2);
    close(ctx->sockfd);
    return ret;
}

void tls_free(struct tls_ctx* ctx) {
    if (ctx) {
        /* Zero secrets before freeing */
    memset(ctx, 0, sizeof(struct tls_ctx));
        free(ctx);
    }
}
