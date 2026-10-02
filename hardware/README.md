# Hardware

Put your PCB files here (schematic, board layout, Gerbers, BOM, photos).

## OLED (SSD1306, 128x64, I2C, address 0x3C)

| OLED | ESP32-WROOM-32 (PCB) | LOLIN S2 Mini (breadboard) |
|------|----------------------|----------------------------|
| VCC  | 3V3                  | 3V3                        |
| GND  | GND                  | GND                        |
| SCL  | GPIO 4               | GPIO 4                     |
| SDA  | GPIO 2               | GPIO 15                    |

Select the board with `BOARD_S2_MINI` / `BOARD_WROOM32` at the top of the firmware.

## 15-LED progress bar

15 LEDs (H1-H15), each with a 220 ohm series resistor, cathodes to GND.
Fill in the GPIO for each LED from your schematic:

| LED | GPIO | LED | GPIO | LED | GPIO |
|-----|------|-----|------|-----|------|
| H1  |      | H6  |      | H11 |      |
| H2  |      | H7  |      | H12 |      |
| H3  |      | H8  |      | H13 |      |
| H4  |      | H9  |      | H14 |      |
| H5  |      | H10 |      | H15 |      |
