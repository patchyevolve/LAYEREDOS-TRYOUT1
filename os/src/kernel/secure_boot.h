#ifndef SECURE_BOOT_H
#define SECURE_BOOT_H

#include "types.h"

/* Initialize secure boot subsystem */
void secure_boot_init(void);

/* Check a binary against the known-good hash whitelist.
 * Returns ERR_OK if the binary is trusted, ERR_PERM if rejected.
 * If secure boot is disabled, always returns ERR_OK. */
int  secure_boot_check(const uint8_t* data, size_t len);

/* Enable or disable secure boot enforcement.
 * Returns ERR_PERM if trying to enable when not allowed (future: locked). */
int  secure_boot_set_enabled(int enabled);

/* Returns 1 if secure boot is currently enforcing, 0 otherwise. */
int  secure_boot_is_enabled(void);

#endif
