#ifndef TLS_AES_H
#define TLS_AES_H

#include <stdint.h>
#include <stddef.h>

#define TLS_AES128_KEY_SIZE 16
#define TLS_AES_BLOCK_SIZE  16
#define TLS_AES128_ROUNDS   10

void tls_aes128_key_expand(const uint8_t key[16], uint8_t rk[16*(TLS_AES128_ROUNDS+1)]);
void tls_aes128_encrypt_block(const uint8_t rk[16*(TLS_AES128_ROUNDS+1)],
                              const uint8_t in[16], uint8_t out[16]);

/* GCM mode */
void tls_aes128_gcm_encrypt(const uint8_t key[16], const uint8_t nonce[12],
                            const uint8_t* aad, size_t aad_len,
                            const uint8_t* plain, size_t plain_len,
                            uint8_t* cipher, uint8_t tag[16]);

int tls_aes128_gcm_decrypt(const uint8_t key[16], const uint8_t nonce[12],
                           const uint8_t* aad, size_t aad_len,
                           const uint8_t* cipher, size_t cipher_len,
                           const uint8_t tag[16], uint8_t* plain);

#endif
