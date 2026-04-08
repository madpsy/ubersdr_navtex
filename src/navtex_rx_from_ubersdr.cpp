/* -*- c++ -*- */
/*
 * Copyright 2024 Contributors.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Decode NAVTEX from a ka9q_ubersdr server — dual-frequency edition.
 *
 * Usage:
 *   navtex_rx_from_ubersdr <http://host:port> [--freq Hz] [--freq Hz] [--web-port N]
 *
 * Examples:
 *   navtex_rx_from_ubersdr http://192.168.1.10:8073
 *   navtex_rx_from_ubersdr http://192.168.1.10:8073 --freq 518000 --freq 490000
 *   navtex_rx_from_ubersdr https://sdr.example.com:443 --web-port 8080
 *
 * Always monitors exactly two NAVTEX frequencies simultaneously (default:
 * 518000 Hz international and 490000 Hz national/coastal).  Each frequency
 * runs in its own thread with an independent WebSocket connection to the SDR
 * server and its own navtex_rx decoder instance.
 *
 * The server URL must use http:// or https://.  The WebSocket connection
 * is made to the same host/port using ws:// or wss:// respectively.
 *
 * Audio format requested: pcm-zstd, version 2.
 * The server delivers 12 kHz mono signed 16-bit big-endian PCM inside
 * zstd-compressed WebSocket binary frames.
 *
 * Protocol sequence (per channel):
 *   1. POST http(s)://host:port/connection  {"user_session_id":"<uuid>"}
 *   2. GET  http(s)://host:port/api/description  (informational, channel 0 only)
 *   3. WebSocket ws(s)://host:port/ws?frequency=<dial>&mode=usb
 *              &format=pcm-zstd&version=2&user_session_id=<uuid>
 *   4. Send {"type":"get_status"} on open
 *   5. Keepalive {"type":"ping"} every 30 s
 *   6. On binary frame: zstd-decompress -> strip header -> byteswap -> process_data()
 *
 * Web UI:
 *   An embedded HTTP + WebSocket server runs on --web-port (default 6040).
 *   Browse to http://localhost:6040/ to see real-time decoded characters for
 *   both frequencies in a tabbed interface.
 *
 * Wire frame protocol (browser WebSocket) — see navtex_html.h for full spec.
 */

#include "navtex_rx.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>   /* ntohs */

#include <curl/curl.h>
#include <zstd.h>

#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include <ixwebsocket/IXHttpServer.h>

/* ------------------------------------------------------------------ */
/* Tiny UUID v4 generator                                               */
/* ------------------------------------------------------------------ */
#include <random>
static std::string make_uuid4()
{
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;

    uint64_t hi = dist(gen);
    uint64_t lo = dist(gen);

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
struct CurlBuf { std::string data; };

static size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    auto *buf = static_cast<CurlBuf *>(userdata);
    buf->data.append(ptr, size * nmemb);
    return size * nmemb;
}

static long http_post_json(const std::string &url,
                           const std::string &json_body,
                           std::string &out_body)
{
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    CurlBuf buf;
    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "User-Agent: navtex_rx_from_ubersdr/2.0");

    curl_easy_setopt(curl, CURLOPT_URL,           url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    json_body.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &buf);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       10L);

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

static long http_get(const std::string &url, std::string &out_body)
{
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    CurlBuf buf;
    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "User-Agent: navtex_rx_from_ubersdr/2.0");

    curl_easy_setopt(curl, CURLOPT_URL,           url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &buf);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       10L);

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
static const uint16_t MAGIC_FULL    = 0x5043;
static const uint16_t MAGIC_MINIMAL = 0x504D;
static const size_t   FULL_HDR_V1  = 29;
static const size_t   FULL_HDR_V2  = 37;
static const size_t   MIN_HDR      = 13;

struct PcmMeta {
    uint32_t sample_rate        = 0;
    uint8_t  channels           = 0;
    float    baseband_power     = 0.0f;
    float    noise_density      = 0.0f;
    bool     has_signal_quality = false;
};

static inline uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) |
           ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static inline uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1]<<8);
}

static bool decode_pcm_zstd_frame(const void *compressed, size_t compressed_len,
                                   std::vector<uint8_t> &decomp_buf,
                                   PcmMeta &meta,
                                   std::vector<int16_t> &pcm_le)
{
    size_t frame_size = ZSTD_getFrameContentSize(compressed, compressed_len);
    if (frame_size == ZSTD_CONTENTSIZE_ERROR ||
        frame_size == ZSTD_CONTENTSIZE_UNKNOWN)
        frame_size = compressed_len * 8;

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

    uint16_t magic    = read_le16(p);
    size_t   hdr_size = 0;

    if (magic == MAGIC_FULL) {
        if (actual < 3) return false;
        uint8_t version = p[2];
        hdr_size = (version >= 2) ? FULL_HDR_V2 : FULL_HDR_V1;
        if (actual < hdr_size) return false;

        meta.sample_rate = read_le32(p + 20);
        meta.channels    = p[24];

        if (version >= 2) {
            uint32_t bb_bits, nd_bits;
            bb_bits = (uint32_t)p[25]|((uint32_t)p[26]<<8)|((uint32_t)p[27]<<16)|((uint32_t)p[28]<<24);
            nd_bits = (uint32_t)p[29]|((uint32_t)p[30]<<8)|((uint32_t)p[31]<<16)|((uint32_t)p[32]<<24);
            memcpy(&meta.baseband_power, &bb_bits, 4);
            memcpy(&meta.noise_density,  &nd_bits, 4);
            meta.has_signal_quality = true;
        } else {
            meta.has_signal_quality = false;
        }
    } else if (magic == MAGIC_MINIMAL) {
        hdr_size = MIN_HDR;
        if (actual < hdr_size) return false;
    } else {
        fprintf(stderr, "unknown PCM magic: 0x%04x\n", magic);
        return false;
    }

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
    if (http_url.substr(0, 8) == "https://") return "wss://" + http_url.substr(8);
    if (http_url.substr(0, 7) == "http://")  return "ws://"  + http_url.substr(7);
    return http_url;
}
static std::string strip_slash(const std::string &s)
{
    if (!s.empty() && s.back() == '/') return s.substr(0, s.size() - 1);
    return s;
}
static std::string strip_trailing_slash(const std::string &s)
{
    std::string r = s;
    while (!r.empty() && r.back() == '/') r.pop_back();
    return r;
}

/* ------------------------------------------------------------------ */
/* NAVTEX message framing parser (ZCZC / NNNN)                         */
/* ------------------------------------------------------------------ */
struct MsgParser {
    char window[4] = {};
    int  win_pos   = 0;

    bool in_msg   = false;
    bool complete = false;
    char station  = 0;
    char subject  = 0;
    int  serial   = -1;

    int  hdr_chars  = 0;
    char hdr_buf[4] = {};

    void feed(char c)
    {
        window[win_pos & 3] = c;
        win_pos++;

        auto w = [&](int i) { return window[(win_pos + i) & 3]; };
        bool is_zczc = (w(0)=='Z' && w(1)=='C' && w(2)=='Z' && w(3)=='C');
        bool is_nnnn = (w(0)=='N' && w(1)=='N' && w(2)=='N' && w(3)=='N');

        if (is_zczc) {
            in_msg    = true;
            complete  = false;
            station   = 0;
            subject   = 0;
            serial    = -1;
            hdr_chars = 0;
            return;
        }
        if (is_nnnn && in_msg) {
            complete = true;
            in_msg   = false;
            return;
        }
        if (in_msg && hdr_chars < 4) {
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
};

/* ------------------------------------------------------------------ */
/* Browser WebSocket broadcast hub                                      */
/* ------------------------------------------------------------------ */
struct BroadcastHub {
    ix::HttpServer *server = nullptr;

    /* audio subscriptions: ws* -> channel_id */
    std::mutex                     audio_subs_mu;
    std::map<ix::WebSocket *, int> audio_subs;

    void subscribe_audio(ix::WebSocket *ws, int ch) {
        std::lock_guard<std::mutex> lk(audio_subs_mu);
        audio_subs[ws] = ch;
    }
    void unsubscribe_audio(ix::WebSocket *ws) {
        std::lock_guard<std::mutex> lk(audio_subs_mu);
        audio_subs.erase(ws);
    }

    /* Type 0x04: audio — only to clients subscribed to this channel.
     * Frame: [0x04][ch][int16 LE samples...] */
    void send_audio(int ch, const std::vector<int16_t> &pcm)
    {
        if (!server || pcm.empty()) return;
        std::lock_guard<std::mutex> lk(audio_subs_mu);
        if (audio_subs.empty()) return;

        std::string frame(2 + pcm.size() * 2, '\0');
        frame[0] = 0x04;
        frame[1] = (char)(uint8_t)ch;
        memcpy(&frame[2], pcm.data(), pcm.size() * 2);

        auto clients = server->getClients();
        for (auto &ws : clients) {
            auto it = audio_subs.find(ws.get());
            if (it != audio_subs.end() && it->second == ch)
                ws->sendBinary(frame);
        }
    }

    /* Text frame: channel digit + decoded character, e.g. "0A" */
    void send_char(int ch, char c)
    {
        if (!server) return;
        char buf[2] = { (char)('0' + ch), c };
        std::string s(buf, 2);
        for (auto &ws : server->getClients())
            ws->sendText(s);
    }

    /* Type 0x02: stats frame (22 bytes).
     * Layout: [0x02][ch][bb:f32][nd:f32][flags:u8][rate_khz:u16][state:u8]
     *         [clean:u8][fec:u8][failed:u8][sig_conf:f32][rsvd:u8] */
    void send_stats(int ch,
                    uint32_t sample_rate,
                    bool has_signal_quality,
                    float baseband_power,
                    float noise_density,
                    bool active,
                    const DecoderStats &dec)
    {
        if (!server) return;

        uint8_t frame[22] = {};
        frame[0] = 0x02;
        frame[1] = (uint8_t)ch;
        memcpy(&frame[2], &baseband_power, 4);
        memcpy(&frame[6], &noise_density,  4);
        frame[10] = (has_signal_quality ? 0x01 : 0x00) | (active ? 0x02 : 0x00);
        uint16_t rate_khz = (uint16_t)(sample_rate / 1000);
        frame[11] = (uint8_t)(rate_khz & 0xFF);
        frame[12] = (uint8_t)(rate_khz >> 8);
        frame[13] = (uint8_t)dec.state;
        frame[14] = (uint8_t)(dec.chars_clean  > 255 ? 255 : dec.chars_clean);
        frame[15] = (uint8_t)(dec.chars_fec    > 255 ? 255 : dec.chars_fec);
        frame[16] = (uint8_t)(dec.chars_failed > 255 ? 255 : dec.chars_failed);
        float sig_conf = (float)dec.signal_confidence;
        memcpy(&frame[17], &sig_conf, 4);
        frame[21] = 0;

        std::string msg(reinterpret_cast<char *>(frame), sizeof(frame));
        for (auto &ws : server->getClients())
            ws->sendBinary(msg);
    }

    /* Type 0x03: message-info frame (6 bytes).
     * Layout: [0x03][ch][flags:u8][station:u8][subject:u8][serial:u8] */
    void send_msg_info(int ch,
                       bool in_message, bool complete,
                       char station, char subject, int serial)
    {
        if (!server) return;

        uint8_t frame[6];
        frame[0] = 0x03;
        frame[1] = (uint8_t)ch;
        frame[2] = (uint8_t)((in_message ? 0x01 : 0x00) | (complete ? 0x02 : 0x00));
        frame[3] = (uint8_t)(station ? station : 0);
        frame[4] = (uint8_t)(subject ? subject : 0);
        frame[5] = (uint8_t)(serial >= 0 && serial <= 99 ? serial : 0xFF);

        std::string msg(reinterpret_cast<char *>(frame), sizeof(frame));
        for (auto &ws : server->getClients())
            ws->sendBinary(msg);
    }
};

/* ------------------------------------------------------------------ */
/* Per-channel context                                                  */
/* ------------------------------------------------------------------ */
struct ChannelContext {
    int         channel_id;
    long        carrier_hz;
    long        dial_hz;          /* carrier_hz - 500 */
    std::string label;            /* e.g. "518 kHz" */
    std::string name;             /* e.g. "International" */

    BroadcastHub *hub = nullptr;  /* shared, not owned */

    navtex_rx   *decoder             = nullptr;
    FILE        *broadcast_file      = nullptr;
    int          current_sample_rate = 12000;

    MsgParser            msg_parser;
    std::atomic<int64_t> last_char_ms{0};

    float cached_bb_power  = 0.0f;
    float cached_noise_den = 0.0f;
    bool  cached_has_sq    = false;

    PcmMeta              meta;
    std::vector<uint8_t> decomp_buf;
    std::vector<int16_t> pcm_le;
};

/* ------------------------------------------------------------------ */
/* fopencookie write callback — per-channel                             */
/* ------------------------------------------------------------------ */
static ssize_t ws_cookie_write(void *cookie, const char *buf, size_t size)
{
    auto *ctx = static_cast<ChannelContext *>(cookie);
    if (!ctx || !ctx->hub) return (ssize_t)size;

    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    ctx->last_char_ms.store((int64_t)now_ms);

    for (size_t i = 0; i < size; i++) {
        char c = buf[i];

        bool was_in  = ctx->msg_parser.in_msg;
        bool was_cmp = ctx->msg_parser.complete;
        char was_sta = ctx->msg_parser.station;
        char was_sub = ctx->msg_parser.subject;
        int  was_ser = ctx->msg_parser.serial;

        ctx->msg_parser.feed(c);

        bool changed = (ctx->msg_parser.in_msg   != was_in)  ||
                       (ctx->msg_parser.complete  != was_cmp) ||
                       (ctx->msg_parser.station   != was_sta) ||
                       (ctx->msg_parser.subject   != was_sub) ||
                       (ctx->msg_parser.serial    != was_ser);
        if (changed) {
            ctx->hub->send_msg_info(ctx->channel_id,
                                    ctx->msg_parser.in_msg,
                                    ctx->msg_parser.complete,
                                    ctx->msg_parser.station,
                                    ctx->msg_parser.subject,
                                    ctx->msg_parser.serial);
        }

        putchar(c);
        ctx->hub->send_char(ctx->channel_id, c);
    }
    fflush(stdout);
    return (ssize_t)size;
}

static FILE *make_broadcast_file(ChannelContext *ctx)
{
    static cookie_io_functions_t fns = {};
    fns.write = ws_cookie_write;
    FILE *f = fopencookie(ctx, "w", fns);
    if (f) setvbuf(f, nullptr, _IONBF, 0);
    return f;
}

/* ------------------------------------------------------------------ */
/* HTML page — defined in navtex_html.h                                 */
/* ------------------------------------------------------------------ */
#include "navtex_html.h"

/* ------------------------------------------------------------------ */
/* Per-channel reconnect loop (runs in its own thread)                  */
/* ------------------------------------------------------------------ */
static void run_channel(ChannelContext *ctx, const std::string &base_url)
{
    const std::string ws_base = http_to_ws(base_url);

    /* Build WebSocket URL */
    char ws_url[1024];
    snprintf(ws_url, sizeof(ws_url),
             "%s/ws?frequency=%ld&mode=usb"
             "&bandwidthLow=50&bandwidthHigh=2700"
             "&format=pcm-zstd&version=2"
             "&user_session_id=%s",
             ws_base.c_str(), ctx->dial_hz,
             make_uuid4().c_str()); /* fresh UUID per reconnect attempt */

    fprintf(stderr, "[ch%d] %s  dial=%ld Hz  ws=%s\n",
            ctx->channel_id, ctx->label.c_str(), ctx->dial_hz, ws_url);

    /* Stats throttle */
    int64_t last_stats_ms = 0;

    /* Outer reconnect loop */
    while (true) {

        /* Register session with the server */
        std::string session_id = make_uuid4();
        curl_global_init(CURL_GLOBAL_DEFAULT);
        {
            std::string conn_url = base_url + "/connection";
            std::string body     = "{\"user_session_id\":\"" + session_id + "\"}";
            std::string resp;
            long code = http_post_json(conn_url, body, resp);
            if (code < 0) {
                fprintf(stderr, "[ch%d] could not reach %s — retrying in 10s\n",
                        ctx->channel_id, conn_url.c_str());
                curl_global_cleanup();
                std::this_thread::sleep_for(std::chrono::seconds(10));
                continue;
            }
            fprintf(stderr, "[ch%d] POST /connection -> HTTP %ld\n",
                    ctx->channel_id, code);
            if (code == 403) {
                fprintf(stderr, "[ch%d] connection rejected (password required?): %s\n",
                        ctx->channel_id, resp.c_str());
                curl_global_cleanup();
                std::this_thread::sleep_for(std::chrono::seconds(30));
                continue;
            }
        }
        curl_global_cleanup();

        /* Rebuild WS URL with fresh session id */
        snprintf(ws_url, sizeof(ws_url),
                 "%s/ws?frequency=%ld&mode=usb"
                 "&bandwidthLow=50&bandwidthHigh=2700"
                 "&format=pcm-zstd&version=2"
                 "&user_session_id=%s",
                 ws_base.c_str(), ctx->dial_hz, session_id.c_str());

        std::atomic<bool> connected{false};
        std::atomic<bool> session_done{false};

        ix::WebSocket ws;
        ws.setUrl(ws_url);
        ws.setHandshakeTimeout(10);
        ix::SocketTLSOptions tls;
        tls.caFile = "NONE";
        ws.setTLSOptions(tls);

        ws.setOnMessageCallback([&](const ix::WebSocketMessagePtr &msg) {
            switch (msg->type) {

            case ix::WebSocketMessageType::Open:
                fprintf(stderr, "[ch%d] WebSocket connected\n", ctx->channel_id);
                connected = true;
                ws.sendText("{\"type\":\"get_status\"}");
                break;

            case ix::WebSocketMessageType::Close:
                fprintf(stderr, "[ch%d] WebSocket closed: %s\n",
                        ctx->channel_id, msg->closeInfo.reason.c_str());
                connected    = false;
                session_done = true;
                break;

            case ix::WebSocketMessageType::Error:
                fprintf(stderr, "[ch%d] WebSocket error: %s\n",
                        ctx->channel_id, msg->errorInfo.reason.c_str());
                session_done = true;
                break;

            case ix::WebSocketMessageType::Message:
                if (msg->binary) {
                    if (!decode_pcm_zstd_frame(msg->str.data(), msg->str.size(),
                                               ctx->decomp_buf, ctx->meta, ctx->pcm_le))
                        break;

                    /* Recreate decoder if sample rate changed */
                    if (ctx->meta.sample_rate != 0 &&
                        (int)ctx->meta.sample_rate != ctx->current_sample_rate) {
                        fprintf(stderr, "[ch%d] sample rate: %u Hz\n",
                                ctx->channel_id, ctx->meta.sample_rate);
                        ctx->current_sample_rate = (int)ctx->meta.sample_rate;
                        ctx->decoder->~navtex_rx();
                        new (ctx->decoder) navtex_rx(ctx->current_sample_rate,
                                                     false, false,
                                                     ctx->broadcast_file);
                    }

                    if (!ctx->pcm_le.empty()) {
                        ctx->decoder->process_data(ctx->pcm_le.data(),
                                                   (int)ctx->pcm_le.size());
                        ctx->hub->send_audio(ctx->channel_id, ctx->pcm_le);
                    }

                    if (ctx->meta.has_signal_quality) {
                        ctx->cached_bb_power  = ctx->meta.baseband_power;
                        ctx->cached_noise_den = ctx->meta.noise_density;
                        ctx->cached_has_sq    = true;
                    }

                    /* Broadcast stats at 100 ms intervals */
                    {
                        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
                        if (now_ms - last_stats_ms >= 100) {
                            last_stats_ms = now_ms;
                            bool active = (now_ms - ctx->last_char_ms.load()) < 5000;
                            uint32_t rate = ctx->meta.sample_rate
                                          ? ctx->meta.sample_rate
                                          : (uint32_t)ctx->current_sample_rate;
                            ctx->hub->send_stats(ctx->channel_id,
                                                 rate,
                                                 ctx->cached_has_sq,
                                                 ctx->cached_bb_power,
                                                 ctx->cached_noise_den,
                                                 active,
                                                 ctx->decoder->get_stats());
                        }
                    }
                } else {
                    /* Text frame: JSON status/pong — ignore */
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

        fprintf(stderr, "[ch%d] reconnecting in 10 seconds...\n", ctx->channel_id);
        for (int i = 10; i > 0; --i) {
            fprintf(stderr, "[ch%d]   retrying in %d s...\r", ctx->channel_id, i);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        fprintf(stderr, "\n");

    } /* end reconnect loop */
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */
int main(int argc, const char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
                "Usage: %s <http://host:port> [--freq Hz] [--freq Hz] [--web-port N]\n"
                "\n"
                "  http://host:port   ubersdr server base URL (http or https)\n"
                "  --freq Hz          NAVTEX carrier frequency in Hz (repeatable, default: 518000 490000)\n"
                "  --web-port N       local web UI port (default: 6040)\n"
                "\n"
                "Examples:\n"
                "  %s http://192.168.1.10:8073\n"
                "  %s http://192.168.1.10:8073 --freq 518000 --freq 490000\n"
                "  %s https://sdr.example.com:443 --web-port 8080\n",
                argv[0], argv[0], argv[0], argv[0]);
        return EXIT_FAILURE;
    }

    const std::string base_url = strip_slash(argv[1]);

    std::vector<long> freqs;
    int web_port = 6040;

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
        } else if (std::string(argv[i]) == "--freq") {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --freq requires a value\n");
                return EXIT_FAILURE;
            }
            char *end = nullptr;
            long f = strtol(argv[++i], &end, 10);
            if (!end || *end != '\0' || f <= 0) {
                fprintf(stderr, "invalid frequency: %s\n", argv[i]);
                return EXIT_FAILURE;
            }
            freqs.push_back(f);
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return EXIT_FAILURE;
        }
    }

    /* Default: both standard NAVTEX frequencies */
    if (freqs.empty()) {
        freqs.push_back(518000);
        freqs.push_back(490000);
    }
    if (freqs.size() < 2) {
        /* Pad to two channels if only one was given */
        freqs.push_back(freqs[0] == 518000 ? 490000 : 518000);
    }

    fprintf(stderr, "ubersdr server : %s\n", base_url.c_str());
    fprintf(stderr, "web UI         : http://localhost:%d/\n", web_port);

    /* ---- Build channel contexts ---- */
    /* Label/name helpers */
    auto freq_label = [](long hz) -> std::string {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.0f kHz", hz / 1000.0);
        return buf;
    };
    auto freq_name = [](long hz) -> std::string {
        if (hz == 518000) return "International";
        if (hz == 490000) return "National";
        char buf[32];
        snprintf(buf, sizeof(buf), "%ld Hz", hz);
        return buf;
    };

    /* BroadcastHub — shared across all channels */
    BroadcastHub hub;

    /* Build ChannelContext vector (non-copyable atomics: use index-based init) */
    std::vector<ChannelContext> channels(freqs.size());
    for (size_t i = 0; i < freqs.size(); i++) {
        channels[i].channel_id = (int)i;
        channels[i].carrier_hz = freqs[i];
        channels[i].dial_hz    = freqs[i] - 500;
        channels[i].label      = freq_label(freqs[i]);
        channels[i].name       = freq_name(freqs[i]);
        channels[i].hub        = &hub;

        fprintf(stderr, "channel %zu     : %s (%s)  dial=%ld Hz\n",
                i, channels[i].label.c_str(), channels[i].name.c_str(),
                channels[i].dial_hz);

        /* Create broadcast FILE* (cookie = pointer to this ChannelContext) */
        channels[i].broadcast_file = make_broadcast_file(&channels[i]);
        if (!channels[i].broadcast_file) {
            fprintf(stderr, "error: fopencookie failed for channel %zu\n", i);
            return EXIT_FAILURE;
        }

        /* Create navtex_rx decoder */
        channels[i].decoder = new navtex_rx(channels[i].current_sample_rate,
                                            false, false,
                                            channels[i].broadcast_file);
    }

    /* ---- GET /api/description (informational, once) ---- */
    curl_global_init(CURL_GLOBAL_DEFAULT);
    {
        std::string desc_url = base_url + "/api/description";
        std::string resp;
        long code = http_get(desc_url, resp);
        if (code > 0)
            fprintf(stderr, "GET /api/description -> HTTP %ld\n", code);
    }
    curl_global_cleanup();

    /* ---- Start web server ---- */
    ix::HttpServer web_server(web_port, "0.0.0.0");

    /* HTTP handler: serve the HTML page */
    web_server.setOnConnectionCallback(
        [&base_url, &channels](ix::HttpRequestPtr req,
                               std::shared_ptr<ix::ConnectionState> /*state*/)
        -> ix::HttpResponsePtr
        {
            std::string base_path;
            for (const auto &h : req->headers) {
                std::string key = h.first;
                for (auto &c : key) c = (char)tolower((unsigned char)c);
                if (key == "x-forwarded-prefix") {
                    base_path = strip_trailing_slash(h.second);
                    break;
                }
            }
            auto resp = std::make_shared<ix::HttpResponse>();
            resp->statusCode = 200;
            resp->description = "OK";
            resp->headers["Content-Type"] = "text/html; charset=utf-8";
            resp->body = make_html_page(base_url, channels, base_path);
            return resp;
        });

    /* WebSocket callback: handle new connections and audio subscription messages */
    web_server.setOnClientMessageCallback(
        [&channels, &hub](std::shared_ptr<ix::ConnectionState> /*state*/,
                          ix::WebSocket &client_ws,
                          const ix::WebSocketMessagePtr &msg)
        {
            if (msg->type == ix::WebSocketMessageType::Open) {
                /* Push current msg-info state for all channels to the new client */
                for (const auto &ctx : channels) {
                    uint8_t frame[6];
                    frame[0] = 0x03;
                    frame[1] = (uint8_t)ctx.channel_id;
                    frame[2] = (uint8_t)((ctx.msg_parser.in_msg   ? 0x01 : 0x00) |
                                         (ctx.msg_parser.complete  ? 0x02 : 0x00));
                    frame[3] = (uint8_t)(ctx.msg_parser.station ? ctx.msg_parser.station : 0);
                    frame[4] = (uint8_t)(ctx.msg_parser.subject ? ctx.msg_parser.subject : 0);
                    frame[5] = (uint8_t)(ctx.msg_parser.serial >= 0 && ctx.msg_parser.serial <= 99
                                         ? ctx.msg_parser.serial : 0xFF);
                    client_ws.sendBinary(std::string(reinterpret_cast<char *>(frame), 6));
                }
            }

            /* Handle audio-preview subscription control messages */
            if (msg->type == ix::WebSocketMessageType::Message && !msg->binary) {
                const std::string &body = msg->str;
                bool is_audio = body.find("\"audio_preview\"") != std::string::npos
                             || body.find("\"type\":\"audio_preview\"") != std::string::npos
                             || body.find("\"type\": \"audio_preview\"") != std::string::npos;
                if (is_audio) {
                    bool enable = body.find("\"enable\":true")  != std::string::npos
                               || body.find("\"enable\": true") != std::string::npos;

                    /* Extract channel number from JSON: "channel":N */
                    int ch = 0;
                    auto pos = body.find("\"channel\":");
                    if (pos == std::string::npos) pos = body.find("\"channel\": ");
                    if (pos != std::string::npos) {
                        const char *p = body.c_str() + pos;
                        while (*p && (*p < '0' || *p > '9')) p++;
                        if (*p) ch = (int)strtol(p, nullptr, 10);
                    }

                    if (enable)
                        hub.subscribe_audio(&client_ws, ch);
                    else
                        hub.unsubscribe_audio(&client_ws);
                }
            }

            if (msg->type == ix::WebSocketMessageType::Close)
                hub.unsubscribe_audio(&client_ws);
        });

    hub.server = &web_server;

    auto res = web_server.listenAndStart();
    if (!res) {
        fprintf(stderr, "error: could not start web server on port %d\n", web_port);
        return EXIT_FAILURE;
    }
    fprintf(stderr, "web UI         : http://localhost:%d/  (listening)\n", web_port);

    /* ---- Start IXWebSocket network system ---- */
    ix::initNetSystem();

    /* ---- Spawn one thread per channel ---- */
    std::vector<std::thread> channel_threads;
    channel_threads.reserve(channels.size());
    for (auto &ctx : channels)
        channel_threads.emplace_back(run_channel, &ctx, base_url);

    /* Main thread waits for all channel threads (they run forever) */
    for (auto &t : channel_threads)
        t.join();

    /* Cleanup (unreachable in normal operation) */
    web_server.stop();
    for (auto &ctx : channels) {
        delete ctx.decoder;
        if (ctx.broadcast_file) fclose(ctx.broadcast_file);
    }
    ix::uninitNetSystem();
    fflush(stdout);
    return 0;
}
