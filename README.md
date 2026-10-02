# bustracker

> **Work in progress.** This project is under active development. Some parts are unfinished, untested on final hardware, or may change.


A desk-sized bus arrival display for **Whatcom Transportation Authority (WTA) Route 190** in Bellingham, WA.
An ESP32 reads WTA's live bus data over Wi-Fi and shows how long until the next bus reaches your stop on a
retro 128x64 OLED. A 15-LED progress bar (on the custom PCB) shows the countdown at a glance.

```
BUS 190
ARR 4:37PM
ETA:7 MINS
```

## Status

| Part | State |
|------|-------|
| ESP32 reads WTA's live feed directly (no PC needed) | Working |
| OLED display (live ETA, scheduled fallback) | Working |
| Web simulator of the OLED + stop picker | Working |
| 15-LED progress bar firmware | In progress |
| Custom PCB | Designed and ordered |

## How it works

1. The ESP32 joins Wi-Fi and sets its clock from the internet.
2. Every 20 seconds it downloads WTA's GTFS-Realtime trip feed (`bustracker.ridewta.com/gtfsrt/trips`) over HTTPS
   and decodes it on the device. The feed is a binary protobuf, so a small decoder is built in (no libraries needed).
3. It picks out Route 190 buses and finds the next one for your stop.
4. If the live feed has no prediction for your stop yet (for example a bus that hasn't left the start of the route),
   it shows the next **scheduled** time instead and labels it `SCH:`. Live times are labelled `ETA:`.

No API key is needed.

## Repository layout

```
firmware/bustracker/        ESP32 sketch (Arduino IDE)
web-simulator/              Node server + web page that mimics the OLED, with a stop picker
hardware/                   PCB files, wiring and pin maps
docs/                       Photos, notes
```

## Getting started: firmware

1. Install the **Arduino IDE** and the **esp32 by Espressif** board package.
2. Install the libraries **Adafruit SSD1306** and **Adafruit GFX** (Library Manager).
3. In `firmware/bustracker/`, copy `secrets.h.example` to `secrets.h` and enter your Wi-Fi name and password.
   Use a **2.4 GHz** network. `secrets.h` is git-ignored.
4. Open `bustracker.ino` and choose your board near the top (`BOARD_S2_MINI` or `BOARD_WROOM32`).
5. Set your stop (see the table below).
6. Tools menu: pick your board. For the LOLIN S2 Mini, set **USB CDC On Boot: Enabled**.
7. Upload. Open Serial Monitor at 115200 to see what it is doing.

### Stops: Highland Dr at Ridgeway

| Direction | `STOP_ID` | `STOP_CODE` |
|-----------|-----------|-------------|
| Downtown-bound | `664` | `3180` |
| Lincoln St-bound | `665` | `2131` |

For other stops, look them up in WTA's static GTFS (`stops.txt`) or with the web simulator's stop picker.
The built-in scheduled fallback only covers the two stops above.

### Screen messages

`CONNECTING`, `CONNECTED`, `SETTING CLOCK`, `NO WIFI`, `FEED ERR` (with the reason on the next line),
`NO BUSES`. Times of 100 minutes or more show as hours and minutes (`SCH:2H05M`).

## Getting started: web simulator

Needs Node.js 18+.

```
cd web-simulator
npm install
npm start
```

Open http://localhost:3000, pick a stop, and the page shows live arrivals in an OLED-style layout.

## Known limitations

- The scheduled fallback ignores holiday schedule changes. The embedded schedule is valid
  2026-09-20 to 2027-02-06 and needs regenerating from WTA's GTFS after that.
- Buses that haven't started their trip can be missing from the live feed, so those show as scheduled times.
- Certificate checking is turned off for the HTTPS request (it's public data, but be aware).

## Data

Transit data is published by Whatcom Transportation Authority. See WTA's GTFS page for the data license and terms.
This project is not affiliated with or endorsed by WTA.
