#include "aes.h"
#include <string.h>

static const uint8_t sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static const uint8_t rcon[11] = {0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36};

static uint8_t xtime(uint8_t a) { return (uint8_t)((a<<1)^(((a>>7)&1)*0x1b)); }

static void gf_mix_column(uint8_t* r) {
    uint8_t t=r[0]^r[1]^r[2]^r[3], u=r[0];
    r[0]^=t^xtime(r[0]^r[1]); r[1]^=t^xtime(r[1]^r[2]);
    r[2]^=t^xtime(r[2]^r[3]); r[3]^=t^xtime(r[3]^u);
}

static void sub_word(uint8_t* w) {
    w[0]=sbox[w[0]];w[1]=sbox[w[1]];w[2]=sbox[w[2]];w[3]=sbox[w[3]];
}
static void rot_word(uint8_t* w) {
    uint8_t t=w[0];w[0]=w[1];w[1]=w[2];w[2]=w[3];w[3]=t;
}

void tls_aes128_key_expand(const uint8_t key[16], uint8_t rk[176]) {
    memcpy(rk,key,16);
    for (int i=4;i<44;i++) {
        uint8_t temp[4];
        memcpy(temp,rk+4*(i-1),4);
        if (i%4==0) { rot_word(temp);sub_word(temp);temp[0]^=rcon[i/4]; }
        for (int j=0;j<4;j++) rk[4*i+j]=rk[4*(i-4)+j]^temp[j];
    }
}

void tls_aes128_encrypt_block(const uint8_t rk[176], const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16]; memcpy(s,in,16);
    for (int i=0;i<16;i++) s[i]^=rk[i];
    for (int r=1;r<=10;r++) {
        for (int i=0;i<16;i++) s[i]=sbox[s[i]];
        uint8_t t[16];
        t[0]=s[0];t[1]=s[5];t[2]=s[10];t[3]=s[15];
        t[4]=s[4];t[5]=s[9];t[6]=s[14];t[7]=s[3];
        t[8]=s[8];t[9]=s[13];t[10]=s[2];t[11]=s[7];
        t[12]=s[12];t[13]=s[1];t[14]=s[6];t[15]=s[11];
        memcpy(s,t,16);
        if (r<10) { gf_mix_column(s);gf_mix_column(s+4);gf_mix_column(s+8);gf_mix_column(s+12); }
        for (int i=0;i<16;i++) s[i]^=rk[r*16+i];
    }
    memcpy(out,s,16);
}

/* GCM GHASH */
static void gcm_mul(uint8_t* x, const uint8_t* y) {
    uint8_t z[16]={0},v[16]; memcpy(v,y,16);
    for (int b=0;b<128;b++) {
        if (x[b/8]&(0x80>>(b%8)))
            for (int j=0;j<16;j++) z[j]^=v[j];
        uint8_t lsb=v[15]&1;
        for (int j=15;j>0;j--) v[j]=(v[j]>>1)|(v[j-1]<<7);
        v[0]>>=1;
        if (lsb) v[0]^=0xe1;
    }
    memcpy(x,z,16);
}

static void gcm_ghash(const uint8_t* h, const uint8_t* data, size_t len, uint8_t out[16]) {
    static const uint8_t zero[16]={0};
    memcpy(out,zero,16);
    for (size_t i=0;i<len;i+=16) {
        uint8_t block[16];
        size_t rem=(len-i<16)?len-i:16;
        memcpy(block,data+i,rem);
        if (rem<16) memset(block+rem,0,16-rem);
        for (int j=0;j<16;j++) out[j]^=block[j];
        gcm_mul(out,h);
    }
}

static void inc32(uint8_t* x) { for (int i=15;i>=12;i--) if (++x[i]) break; }

void tls_aes128_gcm_encrypt(const uint8_t key[16],const uint8_t nonce[12],
    const uint8_t* aad,size_t aad_len,const uint8_t* plain,size_t plain_len,
    uint8_t* cipher,uint8_t tag[16])
{
    uint8_t rk[176]; tls_aes128_key_expand(key,rk);
    uint8_t h[16]={0}; tls_aes128_encrypt_block(rk,h,h);
    uint8_t j0[16]; memcpy(j0,nonce,12); j0[12]=j0[13]=j0[14]=0;j0[15]=1;

    uint8_t cb[16]; memcpy(cb,j0,16);
    for (size_t i=0;i<plain_len;i+=16) {
        inc32(cb); uint8_t e[16]; tls_aes128_encrypt_block(rk,cb,e);
        size_t rem=(plain_len-i<16)?plain_len-i:16;
        for (size_t j=0;j<rem;j++) cipher[i+j]=plain[i+j]^e[j];
    }

    /* Build GHASH input: AAD || pad(AAD) || ciphertext || pad(CT) || len(A) || len(C) */
    size_t aad_pad=(aad_len%16)?(16-aad_len%16):0;
    size_t ct_pad=(plain_len%16)?(16-plain_len%16):0;
    uint8_t auth[256]; memset(auth,0,sizeof(auth)); size_t off=0;
    memcpy(auth,aad,aad_len); off+=aad_len+aad_pad;
    memcpy(auth+off,cipher,plain_len); off+=plain_len+ct_pad;
    uint64_t ab=(uint64_t)aad_len*8,cb64=(uint64_t)plain_len*8;
    for (int i=0;i<8;i++) auth[off+i]=(uint8_t)(ab>>(56-8*i));
    for (int i=0;i<8;i++) auth[off+8+i]=(uint8_t)(cb64>>(56-8*i));
    uint8_t s[16]; gcm_ghash(h,auth,off+16,s);
    uint8_t e0[16]; tls_aes128_encrypt_block(rk,j0,e0);
    for (int i=0;i<16;i++) tag[i]=s[i]^e0[i];
}

int tls_aes128_gcm_decrypt(const uint8_t key[16],const uint8_t nonce[12],
    const uint8_t* aad,size_t aad_len,const uint8_t* cipher,size_t cipher_len,
    const uint8_t tag[16],uint8_t* plain)
{
    uint8_t rk[176]; tls_aes128_key_expand(key,rk);
    uint8_t h[16]={0}; tls_aes128_encrypt_block(rk,h,h);
    uint8_t j0[16]; memcpy(j0,nonce,12); j0[12]=j0[13]=j0[14]=0;j0[15]=1;

    size_t aad_pad=(aad_len%16)?(16-aad_len%16):0;
    size_t ct_pad=(cipher_len%16)?(16-cipher_len%16):0;
    uint8_t auth[256]; memset(auth,0,sizeof(auth)); size_t off=0;
    memcpy(auth,aad,aad_len); off+=aad_len+aad_pad;
    memcpy(auth+off,cipher,cipher_len); off+=cipher_len+ct_pad;
    uint64_t ab=(uint64_t)aad_len*8,cb64=(uint64_t)cipher_len*8;
    for (int i=0;i<8;i++) auth[off+i]=(uint8_t)(ab>>(56-8*i));
    for (int i=0;i<8;i++) auth[off+8+i]=(uint8_t)(cb64>>(56-8*i));
    uint8_t s[16]; gcm_ghash(h,auth,off+16,s);
    uint8_t e0[16]; tls_aes128_encrypt_block(rk,j0,e0);
    for (int i=0;i<16;i++) if ((s[i]^e0[i])!=tag[i]) return -1;

    uint8_t cb[16]; memcpy(cb,j0,16);
    for (size_t i=0;i<cipher_len;i+=16) {
        inc32(cb); uint8_t e[16]; tls_aes128_encrypt_block(rk,cb,e);
        size_t rem=(cipher_len-i<16)?cipher_len-i:16;
        for (size_t j=0;j<rem;j++) plain[i+j]=cipher[i+j]^e[j];
    }
    return 0;
}
