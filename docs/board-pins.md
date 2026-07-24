# T5AI-BOARD Pin Assignment Reference

Board: **Tuya TUYA_T5AI_BOARD** with **T5AI-BOARD-LCD** (3.5″ 480×320)

---

## UART0 — Host Communication (460 800 baud, 8-N-1)

P11 header:

| Signal | GPIO Pin | Header Pin | Direction |
|--------|----------|------------|----------|
| TX     | PIN 11 (P11) | P11 Pin 14 | T5 → OPi  |
| RX     | PIN 10 (P10) | P11 Pin 12 | OPi → T5  |

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
