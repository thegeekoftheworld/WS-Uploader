# ws-uploader (WeatherUploader) — Weather Station → Home Assistant + CWOP/APRS + HTTP Ingest

## Hardware

This project has been tested with the following weather station hardware:

- **Urageuxy Weather Station (7-in-1 outdoor sensor, WiFi)** — Amazon product page: https://www.amazon.com/dp/B0FJF9FTFY

`ws-uploader` polls a local weather-station HTTP interface, parses live conditions, publishes them to MQTT for Home Assistant (including HA discovery), optionally forwards data to CWOP/APRS-IS, and can also push data to an external HTTP ingest endpoint.

This is designed to run as a small always-on Linux service.

---

## Features

- Polls your weather station on a LAN IP (e.g. `http://192.168.1.100/...`)
- Parses current conditions (temp, humidity, wind, gust, direction, pressure, rain, solar, UV, etc.)
- Publishes to MQTT topics for Home Assistant
- Home Assistant MQTT Discovery support (auto-creates entities)
- CWOP/APRS-IS weather packet gate (optional)
  - Logs in, waits for `# logresp ... verified`, then transmits packet
- Optional HTTP ingest (GET) to your endpoint
- Debug logging (`-d`) showing polling, parsing, and gate timing decisions

---

## Requirements

### Build dependencies (Debian/Ubuntu)

- `gcc`
- `make` (optional)
- `libcurl4-openssl-dev` (or `libcurl4-gnutls-dev`)
- `libmosquitto-dev`
- `pkg-config` (optional)

### Runtime dependencies

- `libcurl4`
- `libmosquitto1`

---

## Build

From the project directory:

```bash
gcc -O2 -Wall -Wextra -o ws-uploader w.c -lcurl -lmosquitto -lm
```

(If you’re using a different filename, adjust accordingly.)

---

## Recommended install layout

- Binary: `/opt/ws-uploader/uploader`
- Config: `/etc/ws-uploader/config.ini`
- Service: `systemd` unit `ws-uploader.service`

Manual install example:

```bash
sudo mkdir -p /opt/ws-uploader /etc/ws-uploader
sudo install -m 0755 ./ws-uploader /opt/ws-uploader/uploader
sudo install -m 0644 ./config.ini /etc/ws-uploader/config.ini
```

---

## Configuration

The program uses an INI file. Example `config.ini`:

```ini
[station]
base_ip = 192.168.1.100
poll_seconds = 20

[mqtt]
enabled = 1
host = 127.0.0.1
port = 1883
username =
password =
topic_prefix = weatheruploader
ha_discovery_prefix = homeassistant
expire_after = 120

[cwop]
enabled = 1
server = cwop.aprs.net
port = 14580
station = CALLSIGN
passcode = APRSPASS
interval_seconds = 120

[http_ingest]
enabled = 0
url = http://example.com/wx-ingest/
station =
key =
; NOTE: dateutc contains spaces, so the uploader should URL-encode query strings.
```

### `enabled = true` or `enabled = 1`?

Use **`enabled = 1`** (recommended).  
The parser may accept `true/false`, but `1/0` is the safest.

---

## Running

```bash
/opt/ws-uploader/uploader -c /etc/ws-uploader/config.ini
```

Debug mode:

```bash
/opt/ws-uploader/uploader -c /etc/ws-uploader/config.ini -d
```

What you should see in debug:

- HTTP polling (`connect_status`, `rec_refresh`, `record`)
- Parsed values (e.g. `tempf=... hum=... rain_day=...`)
- CWOP gate decisions (enabled, interval, last_sent, now, how long until next send)
- CWOP login + `verified` response + packet send (when due)

---

## Home Assistant

### MQTT Discovery

If MQTT discovery is enabled, Home Assistant will automatically create entities for the station.

If you see **Daily Rain: Unavailable** in HA:

- Confirm the station is actually returning that field.
- Confirm discovery payload for that entity is being published and that state topic is being updated.
- Confirm `expire_after` isn’t too short (or network/MQTT interruptions aren’t causing entity expiration).

### Suggested rain sensors

If present from the station, this project can publish these sensors:

- `rainrate_inhr` — Rain rate (in/hr)
- `rain_hour_in` — Rain last hour (in)
- `dailyrainin` — Rain today (in)
- `weeklyrainin` — Rain this week (in)
- `monthlyrainin` — Rain this month (in)
- `yearlyrainin` — Rain this year (in)
- `totalrainin` — Total rain (in)

Optional:

- `battery_status` — Battery status

---

## CWOP/APRS Notes

- CWOP uses APRS-IS login: `user CALL pass PASS vers ...`
- The uploader is intended to:
  1. Connect
  2. Read greeting
  3. Send login
  4. Wait for `# logresp <call> verified`
  5. Send weather packet

If APRS services show **“Invalid packet”**:

- Check timestamp format (`z` vs `h`) and ensure it matches APRS spec.
- Confirm your packet has the correct fields and line terminators (CR/LF handling).
- Compare against known-good packets from other CWOP stations.

---

## Troubleshooting

### “URL rejected: Malformed input to a URL function”

Your HTTP ingest URL likely contains a parameter value with spaces (e.g. `dateutc=YYYY-MM-DD HH:MM:SS`).
That must be **URL-encoded** (`%20` or `+`) or built using a URL-encoding function.

### CWOP “connected/sent OK” but server UI shows no user

Some APRS-IS server dashboards don’t display usernames until they’ve fully processed login/traffic,
or may show blank for short-lived sessions. The real source of truth is receiving
`# logresp ... verified` and verifying on aprs.fi / findu / CWOP site after propagation.

### HA entity shows “Unavailable”

Common causes:

- MQTT discovery mismatch (wrong unique_id/topic)
- No state updates being published
- `expire_after` too short
- Home Assistant restart/reset required after entity schema changes

---

## Service (systemd)

Create:

`/etc/systemd/system/ws-uploader.service`

```ini
[Unit]
Description=ws-uploader (WeatherUploader)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=/opt/ws-uploader/uploader -c /etc/ws-uploader/config.ini
Restart=always
RestartSec=5
User=root
WorkingDirectory=/opt/ws-uploader

[Install]
WantedBy=multi-user.target
```

Enable + start:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now ws-uploader
sudo systemctl status ws-uploader
```

Logs:

```bash
journalctl -u ws-uploader -f
```

---

## License / Third-party

This project may embed third-party components such as **jsmn** (MIT licensed JSON parser).

If you redistribute binaries or source, keep applicable third-party license notices
(e.g., in a `THIRD_PARTY_NOTICES.md` file or retained source headers).

---

## Quick command summary

```bash
# Build
gcc -O2 -Wall -Wextra -o ws-uploader w.c -lcurl -lmosquitto -lm

# Run (debug)
./ws-uploader -c ./config.ini -d

# Install
sudo install -m 0755 ./ws-uploader /opt/ws-uploader/uploader
sudo install -m 0644 ./config.ini /etc/ws-uploader/config.ini

# Service
sudo systemctl enable --now ws-uploader
journalctl -u ws-uploader -f
```

---

## Typical fields

- Temperature: `tempf`
- Humidity: `humidity`
- Dew point: `dewptf`
- Wind speed: `windspeedmph`
- Gust: `windgustmph`
- Wind dir: `winddir`
- Pressure: `baromin`
- Rain: `dailyrainin` + optional rate/hour/week/month/year/total
- Solar: `solarradiation`
- UV: `uv`
- Battery: `battery_status` (if station provides it)
