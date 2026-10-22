# Tuning and presets

InkBoy has two display modes, and each has its own settings. This page lists every setting, what you will see when you change it, and the range the firmware accepts. It then covers presets and the transition test. For background on what a dose or a HOLD is, read [how-it-works.md](how-it-works.md) first.

Successful setting changes apply immediately and persist in the board’s flash. They do not overwrite an SD preset file until you choose Save or Save as new. Grayscale adjustments apply without a full refresh. Changing modes or dithered pulse settings establishes a clean reference.

## Settings menu

Open SETTINGS from Home. It contains Display, Controller, Sound and FPS.
See [menu controls](ui-controls.md) for navigation and button mapping.

| Group | Contents |
| --- | --- |
| Display | Mode, Border, Preset, Tuning; grayscale also has Transition test |
| Controller | Connection (Bluetooth / USB), Map buttons, for either transport |
| Sound | Game sound On / Off, Volume 1–10 (Left lowers, Right or A raises) |
| FPS | Show FPS On / Off during gameplay |

Display → Border sets what surrounds the game during play: **Black logo** (default,
the original black bezel with the INKBOY logo), **White**, **White logo**, or
**Fill screen**, which scales the image 1.25× to 200×180 by doubling every fourth
pixel and row. Fill screen shows the FPS counter in small text in the bottom strip.

Volume steps are 3 dB: 6 is the original fixed level and 10 is the codec's unity
gain. A change takes effect when the game next resumes.

Display → Tuning contains Black/White pulses in dithered mode, and the grayscale
controls below in grayscale mode. Switching modes establishes a clean display
reference. For USB switching and flashing, see [USB access](flashing.md#usb-controller-mode).

The active settings of both modes are stored in the board's flash, so they work without an SD card. A `*` after the preset name in the PRESET row means you changed something after loading or saving that preset.

## Dithered settings

| Setting | Range | Default | Effect |
| --- | --- | --- | --- |
| Black pulses | 1 to 8 | 8 | How many scans a pixel is driven when it changes to black |
| White pulses | 1 to 8 | 8 | The same for pixels changing to white |

Each scan is a pulse of drive of about 20 ms. A pixel that keeps changing is restarted each time.

- Higher numbers give deeper blacks and whites and clear ghosts and trails better, but a moving object stays in a half-changed state longer, so motion looks softer.
- Lower numbers look sharper and snappier in motion, but contrast is lower and menus and sprites leave more ghosting.
- A good way to tune is to start from 8 and 8, and lower one number at a time while watching a scrolling background. Black and white do not have to match.

## Grayscale settings

Select Fast grayscale in **Settings → Display → Mode**, then open **Settings → Display → Tuning**. The firmware accepts every value in the ranges below. Moving one value at a time and testing it is far easier than changing several. The Default is a good place to start.

### Doses

LIGHTEN DOSES lists the six transitions that make a pixel lighter (black to dark, black to light, black to white, dark to light, dark to white, light to white). DARKEN DOSES lists the six that make it darker. For each transition you can set:

| Setting | Range | Effect |
| --- | --- | --- |
| Dose (for example `BLACK > LIGHT: 4`) | 1 to 12 | The number of drive pulses sent for this transition |
| HOLD (for example `BLACK > LIGHT HOLD: ON`) | ON or OFF | Whether the transition ends right after its drive scan. Only active in pair hold mode |

A dose that is too low leaves the pixel short of its shade. A dose that is too high overshoots, and the pixel ends up too far toward the target. Doses are whole pulses, so each step is a visible change. Larger doses take more frames to finish, which blurs motion. Same-shade entries do not exist: unchanged pixels are always held.

HOLD ON suits the gray targets, where the pulse should end at once. HOLD OFF lets a transition take a second dose in the same slot. The Default turns HOLD off for transitions to black or white, and for white to dark gray. White to light gray has HOLD on.

### Other grayscale settings

| Setting | Range | Default | Effect |
| --- | --- | --- | --- |
| RETARGET DOSE | 50% to 150%, steps of 25 | 100% | How much of a transition's dose is used when a pixel changes target before it finished. Lower is less correction, higher is more |
| SCANS/FRAME | AUTO 1-2 or 1 | AUTO | AUTO adds a second scan to a frame when there is time. 1 always uses one scan. Shows PAIR, and has no effect, while pair hold mode is on |
| LIGHTEN | STOCK -15V, WEAKER -13V, -11V, -9V | STOCK | The strength of the pulses that lighten a pixel. Weaker pulses move a pixel less per dose, which gives finer steps but needs more doses to reach the same shade. Change doses along with it |
| PAIR HOLD MODE | ON or OFF | ON | ON sends a drive scan plus a follow-up scan in every frame and uses the HOLD flags. OFF follows SCANS/FRAME and ignores the HOLD flags |

### History and selective drive

**Settings → Display → Tuning → History / Selective** (grayscale mode). These settings correct for the history a pixel has: a pixel that has been to black and come back is not the same as one that never left.

| Setting | Range | Default | Effect |
| --- | --- | --- | --- |
| HISTORY | OFF, RETURNS, REVERSALS, BOTH | BOTH | OFF disables the route corrections below. RETURNS corrects a return after the first drive finished. REVERSALS corrects a return after the first drive was cut short. BOTH does each in its own case |
| SETTLED W>L | NORMAL, 11V, 8.8V | NORMAL | Weakens the darkening pulses for light gray when the pixel was just settled at white. It can fix a dark stain behind a moving white object, but weaker values take longer, so check the frame rate with the FPS counter |
| Route controls | see below | see below | One page for each of six round trips |
| RESET THESE OPTIONS | | | Restores this page (and all routes) to the Default. Doses and HOLD flags are unchanged |

Each route page is a round trip that starts and ends on the same shade. In the notation `L > K > L`, a pixel starts light gray, goes to black and comes back.

| Control | Range | Effect |
| --- | --- | --- |
| COMPLETED DOSE | -3 to +3 | Added to the return dose after a finished round trip. Needs HISTORY to be RETURNS or BOTH. 0 changes nothing |
| REVERSE DOSE | BASE, or 50% to 200% in steps of 25 | Scales the return dose after an unfinished trip. Needs REVERSALS or BOTH. BASE follows RETARGET DOSE |

The routes are `L > K > L`, `D > K > D`, `D > W > D`, `L > W > L`, `D > L > D` and `L > D > L` (K black, D dark gray, L light gray, W white). The Default has `L > K > L` set to -1 and 150% and leaves the other five neutral (0 and BASE).

The corrections work like this: a return that goes toward white (from black or from dark to light) gets lighter with a higher number and darker with a lower one. A return that goes toward black behaves the opposite way. Judge the track behind a moving rectangle: it should match the background beside it. Doses are whole pulses, so neighboring percentages can round to the same count.

## The transition test

**Settings → Display → Transition test** (grayscale mode; also available as Run transition test inside tuning) is the main way to judge a tuning. It shows all 12 transitions at once.

The screen has four horizontal bands whose backgrounds are black, dark gray, light gray and white, from top to bottom. The labels on the left show each background. Each band holds three 12-pixel rectangles, one for each of the other three shades, labeled on the right edge. All rectangles sweep left to right over 5 seconds, wait 1 second at the right end, then repeat.

| Input | Action |
| --- | --- |
| A or BOOT tap | Pause or resume the sweep |
| Start or PWR tap | Replay the sweep from the beginning |
| Controller Select | Clean the screen and restart |
| B, or PWR hold | Leave the test |

The screen is cleaned when you enter, so each run starts from the same reference.

What to look for:

1. **The rectangle's shade.** It should match the same shade where it appears as a background or label. If it is too light or dark, the dose for that transition is off.
2. **The track behind it.** Pause with A. The strip it left should match the band's background. A pale strip means the return came back too light, a dark strip too dark. This is where history settings matter.
3. **Buildup.** Let it run several passes. Mottling, texture or a slow drift in the background means the doses are putting too much or too little into a shade and the error accumulates.
4. **Endpoints.** Black and white rectangles on gray bands should stay solidly black or white. If a change helps one lane but weakens an endpoint, undo it.
5. **Motion blur.** A bigger dose may fix a still shade and still blur the moving edge. Judge both.

Change one setting between runs and look at the same lane. After you find a good setting, test it in a game with thin, fast sprites, because the test uses wide rectangles that mostly exercise finished trips.

## Presets

A preset stores the settings of one display mode. FAST DITHERED presets store the two pulse counts. FAST GRAYSCALE presets store everything in the grayscale pages above. Each mode has a built-in Default that cannot be changed, plus any presets you keep as JSON files on the SD card.

Open **Settings → Display → Preset**:

| Item | What it does |
| --- | --- |
| LOAD PRESET | Lists DEFAULT, then up to 16 presets from the card sorted by name. Loading applies at once |
| SAVE AS NEW | Writes a new file named `user-N.json` (preset name `User N`, with the lowest free number) and makes it active |
| SAVE | Appears when the active preset is one of yours and has unsaved edits. It asks to confirm, overwrites the file and keeps the old version as `user-N.json.bak` |
| RESTORE DEFAULT | Replaces the current settings with the built-in Default |

Presets are stored at:

```text
/inkboy/presets/dithered/*.json
/inkboy/presets/grayscale/*.json
```

The folders are created the first time you use SAVE AS NEW. The name `Default` is reserved, so a card file with that name is skipped.

### Sharing presets

A preset is one small text file. To share one, send the file. To use one from someone else, put it in the folder of its mode on the card and pick it in LOAD PRESET. You can also edit a file by hand. Copy `presets/grayscale/default.json` from this repository, change its `"name"` and the values, and save it under a new file name. Files that cannot be used are ignored and logged when USB serial is available. Unknown keys are ignored.

`presets/dithered/default.json` and `presets/grayscale/default.json` are exactly what the firmware writes for its Defaults. They are regenerated with `python3 tools/export_default_presets.py`, and a test fails if they drift from the firmware.

### File format

Both modes use the same envelope:

```json
{
  "format": "inkboy-preset-v1",
  "mode": "dithered",
  "name": "Default",
  "settings": { "black_pulses": 8, "white_pulses": 8 }
}
```

| Field | Rules |
| --- | --- |
| `format` | Always `inkboy-preset-v1` |
| `mode` | `dithered` or `grayscale`. A file is only read from the folder of its own mode |
| `name` | 1 to 20 printable ASCII characters. No quotes, backslashes, or leading or trailing spaces. `Default` is reserved |

A file larger than 8 KiB is ignored. Every key below is required.

Dithered `settings`:

| Key | Values | Menu row |
| --- | --- | --- |
| `black_pulses`, `white_pulses` | integers 1 to 8 | BLACK PULSES, WHITE PULSES |

Grayscale `settings`:

| Key | Values | Menu row |
| --- | --- | --- |
| `doses` | An object keyed by the starting shade `K`, `D`, `L`, `W`. Each value is an array of 4 integers in the order K, D, L, W for the target shade. The entry for the same shade is 0. Every other entry is 1 to 12 | The dose of each transition |
| `retarget_percent` | 50, 75, 100, 125 or 150 | RETARGET DOSE |
| `scan_policy` | `auto` or `one` | SCANS/FRAME |
| `strength` | `stock`, `-13V`, `-11V` or `-9V` | LIGHTEN |
| `pair_hold` | `true` or `false` | PAIR HOLD MODE |
| `holds` | An object keyed like `doses`. Each value is an array of 4 booleans, `true` for the same shade | HOLD flags |
| `history` | `off`, `returns`, `reversals` or `both` | HISTORY |
| `weak_white_to_light` | `normal`, `11V` or `8.8V` | SETTLED W>L |
| `routes` | An object keyed `L>K>L`, `D>K>D`, `D>W>D`, `L>W>L`, `D>L>D`, `L>D>L`. Each value is `{ "return": -3 to 3, "reverse_percent": 50 to 200 in steps of 25, or null }`. `null` follows the retarget dose | Route controls |

The grayscale Default is the author's own tuning for the author's panel. Panels vary, so your best settings may differ.
