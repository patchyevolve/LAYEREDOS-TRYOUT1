# OPERtur TLS Implementation Plan

**Version:** 1.0
**Target:** Minimal TLS 1.3 with TLS_AES_128_GCM_SHA256 (RFC 8446)
**Architecture:** Pure userspace library wrapping existing TCP socket API

---

## Overview

TLS is implemented as a **userspace library** (`libtls.a`) on top of the existing socket layer. No kernel changes required.

```
Application → libtls (encrypt/decrypt, handshake, X.509, AES/X25519/HKDF)
    → libuser (syscall wrappers) → kernel TCP socket API
```

---

## Task TT.1 — AES-128-GCM

**What:** AES-128 core + GCM AEAD mode.

**Sub-operations:**
- `aes128_key_expand(key, round_keys)` — 10-round key schedule
- `aes128_encrypt_block(block, round_keys)` — single block encrypt
- `aes128_ctr(key, nonce, plaintext, ciphertext, len)` — CTR mode
- `aes128_gcm_encrypt(key, nonce, aad, pt, ct, tag)` — AEAD encrypt
- `aes128_gcm_decrypt(key, nonce, aad, ct, tag, pt)` — AEAD decrypt

**Files:** `libtls/aes.h`, `libtls/aes.c`, `libtls/aes_gcm.h`, `libtls/aes_gcm.c`

**Verification:** NIST AESAVS known-answer tests + encrypt/decrypt round-trip.

**Depends on:** Nothing

---

## Task TT.2 — ChaCha20-Poly1305

**What:** ChaCha20 stream cipher + Poly1305 MAC + AEAD (RFC 8439). Optional cipher suite for TLS_CHACHA20_POLY1305_SHA256.

**Files:** `libtls/chacha20.h/c`, `libtls/poly1305.h/c`, `libtls/chacha20_poly1305.h/c`

**Verification:** RFC 8439 test vectors.

**Depends on:** Nothing

---

## Task TT.3 — X25519 (ECDHE)

**What:** Curve25519 Diffie-Hellman key exchange (RFC 7748).

**Sub-operations:**
- `x25519_keypair(pub, priv)` — generate keypair
- `x25519_shared(shared, priv, pub)` — compute shared secret
- Montgomery ladder scalar multiplication (constant-time)
- Field arithmetic mod 2^255-19

**Files:** `libtls/x25519.h`, `libtls/x25519.c`

**Verification:** RFC 7748 test vectors.

**Depends on:** Nothing (pure integer arithmetic)

---

## Task TT.4 — HMAC-SHA256 + HKDF

**What:** HMAC (RFC 2104) + HKDF-Extract-and-Expand (RFC 5869).

**Sub-operations:**
- `hmac_sha256(key, data, len, out[32])`
- `hkdf_extract(salt, ikm, len, prk[32])`
- `hkdf_expand(prk, info, len, out, out_len)`
- `hkdf(salt, ikm, ikm_len, info, info_len, out, out_len)`

**Files:** `libtls/hkdf.h`, `libtls/hkdf.c`

**Note:** SHA-256 must be ported from kernel to userspace (~100 lines). Avoids syscall overhead for thousands of hashes during handshake.

**Verification:** RFC 4231 HMAC test vectors + RFC 5869 HKDF test vectors.

**Depends on:** SHA-256 (ported to libtls)

---

## Task TT.5 — X.509 Certificate Parser

**What:** Parse DER-encoded X.509v3 certificates.

**Sub-operations:**
- ASN.1 DER primitive parser (tag, length, value)
- `x509_parse_cert(der, len, &cert)` — extract TBSCertificate, subject, issuer, SPKI
- `x509_extract_pubkey(cert, &type, data, &len)` — raw public key
- Initial: **no chain validation** (accept any cert, like `curl -k`)
- Future: Ed25519 signature verification + trusted roots

**Files:** `libtls/x509.h`, `libtls/x509.c`

**Verification:** Parse known-good DER certificate, extract correct fields.

**Depends on:** Nothing

---

## Task TT.6 — TLS 1.3 Record Layer

**What:** TLS record framing and encryption (RFC 8446 §5).

**Record format:** ContentType (1B) + legacy_version (2B: 0x0303) + length (2B) + fragment.

Encrypted records use AEAD with 16-byte auth tag. Content type of inner plaintext is appended before encryption.

**Sub-operations:**
- `tls_send_record(ctx, type, data, len)` — frame + encrypt (if epoch > 0)
- `tls_recv_record(ctx, &type, buf, &len)` — receive + decrypt

**Files:** `libtls/tls_record.h`, `libtls/tls_record.c`

**Depends on:** TT.1 (AES-GCM), TT.2 (ChaCha20-Poly1305), TT.4 (HKDF for key derivation)

---

## Task TT.7 — TLS 1.3 Client Handshake

**What:** TLS 1.3 client handshake (RFC 8446 §4).

**Message flow:**
```
ClientHello (key_share: X25519 pub)  -------->
                                        ServerHello (key_share)
                                        EncryptedExtensions
                                        Certificate (opt)
                                        CertificateVerify (opt)
                                        Finished
                    <--------
Finished            -------->
[Application Data]  <------->  [Application Data]
```

**Sub-operations:**
- `tls_build_clienthello(ctx)` — cipher suite TLS_AES_128_GCM_SHA256, X25519 key_share, supported_versions, random
- `tls_parse_serverhello(ctx, data, len)` — extract key_share, derive handshake secret
- `tls_parse_encrypted_extensions(ctx, data, len)`
- `tls_parse_certificate(ctx, data, len)` — store for potential verification
- `tls_parse_certificate_verify(ctx, data, len)` — skip signature check initially
- `tls_parse_finished(ctx, data, len)` — verify server Finished MAC
- `tls_send_finished(ctx)` — client Finished
- `tls_handshake(ctx, sockfd)` — orchestrate full handshake

**Files:** `libtls/tls_handshake.h`, `libtls/tls_handshake.c`

**Depends on:** TT.3 (X25519), TT.4 (HKDF), TT.5 (X.509), TT.6 (Record Layer)

---

## Task TT.8 — Public TLS API

**What:** Public API for TLS connections.

```c
typedef struct tls_ctx tls_ctx_t;
tls_ctx_t* tls_new(int sockfd);
int tls_connect(tls_ctx_t* ctx);
ssize_t tls_send(tls_ctx_t* ctx, const void* buf, size_t len);
ssize_t tls_recv(tls_ctx_t* ctx, void* buf, size_t len);
int tls_close(tls_ctx_t* ctx);
```

**Files:** `libtls/tls.h`, `libtls/tls.c`

**Depends on:** TT.7 (Handshake), TT.6 (Record Layer)

---

## Task TT.9 — Build + Test

**What:** Integrate libtls into build, create test program.

**Files:**
- `os/Makefile` — libtls target
- `os/src/libtls/` — all library source
- `os/src/boot/https_get-c.c` — HTTPS GET test via SLiRP hostfwd

**Test:**
```
socket(AF_INET, SOCK_STREAM, 0)
connect(sockfd, example.com:443)
tls_new(sockfd) → tls_connect(ctx)
tls_send(ctx, "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n")
tls_recv(ctx, buf, len) → verify "200 OK" in response
tls_close(ctx)
```

**Verification:** HTTPS GET succeeds over SLiRP forwarding (host NAT).

**Depends on:** TT.8 (TLS API)

---

## Summary

| Task | Files | Est. lines | Difficulty |
|------|-------|-----------|------------|
| TT.1 AES-128-GCM | 4 | ~500 | Medium |
| TT.2 ChaCha20-Poly1305 | 6 | ~500 | Medium |
| TT.3 X25519 | 2 | ~400 | Hard |
| TT.4 HMAC + HKDF | 2 | ~150 | Easy |
| TT.5 X.509 Parser | 2 | ~400 | Medium |
| TT.6 Record Layer | 2 | ~200 | Medium |
| TT.7 Handshake | 2 | ~500 | Hard |
| TT.8 TLS API | 2 | ~100 | Easy |
| TT.9 Build + Test | ~3 | ~100 | Easy |
| **Total** | **~25** | **~2850** | |

## File Layout
```
os/src/libtls/
    aes.h, aes.c
    aes_gcm.h, aes_gcm.c
    chacha20.h, chacha20.c
    poly1305.h, poly1305.c
    chacha20_poly1305.h, chacha20_poly1305.c
    x25519.h, x25519.c
    sha256.h, sha256.c          (ported from kernel)
    hkdf.h, hkdf.c
    x509.h, x509.c
    tls.h, tls.c
    tls_record.h, tls_record.c
    tls_handshake.h, tls_handshake.c
```

## Kernel FAQ

**Q: Do we need new syscalls?** No. TLS uses existing socket/connect/send/recv/close/getrandom.

**Q: Do we need kernel TLS offload?** No. Userspace crypto is sufficient for single HTTPS connections.

**Q: Does SLiRP affect TLS?** No. SLiRP transparently forwards TCP — TLS works over forwarded connections.

**Q: Server-side TLS (listener)?** Deferred. Initial implementation is client-only (outgoing HTTPS).
