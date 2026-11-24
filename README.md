# ESP32 scale reader

Read a Renpho / QN Bluetooth bathroom scale with an ESP32 board (about $8) and
send every weigh-in over Wi-Fi to a computer in your house: a Raspberry Pi, a
home server, Home Assistant, anything that can accept an HTTP POST. No phone,
no app, no cloud account.

```
 bathroom scale  ──Bluetooth LE──▶  ESP32 by the scale  ──Wi-Fi, HTTP POST──▶  Pi / server in the house
 (Renpho / QN)                      (this firmware)                            (scalestore.py: database + web page)
```

Step on the scale and, a few seconds after the number locks, the reading is
in a database on your network with a page showing the history, a 7-day
average and a tab per person.

Code: [`firmware/`](https://github.com/clemoyne/esp32-scale-reader/tree/main/firmware/scale_reader) (ESP32, Arduino),
[`receiver/`](https://github.com/clemoyne/esp32-scale-reader/tree/main/receiver) (Python standard library),
[`pi/`](https://github.com/clemoyne/esp32-scale-reader/tree/main/pi) (read the scale straight from a Raspberry Pi instead).

## Why an ESP32 and not the Pi you already have

The scale only sends its weight over a **connection**, not in its Bluetooth
advertisements, so whatever reads it has to connect and do a short handshake
while the scale is awake. That's where distance hurts:

- A Raspberry Pi 3 A+ and a Pi 4 both *heard* the scale from another room
  (−70 to −80 dBm) but every connection attempt failed. A metre or two away,
  the Pi connected within a few tries, and only after raising Linux's
  Bluetooth supervision timeout from 420 ms to 4 s.
- An ESP32 plugged into a phone charger next to the scale connected every
  time, in under a second, at −65 to −79 dBm. The handshake took 2 s
  (about 10 s on the Pi), and readings reached the server 3 s after the
  scale locked.

So the reader lives in the bathroom, and the ESP32 is the cheapest, smallest
thing that does Bluetooth and Wi-Fi, has no SD card to wear out, and starts in
a second after a power cut.

## What you need

- A Renpho or other **QN-based** scale. It advertises as `QN-Scale` and has a
  Bluetooth service `FFE0`. Scales that pair with the Renpho app often do;
  check with a BLE scanner app (nRF Connect) for the name and service. The
  firmware also handles the `FFF0` variant, but only `FFE0` has been tested.
- An **ESP32 dev board** with the original ESP32 chip (ESP-WROOM-32). Tested
  with an ELEGOO ESP-32 board (USB-C, CP2102). ESP32-S3 should work too.
  Skip ESP8266 boards (no Bluetooth) and tiny ESP32-C3 "SuperMini" boards
  (weak antennas: range is the whole problem here).
- A USB charger near the scale.
- An always-on machine on your network for the receiver, if you want the
  page and database (a Raspberry Pi is ideal). Or point the ESP32 at your
  own endpoint: see [Sending readings somewhere else](#sending-readings-somewhere-else).

## How the scale talks

Reverse-engineered by [openScale](https://github.com/oliexdev/openScale)
(`BluetoothQNScale`); confirmed here against the scale's display.

| Characteristic | Use |
|---|---|
| `FFE1` (notify) | Packets from the scale |
| `FFE2` (indicate) | Subscribed as well |
| `FFE3` (write) | Config: `13 09 15 UU 10 00 00 00 CS`. `UU` is the display unit (`01` kg, `02` lb), `CS` the byte sum |
| `FFE4` (write) | Time: `02` + seconds since 2000-01-01 UTC, 32-bit little-endian |

1. After you subscribe, the scale sends `12 …` (info; byte 2 is the protocol type, `15` here; byte 10 = `01` means weights are in 0.01 kg). Reply with the config on `FFE3` and the time on `FFE4`.
2. It sends `14 …`. Reply on `FFE3` with `20 08 15` + the same 4 time bytes + the byte sum. The handshake is done.
3. While someone stands on it, it sends `10 0b 15 HH LL SS …`: weight = `0xHHLL` / 100 kg, and `SS` = `01` once the reading locks. Weights are always kg, whatever the display shows.

Example: `10 0b 15 1f 40 01 …` → 0x1F40 = 8000 → 80.00 kg = 176.4 lb, locked.

Things learned the hard way:

- **Be connected before the weigh-in starts.** A weigh-in that begins while
  the reader is connected always arrives. One that wakes the scale arrives
  only if the reader connects and finishes the handshake within a couple of
  seconds; the ESP32 usually manages that.
- **Only stepping on wakes it** (tapping doesn't, on this model). It then
  keeps advertising, slowly, for about 20 minutes, with the display dark.
- **It drops the link itself** a few seconds after a reading, so the reader
  reconnects straight away, ready for the next person.
- **It doesn't sleep while something stays connected** (it held a link for
  48 minutes). So after 10 minutes with no weigh-in the reader disconnects,
  and ignores the slow adverts until the scale wakes: stepping on makes it
  advertise several times a second instead of about once.
- **Don't slow the link to save its battery.** A 1 s connection interval lost
  the locked weight among queued updates. A 30–50 ms interval with
  *peripheral latency* 20 (the scale may skip events while it has nothing to
  say) works: about one wake a second when idle, full speed during a weigh-in.
- openScale also asks some QN models for a stored record (`22 …`); this one
  never answered, so the reader doesn't.

## Flash the ESP32

With [arduino-cli](https://arduino.github.io/arduino-cli/) (`brew install arduino-cli` on a Mac):

```sh
arduino-cli core update-index
arduino-cli core install esp32:esp32                 # tested with 3.3.12
arduino-cli lib install NimBLE-Arduino@2.5.2         # a bare install picked an ancient 1.0.2

cd firmware/scale_reader
cp config.example.h config.h                         # then edit it (below); it's in .gitignore
cd ../..

FQBN=esp32:esp32:esp32:PartitionScheme=min_spiffs   # the sketch (~1.4 MB) is too big for the default layout
arduino-cli compile --fqbn $FQBN firmware/scale_reader
arduino-cli upload  --fqbn $FQBN -p /dev/cu.usbserial-0001 firmware/scale_reader   # your port: arduino-cli board list
```

The Arduino IDE works too: install the same two packages, pick "ESP32 Dev
Module" and the "Minimal SPIFFS" partition scheme.

### `config.h`

| Setting | What to put |
|---|---|
| `WIFI_SSID`, `WIFI_PASSWORD` | Your 2.4 GHz Wi-Fi |
| `HOSTNAME` | The board's name on the network, e.g. `scale-reader` → `http://scale-reader.local/` |
| `OTA_PASSWORD` | Any random string, used only for wireless updates (`openssl rand -hex 8`) |
| `SCALE_ADDRESS` | Your scale's Bluetooth address, lower case. Leave `""` to take the first device named `SCALE_NAME`; fine unless a neighbour has the same scale. Find the address with a BLE scanner app (nRF Connect) or `pi/scan_scale.py` |
| `DISPLAY_UNIT_LB` | `1` sets the scale's display to pounds, `0` to kg |
| `REPORT_HOST`, `REPORT_PORT`, `REPORT_PATH` | Where readings are POSTed. `.local` names are looked up with mDNS |
| `TIMEZONE` | A POSIX TZ string, for the log's timestamps |
| `SUPERVISION_TIMEOUT_MS` | 4000. The link is dropped if the scale goes quiet this long (6 s once the handshake is done) |
| `IDLE_S` | 600. Disconnect after this long with no weigh-in, so the scale can sleep |

## Using it

Put the board within a couple of metres of the scale and plug it in. Then:

Just step on. When the weight locks, the board's LED flashes **three times**
and the reading is sent.

| LED | Meaning |
|---|---|
| Two flashes | Starting up |
| Fast blink | Connecting to the scale |
| **On** | Connected and ready: step on |
| Three slow flashes | Reading taken (then back on, for the next person) |
| Off | Not connected: the scale is asleep or idle; stepping on wakes it |

(The red LED on most boards is just power; the one that blinks is on GPIO 2.
Change `LED_PIN` in `config.h` if yours is elsewhere.)

**Status page:** `http://scale-reader.local/` shows Wi-Fi signal, how strongly
the board hears the scale each time it wakes (handy for finding a spot),
the last reading, anything waiting to be sent, and the recent log:

```
scale-reader  now 11-23 16:52:28  up 107 s
last boot: started (power on)
wifi MyWifi  -65 dBm  192.168.1.50
state scanning  sessions 1  connect failures 0
scale last heard 1 s ago at -65 dBm (52 adverts)
last reading 80.00 kg = 176.4 lb, 25 s ago
waiting to send 0  ->  raspberrypi.local:8091/scale

11-23 16:50:44 scale advertising (-65 dBm), connecting
11-23 16:50:44 connected in 687 ms
11-23 16:50:46 write 0xffe4: 02b6719732
11-23 16:50:46 handshake done, step on now
11-23 16:52:02 WEIGHT 80.00 kg = 176.4 lb STABLE
11-23 16:52:05 posted 80.00 kg to raspberrypi.local
```

**Wireless updates:** after the first USB flash, update it in place:

```sh
arduino-cli upload --fqbn $FQBN -p scale-reader.local --protocol network \
  --upload-field password=YOUR_OTA_PASSWORD firmware/scale_reader
```

**If it can't send:** up to 32 readings wait in memory and are retried
every 30 s (they're lost if the board loses power).

**If it hangs:** a 30 s watchdog restarts the board. The log is kept in
RTC memory, so after a watchdog restart the status page's "last boot" line
says which step it was stuck in.

## The receiver: a database and a page in your house

[`receiver/scalestore.py`](https://github.com/clemoyne/esp32-scale-reader/blob/main/receiver/scalestore.py)
is a single Python file, standard library only (Python 3.7+). It accepts the
ESP32's readings, stores them in SQLite, and serves a page:

- a tab per person, with their latest weight, 7-day average, change over the period, low and high
- a chart of every reading with a 7-day average line, in lb or kg, over 30 days, 90 days, a year or everything
- a table of readings, where **Hide** takes a stray reading (a guest, a bag
  on the scale) out of the figures without deleting it
- an Everyone tab listing all readings with whose each one is

Try it anywhere:

```sh
cd receiver
cp people.example.json people.json     # edit the names and ranges
python3 scalestore.py --port 8091 --db scale.db --people people.json
# open http://localhost:8091/scale/
```

Install it on a Raspberry Pi as a service:

```sh
sudo mkdir -p /opt/scalestore/www /var/lib/scalestore
sudo cp scalestore.py /opt/scalestore/ && sudo cp www/index.html /opt/scalestore/www/
sudo cp people.json /var/lib/scalestore/ && sudo chown -R pi:pi /var/lib/scalestore
sudo cp scalestore.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now scalestore
```

Then set `REPORT_HOST` in the ESP32's `config.h` to that Pi's name
(`raspberrypi.local`) or IP address.

### Who weighed in

A shared scale doesn't know who's standing on it, so the store assigns each
reading by weight range, from `people.json`:

```json
{
  "people": [
    {"name": "Adult 1", "min_lb": 170},
    {"name": "Adult 2", "min_lb": 100, "max_lb": 170},
    {"name": "Child",   "max_lb": 100}
  ]
}
```

`min_lb` counts, `max_lb` doesn't (so 170.0 is Adult 1 and 169.9 is
Adult 2), compared at the 0.1 lb the page shows. The ranges are applied each
time readings are read, not when they're stored, so when someone grows into
another range you change the file and every past reading is re-sorted. No
restart needed. This works for households whose weights don't overlap.

### API

| Request | Does |
|---|---|
| `POST /scale` | A reading from the ESP32 (also `/scale/api/ingest`) |
| `GET /scale/` | The page |
| `GET /scale/api/readings?days=N` | Readings, oldest first, each with its person (`days` 0 or absent: all) |
| `GET /scale/api/status` | Count, latest reading per person, time of the last post |
| `POST /scale/api/hide` | `{"id": 3, "hidden": true}`; `false` shows it again |

## Sending readings somewhere else

The ESP32 sends one small JSON POST per weigh-in:

```json
{"kg": 80.00, "lb": 176.4, "time": 1795400000, "rssi": -65, "reader": "scale-reader"}
```

`time` is Unix time when the weight locked (from NTP; `0` if the board
hasn't got the time yet, in which case use arrival time). Any 2xx reply
counts as delivered; anything else and it retries. So you can point it at:

- **Home Assistant:** a webhook automation
  (`REPORT_PATH "/api/webhook/<your-id>"`, port 8123) that sets an input
  number or a template sensor from `trigger.json.kg`.
- **Node-RED:** an `http in` node on POST.
- **Your own script:** anything that answers 200. Test it with
  `curl -X POST -H 'Content-Type: application/json' -d '{"kg":80,"time":0}' http://HOST:PORT/PATH`

## Reading the scale straight from a Pi

[`pi/`](https://github.com/clemoyne/esp32-scale-reader/tree/main/pi) has two
standard-library Python scripts that do the same over the Pi's own
Bluetooth, with no BlueZ D-Bus bindings or pip packages (written for Python
3.7 on Raspbian 10):

- `scan_scale.py`: shows the scale's advertisements and signal strength
  (also how to find its address). `sudo python3 scan_scale.py`
- `scale_weight.py ADDRESS`: connects, does the handshake and prints the
  weight. `sudo python3 scale_weight.py aa:bb:cc:dd:ee:ff`

They work if the Pi is within a metre or two of the scale; see the docstrings for
the supervision-timeout setting that made it reliable. From another room, use the ESP32.

## Troubleshooting

- **No serial port appears:** the board's USB chip needs a driver (CP210x or CH34x), or the cable is charge-only.
- **Serial monitor shows garbage on a Mac** (a flood of `00 80` bytes): seen with a CP2102 board. Flashing still works; use the status page instead of the serial log.
- **Wireless upload says "No response from the ESP":** check `http://scale-reader.local/` answers. If it doesn't, the board is stuck or off the network; power-cycle it, then upload straight away.
- **"connect failed" in the log:** move the board closer or raise it off the floor. The scale's own signal is weak, and bathroom tile, mirrors and pipes don't help.
- **"settling" weights in the log but no locked one:** the link is too slow (see *How the scale talks*). If the board connected only after you stepped on, step off and on again once the LED is solid.
- **`can't resolve raspberrypi.local`:** use the receiver's IP address in `REPORT_HOST`.

## Privacy and security

- Everything stays on your network. Neither the ESP32 nor the receiver talks to the internet (apart from the board asking `pool.ntp.org` for the time).
- The receiver has no login: anyone on your Wi-Fi can read the page and hide readings. Don't forward its port to the internet. If you want it reachable from outside, put it behind something that authenticates (a VPN, Tailscale, or a Cloudflare Access tunnel).
- `config.h` holds your Wi-Fi password and stays out of git (it's in `.gitignore`).

## Credits and licence

Scale protocol from [openScale](https://github.com/oliexdev/openScale)'s QN
scale driver. Bluetooth on the ESP32 with
[NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino).

MIT licence; see [LICENSE](https://github.com/clemoyne/esp32-scale-reader/blob/main/LICENSE).
