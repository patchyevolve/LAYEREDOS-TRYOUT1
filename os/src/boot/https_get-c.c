/* TLS library self-test */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <tls.h>
#include <tls_internal.h>
#include <sha256.h>
#include <aes.h>
#include <x25519.h>
#include <hkdf.h>

static int passed, failed;

#define TEST(name) write(STDOUT_FILENO, "  ", 2); write(STDOUT_FILENO, name, strlen(name)); write(STDOUT_FILENO, "\n", 1)
#define PASS() do { passed++; } while(0)
#define FAIL(msg) do { \
    write(STDOUT_FILENO, "    FAIL: ", 9); \
    write(STDOUT_FILENO, msg, strlen(msg)); \
    write(STDOUT_FILENO, "\n", 1); failed++; \
} while(0)

static int memeq(const uint8_t* a, const uint8_t* b, size_t n) {
    for (size_t i=0;i<n;i++) if (a[i]!=b[i]) return 0;
    return 1;
}

static void test_sha256(void) {
    TEST("SHA-256 empty string");
    uint8_t h[32]; tls_sha256((const uint8_t*)"",0,h);
    uint8_t e[32]={0xe3,0xb0,0xc4,0x42,0x98,0xfc,0x1c,0x14,0x9a,0xfb,0xf4,0xc8,0x99,0x6f,0xb9,0x24,0x27,0xae,0x41,0xe4,0x64,0x9b,0x93,0x4c,0xa4,0x95,0x99,0x1b,0x78,0x52,0xb8,0x55};
    if (memeq(h,e,32)) PASS(); else FAIL("empty");
    TEST("SHA-256 'abc'");
    tls_sha256((const uint8_t*)"abc",3,h);
    uint8_t e2[32]={0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
    if (memeq(h,e2,32)) PASS(); else FAIL("abc");
}

static void test_aes128(void) {
    TEST("AES-128 block");
    uint8_t k[16]={0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c};
    uint8_t p[16]={0x32,0x43,0xf6,0xa8,0x88,0x5a,0x30,0x8d,0x31,0x31,0x98,0xa2,0xe0,0x37,0x07,0x34};
    uint8_t e[16]={0x39,0x25,0x84,0x1d,0x02,0xdc,0x09,0xfb,0xdc,0x11,0x85,0x97,0x19,0x6a,0x0b,0x32};
    uint8_t rk[176],o[16];
    tls_aes128_key_expand(k,rk); tls_aes128_encrypt_block(rk,p,o);
    if (memeq(o,e,16)) PASS(); else FAIL("encrypt");

    TEST("AES-128-GCM round-trip");
    uint8_t key[16]={0},nonce[12]={0},pt[16]={0},ct[16],tag[16],dec[16];
    tls_aes128_gcm_encrypt(key,nonce,NULL,0,pt,16,ct,tag);
    int r=tls_aes128_gcm_decrypt(key,nonce,NULL,0,ct,16,tag,dec);
    if (r==0&&memeq(pt,dec,16)) PASS(); else FAIL("GCM");
}

static void test_x25519(void) {
    TEST("X25519 DH");
    uint8_t ap[32],bp[32],apub[32],bpub[32],sa[32],sb[32];
    memset(ap,0xAB,32);ap[0]&=248;ap[31]&=127;ap[31]|=64;
    memset(bp,0xCD,32);bp[0]&=248;bp[31]&=127;bp[31]|=64;
    uint8_t base[32];memset(base,0,32);base[0]=9;
    tls_x25519_shared(apub,ap,base);tls_x25519_shared(bpub,bp,base);
    tls_x25519_shared(sa,ap,bpub);tls_x25519_shared(sb,bp,apub);
    if (memeq(sa,sb,32)) PASS(); else FAIL("mismatch");
}

static void test_hmac(void) {
    TEST("HMAC-SHA256");
    uint8_t key[4]={'J','E','P','E'};
    const char* d="what do ya want for nothing?";
    uint8_t e[32]={0x5b,0xdc,0xc1,0x46,0xbf,0x60,0x75,0x4e,0x6a,0x04,0x24,0x26,0x08,0x95,0x75,0xc7,0x5a,0x00,0x3f,0x08,0x9d,0x27,0x39,0x83,0x9d,0xec,0x58,0xb9,0x64,0xec,0x38,0x43};
    uint8_t o[32];
    tls_hmac_sha256(key,4,(const uint8_t*)d,strlen(d),o);
    if (memeq(o,e,32)) PASS(); else FAIL("mismatch");
}

static void test_hkdf(void) {
    TEST("HKDF-Extract");
    uint8_t s[13]={0},ikm[22]={0},prk[32];
    uint8_t e[32]={0x07,0x77,0x09,0x36,0x2c,0x2e,0x32,0xdf,0x0d,0xdc,0x3f,0x0d,0xc4,0x7b,0xba,0x63,0x90,0xb6,0xc7,0x3b,0xb5,0x0f,0x9c,0x31,0x22,0xec,0x84,0x4a,0xd7,0xc2,0xb3,0xe5};
    tls_hkdf_extract(s,13,ikm,22,prk);
    if (memeq(prk,e,32)) PASS(); else FAIL("extract");
}

int main(void) {
    passed=failed=0;
    write(STDOUT_FILENO,"=== TLS crypto self-tests ===\n",30);
    test_sha256(); test_aes128(); test_x25519(); test_hmac(); test_hkdf();
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "Results: %d pass, %d fail\n", passed, failed);
    write(STDOUT_FILENO, buf, (size_t)n);
    return failed>0?1:0;
}
