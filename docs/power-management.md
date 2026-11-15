# Power management

The default firmware scales the CPU between the 40 MHz crystal and 240 MHz.
Menus enable automatic light sleep only when the board buttons are released,
the save writer is idle, and no active USB or Bluetooth connection requires
the CPU to stay awake. GPIO holds preserve the battery latch, panel and audio
power controls during sleep. A one-second backstop checks for missed wakes.

Board buttons use interrupts and latched events. A button press wakes the menu
without being consumed by the wake operation. Gameplay and display errors wake
the paused emulator with task notifications instead of periodic polling.

In USB controller mode, the firmware parks an unused host and watches D+ for a
controller plug. It restores the USB pads before starting the host. An attached
device gets a bounded enumeration attempt; unplugging and reconnecting permits
another attempt.

USB PC mode remains a manual selection: a computer does not provide the same
D+ signal as a controller. Bluetooth, an active controller or a connected USB
serial console prevents menu sleep.

Audio hardware is off outside playback. Playback disables the unused ES8311
recording path and uses the measured line-out and low-power settings. Volume
selection and the RTC initialization order remain unchanged.

Grayscale completion overlaps panel scans. Drive scans upload only the rows
needed to update both controller RAM planes, including rows that need clearing.
The HOLD scan always uploads the full panel: pixels stay driven until it
activates, so its upload time is part of the calibrated gray dose.
Long SPI transfers wait for interrupts; short register and window transfers keep
the polling path. Emulator drawing skips frames that the 30 Hz display cannot
sample while retaining emulation and audio timing.

Host tests cover panel RAM contents through power cycles and faults, menu wake
conditions and scalar/parallel planner equivalence
through classic and fill-layout transitions. These tests complement device
checks on the board.
