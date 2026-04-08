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

## Ports

| Port | Description |
|------|-------------|
| `6092` | Web UI (HTTP) |

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
