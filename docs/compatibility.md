# Compatibility and limitations

- Game Boy (DMG) games only, plus dual-mode cartridges played in DMG mode. Color-only cartridges are rejected.
- Supported cartridge types: ROM only, MBC1, MBC2, MBC3 (including the clock cartridges used by Pokémon Gold and Silver; Crystal is Color-only), MBC5 without rumble.
- Clock cartridges follow the board's PCF85063 real-time clock, so in-game time keeps passing while the game is paused or the board is powered off. If the RTC lost its time (first power-up, or the battery ran flat), it restarts and that gap isn't counted. With no usable clock, game time only advances during play.
- A clock cartridge's `.sav` is cartridge RAM followed by the 48-byte clock footer that BGB and VBA-M use, so saves move between them. Saves without a footer, or with the older 44-byte one, also load.
- ROMs from 32 KiB to 2 MiB.
- Only the InkBoy controller is supported. Other Bluetooth or USB gamepads are not.
- Heavy scenes can drop slightly below 30 fps, mostly in grayscale mode.
- Grayscale mode still leaves faint trails after some shade changes. Pausing and restarting gameplay establishes a fresh display reference.
- Custom display driving is experimental; see [Experimental display drive](../README.md#experimental-display-drive). The software checks do not establish long-term panel safety.
