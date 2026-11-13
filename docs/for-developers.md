# For developers building on FUD-Deck

## Room left for forks

Measured on the div2-dev build (November 2026):

| | Used | Free |
|---|---:|---:|
| Flash (one 6.25 MiB app slot, OTA keeps two) | 1.46 MB | about 4.9 MB |
| Heap, BLE + Wi-Fi scanner running | | about 78 KB (largest block about 37 KB) |
| Heap, scanner paused (quiet screens) | | about 185 KB |

Flash is not the limit. RAM is. FUD-Deck's own code is only about 104 KB of the image; the
Wi-Fi stack (about 500 KB) and Bluetooth (about 290 KB) are most of it, and a fork reuses them.

## Wardriving (after adding a GPS)

Expect roughly 30 to 60 KB of flash: a GPS parser and an export format (WiGLE CSV, for
example). Wi-Fi scanning and SD logging are already there.

Turn off every passive scan other than Wi-Fi and the GPS while wardriving: BLE, the nRF24s
and the CC1101. That frees the radio time and the heap the Wi-Fi scan and the log need.

## Attacks (like stock ESP32-DIV V2 firmware)

Expect roughly 100 to 300 KB of flash. The Wi-Fi and BLE stacks are already linked.

Turn off any radio or scan an attack does not need while it runs. Use the same hooks
FUD-Deck uses for its quiet screens:

- `scanner::pause(true)`: BLE and Wi-Fi scanning off, stacks shut down, about 100 KB of heap back.
- `rfwatch::setFocus(...)`: `Nrf`, `Cc` or `None` powers down the SPI radios a screen does not use.

In `src/ui/ui.cpp`, `quiet()` and `rfFocus()` decide this per screen. Add new screens there
instead of switching radios by hand, so nested screens cannot turn them back on early.

FUD-Deck ships with no attack modes enabled by default, and transmits nothing by default.
A fork that adds one should keep it off until the user turns it on, and warn first.
