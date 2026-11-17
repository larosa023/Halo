# The UltralightX protocol

Halo talks to the mouse the same way Finalmouse's XPANEL web configurator does. This page records what Halo uses. The code is in [`src/protocol.cpp`](../src/protocol.cpp) and the tests are in [`tests/protocol_test.cpp`](../tests/protocol_test.cpp).

## Device

| | |
|---|---|
| Dongle | USB `361D:0100` ("UltralightX dongle") |
| Collection | usage page `0xFF00`, usage `0x0001`, on interface 0 |
| Mouse on its cable | `361D:0102`. It has no settings interface, so Halo asks you to plug in the dongle |

When the cable and the dongle are both plugged in, the mouse keeps talking through the dongle. That's how Halo reads the charging state.

## Framing

Every report is 63 bytes after the report id:

```
[report id] [2 + n] [0x80 | command] [n] [payload: n bytes] [zero padding]
```

| Report | Direction | Use |
|---|---|---|
| `0x04` | out | to the mouse, through the dongle |
| `0x05` | in | the mouse's replies |
| `0x02` | out | to the dongle itself |
| `0x03` | in | the dongle's replies |

A command with an empty payload is a read. Replies use the same framing without the `0x80` bit.

## Commands

| # | Name | Payload | Notes |
|---|---|---|---|
| 3 | DPI (read back) | u16 LE | |
| 4 | Polling rate (read back) | u16 LE, Hz | |
| 5 | Battery voltage | u16 LE, mV | the only source of the battery level on current firmware |
| 10 | Dongle USB firmware | ASCII | on reports `0x02` and `0x03` |
| 11 / 12 | Dongle RF / mouse firmware | ASCII | |
| 13 | RSSI | s8, dBm | |
| 14 | Serial number | ASCII | |
| 16 | Set DPI | u16 LE | 50 to 26,000 |
| 17 | Set polling rate | u16 LE | 500, 1000, 2000, 4000, 8000 |
| 18 | Motion Sync | u8 0/1 | |
| 20 | Dongle light | u8 | 0 off, 1 battery color, 2 solid white |
| 21 | Lift-off distance | u8 | 1 or 2 mm |
| 23 | Tournament scroll | u8 | 0 off, 1 up, 2 down, 3 both |
| 24 | Tournament scroll timeout | u8, ×100 ms | 1, 5, 10, 15 |
| 36 | Link state | u8 | 0 = mouse asleep or off |
| 37 | Charging | u8 0/1 | |
| 38 | State of charge | u8 % + u16 mV | not answered by current firmware |
| 96 | Send everything | none | the mouse replies with every setting |

XPANEL's connect sequence, which Halo copies: read lift-off (21), then *send everything* (96), then ask the dongle for its firmware (10 on report `0x02`).

## Battery level

XPANEL turns the voltage into a percentage with this curve. Halo interpolates linearly between the points:

| V | 3.00 | 3.62 | 3.66 | 3.74 | 3.88 | 4.17 | 4.38 |
|---|---|---|---|---|---|---|---|
| % | 0.2 | 5 | 10 | 25 | 50 | 75 | 100 |

While the mouse is charging, the voltage reads high, so the level does too. XPANEL shows the same.

## Credits

These open-source projects documented the protocol from XPANEL and from captures on real hardware: [OpenMouse mouse-protocol](https://github.com/OpenMouse-Project/mouse-protocol), [johnklucinec/ulx_reverse](https://github.com/johnklucinec/ulx_reverse), [xBambooz/FinalmousePollingRateSwitcher](https://github.com/xBambooz/FinalmousePollingRateSwitcher), and the hardware captures in [HaloBattery #283](https://github.com/HeyOkay/HaloBattery/issues/283). Halo's code is its own. It's written from the facts above, not copied from those projects.

**Halo does not flash firmware, pair, or touch calibration.** Those commands exist, but Halo never sends them.
