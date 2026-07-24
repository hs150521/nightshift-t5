# T5AI-BOARD Pin Assignment Reference

Board: **Tuya TUYA_T5AI_BOARD** with **T5AI-BOARD-LCD** (3.5″ 480×320)

---

## UART0 — Host Communication (460 800 baud, 8-N-1)

**UART0 is the only usable UART for host communication.** UART1 (GPIO0/1,
silkscreen P0/P1) is hard-locked by the SDK as the log console
(`CONFIG_UART_PRINT_PORT=1`; `tkl_uart_init` refuses to init it). UART2's
alternate pins GPIO40/41 are occupied by the LCD RGB bus.

| Signal | GPIO Pin | Silkscreen | Header location | Direction |
|--------|----------|------------|-----------------|----------|
| RX     | PIN 10 (GPIO10) | **P10** | P11 header **Pin 1** | OPi → T5  |
| TX     | PIN 11 (GPIO11) | **P11** | P11 header **Pin 2** | T5 → OPi  |

> Verified by hardware loopback (jumper across header Pin 1/Pin 2) and live
> OPi traffic: silkscreen P10/P11 sit at **Pin 1 and Pin 2 of the P11 header**.

### OPi ↔ T5 wiring (verified working)

| OPi pin | T5 pin |
|---------|--------|
| Pin 28 (UART3 TX) | P11 header Pin 1 (P10, UART0 RX) |
| Pin 27 (UART3 RX) | P11 header Pin 2 (P11, UART0 TX) |
| GND | GND |

### DIP switch (4-bit, controls CH342 USB-serial bridging)

| Scenario | P0 | P1 | P10 | P11 |
|----------|----|----|-----|-----|
| Normal OPi comms (recommended) | OFF | OFF | **OFF** | **ON** |
| Flashing firmware | OFF | OFF | ON | ON |
| PC-side RX injection test (COM7 → GPIO10) | OFF | OFF | ON | ON |

- **P10=ON connects CH342 TX to GPIO10** — must be OFF while the OPi TX is
  wired, otherwise the two drivers fight on the RX line.
- **P11=ON connects CH342 RX to GPIO11** — passive tap; safe to keep ON so
  COM7 can monitor everything the T5 transmits.

### RX path verification (2026-07)

Full stack verified by injecting bytes from the PC via COM7 with P10=ON:
raw bytes → `[NS] RX n bytes` hex dump → 0x00 delimiter framing → COBS
decode → CRC-16 check → `cmd=0x1002` dispatch → RESPONSE frame transmitted.
Use `test_frame.py` to repeat this test.

---

## LCD — ILI9488 3.5″ 480×320 RGB565

The LCD is driven via software SPI (bit-bang) for the command interface; pixel
data is pushed through the RGB parallel bus internally.

| Signal | GPIO Pin | Notes |
|--------|----------|-------|
| SPI CLK  | PIN 49 | Software SPI clock |
| SPI CS   | PIN 48 | Chip-select (active LOW) |
| SPI SDA  | PIN 50 | MOSI / data line |
| RST      | PIN 53 | Active LOW reset |
| Backlight | PWM 7  | `TUYA_PWM_NUM_7` — brightness control |

Display parameters (from `tuya_t5ai_ex_module.h`):

| Parameter | Value |
|-----------|-------|
| Resolution | 320 × 480 |
| Pixel format | RGB565 |
| Rotation | 0° |
| Driver IC | ILI9488 |

---

## Touch Panel — GT1151 (I²C)

| Signal | GPIO Pin |
|--------|----------|
| I²C SCL | PIN 13 |
| I²C SDA | PIN 15 |
| I²C port | `TUYA_I2C_NUM_0` |

---

## On-Board LED

| Parameter | Value |
|-----------|-------|
| GPIO      | PIN 1 |
| Active level | HIGH |
| Driver type  | GPIO (on/off only — no PWM brightness) |

> Single-colour LED. Breathing effects are not supported; falls back to
> blink or steady on/off in `ui_manager.c`.

---

## Button

| Parameter | Value |
|-----------|-------|
| GPIO      | PIN 12 |
| Active level | LOW (pull-up, active on press) |

---

## Pin Summary Table

| Function | Pin | Active |
|----------|-----|--------|
| UART0 TX | 11 (P11) | —      |
| UART0 RX | 10 (P10) | —      |
| LCD SPI CLK | 49 | —   |
| LCD SPI CS  | 48 | LOW  |
| LCD SPI SDA | 50 | —   |
| LCD RST     | 53 | LOW  |
| LCD Backlight | PWM7 | — |
| Touch SCL | 13  | —      |
| Touch SDA | 15  | —      |
| LED       | 1   | HIGH  |
| Button    | 12  | LOW   |
