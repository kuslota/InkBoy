# Codec source provenance

The ES8311 register driver, volume conversion helper, and required headers are
from Espressif's esp_codec_dev 1.3.4 (Apache-2.0), published on the ESP
Component Registry at
https://components.espressif.com/components/espressif/esp_codec_dev and
developed in https://github.com/espressif/esp-adf
(`components/esp_codec_dev`). The same bytes are in releases 1.3.4 to 1.4.0.

Five files differ from the release only in their `#include` lines, which use
relative paths so the files build from this folder: `es8311.c`,
`es8311_codec.h`, `audio_codec_if.h`, `audio_codec_ctrl_if.h` and
`esp_codec_dev_vol.c`. `es8311.c` also has one added clock coefficient row,
8000000 Hz MCLK at 31250 Hz, with the dividers of its 8192000 Hz / 32000 Hz
row. The two C implementation files are renamed to `.c.inc`
and compiled through `CodecDriver.c`. `SOURCE_MANIFEST.json` lists the release
path and the SHA-256 of every vendored file and its upstream original.

The source files identify Espressif Systems (Shanghai) CO LTD, copyright 2023,
and SPDX-License-Identifier: Apache-2.0. The accompanying license is
`LICENSE-2.0.txt`, which is identical to the `LICENSE` file in the release.
