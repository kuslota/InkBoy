#!/usr/bin/env python3
"""Fail closed on changes to the reviewed SSD1681 command surface."""
import re
import argparse
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--config', type=Path, default=ROOT / 'sdkconfig')
args = parser.parse_args()

EXPERIMENTAL_FRAME_TX = 'CONFIG_SSD1681_EXPERIMENTAL_INTERRUPT_FRAME_TX'


def check_ssd1681_config(text):
    """Allow only the reviewed default-off experiment; fail closed otherwise."""
    entries = [line.strip() for line in text.splitlines()
               if line.strip().startswith(('CONFIG_SSD1681_', '# CONFIG_SSD1681_'))]
    if not entries:
        return
    assert len(entries) == 1, f'Unexpected display configuration entries: {entries}'
    entry = entries[0]
    allowed = {
        f'{EXPERIMENTAL_FRAME_TX}=y',
        f'{EXPERIMENTAL_FRAME_TX}=n',
        f'# {EXPERIMENTAL_FRAME_TX} is not set',
    }
    assert entry in allowed, f'Unexpected display configuration option/value: {entry}'


source = (ROOT / 'components/ssd1681_fast/ssd1681_fast.c').read_text()
source = re.sub(r'/\*.*?\*/|//[^\n]*', '', source, flags=re.S)
expected = {0x01, 0x10, 0x11, 0x12, 0x18, 0x20, 0x22, 0x24,
            0x26, 0x32, 0x3c, 0x44, 0x45, 0x4e, 0x4f}
vendor_commands = {0x03, 0x04, 0x2c, 0x37, 0x3f}
commands = dict((name, int(value, 16)) for name, value in
                re.findall(r'(CMD_\w+)\s*=\s*(0x[0-9a-fA-F]+)', source))
# The additional surface must remain privately fixed.
assert set(v for name, v in commands.items() if name.startswith('CMD_VENDOR_')) == vendor_commands
expected |= vendor_commands
assert set(commands.values()) == expected, 'Command enum changed: review required'
guard = source.split('switch (command)', 1)[1].split('default:', 1)[0]
assert set(re.findall(r'case (CMD_\w+):', guard)) == set(commands), 'Whitelist mismatch'
assert 'default: return ESP_ERR_NOT_SUPPORTED;' in source
assert source.count('transfer(d, false,') == 1, 'Raw command path changed'
assert source.count('reg_write(d, CMD_LUT,') == 1, 'LUT upload must stay in the fixed vendor loader'
assert 'load_lut' not in (ROOT / 'components/ssd1681_fast/include/ssd1681_fast.h').read_text(), 'General LUT API returned'
for path in list((ROOT / 'main').glob('*.c')) + list((ROOT / 'components').rglob('*.c')):
    code = re.sub(r'/\*.*?\*/|//[^\n]*', '', path.read_text(), flags=re.S)
    if 'vendor' not in path.parts:
        assert not re.search(r'\b0x30\b', code, re.I), f'Forbidden literal in {path}'
    # Vendored BLE scan intervals may equal 0x30; only the panel driver owns SPI.
    if path.name != 'ssd1681_fast.c':
        assert not re.search(r'\bspi_device_(polling_transmit|transmit|queue_trans)\s*\(', code), f'Panel transport bypass in {path}'
    assert not re.search(r'\besp_efuse_\w+\s*\(', code), f'eFuse call in {path}'
config = args.config
if config.exists():
    text = config.read_text()
    for setting in ('CONFIG_SECURE_BOOT', 'CONFIG_SECURE_FLASH_ENC_ENABLED'):
        assert setting + '=y' not in text, f'Baseline must not enable {setting}'
    assert 'CONFIG_IDF_TARGET="esp32s3"' in text
    check_ssd1681_config(text)
else:
    raise SystemExit(f'Missing configuration: {config}; build before pre-flash audit')
print('PASS: reviewed command whitelist; no OTP/eFuse path; fixed VSL and bounded weak-W>L waveform/VSH2 presets; stock full profile; no raw voltage API')
