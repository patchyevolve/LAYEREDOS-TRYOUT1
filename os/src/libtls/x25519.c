#include "x25519.h"
#include <string.h>

#define MASK51 0x7ffffffffffffULL

typedef struct { uint64_t v[5]; } fe;

static void add(fe* r, const fe* a, const fe* b) {
    for (int i=0;i<5;i++) r->v[i]=a->v[i]+b->v[i];
}
static void sub(fe* r, const fe* a, const fe* b) {
    for (int i=0;i<5;i++) r->v[i]=a->v[i]-b->v[i];
}
static void cpy(fe* r, const fe* a) { memcpy(r,a,sizeof(fe)); }
static void sel(fe* r,const fe* a,const fe* b,int c) {
    uint64_t m=c?(uint64_t)-1:0;
    for (int i=0;i<5;i++) r->v[i]=(a->v[i]&~m)|(b->v[i]&m);
}

static void mul(fe* r, const fe* a, const fe* b) {
    uint64_t t[10]={0};
    for (int i=0;i<5;i++) for (int j=0;j<5;j++) t[i+j]+=a->v[i]*b->v[j];
    for (int i=9;i>=5;i--) { uint64_t c=t[i]>>51; t[i]&=MASK51; t[i-5]+=c*19; }
    for (int i=4;i>=0;i--) { if(i){t[i-1]+=t[i]>>51;t[i]&=MASK51;} else t[0]&=MASK51; }
    for (int i=0;i<4;i++) { t[i+1]+=t[i]>>51;t[i]&=MASK51; }
    t[0]+=(t[4]>>51)*19; t[4]&=MASK51;
    for (int i=0;i<5;i++) r->v[i]=t[i];
}

static void sq(fe* r, const fe* a) { mul(r,a,a); }

static void inv(fe* r, const fe* a) {
    fe t[16]; cpy(&t[0],a);
    for (int i=1;i<16;i++) { sq(&t[i],&t[i-1]); mul(&t[i],&t[i],a); }
    fe u; cpy(&u,&t[15]);
    for (int i=250;i>=0;i--) {
        sq(&u,&u);
        if (i<245||i==240||i==235||(i%5==0&&i<250)) {
            int idx=i%5+(i<5?0:10);
            mul(&u,&u,&t[idx]);
        }
    }
    cpy(r,&u);
}

static void pack(uint8_t out[32], const fe* a) {
    fe x; cpy(&x,a);
    uint64_t c;
    c=x.v[0]>>51; x.v[0]&=MASK51; x.v[1]+=c;
    c=x.v[1]>>51; x.v[1]&=MASK51; x.v[2]+=c;
    c=x.v[2]>>51; x.v[2]&=MASK51; x.v[3]+=c;
    c=x.v[3]>>51; x.v[3]&=MASK51; x.v[4]+=c;
    c=x.v[4]>>51; x.v[4]&=MASK51; x.v[0]+=c*19;

    uint64_t t[5]; memcpy(t,x.v,40);
    for (int i=0;i<4;i++){t[i+1]+=t[i]>>51;t[i]&=MASK51;}
    t[0]+=(t[4]>>51)*19; t[4]&=MASK51;
    memset(out,0,32);
    for (int i=0;i<5;i++)
        for (int j=0;j<8;j++)
            if (i*8+j<32) out[i*8+j]=(uint8_t)(t[i]>>(j*8));
    out[31]&=127;
}

static void unpack(fe* r, const uint8_t in[32]) {
    memset(r,0,sizeof(fe));
    for (int i=0;i<32;i++) r->v[i/8]|=((uint64_t)in[i])<<((i%8)*8);
    r->v[4]&=MASK51;
}

void tls_x25519_keypair(uint8_t pub[32], uint8_t priv[32]) {
    long ret;
    asm volatile("int $0x80":"=a"(ret):"a"(63),"D"((long)priv),"S"(32),"d"(0):"memory");
    priv[0]&=248; priv[31]&=127; priv[31]|=64;
    uint8_t clamped[32]; memcpy(clamped,priv,32);
    clamped[0]&=248; clamped[31]&=127; clamped[31]|=64;

    fe x1; unpack(&x1,(const uint8_t*)"\x09\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0");
    fe x2; cpy(&x2,&((fe){{1,0,0,0,0}}));
    fe z2; memset(&z2,0,sizeof(fe));
    fe x3; unpack(&x3,(const uint8_t*)"\x09\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0");
    fe z3; cpy(&z3,&((fe){{1,0,0,0,0}}));
    int swap=0;

    for (int i=254;i>=0;i--) {
        int k=(clamped[i/8]>>(i%8))&1;
        swap^=k;
        fe t0,t1;
        sel(&t0,&x2,&x3,swap); sel(&t1,&z2,&z3,swap);
        cpy(&x2,&t0); cpy(&z2,&t1);
        cpy(&x3,&t0); cpy(&z3,&t1);
        swap=k;

        fe A,B,C,D,E,DA,CB,AA,BB,a24;
        add(&A,&x2,&z2); sub(&B,&x2,&z2);
        add(&C,&x3,&z3); sub(&D,&x3,&z3);
        sq(&AA,&A); sq(&BB,&B);
        mul(&DA,&D,&A); mul(&CB,&C,&B);
        add(&t0,&DA,&CB); sq(&x3,&t0);
        sub(&t1,&DA,&CB); sq(&z3,&t1); mul(&z3,&z3,&x1);
        mul(&x2,&AA,&BB);
        sub(&E,&AA,&BB);
        memset(&a24,0,sizeof(fe)); a24.v[0]=121666;
        mul(&t0,&E,&a24); add(&t1,&AA,&t0);
        mul(&z2,&E,&t1);
    }

    fe t0,t1;
    sel(&t0,&x2,&x3,swap); sel(&t1,&z2,&z3,swap);
    cpy(&x2,&t0); cpy(&z2,&t1);
    inv(&z2,&z2);
    mul(&x2,&x2,&z2);
    pack(pub,&x2);
}

int tls_x25519_shared(uint8_t shared[32], const uint8_t priv[32], const uint8_t pub[32]) {
    uint8_t clamped[32]; memcpy(clamped,priv,32);
    clamped[0]&=248; clamped[31]&=127; clamped[31]|=64;

    fe x1; unpack(&x1,pub);
    fe x2; cpy(&x2,&((fe){{1,0,0,0,0}}));
    fe z2; memset(&z2,0,sizeof(fe));
    fe x3; cpy(&x3,&x1);
    fe z3; cpy(&z3,&((fe){{1,0,0,0,0}}));
    int swap=0;

    for (int i=254;i>=0;i--) {
        int k=(clamped[i/8]>>(i%8))&1;
        swap^=k;
        fe t0,t1;
        sel(&t0,&x2,&x3,swap); sel(&t1,&z2,&z3,swap);
        cpy(&x2,&t0); cpy(&z2,&t1);
        cpy(&x3,&t0); cpy(&z3,&t1);
        swap=k;

        fe A,B,C,D,E,DA,CB,AA,BB,a24;
        add(&A,&x2,&z2); sub(&B,&x2,&z2);
        add(&C,&x3,&z3); sub(&D,&x3,&z3);
        sq(&AA,&A); sq(&BB,&B);
        mul(&DA,&D,&A); mul(&CB,&C,&B);
        add(&t0,&DA,&CB); sq(&x3,&t0);
        sub(&t1,&DA,&CB); sq(&z3,&t1); mul(&z3,&z3,&x1);
        mul(&x2,&AA,&BB);
        sub(&E,&AA,&BB);
        memset(&a24,0,sizeof(fe)); a24.v[0]=121666;
        mul(&t0,&E,&a24); add(&t1,&AA,&t0);
        mul(&z2,&E,&t1);
    }

    fe t0,t1;
    sel(&t0,&x2,&x3,swap); sel(&t1,&z2,&z3,swap);
    cpy(&x2,&t0); cpy(&z2,&t1);
    inv(&z2,&z2);
    mul(&x2,&x2,&z2);
    pack(shared,&x2);

    for (int i=0;i<32;i++) if (shared[i]) return 0;
    return -1;
}
