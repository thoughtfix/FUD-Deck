# Backport to FUD

Changes made in FUD-Deck that belong in the other FUD releases (FUD-Firmware, FUD-X3).
Code is tagged `BACKPORT(FUD)` so `grep -rn "BACKPORT(FUD)" firmware/src` finds every spot.

## Rule: no broadcasts by default (Updated 2026-11-13)

Every FUD release transmits nothing by default. Bluetooth and Wi-Fi scans are passive: no
scan requests, no probe requests. A feature that has to transmit (active BLE scanning for
device names, anything later) is opt-in, and turning it on warns that it can expose the
user's privacy or location. TV-B-Gone, fired by hand, is the one existing exception.

FUD-Deck's BLE scan was active (it sent scan requests to read device names) and is now
passive. Check FUD-Firmware and FUD-X3 for `setActiveScan(true)` when backporting. Early versions of FUD were "no broadcast ever ever" but additional methods and future expansions will permit users to enable targeted, specific broadcasts at times.

## Rule: no attack modes enabled by default (2026-11-13)

Same shape as the broadcast rule. Wording everywhere says "no attack modes enabled by
default", not "no attack modes": a release may grow an opt-in mode later, off until the user
turns it on, with a warning first.

## Signatures (2026-11-13)

From SquachWatch-CYD by Talking Sasquach (`src/signatures.cpp`, commits 2026-10-14 to
2026-11-03), credited the same way FUD already does. Low-confidence rows skipped, as in FUD.

- **Matcher:** BLE service UUIDs match the advertised list *or* service data. Trackers often
  send only service data. `hasSvc()` in `scanner.cpp`.
- **Ray-Ban Meta fix:** real glasses send company `0x01AB` (Meta) with service `0xFD5F`. FUD
  only accepts `0x0D53` (Luxottica) with `0xFD5F`, so it misses them.
- **Flipper by service UUID:** `0x3081` to `0x3083`, so a renamed Flipper is still caught.
  Do not add Flipper's company ID `0x0E29`: only the BUSY Bar desk sign sends it.
- **New BLE rows:** Tile `FD84`, DULT `FCB2` (any maker's tag away from its owner), Chipolo
  (`FE33`, `FE65`, company `08C3`), Axon UUIDs (`FC81`, `FE6B`, `FE6C`), Flock Raven gunshot
  sensors (`3100` to `3500`), glasses by company (Meta `01AB`, Luxottica `0D53`, Snap `03C2`,
  Vuzix `060C`) and UUID (Snap `FE45`, Meta `FEB7`/`FEB8`). The Meta UUIDs also match Quest
  headsets (a Quest 3 on the bench): flagged on purpose, a Quest has cameras.
- **New Wi-Fi OUIs:** WatchGuard VIDEO and Digital Ally body cams, ShotSpotter, i-PRO, six
  Blink blocks, Luxottica, Skydio, three Parrot blocks.
- **New SSID prefixes:** `flock-`, `Flock Camera`. SSID prefixes now match case-insensitively.

## Zuse vector faces

`firmware/src/ui/facevec.h`, `facevec_data.h` and `firmware/tools/facevec/`. Replaces the
bitmap faces. See `firmware/images/README.md` for what can be removed after the port.
