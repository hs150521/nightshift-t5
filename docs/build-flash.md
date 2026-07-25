# Build, flash, and serial validation

## Fixed toolchain

- TuyaOpen tag/description: `v1.9.0-1-g806690f0`
- TuyaOpen commit: `806690f015f91995bd4c7bb49e76d8010a6db484`
- Target: `TUYA_T5AI_BOARD`, version 1.0.2 board configuration
- Toolchain: GNU Arm Embedded `10.3-2021.10`
- LCD: `CONFIG_TUYA_T5AI_BOARD_LCD_35565`
- LVGL: v9

The local SDK currently contains deliberate board edits that disable the
on-board LED and retain UART1 as the SDK log console. Do not discard those
changes: protocol UART0 is P10/P11 and must remain binary-only.

## Build

Run from `nightshift-t5`:

```powershell
$env:Path=(Resolve-Path '../TuyaOpen/.venv/Scripts').Path + ';' + $env:Path
& ../TuyaOpen/.venv/Scripts/python.exe ../TuyaOpen/tos.py build
```

Output images are under `.build/bin/`. TuyaOpen's Windows T5 platform also
requires GNU `make` inside its bundled bash environment. If the application
library compiles but platform packaging reports `make: command not found`,
run `tos.py prepare`, then include
`../TuyaOpen/.tools/make/4.4.1` in `PATH` and set `OPEN_SDK_MAKE` to its
`make.exe` before rebuilding.

## Detect ports

```powershell
& ../TuyaOpen/.venv/Scripts/python.exe scan_ports.py
```

On the machine inspected on 2026-07-25, the connected devices were:

- CH342 dual interface, serial `5AAE165866` (the T5 board);
- CH340;
- CP210x.

The COM numbers are intentionally not stored here. The CH342 UART0 passive tap
is identified by interface descriptor and by valid T5-Link frames, not by an
assumed number.

## Flash

First list the actual TuyaOpen flash options:

```powershell
& ../TuyaOpen/.venv/Scripts/python.exe ../TuyaOpen/tos.py flash --help
```

Use the detected T5 download interface and the generated UA image. Do not
select the CH340/CP210x merely because it has a lower or higher COM number.
Keep the verified OPi wiring unchanged. Only if the flash tool proves it
cannot enter download mode should the DIP/button state be changed.

## UART validation

`monitor.py` parses COBS/CRC frames; it never interprets UART0 as text.

```powershell
& ../TuyaOpen/.venv/Scripts/python.exe monitor.py --auto
& ../TuyaOpen/.venv/Scripts/python.exe test_frame.py --port COMx
```

With P10 off, the CH342 tap is receive-only and `test_frame.py` must not be
used for injection. With the Orange Pi connected, validate handshake and
heartbeat through its UART3 service instead.

Plain ASCII output on UART0 is a firmware/configuration failure. SDK `PR_*`
logs belong on UART1/mailbox.

## 2026-07-25 hardware record

- Full T5AI build completed successfully and produced
  `.build/bin/nightshift-t5_QIO_1.0.0.bin`.
- The Python contract tests passed (15 tests, 21 vector subtests).
- COM7 was identified as CH342 interface A and the automatic flash tool reached
  `Handshake OK`, but both 921600 and 460800 attempts timed out during the
  following handshake sequence.
- No valid T5-Link frames were observed passively on COM6 or COM7.

The last two items mean the firmware image is ready, but the board still needs
the physical download/reset sequence before another flash attempt. They are not
evidence that a new image is running on the panel.
