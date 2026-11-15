# Third-party notices

InkBoy's own code is MIT-licensed (see `LICENSE`). This file lists everything
else that is in the repository or is fetched when the firmware is built, with
its license and where the license text lives.

## Inspiration

InkBoy's per-pixel e-paper driving approach was inspired by Wenting Zhang
(Zephray)'s PaperBoy, https://gitlab.com/zephray/paperboy. No PaperBoy code is
included in InkBoy. The emulator core was originally obtained through
PaperBoy's port; the provenance is recorded in
`components/esp32-gameboy/THIRD_PARTY_NOTICES.md`.

## In this repository

| Component | Location | License | Required notice |
| --- | --- | --- | --- |
| esp_codec_dev 1.3.4 (ES8311 driver and volume helper) | `components/esp32-ws154/components/game_audio/vendor/` | Apache-2.0 | Keep the per-file SPDX headers (Espressif Systems (Shanghai) CO LTD, 2023) and `LICENSE-2.0.txt`. Five files have their `#include` paths changed; `es8311.c` also has one added clock coefficient row. Provenance and file hashes are in `NOTICE.md` and `SOURCE_MANIFEST.json` in the same folder. Upstream: https://components.espressif.com/components/espressif/esp_codec_dev (source: https://github.com/espressif/esp-adf, `components/esp_codec_dev`) |
| `esp_hid_gap.c` and `esp_hid_gap.h` (from the ESP-IDF `esp_hid_host` example) | `components/ble_gamepad/vendor/` | Unlicense OR CC0-1.0 | None; the SPDX headers are kept. `esp_hid_gap.c` is locally modified (the scan-result print is silenced); `esp_hid_gap.h` is unchanged. Details in `components/ble_gamepad/README.md`. |
| Big2Small v1.0.0 by Matthew D. Steele | `roms/big2small.gb` | GPL-3.0-or-later | Official upstream ROM, unmodified. Licence and complete source for release commit `ac7f03b2bee1a850a427443999db2057bc426854` are provided in `roms/big2small/`; see [sample game](roms/README.md). This separate game retains its own licence. |
| SSD1681 waveform register tables | `components/esp32-ws154/components/ssd1681_fast/ssd1681_vendor_tables.h` | MIT-style permission notice in the source file header (see below) | The two 159-byte arrays are byte-identical to `WF_Full_1IN54` and `WF_PARTIAL_1IN54_0` in Waveshare's e-Paper library, https://github.com/waveshareteam/e-Paper, files `RaspberryPi_JetsonNano/c/lib/e-Paper/EPD_1in54_V2.c` and `Arduino/epd1in54_V2/epd1in54_V2.cpp`. The notice below must accompany copies. |

### Waveshare e-Paper library notice

The files above carry this header (Author: Waveshare team, 2019). The repository
has no top-level LICENSE file and the header has no copyright line; the
permission notice is reproduced exactly as it appears, including its spelling,
with the leading `#` comment markers removed:

> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documnetation files (the "Software"), to deal
> in the Software without restriction, including without limitation the rights
> to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
> copies of the Software, and to permit persons to  whom the Software is
> furished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in
> all copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
> FITNESS OR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
> LIABILITY WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
> THE SOFTWARE.

## Fetched at build time

These are downloaded by the ESP-IDF component manager into
`managed_components/` (not committed). Versions are pinned in
`dependencies.lock`. Each package carries its own `LICENSE`, which must be
kept with any redistributed copy of the source; the compiled firmware image
contains object code from them.

| Component | Version | License | Where the license is |
| --- | --- | --- | --- |
| espressif/cjson (cJSON by Dave Gamble and contributors) | 1.7.19~2 | MIT | `managed_components/espressif__cjson/LICENSE` |
| espressif/usb (USB Host library) | 1.5.0 | Apache-2.0 | `managed_components/espressif__usb/LICENSE` |

## Git submodule

| Component | Location | License | Notice |
| --- | --- | --- | --- |
| esp32-gameboy | `components/esp32-gameboy` | MIT (`LICENSE`) | Its own `THIRD_PARTY_NOTICES.md` covers CrankBoy (MIT, which also carries the Peanut-GB and SameBoy copyrights), Peanut-GB and the MiniGB APU (MIT), and SameBoy snippets (MIT). Those notices must travel with any copy of the emulator core. |

## ESP-IDF

The firmware is built with ESP-IDF v6.1 (Apache-2.0, Espressif Systems
(Shanghai) CO LTD), https://github.com/espressif/esp-idf. ESP-IDF is not
vendored here. A released firmware image links ESP-IDF and its bundled third-party
libraries, each under its own license in the ESP-IDF tree.
