# Nightshift T5

Firmware for the **Tuya T5AI-BOARD** that drives a 3.5″ 480×320 RGB LCD as a
wall-mounted status panel for an Orange Pi (OPi) host running the Nightshift
orchestrator.

Communication uses **T5-Link v1** — a lightweight binary protocol over UART
(460 800 baud, 8-N-1) with COBS framing and CRC-16 integrity checks.

---

## Hardware

| Component | Details |
|-----------|---------|
| MCU board | Tuya T5AI-BOARD (T5AI chip, FreeRTOS) |
| LCD module | T5AI-BOARD-LCD — 3.5″ 480×320 RGB565, ILI9488 driver, GT1151 capacitive touch |
| Host | Orange Pi (any model with a free UART) |

### Wiring (OPi ↔ T5)

| OPi pin | T5 pin | Signal |
|---------|--------|--------|
| TX      | PIN 40 (UART2 RX) | OPi → T5 data |
| RX      | PIN 41 (UART2 TX) | T5 → OPi data |
| GND     | GND    | Common ground |

> Only three wires are needed. No flow-control lines required.

---

## Protocol — T5-Link v1

| Property | Value |
|----------|-------|
| Baud rate | 460 800 |
| Framing | COBS + `0x00` delimiter |
| Integrity | CRC-16/CCITT-FALSE |
| Max payload | 1 024 bytes |
| Heartbeat | 2 s interval, 6 s timeout |

See `Nightshift_OPi5B_T5_联合开发文档_v1.md` for the full command reference.

---

## Build

The project is built with the TuyaOpen SDK using `tos.py`:

```bash
# From the TuyaOpen SDK root
python tos.py build --board TUYA_T5AI_BOARD --app /path/to/nightshift-t5
```

The default board configuration (`app_default.config`) enables the 3.5″ LCD
with LVGL v9 and UART support.

---

## Phase 1 Features (MVP)

- **Three-mode display**
  - 待命 (IDLE) — warm orange background
  - 工作中 (DAY_WORK) — cool blue-white background
  - 夜间执行 (NIGHT_EXEC) — deep blue background
- **Offline overlay** — semi-transparent "主机离线" when heartbeat is lost
- **Progress bar** — live XX.X% during RUNNING work-state
- **Attention alerts** — "待确认: N项" prompt when confirmations are pending
- **LED feedback** — on/off/blink patterns per mode; fast blink on attention
- **Heartbeat watchdog** — 6 s timeout triggers offline state automatically

---

## Directory Structure

```
nightshift-t5/
├── CMakeLists.txt              # Root build file (TuyaOpen SDK)
├── app_default.config          # Default board/LVGL/UART config flags
├── include/
│   └── nightshift_config.h     # Compile-time constants (UART, heartbeat, limits)
├── src/
│   ├── main.c                  # Application entry point & init sequence
│   ├── t5_protocol.h / .c      # Frame codec (COBS, CRC-16, encode/decode)
│   ├── uart_handler.h / .c     # UART2 transport & RX task
│   ├── command_handler.h / .c  # Command dispatcher (routes frames → state)
│   ├── app_state.h / .c        # Global state mirror with revision guard
│   └── ui_manager.h / .c       # LVGL UI: mode pages, overlay, LED control
└── docs/
    └── board-pins.md           # Pin assignment reference
```

---

## Licence

Proprietary — internal use only.
