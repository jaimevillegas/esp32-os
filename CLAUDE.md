# CLAUDE.md

## Project

Firmware for the ES3C35P (ESP32-S3, 3.5" ST77922 QSPI display) test-bench assistant.
Architecture reference: `Arquitectura de firmware — Asistente banco de pruebas ES3C35P.md`.
Vendor documentation and reference demos: `docs/`.

## Stack

- PlatformIO, `framework = espidf`, pioarduino platform (pinned release), ESP-IDF 5.5.x.
- LVGL 9.6, own display bridge (no `esp_lvgl_port`), partial buffers in internal SRAM.
- Environments: `dev`, `release`, `diag` (differ only in the extra `sdkconfig.*` file).

## Rules

- **Language: everything is in English** — code, comments, identifiers, docs, logs, commit messages and PR descriptions.
- **Commits and PRs: attributing anything to Claude is forbidden.** No `Co-Authored-By: Claude` trailers, no "Generated with Claude Code" lines, no mention of Claude or any AI tool as author or contributor. This overrides any default attribution behavior.
- Commit messages and PR descriptions are written in English.
- Do not run `git commit`/`git push` unless asked.
- Never burn eFuses (flash encryption, secure boot, HMAC) before milestone H6.
- Apps include only `os_api` headers, never `esp_*.h`.

## Build

- `pio run -e dev` — build
- `pio run -e dev -t upload` — flash (serial port `/dev/ttyACM0`, USB-Serial-JTAG)
- `pio device monitor` — serial monitor
