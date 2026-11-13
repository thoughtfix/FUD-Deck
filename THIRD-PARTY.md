# Third-party code and assets

Anything taken whole from another project is listed here with its source and license.
Libraries pulled in by PlatformIO (not copied into this repo) are listed separately.

## Copied into this repo

| What | Where here | Source | License |
|------|-----------|--------|---------|
| Zuse face image (one example, `defaultsmile.png`) | `firmware/tools/facevec/art/` | FUD-Firmware by thoughtfix, `images/` | GPL-3.0 |
| Zuse vector faces (traced from the PNGs) | `firmware/src/ui/facevec_data.h`, `firmware/images/vectors/` | FUD-Firmware by thoughtfix, `images/` | GPL-3.0 |
| Detection store, alert tiers, ack/ignore/snooze, eviction (ported, restructured) | `firmware/src/core/detections.*` | FUD-Firmware `src/main.cpp` | GPL-3.0 |
| CC1101 433 MHz carrier-sense setup and thresholds (ported, extended) | `firmware/src/core/rfwatch.cpp` | FUD-Firmware `src/main.cpp` | GPL-3.0 |
| BLE / Wi-Fi / deauth scanner logic (ported) | `firmware/src/core/scanner.cpp` | FUD-Firmware `src/main.cpp` | GPL-3.0 |
| Detection signatures (FUD's, plus rows tagged `BACKPORT(FUD)`) | `firmware/src/core/signatures.h` | FUD-Firmware `src/signatures.h` (includes a subset of SquachWatch-CYD signature data, cross-checked with OUI-SPY); the tagged rows are from SquachWatch-CYD `src/signatures.cpp`, 2026-10-14 to 2026-11-03 | GPL-3.0 |
| User ignore list template (verbatim, one comment line edited) | `firmware/src/core/user_ignore.h` | FUD-Firmware `src/user_ignore.h` | GPL-3.0 |
| SD detection log format and hot-swap design (adapted) | `firmware/src/core/sdlog.*` | FUD-X3 `src/storage/Logger.*`, `SdStore.*` | GPL-3.0 |
| TV-B-Gone power-code set (facts: protocol + code) and Finder smoothing | `firmware/src/ui/toys.cpp` | FUD-Firmware `src/main.cpp` | GPL-3.0 |
| Waterfall sub-GHz sweep method (ported into the radio task) | `firmware/src/core/rfwatch.cpp` | FUD-Firmware `src/main.cpp` (ccSweepWF) | GPL-3.0 |
| Zuse face selection and idle animations (ported) | `firmware/src/core/zuse.*` | FUD-Firmware `src/main.cpp` (mood ideas credited there to Pwnagotchi and M5PORKCHOP) | GPL-3.0 |

Because `signatures.h` carries SquachWatch-CYD's GPL-3.0 data, FUD-Deck is GPL-3.0
(it follows FUD).

## Libraries (not copied, fetched at build time)

| Library | Used by | License |
|---------|---------|---------|
| LovyanGFX | firmware, probe | FreeBSD (BSD-2-Clause) |
| RF24 (nRF24/RF24) | probe | GPL-2.0 |
| Arduino-ESP32 core | firmware, probe | LGPL-2.1 |
| NimBLE-Arduino | firmware | Apache-2.0 |
| IRremoteESP8266 | firmware | LGPL-2.1 |

## Not code, but credit

- FUD-Deck is a port of FUD-Firmware (thoughtfix, GPL-3.0) to new hardware.
- The clock's idea of being honest about set, trusted and guessed time comes from
  SquachWatch-CYD (Talking Sasquach). The code and zone table in `clock.*` are our own.
- Hardware: ESP32-DIV V2 (CiferTech). Only the published schematics were used as a starting
  point, and every pin was re-measured with `tools/probe`. No ESP32-DIV firmware code is used.
- Serial screenshot wire format (RGB332 rows) matches the SquachWatch-CYD dev-tools by
  thoughtfix, reimplemented here.
