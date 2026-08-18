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

Requires: `build-essential`, `cmake`, `libzstd-dev`, `libcurl4-openssl-dev`, `libssl-dev`, `pkg-config`

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
  "body": "GALE WARNING\n…"
}
```

The retained summary carries `messages_total`, `messages_last_hour`,
`messages_by_frequency`, `channels_connected`, `dropped_events`, and the last
message broken out field by field (`last_message_id`, `last_station`,
`last_subject_name`, `last_frequency_khz`, `last_snr_db`, …).

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
| Last Station | sensor | Station identifier letter |
| Last Message Time | sensor | Timestamp |
| Last Frequency | sensor | kHz |
| Last Message SNR | sensor | dB |
| Last Message Quality | sensor | Clean-character %, diagnostic |
| Receiver Link | binary sensor | Audio stream connected, diagnostic |

All ten read from the single retained `summary` topic, so Home Assistant has
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
and the health-response parser.

---

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
