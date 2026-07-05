#include "kernel.h"
#include "secure_boot.h"
#include "sha256.h"

/* The auto-generated hash table */
#include "secure_boot_hashes.c"

static int secure_boot_enabled = 1;

void secure_boot_init(void) {
    secure_boot_enabled = 1;
    kprintf("[SECURE_BOOT] Secure boot enabled (%u known hashes)\n",
            (unsigned)SECURE_BOOT_NUM_HASHES);
}

int secure_boot_check(const uint8_t* data, size_t len) {
    if (!secure_boot_enabled)
        return ERR_OK;

    uint8_t hash[SHA256_DIGEST_SIZE];
    sha256(data, len, hash);

    for (size_t i = 0; i < SECURE_BOOT_NUM_HASHES; i++) {
        if (kmemcmp(hash, secure_boot_known_hashes[i].hash, SHA256_DIGEST_SIZE) == 0)
            return ERR_OK;
    }

    kprintf("[SECURE_BOOT] REJECTED: binary (SHA-256 %02x%02x...) not in whitelist\n",
            hash[0], hash[1]);
    return ERR_PERM;
}

int secure_boot_set_enabled(int enabled) {
    secure_boot_enabled = enabled ? 1 : 0;
    kprintf("[SECURE_BOOT] Enforcement %s\n",
            secure_boot_enabled ? "ENABLED" : "DISABLED");
    return ERR_OK;
}

int secure_boot_is_enabled(void) {
    return secure_boot_enabled;
}
