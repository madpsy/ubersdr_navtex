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
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>   /* ntohs */
#include <dirent.h>      /* opendir/readdir */
#include <sys/stat.h>    /* mkdir, stat */

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

    /* Accumulated body text (from after the 4-char header to before NNNN) */
    std::string body;

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
            body.clear();
            return;
        }
        if (is_nnnn && in_msg) {
            complete = true;
            in_msg   = false;
            /* The first three N's were already appended to body before the
             * window completed — strip them. */
            if (body.size() >= 3)
                body.resize(body.size() - 3);
            else
                body.clear();
            return;
        }
        if (in_msg) {
            if (hdr_chars < 4) {
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
            } else {
                body += c;
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
/* Per-message signal-quality accumulator                               */
/* ------------------------------------------------------------------ */
struct MsgMetrics {
    /* Running sums — accumulated every 100 ms while in_msg == true */
    double   sum_bb_power      = 0.0;
    double   sum_noise_density = 0.0;
    double   sum_signal_conf   = 0.0;
    uint64_t sum_chars_clean   = 0;
    uint64_t sum_chars_fec     = 0;
    uint64_t sum_chars_failed  = 0;
    int      sample_count      = 0;   /* number of 100 ms ticks accumulated */
    int      sq_sample_count   = 0;   /* ticks where has_signal_quality was true */

    /* UTC ISO-8601 timestamps */
    std::string start_utc;   /* set when ZCZC seen */
    std::string end_utc;     /* set when NNNN seen */

    void reset() {
        sum_bb_power = sum_noise_density = sum_signal_conf = 0.0;
        sum_chars_clean = sum_chars_fec = sum_chars_failed = 0;
        sample_count = sq_sample_count = 0;
        start_utc.clear();
        end_utc.clear();
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
    std::string log_dir;          /* base directory for message logs, empty = disabled */

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

    /* Per-message signal-quality accumulator */
    MsgMetrics msg_metrics;

    /* Raw log — one file per day per frequency, rotated at UTC midnight */
    FILE        *raw_log_file = nullptr;
    int          raw_log_year = -1;   /* UTC year of currently-open raw log */
    int          raw_log_mon  = -1;   /* UTC month (1-12) */
    int          raw_log_mday = -1;   /* UTC day (1-31) */
};

/* ------------------------------------------------------------------ */
/* Raw log helpers (one append-mode file per day per frequency)        */
/* ------------------------------------------------------------------ */

/* Build the sanitised frequency directory name, e.g. "518kHz" */
static std::string freq_dir_name(const std::string &label)
{
    std::string d = label;
    d.erase(std::remove(d.begin(), d.end(), ' '), d.end());
    return d;
}

/* Open (or re-open) the raw log file for today's UTC date.
 * Path: <log_dir>/<freq>/raw/<YYYY-MM-DD>.log
 * The file is opened in append mode so restarts don't lose data. */
static FILE *open_raw_log(const std::string &log_dir,
                           const std::string &freq_dir,
                           int year, int mon, int mday)
{
    /* Build directory: <log_dir>/<freq>/raw */
    char dir_path[512];
    snprintf(dir_path, sizeof(dir_path), "%s/%s/raw",
             log_dir.c_str(), freq_dir.c_str());

    /* mkdir -p */
    {
        std::string p = dir_path;
        for (size_t i = 1; i < p.size(); i++) {
            if (p[i] == '/') {
                p[i] = '\0';
                mkdir(p.c_str(), 0755);
                p[i] = '/';
            }
        }
        mkdir(p.c_str(), 0755);
    }

    char file_path[640];
    snprintf(file_path, sizeof(file_path), "%s/%04d-%02d-%02d.log",
             dir_path, year, mon, mday);

    FILE *f = fopen(file_path, "a");
    if (!f)
        fprintf(stderr, "[raw_log] cannot open %s: %s\n",
                file_path, strerror(errno));
    else
        fprintf(stderr, "[raw_log] opened %s\n", file_path);
    return f;
}

/* Check whether the UTC date has changed since the raw log was opened.
 * If so (or if it was never opened), close the old file and open a new one. */
static void rotate_raw_log_if_needed(ChannelContext *ctx)
{
    if (ctx->log_dir.empty()) return;

    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    struct tm utc {};
    gmtime_r(&tt, &utc);

    int y = utc.tm_year + 1900;
    int m = utc.tm_mon  + 1;
    int d = utc.tm_mday;

    if (y == ctx->raw_log_year &&
        m == ctx->raw_log_mon  &&
        d == ctx->raw_log_mday)
        return;  /* still the same day */

    /* Close old file if open */
    if (ctx->raw_log_file) {
        fclose(ctx->raw_log_file);
        ctx->raw_log_file = nullptr;
    }

    std::string fdir = freq_dir_name(ctx->label);
    ctx->raw_log_file = open_raw_log(ctx->log_dir, fdir, y, m, d);
    ctx->raw_log_year = y;
    ctx->raw_log_mon  = m;
    ctx->raw_log_mday = d;
}

/* Forward declaration — defined later in the History API section */
static std::string json_escape(const std::string &s);

/* ------------------------------------------------------------------ */
/* Message-to-disk logging                                              */
/* ------------------------------------------------------------------ */
static void save_message(const ChannelContext &ctx, const MsgParser &mp,
                         const MsgMetrics &metrics)
{
    if (ctx.log_dir.empty()) return;
    if (mp.body.empty())     return;

    /* UTC timestamp */
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    struct tm utc {};
    gmtime_r(&tt, &utc);

    /* Sanitise label for use as a directory component (e.g. "518 kHz" -> "518kHz") */
    std::string freq_dir = ctx.label;
    freq_dir.erase(std::remove(freq_dir.begin(), freq_dir.end(), ' '), freq_dir.end());

    /* Build directory path: <log_dir>/<freq>/<YYYY>/<MM>/<DD>/ */
    char date_path[512];
    snprintf(date_path, sizeof(date_path), "%s/%s/%04d/%02d/%02d",
             ctx.log_dir.c_str(),
             freq_dir.c_str(),
             utc.tm_year + 1900,
             utc.tm_mon  + 1,
             utc.tm_mday);

    /* mkdir -p equivalent */
    {
        std::string p = date_path;
        for (size_t i = 1; i < p.size(); i++) {
            if (p[i] == '/') {
                p[i] = '\0';
                mkdir(p.c_str(), 0755);
                p[i] = '/';
            }
        }
        mkdir(p.c_str(), 0755);
    }

    /* Build filename stem: <HHMMSS>Z_<station><subject><serial>
     * Fall back gracefully if station/subject/serial are missing. */
    char stem[64];
    char id_part[16] = "unknown";
    if (mp.station && mp.subject) {
        if (mp.serial >= 0)
            snprintf(id_part, sizeof(id_part), "%c%c%02d",
                     mp.station, mp.subject, mp.serial);
        else
            snprintf(id_part, sizeof(id_part), "%c%c",
                     mp.station, mp.subject);
    }
    snprintf(stem, sizeof(stem), "%02d%02d%02dZ_%s",
             utc.tm_hour, utc.tm_min, utc.tm_sec, id_part);

    /* ---- Write .txt message file ---- */
    std::string txt_path = std::string(date_path) + "/" + stem + ".txt";
    FILE *f = fopen(txt_path.c_str(), "w");
    if (!f) {
        fprintf(stderr, "[ch%d] save_message: cannot open %s: %s\n",
                ctx.channel_id, txt_path.c_str(), strerror(errno));
        return;
    }

    /* Reconstruct the ZCZC header line */
    if (mp.station && mp.subject && mp.serial >= 0)
        fprintf(f, "ZCZC %c%c%02d\n", mp.station, mp.subject, mp.serial);
    else if (mp.station && mp.subject)
        fprintf(f, "ZCZC %c%c\n", mp.station, mp.subject);
    else
        fprintf(f, "ZCZC\n");

    fwrite(mp.body.c_str(), 1, mp.body.size(), f);
    fprintf(f, "NNNN\n");
    fclose(f);

    fprintf(stderr, "[ch%d] saved message -> %s\n", ctx.channel_id, txt_path.c_str());

    /* ---- Write companion .json metrics file ---- */
    std::string json_path = std::string(date_path) + "/" + stem + ".json";
    FILE *jf = fopen(json_path.c_str(), "w");
    if (!jf) {
        fprintf(stderr, "[ch%d] save_message: cannot open %s: %s\n",
                ctx.channel_id, json_path.c_str(), strerror(errno));
        return;
    }

    /* Compute averages */
    int n  = metrics.sample_count;
    int nq = metrics.sq_sample_count;

    /* Duration in seconds */
    int duration_s = 0;
    if (!metrics.start_utc.empty() && !metrics.end_utc.empty()) {
        /* Parse ISO-8601 strings to compute difference (simple approach) */
        struct tm t0 {}, t1 {};
        strptime(metrics.start_utc.c_str(), "%Y-%m-%dT%H:%M:%SZ", &t0);
        strptime(metrics.end_utc.c_str(),   "%Y-%m-%dT%H:%M:%SZ", &t1);
        time_t e0 = timegm(&t0);
        time_t e1 = timegm(&t1);
        if (e1 > e0) duration_s = (int)(e1 - e0);
    }

    fprintf(jf, "{\n");
    fprintf(jf, "  \"freq_hz\": %ld,\n",      ctx.carrier_hz);
    fprintf(jf, "  \"freq_label\": \"%s\",\n", json_escape(ctx.label).c_str());
    fprintf(jf, "  \"station\": \"%c\",\n",    mp.station ? mp.station : '?');
    fprintf(jf, "  \"subject\": \"%c\",\n",    mp.subject ? mp.subject : '?');
    if (mp.serial >= 0)
        fprintf(jf, "  \"serial\": %d,\n",     mp.serial);
    else
        fprintf(jf, "  \"serial\": null,\n");
    fprintf(jf, "  \"start_utc\": \"%s\",\n",  metrics.start_utc.c_str());
    fprintf(jf, "  \"end_utc\": \"%s\",\n",    metrics.end_utc.c_str());
    fprintf(jf, "  \"duration_s\": %d,\n",     duration_s);
    fprintf(jf, "  \"sample_count\": %d,\n",   n);
    if (nq > 0) {
        double avg_bb = metrics.sum_bb_power      / nq;
        double avg_nd = metrics.sum_noise_density / nq;
        fprintf(jf, "  \"avg_bb_power_dbfs\": %.2f,\n",      avg_bb);
        fprintf(jf, "  \"avg_noise_density_dbfs\": %.2f,\n", avg_nd);
        fprintf(jf, "  \"avg_snr_db\": %.2f,\n",             avg_bb - avg_nd);
    } else {
        fprintf(jf, "  \"avg_bb_power_dbfs\": null,\n");
        fprintf(jf, "  \"avg_noise_density_dbfs\": null,\n");
        fprintf(jf, "  \"avg_snr_db\": null,\n");
    }
    if (n > 0) {
        double avg_sc = metrics.sum_signal_conf / n;
        uint64_t total_chars = metrics.sum_chars_clean + metrics.sum_chars_fec + metrics.sum_chars_failed;
        double clean_pct  = total_chars > 0 ? 100.0 * metrics.sum_chars_clean  / total_chars : 0.0;
        double fec_pct    = total_chars > 0 ? 100.0 * metrics.sum_chars_fec    / total_chars : 0.0;
        double failed_pct = total_chars > 0 ? 100.0 * metrics.sum_chars_failed / total_chars : 0.0;
        fprintf(jf, "  \"avg_signal_confidence\": %.2f,\n",  avg_sc);
        fprintf(jf, "  \"avg_chars_clean_pct\": %.1f,\n",    clean_pct);
        fprintf(jf, "  \"avg_chars_fec_pct\": %.1f,\n",      fec_pct);
        fprintf(jf, "  \"avg_chars_failed_pct\": %.1f\n",    failed_pct);
    } else {
        fprintf(jf, "  \"avg_signal_confidence\": null,\n");
        fprintf(jf, "  \"avg_chars_clean_pct\": null,\n");
        fprintf(jf, "  \"avg_chars_fec_pct\": null,\n");
        fprintf(jf, "  \"avg_chars_failed_pct\": null\n");
    }
    fprintf(jf, "}\n");
    fclose(jf);

    fprintf(stderr, "[ch%d] saved metrics  -> %s\n", ctx.channel_id, json_path.c_str());
}

/* ------------------------------------------------------------------ */
/* fopencookie write callback — per-channel                             */
/* ------------------------------------------------------------------ */

/* Return current UTC time as ISO-8601 string: "2026-04-08T14:30:22Z" */
static std::string utc_iso8601_now()
{
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    struct tm utc {};
    gmtime_r(&tt, &utc);
    char buf[64];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
             utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
             utc.tm_hour, utc.tm_min, utc.tm_sec);
    return std::string(buf);
}

static ssize_t ws_cookie_write(void *cookie, const char *buf, size_t size)
{
    auto *ctx = static_cast<ChannelContext *>(cookie);
    if (!ctx || !ctx->hub) return (ssize_t)size;

    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    ctx->last_char_ms.store((int64_t)now_ms);

    /* Rotate raw log at UTC midnight if logging is enabled */
    rotate_raw_log_if_needed(ctx);

    for (size_t i = 0; i < size; i++) {
        char c = buf[i];

        /* Write to raw log */
        if (ctx->raw_log_file) {
            fputc((unsigned char)c, ctx->raw_log_file);
            fflush(ctx->raw_log_file);
        }

        bool was_in  = ctx->msg_parser.in_msg;
        bool was_cmp = ctx->msg_parser.complete;
        char was_sta = ctx->msg_parser.station;
        char was_sub = ctx->msg_parser.subject;
        int  was_ser = ctx->msg_parser.serial;

        ctx->msg_parser.feed(c);

        /* Send the character to the browser FIRST so that the full marker
         * sequence (ZCZC / NNNN) is visible before the divider is inserted. */
        ctx->hub->send_char(ctx->channel_id, c);

        bool changed = (ctx->msg_parser.in_msg   != was_in)  ||
                       (ctx->msg_parser.complete  != was_cmp) ||
                       (ctx->msg_parser.station   != was_sta) ||
                       (ctx->msg_parser.subject   != was_sub) ||
                       (ctx->msg_parser.serial    != was_ser);
        if (changed) {
            /* Record message start time when ZCZC is first seen */
            if (ctx->msg_parser.in_msg && !was_in) {
                ctx->msg_metrics.reset();
                ctx->msg_metrics.start_utc = utc_iso8601_now();
            }

            ctx->hub->send_msg_info(ctx->channel_id,
                                    ctx->msg_parser.in_msg,
                                    ctx->msg_parser.complete,
                                    ctx->msg_parser.station,
                                    ctx->msg_parser.subject,
                                    ctx->msg_parser.serial);
            /* Save completed message to disk (transition: not-complete -> complete) */
            if (ctx->msg_parser.complete && !was_cmp) {
                ctx->msg_metrics.end_utc = utc_iso8601_now();
                save_message(*ctx, ctx->msg_parser, ctx->msg_metrics);
                ctx->msg_metrics.reset();
            }
        }
    }
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
                            DecoderStats dec = ctx->decoder->get_stats();
                            ctx->hub->send_stats(ctx->channel_id,
                                                 rate,
                                                 ctx->cached_has_sq,
                                                 ctx->cached_bb_power,
                                                 ctx->cached_noise_den,
                                                 active,
                                                 dec);

                            /* Accumulate per-message metrics while a message is in progress */
                            if (ctx->msg_parser.in_msg) {
                                ctx->msg_metrics.sample_count++;
                                ctx->msg_metrics.sum_signal_conf   += dec.signal_confidence;
                                ctx->msg_metrics.sum_chars_clean   += (uint64_t)dec.chars_clean;
                                ctx->msg_metrics.sum_chars_fec     += (uint64_t)dec.chars_fec;
                                ctx->msg_metrics.sum_chars_failed  += (uint64_t)dec.chars_failed;
                                if (ctx->cached_has_sq) {
                                    ctx->msg_metrics.sq_sample_count++;
                                    ctx->msg_metrics.sum_bb_power      += ctx->cached_bb_power;
                                    ctx->msg_metrics.sum_noise_density += ctx->cached_noise_den;
                                }
                            }
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
/* History API helpers                                                  */
/* ------------------------------------------------------------------ */

/* Simple JSON string escaping */
static std::string json_escape(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 4);
    for (unsigned char c : s) {
        if      (c == '"')  out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20)  { char buf[8]; snprintf(buf,sizeof(buf),"\\u%04x",c); out+=buf; }
        else                out += (char)c;
    }
    return out;
}

/* Recursively walk log_dir and collect .txt message files and .log raw files.
 * Returns a JSON array string, newest-first (sorted by filename path).
 * Each entry has a "type" field: "msg" for .txt files, "raw" for .log files.
 * .json sidecar files are not listed separately — they are referenced via
 * the "has_metrics" flag on the corresponding "msg" entry. */
static std::string history_list_json(const std::string &log_dir)
{
    if (log_dir.empty()) return "[]";

    /* Collect all .txt and .log file paths (skip .json sidecars — handled below) */
    struct FileEntry {
        std::string full;
        bool is_raw; /* true = .log raw file, false = .txt message file */
    };
    std::vector<FileEntry> files;

    /* Also collect the set of .json sidecar paths for quick lookup */
    std::set<std::string> json_sidecars;

    std::function<void(const std::string &)> walk = [&](const std::string &dir) {
        DIR *d = opendir(dir.c_str());
        if (!d) return;
        struct dirent *ent;
        std::vector<std::string> subdirs;
        while ((ent = readdir(d)) != nullptr) {
            if (ent->d_name[0] == '.') continue;
            std::string full = dir + "/" + ent->d_name;
            struct stat st {};
            if (stat(full.c_str(), &st) != 0) continue;
            if (S_ISDIR(st.st_mode)) {
                subdirs.push_back(full);
            } else if (S_ISREG(st.st_mode)) {
                std::string name = ent->d_name;
                if (name.size() > 4 && name.substr(name.size()-4) == ".txt")
                    files.push_back({full, false});
                else if (name.size() > 4 && name.substr(name.size()-4) == ".log")
                    files.push_back({full, true});
                else if (name.size() > 5 && name.substr(name.size()-5) == ".json")
                    json_sidecars.insert(full);
            }
        }
        closedir(d);
        for (const auto &sub : subdirs) walk(sub);
    };
    walk(log_dir);

    /* Sort newest-first (lexicographic descending on full path works because
     * the path encodes YYYY/MM/DD/HHMMSS or YYYY-MM-DD) */
    std::sort(files.begin(), files.end(),
              [](const FileEntry &a, const FileEntry &b){ return a.full > b.full; });

    /* Build JSON array */
    std::string json = "[";
    bool first = true;
    for (const auto &fe : files) {
        /* Derive relative path from log_dir */
        std::string rel = fe.full;
        if (rel.substr(0, log_dir.size()) == log_dir)
            rel = rel.substr(log_dir.size());
        if (!rel.empty() && rel[0] == '/') rel = rel.substr(1);

        std::string freq, year, month, day, fname;

        if (fe.is_raw) {
            /* rel is like: 518kHz/raw/2025-04-08.log
             * Split: freq / "raw" / filename */
            size_t p = 0, q;
            auto tok = [&]() -> std::string {
                q = rel.find('/', p);
                std::string t = (q == std::string::npos) ? rel.substr(p) : rel.substr(p, q-p);
                p = (q == std::string::npos) ? rel.size() : q+1;
                return t;
            };
            freq        = tok();
            /* skip "raw" component */
            std::string raw_dir = tok();
            fname       = tok();
            /* fname is like "2025-04-08.log" — parse date */
            std::string date_stem = fname;
            if (date_stem.size() > 4) date_stem = date_stem.substr(0, date_stem.size()-4); /* strip .log */
            /* date_stem: "2025-04-08" */
            if (date_stem.size() == 10) {
                year  = date_stem.substr(0, 4);
                month = date_stem.substr(5, 2);
                day   = date_stem.substr(8, 2);
            }
        } else {
            /* rel is like: 518kHz/2025/04/08/143022Z_EA42.txt
             * Split into components */
            size_t p = 0, q;
            auto tok = [&]() -> std::string {
                q = rel.find('/', p);
                std::string t = (q == std::string::npos) ? rel.substr(p) : rel.substr(p, q-p);
                p = (q == std::string::npos) ? rel.size() : q+1;
                return t;
            };
            freq  = tok();
            year  = tok();
            month = tok();
            day   = tok();
            fname = tok();
        }

        /* Parse message filename: HHMMSSZ_<id>.txt */
        std::string time_part, id_part, serial_part;
        if (!fe.is_raw) {
            size_t us = fname.find('_');
            if (us != std::string::npos) {
                time_part = fname.substr(0, us);   /* e.g. "143022Z" */
                id_part   = fname.substr(us+1);    /* e.g. "EA42.txt" */
                /* strip .txt */
                if (id_part.size() > 4) id_part = id_part.substr(0, id_part.size()-4);
                /* Extract serial: last 2 chars of id if they are digits, e.g. "EA42" -> "42" */
                if (id_part.size() >= 3) {
                    std::string tail = id_part.substr(id_part.size() - 2);
                    if (tail[0] >= '0' && tail[0] <= '9' &&
                        tail[1] >= '0' && tail[1] <= '9')
                        serial_part = tail;
                }
            } else {
                time_part = fname;
                id_part   = "";
            }
        }

        /* For .txt message files, check whether a .json sidecar exists and
         * extract the two key summary fields (avg_snr_db, duration_s) for
         * inline display in the history list. */
        bool has_metrics = false;
        std::string json_sidecar_path;
        std::string inline_snr;      /* e.g. "45.8" or "" */
        std::string inline_duration; /* e.g. "132"  or "" */
        if (!fe.is_raw) {
            json_sidecar_path = fe.full.substr(0, fe.full.size() - 4) + ".json";
            has_metrics = (json_sidecars.count(json_sidecar_path) > 0);

            if (has_metrics) {
                /* Read sidecar and extract avg_snr_db and duration_s with
                 * simple substring searches — no full JSON parser needed. */
                FILE *sf = fopen(json_sidecar_path.c_str(), "r");
                if (sf) {
                    std::string sc;
                    char sbuf[4096]; size_t sn;
                    while ((sn = fread(sbuf, 1, sizeof(sbuf), sf)) > 0)
                        sc.append(sbuf, sn);
                    fclose(sf);

                    auto extract_num = [&](const std::string &key) -> std::string {
                        auto p = sc.find("\"" + key + "\":");
                        if (p == std::string::npos) return "";
                        p = sc.find_first_not_of(" \t\r\n", p + key.size() + 3);
                        if (p == std::string::npos) return "";
                        if (sc[p] == 'n') return ""; /* null */
                        auto e = sc.find_first_of(",}\n", p);
                        return (e == std::string::npos) ? sc.substr(p) : sc.substr(p, e - p);
                    };

                    inline_snr      = extract_num("avg_snr_db");
                    inline_duration = extract_num("duration_s");
                }
            }
        }

        if (!first) json += ",";
        first = false;
        json += "{";
        json += "\"type\":\""   + std::string(fe.is_raw ? "raw" : "msg") + "\"";
        json += ",\"path\":\"" + json_escape(fe.full)  + "\"";
        json += ",\"rel\":\""  + json_escape(rel)       + "\"";
        json += ",\"freq\":\""  + json_escape(freq)      + "\"";
        json += ",\"date\":\""  + json_escape(year + "-" + month + "-" + day) + "\"";
        if (!fe.is_raw) {
            json += ",\"time\":\""        + json_escape(time_part)   + "\"";
            json += ",\"id\":\""          + json_escape(id_part)     + "\"";
            json += ",\"serial\":\""      + json_escape(serial_part) + "\"";
            json += ",\"has_metrics\":"   + std::string(has_metrics ? "true" : "false");
            if (has_metrics) {
                json += ",\"metrics_path\":\"" + json_escape(json_sidecar_path) + "\"";
                if (!inline_snr.empty())
                    json += ",\"snr\":" + inline_snr;
                if (!inline_duration.empty())
                    json += ",\"duration_s\":" + inline_duration;
            }
        }
        json += "}";
    }
    json += "]";
    return json;
}

/* Read a file and return its contents, or empty string on error.
 * Validates that the resolved path starts with log_dir to prevent traversal. */
/* ------------------------------------------------------------------ */
/* Log retention / cleanup                                              */
/* ------------------------------------------------------------------ */
/* Delete .txt files (and empty parent directories) older than         */
/* retain_days days.  Runs once immediately, then every hour.          */
static void purge_old_logs(const std::string &log_dir, int retain_days)
{
    if (log_dir.empty() || retain_days <= 0) return;

    auto now = std::chrono::system_clock::now();
    std::time_t cutoff = std::chrono::system_clock::to_time_t(
        now - std::chrono::hours(24 * retain_days));

    /* Collect all .txt (message) and .log (raw) files */
    std::vector<std::string> files;
    std::function<void(const std::string &)> walk = [&](const std::string &dir) {
        DIR *d = opendir(dir.c_str());
        if (!d) return;
        struct dirent *ent;
        std::vector<std::string> subdirs;
        while ((ent = readdir(d)) != nullptr) {
            if (ent->d_name[0] == '.') continue;
            std::string full = dir + "/" + ent->d_name;
            struct stat st {};
            if (stat(full.c_str(), &st) != 0) continue;
            if (S_ISDIR(st.st_mode))
                subdirs.push_back(full);
            else if (S_ISREG(st.st_mode)) {
                std::string name = ent->d_name;
                bool is_txt  = name.size() > 4 && name.substr(name.size()-4) == ".txt";
                bool is_log  = name.size() > 4 && name.substr(name.size()-4) == ".log";
                bool is_json = name.size() > 5 && name.substr(name.size()-5) == ".json";
                if (is_txt || is_log || is_json)
                    files.push_back(full);
            }
        }
        closedir(d);
        for (const auto &sub : subdirs) walk(sub);
    };
    walk(log_dir);

    int removed = 0;
    for (const auto &f : files) {
        struct stat st {};
        if (stat(f.c_str(), &st) != 0) continue;
        if (st.st_mtime < cutoff) {
            if (remove(f.c_str()) == 0) {
                ++removed;
                fprintf(stderr, "[cleanup] removed %s\n", f.c_str());
            }
        }
    }

    /* Remove empty directories under log_dir (bottom-up: walk again) */
    std::function<void(const std::string &)> rmempty = [&](const std::string &dir) {
        if (dir == log_dir) return;
        DIR *d = opendir(dir.c_str());
        if (!d) return;
        bool empty = true;
        struct dirent *ent;
        while ((ent = readdir(d)) != nullptr) {
            if (ent->d_name[0] == '.') continue;
            empty = false;
            break;
        }
        closedir(d);
        if (empty) {
            if (rmdir(dir.c_str()) == 0)
                fprintf(stderr, "[cleanup] removed empty dir %s\n", dir.c_str());
        }
    };
    /* Collect dirs, sort deepest-first, then prune */
    std::vector<std::string> dirs;
    std::function<void(const std::string &)> collect_dirs = [&](const std::string &dir) {
        DIR *d = opendir(dir.c_str());
        if (!d) return;
        struct dirent *ent;
        while ((ent = readdir(d)) != nullptr) {
            if (ent->d_name[0] == '.') continue;
            std::string full = dir + "/" + ent->d_name;
            struct stat st {};
            if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                collect_dirs(full);
                dirs.push_back(full);
            }
        }
        closedir(d);
    };
    collect_dirs(log_dir);
    /* Sort deepest paths first (longest path = deepest) */
    std::sort(dirs.begin(), dirs.end(),
              [](const std::string &a, const std::string &b){ return a.size() > b.size(); });
    for (const auto &dir : dirs) rmempty(dir);

    if (removed > 0)
        fprintf(stderr, "[cleanup] purged %d file(s) older than %d days\n", removed, retain_days);
}

static std::string history_read_file(const std::string &log_dir,
                                     const std::string &path)
{
    if (log_dir.empty() || path.empty()) return "";
    /* Security: path must start with log_dir */
    if (path.substr(0, log_dir.size()) != log_dir) return "";
    /* No ".." components allowed */
    if (path.find("..") != std::string::npos) return "";

    FILE *f = fopen(path.c_str(), "r");
    if (!f) return "";
    std::string content;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        content.append(buf, n);
    fclose(f);
    return content;
}

/* Read the .json sidecar for a .txt message file, if it exists.
 * Returns empty string if not present or on error. */
static std::string history_read_metrics(const std::string &log_dir,
                                        const std::string &txt_path)
{
    if (log_dir.empty() || txt_path.empty()) return "";
    if (txt_path.substr(0, log_dir.size()) != log_dir) return "";
    if (txt_path.find("..") != std::string::npos) return "";
    /* Must end in .txt */
    if (txt_path.size() < 5 || txt_path.substr(txt_path.size()-4) != ".txt") return "";

    std::string json_path = txt_path.substr(0, txt_path.size()-4) + ".json";
    FILE *f = fopen(json_path.c_str(), "r");
    if (!f) return "";
    std::string content;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        content.append(buf, n);
    fclose(f);
    return content;
}

/* URL-decode a query string value */
static std::string url_decode(const std::string &s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i+2 < s.size()) {
            char hex[3] = { s[i+1], s[i+2], 0 };
            out += (char)strtol(hex, nullptr, 16);
            i += 2;
        } else if (s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
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
                "  http://host:port      ubersdr server base URL (http or https)\n"
                "  --freq Hz             NAVTEX carrier frequency in Hz (repeatable, default: 518000 490000)\n"
                "  --web-port N          local web UI port (default: 6040)\n"
                "  --log-dir DIR         directory to save decoded messages\n"
                "                        Also writes a raw character log per frequency:\n"
                "                          <DIR>/<freq>/raw/<YYYY-MM-DD>.log\n"
                "                        A new file is started at UTC midnight each day.\n"
                "  --log-retain-days N   delete log files older than N days (default: 90, 0=disabled)\n"
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
    int         web_port = 6040;
    std::string log_dir;             /* empty = logging disabled */
    int         log_retain_days = 90;

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
        } else if (std::string(argv[i]) == "--log-dir") {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --log-dir requires a value\n");
                return EXIT_FAILURE;
            }
            log_dir = argv[++i];
        } else if (std::string(argv[i]) == "--log-retain-days") {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --log-retain-days requires a value\n");
                return EXIT_FAILURE;
            }
            char *end = nullptr;
            log_retain_days = (int)strtol(argv[++i], &end, 10);
            if (!end || *end != '\0' || log_retain_days < 0) {
                fprintf(stderr, "invalid --log-retain-days value: %s\n", argv[i]);
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

    /* ---- Validate log directory at startup ---- */
    if (!log_dir.empty()) {
        /* Attempt to create the base log directory (mkdir -p) */
        {
            std::string p = log_dir;
            for (size_t i = 1; i < p.size(); i++) {
                if (p[i] == '/') {
                    p[i] = '\0';
                    mkdir(p.c_str(), 0755);
                    p[i] = '/';
                }
            }
            mkdir(p.c_str(), 0755);
        }
        /* Check it is accessible and writable */
        struct stat st {};
        if (stat(log_dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
            fprintf(stderr,
                    "WARNING: log directory '%s' does not exist and could not be created.\n"
                    "         Message logging is disabled. Rebuild the Docker image to fix this.\n",
                    log_dir.c_str());
            log_dir.clear();   /* disable logging to avoid repeated errors */
        } else if (access(log_dir.c_str(), W_OK) != 0) {
            fprintf(stderr,
                    "WARNING: log directory '%s' is not writable by this process.\n"
                    "         Message logging is disabled. Check directory permissions.\n",
                    log_dir.c_str());
            log_dir.clear();
        } else {
            fprintf(stderr, "log directory  : %s\n", log_dir.c_str());
        }
    }

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
        channels[i].log_dir    = log_dir;

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

    /* HTTP handler: serve the HTML page and API endpoints */
    web_server.setOnConnectionCallback(
        [&base_url, &channels, &log_dir](ix::HttpRequestPtr req,
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

            const std::string &uri = req->uri;

            /* Strip base_path prefix for routing */
            std::string path = uri;
            if (!base_path.empty() && path.substr(0, base_path.size()) == base_path)
                path = path.substr(base_path.size());
            /* Strip query string for path matching */
            std::string path_only = path;
            auto qpos = path_only.find('?');
            if (qpos != std::string::npos) path_only = path_only.substr(0, qpos);

            auto resp = std::make_shared<ix::HttpResponse>();
            resp->headers["Access-Control-Allow-Origin"] = "*";

            /* GET /api/history — return JSON list of saved messages */
            if (path_only == "/api/history") {
                resp->statusCode  = 200;
                resp->description = "OK";
                resp->headers["Content-Type"] = "application/json";
                resp->body = history_list_json(log_dir);
                return resp;
            }

            /* GET /api/history/file?path=<absolute-path> — return file content */
            if (path_only == "/api/history/file") {
                std::string file_path;
                /* Extract path= query param */
                if (qpos != std::string::npos) {
                    std::string qs = path.substr(qpos + 1);
                    const std::string key = "path=";
                    auto kpos = qs.find(key);
                    if (kpos != std::string::npos)
                        file_path = url_decode(qs.substr(kpos + key.size()));
                }
                std::string content = history_read_file(log_dir, file_path);
                if (content.empty()) {
                    resp->statusCode  = 404;
                    resp->description = "Not Found";
                    resp->headers["Content-Type"] = "text/plain";
                    resp->body = "not found";
                } else {
                    resp->statusCode  = 200;
                    resp->description = "OK";
                    resp->headers["Content-Type"] = "text/plain; charset=utf-8";
                    resp->body = content;
                }
                return resp;
            }

            /* GET /api/history/metrics?path=<absolute-txt-path> — return JSON sidecar */
            if (path_only == "/api/history/metrics") {
                std::string file_path;
                if (qpos != std::string::npos) {
                    std::string qs = path.substr(qpos + 1);
                    const std::string key = "path=";
                    auto kpos = qs.find(key);
                    if (kpos != std::string::npos)
                        file_path = url_decode(qs.substr(kpos + key.size()));
                }
                std::string content = history_read_metrics(log_dir, file_path);
                if (content.empty()) {
                    resp->statusCode  = 404;
                    resp->description = "Not Found";
                    resp->headers["Content-Type"] = "application/json";
                    resp->body = "{}";
                } else {
                    resp->statusCode  = 200;
                    resp->description = "OK";
                    resp->headers["Content-Type"] = "application/json";
                    resp->body = content;
                }
                return resp;
            }

            /* Default: serve the HTML page */
            resp->statusCode = 200;
            resp->description = "OK";
            resp->headers["Content-Type"] = "text/html; charset=utf-8";
            resp->body = make_html_page(base_url, channels, base_path, !log_dir.empty());
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

    /* ---- Log cleanup thread (runs on startup then every hour) ---- */
    if (!log_dir.empty() && log_retain_days > 0) {
        fprintf(stderr, "log retention  : %d days\n", log_retain_days);
        std::thread([log_dir, log_retain_days]() {
            while (true) {
                purge_old_logs(log_dir, log_retain_days);
                std::this_thread::sleep_for(std::chrono::hours(1));
            }
        }).detach();
    }

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
        if (ctx.raw_log_file)   fclose(ctx.raw_log_file);
    }
    ix::uninitNetSystem();
    fflush(stdout);
    return 0;
}
