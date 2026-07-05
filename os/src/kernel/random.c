#include "random.h"
#include "sha256.h"
#include "hal.h"

/* Simple CSPRNG: SHA-256 in counter mode.
 * Seeded from RDTSC + interrupt timing jitter. */

static uint8_t  seed[SHA256_DIGEST_SIZE];
static uint64_t counter;
static int      initialized = 0;

static void mix_in_entropy(void) {
    /* Mix RDTSC into the seed */
    uint64_t tsc;
    asm volatile("rdtsc" : "=A"(tsc));
    for (int i = 0; i < 8; i++)
        seed[i] ^= (tsc >> (i * 8)) & 0xFF;
    /* Mix interrupt timing (HPET granularity) */
    uint64_t ns = hal_timer_get_ns();
    for (int i = 0; i < 8; i++)
        seed[i + 8] ^= (ns >> (i * 8)) & 0xFF;
}

void random_init(void) {
    /* Initialize seed from hardware sources */
    for (int i = 0; i < SHA256_DIGEST_SIZE; i++)
        seed[i] = 0;
    counter = 0;
    /* Stir in entropy multiple times */
    for (int i = 0; i < 8; i++) {
        mix_in_entropy();
        sha256(seed, SHA256_DIGEST_SIZE, seed);
    }
    initialized = 1;
}

void random_get_bytes(uint8_t* buf, size_t count) {
    if (!initialized) random_init();

    size_t offset = 0;
    while (offset < count) {
        /* Generate 32 bytes from SHA-256(seed || counter) */
        sha256_ctx_t ctx;
        sha256_init(&ctx);
        sha256_update(&ctx, seed, SHA256_DIGEST_SIZE);
        sha256_update(&ctx, (const uint8_t*)&counter, sizeof(counter));
        uint8_t block[SHA256_DIGEST_SIZE];
        sha256_final(&ctx, block);

        size_t to_copy = count - offset;
        if (to_copy > SHA256_DIGEST_SIZE)
            to_copy = SHA256_DIGEST_SIZE;
        for (size_t i = 0; i < to_copy; i++)
            buf[offset + i] = block[i];
        offset += to_copy;
        counter++;

        /* Re-seed periodically */
        if ((counter & 0xFF) == 0)
            mix_in_entropy();
    }
}
