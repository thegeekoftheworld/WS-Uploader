# Custom Upload Guide (weather_to_ha)

This guide explains how to configure **custom upload targets** in `weather_to_ha.ini` using either:

- **GET** request templates (URL with placeholders)
- **POST** request templates (form-urlencoded or JSON body)

You can enable **multiple** upload targets by adding additional sections:
`[upload1]`, `[upload2]`, `[upload3]`, … (up to the compiled limit in your build).

---

## 1) Upload target types

### A) WU-style uploader (`type = wu`)
This is the built-in Weather Underground / PWSWeather style mapping that automatically generates standard WU/PWS query parameters.

```ini
[upload1]
enabled = 1
type = wu
base_url = http://rtupdate.wunderground.com/weatherstation/updateweatherstation.php
id = KORDUNDE14
password = PASSFROMCONFIG
realtime = 1
rtfreq = 20
```

### B) Custom uploader (`type = custom`)
This is the flexible one. You provide the request template(s).

---

## 2) Custom uploader settings (all keys)

> The settings below apply to `[uploadX]` sections where `type = custom`.

### Common keys

| Key | Required | Meaning |
|---|---:|---|
| `enabled` | ✅ | `1` to enable, `0` to disable |
| `type` | ✅ | Must be `custom` |
| `method` | ✅ | `GET` or `POST` |
| `timeout_ms` | optional | Per-upload HTTP timeout (ms). If omitted, the program default is used. |

### GET mode keys (`method = GET`)

| Key | Required | Meaning |
|---|---:|---|
| `template` | ✅ | Full URL template including query string placeholders |

Example (GET):

```ini
[upload3]
enabled = 1
type = custom
method = GET
template = https://example.com/ingest?station={station}&tempf={tempf}&humidity={humidity}&baromin={baromin}&dateutc={dateutc}
```

### POST mode keys (`method = POST`)

| Key | Required | Meaning |
|---|---:|---|
| `url` | ✅ | Target URL to POST to |
| `post_format` | ✅ | `form` or `json` (how you intend the body to be interpreted) |
| `body_template` | ✅ | Body template containing placeholders |
| `header1..headerN` | optional | Extra HTTP headers to send (e.g., auth headers, content-type) |

Examples (POST):

**POST form-urlencoded**
```ini
[upload4]
enabled = 1
type = custom
method = POST
post_format = form
url = https://example.com/ingest
header1 = Content-Type: application/x-www-form-urlencoded
body_template = station={station}&tempf={tempf}&humidity={humidity}&dateutc={dateutc}
```

**POST JSON**
```ini
[upload5]
enabled = 1
type = custom
method = POST
post_format = json
url = https://example.com/ingest
header1 = Content-Type: application/json
body_template = {"station":"{station}","tempf":{tempf},"humidity":{humidity},"baromin":{baromin},"dateutc":"{dateutc}"}
```

**POST with Authorization header**
```ini
[upload6]
enabled = 1
type = custom
method = POST
post_format = json
url = https://example.com/ingest
header1 = Content-Type: application/json
header2 = Authorization: Bearer YOUR_API_TOKEN
body_template = {"station":"{station}","tempf":{tempf},"humidity":{humidity},"dateutc":"{dateutc}"}
```

---

## 3) Placeholders you can use (complete list)

Placeholders are written like `{tempf}` and can be used in:

- GET: `template=...`
- POST: `body_template=...`

### 3.1 Identity & status (from `connect_status`)
| Placeholder | Meaning | Example |
|---|---|---|
| `{mac}` | Device MAC address | `FC:F5:C4:B0:09:51` |
| `{mac_id}` | Normalized MAC (lowercase, no separators) | `fcf5c4b00951` |
| `{ip}` | Station IP | `10.13.37.76` |
| `{rssi}` | RSSI / signal indicator as reported by station | `2` |

### 3.2 Time
| Placeholder | Meaning | Notes |
|---|---|---|
| `{dateutc}` | UTC timestamp `YYYY-MM-DD HH:MM:SS` | Good for WU/PWS-style ingest |
| `{epoch}` | Unix epoch seconds | Integer |

### 3.3 Location (from `[location]` in INI)
| Placeholder | Meaning |
|---|---|
| `{lat}` | Decimal latitude |
| `{lon}` | Decimal longitude |
| `{elev_ft}` | Elevation in feet (from config) |
| `{elev_m}` | Elevation in meters (computed internally from feet) |

### 3.4 Indoor sensor values
| Placeholder | Meaning | Units |
|---|---|---|
| `{indoortempf}` | Indoor temperature | °F |
| `{indoorhumidity}` | Indoor humidity | % |

### 3.5 Outdoor sensor values
| Placeholder | Meaning | Units |
|---|---|---|
| `{tempf}` | Outdoor temperature | °F |
| `{humidity}` | Outdoor humidity | % |
| `{dewptf}` | Dew point (computed) | °F |
| `{windchillf}` | Wind chill (computed) | °F |

### 3.6 Pressure
| Placeholder | Meaning | Units |
|---|---|---|
| `{absbaromin}` | Absolute pressure | inHg |
| `{baromin}` | Relative pressure | inHg |

### 3.7 Wind
| Placeholder | Meaning | Units |
|---|---|---|
| `{windspeedmph}` | Current wind speed | mph |
| `{windgustmph}` | Current gust | mph |
| `{winddir}` | Wind direction | degrees |
| `{windspdmph_avg2m}` | 2-minute average speed | mph |
| `{winddir_avg2m}` | 2-minute average direction | degrees |
| `{windspdmph_avg10m}` | 10-minute average speed | mph |
| `{winddir_avg10m}` | 10-minute average direction | degrees |

> Note: If you later discover a true “10-minute gust” field on the station, you can add placeholders like `{windgustmph_10m}` and `{windgustdir_10m}` easily.

### 3.8 Rain
| Placeholder | Meaning | Units |
|---|---|---|
| `{rainrate_inhr}` | Rain rate | inch/hr |
| `{rain_hour_in}` | Rain in the last hour | inches |
| `{dailyrainin}` | Rain today | inches |
| `{weeklyrainin}` | Rain this week | inches |
| `{monthlyrainin}` | Rain this month | inches |
| `{yearlyrainin}` | Rain this year | inches |
| `{totalrainin}` | Total rain | inches |

### 3.9 Solar / UV
| Placeholder | Meaning | Units |
|---|---|---|
| `{solarradiation}` | Solar radiation / light | W/m² |
| `{uv}` | UV index | index |

### 3.10 Battery
| Placeholder | Meaning |
|---|---|
| `{battery_status}` | Battery status string from device |

### 3.11 Station identity (from `[cwop]` / config)
| Placeholder | Meaning |
|---|---|
| `{station}` | Station identifier you configured (e.g., APRS/CWOP station) |

---

## 4) Encoding rules (important)

- For **GET templates**, values should be **URL-encoded** when substituted into the URL.
  - Example: `{dateutc}` contains a space; it should become `%20` in a GET query string.
- For **POST bodies**, substitution is usually **as-is** (unless you are also sending a form body and need encoding).

If you want strict behavior, the uploader can support **paired placeholders**:
- `{key}` (raw) and `{key_url}` (URL-encoded)

---

## 5) Practical examples

### Example A: GET to a custom collector
```ini
[upload3]
enabled = 1
type = custom
method = GET
template = https://collector.example.net/push?station={station}&mac={mac}&tempf={tempf}&humidity={humidity}&baromin={baromin}&windspeedmph={windspeedmph}&winddir={winddir}&dailyrainin={dailyrainin}&dateutc={dateutc}
```

### Example B: POST JSON to a webhook
```ini
[upload4]
enabled = 1
type = custom
method = POST
post_format = json
url = https://hooks.example.com/weather
header1 = Content-Type: application/json
body_template = {"station":"{station}","mac":"{mac}","lat":{lat},"lon":{lon},"tempf":{tempf},"humidity":{humidity},"baromin":{baromin},"windspeedmph":{windspeedmph},"winddir":{winddir},"dailyrainin":{dailyrainin},"dateutc":"{dateutc}"}
```

### Example C: POST form-urlencoded
```ini
[upload5]
enabled = 1
type = custom
method = POST
post_format = form
url = https://example.com/submit
header1 = Content-Type: application/x-www-form-urlencoded
body_template = station={station}&tempf={tempf}&humidity={humidity}&baromin={baromin}&windspeedmph={windspeedmph}&winddir={winddir}&dateutc={dateutc}
```

---

## 6) Troubleshooting tips

- If an endpoint requires authentication, add headers:
  - `header2 = Authorization: Bearer ...`
  - or a custom API key header such as `header2 = X-API-Key: ...`
- If your endpoint rejects timestamps, try `{epoch}` instead of `{dateutc}`.
- If you see failures, temporarily switch to a simple template with only a few fields to confirm connectivity first.
