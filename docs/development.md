# Development and releases

Host tests compile the real firmware sources against fakes and need no hardware. They use cJSON from `managed_components/`, so run `idf.py reconfigure` once before the first run:

```sh
tools/run_tests.sh
python3 tools/check_safety.py --config sdkconfig
```

`check_safety.py` fails if the display driver's command set changes. See [display command checks](how-it-works.md#what-the-safety-check-guards).

`tools/run_tests.sh` also runs `components/esp32-ws154/check-host.sh`. The module's tests need only `cc` and `python3`, and `check_safety.py` here wraps `components/esp32-ws154/tools/check_safety.py` over every source in the tree.

## Release images

Activate ESP-IDF and use a clean tree with the submodule at its committed revision. Explicitly disable diagnostics first: the release script builds with the existing build configuration.

```sh
idf.py -DINKBOY_DIAGNOSTICS=OFF reconfigure
tools/make_release.sh <version>
```

The script builds, runs host tests and the safety check, and writes `dist/inkboy-<version>.bin` (merged image at 0x0), `dist/inkboy-<version>-app.bin` (app only at 0x10000) and `dist/SHA256SUMS`. It stops on failure. See [flashing](flashing.md) for which image to use.

## Dependencies

The emulator core is the `components/esp32-gameboy` git submodule. Clone with `--recursive`. ESP-IDF downloads `espressif/cjson` and `espressif/usb` on first configure; versions are pinned in `dependencies.lock`.

For repeated flashing and serial logging, select Bluetooth so USB stays available across resets. See [flashing and recovery](flashing.md).

## Diagnostic build

`idf.py -B build-test -DINKBOY_DIAGNOSTICS=ON build` enables test-only USB input
commands (`ib boot 100`, `ib pwr 850`, `ib controller 00`, `ib usbstate`).
Use this only for development tests. The ordinary production build excludes
the diagnostic console and simulated input; explicitly build with
`-DINKBOY_DIAGNOSTICS=OFF` when returning to production.

## Menu display handover

Either board button pauses on its debounced press during gameplay. The emulator
and audio stop, the live panel context passes to the menu, and the existing
battery-save/checkpoint path runs before Home appears. Home rendering can use
fast updates rather than a game-exit clean followed by a menu-entry clean.
The menu refresh counter starts anew after continuous gameplay and retains its
bounded lifetime policy.

## Verification

Host checks cover GPIO debounce, tap/hold/release, held wake, chords, menu event
routing, safe confirmations, board-only decrease editing, framebuffer layout,
partial mapping, failed mapping commits, Settings persistence/rollback, game
handover followed by Home rendering, and existing save/checkpoint behavior.
The SSD1681 command/voltage safety check passes.

### Hardware verification

V2-board checks covered settings navigation, mapping without full cleans, button pause/save, checkpoint reload and gameplay in both display modes. Observed command-to-Home times were 0.38 s grayscale and 0.34 s dithered. These timings are measurements from that build, not guarantees for every configuration.

The manual USB toggle was confirmed to show USB: PC and restore the Mac serial port. Opening the serial capture then reset the board and restored the saved USB-controller mode. Controller reconnect after the manual toggle still needs hardware verification.
