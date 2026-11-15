#!/usr/bin/env python3
"""Pre-flash audit of InkBoy: the display module's panel check over every C source in the tree."""
import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--config', type=Path, default=ROOT / 'sdkconfig')
args = parser.parse_args()
MODULE = ROOT / 'components/esp32-ws154'
# The module's own tests fake the SPI driver; only its firmware sources are audited.
directories = [ROOT / 'main', *(MODULE / 'components' if child == MODULE else child
                                for child in sorted((ROOT / 'components').iterdir()) if child.is_dir())]
scan = [arg for directory in directories for arg in ('--scan', str(directory))]
sys.exit(subprocess.run([sys.executable, str(ROOT / 'components/esp32-ws154/tools/check_safety.py'),
                         '--config', str(args.config), *scan]).returncode)
