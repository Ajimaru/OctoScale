<!-- markdownlint-disable MD033 -->

# OctoScale Assembly Guide

## Wiring

All pin numbers below are GPIO numbers and match `src/main.cpp`. The PN5180 and TFT use separate SPI buses.

### Power

<img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/USB-C_breakout_board.jpg" width="300" alt="USB-C breakout board"> <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/Mini-360_buck_converter.jpg" width="300" alt="Mini-360 buck converter">

Power enters through the USB-C breakout. The 5 V rail powers the ESP32-S3, PN5180 RF section, and TFT. The Mini-360 converts 5 V to 3.3 V for the HX711, PN5180 logic, and external WS2812. Everything shares one ground.

Before connecting any load, feed the Mini-360 with 5 V and adjust `OUT+` to exactly 3.3 V with a multimeter. Connect the 470 uF capacitor directly across PN5180 `#5V` and `GND`.

Never power the ESP32-S3 from USB while external 5 V is connected to its `5V`/`VBUS`/`VIN` pin. Disconnect the external supply for the first USB flash and any later recovery flash.

### Signal wiring

<img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/HX711_5kg_load_cell_acrylic_mounting_plates_combo.jpg" width="420" alt="5 kg load cell with HX711 breakout and acrylic mounting plates">

| Component | Pin | ESP32-S3 / rail |
| --- | --- | --- |
| Load cell | red / black / white / green | HX711 `E+` / `E-` / `A-` / `A+` |
| HX711 | `VCC`, `GND`, `DT`, `SCK` | 3.3 V, GND, GPIO 5, GPIO 6 |
| PN5180 | `+5V`, `+3.3V`, `GND` | 5 V, 3.3 V, GND |
| PN5180 | `SCK`, `MOSI`, `MISO`, `NSS` | GPIO 12, 11, 13, 10 |
| PN5180 | `BUSY`, `RST`, `IRQ` | GPIO 14, 21, 47 |
| TFT/encoder module | `VCC`, `GND` | 5 V, GND |
| TFT | `SCL`, `SDA`, `CS`, `DC`, `RES`, `BLK` | GPIO 42, 44, 38, 39, 40, 41 |
| Encoder/buttons | `A`, `B`, `PUSH`, `KO` | GPIO 15, 16, 17, 18; other switch legs to GND |
| Buzzer | `+`, `-` | GPIO 7, GND |
| External WS2812 | `DIN`, `VCC`, `GND` | GPIO 9, 3.3 V, GND |

<img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/PN5180_NFC_module.jpg" width="420" alt="PN5180 NFC module">
<img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/ST7789_TFT_320x240_EC11_encoder_2buttons.jpg" width="420" alt="S11-05 module with ST7789 display, EC11 encoder, and two buttons">
<img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/buzzer.jpg" width="420" alt="Passive buzzer">
<img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/WS2812.jpg" width="420" alt="WS2812 RGB LED module">

GPIO 44 is often labelled `RX`; it is still GPIO 44. If encoder rotation is reversed, swap `A` and `B`. If weight becomes negative when loaded, swap the load-cell white and green wires. PN5180 `GPIO`, `AUX`, and `REQ` remain unconnected.

Complete summary: GPIO 5/6 HX711; 7 buzzer; 9 external WS2812; 10 PN5180 NSS; 11/12/13 PN5180 MOSI/SCK/MISO; 14 PN5180 BUSY; 15/16/17 encoder A/B/PUSH; 18 start button; 21 PN5180 RST; 38/39/40/41 TFT CS/DC/RES/BLK; 42/44 TFT SCL/SDA; 47 PN5180 IRQ; 48 onboard WS2812. Do not repurpose strapping pins 0/3/45/46, USB pins 19/20, or OPI pins 26-37. GPIO 8 remains free; GPIO 43 is the USB-serial TX pin.

## Enclosure

Mounting order is printed part, board, spacer, screw. Component names are embossed on the printed parts. The external WS2812 has no dedicated seat: solder its wires, test it, then glue it inside `OctoScaleLit.stl` behind the front opening. Use transparent filament for `OctoScaleLEDCover.stl`; sparse infill makes the diffuser cloudy.

Continue with the [Setup Guide](https://github.com/Ajimaru/OctoScale/wiki/Setup-Guide) for build, flash, and first-time configuration.

<!-- markdownlint-enable MD033 -->
