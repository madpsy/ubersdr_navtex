/* -*- c++ -*- */
/*
 * Copyright 2024 Contributors.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Decode NAVTEX from a ka9q_ubersdr server.
 *
 * Usage:
 *   navtex_rx_from_ubersdr <http://host:port> [frequency_hz] [--web-port N]
 *
 * Examples:
 *   navtex_rx_from_ubersdr http://192.168.1.10:8073
 *   navtex_rx_from_ubersdr http://192.168.1.10:8073 490000
 *   navtex_rx_from_ubersdr https://sdr.example.com:443 518000 --web-port 8080
 *
 * The server URL must use http:// or https://.  The WebSocket connection
 * is made to the same host/port using ws:// or wss:// respectively.
 *
 * frequency_hz is the NAVTEX carrier frequency (default 518000 Hz).
 * The dial frequency sent to the server is (frequency_hz - 500) so that
 * the NAVTEX audio tone falls at 500 Hz in the USB passband, matching
 * the navtex_rx default centre frequency (dflt_center_freq = 500.0 Hz).
 *
 * Audio format requested: pcm-zstd, version 2.
 * The server delivers 12 kHz mono signed 16-bit big-endian PCM inside
 * zstd-compressed WebSocket binary frames.
 *
 * Protocol sequence:
 *   1. POST http(s)://host:port/connection  {"user_session_id":"<uuid>"}
 *   2. GET  http(s)://host:port/api/description  (informational)
 *   3. WebSocket ws(s)://host:port/ws?frequency=<dial>&mode=usb
 *              &format=pcm-zstd&version=2&user_session_id=<uuid>
 *   4. Send {"type":"get_status"} on open
 *   5. Keepalive {"type":"ping"} every 30 s
 *   6. On binary frame: zstd-decompress → strip header → byteswap → process_data()
 *
 * Web UI:
 *   An embedded HTTP + WebSocket server runs on --web-port (default 6040).
 *   Browse to http://localhost:6040/ to see real-time decoded characters.
 *   Each decoded character is broadcast as a plain text WebSocket frame.
 */

#include "navtex_rx.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include <arpa/inet.h>   /* ntohs */

#include <curl/curl.h>
#include <zstd.h>

#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include <ixwebsocket/IXHttpServer.h>

/* ------------------------------------------------------------------ */
/* Tiny UUID v4 generator (no external dependency)                     */
/* ------------------------------------------------------------------ */
#include <random>
static std::string make_uuid4()
{
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;

    uint64_t hi = dist(gen);
    uint64_t lo = dist(gen);

    /* Set version 4 and variant bits */
    hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

    char buf[37];
    snprintf(buf, sizeof(buf),
             "%08x-%04x-%04x-%04x-%012llx",
             (unsigned)(hi >> 32),
             (unsigned)((hi >> 16) & 0xFFFF),
             (unsigned)(hi & 0xFFFF),
             (unsigned)(lo >> 48),
             (unsigned long long)(lo & 0x0000FFFFFFFFFFFFULL));
    return std::string(buf);
}

/* ------------------------------------------------------------------ */
/* libcurl helpers                                                      */
/* ------------------------------------------------------------------ */
struct CurlBuf {
    std::string data;
};

static size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    auto *buf = static_cast<CurlBuf *>(userdata);
    buf->data.append(ptr, size * nmemb);
    return size * nmemb;
}

/* POST json_body to url, return HTTP response body in out_body.
 * Returns the HTTP status code, or -1 on transport error. */
static long http_post_json(const std::string &url,
                           const std::string &json_body,
                           std::string &out_body)
{
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    CurlBuf buf;
    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "User-Agent: navtex_rx_from_ubersdr/1.0");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L); /* allow self-signed */
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = -1;
    if (res == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    else
        fprintf(stderr, "curl POST error: %s\n", curl_easy_strerror(res));

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    out_body = buf.data;
    return http_code;
}

/* GET url, return body in out_body. Returns HTTP status or -1. */
static long http_get(const std::string &url, std::string &out_body)
{
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    CurlBuf buf;
    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "User-Agent: navtex_rx_from_ubersdr/1.0");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = -1;
    if (res == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    else
        fprintf(stderr, "curl GET error: %s\n", curl_easy_strerror(res));

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    out_body = buf.data;
    return http_code;
}

/* ------------------------------------------------------------------ */
/* PCM-zstd frame decoder                                              */
/* ------------------------------------------------------------------ */

/*
 * Magic values match the Python reference client (struct.unpack('<H', ...)):
 *   Full header:    0x5043  (bytes on wire: 0x43 'C', 0x50 'P' → LE uint16 = 0x5043)
 *   Minimal header: 0x504D  (bytes on wire: 0x4D 'M', 0x50 'P' → LE uint16 = 0x504D)
 *
 * Header layout (all multi-byte fields little-endian except PCM samples):
 *   Full v1 (29 bytes): magic(2) ver(1) fmt(1) rtp_ts(8) wall(8) rate(4) ch(1) rsvd(4)
 *   Full v2 (37 bytes): magic(2) ver(1) fmt(1) rtp_ts(8) wall(8) rate(4) ch(1) bbpwr(4) noise(4) rsvd(4)
 *   Minimal (13 bytes): magic(2) ver(1) rtp_ts(8) rsvd(2)
 *   PCM payload: big-endian int16 samples
 */
static const uint16_t MAGIC_FULL    = 0x5043; /* matches Python: struct.unpack('<H', b'\x43\x50') */
static const uint16_t MAGIC_MINIMAL = 0x504D; /* matches Python: struct.unpack('<H', b'\x4D\x50') */

/* Full header v1 = 29 bytes, v2 = 37 bytes */
static const size_t FULL_HDR_V1 = 29;
static const size_t FULL_HDR_V2 = 37;
static const size_t MIN_HDR     = 13;

struct PcmMeta {
    uint32_t sample_rate    = 0;
    uint8_t  channels       = 0;
    float    baseband_power = 0.0f; /* dBFS, v2 header only */
    float    noise_density  = 0.0f; /* dBFS/Hz, v2 header only */
    bool     has_signal_quality = false;
};

/* Read a little-endian uint32 from a byte pointer */
static inline uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Read a little-endian uint16 from a byte pointer */
static inline uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

/*
 * Decompress one zstd-compressed PCM frame.
 * Fills meta (from full header), appends little-endian int16 samples to pcm_le.
 * Returns true on success.
 */
static bool decode_pcm_zstd_frame(const void *compressed, size_t compressed_len,
                                   std::vector<uint8_t> &decomp_buf,
                                   PcmMeta &meta,
                                   std::vector<int16_t> &pcm_le)
{
    /* Decompress */
    size_t frame_size = ZSTD_getFrameContentSize(compressed, compressed_len);
    if (frame_size == ZSTD_CONTENTSIZE_ERROR ||
        frame_size == ZSTD_CONTENTSIZE_UNKNOWN) {
        /* Try a fixed upper bound */
        frame_size = compressed_len * 8;
    }
    if (decomp_buf.size() < frame_size)
        decomp_buf.resize(frame_size);

    size_t actual = ZSTD_decompress(decomp_buf.data(), decomp_buf.size(),
                                    compressed, compressed_len);
    if (ZSTD_isError(actual)) {
        fprintf(stderr, "zstd error: %s\n", ZSTD_getErrorName(actual));
        return false;
    }

    const uint8_t *p   = decomp_buf.data();
    const uint8_t *end = p + actual;

    if (actual < 2) return false;

    /* Magic is little-endian uint16 */
    uint16_t magic = read_le16(p);
    size_t   hdr_size = 0;

    if (magic == MAGIC_FULL) {
        /* Full header: version byte at offset 2 determines size */
        if (actual < 3) return false;
        uint8_t version = p[2];
        hdr_size = (version >= 2) ? FULL_HDR_V2 : FULL_HDR_V1;
        if (actual < hdr_size) return false;

        /* sample_rate at offset 20 (little-endian uint32) */
        meta.sample_rate = read_le32(p + 20);
        /* channels at offset 24 (uint8) */
        meta.channels    = p[24];

        /* v2: baseband_power at offset 25, noise_density at offset 29 (LE float32) */
        if (version >= 2) {
            uint32_t bb_bits, nd_bits;
            /* Ensure little-endian interpretation on all platforms */
            bb_bits = (uint32_t)p[25] | ((uint32_t)p[26]<<8) | ((uint32_t)p[27]<<16) | ((uint32_t)p[28]<<24);
            nd_bits = (uint32_t)p[29] | ((uint32_t)p[30]<<8) | ((uint32_t)p[31]<<16) | ((uint32_t)p[32]<<24);
            memcpy(&meta.baseband_power, &bb_bits, 4);
            memcpy(&meta.noise_density,  &nd_bits, 4);
            meta.has_signal_quality = true;
        } else {
            meta.has_signal_quality = false;
        }

    } else if (magic == MAGIC_MINIMAL) {
        hdr_size = MIN_HDR;
        if (actual < hdr_size) return false;
        /* Minimal header reuses last known sample_rate/channels */
    } else {
        fprintf(stderr, "unknown PCM magic: 0x%04x\n", magic);
        return false;
    }

    /* PCM payload: big-endian int16, convert to host (little-endian) */
    const uint8_t *pcm_start = p + hdr_size;
    size_t         pcm_bytes = (size_t)(end - pcm_start);
    size_t         n_samples = pcm_bytes / 2;

    pcm_le.resize(n_samples);
    for (size_t i = 0; i < n_samples; i++) {
        int16_t be;
        memcpy(&be, pcm_start + i * 2, 2);
        pcm_le[i] = (int16_t)ntohs((uint16_t)be);
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* URL helpers                                                          */
/* ------------------------------------------------------------------ */
static std::string http_to_ws(const std::string &http_url)
{
    if (http_url.substr(0, 8) == "https://")
        return "wss://" + http_url.substr(8);
    if (http_url.substr(0, 7) == "http://")
        return "ws://" + http_url.substr(7);
    return http_url; /* pass through if already ws:// */
}

/* Strip trailing slash */
static std::string strip_slash(const std::string &s)
{
    if (!s.empty() && s.back() == '/')
        return s.substr(0, s.size() - 1);
    return s;
}

/* ------------------------------------------------------------------ */
/* Browser WebSocket broadcast hub                                      */
/* ------------------------------------------------------------------ */
/*
 * BroadcastHub holds a pointer to the ix::HttpServer (which inherits from
 * WebSocketServer and maintains its own client set internally).
 * send_char() calls server->getClients() to get the current set of connected
 * browser WebSocket clients and sends each one a single-character text frame.
 */
struct BroadcastHub {
    ix::HttpServer *server = nullptr;

    /* Per-client audio-preview subscription set */
    std::mutex                audio_subs_mu;
    std::set<ix::WebSocket *> audio_subs;

    void subscribe_audio(ix::WebSocket *ws) {
        std::lock_guard<std::mutex> lk(audio_subs_mu);
        audio_subs.insert(ws);
    }
    void unsubscribe_audio(ix::WebSocket *ws) {
        std::lock_guard<std::mutex> lk(audio_subs_mu);
        audio_subs.erase(ws);
    }

    /*
     * Broadcast a PCM audio frame (type 0x04) to subscribed clients only.
     * Frame layout: [0x04][int16 LE samples...]
     * Sample rate matches the decoder's current sample rate (typically 12000 Hz).
     */
    void send_audio(const std::vector<int16_t> &pcm)
    {
        if (!server || pcm.empty()) return;
        std::lock_guard<std::mutex> lk(audio_subs_mu);
        if (audio_subs.empty()) return;

        /* Build frame: 1-byte type + raw int16 LE samples */
        std::string frame(1 + pcm.size() * 2, '\0');
        frame[0] = 0x04;
        memcpy(&frame[1], pcm.data(), pcm.size() * 2);

        /* Send only to subscribed clients that are still connected */
        auto clients = server->getClients();
        for (auto &ws : clients) {
            if (audio_subs.count(ws.get()))
                ws->sendBinary(frame);
        }
    }

    /* Broadcast a single character to all connected browser clients */
    void send_char(char c)
    {
        if (!server) return;
        std::string s(1, c);
        for (auto &ws : server->getClients())
            ws->sendText(s);
    }

    /*
     * Broadcast a compact binary stats frame to all connected browser clients.
     *
     * Frame layout (20 bytes, all LE):
     *   [0]     0x02  — stats frame type marker (v2)
     *   [1-4]   float32  baseband_power (dBFS)
     *   [5-8]   float32  noise_density  (dBFS/Hz)
     *   [9]     uint8    flags: bit0=has_signal_quality, bit1=active
     *   [10-11] uint16   sample_rate_khz (Hz / 1000, rounded)
     *   [12]    uint8    decoder_state: 0=SYNC_SETUP, 1=SYNC, 2=READ_DATA
     *   [13]    uint8    chars_clean   (rolling window, last 50 chars)
     *   [14]    uint8    chars_fec     (rolling window)
     *   [15]    uint8    chars_failed  (rolling window)
     *   [16-19] float32  signal_confidence (m_average_prompt_signal)
     *
     * The browser uses DataView to parse; no JSON overhead.
     */
    void send_stats(uint32_t sample_rate,
                    bool has_signal_quality,
                    float baseband_power,
                    float noise_density,
                    bool active,
                    const DecoderStats &dec)
    {
        if (!server) return;

        uint8_t frame[20] = {};
        frame[0] = 0x02; /* stats type v2 */

        /* float32 LE: copy bytes directly (host is LE on x86/ARM Linux) */
        memcpy(&frame[1], &baseband_power, 4);
        memcpy(&frame[5], &noise_density,  4);

        frame[9]  = (has_signal_quality ? 0x01 : 0x00) |
                    (active             ? 0x02 : 0x00);

        uint16_t rate_khz = (uint16_t)(sample_rate / 1000);
        frame[10] = (uint8_t)(rate_khz & 0xFF);
        frame[11] = (uint8_t)(rate_khz >> 8);

        frame[12] = (uint8_t)dec.state;
        frame[13] = (uint8_t)(dec.chars_clean  > 255 ? 255 : dec.chars_clean);
        frame[14] = (uint8_t)(dec.chars_fec    > 255 ? 255 : dec.chars_fec);
        frame[15] = (uint8_t)(dec.chars_failed > 255 ? 255 : dec.chars_failed);

        float sig_conf = (float)dec.signal_confidence;
        memcpy(&frame[16], &sig_conf, 4);

        std::string msg(reinterpret_cast<char *>(frame), sizeof(frame));
        for (auto &ws : server->getClients())
            ws->sendBinary(msg);
    }

    /*
     * Broadcast a message-info frame (type 0x03) to all browser clients.
     *
     * Frame layout (5 bytes, fixed):
     *   [0]     0x03   — message-info frame type
     *   [1]     uint8  — flags: bit0=in_message, bit1=complete
     *   [2]     uint8  — station letter (origin), 0 if unknown
     *   [3]     uint8  — subject letter, 0 if unknown
     *   [4]     uint8  — serial number (0-99), 0xFF if unknown
     */
    void send_msg_info(bool in_message, bool complete,
                       char station, char subject, int serial)
    {
        if (!server) return;

        uint8_t frame[5];
        frame[0] = 0x03;
        frame[1] = (uint8_t)((in_message ? 0x01 : 0x00) |
                              (complete   ? 0x02 : 0x00));
        frame[2] = (uint8_t)(station ? station : 0);
        frame[3] = (uint8_t)(subject ? subject : 0);
        frame[4] = (uint8_t)(serial >= 0 && serial <= 99 ? serial : 0xFF);

        std::string msg(reinterpret_cast<char *>(frame), sizeof(frame));
        for (auto &ws : server->getClients())
            ws->sendBinary(msg);
    }
};

/* Global hub — used by the fopencookie write callback */
static BroadcastHub *g_hub = nullptr;

/* Timestamp of last decoded character (seconds since epoch, atomic double via int64) */
static std::atomic<int64_t> g_last_char_ms{0};

/* ------------------------------------------------------------------ */
/* NAVTEX message framing parser (ZCZC / NNNN)                         */
/* ------------------------------------------------------------------ */
/*
 * Watches the raw character stream for ZCZC and NNNN markers.
 * State is kept in a plain struct so it persists across calls.
 *
 * NAVTEX header format:  ZCZC B1 B2 nn
 *   B1 = station/origin letter  (e.g. 'E')
 *   B2 = subject letter         (e.g. 'A' = nav warning)
 *   nn = two-digit serial       (e.g. "01")
 */
struct MsgParser {
    /* Sliding window to detect 4-char markers */
    char     window[4] = {};
    int      win_pos   = 0;   /* next write position (ring) */

    /* Current message state */
    bool     in_msg    = false;
    bool     complete  = false;
    char     station   = 0;
    char     subject   = 0;
    int      serial    = -1;

    /* Header parsing: we need the 4 chars after ZCZC */
    int      hdr_chars = 0;   /* how many post-ZCZC chars consumed */
    char     hdr_buf[4] = {}; /* B1 B2 d1 d2 */

    void feed(char c)
    {
        /* Advance sliding window */
        window[win_pos & 3] = c;
        win_pos++;

        /* Read window in order */
        auto w = [&](int i) { return window[(win_pos + i) & 3]; };
        bool is_zczc = (w(0)=='Z' && w(1)=='C' && w(2)=='Z' && w(3)=='C');
        bool is_nnnn = (w(0)=='N' && w(1)=='N' && w(2)=='N' && w(3)=='N');

        if (is_zczc) {
            /* Start of a new message — reset state */
            in_msg    = true;
            complete  = false;
            station   = 0;
            subject   = 0;
            serial    = -1;
            hdr_chars = 0;
            return;
        }

        if (is_nnnn && in_msg) {
            /* End of message */
            complete = true;
            in_msg   = false;
            return;
        }

        if (in_msg && hdr_chars < 4) {
            /* Parse the 4 header chars immediately after ZCZC:
             * skip whitespace that separates ZCZC from B1B2nn */
            if (c != ' ' && c != '\r' && c != '\n') {
                hdr_buf[hdr_chars++] = c;
                if (hdr_chars == 4) {
                    station = hdr_buf[0];
                    subject = hdr_buf[1];
                    int d1  = hdr_buf[2] - '0';
                    int d2  = hdr_buf[3] - '0';
                    if (d1 >= 0 && d1 <= 9 && d2 >= 0 && d2 <= 9)
                        serial = d1 * 10 + d2;
                }
            }
        }
    }
} g_msg_parser;

/* ------------------------------------------------------------------ */
/* Custom FILE* that broadcasts each character to browser clients       */
/* ------------------------------------------------------------------ */
/*
 * We use fopencookie() (Linux glibc) to create a FILE* whose write
 * function calls BroadcastHub::send_char() for every byte written.
 * This FILE* is passed as rawfile to navtex_rx so that put_rx_char()
 * broadcasts each decoded character in real time.
 */
static ssize_t ws_cookie_write(void * /*cookie*/, const char *buf, size_t size)
{
    if (g_hub) {
        /* Record time of last decoded character (ms since epoch) */
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        g_last_char_ms.store((int64_t)now_ms);

        for (size_t i = 0; i < size; i++) {
            char c = buf[i];

            /* Feed character into NAVTEX framing parser */
            bool was_in  = g_msg_parser.in_msg;
            bool was_cmp = g_msg_parser.complete;
            char was_sta = g_msg_parser.station;
            char was_sub = g_msg_parser.subject;
            int  was_ser = g_msg_parser.serial;

            g_msg_parser.feed(c);

            /* Broadcast a msg-info frame on any state change */
            bool changed = (g_msg_parser.in_msg   != was_in)  ||
                           (g_msg_parser.complete  != was_cmp) ||
                           (g_msg_parser.station   != was_sta) ||
                           (g_msg_parser.subject   != was_sub) ||
                           (g_msg_parser.serial    != was_ser);
            if (changed) {
                g_hub->send_msg_info(g_msg_parser.in_msg,
                                     g_msg_parser.complete,
                                     g_msg_parser.station,
                                     g_msg_parser.subject,
                                     g_msg_parser.serial);
            }

            /* Also echo to real stdout */
            putchar(c);
            g_hub->send_char(c);
        }
        fflush(stdout);
    }
    return (ssize_t)size;
}

static FILE *make_broadcast_file()
{
    static cookie_io_functions_t fns = {};
    fns.write = ws_cookie_write;
    return fopencookie(nullptr, "w", fns);
}

/* ------------------------------------------------------------------ */
/* Embedded HTML page                                                   */
/* ------------------------------------------------------------------ */
static std::string make_html_page(const std::string &sdr_url,
                                  long carrier_hz)
{
    double freq_khz = carrier_hz / 1000.0;
    char freq_str[32];
    snprintf(freq_str, sizeof(freq_str), "%.1f kHz", freq_khz);

    std::string html =
        "<!DOCTYPE html>\n"
        "<html lang=\"en\">\n"
        "<head>\n"
        "<meta charset=\"UTF-8\">\n"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
        "<title>NAVTEX Decoder</title>\n"
        "<style>\n"
        "  * { box-sizing: border-box; margin: 0; padding: 0; }\n"
        "  body {\n"
        "    font-family: 'Courier New', Courier, monospace;\n"
        "    background: #1a1a2e;\n"
        "    color: #e0e0e0;\n"
        "    display: flex;\n"
        "    flex-direction: column;\n"
        "    height: 100vh;\n"
        "    padding: 16px;\n"
        "    gap: 10px;\n"
        "  }\n"
        "  header {\n"
        "    background: #16213e;\n"
        "    border: 1px solid #0f3460;\n"
        "    border-radius: 6px;\n"
        "    padding: 10px 16px;\n"
        "    display: flex;\n"
        "    align-items: center;\n"
        "    gap: 20px;\n"
        "    flex-wrap: wrap;\n"
        "  }\n"
        "  header h1 { font-size: 1.1rem; color: #e94560; letter-spacing: 2px; text-transform: uppercase; }\n"
        "  .info-item { display: flex; flex-direction: column; gap: 2px; }\n"
        "  .info-label { font-size: 0.62rem; color: #888; text-transform: uppercase; letter-spacing: 1px; }\n"
        "  .info-value { font-size: 0.9rem; color: #53d8fb; }\n"
        "  #status-dot {\n"
        "    width: 9px; height: 9px; border-radius: 50%; background: #555;\n"
        "    display: inline-block; margin-right: 5px; transition: background 0.3s;\n"
        "  }\n"
        "  #status-dot.connected { background: #4caf50; }\n"
        "  #status-dot.disconnected { background: #e94560; }\n"
        "  /* Signal stats bar */\n"
        "  #stats-bar {\n"
        "    background: #16213e;\n"
        "    border: 1px solid #0f3460;\n"
        "    border-radius: 6px;\n"
        "    padding: 8px 16px;\n"
        "    display: flex;\n"
        "    align-items: center;\n"
        "    gap: 20px;\n"
        "    flex-wrap: wrap;\n"
        "    font-size: 0.82rem;\n"
        "  }\n"
        "  .stat { display: flex; flex-direction: column; gap: 2px; min-width: 80px; }\n"
        "  .stat-label { font-size: 0.6rem; color: #888; text-transform: uppercase; letter-spacing: 1px; }\n"
        "  .stat-value { color: #53d8fb; }\n"
        "  .stat-value.good  { color: #4caf50; }\n"
        "  .stat-value.warn  { color: #ffeb3b; }\n"
        "  .stat-value.bad   { color: #e94560; }\n"
        "  .stat-value.dim   { color: #555; }\n"
        "  /* Signal level bar */\n"
        "  #sig-bar-wrap {\n"
        "    flex: 1; min-width: 120px;\n"
        "    display: flex; flex-direction: column; gap: 3px;\n"
        "  }\n"
        "  #sig-bar-track {\n"
        "    height: 8px; background: #0d0d1a; border-radius: 4px; overflow: hidden;\n"
        "    border: 1px solid #0f3460;\n"
        "  }\n"
        "  #sig-bar-fill {\n"
        "    height: 100%; width: 0%; border-radius: 4px;\n"
        "    background: linear-gradient(90deg, #1a6b1a, #4caf50, #ffeb3b, #e94560);\n"
        "  }\n"
        "  /* SNR bar */\n"
        "  #snr-bar-wrap {\n"
        "    flex: 1; min-width: 120px;\n"
        "    display: flex; flex-direction: column; gap: 3px;\n"
        "  }\n"
        "  #snr-bar-track {\n"
        "    height: 8px; background: #0d0d1a; border-radius: 4px; overflow: hidden;\n"
        "    border: 1px solid #0f3460;\n"
        "  }\n"
        "  #snr-bar-fill {\n"
        "    height: 100%; width: 0%; border-radius: 4px; transition: width 0.3s, background 0.3s;\n"
        "    background: #4caf50;\n"
        "  }\n"
        "  /* FEC quality bar */\n"
        "  #fec-bar-wrap {\n"
        "    flex: 1; min-width: 120px;\n"
        "    display: flex; flex-direction: column; gap: 3px;\n"
        "  }\n"
        "  #fec-bar-track {\n"
        "    height: 8px; background: #0d0d1a; border-radius: 4px; overflow: hidden;\n"
        "    border: 1px solid #0f3460; display: flex;\n"
        "  }\n"
        "  #fec-bar-clean  { height: 100%; background: #4caf50; transition: width 0.3s; }\n"
        "  #fec-bar-fec    { height: 100%; background: #ffeb3b; transition: width 0.3s; }\n"
        "  #fec-bar-failed { height: 100%; background: #e94560; transition: width 0.3s; }\n"
        "  #act-dot {\n"
        "    width: 9px; height: 9px; border-radius: 50%; background: #333;\n"
        "    display: inline-block; margin-right: 5px; transition: background 0.5s;\n"
        "  }\n"
        "  #act-dot.active { background: #4caf50; box-shadow: 0 0 6px #4caf50; }\n"
        "  #output {\n"
        "    flex: 1;\n"
        "    background: #0d0d1a;\n"
        "    border: 1px solid #0f3460;\n"
        "    border-radius: 6px;\n"
        "    padding: 14px;\n"
        "    overflow-y: auto;\n"
        "    white-space: pre-wrap;\n"
        "    word-break: break-all;\n"
        "    font-size: 0.95rem;\n"
        "    line-height: 1.5;\n"
        "    color: #b0ffb0;\n"
        "  }\n"
        "  #output .dim { color: #555; }\n"
        "  /* Current message info bar */\n"
        "  #msg-bar {\n"
        "    background: #16213e;\n"
        "    border: 1px solid #0f3460;\n"
        "    border-radius: 6px;\n"
        "    padding: 8px 16px;\n"
        "    display: flex;\n"
        "    align-items: flex-start;\n"
        "    gap: 20px;\n"
        "    flex-wrap: wrap;\n"
        "    font-size: 0.82rem;\n"
        "    transition: opacity 0.4s;\n"
        "  }\n"
        "  #msg-bar.idle { opacity: 0.4; }\n"
        "  #msg-bar .stat { min-width: 60px; }\n"
        "  #msg-status-dot {\n"
        "    width: 9px; height: 9px; border-radius: 50%; background: #333;\n"
        "    display: inline-block; margin-right: 5px; transition: background 0.3s;\n"
        "  }\n"
        "  #msg-status-dot.receiving { background: #ffeb3b; box-shadow: 0 0 6px #ffeb3b; }\n"
        "  #msg-status-dot.complete  { background: #4caf50; box-shadow: 0 0 6px #4caf50; }\n"
        "  /* Audio preview button */\n"
        "  #audio-btn {\n"
        "    background: #0f3460; border: 1px solid #1a6b8a; color: #53d8fb;\n"
        "    border-radius: 4px; padding: 4px 10px; cursor: pointer;\n"
        "    font-family: inherit; font-size: 0.78rem; letter-spacing: 0.5px;\n"
        "    transition: background 0.2s, color 0.2s;\n"
        "    white-space: nowrap;\n"
        "  }\n"
        "  #audio-btn:hover { background: #1a6b8a; }\n"
        "  #audio-btn.active { background: #1a6b1a; border-color: #4caf50; color: #4caf50; }\n"
        "  #audio-btn.active:hover { background: #2a8a2a; }\n"
        "</style>\n"
        "</head>\n"
        "<body>\n"
        "<header>\n"
        "  <h1>&#x1F4E1; NAVTEX</h1>\n"
        "  <div class=\"info-item\">\n"
        "    <span class=\"info-label\">SDR Server</span>\n"
        "    <span class=\"info-value\">" + sdr_url + "</span>\n"
        "  </div>\n"
        "  <div class=\"info-item\">\n"
        "    <span class=\"info-label\">Frequency</span>\n"
        "    <span class=\"info-value\">" + std::string(freq_str) + "</span>\n"
        "  </div>\n"
        "  <div class=\"info-item\">\n"
        "    <span class=\"info-label\">Status</span>\n"
        "    <span class=\"info-value\"><span id=\"status-dot\"></span><span id=\"status-text\">Connecting&hellip;</span></span>\n"
        "  </div>\n"
        "  <div class=\"info-item\" style=\"margin-left:auto\">\n"
        "    <button id=\"audio-btn\" title=\"Stream audio from decoder to browser\">&#x1F50A; Audio Preview</button>\n"
        "  </div>\n"
        "</header>\n"
        "<div id=\"stats-bar\">\n"
        "  <div class=\"stat\">\n"
        "    <span class=\"stat-label\">Signal (dBFS)</span>\n"
        "    <span class=\"stat-value\" id=\"bb-val\">&mdash;</span>\n"
        "  </div>\n"
        "  <div class=\"stat\">\n"
        "    <span class=\"stat-label\">Noise (dBFS/Hz)</span>\n"
        "    <span class=\"stat-value\" id=\"nd-val\">&mdash;</span>\n"
        "  </div>\n"
        "  <div class=\"stat\">\n"
        "    <span class=\"stat-label\">Sample Rate</span>\n"
        "    <span class=\"stat-value\" id=\"rate-val\">&mdash;</span>\n"
        "  </div>\n"
        "  <div class=\"stat\">\n"
        "    <span class=\"stat-label\">Decoder</span>\n"
        "    <span class=\"stat-value\" id=\"dec-state\">&mdash;</span>\n"
        "  </div>\n"
        "  <div class=\"stat\">\n"
        "    <span class=\"stat-label\">FEC Rate</span>\n"
        "    <span class=\"stat-value\" id=\"fec-val\">&mdash;</span>\n"
        "  </div>\n"
        "  <div class=\"stat\">\n"
        "    <span class=\"stat-label\">Errors</span>\n"
        "    <span class=\"stat-value\" id=\"err-val\">&mdash;</span>\n"
        "  </div>\n"
        "  <div class=\"stat\">\n"
        "    <span class=\"stat-label\">Decoding</span>\n"
        "    <span class=\"stat-value\"><span id=\"act-dot\"></span><span id=\"act-text\">Idle</span></span>\n"
        "  </div>\n"
        "  <div class=\"stat\">\n"
        "    <span class=\"stat-label\">SNR (dB)</span>\n"
        "    <span class=\"stat-value\" id=\"snr-val\">&mdash;</span>\n"
        "  </div>\n"
        "  <div id=\"sig-bar-wrap\">\n"
        "    <span class=\"stat-label\">Signal Level</span>\n"
        "    <div id=\"sig-bar-track\"><div id=\"sig-bar-fill\"></div></div>\n"
        "  </div>\n"
        "  <div id=\"snr-bar-wrap\">\n"
        "    <span class=\"stat-label\">SNR Level</span>\n"
        "    <div id=\"snr-bar-track\"><div id=\"snr-bar-fill\"></div></div>\n"
        "  </div>\n"
        "  <div id=\"fec-bar-wrap\">\n"
        "    <span class=\"stat-label\">Quality (clean / fec / err)</span>\n"
        "    <div id=\"fec-bar-track\">\n"
        "      <div id=\"fec-bar-clean\"  style=\"width:0%\"></div>\n"
        "      <div id=\"fec-bar-fec\"    style=\"width:0%\"></div>\n"
        "      <div id=\"fec-bar-failed\" style=\"width:0%\"></div>\n"
        "    </div>\n"
        "  </div>\n"
        "</div>\n"
        "<div id=\"msg-bar\" class=\"idle\">\n"
        "  <div class=\"stat\">\n"
        "    <span class=\"stat-label\">Message</span>\n"
        "    <span class=\"stat-value\"><span id=\"msg-status-dot\"></span><span id=\"msg-status-text\">Idle</span></span>\n"
        "  </div>\n"
        "  <div class=\"stat\">\n"
        "    <span class=\"stat-label\">Station</span>\n"
        "    <span class=\"stat-value\" id=\"msg-station\">&mdash;</span>\n"
        "  </div>\n"
        "  <div class=\"stat\">\n"
        "    <span class=\"stat-label\">Subject</span>\n"
        "    <span class=\"stat-value\" id=\"msg-subject\">&mdash;</span>\n"
        "  </div>\n"
        "  <div class=\"stat\">\n"
        "    <span class=\"stat-label\">Serial</span>\n"
        "    <span class=\"stat-value\" id=\"msg-serial\">&mdash;</span>\n"
        "  </div>\n"
        "</div>\n"
        "<div id=\"output\"><span class=\"dim\">Waiting for signal&hellip;</span></div>\n"
        "<script>\n"
        "  const out      = document.getElementById('output');\n"
        "  const dot      = document.getElementById('status-dot');\n"
        "  const stxt     = document.getElementById('status-text');\n"
        "  const bbEl     = document.getElementById('bb-val');\n"
        "  const ndEl     = document.getElementById('nd-val');\n"
        "  const rateEl   = document.getElementById('rate-val');\n"
        "  const actDot   = document.getElementById('act-dot');\n"
        "  const actTxt   = document.getElementById('act-text');\n"
        "  const sigFill  = document.getElementById('sig-bar-fill');\n"
        "  const snrVal   = document.getElementById('snr-val');\n"
        "  const snrFill  = document.getElementById('snr-bar-fill');\n"
        "  const decState = document.getElementById('dec-state');\n"
        "  const fecVal   = document.getElementById('fec-val');\n"
        "  const errVal   = document.getElementById('err-val');\n"
        "  const fecClean = document.getElementById('fec-bar-clean');\n"
        "  const fecFec   = document.getElementById('fec-bar-fec');\n"
        "  const fecFail  = document.getElementById('fec-bar-failed');\n"
        "  /* Message info bar elements */\n"
        "  const msgBar      = document.getElementById('msg-bar');\n"
        "  const msgStatDot  = document.getElementById('msg-status-dot');\n"
        "  const msgStatTxt  = document.getElementById('msg-status-text');\n"
        "  const msgStation  = document.getElementById('msg-station');\n"
        "  const msgSubject  = document.getElementById('msg-subject');\n"
        "  const msgSerial   = document.getElementById('msg-serial');\n"
        "  let firstChar = true;\n"
        "\n"
        "  /* ---- Audio preview ----------------------------------------- */\n"
        "  const audioBtn = document.getElementById('audio-btn');\n"
        "  let audioCtx = null;\n"
        "  let audioEnabled = false;\n"
        "  let audioSampleRate = 12000;\n"
        "  let audioQueue = [];       /* pending Float32Array chunks */\n"
        "  let audioNextTime = 0;     /* next scheduled playback time (AudioContext.currentTime) */\n"
        "  const AUDIO_AHEAD = 0.1;   /* schedule 100 ms ahead */\n"
        "\n"
        "  function audioFlush() {\n"
        "    if (!audioCtx || audioQueue.length === 0) return;\n"
        "    const now = audioCtx.currentTime;\n"
        "    if (audioNextTime < now) audioNextTime = now + 0.05;\n"
        "    while (audioQueue.length > 0) {\n"
        "      const samples = audioQueue.shift();\n"
        "      const buf = audioCtx.createBuffer(1, samples.length, audioSampleRate);\n"
        "      buf.copyToChannel(samples, 0);\n"
        "      const src = audioCtx.createBufferSource();\n"
        "      src.buffer = buf;\n"
        "      src.connect(audioCtx.destination);\n"
        "      src.start(audioNextTime);\n"
        "      audioNextTime += buf.duration;\n"
        "    }\n"
        "  }\n"
        "\n"
        "  function audioEnable(ws) {\n"
        "    if (audioEnabled) return;\n"
        "    audioCtx = new (window.AudioContext || window.webkitAudioContext)({ sampleRate: audioSampleRate });\n"
        "    audioNextTime = 0;\n"
        "    audioQueue = [];\n"
        "    audioEnabled = true;\n"
        "    audioBtn.className = 'active';\n"
        "    audioBtn.textContent = '\\uD83D\\uDD0A Audio On';\n"
        "    ws.send(JSON.stringify({type:'audio_preview', enable:true}));\n"
        "  }\n"
        "\n"
        "  function audioDisable(ws) {\n"
        "    if (!audioEnabled) return;\n"
        "    audioEnabled = false;\n"
        "    audioQueue = [];\n"
        "    if (audioCtx) { audioCtx.close(); audioCtx = null; }\n"
        "    audioBtn.className = '';\n"
        "    audioBtn.textContent = '\\uD83D\\uDD0A Audio Preview';\n"
        "    ws.send(JSON.stringify({type:'audio_preview', enable:false}));\n"
        "  }\n"
        "\n"
        "  const STATE_NAMES = ['Searching', 'Syncing', 'Locked'];\n"
        "  const STATE_CLASS = ['dim', 'warn', 'good'];\n"
        "\n"
        "  function updateStats(s) {\n"
        "    if (s.bb !== undefined) {\n"
        "      if (isFinite(s.bb)) {\n"
        "        bbEl.textContent = s.bb.toFixed(1);\n"
        "        /* Map dBFS range -120..0 to 0..100% bar */\n"
        "        const pct = Math.max(0, Math.min(100, (s.bb + 120) / 120 * 100));\n"
        "        sigFill.style.width = pct + '%';\n"
        "      } else {\n"
        "        bbEl.textContent = '\\u2014';\n"
        "        sigFill.style.width = '0%';\n"
        "      }\n"
        "    }\n"
        "    if (s.nd !== undefined) ndEl.textContent = isFinite(s.nd) ? s.nd.toFixed(1) : '\\u2014';\n"
        "    /* SNR = baseband_power - noise_density (relative dB figure).\n"
        "     * Bar maps 25..60 dB to 0..100%; below 25 = empty, above 60 = full.\n"
        "     * Thresholds: >45 good, >35 warn, <=35 bad. */\n"
        "    if (s.bb !== undefined && s.nd !== undefined && isFinite(s.bb) && isFinite(s.nd)) {\n"
        "      const snr = s.bb - s.nd;\n"
        "      snrVal.textContent = snr.toFixed(1) + ' dB';\n"
        "      snrVal.className = 'stat-value ' + (snr > 45 ? 'good' : snr > 35 ? 'warn' : 'bad');\n"
        "      /* Map SNR 25..60 dB to 0..100% bar */\n"
        "      const snrPct = Math.max(0, Math.min(100, (snr - 25) / 35 * 100));\n"
        "      snrFill.style.width = snrPct + '%';\n"
        "      snrFill.style.background = snr > 45 ? '#4caf50' : snr > 35 ? '#ffeb3b' : '#e94560';\n"
        "    } else if (s.bb !== undefined) {\n"
        "      snrVal.textContent = '\\u2014'; snrVal.className = 'stat-value';\n"
        "      snrFill.style.width = '0%';\n"
        "    }\n"
        "    if (s.rate !== undefined) rateEl.textContent = (s.rate/1000).toFixed(1) + ' kHz';\n"
        "    if (s.act !== undefined) {\n"
        "      if (s.act) {\n"
        "        actDot.className = 'active';\n"
        "        actTxt.textContent = 'Active';\n"
        "      } else {\n"
        "        actDot.className = '';\n"
        "        actTxt.textContent = 'Idle';\n"
        "      }\n"
        "    }\n"
        "    /* Decoder state (only present in v2 frames) */\n"
        "    if (s.decState !== undefined) {\n"
        "      const name = STATE_NAMES[s.decState] || '?';\n"
        "      const cls  = STATE_CLASS[s.decState] || '';\n"
        "      decState.textContent = name;\n"
        "      decState.className = 'stat-value ' + cls;\n"
        "    }\n"
        "    /* FEC / error metrics — only meaningful when locked (state 2) */\n"
        "    if (s.decState === 2 && s.total > 0) {\n"
        "      const fecPct = (s.fec    / s.total * 100);\n"
        "      const errPct = (s.failed / s.total * 100);\n"
        "      const clnPct = (s.clean  / s.total * 100);\n"
        "      fecVal.textContent = fecPct.toFixed(0) + '%';\n"
        "      fecVal.className = 'stat-value ' + (fecPct > 30 ? 'warn' : 'good');\n"
        "      errVal.textContent = errPct.toFixed(0) + '%';\n"
        "      errVal.className = 'stat-value ' + (errPct > 10 ? 'bad' : errPct > 3 ? 'warn' : 'good');\n"
        "      fecClean.style.width = clnPct.toFixed(1) + '%';\n"
        "      fecFec.style.width   = fecPct.toFixed(1) + '%';\n"
        "      fecFail.style.width  = errPct.toFixed(1) + '%';\n"
        "    } else if (s.decState !== undefined && s.decState !== 2) {\n"
        "      /* Not locked — grey out quality metrics */\n"
        "      fecVal.textContent = '\\u2014'; fecVal.className = 'stat-value dim';\n"
        "      errVal.textContent = '\\u2014'; errVal.className = 'stat-value dim';\n"
        "      fecClean.style.width = '0%';\n"
        "      fecFec.style.width   = '0%';\n"
        "      fecFail.style.width  = '0%';\n"
        "    }\n"
        "  }\n"
        "\n"
        "  function connect() {\n"
        "    const proto = location.protocol === 'https:' ? 'wss' : 'ws';\n"
        "    const ws = new WebSocket(proto + '://' + location.host + '/ws');\n"
        "\n"
        "    ws.onopen = function() {\n"
        "      dot.className = 'connected';\n"
        "      stxt.textContent = 'Connected';\n"
        "      wireAudioBtn(ws);\n"
        "      /* Re-subscribe if audio was active before reconnect */\n"
        "      if (audioEnabled) ws.send(JSON.stringify({type:'audio_preview', enable:true}));\n"
        "    };\n"
        "\n"
        "    ws.onclose = function() {\n"
        "      dot.className = 'disconnected';\n"
        "      stxt.textContent = 'Disconnected — reconnecting…';\n"
        "      setTimeout(connect, 3000);\n"
        "    };\n"
        "\n"
        "    ws.onerror = function() { ws.close(); };\n"
        "\n"
        "    ws.binaryType = 'arraybuffer';\n"
        "\n"
        "    ws.onmessage = function(e) {\n"
        "      if (e.data instanceof ArrayBuffer) {\n"
        "        const v = new DataView(e.data);\n"
        "        const type = v.getUint8(0);\n"
        "        /* v2 stats frame: 20 bytes */\n"
        "        if (type === 0x02 && e.data.byteLength >= 20) {\n"
        "          const bb    = v.getFloat32(1, true);\n"
        "          const nd    = v.getFloat32(5, true);\n"
        "          const flags = v.getUint8(9);\n"
        "          const rate  = v.getUint16(10, true) * 1000;\n"
        "          const state = v.getUint8(12);\n"
        "          const clean = v.getUint8(13);\n"
        "          const fec   = v.getUint8(14);\n"
        "          const fail  = v.getUint8(15);\n"
        "          updateStats({\n"
        "            bb:       (flags & 0x01) ? bb : undefined,\n"
        "            nd:       (flags & 0x01) ? nd : undefined,\n"
        "            rate:     rate,\n"
        "            act:      (flags & 0x02) ? 1 : 0,\n"
        "            decState: state,\n"
        "            clean:    clean,\n"
        "            fec:      fec,\n"
        "            failed:   fail,\n"
        "            total:    clean + fec + fail\n"
        "          });\n"
        "        }\n"
        "        /* v1 frame (12 bytes) — legacy fallback */\n"
        "        else if (type === 0x01 && e.data.byteLength >= 12) {\n"
        "          const bb    = v.getFloat32(1, true);\n"
        "          const nd    = v.getFloat32(5, true);\n"
        "          const flags = v.getUint8(9);\n"
        "          const rate  = v.getUint16(10, true) * 1000;\n"
        "          updateStats({\n"
        "            bb:   (flags & 0x01) ? bb : undefined,\n"
        "            nd:   (flags & 0x01) ? nd : undefined,\n"
        "            rate: rate,\n"
        "            act:  (flags & 0x02) ? 1 : 0\n"
        "          });\n"
        "        }\n"
        "        /* 0x03 message-info frame */\n"
        "        if (type === 0x03 && e.data.byteLength >= 6) {\n"
        "          const flags   = v.getUint8(1);\n"
        "          const inMsg   = !!(flags & 0x01);\n"
        "          const done    = !!(flags & 0x02);\n"
        "          const sta     = v.getUint8(2);\n"
        "          const sub     = v.getUint8(3);\n"
        "          const ser     = v.getUint8(4);\n"
        "          /* Subject code → human label */\n"
        "          const SUBJECT = {\n"
        "            A:'Nav Warning', B:'Met Warning', C:'Ice', D:'SAR',\n"
        "            E:'Met Forecast', F:'Pilot Service', G:'AIS', H:'LORAN',\n"
        "            I:'Omega', J:'Satnav', K:'Other Nav', L:'Nav Warning (LORAN)',\n"
        "            T:'Test', X:'Special', Z:'No msg'\n"
        "          };\n"
        "          if (inMsg || done) {\n"
        "            msgBar.className = '';\n"
        "            if (done) {\n"
        "              msgStatDot.className = 'complete';\n"
        "              msgStatTxt.textContent = 'Complete';\n"
        "            } else {\n"
        "              msgStatDot.className = 'receiving';\n"
        "              msgStatTxt.textContent = 'Receiving\\u2026';\n"
        "            }\n"
        "            msgStation.textContent = sta ? String.fromCharCode(sta) : '\\u2014';\n"
        "            const subCh = sub ? String.fromCharCode(sub) : '';\n"
        "            msgSubject.textContent = subCh\n"
        "              ? (subCh + (SUBJECT[subCh] ? ' \\u2013 ' + SUBJECT[subCh] : ''))\n"
        "              : '\\u2014';\n"
        "            msgSerial.textContent = (ser !== 0xFF) ? String(ser).padStart(2,'0') : '\\u2014';\n"
        "          } else {\n"
        "            msgBar.className = 'idle';\n"
        "            msgStatDot.className = '';\n"
        "            msgStatTxt.textContent = 'Idle';\n"
        "            msgStation.textContent = '\\u2014';\n"
        "            msgSubject.textContent = '\\u2014';\n"
        "            msgSerial.textContent  = '\\u2014';\n"
        "          }\n"
        "          return;\n"
        "        }\n"
        "        /* 0x04 audio frame: int16 LE PCM samples */\n"
        "        if (type === 0x04 && audioEnabled && audioCtx) {\n"
        "          const n = (e.data.byteLength - 1) / 2;\n"
        "          if (n > 0) {\n"
        "            const f32 = new Float32Array(n);\n"
        "            for (let i = 0; i < n; i++)\n"
        "              f32[i] = v.getInt16(1 + i * 2, true) / 32768.0;\n"
        "            audioQueue.push(f32);\n"
        "            audioFlush();\n"
        "          }\n"
        "          return;\n"
        "        }\n"
        "        return;\n"
        "      }\n"
        "      /* Text frame: single decoded character */\n"
        "      if (firstChar) { out.innerHTML = ''; firstChar = false; }\n"
        "      out.appendChild(document.createTextNode(e.data));\n"
        "      out.scrollTop = out.scrollHeight;\n"
        "    };\n"
        "  }\n"
        "\n"
        "  connect();\n"
        "\n"
        "  /* Wire up audio button after connect() so ws is in scope */\n"
        "  function wireAudioBtn(ws) {\n"
        "    audioBtn.onclick = function() {\n"
        "      if (audioEnabled) audioDisable(ws); else audioEnable(ws);\n"
        "    };\n"
        "  }\n"
        "</script>\n"
        "</body>\n"
        "</html>\n";

    return html;
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */
int main(int argc, const char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
                "Usage: %s <http://host:port> [frequency_hz] [--web-port N]\n"
                "\n"
                "  http://host:port   ubersdr server base URL (http or https)\n"
                "  frequency_hz       NAVTEX carrier frequency in Hz (default: 518000)\n"
                "  --web-port N       local web UI port (default: 6040)\n"
                "\n"
                "Examples:\n"
                "  %s http://192.168.1.10:8073\n"
                "  %s http://192.168.1.10:8073 490000\n"
                "  %s https://sdr.example.com:443 518000 --web-port 8080\n",
                argv[0], argv[0], argv[0], argv[0]);
        return EXIT_FAILURE;
    }

    const std::string base_url = strip_slash(argv[1]);

    /* NAVTEX carrier frequency (default 518 kHz international) */
    long carrier_hz = 518000;
    int  web_port   = 6040;

    for (int i = 2; i < argc; i++) {
        if (std::string(argv[i]) == "--web-port") {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --web-port requires a value\n");
                return EXIT_FAILURE;
            }
            char *end = nullptr;
            web_port = (int)strtol(argv[++i], &end, 10);
            if (!end || *end != '\0' || web_port <= 0 || web_port > 65535) {
                fprintf(stderr, "invalid web port: %s\n", argv[i]);
                return EXIT_FAILURE;
            }
        } else {
            char *end = nullptr;
            carrier_hz = strtol(argv[i], &end, 10);
            if (!end || *end != '\0' || carrier_hz <= 0) {
                fprintf(stderr, "invalid frequency: %s\n", argv[i]);
                return EXIT_FAILURE;
            }
        }
    }

    /*
     * Dial frequency: in USB mode, audio_freq = RF_freq - dial_freq.
     * navtex_rx expects the NAVTEX tone at dflt_center_freq = 500 Hz,
     * so:  dial_hz = carrier_hz - 500
     * e.g. 518000 - 500 = 517500 Hz dial → tone at 500 Hz audio.
     */
    long dial_hz = carrier_hz - 500;

    fprintf(stderr, "ubersdr server : %s\n", base_url.c_str());
    fprintf(stderr, "carrier        : %ld Hz\n", carrier_hz);
    fprintf(stderr, "dial frequency : %ld Hz (USB)\n", dial_hz);
    fprintf(stderr, "web UI         : http://localhost:%d/\n", web_port);

    /* ---- 1. Generate session UUID ---------------------------------- */
    std::string session_id = make_uuid4();
    fprintf(stderr, "session id     : %s\n", session_id.c_str());

    /* ---- 2. POST /connection --------------------------------------- */
    curl_global_init(CURL_GLOBAL_DEFAULT);

    {
        std::string conn_url = base_url + "/connection";
        std::string body = "{\"user_session_id\":\"" + session_id + "\"}";
        std::string resp;
        long code = http_post_json(conn_url, body, resp);
        if (code < 0) {
            fprintf(stderr, "error: could not reach %s\n", conn_url.c_str());
            curl_global_cleanup();
            return EXIT_FAILURE;
        }
        fprintf(stderr, "POST /connection → HTTP %ld\n", code);
        if (code == 403) {
            fprintf(stderr, "error: connection rejected (password required?): %s\n",
                    resp.c_str());
            curl_global_cleanup();
            return EXIT_FAILURE;
        }
        /* Simple check: look for "allowed":true in the JSON response */
        if (resp.find("\"allowed\":true") == std::string::npos &&
            resp.find("\"allowed\": true") == std::string::npos) {
            fprintf(stderr, "warning: server may have rejected connection: %s\n",
                    resp.c_str());
            /* Continue anyway, like the reference clients do */
        }
    }

    /* ---- 3. GET /api/description (informational) ------------------- */
    {
        std::string desc_url = base_url + "/api/description";
        std::string resp;
        long code = http_get(desc_url, resp);
        if (code > 0)
            fprintf(stderr, "GET /api/description → HTTP %ld\n", code);
    }

    curl_global_cleanup();

    /* ---- 4. Build WebSocket URL ------------------------------------ */
    std::string ws_base = http_to_ws(base_url);

    /*
     * Bandwidth hint for USB mode.
     *
     * In ubersdr USB mode, bandwidthLow and bandwidthHigh are both
     * *positive* offsets (in Hz) above the dial frequency.  The server
     * rejects negative bandwidthLow values for USB.
     *
     * Use the same wide passband as the ubersdr browser UI (low=50,
     * high=2700) to ensure the server delivers unfiltered audio.
     * The fldigi-derived decoder's own bandpass filters handle
     * frequency selectivity internally.
     */
    char ws_url[1024];
    snprintf(ws_url, sizeof(ws_url),
             "%s/ws?frequency=%ld&mode=usb"
             "&bandwidthLow=50&bandwidthHigh=2700"
             "&format=pcm-zstd&version=2"
             "&user_session_id=%s",
             ws_base.c_str(), dial_hz, session_id.c_str());

    fprintf(stderr, "WebSocket URL  : %s\n", ws_url);

    /* ---- 5. Start broadcast hub + web server ----------------------- */
    BroadcastHub hub;
    g_hub = &hub;

    std::string html_page = make_html_page(base_url, carrier_hz);

    /*
     * ix::HttpServer handles both plain HTTP (serves the HTML page) and
     * WebSocket upgrades (browser clients connecting to /ws).
     * It inherits from WebSocketServer so we use setOnClientMessageCallback
     * for WebSocket connections and setOnConnectionCallback for HTTP.
     */
    ix::HttpServer web_server(web_port, "0.0.0.0");

    /* HTTP handler: serve the HTML page for any GET request */
    web_server.setOnConnectionCallback(
        [&html_page](ix::HttpRequestPtr req,
                     std::shared_ptr<ix::ConnectionState> /*state*/)
        -> ix::HttpResponsePtr
        {
            auto resp = std::make_shared<ix::HttpResponse>();
            resp->statusCode = 200;
            resp->description = "OK";
            resp->headers["Content-Type"] = "text/html; charset=utf-8";
            resp->body = html_page;
            return resp;
        });

    /* WebSocket callback: on new client Open, push current msg-info state.
     * Client tracking is done via getClients() in BroadcastHub::send_char(). */
    web_server.setOnClientMessageCallback(
        [](std::shared_ptr<ix::ConnectionState> /*state*/,
           ix::WebSocket &client_ws,
           const ix::WebSocketMessagePtr &msg) {
            if (msg->type == ix::WebSocketMessageType::Open && g_hub) {
                /* Send current message state to the newly connected browser */
                const MsgParser &p = g_msg_parser;
                uint8_t frame[5];
                frame[0] = 0x03;
                frame[1] = (uint8_t)((p.in_msg   ? 0x01 : 0x00) |
                                     (p.complete  ? 0x02 : 0x00));
                frame[2] = (uint8_t)(p.station ? p.station : 0);
                frame[3] = (uint8_t)(p.subject ? p.subject : 0);
                frame[4] = (uint8_t)(p.serial >= 0 && p.serial <= 99 ? p.serial : 0xFF);
                client_ws.sendBinary(std::string(reinterpret_cast<char *>(frame), 5));
            }
            /* Handle audio-preview subscription control messages */
            if (msg->type == ix::WebSocketMessageType::Message &&
                !msg->binary && g_hub) {
                const std::string &body = msg->str;
                /* Simple JSON parse: look for "audio_preview" and "enable" */
                bool is_audio_preview = body.find("\"audio_preview\"") != std::string::npos
                                     || body.find("\"type\":\"audio_preview\"") != std::string::npos
                                     || body.find("\"type\": \"audio_preview\"") != std::string::npos;
                if (is_audio_preview) {
                    bool enable = body.find("\"enable\":true") != std::string::npos
                               || body.find("\"enable\": true") != std::string::npos;
                    if (enable)
                        g_hub->subscribe_audio(&client_ws);
                    else
                        g_hub->unsubscribe_audio(&client_ws);
                }
            }
            /* On close, remove from audio subscription set */
            if (msg->type == ix::WebSocketMessageType::Close && g_hub) {
                g_hub->unsubscribe_audio(&client_ws);
            }
        });

    /* Point the hub at the server so send_char() can call getClients() */
    hub.server = &web_server;

    auto res = web_server.listenAndStart();
    if (!res) {
        fprintf(stderr, "error: could not start web server on port %d\n", web_port);
        return EXIT_FAILURE;
    }
    fprintf(stderr, "web UI         : http://localhost:%d/  (listening)\n", web_port);

    /* ---- 6. Create navtex_rx decoder ------------------------------ */
    /* Sample rate will be updated from the first full PCM header.
     * Start with 12000 Hz (ubersdr default). */
    int sample_rate = 12000;
    bool only_sitor_b = false;
    bool reverse      = false;

    /* Create a custom FILE* that broadcasts each character to browser clients
     * AND echoes to real stdout */
    FILE *broadcast_file = make_broadcast_file();
    if (!broadcast_file) {
        fprintf(stderr, "error: fopencookie failed\n");
        return EXIT_FAILURE;
    }

    navtex_rx nv(sample_rate, only_sitor_b, reverse, broadcast_file);

    /* ---- 7. Connect WebSocket to ubersdr server (with reconnect loop) -- */
    ix::initNetSystem();

    /* Decoder state — persists across reconnects */
    PcmMeta            meta;
    std::vector<uint8_t>  decomp_buf;
    std::vector<int16_t>  pcm_le;
    int current_sample_rate = sample_rate;

    /* Stats throttle: broadcast at 100ms intervals */
    int64_t last_stats_ms = 0;

    /* Cached signal quality — updated on full header frames, reused on minimal */
    float   cached_bb_power  = 0.0f;
    float   cached_noise_den = 0.0f;
    bool    cached_has_sq    = false;

    /* Outer reconnect loop — runs forever until the process is killed */
    while (true) {

        /* Per-attempt shared state */
        std::atomic<bool> connected{false};
        std::atomic<bool> session_done{false}; /* set when this WS session ends */

        ix::WebSocket ws;
        ws.setUrl(ws_url);
        ws.setHandshakeTimeout(10);
        /* Allow self-signed TLS certificates (same as reference clients) */
        ix::SocketTLSOptions tls;
        tls.caFile = "NONE"; /* disable CA verification */
        ws.setTLSOptions(tls);

        ws.setOnMessageCallback([&](const ix::WebSocketMessagePtr &msg) {
            switch (msg->type) {

            case ix::WebSocketMessageType::Open:
                fprintf(stderr, "WebSocket connected\n");
                connected = true;
                /* Request initial status */
                ws.sendText("{\"type\":\"get_status\"}");
                break;

            case ix::WebSocketMessageType::Close:
                fprintf(stderr, "WebSocket closed: %s\n",
                        msg->closeInfo.reason.c_str());
                connected    = false;
                session_done = true;
                break;

            case ix::WebSocketMessageType::Error:
                fprintf(stderr, "WebSocket error: %s\n",
                        msg->errorInfo.reason.c_str());
                session_done = true;
                break;

            case ix::WebSocketMessageType::Message:
                if (msg->binary) {
                    /* Binary frame: PCM-zstd */
                    if (!decode_pcm_zstd_frame(msg->str.data(), msg->str.size(),
                                               decomp_buf, meta, pcm_le))
                        break;

                    /* Recreate decoder if sample rate changed */
                    if (meta.sample_rate != 0 &&
                        (int)meta.sample_rate != current_sample_rate) {
                        fprintf(stderr, "sample rate: %u Hz\n", meta.sample_rate);
                        current_sample_rate = (int)meta.sample_rate;
                        /* Replace decoder in-place */
                        nv.~navtex_rx();
                        new (&nv) navtex_rx(current_sample_rate,
                                            only_sitor_b, reverse, broadcast_file);
                    }

                    if (!pcm_le.empty()) {
                        nv.process_data(pcm_le.data(), (int)pcm_le.size());
                        hub.send_audio(pcm_le);
                    }

                    /* Update cached signal quality from full header frames */
                    if (meta.has_signal_quality) {
                        cached_bb_power  = meta.baseband_power;
                        cached_noise_den = meta.noise_density;
                        cached_has_sq    = true;
                    }

                    /* Broadcast stats at 100 ms intervals using cached values */
                    {
                        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
                        if (now_ms - last_stats_ms >= 100) {
                            last_stats_ms = now_ms;
                            /* Active = decoded a character in the last 5 seconds */
                            bool active = (now_ms - g_last_char_ms.load()) < 5000;
                            uint32_t rate = meta.sample_rate ? meta.sample_rate
                                                             : (uint32_t)current_sample_rate;
                            hub.send_stats(rate,
                                           cached_has_sq,
                                           cached_bb_power,
                                           cached_noise_den,
                                           active,
                                           nv.get_stats());
                        }
                    }

                } else {
                    /* Text frame: JSON status/pong – log at debug level only */
                    /* Uncomment to see all server JSON:
                    fprintf(stderr, "server: %s\n", msg->str.c_str()); */
                }
                break;

            default:
                break;
            }
        });

        ws.start();

        /* Keepalive thread for this session */
        std::thread keepalive([&]() {
            while (!session_done) {
                for (int i = 0; i < 30 && !session_done; ++i)
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                if (!session_done && connected)
                    ws.sendText("{\"type\":\"ping\"}");
            }
        });

        /* Wait until this session ends */
        while (!session_done)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

        ws.stop();
        keepalive.join();

        /* Wait 10 seconds before reconnecting, then re-register the session
         * with the server (POST /connection) and open a new WebSocket. */
        fprintf(stderr, "reconnecting in 10 seconds...\n");
        for (int i = 10; i > 0; --i) {
            fprintf(stderr, "  retrying in %d s...\r", i);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        fprintf(stderr, "\n");

        /* Re-register session with the server before reconnecting */
        curl_global_init(CURL_GLOBAL_DEFAULT);
        {
            std::string conn_url = base_url + "/connection";
            std::string body = "{\"user_session_id\":\"" + session_id + "\"}";
            std::string resp;
            long code = http_post_json(conn_url, body, resp);
            if (code < 0)
                fprintf(stderr, "reconnect: could not reach %s — will retry\n",
                        conn_url.c_str());
            else
                fprintf(stderr, "POST /connection → HTTP %ld\n", code);
        }
        curl_global_cleanup();

    } /* end reconnect loop — never reached */

    web_server.stop();
    fclose(broadcast_file);
    g_hub = nullptr;

    ix::uninitNetSystem();
    fflush(stdout);
    return 0;
}
