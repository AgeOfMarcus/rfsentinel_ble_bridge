# rfsentinel_ble_bridge

ESP-IDF firmware for a base **ESP32** board that continuously scans Bluetooth Low Energy (BLE) advertisements and streams findings over USB serial to the **RFSentinel** app by emulating **OUI-Spy Detector** mode (`ble_detector`).

> **AI Generation Disclosure:** The source code, build configuration, and documentation for this project were generated with AI assistance.

---

## Why OUI-Spy Detector Emulation

RFSentinel's `GhostEspReports` parser hardcodes `ble = false` for GhostESP network lists, which causes BLE devices to be recorded as Wi-Fi entries. Emulating **OUI-Spy Detector** mode (`OuiSpyReports`) provides clean integration:

* Sets `ble = true` and `frequencyMhz = 0` via `"event":"detection"` with `"protocol":"ble"`.
* Forwards `company_id` (from manufacturer specific data) and `service_uuid` (from 16-bit advertised/service data UUIDs as decimal integers), plus `mac_address`, `rssi`, and `local_name`, so RFSentinel's own detector engine can evaluate signatures.
* Omits `detection_method`, `detection_tier`, and `matched_signature` so sightings are not falsely labeled as Flock cameras or custom watchlist hits.
* Responds to `CMD:VERSION` with `{"firmware":"OUI-SPY","mode":"ble_detector"}` and flushes cached live devices on `CMD:DUMP_LIVE`.

---

## Project Layout

```text
rfsentinel_ble_bridge/
├── CMakeLists.txt
├── sdkconfig.defaults
├── README.md
└── main/
    ├── CMakeLists.txt
    └── main.c
```

---

## Configuration & Behavior

* **UART:** `UART_NUM_0` at `115200` baud (8N1, no hardware flow control), with `CONFIG_LOG_DEFAULT_LEVEL_NONE=y` so ESP-IDF log output does not pollute the JSON stream.
* **BLE Stack & Scan:** Uses **NimBLE** with continuous scanning (`0x0050` interval / `0x0050` window = 100% duty cycle) and `filter_duplicates = 0` so RSSI values and timestamps stay current. Set to passive scanning (`params.passive = 1`) by default; change to `0` in `main/main.c` if active scan responses are desired.
* **Device Cache & Throttling:** Tracks up to 128 devices in RAM, drops entries unseen for 15 seconds (`STALE_TIMEOUT_US`), and emits updates at most once per second per MAC (`REPORT_INTERVAL_US`).

### Sample Serial Output

```json
{"firmware":"OUI-SPY","mode":"ble_detector"}
{"event":"detection","protocol":"ble","mac_address":"A4:C1:38:12:34:56","rssi":-64,"company_id":76}
{"event":"detection","protocol":"ble","mac_address":"DC:2C:26:98:76:54","rssi":-71,"local_name":"Tile","service_uuid":65261}
```

---

## Build & Flash (ESP-IDF)

> **Or, use the [web flasher](https://ageofmarcus.github.io/rfsentinel_ble_bridge/)**

From the project root (`rfsentinel_ble_bridge/`) in an initialized ESP-IDF environment:

```powershell
idf.py set-target esp32
idf.py build
idf.py -p COMx flash
```

Replace `COMx` with your board's serial port. To inspect the stream directly over USB serial, open a `115200`-baud monitor and send `CMD:VERSION` or `CMD:DUMP_LIVE`.
