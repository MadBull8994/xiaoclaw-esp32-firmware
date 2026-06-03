# OpenClaw V2.7 Board Profile

Target hardware: ESP32-S3-N16R8, 1.8 inch 8-pin ST7735 TFT, I2S microphone,
I2S speaker amplifier, BOOT / VOL+ / VOL- buttons.

Pin map:

| Function | GPIO |
| --- | --- |
| TFT SCK | GPIO21 |
| TFT SDA/MOSI | GPIO47 |
| TFT RST | GPIO45 |
| TFT DC | GPIO40 |
| TFT CS | GPIO41 |
| TFT BL | GPIO42 |
| BOOT | GPIO0 |
| VOL+ | GPIO38 |
| VOL- | GPIO39 |
| MIC WS | GPIO4 |
| MIC SCK | GPIO5 |
| MIC DIN | GPIO6 |
| SPK DOUT | GPIO7 |
| SPK BCLK | GPIO15 |
| SPK LRCK | GPIO16 |

Build:

```bash
source /Users/mashiyue/esp/esp-idf-v5.3.2/export.sh
python scripts/release.py openclaw-v2.7
```
