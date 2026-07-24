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

### Wiring (OPi ↔ T5) — verified working 2026-07

| OPi pin | T5 pin | Signal |
|---------|--------|--------|
| Pin 28 (UART3 TX) | P11 header Pin 1 (silkscreen **P10**, GPIO10, UART0 RX) | OPi → T5 data |
| Pin 27 (UART3 RX) | P11 header Pin 2 (silkscreen **P11**, GPIO11, UART0 TX) | T5 → OPi data |
| GND | GND | Common ground |

> Only three wires are needed. No flow-control lines required.
> DIP switch for normal OPi communication: **P0=OFF, P1=OFF, P10=OFF,
> P11=ON**. P10 must remain off while OPi TX is connected. Detect the CH342
> interface with `scan_ports.py`; do not persist a machine-specific COM number.

---

## Protocol — T5-Link v1

| Property | Value |
|----------|-------|
| Baud rate | 460 800 |
| Framing | COBS + `0x00` delimiter |
| Integrity | CRC-16/CCITT-FALSE |
| Max payload | 1 024 bytes |
| Heartbeat | 2 s interval, 6 s timeout |

See [docs/t5-link-v1.md](docs/t5-link-v1.md) for the frozen layouts and
the deliberate compatibility decisions against the Orange Pi golden vectors.

---

## Build

The verified SDK baseline is TuyaOpen `v1.9.0-1-g806690f0`, commit
`806690f015f91995bd4c7bb49e76d8010a6db484`.

```powershell
$env:Path=(Resolve-Path '../TuyaOpen/.venv/Scripts').Path + ';' + $env:Path
& ../TuyaOpen/.venv/Scripts/python.exe ../TuyaOpen/tos.py build
```

Run that command from this repository. Full build, flash, port-detection, and
troubleshooting instructions are in [docs/build-flash.md](docs/build-flash.md).

---

## Implemented panel behavior

- Atomic `STATE_SYNC_BEGIN` / `STATE_SYNC_END` staging with coherent commit.
- Revision guards and 32-entry duplicate request replay.
- Mode, work state, progress, dashboard, attention, notices, and task list.
- Confirm, reject, retry, pause, resume, dismiss, open-task, and page events.
- Full-screen host-offline overlay and disabled side-effect controls after 6 s.
- Binary-only protocol UART0; SDK diagnostics stay on UART1/mailbox.
- Backlight control through the Tuya display driver.
- No Wi-Fi or MQTT communication in the T5 application.

The on-board LED is intentionally not advertised in the current build because
the verified SDK board override disables it to preserve UART0 P10/P11.

## Tests

```powershell
python -m unittest discover -s tests -v
python tools/sync_golden_vectors.py --check
```

The suite covers golden frames, malformed COBS, CRC/length/version/flag errors,
field offsets, heartbeat response size, duplicate replay, revision rejection,
atomic state/task replacement, UI action encoding, and watchdog recovery.

---

## Directory Structure

```
nightshift-t5/
├── CMakeLists.txt              # Root build file (TuyaOpen SDK)
├── app_default.config          # Default board/LVGL/UART config flags
├── include/
│   └── nightshift_config.h     # Compile-time constants (UART, heartbeat, limits)
├── src/
│   ├── firmware_main.c         # Application entry and subsystem lifecycle
│   ├── t5_protocol.*           # COBS, CRC-16, strict frame codec
│   ├── frame_stream.*          # Delimiter/overflow recovery
│   ├── request_cache.*         # Duplicate request response replay
│   ├── command_router.*        # Strict command payload parsers
│   ├── state_store.*           # Atomic committed/staging state
│   ├── uart_transport.*        # UART0 RX/TX, events, watchdog
│   └── panel_ui.*              # LVGL UI and touch controls
├── contracts/uart/             # Canonical Orange Pi golden vectors
├── tests/                      # Host protocol/state tests
├── tools/                      # Host codec and synchronization tooling
└── docs/                       # Wiring, protocol, build/flash notes
```

---

## Licence

Proprietary — internal use only.
