#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Boot sequence of the OS layer: NVS, settings, bus, services, UI shell, launcher.
void os_core_start(void);

#ifdef __cplusplus
}
#endif
