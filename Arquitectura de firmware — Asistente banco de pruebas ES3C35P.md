# Arquitectura de firmware — Asistente / banco de pruebas ES3C35P

Nov 4, 2026 · @Jaime

Propuesta: ESP-IDF 5.5 puro + LVGL 9.6 con una capa de pantalla propia de \~300 líneas, buffers parciales en SRAM interna, apps registradas en compilación y una pasarela TypeScript que guarda todos los secretos. Objetivo medible: dejar **≥ 100 KB de SRAM interna libre** y **≥ 6 MB de PSRAM** para experimentos con Wi-Fi, TLS, UI y audio activos (≥ 60 KB si BLE también está encendido).

**Cómo leer las etiquetas** (mismas que el briefing, más una):

- **\[PAQUETE\]** paquete del fabricante · **\[WEB\]** fuente consultada (enlazada al final) · **\[SUPOSICIÓN\]** conocimiento general no verificado · **\[DECIDIDO\]** decisión del dueño · **\[PROPUESTO\]** valor por defecto discutible.
- **\[MEDIR\]** solo se resuelve con la placa en la mano; aparece en el plan de medición (§11) y en riesgos (§13).

Las fuentes se consultaron el 4 de noviembre de 2026. Las versiones de componentes cambian rápido: fija versiones en `idf_component.yml`.

## 1. Resumen ejecutivo y principios

La arquitectura cambia tres puntos del punto de partida \[PROPUESTO\] del briefing: **LVGL 9.6 en vez de 8.4**, **capa de pantalla propia en vez de `esp_lvgl_port`**, y **ningún token de Claude en la placa ni en la pasarela**. Ninguna decisión de la sección 2 del briefing se contradice.

### Decisiones clave

| Tema | Decisión | Por qué (evidencia) |
| --- | --- | --- |
| Framework | `framework = espidf` sobre la plataforma **pioarduino**, ESP-IDF 5.5.x fijado | La plataforma oficial de PlatformIO se quedó en IDF 5.4; pioarduino ya empaqueta IDF 5.5.x \[WEB\] |
| UI | **LVGL 9.6.0** (16 sep 2026) | Última de la serie 9, release de estabilidad con rutas de mezcla por software reescritas y más rápidas \[WEB\]. La serie 8 ya no aparece en la tabla de soporte activo \[WEB\] |
| Driver de panel | `espressif/esp_lcd_st77922` **2.1.1** | Desde 2.1.0, `swap_xy()` devuelve `ESP_ERR_NOT_SUPPORTED` salvo opt-in `supports_madctl_mv` \[WEB\] |
| Puente LVGL ↔ panel | Propio, \~300 líneas | `sw_rotate` de `esp_lvgl_port` solo existe con LVGL 8 \[WEB\]; `esp_lvgl_adapter` en QSPI sin TE solo admite modo `NONE` y delega la rotación al panel \[WEB\]. Además hay que redondear X a múltiplos de 4 \[WEB\] |
| Rotación | Plan A: MADCTL.MV validado en hardware (coste cero). Plan B: rotar **durante** la copia al buffer DMA (coste ≈ 1 `memcpy`) | Ver §5 |
| Buffers | Doble buffer parcial en SRAM interna apta para DMA, franjas de 32 filas (≈ 30 KB c/u) | El driver SPI de IDF **copia** a SRAM los datos que están en PSRAM \[WEB\]: un lienzo en PSRAM no ahorra SRAM, solo añade una copia |
| Apps | Registro en compilación (Kconfig + X-macro), arena de PSRAM por app | Sin trucos de linker; fugas imposibles por construcción |
| Datos de Claude | Script `statusLine` de Claude Code → pasarela → placa | El campo oficial `rate_limits` trae ventanas 5 h/7 d \[WEB\]. Usar tokens OAuth de consumo en herramientas de terceros viola los términos desde feb 2026 \[WEB\] |

### Principios y compensaciones elegidas

1. **La SRAM interna es el recurso escaso; la PSRAM no.** Todo lo que no necesite DMA, ISR o latencia baja va a PSRAM (`heap_caps`). Se acepta algo menos de velocidad en objetos LVGL a cambio de SRAM para experimentos.
2. **Dormir por defecto.** UI dirigida por eventos (la tarea LVGL bloquea hasta el próximo temporizador o un toque), radio en modem-sleep, sueño ligero automático con pantalla apagada.
3. **Un dueño por recurso.** Una tarea para LVGL, un bus I2C con dispositivos registrados, una conexión TLS a la vez, un solo cliente de red.
4. **Cliente delgado.** La placa nunca habla con APIs de terceros. Una petición agregada a la pasarela, con caché `ETag`.
5. **Apps desechables.** La UI de una app existe solo en primer plano; su memoria vive en una arena que se libera entera al cerrarla.
6. **Medir antes de optimizar.** Cada hito tiene un umbral numérico (§11–12). Si un número contradice este documento, gana el número.

**Compensación más importante:** se renuncia a la comodidad de `esp_lvgl_port` (y a los ejemplos del fabricante) para controlar rotación, redondeo y buffers. Coste: escribir y mantener el puente de pantalla y el driver táctil.

## 2. Capas y flujo de datos

&#91;embedded content: capas del firmware y flujos externos · 6 capas, 3 sistemas externos\]

La placa tiene dos conexiones: HTTPS con la pasarela, que concentra Gmail, Claude y la OTA, y BLE HID con el portátil para las teclas multimedia.

Flujo interno de un dato nuevo:

1. `net` recibe el snapshot (o un `304`) y lo escribe en el store con versión N.
2. El bus publica `Snapshot v=N` a `ui` y a `svc`.
3. `svc` entrega el evento al `Service` de cada app interesada (p. ej. notificación de correo).
4. `ui` despierta, lee del store solo los campos de la app en primer plano y actualiza sus widgets; LVGL redibuja solo las zonas sucias.

## 3. Mapa de memoria

Con todos los servicios activos, la estimación deja **60–130 KB de SRAM interna libre**; llegar a ≥ 100 KB exige mover Wi-Fi/lwIP y TLS a PSRAM y cargar BLE solo cuando se use. Todas las cifras son \[SUPOSICIÓN\] salvo las marcadas; el hito H2 las sustituye por medidas.

### SRAM interna (512 KB)

| Consumidor | Estimado (KB) | Dónde / cómo reducirlo |
| --- | --- | --- |
| Caché de flash/PSRAM (I 32 + D 64) | 96 | Sale de la SRAM \[SUPOSICIÓN\]. Con I 16 + D 32 se ganan 48 KB a costa de rendimiento de PSRAM. Espressif recomienda 32/64 para LVGL \[WEB\] → **\[MEDIR\]** ambas |
| IDF + FreeRTOS estático (IRAM, .data, .bss) | 90–130 | `pio run -t size` / `idf.py size-components`. `CONFIG_ESP_COREDUMP_LOGS=n` ahorra \~5 KB de DRAM \[WEB\] |
| Wi-Fi (buffers estáticos + dinámicos) | 40–60 | `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y` \[SUPOSICIÓN\], menos buffers RX/TX ([guía Wi-Fi buffer usage](https://docs.espressif.com/projects/esp-idf/en/v4.4/esp32s3/api-guides/performance/ram-usage.html)) |
| lwIP + 2 sockets | 10–20 | Idem |
| mbedTLS, pico de un handshake | 20–40 (transitorio) | `CONFIG_MBEDTLS_DYNAMIC_BUFFER` \[WEB\] + `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC` \[SUPOSICIÓN\] |
| NimBLE host + controlador | 30–50 | Solo si BLE está activo; init/deinit bajo demanda |
| Buffers de dibujo LVGL | **60** | 2 × 480 × 32 × 2 B = 61 440 B, `MALLOC_CAP_DMA \| MALLOC_CAP_INTERNAL` |
| I2S DMA (audio) | 6–10 | Solo con audio activo |
| Pilas propias en SRAM | \~30 | Ver §4; las de experimentos van a PSRAM |
| **Margen fijo (no se asigna)** | **≥ 40** | Bloque libre mayor ≥ 32 KB |

Regla: si un buffer de color está en PSRAM, el driver `spi_master` reserva un temporal en SRAM y hace `memcpy` en cada transferencia; su tamaño ≈ `max_transfer_sz × trans_queue_depth` y compite con Wi-Fi/BLE \[WEB\]. Por eso los buffers de dibujo van en SRAM desde el principio.

### PSRAM (8 MB, octal a 80 MHz)

| Uso | Presupuesto |
| --- | --- |
| Heap de LVGL (objetos, estilos, textos) | 256 KB – 1 MB |
| Caché de imágenes decodificadas | 512 KB |
| Red desplazada (Wi-Fi/lwIP, TLS, JSON) | \~150 KB |
| Audio: decodificador MP3 (\~28 KB medido en S3 \[WEB\]) + anillo PCM + lectura SD | \~128 KB |
| Sistema: store, anillo de logs, eventos | \~256 KB |
| **Arenas de apps y experimentos** | **≥ 6 MB** |

`CONFIG_SPIRAM_XIP_FROM_PSRAM` (recomendado por Espressif para LVGL \[WEB\]) copia el código a PSRAM y evita bloqueos de caché durante escrituras en flash, pero consume \~el tamaño del binario (2–3 MB). Arrancar **sin** XIP y activarlo solo si las escrituras NVS/OTA congelan la UI.

### Flash (16 MB)

Dos ranuras OTA de 5 MB, volcado de fallos, dos NVS y \~5,6 MB de recursos mapeables (detalle en §9).

### Cómo medir

- `heap_caps_get_free_size()`, `heap_caps_get_largest_free_block()` y `heap_caps_get_minimum_free_size()` con `MALLOC_CAP_INTERNAL` y `MALLOC_CAP_SPIRAM`, registrados en cada hito (arranque, Wi-Fi arriba, pico TLS, BLE activo, audio sonando).
- `uxTaskGetStackHighWaterMark()` por tarea y `uxTaskGetSystemState()` para el resumen \[WEB\].
- El bloque libre mayor importa tanto como el total: es lo que delata la fragmentación \[WEB\].

## 4. Modelo de tareas

Seis tareas propias (más las de IDF) y un patrón de actores: cada tarea tiene **una** cola de entrada y nunca comparte objetos LVGL. La mejor pila es la que no existe: los servicios de las apps corren cooperativamente en una sola tarea `svc` en vez de una tarea por app \[WEB\].

### Tareas

| Tarea | Núcleo | Prioridad | Pila | Memoria de pila | Función |
| --- | --- | --- | --- | --- | --- |
| `ui` (LVGL) | 1 | 4 | 8 KB | SRAM | `lv_timer_handler()`; bloquea con `ulTaskNotifyTake(next_ms)`. La ISR del SPI se registra también en el núcleo 1 (`isr_cpu_id`) para evitar la carrera de `spi_bus_lock` \[WEB\] |
| `svc` | 0 | 5 | 6 KB | SRAM | Bus de eventos + servicios de apps (`onEvent`, `onTick`) |
| `net` | 0 | 6 | 7 KB | SRAM | Único cliente HTTP/TLS, SNTP, OTA |
| `audio` | 0 | 8 | 6 KB | SRAM | Decodificación + escritura I2S; solo existe con audio activo |
| `diag` | 0 | 1 | 3 KB | PSRAM | Estadísticas de heap/CPU cada 5 s; solo en build `diag` |
| `exp_*` | sin afinidad | 2–3 | según app | **PSRAM** (`xTaskCreateWithCaps`) \[WEB\] | Experimentos; suscritos al watchdog |
| IDF: `wifi` | 0 | 23 | — | — | Fijo por IDF |
| IDF: `tiT` (lwIP) | 0 | 18 | ajustar | — | `CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0` \[SUPOSICIÓN\] |
| IDF: `nimble_host` | 0 | 21 | 4 KB | — | Solo con BLE activo |
| IDF: `esp_timer`, `sys_evt`, `ipc0/1`, `IDLE0/1` | — | — | por defecto | — | Medir y recortar con margen |

Prioridades \[PROPUESTO\]: audio > net > svc > ui > experimentos. La UI está sola en el núcleo 1, así que su prioridad baja solo la compite contra experimentos sin afinidad. Una pila en PSRAM no puede ejecutarse con la caché desactivada (escrituras de flash): por eso las tareas del sistema mantienen pila interna \[SUPOSICIÓN\].

**Segundo hilo de render (`CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=2`).** Espressif lo recomienda en chips de dos núcleos \[WEB\], pero pondría trabajo de UI en el núcleo 0 junto a Wi-Fi. Se deja en 1 y se prueba en el benchmark (§11).

### Comunicación

- **Bus de eventos propio** (no `esp_event`): `bus.publish(topic, msg)` copia un mensaje de 16 B a la cola de cada suscriptor. Tópicos como bits de un `uint32_t`.
- **Store versionado**: los datos grandes (snapshot de la pasarela, estado de batería, estado de red) viven en el store, protegidos por un mutex corto. El mensaje solo dice `topic X cambió, versión N`; el lector copia lo que necesita.
- **UI:** las otras tareas nunca llaman a LVGL. Publican al tópico; la tarea `ui` despierta por notificación, lee el store y actualiza widgets.
- **Toque:** la ISR de INT (IO47) despierta a `ui`; la lectura I2C ocurre dentro del callback de lectura del indev. Sin sondeo.

```cpp
struct Msg {            // 16 bytes, se copia por valor
  uint16_t topic;       // Topic::Snapshot, Topic::Battery, ...
  uint16_t app;         // destino o 0 = broadcast
  uint32_t version;     // versión del dato en el store
  uint32_t arg;
  uint32_t ts_ms;
};
```

Colas: `ui_q` 16, `svc_q` 16, `net_q` 8, `audio_q` 8 mensajes. PCM entre decodificador e I2S: `StreamBuffer` de 64 KB en PSRAM.

## 5. Pipeline de pantalla

LVGL 9.6 en modo `PARTIAL` dibuja franjas de 480 × 32 px en SRAM interna; la rotación a horizontal la hace el panel (plan A) o la copia al buffer DMA (plan B). Ambos planes ocupan los mismos 60 KB de SRAM y ninguno toca PSRAM.

### Bus y panel

- `spi_bus_initialize(SPI2_HOST)` con `ST77922_PANEL_BUS_QSPI_CONFIG(...)` y `max_transfer_sz = 480 × 32 × 2` (30 720 B) \[WEB\].
- `esp_lcd_panel_io_spi_config_t`: `pclk_hz` empieza en 40 MHz y sube a 80 MHz \[PAQUETE\] solo tras la prueba térmica; `trans_queue_depth = 2–4` \[WEB\].
- Reset: el panel comparte EN con el ESP32 \[PAQUETE\] → `reset_gpio_num = -1` y reset por comando.
- **Restricción del ST77922:** `x_start` múltiplo de 4 y `x_end` = 4N+3 en cada `draw_bitmap` \[WEB\]. En LVGL 9 se aplica en el evento `LV_EVENT_INVALIDATE_AREA` (reemplaza al `rounder_cb` de la v8) \[SUPOSICIÓN: verificar en 9.6\].
- Techo teórico: 30 720 B / 40 MB/s ≈ **0,77 ms por franja** y ≈ 8 ms por pantalla completa a 80 MHz. El cuello de botella será el render, como anticipaba el briefing.

### Rotación a 480 × 320

**Plan A — MADCTL.MV (coste cero).** El componente 2.1.x no intercambia ejes por defecto: el datasheet describe el bit MV de forma inconsistente y el comportamiento depende del módulo y de la secuencia de init \[WEB\]. Se habilita con `supports_madctl_mv = 1` **solo después** de comprobar visualmente la orientación con la secuencia de init exacta del fabricante \[WEB\]. Si funciona, LVGL se crea en 480 × 320, sin rotación, y la restricción de múltiplos de 4 se aplica a ambos ejes hasta confirmar cuál la sufre **\[MEDIR\]**.

**Plan B — rotar al copiar.** LVGL se crea con la resolución nativa y `lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90)`; el `flush_cb` gira la franja con `lv_draw_sw_rotate()` o una función propia \[WEB\]. La copia es inevitable y se aprovecha: girar y hacer el swap de bytes RGB565 en **una sola pasada** sobre 30 KB. Como la columna física del panel equivale a la fila lógica, el redondeo a 4 se aplica a `y1`/`y2` lógicos; 320 es múltiplo de 4, así que el espejo conserva el alineado.

```cpp
// Plan B: 1 buffer de dibujo + 1 buffer de transmisión, ambos 30 KB internos aptos para DMA
static void flush_cb(lv_display_t* d, const lv_area_t* a, uint8_t* px) {
  xSemaphoreTake(tx_free, portMAX_DELAY);   // espera al DMA anterior (≈0,8 ms)
  lv_area_t r = *a;
  lv_display_rotate_area(d, &r);
  rotate90_swap565(px, tx_buf, lv_area_get_width(a), lv_area_get_height(a));
  lv_display_flush_ready(d);                // LVGL ya pinta la franja siguiente
  esp_lcd_panel_draw_bitmap(panel, r.x1, r.y1, r.x2 + 1, r.y2 + 1, tx_buf);
}
// on_color_trans_done (ISR): xSemaphoreGiveFromISR(tx_free, &woken);
```

En plan A el `flush_cb` solo hace el swap de bytes (`lv_draw_sw_rgb565_swap()` \[WEB\]) y envía el buffer; `lv_display_flush_ready()` se llama desde el callback de fin de DMA, con doble buffer. Si LVGL 9.6 permite renderizar directamente en RGB565 con bytes intercambiados, esa pasada desaparece \[SUPOSICIÓN\].

### Comparación con el ejemplo del fabricante

| Estrategia | SRAM | PSRAM | Copias por píxel |
| --- | --- | --- | --- |
| Fabricante: lienzo completo PSRAM + `trans_size` 1/10 + `sw_rotate` | \~30 KB + temporales del driver | 300 KB | render → rotar → copiar a SRAM → DMA |
| **Plan A** (MV) | 60 KB | 0 | render → swap → DMA |
| **Plan B** (rotar al copiar) | 60 KB | 0 | render → (rotar + swap) → DMA |

### Tearing y refresco

- Sin línea TE confirmada \[SUPOSICIÓN del briefing\]; `esp_lvgl_adapter` solo ofrece anti-tearing en QSPI con TE cableado a un GPIO \[WEB\]. Mitigación: transiciones de pantalla instantáneas o fundidos cortos, nada de desplazamientos a pantalla completa. **\[MEDIR\]** buscar TE en el esquemático y el FPC.
- `LV_DEF_REFR_PERIOD = 16 ms` mientras hay animación; en reposo la tarea `ui` duerme hasta el próximo temporizador (dirigido por eventos).

### Táctil

- Driver propio en C sobre `i2c_master`, portado de `ST77922_TOUCH` del fabricante (0x55, INT activo bajo en IO47, RST en IO48) \[PAQUETE\]. Se usa 1 punto (2 para pellizco); los gestos los detecta LVGL.
- La transformación de coordenadas es independiente del plan A/B: tres flags (`swap_xy`, `mirror_x`, `mirror_y`) calibrados con una pantalla de 4 esquinas.
- Lectura solo tras INT; el indev de LVGL 9 en modo evento (`LV_INDEV_MODE_EVENT`) evita el sondeo cada 30 ms \[SUPOSICIÓN: verificar API en 9.6\].

### Retroiluminación y reposo del panel

- LEDC en IO41 a \~20 kHz (fuera del rango audible), 10 bits, curva gamma para el brillo \[SUPOSICIÓN\].
- Pantalla apagada: brillo 0 + `esp_lcd_panel_disp_sleep()`. **Riesgo:** el ST77922 es TDDI; si el táctil se duerme con el display, el despertar pasa al botón BOOT (IO0) **\[MEDIR\]**.

## 6. Marco de apps

Cada app es una clase C++ con manifiesto estático, una parte UI que solo existe en primer plano y una parte de servicio opcional. Las apps solo incluyen cabeceras `os/` (nunca de ESP-IDF), lo que permite compilarlas igual en el simulador.

### Interfaz base

```cpp
namespace os {

enum Perm : uint32_t { NET = 1, AUDIO = 2, SD = 4, BLE = 8, NVS = 16, LONG_TASK = 32 };

struct Manifest {
  const char*  id;            // "claude.usage" (también namespace NVS)
  const char*  name;          // texto del launcher
  const void*  icon;          // lv_image_dsc_t en flash
  uint32_t     perms;         // máscara de Perm
  uint32_t     psram_arena;   // bytes reservados al crear
  uint32_t     sram_budget;   // tope de SRAM interna (0 = ninguna)
  bool         background;    // tiene Service
  App*       (*make)(std::pmr::memory_resource&);
};

class Context {                 // inyectado; servicios nulos si falta el permiso
public:
  Store&                     store();
  Bus&                       bus();
  Net*                       net();
  Audio*                     audio();
  Fs*                        sd();
  Kv&                        kv();      // NVS namespace propio
  std::pmr::memory_resource& mem();     // arena PSRAM de la app
};

class App {
public:
  virtual ~App() = default;
  virtual void onCreate(Context&) {}           // una vez; reserva datos
  virtual void onStart(lv_obj_t* root) = 0;    // crea UI bajo root (tarea ui)
  virtual void onResume() {}                   // visible e interactiva
  virtual void onEvent(const Msg&) {}          // en tarea ui, solo en primer plano
  virtual void onPause() {}                    // pierde foco (diálogo, notificación)
  virtual void onStop() {}                     // el sistema borra root después
  virtual void onDestroy() {}                  // libera todo; la arena se descarta
};

class Service {                                // parte en segundo plano, tarea svc
public:
  virtual ~Service() = default;
  virtual void     onEvent(const Msg&) = 0;
  virtual uint32_t onTick(uint32_t now_ms) { return UINT32_MAX; }  // ms hasta el próximo tick
};

}  // namespace os
```

### Ciclo de vida y memoria

1. **Abrir:** el gestor crea la arena (`heap_caps_malloc(psram_arena, MALLOC_CAP_SPIRAM)` + `multi_heap_register()` \[SUPOSICIÓN: API de `multi_heap.h`\]), construye la app dentro con `std::pmr`, llama `onCreate` → `onStart(root)` → `onResume`.
2. **Ir al fondo:** `onPause` → `onStop`; el gestor borra `root` con todos sus objetos LVGL. Solo sobrevive el `Service` y los datos en la arena o en el store.
3. **Cerrar:** `onDestroy`, luego se descarta la arena **entera**. Las fugas de la app desaparecen por construcción.
4. **Verificación:** el gestor compara `lv_mem_monitor()` y el heap interno antes y después; una diferencia > 1 KB se registra como fuga de la app (no de su arena).

### Registro en compilación

Cada app tiene su `Kconfig` (`CONFIG_OS_APP_CLAUDE=y`) y una línea en `apps.def`. Sin secciones de linker ni constructores globales:

```cpp
// components/apps/apps.def
#if CONFIG_OS_APP_CLAUDE
OS_APP(claude, apps::claude::kManifest)
#endif
#if CONFIG_OS_APP_DIAG
OS_APP(diag, apps::diag::kManifest)
#endif

// os_core/registry.cpp
#define OS_APP(name, manifest) &manifest,
constexpr const Manifest* kApps[] = {
#include "apps.def"
};
```

### Permisos y aislamiento

El ESP32-S3 no tiene MMU para separar apps; solo protección de buses IRAM/DRAM del sistema \[WEB\]. Lo que sí se puede hacer:

- **Permisos por inyección:** `ctx.net()` devuelve `nullptr` sin `Perm::NET`. Es una barrera contra errores, no contra código malicioso.
- **Watchdog:** cada `exp_*` se suscribe al TWDT; la tarea `svc` mide el tiempo de cada `onEvent`/`onTick` y avisa si pasa de 20 ms.
- **Pilas:** `CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK` para que el desbordamiento sea pánico inmediato \[WEB\].
- **Modo seguro:** el gestor escribe el `id` de la app en primer plano en memoria `RTC_NOINIT`. Tras un pánico o WDT, el arranque deshabilita esa app y muestra el informe; tres fallos seguidos activan el modo seguro (§8).
- **Topes:** una app no puede crecer más allá de su arena; `sram_budget` se comprueba con un envoltorio de `heap_caps_malloc` interno en la build `diag`.

### Apps de base

| App | Fondo | Permisos | Uso |
| --- | --- | --- | --- |
| Launcher | — | — | Rejilla de iconos, barra de estado (hora, Wi-Fi, batería) |
| Claude | Sí | — (lee el store) | Ventanas 5 h / 7 d, cuenta atrás local, antigüedad del dato |
| Correo | Sí | — | No leídos + último remitente |
| Multimedia | Sí | BLE, AUDIO | Teclas HID; reproductor de la SD |
| Ajustes | — | NVS | Brillo, tiempos de reposo, Wi-Fi |
| Diagnóstico | — | — | Heap, pilas, CPU, FPS, registros (§11) |

La carga dinámica de ELF desde la SD (el modelo de Tactility) queda para después de H7: añade un cargador, un SDK estable y una ABI que hoy frenarían todo lo demás.

## 7. Red y pasarela

La placa hace **una** petición HTTPS a la pasarela por ciclo y solo guarda un token de dispositivo revocable. Los secretos de Gmail viven en la pasarela; los datos de Claude los empuja tu portátil.

### Contradicción explícita con el briefing: datos de Claude

El briefing toma como referencia **Claude Usage Stick**, que guarda un token OAuth de `claude setup-token` en la placa y consulta la API de mensajes para leer las cabeceras `anthropic-ratelimit-unified-*` \[WEB\]. Desde el 19 de febrero de 2026, la documentación de Anthropic aclara que los tokens OAuth de planes Free/Pro/Max no pueden usarse en herramientas de terceros \[WEB\]. Ese diseño arriesga la cuenta, aunque el token se cifre. Moverlo a la pasarela no cambia nada.

**Alternativa propuesta:** Claude Code entrega a los scripts de `statusLine` un JSON con `rate_limits.five_hour` y `rate_limits.seven_day` (`used_percentage`, `resets_at`) desde la v2.1.80 \[WEB\]. Un script en el portátil reenvía ese bloque a la pasarela:

```bash
#!/usr/bin/env bash
# ~/.claude/statusline.sh — imprime la línea y reenvía rate_limits sin bloquear
in=$(cat)
rl=$(jq -c '.rate_limits // empty' <<<"$in")
[ -n "$rl" ] && curl -s -m 2 -X POST https://gw.lan:8443/v1/ingest/claude \
  -H "Authorization: Bearer $GW_INGEST_TOKEN" -H 'Content-Type: application/json' \
  -d "$rl" >/dev/null &
jq -r '"\(.model.display_name) · 5h \(.rate_limits.five_hour.used_percentage // "–")%"' <<<"$in"
```

El script se ejecuta en cada refresco de la barra: en la versión final debe enviar solo si rate\_limits cambió respecto a una copia en /tmp. Límites: solo se actualiza mientras usas Claude Code, y hay reportes de versiones en que el campo faltaba \[WEB\]. La placa muestra el último valor, su antigüedad y la cuenta atrás hasta `resets_at`, calculada localmente.

### Protocolo placa ↔ pasarela

| Aspecto | Decisión |
| --- | --- |
| Transporte | HTTPS sobre la LAN, keep-alive, certificado ECDSA P-256 de una CA propia embebida en el firmware |
| Endpoint | `GET /v1/snapshot` con `If-None-Match: "<versión>"` → `304` sin cuerpo si nada cambió |
| Formato | JSON compacto versionado (≤ 2 KB). CBOR solo si la medición muestra que el parseo importa: el coste real está en el handshake, no en el formato |
| Frecuencia | Pantalla encendida: *long-poll* (la pasarela retiene la petición hasta 55 s y responde al primer cambio). Pantalla apagada: un sondeo cada 5 min y cierre de conexión |
| Autenticación | `Authorization: Bearer <token de 32 B>` guardado en NVS cifrado; revocable en la pasarela |
| Sin conexión | El store conserva el último snapshot; cada bloque lleva `src_ts` y la UI lo marca como desactualizado tras 2 intervalos |
| Reconexión | Backoff 1, 2, 4… 300 s con jitter ±20 %; al encender la pantalla se reintenta ya |
| Hora | SNTP al conectar y cada 6 h (no hay RTC con pila) |

```json
{"v":1287,"ts":1793836800,
 "claude":{"h5":{"pct":42,"reset":1793840400},"d7":{"pct":15,"reset":1794268800},"src_ts":1793836700},
 "mail":{"unread":3,"top":{"from":"…","subj":"…"},"src_ts":1793836790},
 "stale":[]}
```

### TLS con poca SRAM

- `esp_http_client` + `esp-tls`, una sola conexión a la vez, en la tarea `net`.
- `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y` \[WEB\], reserva de mbedTLS en PSRAM, reanudación con session tickets para que el handshake completo solo ocurra tras reiniciar \[SUPOSICIÓN: medir ms y KB de ambos casos\].
- Solo un conjunto de cifrado (ECDHE-ECDSA-AES128-GCM) y la CA propia: menos código y verificación más barata que RSA.

### Pasarela (Node/TypeScript)

- Servidor Fastify en un equipo siempre encendido de la LAN; cada fuente es un plugin con su propio horario y caché.
- **Correo:** API de Gmail con alcance de solo lectura (o IMAP IDLE) → número de no leídos y último remitente. El refresh token vive solo aquí.
- **Claude:** endpoint `/v1/ingest/claude` alimentado por el script anterior.
- Cada cambio incrementa la versión del snapshot y despierta los long-polls pendientes.
- También sirve los binarios OTA firmados (§8) y recibe volcados de fallos.

### Multimedia

No pasa por la pasarela: la placa es un periférico **BLE HID** de *consumer control* (play/pausa, siguiente, volumen) emparejado con el portátil. ESP-IDF soporta Wi-Fi y BLE simultáneos en el S3 con NimBLE \[WEB\]; el host BLE se inicia al abrir la app Multimedia o al emparejar, y se apaga tras 10 min sin uso para recuperar SRAM \[SUPOSICIÓN: medir la memoria que devuelve el deinit\].

## 8. Energía y arranque

Cuatro estados de energía; con la pantalla apagada el sistema entra en sueño ligero automático y el consumo baja de \~200 mA a pocos mA. La autonomía estimada va de \~13 h con pantalla encendida a semanas en reposo \[SUPOSICIÓN, **\[MEDIR\]** en H6\].

### Estados

| Estado | Entrada | Retroiluminación | Panel / LVGL | Wi-Fi | CPU | Salida |
| --- | --- | --- | --- | --- | --- | --- |
| ACTIVO | toque, BOOT, notificación | nivel del usuario | refresco a 16 ms si anima | modem-sleep mínimo | 240 MHz | 30 s sin toque |
| ATENUADO | 30 s sin toque | 20 % | igual | igual | 240 MHz | toque → ACTIVO; 90 s más → APAGADO |
| APAGADO | 2 min sin toque, o botón | 0 | `disp_sleep`, `lvgl` en pausa | modem-sleep máximo, listen interval 3–10 | DFS 80–240 MHz + sueño ligero auto | INT táctil (si sigue vivo), BOOT, temporizador de la pasarela |
| CRÍTICO | batería < 3,45 V | 0 | apagado | apagado | sueño profundo | BOOT (IO0 es GPIO RTC) |

Implementación: `CONFIG_PM_ENABLE`, `esp_pm_configure()` con `light_sleep_enable = true` y `CONFIG_FREERTOS_USE_TICKLESS_IDLE`. Los drivers de SPI, I2S y Wi-Fi toman sus propios *locks* de PM mientras trabajan. Espressif documenta corrientes medias de pocos mA con Wi-Fi conectado en sueño ligero automático, bajando con DTIM más largo \[WEB\].

### Batería

- Tensión por ADC1 en IO8 (`adc_oneshot` + calibración por curva). ADC1 no compite con Wi-Fi \[SUPOSICIÓN\]. Divisor: leer la relación en el esquemático \[PAQUETE\] y calibrar con multímetro **\[MEDIR\]**.
- Muestrear con la radio en reposo, mediana de 8 lecturas, tabla tensión → % de LiPo con histéresis.
- No hay pin de estado del TP4054 ni detección de VBUS en el mapa de pines: la carga se infiere por la tendencia de tensión **\[MEDIR\]** si el esquemático expone CHRG o VBUS.
- Corte a 3,45 V \[PROPUESTO\]: el ME6217 necesita margen sobre 3,3 V; caída real **\[MEDIR\]**.

### Autonomía estimada (3000 mAh, 85 % útil)

| Estado | Corriente supuesta | Autonomía |
| --- | --- | --- |
| ACTIVO, brillo alto | \~200 mA (198 mA medidos por el fabricante con pantalla \[PAQUETE\]) | \~13 h |
| APAGADO con modem-sleep (sin sueño ligero) | 25–35 mA | 3–4 días |
| APAGADO con sueño ligero auto + panel dormido | 3–10 mA | 10–35 días |

### Calor al cargar

Con USB, pantalla encendida y carga a la vez: ≈ (5 − 3,3) V × 0,2 A en el regulador + (5 − 3,7) V × 0,29 A en el cargador ≈ **0,7 W** en una placa pequeña \[SUPOSICIÓN\]. El TP4054 reduce la corriente al calentarse y la carga se corta a 62 °C \[PAQUETE\]. Mitigación: la app de diagnóstico registra el sensor de temperatura interno del S3 \[WEB\]; si sube de 55 °C con USB conectado, se limita el brillo al 50 %.

### Arranque

1. Comprobación de modo seguro: motivo de reset + contador de arranques en `RTC_NOINIT`.
2. NVS (`nvs_flash_init_partition` de `sys` y `nvs_app`).
3. BSP: I2C, panel con retroiluminación **apagada**, táctil, LED.
4. LVGL + launcher; la retroiluminación sube tras el primer frame (sin destello de basura).
5. Servicios: Wi-Fi, red, store.
6. A los 30 s estables: `esp_ota_mark_app_valid_cancel_rollback()` y contador a 0.

### OTA con rollback

- `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`: una imagen nueva que no se marca válida vuelve sola a la anterior.
- Fuentes: la pasarela (`esp_https_ota`) o un `update.bin` en la SD para recuperación sin red.
- Firma de aplicación: la pasarela publica `sha256 + firma ECDSA P-256`; la placa verifica con su clave pública antes de cambiar `otadata`. No requiere secure boot ni quemar eFuses.

### Modo seguro

- Se activa con 3 reinicios por pánico/WDT en menos de 30 s cada uno, o manteniendo BOOT 5 s al encender.
- Carga solo launcher, Ajustes, Diagnóstico y OTA; sin BLE, sin experimentos ni apps de terceros.
- Muestra el último volcado de fallos (app culpable, PC, pila) y ofrece borrar `nvs_app`.

## 9. Almacenamiento

Dos ranuras OTA de 5 MB, NVS del sistema separada de la NVS de apps, 4 MB de recursos de solo lectura mapeados en memoria y la SD para todo lo grande o escribible.

### Tabla de particiones (16 MB)

```csv
# Name,    Type, SubType,  Offset,    Size,     Flags
nvs,       data, nvs,      0x9000,    0x6000,
otadata,   data, ota,      0xF000,    0x2000,
phy_init,  data, phy,      0x11000,   0x1000,
nvs_keys,  data, nvs_keys, 0x12000,   0x1000,   encrypted
nvs_app,   data, nvs,      0x13000,   0x2D000,
ota_0,     app,  ota_0,    0x40000,   0x500000,
ota_1,     app,  ota_1,    0x540000,  0x500000,
coredump,  data, coredump, 0xA40000,  0x40000,
assets,    data, 0x40,     0xA80000,  0x400000,
lfs,       data, littlefs, 0xE80000,  0x180000,
```

- Las ranuras de app empiezan alineadas a 64 KB. 5 MB deja margen sobre un binario estimado de 2–3 MB con LVGL, Wi-Fi, BLE y TLS \[SUPOSICIÓN\].
- `nvs_app` separada: un experimento que corrompa o llene su NVS no se lleva las credenciales Wi-Fi ni el token del dispositivo.
- `nvs_keys` + cifrado de NVS: en el S3 se puede usar el esquema basado en el periférico HMAC, sin activar cifrado de flash \[SUPOSICIÓN: verificar requisitos de eFuse; quemar eFuses es **irreversible**, dejarlo para H6\].
- `assets` (subtipo propio) se empaqueta con `esp_mmap_assets` \[WEB\] y se mapea con `esp_partition_mmap`: iconos y fuentes se leen directamente desde flash por la caché, sin copiarlos a RAM.
- `lfs` (LittleFS) para datos pequeños escribibles que deben existir sin SD: registros de fallos, último snapshot, ajustes de apps grandes. Si el subtipo `littlefs` no lo reconoce la versión de IDF fijada, usar `spiffs` con el componente LittleFS \[SUPOSICIÓN\].

### NVS

| Partición | Namespace | Contenido |
| --- | --- | --- |
| `nvs` | `sys` | SSID/clave Wi-Fi, token del dispositivo, URL de la pasarela, CA pinneada |
| `nvs` | `cfg` | Brillo, tiempos de reposo, orientación/calibración táctil |
| `nvs_app` | `a.<id>` | Un namespace por app (nombre ≤ 15 caracteres) |

Escrituras: solo al cambiar un valor y nunca en bucle. El snapshot de la pasarela **no** se guarda en NVS (se reescribe a menudo); va a `lfs` como máximo cada 30 min.

### microSD

- SDMMC de 4 bits con los pines del §4.6 del briefing, FAT32, `esp_vfs_fat_sdmmc_mount` al abrir una app que lo pida y desmontaje al cerrarla.
- Uso: música, imágenes grandes, datos de experimentos, registros largos, `update.bin` para OTA offline, `wifi.json` de aprovisionamiento inicial (se lee una vez, se pasa a NVS y se borra).
- No hay interruptor de alimentación de la tarjeta en el mapa de pines: su consumo en reposo cuenta en la autonomía **\[MEDIR\]**.

### Formatos de recursos

| Recurso | Formato | Motivo |
| --- | --- | --- |
| Iconos y gráficos de UI | Binario LVGL RGB565 / RGB565A8 en `assets` | Cero decodificación, se dibujan desde flash |
| Fuentes | Compiladas en el firmware (const → flash) | Rápidas, sin sistema de archivos |
| Fotos de la SD | JPEG decodificado a la caché de PSRAM | Un decodificado por imagen, no por frame |
| Sonidos del sistema | PCM 16 kHz mono en `assets` | Sin decodificador para un *bip* |
| Música | MP3 (u Opus) en la SD | Pipeline de audio en §14, pregunta 6 |

## 10. Estructura del proyecto y simulador

Un único repositorio con firmware ESP-IDF en componentes, el simulador SDL que compila las mismas apps y la pasarela TypeScript. Tres entornos de PlatformIO (`dev`, `release`, `diag`) que solo difieren en el fichero de `sdkconfig.defaults` que añaden.

### Árbol

```text
es3c35p-os/
├─ platformio.ini
├─ boards/es3c35p.json          # 16 MB QIO, PSRAM OPI 8 MB
├─ partitions.csv
├─ sdkconfig.defaults           # común
├─ sdkconfig.dev / .release / .diag   # deltas por entorno
├─ CMakeLists.txt
├─ main/                        # app_main: arranque, modo seguro, orden de init
├─ components/
│  ├─ bsp_es3c35p/              # pines, QSPI+panel, táctil, i2c, ES8311+PA, ADC, LED, backlight
│  ├─ os_api/                   # SOLO cabeceras que ven las apps (App, Context, Msg, Store)
│  ├─ os_core/                  # bus, store, gestor de apps, arenas, power, modo seguro
│  ├─ os_ui/                    # puente LVGL 9 ↔ esp_lcd, tema, widgets comunes, launcher
│  ├─ svc_net/                  # wifi, cliente de pasarela, sntp, ota
│  ├─ svc_audio/                # i2s, códec, decodificadores, mezcla de avisos
│  ├─ svc_ble/                  # HID consumer control (NimBLE)
│  └─ apps/
│     ├─ Kconfig                # un CONFIG_OS_APP_* por app
│     ├─ apps.def
│     ├─ claude/  mail/  media/  settings/  diag/
│     └─ exp_template/          # plantilla de experimento
├─ sim/                         # CMake nativo + SDL2
│  ├─ CMakeLists.txt
│  ├─ hal_sim/                  # Store, Bus y Net falsos; fixtures JSON
│  └─ main_sim.cpp
├─ assets/                      # PNG/fuentes fuente → tools/ genera binarios LVGL
├─ gateway/                     # Node/TypeScript (Fastify)
└─ tools/                       # statusline.sh, empaquetado de assets, firma OTA
```

Regla de dependencias: `apps/*` solo dependen de `os_api` y LVGL. Si una app incluye `esp_*.h`, el simulador deja de compilarla; eso es intencional.

### `platformio.ini` (boceto)

```ini
[platformio]
default_envs = dev
boards_dir = boards

[env]
; fija una versión concreta de pioarduino; no uses "stable" en un proyecto largo
platform = https://github.com/pioarduino/platform-espressif32/releases/download/<versión>/platform-espressif32.zip
framework = espidf
board = es3c35p
board_build.partitions = partitions.csv
monitor_speed = 115200
monitor_filters = esp32_exception_decoder

[env:dev]
board_build.cmake_extra_args = -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.dev"

[env:release]
board_build.cmake_extra_args = -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.release"

[env:diag]
board_build.cmake_extra_args = -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.diag"
```

`board_build.cmake_extra_args` y la forma de pasar varios `SDKCONFIG_DEFAULTS` deben verificarse con la versión fijada de pioarduino \[SUPOSICIÓN\]. La última versión listada de pioarduino trae IDF 5.5.4 \[WEB\]; ESP-IDF 5.5.5 salió en julio de 2026 \[WEB\]. Si hace falta 5.5.5, se puede sustituir el paquete `framework-espidf` con `platform_packages` \[WEB\].

### `sdkconfig.defaults` (extracto)

```ini
# Flash y PSRAM
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y                 # 120 MHz es experimental
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y
CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y
# CPU, caché y RTOS
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB=y   # [MEDIR] frente a 16 KB
CONFIG_ESP32S3_DATA_CACHE_64KB=y          # [MEDIR] frente a 32 KB
CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y
CONFIG_COMPILER_OPTIMIZATION_PERF=y
CONFIG_FREERTOS_HZ=1000
CONFIG_FREERTOS_USE_TICKLESS_IDLE=y
CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK=y
CONFIG_PM_ENABLE=y
# Red
CONFIG_MBEDTLS_DYNAMIC_BUFFER=y
CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y
CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0=y
# Robustez
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y
CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF=y
CONFIG_ESP_COREDUMP_LOGS=n
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
# LVGL 9.6
CONFIG_LV_COLOR_DEPTH_16=y
CONFIG_LV_DEF_REFR_PERIOD=16
CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=1         # probar 2 en el benchmark
```

Los nombres exactos de las opciones de LVGL cambian entre versiones: generar el `sdkconfig` con `menuconfig` y copiar solo las líneas confirmadas \[SUPOSICIÓN\]. `sdkconfig.diag` añade `CONFIG_LV_USE_SYSMON`, `CONFIG_LV_USE_PERF_MONITOR` \[WEB\], `CONFIG_HEAP_TRACING_STANDALONE` y `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS`.

### Qué apps se compilan

`pio run -e dev -t menuconfig` → *OS apps* → marcar o desmarcar `CONFIG_OS_APP_*`. Un experimento nuevo = copiar `exp_template/`, añadir su entrada en `Kconfig` y `apps.def`.

### Simulador en PC

- LVGL 9.6 + driver SDL (`lv_sdl_window_create(480, 320)`) en un CMake nativo que compila `os_api`, `os_ui` (sin el puente esp\_lcd) y todas las apps.
- `hal_sim` implementa `Store`, `Bus` y `Net` con hilos de `std::thread` y fixtures JSON (`sim/fixtures/snapshot_*.json`), incluido el caso "pasarela caída".
- Ratón = táctil. Teclas para simular eventos: batería baja, pérdida de Wi-Fi, notificación.
- Lo que el simulador **no** prueba: rendimiento, DMA, consumo, I2C. Eso solo lo dice la placa.

## 11. Plan de medición

Primero se mide la placa con el firmware del fabricante para tener una línea base; después, cada cambio de arquitectura se acepta solo si mejora o iguala esa base. Los umbrales son \[PROPUESTO\] y se pueden ajustar tras la primera ronda.

### Orden de ejecución

1. **Línea base del fabricante:** flashear `lv_demo_benchmark.bin` y `lv_demo_widgets.bin` del paquete \[PAQUETE\]; anotar FPS medio, tiempo de render y de flush por escena.
2. **Bus QSPI aislado:** rellenar la pantalla completa 100 veces y cronometrar con `esp_timer_get_time()`; comparar con los \~8 ms teóricos a 80 MHz.
3. **Matriz de render propia** con `lv_demo_benchmark` sobre LVGL 9.6: plan A frente a plan B × franjas de 24, 32, 48 y 80 filas × buffer interno frente a PSRAM × `DRAW_UNIT_CNT` 1 frente a 2 × caché 16/32 frente a 32/64 KB.
4. **Memoria por fases** (§3): arranque, Wi-Fi arriba, pico TLS, BLE activo, audio sonando.
5. **Energía** por estado (§8) con un medidor en la línea de batería (PPK2 o similar), no en el USB.
6. **Pruebas de resistencia** antes de cerrar H6.

### Umbrales de aceptación

| Prueba | Herramienta | Umbral |
| --- | --- | --- |
| Flush de pantalla completa | `esp_timer` | ≤ 10 ms a 80 MHz; estable 1 h a temperatura de uso |
| Benchmark LVGL | `lv_demo_benchmark` | ≥ línea base del fabricante en todas las escenas; ≥ 30 FPS en escenas de widgets |
| Frame más lento | `CONFIG_LV_USE_SYSMON` | ≤ 100 ms |
| Latencia toque → evento LVGL | marca de tiempo en ISR y en el callback | ≤ 20 ms |
| Toque → imagen | vídeo a 240 fps | ≤ 60 ms |
| SRAM interna libre, configuración completa | `heap_caps_get_free_size` | ≥ 100 KB sin BLE; ≥ 60 KB con BLE |
| Mínimo histórico de SRAM | `heap_caps_get_minimum_free_size` | ≥ 40 KB tras 24 h |
| Bloque libre mayor | `heap_caps_get_largest_free_block` | ≥ 32 KB |
| Margen de pilas | `uxTaskGetStackHighWaterMark` | ≥ 25 % en cada tarea |
| Abrir/cerrar una app 100 veces | gestor + `lv_mem_monitor` | deriva ≤ 1 KB interna y ≤ 4 KB PSRAM |
| Audio MP3 128 kbps | contador de *underrun* del I2S | 0 en 1 h con el benchmark de UI corriendo |
| Caída de FPS con audio | benchmark con y sin audio | ≤ 10 % |
| Handshake TLS completo / reanudado | `esp_timer` en `net` | medir; objetivo reanudado ≤ 300 ms |
| Corriente ACTIVO / APAGADO | PPK2 | documentar; objetivo APAGADO ≤ 10 mA |
| Despertar desde APAGADO | toque o BOOT → primer frame | ≤ 300 ms |
| OTA con imagen defectuosa | imagen que entra en pánico al arrancar | vuelve sola a la anterior |
| Bucle de fallos | app que entra en pánico al abrir | modo seguro al 3.er reinicio |
| Resistencia | 72 h encendida con Wi-Fi y pasarela | sin reinicios ni deriva de heap |

### App de diagnóstico

Muestra en pantalla lo que la build `diag` recoge, para no depender del puerto serie:

- Heap interno y PSRAM: libre, mínimo, bloque mayor; arena usada por app.
- Tareas: % de CPU (run-time stats), margen de pila, núcleo.
- UI: FPS, tiempo de render y de flush (sysmon de LVGL).
- Red: RSSI, reconexiones, latencia del último snapshot, edad de cada fuente.
- Energía: tensión de batería, estado, temperatura interna.
- Exporta un volcado JSON por USB-Serial-JTAG y lo sube a la pasarela con el último coredump.

## 12. Hoja de ruta por hitos

&#91;embedded content: hoja de ruta · 8 hitos con criterio de aceptación\]

Los hitos van en orden de dependencia, no de valor: el asistente útil llega en H3, pero H1 y H2 fijan la memoria y el rendimiento sobre los que se apoya todo. Ningún hito se cierra sin su línea **Pasa**; los umbrales completos están en el §11.

## 13. Riesgos, supuestos y preguntas de hardware

El riesgo técnico más probable es que el bit MADCTL.MV no funcione en este módulo; el plan B lo absorbe sin coste de memoria. El más grave es de política: usar el token OAuth de Claude en la placa, que esta arquitectura evita.

### Riesgos

| Riesgo | Impacto | Mitigación | Cómo confirmarlo |
| --- | --- | --- | --- |
| MADCTL.MV no intercambia ejes en este módulo | Medio | Plan B (rotar al copiar), mismo presupuesto de SRAM | H1: pantalla de prueba con flechas y texto |
| Restricción de múltiplos de 4 en el eje inesperado con MV | Bajo | Redondear ambos ejes | H1: líneas de 1 px en bordes |
| QSPI inestable a 80 MHz con calor | Medio | Empezar a 40 MHz; subir por pasos; prueba térmica de 1 h | H1 |
| Sin línea TE → tearing en animaciones grandes | Bajo | Transiciones instantáneas o fundidos | Esquemático + vídeo a cámara lenta |
| El táctil TDDI se duerme con el panel | Medio | Despertar con BOOT; o mantener el panel encendido con retroiluminación a 0 | H1/H6 |
| Dirección táctil 0x38 en vez de 0x55, u otro chip | Bajo | Escaneo I2C en H0 | H0 |
| Desfase de versiones pioarduino ↔ ESP-IDF | Medio | Fijar versión; `platform_packages` como salida; no depender de `esp_lvgl_adapter` (pide IDF ≥ 5.5.5 \[WEB\]) | Al crear el proyecto |
| Campo `rate_limits` ausente en algunas versiones de Claude Code \[WEB\] | Bajo | La UI muestra antigüedad y "sin datos" | H3 |
| Uso de token OAuth de Claude en terceros | **Alto** (cuenta) | No se usa; datos vía `statusLine` | — |
| Pines de *strapping* en el conector de expansión | **Alto** (no arranca) | IO45 elige la tensión VDD\_SPI al arrancar: en alto selecciona 1,8 V \[WEB\] → nada que lo tire a alto en el reset. IO46 con IO0 a 0 debe estar a 0 para entrar en descarga \[WEB\] | Revisar antes de conectar periféricos |
| IO3 (D3 de la SD) también es *strapping* | Bajo | Solo importa si se quema `EFUSE_STRAP_JTAG_SEL` \[WEB\]: no quemarlo | — |
| Colisiones I2C en el conector | Medio | 0x55 coincide con medidores de batería tipo BQ27xxx; el ES8311 suele estar en 0x18, como el acelerómetro LIS3DH \[SUPOSICIÓN\] | Escaneo I2C en H0 |
| Altavoz del móvil sobrecargado por el amplificador BTL (2 W a 4 Ω) | Medio | Medir impedancia; limitar ganancia del códec y volumen máximo por software | H4 |
| Calor: carga + pantalla ≈ 0,7 W | Medio | Limitar brillo con USB y temperatura > 55 °C | H6 |
| Caída del regulador con batería baja → *brownout* | Medio | Corte a 3,45 V; detector de brownout activo | H6 |
| Binarios xiaozhi del paquete con servidor desconocido | Medio (privacidad) | No flashearlos, o solo en una red aislada | — |
| Operaciones irreversibles de eFuse (cifrado de flash, secure boot, HMAC) | **Alto** | Ninguna antes de H6; probar primero en modo desarrollo | — |

### Supuestos principales

- Estimaciones de SRAM y PSRAM del §3 (todas \[SUPOSICIÓN\] hasta H2).
- Que la caché de instrucciones/datos se reserve de la SRAM interna.
- Corrientes y autonomía del §8.
- Que LVGL 9.6 conserve `LV_EVENT_INVALIDATE_AREA`, `lv_draw_sw_rotate()` y el modo de indev por eventos tal como se describen.

### Preguntas que solo resuelve la placa

- [ ] ¿Qué dispositivos responden en el bus I2C (0x55, 0x38, 0x18…)?
- [ ] ¿Funciona `supports_madctl_mv = 1` con la secuencia de init del fabricante? ¿En qué eje aplica el múltiplo de 4?
- [ ] ¿Existe línea TE en el FPC o en el esquemático?
- [ ] ¿El táctil sigue respondiendo con el panel en `SLPIN`?
- [ ] ¿Estabilidad del QSPI a 80 MHz tras 1 h?
- [ ] ¿Relación del divisor del ADC de batería (IO8)?
- [ ] ¿Polaridad del conector BAT y comportamiento al conmutar USB ↔ batería?
- [ ] ¿Expone el esquemático CHRG del TP4054 o VBUS a algún GPIO?
- [ ] ¿Impedancia y potencia del altavoz?
- [ ] ¿Cómo llega el micrófono MEMS al ES8311 (entrada analógica, ganancia)?
- [ ] ¿Consumo de la microSD en reposo?
- [ ] ¿Corriente real en cada estado de energía?

## 14. Respuestas a las 15 preguntas abiertas

Respuesta corta por pregunta; el detalle está en la sección indicada. Audio (6) y voz (13) no tienen sección propia y se desarrollan debajo de la tabla.

| # | Pregunta | Respuesta | Confianza | Detalle |
| --- | --- | --- | --- | --- |
| 1 | Rotación sin coste | `esp_lcd_st77922` ≥ 2.1.0 permite MV solo con opt-in `supports_madctl_mv` tras validarlo; si falla, rotar en la copia al buffer DMA (≈ coste de un `memcpy`). El táctil se transforma aparte con 3 flags | \[WEB\] + \[MEDIR\] | §5 |
| 2 | Estrategia de buffers | Doble buffer parcial de 32 filas en SRAM interna DMA (60 KB). El hardware puede leer PSRAM por DMA, pero el driver `spi_master` de IDF copia a SRAM \[WEB\] | \[WEB\] | §5 |
| 3 | Presupuesto de SRAM | 60–130 KB libres con todo activo; ≥ 100 KB exige Wi-Fi/lwIP/TLS en PSRAM y BLE bajo demanda | \[SUPOSICIÓN\] → H2 | §3 |
| 4 | Bus I2C | Un `i2c_master_bus` a 400 kHz en IO38/39; cada chip es un dispositivo del bus; las apps añaden dispositivos externos por la API del sistema (permiso). Táctil solo tras INT; códec solo al iniciar audio | \[WEB\] API; serialización interna del driver \[SUPOSICIÓN\] | abajo |
| 5 | Wi-Fi + BLE | Coexistencia soportada en el S3 con NimBLE \[WEB\]; HID usa muy poco ancho de banda; coste 30–50 KB de SRAM → BLE se inicia bajo demanda | \[WEB\] + \[SUPOSICIÓN\] | §7 |
| 6 | Audio | I2S de IDF + `esp_codec_dev` (ES8311) + decodificador ligero; no ESP-ADF ni `ESP32-audioI2S` | \[WEB\] | abajo |
| 7 | Energía | 4 estados, sueño ligero automático con pantalla apagada, \~13 h encendida | \[SUPOSICIÓN\] → H6 | §8 |
| 8 | Particiones | 2 × 5 MB OTA, NVS sys + NVS apps, 256 KB coredump, 4 MB assets mmap, 1,5 MB LittleFS | \[PROPUESTO\] | §9 |
| 9 | Aislamiento | Arena PSRAM por app, permisos por inyección, TWDT, watchpoint de pila, contador de fallos → modo seguro | \[PROPUESTO\] | §6, §8 |
| 10 | LVGL 8.4 vs 9.x | 9.6: release de estabilidad, más rápida en render por software \[WEB\]; el sueño dirigido por eventos de `esp_lvgl_port` solo existe con LVGL 9 \[WEB\]. Coste: migrar los ejemplos del fabricante | \[WEB\] | §1 |
| 11 | API de apps | `App` + `Service` + `Manifest` + `Context` inyectado, registro con Kconfig | \[PROPUESTO\] | §6 |
| 12 | Protocolo | HTTPS + `ETag` + long-poll con pantalla encendida, sondeo cada 5 min apagada, JSON ≤ 2 KB, token de dispositivo | \[PROPUESTO\] | §7 |
| 13 | Voz | Sí como app *push-to-talk* con servidor propio; no palabra de activación permanente | \[WEB\] | abajo |
| 14 | Seguridad | Ningún secreto de terceros en la placa; NVS cifrado; eFuses solo tras H6 | \[PROPUESTO\] | §7, §9 |
| 15 | Observabilidad | App de diagnóstico + build `diag` + coredump a la pasarela | \[PROPUESTO\] | §11 |

### 4. Bus I2C compartido

```c
i2c_master_bus_config_t bus = {
  .i2c_port = I2C_NUM_0, .sda_io_num = 38, .scl_io_num = 39,
  .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
  .flags.enable_internal_pullup = false,   // [MEDIR] pull-ups de la placa
};
i2c_new_master_bus(&bus, &g_i2c);
i2c_master_bus_add_device(g_i2c, &(i2c_device_config_t){
  .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x55, .scl_speed_hz = 400000 }, &g_touch);
```

El driver nuevo sigue un modelo bus–dispositivo y no puede convivir con el driver antiguo `i2c.h` en el mismo binario \[WEB\]; los ejemplos Arduino del fabricante no sirven tal cual. El máximo en modo maestro es 400 kHz \[WEB\]. El S3 tiene un segundo controlador I2C \[WEB\], útil si un experimento necesita otro bus en IO45/IO46 (con la precaución de *strapping* del §13).

### 6. Pipeline de audio

- **Salida:** I2S estándar de IDF en maestro (MCLK IO17, BCLK IO18, LRC IO21, DO IO16), 44,1/48 kHz, 16 bits. El ES8311 se configura por I2C con `esp_codec_dev` \[SUPOSICIÓN: verificar soporte ES8311 en la versión fijada\].
- **Decodificación:** `esp_audio_codec` de Espressif (MP3, AAC, Opus, ADPCM…) \[WEB\], o `micro-mp3`, que en el S3 decodifica MP3 48 kHz estéreo 64 kbps a 16× tiempo real (≈ 6 % de un núcleo) con 28 KB de estado en PSRAM \[WEB\].
- **Tarea `audio`** en el núcleo 0, prioridad 8: lee la SD en bloques de 32 KB → decodifica → `StreamBuffer` PCM de 64 KB en PSRAM → `i2s_channel_write`. Los buffers DMA del I2S (≈ 6 × 240 tramas) en SRAM.
- **No interfiere con la UI:** núcleo distinto, sin llamadas a LVGL, el progreso se publica al bus una vez por segundo.
- **Amplificador:** IO1 a bajo solo mientras suena (ahorro y sin soplido en reposo).
- **Micrófono:** ADC del ES8311 → I2S DI (IO15), 16 kHz mono; prueba en H4 grabando WAV a la SD.
- **Por qué no ESP-ADF:** es un framework completo de pipelines, más memoria y más superficie que un reproductor de un solo flujo. `ESP32-audioI2S` es una librería Arduino.

### 13. Voz

- **Local:** el frente de audio de ESP-SR con 1 micrófono + WakeNet en el S3 usa ≈ **72 KB de SRAM interna** y ≈ 733 KB de PSRAM, más ≈ 23 % de un núcleo \[WEB\]. Es más que el margen de experimentos que queremos dejar libre, y MultiNet (comandos) solo reconoce chino e inglés \[WEB\].
- **Propuesta:** app *push-to-talk* (botón en pantalla o BOOT): graba 16 kHz mono, comprime (Opus o ADPCM), envía a la pasarela; la pasarela hace STT en español (p. ej. Whisper), consulta un LLM con **clave de API** y devuelve texto + audio TTS. Sin palabra de activación permanente.
- Encaja después de H7, como experimento; no condiciona la arquitectura.

## Fuentes

- [esp\_lcd\_st77922 2.1.1, readme](https://components.espressif.com/components/espressif/esp_lcd_st77922/versions/2.1.1/readme) · [changelog 2.1.0](https://components.espressif.com/components/espressif/esp_lcd_st77922/versions/2.1.0/changelog)
- [esp\_lvgl\_port (esp-bsp)](https://github.com/espressif/esp-bsp/blob/master/components/esp_lvgl_port)
- [ESP LVGL Adapter, documentación](https://docs.espressif.com/projects/esp-iot-solution/en/latest/display/tools/esp_lvgl_adapter.html) · [readme 0.7.3](https://components.espressif.com/components/espressif/esp_lvgl_adapter/versions/0.7.3~1/readme) · [changelog](https://components.espressif.com/components/espressif/esp_lvgl_adapter/versions/0.7.3~1/changelog)
- [ESP-Techpedia: pantallas SPI/QSPI](https://docs.espressif.com/projects/esp-techpedia/en/latest/esp-friends/advanced-development/lcd-application-note/spi-qspi-summary.html)
- [Foro ESP32: DMA SPI y PSRAM en el S3](https://esp32.com/viewtopic.php?p=148400)
- [LVGL: rotación](https://lvgl.io/docs/open/main-modules/display/rotation) · [política de versiones](https://lvgl.io/docs/open/introduction/repo.mdx) · [changelog 9.6.0](https://lvgl.io/docs/open/dev/changelog/CHANGELOG) · [API lv\_display 9.2](https://docs.lvgl.io/9.2/API/display/lv_display.html)
- [ESP-IDF: minimizar RAM (S3)](https://docs.espressif.com/projects/esp-idf/en/v4.4/esp32s3/api-guides/performance/ram-usage.html) · [I2C (S3, 5.3)](https://docs.espressif.com/projects/esp-idf/zh_CN/release-v5.3/esp32s3/api-reference/peripherals/i2c.html) · [bajo consumo con Wi-Fi](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32c3/api-guides/low-power-mode/low-power-mode-wifi.html) · [BLE en el S3](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/ble/overview.html) · [notas de la v5.1](https://github.com/espressif/esp-idf/releases/tag/v5.1)
- [Pines de strapping del S3 (RIOT, basado en el datasheet)](https://doc.riot-os.org/group__cpu__esp32__esp32s3.html) · [esp32-s3-pinouts](https://github.com/mcuw/esp32-s3-pinouts)
- [pioarduino platform-espressif32](https://github.com/pioarduino/platform-espressif32) · [tabla de versiones](https://github.com/sivar2311/platform-espressif32-versions) · [releases de la plataforma oficial](https://github.com/platformio/platform-espressif32/releases) · [IDF 5.5.5 / 6.0.2](https://releasebot.io/updates/espressif) · [cambiar framework-espidf](https://community.platformio.org/t/using-higher-version-of-espidf/47924)
- [ESP-SR, benchmark S3](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/benchmark/README.html) · [ESP-SR, repositorio](https://github.com/espressif/esp-sr)
- [esp\_audio\_codec](https://components.espressif.com/components/espressif/esp_audio_codec/versions/2.0.0) · [micro-mp3, benchmark](https://components.espressif.com/components/esphome/micro-mp3/versions/0.4.0/examples/decode_benchmark)
- [Claude Usage Stick, cómo funciona](https://github.com/oauramos/claude-usage-stick/wiki/How-It-Works) · [WinBuzzer: OAuth de suscripción en terceros](https://winbuzzer.com/2026/02/19/anthropic-bans-claude-subscription-oauth-in-third-party-apps-xcxwbn/) · [statusLine y rate\_limits](https://wmedia.es/es/tips/claude-code-limite-uso-status-line) · [issue #40094](https://github.com/anthropics/claude-code/issues/40094) · [issue #45133](https://github.com/anthropics/claude-code/issues/45133)
