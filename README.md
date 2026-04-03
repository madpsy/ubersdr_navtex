# ubersdr_navtex

Automated NAVTEX receiver for [UberSDR](https://ubersdr.org) — connects to a remote UberSDR instance, tunes to a NAVTEX frequency, decodes the SITOR-B/NAVTEX signal, and serves decoded messages in a live web UI.

---

## How it works

```
UberSDR (remote SDR) ──► navtex_rx_from_ubersdr (C++) ──► decoded NAVTEX messages
                                    │
                                    └──► web UI  http://<host>:6092
```

- **`navtex_rx_from_ubersdr`** — C++ service that connects to UberSDR via WebSocket, streams demodulated audio, decodes NAVTEX (SITOR-B), and serves a real-time web UI
- **Web UI** — live decoded text with signal stats (dBFS, SNR, FEC quality, decoder state); served on port 6092

---

## Quick start (Docker — recommended)

```bash
curl -fsSL https://raw.githubusercontent.com/madpsy/ubersdr_navtex/master/install.sh | bash
```

This will:
1. Create `~/ubersdr/navtex/` and download `docker-compose.yml` + helper scripts
2. Pull the latest `madpsy/ubersdr_navtex` image
3. Start the service

Then edit `~/ubersdr/navtex/docker-compose.yml` to set your UberSDR URL and frequency, and run `./restart.sh`.

---

## Configuration

All configuration is via environment variables in `docker-compose.yml`:

| Variable | Default | Description |
|----------|---------|-------------|
| `UBERSDR_URL` | `http://172.20.0.1:8080` | UberSDR base URL |
| `NAVTEX_FREQ` | `518000` | NAVTEX carrier frequency in Hz (`518000` = international, `490000` = UK/coastal) |
| `WEB_PORT` | `6092` | Web UI port |

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

- Real-time decoded NAVTEX characters
- Signal level and SNR bars
- Decoder state (Searching / Syncing / Locked)
- FEC quality (clean / FEC-corrected / failed characters)
- Current message info (station, subject, serial number)
- Audio preview (stream decoder audio to browser)

---

## Ports

| Port | Description |
|------|-------------|
| `6092` | Web UI (HTTP) |

---

## NAVTEX frequencies

| Frequency | Usage |
|-----------|-------|
| `518000` | International NAVTEX (default) |
| `490000` | National/coastal NAVTEX (e.g. UK) |
| `4209500` | HF NAVTEX |

---

## Credits

- Dave Freese, W1HKJ for creating [fldigi](http://www.w1hkj.com/)
- Rik van Riel, AB1KW for the original NAVTEX decoder
- Franco Venturi for the [navtex](https://github.com/fventuri/navtex) library

---

## License

Licensed under the GNU GPL V3. See [LICENSE](LICENSE) for details.
