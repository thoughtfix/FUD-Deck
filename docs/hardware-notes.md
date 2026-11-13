# ESP32-DIV V2 hardware notes

Measured on real V2 hardware while bringing up FUD-Deck. If you write firmware for this
board, these will save you hours.

## ESP32-DIV V2 Flaws (Stock)

Design problems in the stock hardware and firmware, with what we could and could not
code around.

### SD writes fail with the radio shield attached

**Symptom.** The card mounts and small writes work, but longer writes fail, files come back
short or unreadable, and occasionally data is corrupted. It happens at any SPI clock, even
1 MHz, and looks random across reboots. It looks like a bad slot or bad cards. It is not.

**Cause.** The SD card, both nRF24s and the CC1101 share one SPI bus (SCK 12, MOSI 11,
MISO 13), and that bus runs through the umbilical to the shield. At the ESP32-S3 default
pad drive strength (2), the clock and data lines misbehave on that long stub. Every other
drive strength works.

Measured (32 KB write, read back, verify; shield attached):

| SCK/MOSI drive | Result |
|---|---|
| 0 | 30/30 at 1-20 MHz |
| 1 | 30/30 at 1-20 MHz |
| 2 (default) | 0/32 |
| 3 | 12/12 |
| default, shield unplugged | 20/20 at 1-20 MHz |

Ruled out: the radios (powered down, no change), the CC1101 GDO0 clock output (set static,
no change), nRF24 #3 / IR receiver on GPIO21 (no activity during failures), the slot and
the cards (all fine once the drive is changed).

**Coded around: yes.** After every `SPI.begin()` and every `SD.begin()`:

```cpp
gpio_set_drive_capability((gpio_num_t)12, GPIO_DRIVE_CAP_1);   // SCK
gpio_set_drive_capability((gpio_num_t)11, GPIO_DRIVE_CAP_1);   // MOSI
```

Verified over repeated power cycles, with the radios still working at 8 MHz.

**Why it looks random.** The pad drive setting survives a software reset, so a test that
changes it carries into the next boot. Only a real power cycle restores the default. Test
with the power switch, not the reset button.

### Some SD cards block every radio

Some cards keep driving MISO after their chip select goes high. Every nRF24 and the
CC1101 then read 0xFF and look dead. The stock firmware never releases the card after use,
so with a card like that inserted, every radio silently fails.

This is per card, not per brand. We only had one card of each brand we tried, and one of
them did it. That says nothing about the brand, only that some cards strangely do this.

**Coded around: mostly.** After every SD access, clock 0xFF with every chip select high.
That releases most cards. A card that still holds the bus cannot be fixed in firmware;
FUD-Deck's POST warns "card may hold the bus. Try another card" instead of blaming the
radios. A hardware fix would be a tri-state buffer on the card's MISO.

New cards over 32 GB ship as exFAT. Format them FAT32 (MBR) on a computer.

### nRF24 #3 cannot listen

nRF24 #3's CE (GPIO14) is tied through R5 (0 ohm) to the IR LED driver transistor's base,
with no base resistor. The base clamps CE low, so #3 never enters receive. Its CSN
(GPIO21) is also the IR receiver's output, so the firmware cannot drive it without
fighting the receiver.

**Coded around: partly.** FUD-Deck tests CE at boot and runs on two nRF24s when it is
clamped, splitting the 2.4 GHz band between them. GPIO21 is never driven unless #3 proves
usable.

**Not fixable in firmware.** Getting #3 back needs R5 removed, which disables IR transmit.
The IR receiver keeps working.

### Battery reading

The schematic shows a 100k/100k divider on IO2 (VBAT / 2), shared with the speaker stage.
On units with the speaker stage (LS1 and its driver transistor) fitted, the transistor's
base clamps IO2 near 0.7 V and the reading is useless. Units differ: ours has no speaker
stage fitted and a 4.7k/10k divider, so VBAT = IO2 x 1.47, not the schematic's x 2.

**Coded around: partly.** FUD-Deck uses IO2 when the reading is plausible and falls back
to the IP5306 (I2C 0x75, register 0x78) otherwise.

**Not fixable in firmware.** On clamped units the IP5306 only gives 4 coarse steps. A real
fuel gauge (for example a MAX17048 on QWIIC) would fix it.

### IP5306 light-load shutdown

The IP5306 ships with light-load auto-shutdown enabled (register 0x00 bit 1 clear), so a
very idle board can lose power.

**Coded around: yes.** FUD-Deck sets bit 1 (boost keep-on) at boot, reads it back, and
re-checks it every minute. The V2 has a hardware battery switch, so nothing is lost.

### Power, charging and battery safety

For users: [Will the ESP32-DIV V2 burn my house down?](ESP32-DIV_V2_Power/ESP32-DIV_V2_Power.md)
covers polarity, batteries, charging with the switch off and the unplug restart.

Firmware detail on the unplug restart: it logs a power-on reset, not a brownout, so the 5 V
rail drops out completely during the USB to battery handover. The IP5306's "boost on when
USB is pulled" bit (register 0x01 bit 2) is already set (0x1D). FUD-Deck saves the last 8
reset reasons (`[fud] reset ...` at boot; U is the USB serial connection, not power).

### PSRAM is unusable

The ESP32-S3's embedded octal PSRAM shares GPIO33-37 with the display bus.

**Not fixable in firmware.** The PSRAM cannot be enabled, so everything must fit in
internal RAM. FUD-Deck shuts down the BLE and Wi-Fi stacks while a toy needs memory.

## Firmware gotchas (not V2 flaws)

General Arduino-ESP32 core 3.3 issues we hit on this board.

- The four WS2812 LEDs on GPIO1 needed an RMT driver. The Adafruit NeoPixel library showed
  nothing.
- USB CDC: never call `setTxTimeoutMs(0)`. It underflows and hangs the boot when USB is
  connected.
- Start Wi-Fi only from core 0. Starting it from core 1 overflowed the ipc0 task's stack.
