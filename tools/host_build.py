"""Compile and run a C test against the real module sources and tools/host_fakes."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FAKES = ROOT / 'tools/host_fakes'
INCLUDES = [FAKES, ROOT / 'main', ROOT / 'components/ssd1681_fast', ROOT / 'components/ssd1681_fast/include',
            ROOT / 'components/game_audio/include', ROOT / 'components/ble_gamepad/include', ROOT / 'components/esp32-gameboy/include',
            ROOT / 'components/esp32-gameboy/src/gb_core', ROOT / 'components/esp32-gameboy/src/storage',
            ROOT / 'managed_components/espressif__cjson/cJSON']
LIBRARY_FAKES = ['host_gpio.c', 'host_heap.c', 'host_log.c', 'host_nvs.c', 'host_reset.c', 'host_rtos.c', 'host_spi.c']
# The display path as one unit: menus, gameplay and the fast backend over the fake panel.
DISPLAY_MODULES = ['fast_display.c', 'game_view.c', 'gray_plan.c', 'gray_plan_parallel.c', 'drive_counters.c', 'screen_draw.c',
                   'screen.c', 'display_config.c', 'latest_frame.c']
SESSION_FAKES = ['host_panel.c', 'fake_input.c', 'fake_menu.c', 'fake_game_audio.c',
                 'fake_gb_emulator.c', 'fake_game_storage.c']
GB_VIDEO = ROOT / 'components/esp32-gameboy/src/gb_core/gb_video.c'
SAVE_FILE = ROOT / 'components/esp32-gameboy/src/storage/save_file.c'
# Fetched by the component manager; run `idf.py reconfigure` once on a fresh checkout.
CJSON = ROOT / 'managed_components/espressif__cjson/cJSON/cJSON.c'
PRESET_STORE_MODULES = ['display_preset.c', 'gray_profile.c', 'preset_json.c', 'preset_store.c']


def run(test_source, modules=(), fakes=(), optimize='-O1', extra_sources=(), vendor_sources=(), scenarios=((),)):
    """Build test_source with main/<module> and host_fakes/<fake> sources, then run it.

    modules: file names under main/ compiled as separate translation units.
    fakes: extra fake file names under tools/host_fakes (module-level stand-ins).
    vendor_sources: third-party files, compiled without warnings so -Werror stays on our code.
    A test may instead #include a main/*.c file to reach its static state.
    scenarios: argument lists; the binary runs once per list so each run starts
    with fresh static state, and with an empty directory named by $HOST_TEST_DIR.
    """
    with tempfile.TemporaryDirectory(prefix='host-test-') as directory:
        work = Path(directory)
        (work / 'test.c').write_text(test_source)
        sources = [work / 'test.c', *(ROOT / 'main' / m for m in modules),
                   *(FAKES / f for f in (*LIBRARY_FAKES, *fakes)), *extra_sources]
        includes = [f'-I{p}' for p in INCLUDES]
        vendor_objects = []
        for source in vendor_sources:
            if not Path(source).exists(): sys.exit(f'{source} is missing; run `idf.py reconfigure` to fetch managed components')
            vendor_objects.append(str(work / f'{Path(source).stem}.o'))
            if subprocess.run(['cc', '-std=c11', '-w', '-fsanitize=address,undefined', optimize, *includes, '-c', str(source),
                               '-o', vendor_objects[-1]]).returncode: sys.exit('host test failed to compile')
        compile_ = subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', optimize,
                        *includes, *map(str, sources), *vendor_objects, '-o', str(work / 'test'), '-lm'])
        if compile_.returncode: sys.exit('host test failed to compile')
        for index, arguments in enumerate(scenarios):
            scratch = work / f'scratch{index}'
            scratch.mkdir()
            result = subprocess.run([str(work / 'test'), *arguments], timeout=120,
                                    env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0', 'HOST_TEST_DIR': str(scratch)})
            if result.returncode: sys.exit(f'host test {" ".join(arguments) or "run"} failed ({result.returncode})')
