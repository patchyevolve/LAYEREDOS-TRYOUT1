#ifndef TLS_X25519_H
#define TLS_X25519_H

#include <stdint.h>

void tls_x25519_keypair(uint8_t pub[32], uint8_t priv[32]);
int  tls_x25519_shared(uint8_t shared[32], const uint8_t priv[32], const uint8_t pub[32]);

#endif
