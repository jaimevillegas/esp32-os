#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Logs free / minimum-ever / largest-block for internal SRAM and PSRAM, plus the
// stack high-water mark of the `ui` task. One line per metric, tagged with `phase`.
void os_diag_mem(const char *phase);

// Runs lv_demo_benchmark and logs one BENCH line per scene plus a summary.
// Call from the `screen_cb` passed to os_ui_start().
void os_diag_run_lvgl_benchmark(void);

#ifdef __cplusplus
}
#endif
