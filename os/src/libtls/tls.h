#ifndef TLS_H
#define TLS_H

#include <stdint.h>
#include <stddef.h>

typedef struct tls_ctx tls_ctx_t;

tls_ctx_t* tls_new(int sockfd);
int tls_connect(tls_ctx_t* ctx);
ssize_t tls_send(tls_ctx_t* ctx, const void* buf, size_t len);
ssize_t tls_recv(tls_ctx_t* ctx, void* buf, size_t len);
int tls_close(tls_ctx_t* ctx);
void tls_free(tls_ctx_t* ctx);

#endif
