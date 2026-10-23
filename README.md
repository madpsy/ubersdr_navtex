# ubersdr_navtex

Automated dual-frequency NAVTEX receiver for [UberSDR](https://ubersdr.org) — connects to a remote UberSDR instance, simultaneously monitors both standard NAVTEX frequencies (518 kHz international and 490 kHz national/coastal), decodes the SITOR-B/NAVTEX signals, and serves decoded messages in a live tabbed web UI.

---

## How it works

```
                          ┌──► channel 0: 518 kHz (International) ──► tab 0
UberSDR (remote SDR) ──►  │
                          └──► channel 1: 490 kHz (National)       ──► tab 1
                                    │
                                    └──► web UI  http://<host>:6092
```

- **`navtex_rx_from_ubersdr`** — C++ service that opens two simultaneous WebSocket connections to UberSDR (one per frequency), streams demodulated audio for each, decodes NAVTEX (SITOR-B) independently, and serves a real-time tabbed web UI
- **Web UI** — tabbed interface with one tab per frequency; each tab shows live decoded text, signal stats (dBFS, SNR, FEC quality, decoder state), message info (station/subject/serial), and an audio preview button; served on port 6092

---

## Quick start (Docker — recommended)

```bash
curl -fsSL https://raw.githubusercontent.com/madpsy/ubersdr_navtex/master/install.sh | bash
```

This will:
1. Create `~/ubersdr/navtex/` and download `docker-compose.yml` + helper scripts
2. Pull the latest `madpsy/ubersdr_navtex` image
3. Start the service

Then edit `~/ubersdr/navtex/docker-compose.yml` to set your UberSDR URL and run `./restart.sh`.

---

## Configuration

All configuration is via environment variables in `docker-compose.yml`:

| Variable | Default | Description |
|----------|---------|-------------|
| `UBERSDR_URL` | `http://ubersdr:8080` | UberSDR base URL |
| `NAVTEX_FREQ_1` | `518000` | First NAVTEX carrier frequency in Hz (International) |
| `NAVTEX_FREQ_2` | `490000` | Second NAVTEX carrier frequency in Hz (National/coastal) |
| `WEB_PORT` | `6092` | Web UI port |
| `NAVTEX_LOG_DIR` | `/data/navtex_logs` | Directory inside the container to save decoded messages. Set to `""` to disable. |
| `NAVTEX_RX_LAT` / `NAVTEX_RX_LON` | *(unset)* | Receiver position in decimal degrees, for station identification. Only needed if UberSDR has no GPS position set; overrides it when both are given. **The position is published** — see [Receiver location](#station-identification). |
| `NAVTEX_MAP_TILES` | *(OpenStreetMap)* | Leaflet tile URL template for the web UI's maps, e.g. `https://tiles.example.lan/{z}/{x}/{y}.png`. Default `https://tile.openstreetmap.org/{z}/{x}/{y}.png`. |
| `NAVTEX_MAP_ATTRIBUTION` | *(OpenStreetMap credit)* | HTML credit line shown on the maps for those tiles. Set it whenever you set `NAVTEX_MAP_TILES`. |

Both frequencies are **always monitored simultaneously** — there is no single-frequency mode.

### Message logging

When `NAVTEX_LOG_DIR` is set (enabled by default), every completed NAVTEX message is saved to disk as a plain-text `.txt` file. The directory structure is:

```
navtex_logs/
└── <frequency>/          e.g. 518kHz or 490kHz
    └── <YYYY>/
        └── <MM>/
            └── <DD>/
                └── <HHMMSS>Z_<station><subject><serial>.txt
```

For example: `navtex_logs/518kHz/2025/04/08/143022Z_EA42.txt`

If the station/subject/serial header fields are not decoded (e.g. the message was truncated), the filename falls back to `<HHMMSS>Z_unknown.txt`.

Each file contains the full message including the `ZCZC` header line and `NNNN` end marker.

The `docker-compose.yml` bind-mounts `./navtex_logs` on the host to `/data/navtex_logs` inside the container. The `install.sh` script creates this directory automatically.

#### Automatic log retention

Set `NAVTEX_LOG_RETAIN_DAYS` (default: `90`) to automatically delete log files older than that many days. The cleanup runs once at startup and then every hour. Empty date directories are removed after cleanup. Set to `0` to keep all logs indefinitely.

```yaml
NAVTEX_LOG_RETAIN_DAYS: "90"   # delete files older than 90 days (0 = keep forever)
```

---

## Helper scripts

After running `install.sh`, the following scripts are available in `~/ubersdr/navtex/`:

| Script | Action |
|--------|--------|
| `./start.sh` | Start the service |
| `./stop.sh` | Stop the service |
| `./restart.sh` | Restart the service (apply config changes) |
| `./update.sh` | Pull the latest image and restart |

---

## Building from source

### Docker image

```bash
./docker.sh build          # build madpsy/ubersdr_navtex:latest
./docker.sh push           # build and push to Docker Hub
./docker.sh run            # run locally (uses env vars)
```

Override the image name:
```bash
IMAGE=myrepo/ubersdr_navtex:dev ./docker.sh build
```

### Local build (no Docker)

Requires: `build-essential`, `cmake`, `libcurl4-openssl-dev`, `libssl-dev`, `pkg-config`

IXWebSocket is cloned automatically from GitHub if not present.

```bash
./build.sh
# Binary: ./build/src/navtex_rx_from_ubersdr
```

---

## Web UI

Open `http://<host>:6092` in a browser to view:

- **Two tabs** — one per frequency (e.g. `518 kHz International` / `490 kHz National`)
- Tab status dot: green = Locked, amber = Syncing, grey = Searching
- Real-time decoded NAVTEX characters (per tab)
- Signal level and SNR bars (per tab)
- Decoder state (Searching / Syncing / Locked) (per tab)
- FEC quality (clean / FEC-corrected / failed characters) (per tab)
- Current message info (station, subject, serial number) (per tab)
- Audio preview — stream decoder audio to browser (per tab, one channel at a time)

---

## MQTT / Home Assistant

If the UberSDR receiver has MQTT enabled, the decoder publishes through it
automatically. **There is nothing to configure and nothing to add to
`docker-compose.yml`** — the ingest endpoint is derived from `UBERSDR_URL`,
which the container already has.

When the receiver has MQTT turned off, or this container is not a recognised
addon, publishing is silently skipped and decoding is unaffected. Completed
messages go onto a bounded queue drained by a background thread, so a slow or
unreachable receiver can never stall the audio path.

### Topics

Published under the receiver's own topic prefix (`ubersdr/metrics` by default):

| Topic | Retained | Contents |
|-------|----------|----------|
| `…/addons/navtex/messages` | no | One message per completed transmission, **including the full decoded text** |
| `…/addons/navtex/summary` | yes | Counters and last-message state, every 30 s |
| `…/addons/navtex/status` | yes | `online` / `offline`, maintained by UberSDR |

A NAVTEX body is capped at 8 KiB by the parser and the ingest payload limit is
64 KiB, so the complete message is always published rather than a summary of it:

```json
{
  "id": "EA02",
  "freq_hz": 518000,
  "freq_khz": 518.0,
  "freq_label": "518 kHz",
  "channel": "International",
  "station": "E",
  "subject": "A",
  "subject_name": "Navigational warning",
  "serial": 2,
  "start_utc": "2026-08-18T14:00:00Z",
  "end_utc": "2026-08-18T14:01:30Z",
  "duration_s": 90,
  "char_count": 412,
  "snr_db": 18.25,
  "chars_clean_pct": 97.5,
  "chars_fec_pct": 2.0,
  "chars_failed_pct": 0.5,
  "text": "ZCZC EA02\nGALE WARNING\n…\nNNNN\n",
  "body": "GALE WARNING\n…",
  "station_name": "Niton",
  "station_country": "United Kingdom",
  "station_iso2": "gb",
  "station_confidence": 0.973,
  "station_level": "high",
  "station_distance_km": 622.4,
  "ident": { "key": "518:E:Niton", "conf": 0.973, "facts": ["…"], "…": "…" }
}
```

`station` stays the header letter. `station_*` name the identified transmitter
(`station_confidence` is 0–1, like `ident.conf`); they are `null` when the
message could not be attributed. Because the message is published once it has
been identified, a message that completes during the startup replay is published
when the replay finishes.

The retained summary carries `messages_total`, `messages_last_hour`,
`messages_by_frequency`, `channels_connected`, `dropped_events`, and the last
message broken out field by field (`last_message_id`, `last_station`,
`last_subject_name`, `last_frequency_khz`, `last_snr_db`, `last_station_name`,
`last_station_country`, `last_station_confidence`, `last_station_distance_km`, …).

Message text is sanitised to printable ASCII before publishing. This matters:
`CCIR476::code_to_char` returns a *negated* code for unassigned bit patterns and
`filter_print` forwards it, so a garbled SITOR-B decode puts bytes ≥ 0x80 into
the body. Those are not valid UTF-8, and a JSON string must be — unsanitised
they would reach the broker but fail to parse in Home Assistant. Unresolvable
characters appear as `?`.

### Home Assistant

When the receiver has Home Assistant discovery enabled, the decoder declares its
entities automatically and appears as its own device — nested under the
receiver, with a link straight through to this web UI:

| Entity | Type | Notes |
|--------|------|-------|
| Messages Received | sensor | Running total, `total_increasing` |
| Messages (Last Hour) | sensor | Rolling 60-minute count |
| Last Message | sensor | State is the message id (e.g. `EA02`); text and metadata as attributes |
| Last Subject | sensor | Subject letter decoded, e.g. `Navigational warning` |
| Last Station | sensor | Station identifier letter (unchanged) |
| Last Station Name | sensor | Identified transmitter, e.g. `Niton` |
| Last Station Country | sensor | e.g. `United Kingdom` |
| Last Station Confidence | sensor | % |
| Last Station Distance | sensor | km from the receiver |
| Last Message Time | sensor | Timestamp |
| Last Frequency | sensor | kHz |
| Last Message SNR | sensor | dB |
| Last Message Quality | sensor | Clean-character %, diagnostic |
| Receiver Link | binary sensor | Audio stream connected, diagnostic |

All fourteen read from the single retained `summary` topic, so Home Assistant has
values the moment it subscribes rather than waiting for the next transmission.

The message text is trimmed to 1 KiB in the *Last Message* attributes, because
Home Assistant keeps attributes in its recorder and an 8 KiB body on every state
change does not belong there. The complete text is always on the `messages`
topic.

### Overriding the endpoint

Only needed if the operator has changed `mqtt.addon_ingest.port` from its
default of 6926 on the receiver:

```yaml
environment:
  UBERSDR_INGEST_URL: "http://ubersdr:7000"
```

See `addon_mqtt.md` in the ka9q_ubersdr repository for the full ingest API.

### Tests

```bash
cmake --build build --target mqtt_selftest && ./build/src/mqtt_selftest
```

Covers endpoint derivation, the text sanitiser, message-payload JSON (including
hostile input: embedded quotes, backslashes and invalid UTF-8), subject decoding
and the health-response parser — and the SNR scale: that a measured strong
signal classifies as good, that the idle floor classifies as bad, that the
thresholds reach the generated page, and that a message recorded before the
audio protocol version 4 migration is converted onto the current scale rather
than read 34 dB high.

Station identification facts (`src/navtex_ident_facts.h`) — header-only, needs
nothing but a compiler:

```bash
cmake --build build --target ident_facts_test
ctest --test-dir build -R ident_facts --output-on-failure
```

`mqtt_selftest` also covers the identification plumbing: the ident JSON and its
MQTT fields, parsing `/api/description`, the sidecar rewrite (atomic, replaces
rather than duplicates, leaves the rest of the file untouched) and the worker end
to end on a scratch log directory — replay, a live completion, a provisional
answer and the pending state in `/api/history`.

Audio protocol version 4 conformance — decodes packet streams the **server's**
encoder produced and compares the SHA-256 of the decoded samples:

```bash
cmake --build build --target pcmv4_conformance
ctest --test-dir build --output-on-failure
```

Or without configuring the project at all (`src/pcm_v4.hpp` is header-only, so
this needs nothing but a C++ compiler):

```bash
./test/run.sh
```

This matters more than it looks. The version 4 predictor is backward adaptive:
the encoder and `src/pcm_v4.hpp` derive their filter taps independently from the
samples already seen and never exchange a coefficient, so an arithmetic
difference between the two returns plausible-sounding noise rather than an
error. `test/testdata/pcmv4_stream.bin` covers mono audio, silent packets
carrying no body, an escape to verbatim samples and a sample-rate change;
`pcmv4_rice_edge.bin` covers a Rice codeword whose unary run is exactly 63 bits,
where a 64-bit shift is zero in Go and undefined in C++; `pcmv4_scaled.bin`
covers the reduced-depth IQ profile, where a shift byte leads the body. NAVTEX
never meets that last one — it takes demodulated mono audio, and the server
offers the profile only on IQ — but the decoder implements it, so it is checked.

---

## SNR scale

The decoder speaks audio protocol version 4, in which the server sends the noise
figure as a **power in the demodulator passband (dBFS)**. Versions 1–3 sent the
noise **density** (dBFS/Hz), which is `10*log10(passband)` lower — 34.2 dB on the
2650 Hz filter this program requests — so every SNR shown here reads that much
lower than it did before, for the same signal.

Measured on 490 and 518 kHz (5 minutes each): an idle channel sits near −6 dB and
a real NAVTEX transmission around +16 dB, peaking at +25. The good/warn
boundaries and the SNR bar range are set from those distributions and live in one
place, `src/navtex_snr.h`; the web page's copies are generated from it.

Messages saved before the migration are stored on the old scale. They are
converted when the history API reads them, are marked as converted in what it
returns, and the history table says so on hover — so old and new messages are
comparable in one column. Sidecars written from now on carry an
`audio_protocol_version` stamp so no future migration has to infer their scale.

## Station identification

A NAVTEX header names its transmitter with one letter, and the letters are
re-used around the world: on 518 kHz `E` is Niton in the English Channel, but
also Samsun, Charleston and four more. Every message is therefore identified —
which transmitter, how sure, and why — from the station table
(`src/navtex_stations.h`, from Wikipedia's *List of Navtex stations*), the
receiver's position, the transmission schedule and what has been heard before.

**Receiver location.** Read from UberSDR's `/api/description`
(`receiver.gps.lat` / `lon`). Set the GPS position in UberSDR, or override it:

```yaml
environment:
  NAVTEX_RX_LAT: "56.0403"
  NAVTEX_RX_LON: "-3.3554"
```

Both are required; a half-set or invalid override is logged and ignored. If
UberSDR cannot be reached at startup the decoder retries after 2, 4, 8 and 16 s
(the history replay waits for this, about half a minute at most), then every
60 s; it refreshes the position every 6 hours.

> **Privacy.** Whatever position the decoder uses is public to anyone who can
> reach the web UI: `/api/stations` and `/api/analytics` show it (to 4
> decimals, about 11 m), every identification carries it (`"rx"` in
> `/api/ident`, `/api/latest`, the WebSocket and the sidecars), it goes out over
> MQTT, and the API answers cross-origin (`Access-Control-Allow-Origin: *`).
> Even without those fields it could be worked out from the per-message
> distances to known transmitters. If you set `NAVTEX_RX_LAT` / `NAVTEX_RX_LON`
> because you did not want to publish a GPS position in UberSDR, give a rounded
> one instead (two decimals is about 1 km — plenty for identification, which
> works at the scale of hundreds of km). A position that changes by more than 5 km re-identifies
the whole history, because it changes every answer. Without a position,
identification still runs but confidence is limited, and the web UI says so.

**At startup** the decoder replays the whole log history through the identifier
in chronological order (it learns from what it has heard) and writes each answer
into the message's `.json` sidecar as an `"ident"` member. A sidecar already
identified by the same algorithm version, station table and receiver position is
not rewritten. Live messages that complete during the replay are identified
after it. On 32,651 real messages, reading the history takes about a second and
rewriting every sidecar (the first start, or after the receiver moves) about six
more, on top of the identification itself.

**Every identification carries facts** — three to six plain sentences such as
*"7 stations use letter E on 518 kHz"*, *"Nearest is Niton, 622 km at 166°"*,
*"Started 1 min 42 s into E's 00:40 slot"* — produced in one place
(`src/navtex_ident_facts.h`) and shown verbatim by the web UI, the API and MQTT.

### API

| Endpoint | Returns |
|----------|---------|
| `GET /api/history` | As before, plus `st` per message: `{key, name, iso2, country_name, navarea, conf, level, dist_km, bearing, night}`; `st: null, st_pending: true` until identified; `{"level":"none","key":null}` when unattributed |
| `GET /api/ident?rel=<path>` | The full identification of one message (station, confidence, distance, members, alternatives, facts) |
| `GET /api/latest` | As before, each entry with an `ident` object |
| `GET /api/stations` | Receiver location, replay progress, and every table station with distance, schedule and `heard` statistics |
| `GET /flags/<iso2>.svg` | Country flag, embedded in the binary (works offline) |
| `GET /api/analytics[?from=&to=]` | Every identified message as compact columnar JSON (time, frequency, station, confidence, distance, bearing, path, SNR, duration, failed characters) for the Metrics view; `from`/`to` are optional unix seconds |

`/api/history` is served from an in-memory index rather than by re-reading every
sidecar, and the response is reused until something changes (or for at most
30 s): a few milliseconds instead of about a second for a few months of logs.

A station `key` is `freq_khz:letter:name` (e.g. `518:E:Niton`) — stable across
regenerations of the station table, unlike its index.

The browser WebSocket gains frame `0x05`: `[0x05][channel][JSON ident]`, sent
provisionally (`"provisional": true`) as soon as the header is decoded and
finally when the message completes.

**Maps.** The Stations tab, the Metrics view's Coverage tab and each message's
Identification panel draw maps with Leaflet (from cdnjs) over OpenStreetMap
tiles, darkened in the page. Set `NAVTEX_MAP_TILES` and `NAVTEX_MAP_ATTRIBUTION`
to use your own tile server instead. Without internet access the maps hide
themselves and the tables, charts and identification details still work.

### How it decides, and how far to trust it

Two independently built methods run side by side — a probabilistic model of
propagation, schedule and SNR, and a set of transparent rules — and their
answers are combined (`src/navtex_ident.h` explains both and the combiner). A
**high** answer needs both to agree, the message's transmission run to fit the
station's schedule, the station to be within reach, and a receiver position
that has been checked against what is heard; a disagreement is never high.

Measured with the harness in `test/ident/` (see its README): on 32,651 real
messages from one receiver, labelled independently from their text, no high
answer was wrong (0 of 8,263). On synthetic worlds generated to be unlike the
tuning data, 0.2–0.6 % of high answers were wrong — and up to about 1.5 % in a
world that happens to put an unlisted transmitter beside a busy listed one,
which is the remaining known weakness: a transmitter that is **missing from the station table** but near the
receiver, using a letter a listed station also uses, can be confidently named as
that listed station. "High" therefore assumes the table is complete near you —
reports of missing or wrong stations are very welcome.

A new install shows no "high" answers until about 100 messages have been heard
at the configured position (the position check); the startup replay of stored
history covers this on every later start.

### Data sources

- **Station table** — `src/navtex_stations.h` is generated by
  `tools/gen_navtex_stations.py` from a snapshot of Wikipedia's
  [List of Navtex stations](https://en.wikipedia.org/wiki/List_of_Navtex_stations)
  (`tools/navtex_stations.wiki`, CC BY-SA 4.0), plus `CORRECTIONS`, `REMOVALS`
  and `ADDITIONS` in the generator. Each correction says what was wrong and the
  evidence for it — receiver logs read for their text, published station
  notices, and the RXX signal database; only individual facts are used, never
  bulk data. Regenerate with
  `python3 -I tools/gen_navtex_stations.py tools/navtex_stations.wiki > src/navtex_stations.h`.
- **Flags** — SVGs from [flag-icons](https://github.com/lipis/flag-icons) 7.2.3
  (MIT, `tools/flags/LICENSE`), embedded by `tools/gen_navtex_countries.py`
  into `src/navtex_countries.h` and served from the binary. Flags are images
  rather than emoji because Windows has no flag emoji.

## Ports

| Port | Description |
|------|-------------|
| `6092` | Web UI (HTTP) |
| `6926` | Outbound only — UberSDR's MQTT ingest port on the sdr-network. Nothing to expose. |

---

## NAVTEX frequencies

| Frequency | Usage |
|-----------|-------|
| `518000` | International NAVTEX — English, worldwide (default channel 0) |
| `490000` | National/coastal NAVTEX — regional language (default channel 1) |
| `4209500` | HF NAVTEX (set via `NAVTEX_FREQ_1` / `NAVTEX_FREQ_2`) |

---

## Credits

- Dave Freese, W1HKJ for creating [fldigi](http://www.w1hkj.com/)
- Rik van Riel, AB1KW for the original NAVTEX decoder
- Franco Venturi for the [navtex](https://github.com/fventuri/navtex) library

---

## License

Licensed under the GNU GPL V3. See [LICENSE](LICENSE) for details.
