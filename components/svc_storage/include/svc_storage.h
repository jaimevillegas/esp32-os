#pragma once
// microSD service: SDMMC 4-bit host + FAT, mounted through the VFS at svc_storage_mount_point().
// There is no card-detect pin, so a low-priority task polls: it probes for a card while
// unmounted and checks the card status while mounted, unmounting cleanly on removal.
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Called from the storage task whenever the mounted state changes (true = card mounted and
// capacity known, false = card gone / unmounted). Must be quick and must not block.
typedef void (*svc_storage_cb_t)(bool mounted);

// Starts the polling task (idempotent). `cb` may be NULL.
void svc_storage_start(svc_storage_cb_t cb);

bool svc_storage_mounted(void);
const char *svc_storage_mount_point(void);

// Capacity of the mounted volume (cached when the card was mounted, no card I/O here).
// Returns false when nothing is mounted.
bool svc_storage_capacity(uint64_t *total_bytes, uint64_t *free_bytes);

#ifdef __cplusplus
}
#endif
