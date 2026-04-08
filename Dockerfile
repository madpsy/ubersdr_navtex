# syntax=docker/dockerfile:1
# ---------------------------------------------------------------------------
# Stage 1: build navtex_rx_from_ubersdr binary
# ---------------------------------------------------------------------------
FROM ubuntu:24.04 AS navtex-builder

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        git \
        libzstd-dev \
        libcurl4-openssl-dev \
        libssl-dev \
        pkg-config \
    && rm -rf /var/lib/apt/lists/*

# Clone IXWebSocket (no system package available on Ubuntu 24.04)
RUN git clone --depth 1 https://github.com/machinezone/IXWebSocket.git /ixwebsocket

WORKDIR /src
COPY src/ ./src/
COPY CMakeLists.txt .

RUN cmake -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DIXWS_ROOT=/ixwebsocket \
    && cmake --build build --parallel "$(nproc)" --target navtex_rx_from_ubersdr

# ---------------------------------------------------------------------------
# Stage 2: runtime image
# ---------------------------------------------------------------------------
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        libzstd1 \
        libcurl4 \
        libssl3 \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd -r -s /bin/false navtex \
    && mkdir -p /data/navtex_logs \
    && chown navtex:navtex /data/navtex_logs

COPY --from=navtex-builder /src/build/src/navtex_rx_from_ubersdr /usr/local/bin/navtex_rx_from_ubersdr
COPY --from=navtex-builder /src/build/src/liblibnavtex.so         /usr/local/lib/liblibnavtex.so
RUN ldconfig

# Copy entrypoint script (translates env vars to navtex_rx_from_ubersdr flags)
COPY entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

USER navtex

# Declare the log directory as a volume mount point
VOLUME ["/data/navtex_logs"]

# Expose the web UI port (default; override with WEB_PORT env var)
EXPOSE 6092

HEALTHCHECK --interval=30s --timeout=5s --retries=3 \
    CMD ["/usr/local/bin/navtex_rx_from_ubersdr"] || exit 0

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
