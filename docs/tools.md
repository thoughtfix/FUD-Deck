# Tools for developers

Everything here is for working on FUD-Deck. None of it is needed to use it.

| Tool | Where | What it is for |
|---|---|---|
| Dev build | `firmware/`, env `div2-dev` | the firmware plus a serial remote control |
| `probe.py` | `tools/probe.py` | talks to the dev build or the probe: screenshots, keys, taps, logs |
| Probe firmware | `probe/` | read-only hardware bring-up for a new or unknown board |
| Face tracer | `firmware/tools/facevec/` | turns a face PNG into Zuse's vector data |
| Face preview | `firmware/tools/facepreview/` | draws every face on your computer as a PNG |
| Face check | `firmware/scripts/check_facevec.py` | build step: warns if face art changed without a re-trace |

Python: PlatformIO's own (`~/.platformio/penv/bin/python`) has pyserial, which is all
`probe.py` needs.

## Builds

From `firmware/`:

```
pio run -e div2              # release build
pio run -e div2-dev          # adds the serial remote control below
pio run -e div2-dev -t upload
```

Each board has a pin file in `firmware/src/board/boards/` and an env in `platformio.ini`.
To add a board, copy `esp32div_v2.h`, change the pins (one line each), and add an env that
defines its `FUD_BOARD_...` name. Nothing else in the firmware names a pin number.

## Serial remote control (dev build)

The `div2-dev` build reads commands on USB serial. Type them in any serial monitor at 115200,
or send them with `probe.py`. Each one answers `[dev] DONE <cmd>`.

| Command | Does |
|---|---|
| `key up\|down\|left\|right\|center\|back\|holdcenter` | press a button |
| `tap X Y`, `swipe up\|down\|right` | touch |
| `go <screen>` | open a screen by name (`go waterfall`, `go settings`, `go buttons`) |
| `state`, `post`, `heap` | current screen, the boot report, free memory |
| `shot` | the screen as image rows (use `probe.py shot` to get a PNG) |
| `fake tracker\|camera\|drone\|subghz\|hackdev\|attack\|jam` | inject a detection, to test screens and alerts |
| `face <name>`, `say <text>`, `toast <text>`, `anim spin\|sweep` | show Zuse states on Home |
| `facebench` | time every face drawn at 162 px |
| `irtest` | a fake decoded remote on the IR Decoder |
| `time <epoch> [zone]` | set the clock |
| `sdhead <file> [lines]`, `sdtail`, `sdrm <file>`, `sdbench` | read and test the SD log |
| `sdformat yes` | format a card that has no readable FAT filesystem (erases it) |
| `ipregs` | the IP5306 charger's registers |
| `reboot` | restart |

Nothing in the remote control transmits.

## probe.py

```
probe.py ports                              # list serial ports
probe.py shot out.png                       # screenshot to PNG
probe.py <cmd> [args]                       # send one command, print until DONE
probe.py serve LOG FIFO                     # hold the port open and log everything
probe.py send FIFO LOG <cmd> [args]         # send through a running serve
probe.py shotlog FIFO LOG out.png           # screenshot through a running serve
```

`serve` is the comfortable way to work: it keeps the port open (opening it can reset the
board on macOS), appends everything the board says to LOG, and sends each line written to the
FIFO. Create the FIFO with `mkfifo` first. Screenshots for the README were taken this way.

## Probe firmware

`probe/` is a separate firmware for finding out what a board really is: I2C devices, pins,
radios, SD, display, touch, buttons, LEDs, battery sensing. It only reads and receives: the
nRF24s use carrier detect, the CC1101 listens, Wi-Fi and BLE scans are passive, IR only
receives. Every pin in `esp32div_v2.h` was confirmed with it.

```
cd probe && pio run -t upload
probe.py auto          # every automatic test
probe.py guided        # buttons, screen, touch, LEDs, IR, SD, with you answering prompts
probe.py help          # all commands
```

Start here when bringing up a new board, before writing its pin file.

## Faces

Zuse's faces are vectors drawn at any size with no bitmaps. To add or change one, see
[firmware/tools/facevec/README.md](../firmware/tools/facevec/README.md):

```
firmware/tools/facevec/trace_face.py my-face.png -o out/   # one PNG > SVG + JSON
firmware/tools/facevec/run.sh                              # every PNG in art/ > the firmware
firmware/tools/facepreview/run.sh                          # contact sheet of all faces
```

## More

- [Hardware notes](hardware-notes.md): ESP32-DIV V2 quirks and what the firmware works around.
- [For developers](for-developers.md): flash and RAM budget, radio etiquette for forks.
- [Backport to FUD](backport-to-fud.md): changes that belong in the other FUD releases.
