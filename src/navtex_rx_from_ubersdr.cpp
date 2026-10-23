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
 * Audio format requested: pcm-zstd, version 4.
 * The server delivers 12 kHz mono signed 16-bit PCM, losslessly coded by the
 * version 4 predictive codec (see src/pcm_v4.hpp).  The query parameter is
 * still called "pcm-zstd" — only the version changed; version 4 carries no
 * zstd at all.
 *
 * Protocol sequence (per channel):
 *   1. POST http(s)://host:port/connection  {"user_session_id":"<uuid>"}
 *   2. GET  http(s)://host:port/api/description  (informational, channel 0 only)
 *   3. WebSocket ws(s)://host:port/ws?frequency=<dial>&mode=usb
 *              &format=pcm-zstd&version=4&user_session_id=<uuid>
 *   4. Send {"type":"get_status"} on open
 *   5. Keepalive {"type":"ping"} every 30 s
 *   6. On binary frame: PCMv4StreamDecoder::decode -> process_data()
 *
 * Web UI:
 *   An embedded HTTP + WebSocket server runs on --web-port (default 6040).
 *   Browse to http://localhost:6040/ to see real-time decoded characters for
 *   both frequencies in a tabbed interface.
 *
 * Wire frame protocol (browser WebSocket) — see navtex_html.h for full spec.
 */

#include "navtex_rx.h"
#include "navtex_snr.h"
#include "navtex_stations.h"
#include "navtex_countries.h"
#include "navtex_ident.h"
#include "navtex_ident_facts.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <dirent.h>      /* opendir/readdir */
#include <sys/stat.h>    /* mkdir, stat, utimensat */
#include <fcntl.h>       /* AT_FDCWD */

#include <curl/curl.h>
#include <zlib.h>

#include "pcm_v4.hpp"

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
    headers = curl_slist_append(headers, "User-Agent: ubersdr_navtex/2.0");

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
    headers = curl_slist_append(headers, "User-Agent: ubersdr_navtex/2.0");

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
/* Audio protocol version 4 frame decoder                              */
/* ------------------------------------------------------------------ */

/*
 * Version 4 replaced the zstd wrapper of versions 1-3 with a predictive
 * lossless codec and a variable-length header, so there is no compression
 * library here at all: pcm_v4.hpp is header-only and self-contained.
 *
 * The header carries the sample rate and the channel count itself, on every
 * resynchronisation packet — and a resync is the first packet any decoder will
 * accept — so the stream is self-describing from the first frame that gets
 * through.
 */

struct PcmMeta {
    uint32_t sample_rate        = 0;
    uint8_t  channels           = 0;
    float    baseband_power     = 0.0f;  /* dBFS */
    /* Version 4 reports the noise POWER in the passband in dBFS.  Versions 1-3
     * sent the dBFS/Hz DENSITY, which is 10*log10(2650) = 34.2 dB lower -- see
     * navtex_snr.h.  The stored sidecar key is still called
     * avg_noise_density_dbfs, for compatibility with records already on disk. */
    float    noise_dbfs         = 0.0f;  /* dBFS, passband noise power */
    bool     has_signal_quality = false;

    /* Consecutive decode failures, so a desynchronised stream reports itself
     * once rather than once per packet on every channel.  Cleared by the next
     * packet that decodes. */
    unsigned long decode_errors = 0;
};

/*
 * Decode one WebSocket binary frame.
 *
 * `dec` holds the stream's adaptation state and belongs to exactly one
 * WebSocket: the predictor is backward adaptive, so every packet has to reach
 * it (even one whose samples are then dropped) and it has to be reset when a
 * new socket opens.
 *
 * Fills meta from the packet header and the samples into pcm_le, host-order
 * int16.  Unlike versions 1-3 the codec emits host order already, so there is
 * no byte swap.
 */
static bool decode_pcm_v4_frame(ubersdr::PCMv4StreamDecoder &dec,
                                const void *frame, size_t frame_len,
                                PcmMeta &meta,
                                std::vector<int16_t> &pcm_le)
{
    const uint8_t *p = static_cast<const uint8_t *>(frame);

    if (ubersdr::PCMv4StreamDecoder::isZstdFrame(p, frame_len)) {
        /* A server older than 0.1.63 clamps the requested version to 1-3 and
         * answers with a zstd frame rather than refusing.  Say so plainly
         * instead of reporting "bad magic" on every packet. */
        if (++meta.decode_errors == 1)
            fprintf(stderr, "pcm v4: server sent a version 1-3 zstd frame; "
                            "it is older than 0.1.63 and cannot serve version 4\n");
        return false;
    }

    ubersdr::PCMv4Header h;
    std::string err;
    if (!dec.decode(p, frame_len, h, err)) {
        ++meta.decode_errors;
        if (meta.decode_errors == 1 || meta.decode_errors % 500 == 0)
            fprintf(stderr, "pcm v4: %s (%lu consecutive)\n",
                    err.c_str(), meta.decode_errors);
        return false;
    }
    meta.decode_errors = 0;

    meta.sample_rate = (uint32_t)h.sampleRate;
    meta.channels    = (uint8_t)h.channels;
    meta.baseband_power = h.basebandPower;
    meta.noise_dbfs     = h.noise;
    /* -999 is the "radiod reported nothing" sentinel.  Quality rides every
     * resynchronisation packet, and a resync is the first packet the decoder
     * accepts, so this is set from real data from the first frame on. */
    meta.has_signal_quality = (h.basebandPower > -998.0f);

    const int16_t *s = dec.samples();
    if (s == nullptr || h.sampleCount <= 0) {
        pcm_le.clear();
        return true;
    }
    pcm_le.assign(s, s + h.sampleCount);
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
/* Maximum body size for a single NAVTEX message.  Real messages are at most
 * a few hundred characters; 8 KiB is very generous.  If a message body grows
 * beyond this limit we assume NNNN was lost (corrupt transmission or noise
 * burst) and reset the parser so it can re-sync on the next ZCZC. */
static constexpr size_t MAX_MSG_BODY = 8192;

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
                /* Safety valve: if the body grows beyond the maximum allowed
                 * size, the NNNN terminator was almost certainly lost.  Reset
                 * so the parser can re-sync on the next ZCZC. */
                if (body.size() > MAX_MSG_BODY) {
                    in_msg    = false;
                    complete  = false;
                    station   = 0;
                    subject   = 0;
                    serial    = -1;
                    hdr_chars = 0;
                    body.clear();
                    fprintf(stderr, "[navtex] message body exceeded %zu bytes — "
                            "assuming lost NNNN, resetting parser\n", MAX_MSG_BODY);
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

    /* Type 0x05: station identification.
     * Frame: [0x05][ch][UTF-8 JSON ident object]
     * Sent provisionally ("provisional":true) once the header is decoded, and
     * finally when the message completes.  Called from the ident worker
     * thread, never from audio.  The JSON is UTF-8: station names come from
     * the table (which has "Bodø", "Tórshavn"), everything else is ASCII. */
    void send_ident(int ch, const std::string &json)
    {
        if (!server) return;
        std::string frame;
        frame.reserve(2 + json.size());
        frame += (char)0x05;
        frame += (char)(uint8_t)ch;
        frame += json;
        for (auto &ws : server->getClients())
            ws->sendBinary(frame);
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
    std::vector<int16_t> pcm_le;
    bool                 warned_channels = false;

    /* Version 4 stream state.  Backward adaptive, so it belongs to one
     * WebSocket and is reset at the top of every session in run_channel();
     * carrying the previous session's taps over would decode the first packets
     * of the new one as noise. */
    ubersdr::PCMv4StreamDecoder v4;

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

/* ================================================================== */
/* MQTT publishing via UberSDR's addon ingest port                     */
/* ================================================================== */
/*
 * UberSDR exposes an ingest port on the sdr-network that lets addons publish
 * through the receiver's own MQTT connection, and declare Home Assistant
 * entities, without holding any broker credentials. See addon_mqtt.md in the
 * ka9q_ubersdr repository for the full API.
 *
 * This is not optional and needs no configuration. The endpoint is derived from
 * the UberSDR URL this decoder is already using, so the existing
 * docker-compose.yml is sufficient as-is. When the receiver has MQTT disabled,
 * or this container is not a recognised addon, publishing is skipped and the
 * decoder runs exactly as before.
 *
 * Two topics:
 *
 *   messages  — one message per completed NAVTEX transmission, INCLUDING the
 *               full decoded text (not retained)
 *   summary   — counters and last-message state, retained, every 30 s
 *
 * Publishing never blocks decoding. Completed messages go onto a bounded queue
 * drained by one background thread: the completion hook runs on the channel's
 * audio callback, where a stalled HTTP request would drop incoming samples.
 */

/* UberSDR's mqtt.addon_ingest.port default. */
static constexpr int MQTT_DEFAULT_INGEST_PORT = 6926;

/* Populates this addon's Home Assistant device card. */
static const char *MQTT_ADDON_VERSION = "2.1.0";
static const char *MQTT_ADDON_MODEL   = "NAVTEX / SITOR-B decoder";

/* Must stay well below the receiver's offline_after_sec (default 300) or it
 * would mark us offline between updates and Home Assistant would flap. */
static constexpr int MQTT_SUMMARY_INTERVAL_S = 30;

/* A NAVTEX body is capped at MAX_MSG_BODY (8 KiB) by the parser, and the ingest
 * default payload cap is 64 KiB, so a full message plus its metadata always
 * fits — the whole text is published rather than a summary of it. */
static constexpr size_t MQTT_QUEUE_MAX = 64;

/* Home Assistant caps a state at 255 characters and keeps attributes in the
 * recorder, so the text carried as an ATTRIBUTE is trimmed. The complete text
 * always goes out on the messages topic. */
static constexpr size_t MQTT_TEXT_ATTR_MAX = 1024;

/* NAVTEX subject indicator (B2 character), ITU-R M.540 / IMO NAVTEX Manual.
 * Turning the letter into words is the difference between a Home Assistant
 * card that reads "A" and one that reads "Navigational warning". */
static const char *navtex_subject_name(char subject)
{
    switch (subject) {
    case 'A': return "Navigational warning";
    case 'B': return "Meteorological warning";
    case 'C': return "Ice report";
    case 'D': return "Search and rescue";
    case 'E': return "Meteorological forecast";
    case 'F': return "Pilot service";
    case 'G': return "AIS";
    case 'H': return "LORAN";
    case 'I': return "Spare";
    case 'J': return "SATNAV";
    case 'K': return "Other electronic navaid";
    case 'L': return "Additional navigational warning";
    case 'T': return "Test transmission";
    case 'V': case 'W': case 'X': case 'Y': return "Special service";
    case 'Z': return "No messages on hand";
    default:  return "";
    }
}

/* Replace anything that is not printable ASCII (or ordinary whitespace) with
 * '?'.
 *
 * This is not cosmetic. CCIR476::code_to_char returns a NEGATED code for any
 * bit pattern with no character assigned, and navtex_rx::filter_print passes
 * everything except -1, CR, ALPHA and REP straight to the output — so a garbled
 * SITOR-B decode puts bytes >= 0x80 into the message body. Escaped as-is those
 * bytes are not valid UTF-8, and JSON strings must be: the payload would reach
 * the broker but fail to decode in Home Assistant.
 *
 * NAVTEX is a 5-bit ASCII-subset alphabet, so nothing legitimate is lost, and a
 * '?' is an honest rendering of a character the decoder could not resolve. */
static std::string mqtt_sanitise_text(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c == '\n' || c == '\r' || c == '\t') out += (char)c;
        else if (c >= 0x20 && c < 0x7F)          out += (char)c;
        else                                     out += '?';
    }
    return out;
}

/* Read a boolean field out of a flat JSON object.
 *
 * This file has no JSON parser and pulling one in for two booleans is not
 * worth it — but a plain substring search for "key":true is too fragile: a
 * single space after the colon, which most encoders emit, would silently read
 * as false and quietly disable Home Assistant discovery with no error anywhere.
 * So skip whitespace explicitly rather than assuming a layout. */
static bool json_find_bool(const std::string &body, const char *key, bool dflt)
{
    std::string needle = std::string("\"") + key + "\"";
    size_t p = body.find(needle);
    if (p == std::string::npos) return dflt;

    p += needle.size();
    while (p < body.size() && isspace((unsigned char)body[p])) p++;
    if (p >= body.size() || body[p] != ':') return dflt;
    p++;
    while (p < body.size() && isspace((unsigned char)body[p])) p++;

    if (body.compare(p, 4, "true")  == 0) return true;
    if (body.compare(p, 5, "false") == 0) return false;
    return dflt;
}

/* One completed message, queued for publishing. */
struct MqttMessageEvent {
    long        freq_hz    = 0;
    std::string freq_label;
    std::string channel_name;
    char        station    = 0;
    char        subject    = 0;
    int         serial     = -1;
    std::string text;         /* full ZCZC … NNNN */
    std::string body;         /* body only */
    std::string start_utc;
    std::string end_utc;
    int         duration_s  = 0;
    bool        has_snr     = false;
    double      snr_db      = 0.0;
    double      chars_clean_pct  = 0.0;
    double      chars_fec_pct    = 0.0;
    double      chars_failed_pct = 0.0;
    bool        has_char_stats   = false;

    /* Station identification (see the "Station identification" section).
     * Filled in by the ident worker before the event is queued, so the
     * messages topic carries the answer rather than a letter.  has_ident is
     * false only if the ident queue overflowed and the event had to be
     * published bare. */
    bool        has_ident      = false;
    std::string ident_json;            /* the full ident object */
    bool        st_attributed  = false;
    std::string st_name;
    std::string st_country;            /* display name, e.g. "United Kingdom" */
    std::string st_iso2;
    std::string st_level;              /* high | medium | low | none */
    double      st_confidence  = 0.0;  /* 0..1, as ident.conf */
    double      st_distance_km = -1.0; /* -1: receiver location unknown */
};

/* Number of channels with an open audio WebSocket. Written by run_channel. */
static std::atomic<int> g_mqtt_channels_connected{0};

/* ---------------------------------------------------------------- */
/* Payload building — free functions so the self-test can call them   */
/* (see mqtt_selftest.cpp). Pure: no I/O, no shared state.            */
/* ---------------------------------------------------------------- */

static std::string mqtt_fmt_double(double v, int places)
{
    if (!std::isfinite(v)) return "null";
    char b[64];
    snprintf(b, sizeof(b), "%.*f", places, v);
    return std::string(b);
}

static std::string mqtt_msg_id(const MqttMessageEvent &ev)
{
    if (!ev.station || !ev.subject) return "";
    std::string id;
    id += ev.station;
    id += ev.subject;
    if (ev.serial >= 0) {
        char b[8];
        snprintf(b, sizeof(b), "%02d", ev.serial);
        id += b;
    }
    return id;
}

static std::string mqtt_message_json(const MqttMessageEvent &ev)
{
    std::string j = "{";
    j += "\"id\":\""           + json_escape(mqtt_msg_id(ev)) + "\"";
    j += ",\"freq_hz\":"       + std::to_string(ev.freq_hz);
    j += ",\"freq_khz\":"      + mqtt_fmt_double(ev.freq_hz / 1000.0, 1);
    j += ",\"freq_label\":\""  + json_escape(ev.freq_label) + "\"";
    j += ",\"channel\":\""     + json_escape(ev.channel_name) + "\"";
    j += ",\"station\":\""     + json_escape(ev.station ? std::string(1, ev.station) : "") + "\"";
    j += ",\"subject\":\""     + json_escape(ev.subject ? std::string(1, ev.subject) : "") + "\"";
    j += ",\"subject_name\":\"" + json_escape(navtex_subject_name(ev.subject)) + "\"";
    if (ev.serial >= 0) j += ",\"serial\":" + std::to_string(ev.serial);
    else                j += ",\"serial\":null";
    j += ",\"start_utc\":\""   + json_escape(ev.start_utc) + "\"";
    j += ",\"end_utc\":\""     + json_escape(ev.end_utc)   + "\"";
    j += ",\"duration_s\":"    + std::to_string(ev.duration_s);
    j += ",\"char_count\":"    + std::to_string(ev.body.size());
    if (ev.has_snr) j += ",\"snr_db\":" + mqtt_fmt_double(ev.snr_db, 2);
    else            j += ",\"snr_db\":null";
    if (ev.has_char_stats) {
        j += ",\"chars_clean_pct\":"  + mqtt_fmt_double(ev.chars_clean_pct, 1);
        j += ",\"chars_fec_pct\":"    + mqtt_fmt_double(ev.chars_fec_pct, 1);
        j += ",\"chars_failed_pct\":" + mqtt_fmt_double(ev.chars_failed_pct, 1);
    }
    /* The whole decoded message. A NAVTEX body cannot exceed MAX_MSG_BODY
     * (8 KiB), well inside the ingest payload cap. */
    j += ",\"text\":\""        + json_escape(ev.text) + "\"";
    j += ",\"body\":\""        + json_escape(ev.body) + "\"";

    /* Station identification.  The flat fields are for consumers that want
     * one value without walking the object (a Node-RED switch, a template);
     * the object is the whole answer, facts included.  Unattributed — or not
     * identified at all — reads as nulls rather than as missing keys, so a
     * consumer can tell "no station" from "old publisher". */
    if (ev.has_ident && ev.st_attributed) {
        j += ",\"station_name\":\""    + json_escape(ev.st_name)    + "\"";
        j += ",\"station_country\":\"" + json_escape(ev.st_country) + "\"";
        j += ",\"station_iso2\":\""    + json_escape(ev.st_iso2)    + "\"";
        j += ",\"station_confidence\":" + mqtt_fmt_double(ev.st_confidence, 3);
    } else {
        j += ",\"station_name\":null,\"station_country\":null,\"station_iso2\":null";
        j += ",\"station_confidence\":" + std::string(ev.has_ident ? "0" : "null");
    }
    j += ",\"station_level\":" + (ev.has_ident ? "\"" + json_escape(ev.st_level) + "\""
                                               : std::string("null"));
    if (ev.has_ident && ev.st_attributed && ev.st_distance_km >= 0)
        j += ",\"station_distance_km\":" + mqtt_fmt_double(ev.st_distance_km, 1);
    else
        j += ",\"station_distance_km\":null";
    j += ",\"ident\":" + (ev.has_ident && !ev.ident_json.empty() ? ev.ident_json
                                                               : std::string("null"));
    j += "}";
    return j;
}


class MqttPublisher {
public:
    /* Derive the ingest endpoint from the UberSDR base URL: same host, the
     * ingest port. UBERSDR_INGEST_URL overrides it outright for the rare case
     * where the operator has moved the port.
     *
     * Always plain http: the ingest port is reachable only from the
     * sdr-network, so an https base URL for the public interface is irrelevant
     * here. */
    static std::string derive_base(const std::string &ubersdr_url)
    {
        const char *env = getenv("UBERSDR_INGEST_URL");
        if (env && *env) {
            std::string s(env);
            while (!s.empty() && s.back() == '/') s.pop_back();
            if (!s.empty()) return s;
        }

        /* Strip scheme */
        std::string rest = ubersdr_url;
        size_t scheme = rest.find("://");
        if (scheme != std::string::npos) rest = rest.substr(scheme + 3);

        /* Drop any path, query or fragment */
        size_t cut = rest.find_first_of("/?#");
        if (cut != std::string::npos) rest = rest.substr(0, cut);

        /* Drop credentials */
        size_t at = rest.rfind('@');
        if (at != std::string::npos) rest = rest.substr(at + 1);

        /* Split host from port. An IPv6 literal is bracketed, and its colons
         * must not be mistaken for a port separator. */
        std::string host = rest;
        if (!rest.empty() && rest[0] == '[') {
            size_t close = rest.find(']');
            if (close != std::string::npos) host = rest.substr(0, close + 1);
        } else {
            size_t colon = rest.find(':');
            if (colon != std::string::npos) host = rest.substr(0, colon);
        }

        if (host.empty()) host = "ubersdr";
        return "http://" + host + ":" + std::to_string(MQTT_DEFAULT_INGEST_PORT);
    }

    void start(const std::string &ubersdr_url)
    {
        base_ = derive_base(ubersdr_url);
        fprintf(stderr, "[mqtt] ingest endpoint: %s\n", base_.c_str());

        probe();

        running_ = true;
        worker_  = std::thread(&MqttPublisher::run_worker,  this);
        summary_ = std::thread(&MqttPublisher::run_summary, this);
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lk(mu_);
            running_ = false;
        }
        cv_.notify_all();
        if (worker_.joinable())  worker_.join();
        if (summary_.joinable()) summary_.join();
    }

    /* Queue a completed message. Never blocks: this is called from the audio
     * callback thread. */
    void publish_message(const MqttMessageEvent &ev)
    {
        {
            std::lock_guard<std::mutex> lk(mu_);
            total_messages_++;
            message_times_.push_back(std::chrono::steady_clock::now());
            last_ = ev;
            has_last_ = true;
            per_freq_[ev.freq_label]++;

            if (!available_) return;   /* nothing to send to */

            if (queue_.size() >= MQTT_QUEUE_MAX) {
                dropped_++;
                return;
            }
            queue_.push_back(ev);
        }
        cv_.notify_one();
    }

private:
    /* ---------------- availability ---------------- */

    bool probe()
    {
        std::string body;
        long code = http_get(base_ + "/health", body);

        if (code < 0) {
            unavailable("ingest port unreachable — continuing without MQTT");
            return false;
        }
        if (code == 403) {
            unavailable("this container is not a recognised UberSDR addon — "
                        "continuing without MQTT");
            return false;
        }
        if (code != 200) {
            unavailable("ingest health returned %ld — continuing without MQTT", code);
            return false;
        }

        bool was = available_;
        available_ = true;
        warned_    = false;
        if (was) return true;

        bool ha     = json_find_bool(body, "ha_discovery",  false);
        bool broker = json_find_bool(body, "mqtt_connected", false);
        fprintf(stderr, "[mqtt] connected to ingest (broker=%s, ha_discovery=%s)\n",
                broker ? "true" : "false", ha ? "true" : "false");

        if (ha) declare_entities();
        else fprintf(stderr, "[mqtt] Home Assistant discovery is disabled on the "
                             "receiver — publishing data only\n");
        return true;
    }

    void unavailable(const char *fmt, ...) __attribute__((format(printf, 2, 3)))
    {
        available_ = false;
        if (warned_) return;
        warned_ = true;

        char buf[256];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        fprintf(stderr, "[mqtt] %s\n", buf);
    }

    /* ---------------- Home Assistant ---------------- */

    /* One declaration. Every entity reads the retained summary topic and is
     * told apart by entity_key, so a single publish updates all of them. */
    void declare(const std::string &entity_key,
                 const std::string &component,
                 const std::string &name,
                 const std::string &value_template,
                 const std::string &extra = "")
    {
        std::string j = "{";
        j += "\"sub_topic\":\"summary\"";
        j += ",\"entity_key\":\""  + entity_key + "\"";
        j += ",\"component\":\""   + component  + "\"";
        j += ",\"name\":\""        + json_escape(name) + "\"";
        j += ",\"value_template\":\"" + json_escape(value_template) + "\"";
        if (!extra.empty()) j += "," + extra;
        j += ",\"addon_version\":\"" + std::string(MQTT_ADDON_VERSION) + "\"";
        j += ",\"addon_model\":\""   + std::string(MQTT_ADDON_MODEL)   + "\"";
        j += "}";

        std::string resp;
        long code = http_post_json(base_ + "/discovery", j, resp);
        if (code >= 400) {
            fprintf(stderr, "[mqtt] declare %s rejected (%ld): %s\n",
                    entity_key.c_str(), code, resp.c_str());
            return;
        }
        declared_++;
    }

    void declare_entities()
    {
        declared_ = 0;

        declare("messages_total", "sensor", "Messages Received",
                "{{ value_json.messages_total }}",
                "\"unit_of_measurement\":\"messages\","
                "\"state_class\":\"total_increasing\","
                "\"icon\":\"mdi:message-text\"");

        declare("messages_hour", "sensor", "Messages (Last Hour)",
                "{{ value_json.messages_last_hour }}",
                "\"unit_of_measurement\":\"messages\","
                "\"state_class\":\"measurement\","
                "\"icon\":\"mdi:message-clock\"");

        /* State is the message id (e.g. "EA02"); the decoded text and the rest
         * of the metadata ride along as attributes. */
        declare("last_message", "sensor", "Last Message",
                "{{ value_json.last_message_id | default('', true) }}",
                "\"json_attributes_template\":\""
                + json_escape("{{ value_json.last_message_detail | default({}, true) | tojson }}")
                + "\",\"icon\":\"mdi:script-text\"");

        declare("last_subject", "sensor", "Last Subject",
                "{{ value_json.last_subject_name | default('', true) }}",
                "\"icon\":\"mdi:tag-text\"");

        /* Still the B1 letter, exactly as before: an automation written
         * against "E" must not start receiving "Niton".  The identified
         * transmitter gets entities of its own below. */
        declare("last_station", "sensor", "Last Station",
                "{{ value_json.last_station | default('', true) }}",
                "\"icon\":\"mdi:radio-tower\"");

        declare("last_station_name", "sensor", "Last Station Name",
                "{{ value_json.last_station_name | default('', true) }}",
                "\"icon\":\"mdi:transmission-tower\"");

        declare("last_station_country", "sensor", "Last Station Country",
                "{{ value_json.last_station_country | default('', true) }}",
                "\"icon\":\"mdi:flag\"");

        /* The payload carries a 0..1 fraction, as ident.conf does everywhere
         * else; Home Assistant shows a percentage.
         *
         * "No value" is rendered as the string None, not as an empty string:
         * Home Assistant's MQTT sensor IGNORES an empty payload for a sensor
         * with a unit or state_class ("Ignore empty state"), so after "Niton
         * 97 %" an unidentified message would leave 97 % showing.  "None" is
         * the one payload it turns into unknown. */
        declare("last_station_confidence", "sensor", "Last Station Confidence",
                "{% set c = value_json.get('last_station_confidence') %}"
                "{{ (c * 100) | round(0) if c is not none else 'None' }}",
                "\"unit_of_measurement\":\"%\","
                "\"state_class\":\"measurement\","
                "\"icon\":\"mdi:check-circle-outline\"");

        /* 'None' when unknown, for the same reason as the confidence. */
        declare("last_station_distance", "sensor", "Last Station Distance",
                "{% set d = value_json.get('last_station_distance_km') %}"
                "{{ d if d is not none else 'None' }}",
                "\"unit_of_measurement\":\"km\","
                "\"device_class\":\"distance\","
                "\"state_class\":\"measurement\","
                "\"icon\":\"mdi:map-marker-distance\"");

        declare("last_message_utc", "sensor", "Last Message Time",
                "{{ value_json.last_message_utc | default('', true) }}",
                "\"device_class\":\"timestamp\",\"icon\":\"mdi:clock-outline\"");

        /* NAVTEX lives on 490/518/4209.5 kHz, so kHz is the natural unit.
         * No state_class: this is a current reading, not a series worth
         * accumulating long-term statistics over. */
        declare("last_frequency", "sensor", "Last Frequency",
                "{{ value_json.last_frequency_khz | default('', true) }}",
                "\"unit_of_measurement\":\"kHz\","
                "\"device_class\":\"frequency\","
                "\"icon\":\"mdi:sine-wave\"");

        declare("last_snr", "sensor", "Last Message SNR",
                "{{ value_json.last_snr_db | default('', true) }}",
                "\"unit_of_measurement\":\"dB\","
                "\"state_class\":\"measurement\","
                "\"icon\":\"mdi:signal-variant\"");

        declare("last_quality", "sensor", "Last Message Quality",
                "{{ value_json.last_chars_clean_pct | default('', true) }}",
                "\"unit_of_measurement\":\"%\","
                "\"state_class\":\"measurement\","
                "\"entity_category\":\"diagnostic\","
                "\"icon\":\"mdi:check-decagram\"");

        declare("receiver_link", "binary_sensor", "Receiver Link",
                "{{ 'ON' if value_json.channels_connected > 0 else 'OFF' }}",
                "\"device_class\":\"connectivity\","
                "\"entity_category\":\"diagnostic\"");

        fprintf(stderr, "[mqtt] declared %d Home Assistant entities\n", declared_);
    }

    /* ---------------- payload building ---------------- */

    std::string summary_json()
    {
        std::lock_guard<std::mutex> lk(mu_);

        /* Trim the rolling window before counting. */
        auto cutoff = std::chrono::steady_clock::now() - std::chrono::hours(1);
        while (!message_times_.empty() && message_times_.front() < cutoff)
            message_times_.pop_front();

        std::string j = "{";
        j += "\"messages_total\":"     + std::to_string(total_messages_);
        j += ",\"messages_last_hour\":" + std::to_string(message_times_.size());
        j += ",\"dropped_events\":"    + std::to_string(dropped_);
        j += ",\"channels_connected\":" + std::to_string(g_mqtt_channels_connected.load());

        /* Per-frequency totals, e.g. {"518 kHz": 12, "490 kHz": 3} */
        j += ",\"messages_by_frequency\":{";
        bool first = true;
        for (const auto &kv : per_freq_) {
            if (!first) j += ",";
            first = false;
            j += "\"" + json_escape(kv.first) + "\":" + std::to_string(kv.second);
        }
        j += "}";

        if (has_last_) {
            const MqttMessageEvent &ev = last_;

            /* Top-level fields, one per Home Assistant entity: a value_template
             * reading a top-level key is far more legible than one reaching
             * into a nested object. */
            j += ",\"last_message_id\":\""  + json_escape(mqtt_msg_id(ev)) + "\"";
            j += ",\"last_station\":\""     + json_escape(ev.station ? std::string(1, ev.station) : "") + "\"";
            j += ",\"last_subject\":\""     + json_escape(ev.subject ? std::string(1, ev.subject) : "") + "\"";
            j += ",\"last_subject_name\":\"" + json_escape(navtex_subject_name(ev.subject)) + "\"";
            j += ",\"last_message_utc\":\"" + json_escape(ev.end_utc) + "\"";
            j += ",\"last_frequency_hz\":"  + std::to_string(ev.freq_hz);
            j += ",\"last_frequency_khz\":" + mqtt_fmt_double(ev.freq_hz / 1000.0, 1);
            j += ",\"last_freq_label\":\""  + json_escape(ev.freq_label) + "\"";
            j += ",\"last_char_count\":"    + std::to_string(ev.body.size());
            if (ev.has_snr)
                j += ",\"last_snr_db\":" + mqtt_fmt_double(ev.snr_db, 2);
            if (ev.has_char_stats)
                j += ",\"last_chars_clean_pct\":" + mqtt_fmt_double(ev.chars_clean_pct, 1);

            /* The identified transmitter.  last_station above stays the
             * letter; these name it.  Null when unattributed, so the Home
             * Assistant entity goes blank rather than keeping a stale name. */
            if (ev.has_ident && ev.st_attributed) {
                j += ",\"last_station_name\":\""    + json_escape(ev.st_name)    + "\"";
                j += ",\"last_station_country\":\"" + json_escape(ev.st_country) + "\"";
                j += ",\"last_station_confidence\":" + mqtt_fmt_double(ev.st_confidence, 3);
                j += ",\"last_station_level\":\""   + json_escape(ev.st_level)   + "\"";
                if (ev.st_distance_km >= 0)
                    j += ",\"last_station_distance_km\":" + mqtt_fmt_double(ev.st_distance_km, 1);
                else
                    j += ",\"last_station_distance_km\":null";
            } else {
                j += ",\"last_station_name\":null,\"last_station_country\":null"
                     ",\"last_station_confidence\":null,\"last_station_distance_km\":null";
                j += ",\"last_station_level\":" +
                     (ev.has_ident ? "\"" + json_escape(ev.st_level) + "\"" : std::string("null"));
            }

            /* Attribute bundle for the Last Message entity. The text is trimmed
             * here only — Home Assistant keeps attributes in the recorder, and
             * an 8 KiB body on every state change is not something to put
             * there. The complete text is always on the messages topic. */
            std::string preview = ev.text;
            bool truncated = preview.size() > MQTT_TEXT_ATTR_MAX;
            if (truncated) preview.resize(MQTT_TEXT_ATTR_MAX);

            j += ",\"last_message_detail\":{";
            j += "\"id\":\""           + json_escape(mqtt_msg_id(ev)) + "\"";
            j += ",\"frequency\":\""   + json_escape(ev.freq_label) + "\"";
            j += ",\"channel\":\""     + json_escape(ev.channel_name) + "\"";
            j += ",\"subject\":\""     + json_escape(navtex_subject_name(ev.subject)) + "\"";
            j += ",\"char_count\":"    + std::to_string(ev.body.size());
            j += ",\"duration_s\":"    + std::to_string(ev.duration_s);
            if (ev.has_snr) j += ",\"snr_db\":" + mqtt_fmt_double(ev.snr_db, 2);
            if (ev.has_ident && ev.st_attributed) {
                j += ",\"station_name\":\""    + json_escape(ev.st_name)    + "\"";
                j += ",\"station_country\":\"" + json_escape(ev.st_country) + "\"";
                j += ",\"station_confidence\":" + mqtt_fmt_double(ev.st_confidence, 3);
            }
            j += ",\"truncated\":"     + std::string(truncated ? "true" : "false");
            j += ",\"text\":\""        + json_escape(preview) + "\"";
            j += "}";
        }

        j += "}";
        return j;
    }

    /* ---------------- transport ---------------- */

    void post(const std::string &sub_topic, const std::string &payload, bool retain)
    {
        std::string url = base_ + "/publish/" + sub_topic;
        if (retain) url += "?retain=true";

        std::string resp;
        long code = http_post_json(url, payload, resp);

        if (code < 0) {
            /* The receiver may be restarting. Drop to dormant so the next
             * summary re-probes (and re-declares) rather than logging per
             * message. */
            unavailable("publish %s failed — will retry", sub_topic.c_str());
            return;
        }
        if (code < 300)  return;
        if (code == 503) return;   /* broker down; transient, stay available */
        if (code == 403) { unavailable("no longer a recognised addon — pausing MQTT"); return; }

        fprintf(stderr, "[mqtt] publish %s: HTTP %ld: %s\n",
                sub_topic.c_str(), code, resp.c_str());
    }

    void run_worker()
    {
        for (;;) {
            MqttMessageEvent ev;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [&] { return !running_ || !queue_.empty(); });
                if (!running_ && queue_.empty()) return;
                ev = queue_.front();
                queue_.pop_front();
            }
            post("messages", mqtt_message_json(ev), false);
        }
    }

    void run_summary()
    {
        /* Publish once immediately so Home Assistant has values on subscribe
         * rather than after the first interval. */
        publish_summary();

        for (;;) {
            std::unique_lock<std::mutex> lk(mu_);
            if (cv_.wait_for(lk, std::chrono::seconds(MQTT_SUMMARY_INTERVAL_S),
                             [&] { return !running_; }))
                return;
            lk.unlock();
            publish_summary();
        }
    }

    void publish_summary()
    {
        /* Re-probe while dormant so the addon recovers on its own if the
         * receiver was restarted, or had MQTT enabled, after we started. */
        if (!available_ && !probe()) return;
        post("summary", summary_json(), true);
    }

    std::string base_;
    bool available_ = false;
    bool warned_    = false;
    int  declared_  = 0;

    std::mutex              mu_;
    std::condition_variable cv_;
    bool                    running_ = false;
    std::deque<MqttMessageEvent> queue_;
    uint64_t                dropped_ = 0;

    uint64_t                total_messages_ = 0;
    std::deque<std::chrono::steady_clock::time_point> message_times_;
    std::map<std::string, uint64_t> per_freq_;
    MqttMessageEvent        last_;
    bool                    has_last_ = false;

    std::thread worker_;
    std::thread summary_;
};

static MqttPublisher g_mqtt;

/* ------------------------------------------------------------------ */
/* Station identification: the job the audio threads hand over         */
/* ------------------------------------------------------------------ */
/*
 * Everything about identification lives further down, in the "Station
 * identification" section, because it needs the sidecar readers.  The audio
 * path only needs to know what a job looks like and how to hand one over;
 * ident_submit() takes a short queue lock and returns — it never waits for the
 * worker, never touches the identifier and never does I/O.
 */
enum class IdentJobKind {
    Final,        /* message complete: identify, learn, store, publish  */
    Provisional,  /* header decoded, message in progress: identify only */
    Reidentify,   /* receiver location changed: redo all of history     */
};

struct IdentJob {
    IdentJobKind     kind    = IdentJobKind::Final;
    int              channel = -1;
    NavtexIdentQuery q       {};   /* rx_* are filled in by the worker  */
    std::string      rel;          /* log-relative .txt path, "" = not logged */
    std::string      msg_id;       /* "EA02", for matching on the browser side */

    /* Final only: where the answer has to go once it exists. */
    std::string      latest_key;   /* g_latest_messages key            */
    uint64_t         latest_seq = 0;
    MqttMessageEvent mqtt;         /* published by the worker, with the ident */
    bool             mqtt_sent = false;   /* already published bare (see ident_submit) */
};

static void ident_submit(IdentJob &&job);
static void history_gen_bump();   /* history index, below */

/* "2026-04-08T14:30:22Z" -> epoch seconds, or 0.  Hand-rolled rather than
 * strptime+timegm because the startup replay calls it 32,000 times and timegm
 * consults the timezone database on every call. */
static int64_t iso8601_to_epoch(const std::string &s)
{
    if (s.size() < 19) return 0;
    auto num = [&](size_t at, size_t n) -> int {
        int v = 0;
        for (size_t i = at; i < at + n; i++) {
            if (s[i] < '0' || s[i] > '9') return -1;
            v = v * 10 + (s[i] - '0');
        }
        return v;
    };
    int y = num(0, 4), mo = num(5, 2), d = num(8, 2);
    int h = num(11, 2), mi = num(14, 2), se = num(17, 2);
    if (y < 0 || mo < 1 || mo > 12 || d < 1 || h < 0 || mi < 0 || se < 0) return 0;
    /* days_from_civil (H. Hinnant) */
    y -= mo <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + (int64_t)doe - 719468;
    return days * 86400 + h * 3600 + mi * 60 + se;
}

/* ---------------- cleaning a query before the identifier sees it ----------------
 *
 * Contract section 11.  The identifier learns from every message it is shown,
 * and the algorithm review proved that one bad input can poison what it has
 * learned for good: a non-finite receiver position corrupts the bayes member's
 * state, a NaN SNR produces a bogus 0.999 attribution.  The algorithm has been
 * hardened against these, but the server does not rely on that — nothing
 * non-finite or implausible is ever passed in, whichever path (live or replay)
 * built the query.
 *
 *   - SNR and failed-character rate: dropped (has_* = false) unless finite.
 *     The real logs contain legacy sidecars with "-nan" and "inf" fields.
 *   - receiver position: rx_valid = false unless finite and on the globe.
 *   - start time: must lie between 2000-01-01 and a day from now.  A
 *     Raspberry Pi with no real-time clock that boots before NTP has synced
 *     stamps its messages 1970; a corrupt sidecar can say 2100.  Either one,
 *     replayed in chronological order, lands at one end of history and shifts
 *     the learned schedule for every message after it (a single 2100 message
 *     changed the confidence of 1,791 later ones in review).  Such a message
 *     is NOT identified: the caller logs it and leaves it out.
 */
static constexpr int64_t IDENT_EARLIEST_UTC   = 946684800;   /* 2000-01-01T00:00:00Z */
static constexpr int64_t IDENT_FUTURE_SLACK_S = 86400;

static bool ident_time_plausible(int64_t t)
{
    return t >= IDENT_EARLIEST_UTC && t <= (int64_t)time(nullptr) + IDENT_FUTURE_SLACK_S;
}

/* Clean q in place.  Returns false if the message must not be identified at
 * all (implausible start time); the numeric fields are cleaned either way. */
static bool ident_query_clean(NavtexIdentQuery &q)
{
    if (q.has_snr && !std::isfinite(q.snr_db)) { q.has_snr = false; q.snr_db = 0; }
    if (q.has_chars && !(std::isfinite(q.chars_failed_pct) &&
                         q.chars_failed_pct >= 0 && q.chars_failed_pct <= 100)) {
        q.has_chars = false;
        q.chars_failed_pct = 0;
    }
    if (q.rx_valid && !(std::isfinite(q.rx_lat) && std::isfinite(q.rx_lon) &&
                        q.rx_lat >= -90 && q.rx_lat <= 90 &&
                        q.rx_lon >= -180 && q.rx_lon <= 180)) {
        q.rx_valid = false;
        q.rx_lat = q.rx_lon = 0;
    }
    if (q.duration_s < 0) q.duration_s = 0;
    return ident_time_plausible(q.start_utc);
}

/* ------------------------------------------------------------------ */
/* Message-to-disk logging                                              */
/* ------------------------------------------------------------------ */
/* Returns the log-relative path of the .txt written ("518kHz/2026/10/18/
 * 005228Z_FA73.txt"), or "" if nothing was saved — the ident worker uses it to
 * find the sidecar it must add the identification to. */
static std::string save_message(const ChannelContext &ctx, const MsgParser &mp,
                                const MsgMetrics &metrics)
{
    if (ctx.log_dir.empty()) return "";
    if (mp.body.empty())     return "";

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
        return "";
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
        return "";
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
    /* Which scale the SNR and noise figures below are on.  A sidecar without
     * these keys predates audio protocol version 4 and is on the old
     * noise-DENSITY scale; sidecar_normalise_scale() converts one on the way
     * back out.  Nothing but this program writes these files, so the absence of
     * the stamp is a reliable marker rather than a guess. */
    fprintf(jf, "  \"" NAVTEX_SIDECAR_VERSION_KEY "\": %d,\n", NAVTEX_AUDIO_PROTOCOL_VERSION);
    fprintf(jf, "  \"noise_scale\": \"passband_power\",\n");
    fprintf(jf, "  \"noise_bandwidth_hz\": %.0f,\n", NAVTEX_PASSBAND_HZ);
    fprintf(jf, "  \"freq_hz\": %ld,\n",      ctx.carrier_hz);
    fprintf(jf, "  \"freq_label\": \"%s\",\n", json_escape(ctx.label).c_str());
    /* Escaped: a garbled header can decode to '"' or '\\', and an unescaped
     * one made the whole sidecar invalid JSON (one such file is in the real
     * logs: "subject": """). */
    fprintf(jf, "  \"station\": \"%s\",\n",
            json_escape(std::string(1, mp.station ? mp.station : '?')).c_str());
    fprintf(jf, "  \"subject\": \"%s\",\n",
            json_escape(std::string(1, mp.subject ? mp.subject : '?')).c_str());
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
        double avg_snr = avg_bb - avg_nd;
        if (std::isfinite(avg_bb) && std::isfinite(avg_nd) && std::isfinite(avg_snr)) {
            fprintf(jf, "  \"avg_bb_power_dbfs\": %.2f,\n",      avg_bb);
            fprintf(jf, "  \"avg_noise_density_dbfs\": %.2f,\n", avg_nd);
            fprintf(jf, "  \"avg_snr_db\": %.2f,\n",             avg_snr);
        } else {
            fprintf(jf, "  \"avg_bb_power_dbfs\": null,\n");
            fprintf(jf, "  \"avg_noise_density_dbfs\": null,\n");
            fprintf(jf, "  \"avg_snr_db\": null,\n");
        }
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
        fprintf(jf, "  \"avg_chars_failed_pct\": %.1f,\n",   failed_pct);
    } else {
        fprintf(jf, "  \"avg_signal_confidence\": null,\n");
        fprintf(jf, "  \"avg_chars_clean_pct\": null,\n");
        fprintf(jf, "  \"avg_chars_fec_pct\": null,\n");
        fprintf(jf, "  \"avg_chars_failed_pct\": null,\n");
    }
    fprintf(jf, "  \"char_count\": %zu\n", mp.body.size());
    fprintf(jf, "}\n");
    fclose(jf);

    fprintf(stderr, "[ch%d] saved metrics  -> %s\n", ctx.channel_id, json_path.c_str());

    history_gen_bump();

    char rel[256];
    snprintf(rel, sizeof(rel), "%s/%04d/%02d/%02d/%s.txt", freq_dir.c_str(),
             utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, stem);
    return rel;
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

/* ------------------------------------------------------------------ */
/* In-memory latest-message store                                       */
/* ------------------------------------------------------------------ */
struct LatestMessage {
    std::string freq_label;   /* e.g. "518 kHz" */
    char        station  = 0; /* e.g. 'E' */
    char        subject  = 0; /* e.g. 'A' */
    int         serial   = -1;
    std::string text;         /* full reconstructed message (ZCZC … NNNN) */
    std::string timestamp;    /* ISO-8601 UTC when NNNN was received */
    double      snr_db   = 0.0;
    bool        has_snr  = false;

    /* The full ident object, filled in by the ident worker after the fact
     * ("" until then, served as null).  seq says which completion this entry
     * is, so a slow identification of an older message cannot overwrite the
     * entry of a newer one with the same key. */
    std::string ident_json;
    uint64_t    seq      = 0;
};

/* Key: "<freq_label>:<station>:<subject>", e.g. "518 kHz:E:A" */
static std::map<std::string, LatestMessage> g_latest_messages;
static std::mutex                           g_latest_messages_mu;
static uint64_t                             g_latest_seq = 0;   /* under the mutex */

/* Attach an identification to the latest-message entry it belongs to. */
static void latest_set_ident(const std::string &key, uint64_t seq, const std::string &ident)
{
    std::lock_guard<std::mutex> lk(g_latest_messages_mu);
    auto it = g_latest_messages.find(key);
    if (it != g_latest_messages.end() && it->second.seq == seq)
        it->second.ident_json = ident;
}

/* Build the MQTT event for a completed message.  Mirrors
 * upsert_latest_message's reconstruction of the ZCZC…NNNN text so the two never
 * disagree about what the message said.
 *
 * Not published here any more: the event rides the ident job, and the ident
 * worker publishes it once the station is known, so the messages topic says
 * "Niton" rather than "E".  Still queued and never blocking — see
 * ident_submit(). */
static MqttMessageEvent mqtt_event_from_completed(const ChannelContext &ctx,
                                                  const MsgParser      &mp,
                                                  const MsgMetrics     &metrics)
{
    MqttMessageEvent ev;
    ev.freq_hz      = ctx.carrier_hz;
    ev.freq_label   = ctx.label;
    ev.channel_name = ctx.name;
    ev.station      = mp.station;
    ev.subject      = mp.subject;
    ev.serial       = mp.serial;
    /* Sanitised at the boundary rather than at each use, so every consumer of
     * the event gets UTF-8-safe text. */
    ev.body         = mqtt_sanitise_text(mp.body);
    ev.start_utc    = metrics.start_utc;
    ev.end_utc      = metrics.end_utc;

    if (mp.station && mp.subject && mp.serial >= 0) {
        char hdr[32];
        snprintf(hdr, sizeof(hdr), "ZCZC %c%c%02d\n", mp.station, mp.subject, mp.serial);
        ev.text = hdr;
    } else if (mp.station && mp.subject) {
        ev.text = std::string("ZCZC ") + mp.station + mp.subject + "\n";
    } else {
        ev.text = "ZCZC\n";
    }
    ev.text += ev.body;   /* already sanitised */
    ev.text += "NNNN\n";

    if (!metrics.start_utc.empty() && !metrics.end_utc.empty()) {
        struct tm t0 {}, t1 {};
        strptime(metrics.start_utc.c_str(), "%Y-%m-%dT%H:%M:%SZ", &t0);
        strptime(metrics.end_utc.c_str(),   "%Y-%m-%dT%H:%M:%SZ", &t1);
        time_t e0 = timegm(&t0), e1 = timegm(&t1);
        if (e1 > e0) ev.duration_s = (int)(e1 - e0);
    }

    if (metrics.sq_sample_count > 0) {
        double avg_bb = metrics.sum_bb_power      / metrics.sq_sample_count;
        double avg_nd = metrics.sum_noise_density / metrics.sq_sample_count;
        double snr    = avg_bb - avg_nd;
        if (std::isfinite(snr)) {
            ev.snr_db  = snr;
            ev.has_snr = true;
        }
    }

    uint64_t total = metrics.sum_chars_clean + metrics.sum_chars_fec + metrics.sum_chars_failed;
    if (total > 0) {
        ev.chars_clean_pct  = 100.0 * metrics.sum_chars_clean  / total;
        ev.chars_fec_pct    = 100.0 * metrics.sum_chars_fec    / total;
        ev.chars_failed_pct = 100.0 * metrics.sum_chars_failed / total;
        ev.has_char_stats   = true;
    }
    return ev;
}

/* Returns the entry's seq (0 if nothing was stored), for latest_set_ident(). */
static uint64_t upsert_latest_message(const ChannelContext &ctx,
                                      const MsgParser      &mp,
                                      const MsgMetrics     &metrics)
{
    if (!mp.station || !mp.subject) return 0;

    /* Build the full message text */
    std::string text;
    if (mp.station && mp.subject && mp.serial >= 0)
        text = "ZCZC " + std::string(1, mp.station) + std::string(1, mp.subject)
             + (mp.serial < 10 ? "0" : "") + std::to_string(mp.serial) + "\n";
    else if (mp.station && mp.subject)
        text = "ZCZC " + std::string(1, mp.station) + std::string(1, mp.subject) + "\n";
    else
        text = "ZCZC\n";
    text += mp.body;
    text += "NNNN\n";

    /* Compute SNR */
    double snr = 0.0;
    bool has_snr = false;
    if (metrics.sq_sample_count > 0) {
        double avg_bb = metrics.sum_bb_power      / metrics.sq_sample_count;
        double avg_nd = metrics.sum_noise_density / metrics.sq_sample_count;
        snr = avg_bb - avg_nd;
        has_snr = true;
    }

    std::string key = ctx.label + ":" + mp.station + ":" + mp.subject;

    LatestMessage lm;
    lm.freq_label = ctx.label;
    lm.station    = mp.station;
    lm.subject    = mp.subject;
    lm.serial     = mp.serial;
    lm.text       = std::move(text);
    lm.timestamp  = utc_iso8601_now();
    lm.snr_db     = snr;
    lm.has_snr    = has_snr;

    std::lock_guard<std::mutex> lk(g_latest_messages_mu);
    lm.seq = ++g_latest_seq;
    uint64_t seq = lm.seq;
    g_latest_messages[key] = std::move(lm);
    return seq;
}

/* Serialise g_latest_messages to a JSON array.
 * Forward-declares json_escape which is defined later in the History API section. */
static std::string latest_messages_json()
{
    std::lock_guard<std::mutex> lk(g_latest_messages_mu);
    std::string j = "[";
    bool first = true;
    for (const auto &kv : g_latest_messages) {
        const LatestMessage &lm = kv.second;
        if (!first) j += ",";
        first = false;
        j += "{";
        j += "\"freq\":\""      + json_escape(lm.freq_label)          + "\"";
        j += ",\"station\":\""  + json_escape(std::string(1, lm.station)) + "\"";
        j += ",\"subject\":\""  + json_escape(std::string(1, lm.subject)) + "\"";
        if (lm.serial >= 0)
            j += ",\"serial\":"  + std::to_string(lm.serial);
        else
            j += ",\"serial\":null";
        j += ",\"timestamp\":\"" + json_escape(lm.timestamp) + "\"";
        if (lm.has_snr && std::isfinite(lm.snr_db)) {
            char snrbuf[32];
            snprintf(snrbuf, sizeof(snrbuf), "%.1f", lm.snr_db);
            j += ",\"snr_db\":"  + std::string(snrbuf);
        } else {
            j += ",\"snr_db\":null";
        }
        j += ",\"text\":\""     + json_escape(lm.text) + "\"";
        j += ",\"ident\":"       + (lm.ident_json.empty() ? std::string("null") : lm.ident_json);
        j += "}";
    }
    j += "]";
    return j;
}

/* "EA02" from the parser, as mqtt_msg_id() builds it from an event. */
static std::string mqtt_msg_id_for(const MsgParser &mp)
{
    MqttMessageEvent ev;
    ev.station = mp.station; ev.subject = mp.subject; ev.serial = mp.serial;
    return mqtt_msg_id(ev);
}

/* The identification query for a message on this channel, from what the
 * parser and the metrics accumulator know so far.  Used both when the header
 * has just been decoded (provisional: no duration yet) and on completion.
 *
 * SNR and the failed-character rate are rounded exactly as save_message()
 * writes them, so a live identification and a replay of the same sidecar
 * after a restart see identical inputs. */
static NavtexIdentQuery ident_query_for(const ChannelContext &ctx, const MsgParser &mp,
                                        const MsgMetrics &metrics, int duration_s)
{
    NavtexIdentQuery q {};
    q.freq_hz    = ctx.carrier_hz;
    q.b1         = mp.station;
    q.b2         = mp.subject;
    q.serial     = mp.serial;
    q.start_utc  = iso8601_to_epoch(metrics.start_utc);
    q.duration_s = duration_s;
    /* No start time recorded (it should always be): now, less the duration,
     * is as good an estimate as the clock allows.  If the clock itself is
     * wrong the worker's ident_query_clean() catches it. */
    if (q.start_utc == 0) q.start_utc = (int64_t)time(nullptr) - (duration_s > 0 ? duration_s : 0);
    if (metrics.sq_sample_count > 0) {
        double snr = metrics.sum_bb_power      / metrics.sq_sample_count
                   - metrics.sum_noise_density / metrics.sq_sample_count;
        if (std::isfinite(snr)) { q.has_snr = true; q.snr_db = std::round(snr * 100) / 100; }
    }
    uint64_t total = metrics.sum_chars_clean + metrics.sum_chars_fec + metrics.sum_chars_failed;
    if (metrics.sample_count > 0 && total > 0) {
        q.has_chars = true;
        q.chars_failed_pct = std::round(1000.0 * metrics.sum_chars_failed / total) / 10;
    }
    return q;
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
            /* Header just decoded (B1 B2 serial known, body still to come):
             * ask for a provisional identification so the UI can name the
             * station while the message is still arriving. */
            if (ctx->msg_parser.in_msg && ctx->msg_parser.station && !was_sta) {
                IdentJob job;
                job.kind    = IdentJobKind::Provisional;
                job.channel = ctx->channel_id;
                job.q       = ident_query_for(*ctx, ctx->msg_parser, ctx->msg_metrics, 0);
                job.msg_id  = mqtt_msg_id_for(ctx->msg_parser);
                ident_submit(std::move(job));
            }

            if (ctx->msg_parser.complete && !was_cmp) {
                ctx->msg_metrics.end_utc = utc_iso8601_now();
                std::string rel = save_message(*ctx, ctx->msg_parser, ctx->msg_metrics);
                uint64_t    seq = upsert_latest_message(*ctx, ctx->msg_parser, ctx->msg_metrics);

                /* Identification, the sidecar's "ident" member, the latest
                 * store's ident, the MQTT publish and the final 0x05 frame all
                 * happen on the ident worker.  Queued, not done inline: this
                 * runs on the audio callback thread, where a stalled HTTP
                 * request — or a wait for the worker — would drop samples. */
                if (!ctx->msg_parser.body.empty()) {
                    IdentJob job;
                    job.kind       = IdentJobKind::Final;
                    job.channel    = ctx->channel_id;
                    job.mqtt       = mqtt_event_from_completed(*ctx, ctx->msg_parser,
                                                               ctx->msg_metrics);
                    job.q          = ident_query_for(*ctx, ctx->msg_parser, ctx->msg_metrics,
                                                     job.mqtt.duration_s);
                    job.rel        = rel;
                    job.msg_id     = mqtt_msg_id(job.mqtt);
                    job.latest_key = ctx->label + ":" + ctx->msg_parser.station + ":" +
                                     ctx->msg_parser.subject;
                    job.latest_seq = seq;
                    ident_submit(std::move(job));
                }
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
             "&format=pcm-zstd&version=4"
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
                 "&format=pcm-zstd&version=4"
                 "&user_session_id=%s",
                 ws_base.c_str(), ctx->dial_hz, session_id.c_str());

        /* Reset the decoder at the start of every session so that
         * m_sample_count and all internal state start fresh.  Without
         * this, m_sample_count (int64_t) would accumulate across
         * reconnections and the event-counter comparisons would
         * eventually diverge. */
        fprintf(stderr, "[ch%d] resetting decoder for new session\n", ctx->channel_id);
        ctx->decoder->~navtex_rx();
        new (ctx->decoder) navtex_rx(ctx->current_sample_rate,
                                     false, false,
                                     ctx->broadcast_file);

        /* Same for the wire decoder: a new socket is a new version 4 stream,
         * with its own metadata resynchronisation and its own predictor
         * adaptation.  Reset before any frame of it can arrive. */
        ctx->v4.reset();

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
                if (!connected.exchange(true)) g_mqtt_channels_connected++;
                ws.sendText("{\"type\":\"get_status\"}");
                break;

            case ix::WebSocketMessageType::Close:
                fprintf(stderr, "[ch%d] WebSocket closed: %s\n",
                        ctx->channel_id, msg->closeInfo.reason.c_str());
                if (connected.exchange(false)) g_mqtt_channels_connected--;
                session_done = true;
                break;

            case ix::WebSocketMessageType::Error:
                fprintf(stderr, "[ch%d] WebSocket error: %s\n",
                        ctx->channel_id, msg->errorInfo.reason.c_str());
                session_done = true;
                break;

            case ix::WebSocketMessageType::Message:
                if (msg->binary) {
                    /* Every binary frame must reach the version 4 stream
                     * decoder, even one whose samples are then dropped — it is
                     * the only thing keeping this side's predictor in step with
                     * the server's. */
                    if (!decode_pcm_v4_frame(ctx->v4, msg->str.data(), msg->str.size(),
                                             ctx->meta, ctx->pcm_le))
                        break;

                    /* Recreate decoder if sample rate changed.  On a version 4
                     * stream the rate is authoritative from the first frame
                     * that decodes, because that frame is a resynchronisation
                     * packet and carries it. */
                    if (ctx->meta.sample_rate != 0 &&
                        (int)ctx->meta.sample_rate != ctx->current_sample_rate) {
                        fprintf(stderr, "[ch%d] sample rate changed to %u Hz — resetting decoder\n",
                                ctx->channel_id, ctx->meta.sample_rate);
                        ctx->current_sample_rate = (int)ctx->meta.sample_rate;
                        ctx->decoder->~navtex_rx();
                        new (ctx->decoder) navtex_rx(ctx->current_sample_rate,
                                                     false, false,
                                                     ctx->broadcast_file);
                    }

                    /* mode=usb is mono, and the version 4 header says so on
                     * every resynchronisation.  navtex_rx has no notion of
                     * interleaving, so anything else would be fed to it as if
                     * it were consecutive samples. */
                    if (ctx->meta.channels != 1) {
                        if (!ctx->warned_channels) {
                            ctx->warned_channels = true;
                            fprintf(stderr,
                                    "[ch%d] server reports %u channels, not the mono "
                                    "navtex_rx needs — dropping samples\n",
                                    ctx->channel_id, (unsigned)ctx->meta.channels);
                        }
                        break;
                    }
                    ctx->warned_channels = false;

                    if (!ctx->pcm_le.empty()) {
                        ctx->decoder->process_data(ctx->pcm_le.data(),
                                                   (int)ctx->pcm_le.size());
                        ctx->hub->send_audio(ctx->channel_id, ctx->pcm_le);
                    }

                    if (ctx->meta.has_signal_quality) {
                        ctx->cached_bb_power  = ctx->meta.baseband_power;
                        ctx->cached_noise_den = ctx->meta.noise_dbfs;
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

        /* Release this channel's slot in the connected count however the
         * session ended. A Close event already did it; an Error, a dropped
         * socket, or any other exit path did not, and `connected` goes out of
         * scope on the next loop iteration — so without this the gauge would
         * drift upward on every reconnect. exchange() makes it idempotent. */
        if (connected.exchange(false)) g_mqtt_channels_connected--;

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

/* Walk log_dir and compute per-frequency message counts for the last 24 hours
 * (bucketed by UTC hour) and last 30 days (bucketed by UTC date).
 * Returns a JSON object:
 * {
 *   "freqs": ["518kHz","490kHz",...],
 *   "hours": [                        // 24 entries, index 0 = oldest hour
 *     {"label":"HH:00","counts":[n,n,...]}, ...
 *   ],
 *   "days": [                         // 30 entries, index 0 = oldest day
 *     {"label":"YYYY-MM-DD","counts":[n,n,...]}, ...
 *   ]
 * }
 */
static std::string metrics_json(const std::string &log_dir)
{
    if (log_dir.empty()) return "{}";

    /* Collect all .txt message file paths */
    std::vector<std::string> txt_files;
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
                    txt_files.push_back(full);
            }
        }
        closedir(d);
        for (const auto &sub : subdirs) walk(sub);
    };
    walk(log_dir);

    /* Get current UTC time */
    time_t now = time(nullptr);
    struct tm now_tm {};
    gmtime_r(&now, &now_tm);

    /* Build ordered list of unique frequencies (from directory names) */
    std::vector<std::string> freqs;
    std::map<std::string, int> freq_idx;

    /* Bucket structures: 24 hours (index 0 = 23 hours ago) and 30 days (index 0 = 29 days ago) */
    /* We'll fill these after we know all freqs */
    /* First pass: collect freq names and parse timestamps */
    struct MsgEntry {
        std::string freq;
        int year, mon, mday, hour;
    };
    std::vector<MsgEntry> entries;

    for (const auto &full : txt_files) {
        /* rel path from log_dir */
        std::string rel = full;
        if (rel.substr(0, log_dir.size()) == log_dir)
            rel = rel.substr(log_dir.size());
        if (!rel.empty() && rel[0] == '/') rel = rel.substr(1);

        /* Expected: <freq>/<YYYY>/<MM>/<DD>/<HHMMSSZ_id>.txt */
        size_t p = 0;
        auto tok = [&]() -> std::string {
            size_t q = rel.find('/', p);
            std::string t = (q == std::string::npos) ? rel.substr(p) : rel.substr(p, q-p);
            p = (q == std::string::npos) ? rel.size() : q+1;
            return t;
        };
        std::string freq  = tok();
        std::string syear = tok();
        std::string smon  = tok();
        std::string sday  = tok();
        std::string fname = tok();

        if (freq.empty() || syear.size() != 4 || smon.size() != 2 || sday.size() != 2)
            continue;
        /* fname: HHMMSSZ_id.txt — extract HH */
        if (fname.size() < 2) continue;
        int hour = -1;
        if (fname[0] >= '0' && fname[0] <= '9' && fname[1] >= '0' && fname[1] <= '9')
            hour = (fname[0]-'0')*10 + (fname[1]-'0');
        if (hour < 0 || hour > 23) continue;

        int year = atoi(syear.c_str());
        int mon  = atoi(smon.c_str());
        int mday = atoi(sday.c_str());

        if (freq_idx.find(freq) == freq_idx.end()) {
            freq_idx[freq] = (int)freqs.size();
            freqs.push_back(freq);
        }
        entries.push_back({freq, year, mon, mday, hour});
    }

    int nf = (int)freqs.size();

    /* Hour buckets: last 24 complete hours + current partial hour = 24 slots.
     * Slot i corresponds to (now - (23-i) hours), truncated to the hour. */
    struct HourBucket { char label[8]; std::vector<int> counts; };
    std::vector<HourBucket> hours(24);
    for (int i = 0; i < 24; i++) {
        time_t t = now - (time_t)(23 - i) * 3600;
        struct tm tm2 {};
        gmtime_r(&t, &tm2);
        snprintf(hours[i].label, sizeof(hours[i].label), "%02d:00", tm2.tm_hour);
        hours[i].counts.assign(nf, 0);
    }

    /* Day buckets: last 30 days (index 0 = 29 days ago) */
    struct DayBucket { char label[16]; std::vector<int> counts; };
    std::vector<DayBucket> days(30);
    for (int i = 0; i < 30; i++) {
        time_t t = now - (time_t)(29 - i) * 86400;
        struct tm tm2 {};
        gmtime_r(&t, &tm2);
        snprintf(days[i].label, sizeof(days[i].label), "%04d-%02d-%02d",
                 tm2.tm_year+1900, tm2.tm_mon+1, tm2.tm_mday);
        days[i].counts.assign(nf, 0);
    }

    /* Fill buckets */
    for (const auto &e : entries) {
        int fi = freq_idx[e.freq];

        /* Convert entry timestamp to time_t for comparison */
        struct tm etm {};
        etm.tm_year = e.year - 1900;
        etm.tm_mon  = e.mon  - 1;
        etm.tm_mday = e.mday;
        etm.tm_hour = e.hour;
        etm.tm_min  = 0;
        etm.tm_sec  = 0;
        time_t et = timegm(&etm);

        /* Hour bucket */
        double diff_h = difftime(now, et) / 3600.0;
        if (diff_h >= 0.0 && diff_h < 24.0) {
            int slot = 23 - (int)diff_h;
            if (slot >= 0 && slot < 24) hours[slot].counts[fi]++;
        }

        /* Day bucket */
        double diff_d = difftime(now, et) / 86400.0;
        if (diff_d >= 0.0 && diff_d < 30.0) {
            int slot = 29 - (int)diff_d;
            if (slot >= 0 && slot < 30) days[slot].counts[fi]++;
        }
    }

    /* Build JSON */
    std::string j = "{";

    /* freqs array */
    j += "\"freqs\":[";
    for (int i = 0; i < nf; i++) {
        if (i) j += ",";
        j += "\"" + json_escape(freqs[i]) + "\"";
    }
    j += "]";

    /* hours array */
    j += ",\"hours\":[";
    for (int i = 0; i < 24; i++) {
        if (i) j += ",";
        j += "{\"label\":\"";
        j += hours[i].label;
        j += "\",\"counts\":[";
        for (int f = 0; f < nf; f++) {
            if (f) j += ",";
            j += std::to_string(hours[i].counts[f]);
        }
        j += "]}";
    }
    j += "]";

    /* days array */
    j += ",\"days\":[";
    for (int i = 0; i < 30; i++) {
        if (i) j += ",";
        j += "{\"label\":\"";
        j += days[i].label;
        j += "\",\"counts\":[";
        for (int f = 0; f < nf; f++) {
            if (f) j += ",";
            j += std::to_string(days[i].counts[f]);
        }
        j += "]}";
    }
    j += "]";

    j += "}";
    return j;
}

/* ------------------------------------------------------------------ */
/* Stored metrics: reading records written on either SNR scale          */
/* ------------------------------------------------------------------ */

/*
 * A log directory that survives the audio protocol version 4 upgrade holds
 * sidecars from both scales in one field — see navtex_snr.h.  Records written
 * from now on carry a version stamp; one without it is pre-migration, because
 * nothing but save_message() has ever written these files.
 *
 * Both readers below funnel through here, so a stored SNR is converted in
 * exactly one place and the browser never has to know which scale a record was
 * written on.
 */

/* The raw text of a JSON value for `key` in a flat object, or "" if the key is
 * absent or null.  A substring search, matching how these files are written —
 * one key per line, no nesting. */
static std::string sidecar_field(const std::string &sc, const std::string &key)
{
    auto p = sc.find("\"" + key + "\":");
    if (p == std::string::npos) return "";
    p = sc.find_first_not_of(" \t\r\n", p + key.size() + 3);
    if (p == std::string::npos) return "";
    if (sc[p] == 'n') return ""; /* null */
    auto e = sc.find_first_of(",}\n", p);
    return (e == std::string::npos) ? sc.substr(p) : sc.substr(p, e - p);
}

/* Is this a real JSON number, and a finite one?  Guards against "-nan" and
 * "inf", which older sidecars can contain.
 *
 * Checked against the JSON grammar (-? int frac? exp?) rather than a set of
 * allowed characters: a character set lets "-", "e", "." and "1e999" through,
 * and the last of those parses to infinity — a value that must never reach
 * the identifier (contract section 11) or be embedded in /api/history. */
static bool sidecar_is_number(const std::string &s)
{
    size_t i = 0, n = s.size();
    auto digits = [&] { size_t b = i; while (i < n && isdigit((unsigned char)s[i])) i++; return i > b; };
    if (i < n && s[i] == '-') i++;
    if (i < n && s[i] == '0') i++;
    else if (!digits()) return false;
    if (i < n && s[i] == '.') { i++; if (!digits()) return false; }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < n && (s[i] == '+' || s[i] == '-')) i++;
        if (!digits()) return false;
    }
    return i == n && std::isfinite(strtod(s.c_str(), nullptr));
}

/* A sidecar with no version stamp was written before the migration, so its
 * noise figure is a density and its SNR is 10*log10(passband) too high. */
static bool sidecar_is_legacy_scale(const std::string &sc)
{
    return sidecar_field(sc, NAVTEX_SIDECAR_VERSION_KEY).empty();
}

/* Replace the numeric value of `key` in place.  Returns false if the key is
 * missing or not a number, leaving sc untouched. */
static bool sidecar_set_number(std::string &sc, const std::string &key, double v)
{
    auto p = sc.find("\"" + key + "\":");
    if (p == std::string::npos) return false;
    auto vs = sc.find_first_not_of(" \t\r\n", p + key.size() + 3);
    if (vs == std::string::npos) return false;
    auto ve = sc.find_first_of(",}\n", vs);
    if (ve == std::string::npos) return false;
    if (!sidecar_is_number(sc.substr(vs, ve - vs))) return false;
    char buf[64];
    snprintf(buf, sizeof(buf), "%.2f", v);
    sc.replace(vs, ve - vs, buf);
    return true;
}

/* Put a legacy sidecar's figures onto the version 4 scale, and say so in the
 * object so a reader can tell a converted record from a natively recorded one.
 * The record's own origin version is reported truthfully; only the numbers
 * move. */
static std::string sidecar_normalise_scale(const std::string &sc)
{
    if (sc.empty() || !sidecar_is_legacy_scale(sc)) return sc;

    std::string out = sc;

    std::string snr_s   = sidecar_field(out, "avg_snr_db");
    std::string noise_s = sidecar_field(out, "avg_noise_density_dbfs");
    if (sidecar_is_number(snr_s))
        sidecar_set_number(out, "avg_snr_db", navtex_snr_legacy_to_v4(atof(snr_s.c_str())));
    if (sidecar_is_number(noise_s))
        sidecar_set_number(out, "avg_noise_density_dbfs",
                           navtex_noise_density_to_power(atof(noise_s.c_str())));

    auto brace = out.find('{');
    if (brace == std::string::npos) return out;
    char stamp[256];
    snprintf(stamp, sizeof(stamp),
             "\n  \"" NAVTEX_SIDECAR_VERSION_KEY "\": 2,"
             "\n  \"noise_scale\": \"passband_power\","
             "\n  \"noise_bandwidth_hz\": %.0f,"
             "\n  \"snr_rescaled_from_density\": true,",
             NAVTEX_PASSBAND_HZ);
    out.insert(brace + 1, stamp);
    return out;
}

/* ================================================================== */
/* Station identification                                              */
/* ================================================================== */
/*
 * NAVTEX names its transmitter with one letter, B1, and the letters are
 * re-used: on 518 kHz "E" is Niton in the English Channel, Samsun in the Black
 * Sea, Charleston in South Carolina and four more.  Which one was heard is a
 * question of geography, timing and history — navtex_ident.h answers it.  This
 * section is the plumbing around that answer:
 *
 *   receiver location   where we are, from UberSDR's /api/description or the
 *                       NAVTEX_RX_LAT / NAVTEX_RX_LON override
 *   ident worker        the one thread that owns the identifier, replays the
 *                       log history through it at startup and identifies each
 *                       live message as it completes
 *   sidecar "ident"     the answer, persisted as the last member of the
 *                       message's .json sidecar
 *   history index       an in-memory copy of every sidecar's summary, so
 *                       /api/history no longer re-reads 30,000 files per call
 *
 * THREADS AND LOCKING
 *
 *   audio threads (one per channel, ws_cookie_write)
 *       hand jobs over with ident_submit(): one short lock of the queue mutex,
 *       a push, a notify.  They never wait for the worker, never touch the
 *       identifier, the index or the location, and do no I/O for this.  If the
 *       queue is full (it is bounded) a provisional job is dropped and a final
 *       one is published to MQTT without its identification rather than
 *       blocking.
 *
 *   ident worker (one thread, IdentWorker::run)
 *       the ONLY thread that touches the NavtexIdentifier, so the algorithm
 *       itself needs no locking at all.  It does all the sidecar I/O for
 *       identification, takes the index lock only to copy an entry in or out
 *       (never across I/O or across identify()), and takes the location mutex
 *       only to snapshot it.
 *
 *   HTTP threads (ixwebsocket's pool)
 *       read the index under its shared lock, one entry at a time, so a long
 *       /api/history never holds the worker up for more than one lookup.
 *
 *   location thread
 *       fetches /api/description, swaps g_rx under g_rx_mu, and on a real
 *       change asks the worker for a re-identification (a flag, not a job, so
 *       repeated changes coalesce).
 *
 *   Lock order, where two are ever held together: none are.  Every lock in
 *   this section is taken, used and released without taking another.
 *
 * LEARNING ORDER
 *
 *   The identifier learns from what it has identified (observe()), so the
 *   order it sees messages in matters.  The replay sorts history by start time
 *   and runs it oldest first; live messages that complete during the replay
 *   wait in the queue and are handled after it, so the learned state is
 *   always built chronologically.  A location change re-runs the whole
 *   history from the index (no disk reads) with a fresh identifier, because a
 *   different receiver position changes every answer.
 */

/* Defined with the other history readers, further down. */
static std::string history_read_metrics(const std::string &log_dir, const std::string &txt_rel);
/* Defined with the HTTP helpers, further down. */
static std::string url_decode(const std::string &s);

/* ---------------- station table fingerprint ---------------- */

/* A hash of everything in NAVTEX_STATIONS that can change an answer.  Stored
 * in each sidecar's ident so a regenerated table re-identifies history on the
 * next start, the same way a new NAVTEX_IDENT_VERSION does.  FNV-1a 64: not
 * cryptographic, and does not need to be. */
static std::string ident_table_fingerprint()
{
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&](const std::string &s) {
        for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    };
    char b[512];
    for (int i = 0; i < NAVTEX_STATION_COUNT; i++) {
        const NavtexStation &s = NAVTEX_STATIONS[i];
        snprintf(b, sizeof(b), "%d|%c|%s|%s|%d|%.4f|%.4f|%d|%d|%d|",
                 s.freq_hz, s.id, s.name, s.country, s.navarea, s.lat, s.lon,
                 s.range_nm, s.active, s.n_minutes);
        mix(b);
        for (int k = 0; k < s.n_minutes && k < 12; k++) mix(std::to_string(s.minutes[k]) + ",");
        mix("\n");
    }
    snprintf(b, sizeof(b), "%016llx", (unsigned long long)h);
    return b;
}

static const std::string &ident_table_fp()
{
    static const std::string fp = ident_table_fingerprint();
    return fp;
}

/* ---------------- tiny JSON readers ---------------- */
/*
 * /api/description is the one foreign JSON document this program reads.  Like
 * json_find_bool() above, these are deliberately small: find a key, skip
 * whitespace explicitly rather than assume a layout, and return the value.
 * json_object_at() returns a balanced {...} span so a lookup can be scoped to
 * one object ("lat" inside receiver.gps, not some other "lat").
 */

/* Position of the value for "key" in s at or after `from`, or npos. */
static size_t json_value_pos(const std::string &s, const char *key, size_t from = 0)
{
    std::string needle = std::string("\"") + key + "\"";
    for (size_t p = s.find(needle, from); p != std::string::npos; p = s.find(needle, p + 1)) {
        size_t q = p + needle.size();
        while (q < s.size() && isspace((unsigned char)s[q])) q++;
        if (q >= s.size() || s[q] != ':') continue;   /* a string value, not a key */
        q++;
        while (q < s.size() && isspace((unsigned char)s[q])) q++;
        return q < s.size() ? q : std::string::npos;
    }
    return std::string::npos;
}

/* The balanced {...} starting at s[p] (which must be '{'), strings respected. */
static std::string json_balanced_at(const std::string &s, size_t p)
{
    if (p >= s.size() || (s[p] != '{' && s[p] != '[')) return "";
    int depth = 0;
    bool in_str = false;
    for (size_t i = p; i < s.size(); i++) {
        char c = s[i];
        if (in_str) {
            if (c == '\\') i++;
            else if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') in_str = true;
        else if (c == '{' || c == '[') depth++;
        else if (c == '}' || c == ']') { if (--depth == 0) return s.substr(p, i - p + 1); }
    }
    return "";
}

static std::string json_object_at(const std::string &s, const char *key)
{
    size_t p = json_value_pos(s, key);
    return (p != std::string::npos && s[p] == '{') ? json_balanced_at(s, p) : "";
}

static bool json_number_at(const std::string &s, const char *key, double &out)
{
    size_t p = json_value_pos(s, key);
    if (p == std::string::npos) return false;
    const char *b = s.c_str() + p;
    char *e = nullptr;
    double v = strtod(b, &e);
    if (e == b || !std::isfinite(v)) return false;
    out = v;
    return true;
}

/* Append code point cp to out as UTF-8. */
static void utf8_append(std::string &out, uint32_t cp)
{
    if (cp < 0x80)         out += (char)cp;
    else if (cp < 0x800)   { out += (char)(0xC0 | cp >> 6);  out += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += (char)(0xE0 | cp >> 12); out += (char)(0x80 | (cp >> 6 & 0x3F));
                             out += (char)(0x80 | (cp & 0x3F)); }
    else                   { out += (char)(0xF0 | cp >> 18); out += (char)(0x80 | (cp >> 12 & 0x3F));
                             out += (char)(0x80 | (cp >> 6 & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
}

/* Keep valid UTF-8 (shortest form, no surrogates, <= U+10FFFF); replace
 * control characters and every byte of an invalid sequence with '?'.  For
 * text from UberSDR's description — a receiver in Bodø or São Miguel must
 * read as such — as opposed to decoded NAVTEX text, which is 7-bit by
 * definition and goes through mqtt_sanitise_text() instead. */
static std::string utf8_clean(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { out += (c < 0x20 || c == 0x7F) ? '?' : (char)c; i++; continue; }
        int n = c >= 0xF0 && c <= 0xF4 ? 3 : c >= 0xE0 ? 2 : c >= 0xC2 && c < 0xE0 ? 1 : -1;
        if (c >= 0xF5) n = -1;
        uint32_t cp = n == 1 ? c & 0x1F : n == 2 ? c & 0x0F : c & 0x07;
        bool ok = n > 0;
        for (int k = 1; ok && k <= n; k++) {
            if (i + k >= s.size() || ((unsigned char)s[i + k] & 0xC0) != 0x80) { ok = false; break; }
            cp = cp << 6 | ((unsigned char)s[i + k] & 0x3F);
        }
        if (ok && ((n == 2 && cp < 0x800) || (n == 3 && (cp < 0x10000 || cp > 0x10FFFF)) ||
                   (cp >= 0xD800 && cp <= 0xDFFF)))
            ok = false;
        if (!ok) { out += '?'; i++; continue; }
        out.append(s, i, n + 1);
        i += n + 1;
    }
    return out;
}

static int hex4(const std::string &s, size_t at)
{
    if (at + 4 > s.size()) return -1;
    int v = 0;
    for (size_t i = at; i < at + 4; i++) {
        int d = isdigit((unsigned char)s[i]) ? s[i] - '0'
              : (s[i] >= 'a' && s[i] <= 'f') ? s[i] - 'a' + 10
              : (s[i] >= 'A' && s[i] <= 'F') ? s[i] - 'A' + 10 : -1;
        if (d < 0) return -1;
        v = v * 16 + d;
    }
    return v;
}

/* A JSON string value, unescaped to UTF-8.  \uXXXX escapes are decoded —
 * surrogate pairs included — because UberSDR is a Go server and Go's
 * encoding/json writes & < > as \u0026 \u003c \u003e by default: "Bodø &
 * Tromsø" used to come out as "Bod? ? Troms?".  Newlines and tabs become
 * spaces (these are one-line display strings); json_escape() makes the result
 * safe wherever it is re-emitted. */
static bool json_string_at(const std::string &s, const char *key, std::string &out)
{
    size_t p = json_value_pos(s, key);
    if (p == std::string::npos || s[p] != '"') return false;
    std::string v;
    for (size_t i = p + 1; i < s.size(); i++) {
        char c = s[i];
        if (c == '"') { out = utf8_clean(v); return true; }
        if (c == '\\' && i + 1 < s.size()) {
            char n = s[++i];
            switch (n) {
            case 'n': case 't': case 'r': v += ' '; break;
            case 'b': case 'f':           break;
            case 'u': {
                int cp = hex4(s, i + 1);
                if (cp < 0) { v += '?'; break; }
                i += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 2 < s.size() &&
                    s[i + 1] == '\\' && s[i + 2] == 'u') {
                    int lo = hex4(s, i + 3);
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        utf8_append(v, 0x10000 + (((uint32_t)cp - 0xD800) << 10) + (lo - 0xDC00));
                        i += 6;
                        break;
                    }
                }
                if (cp >= 0xD800 && cp <= 0xDFFF) v += '?';   /* lone surrogate */
                else if (cp < 0x20)               v += ' ';
                else                              utf8_append(v, (uint32_t)cp);
                break;
            }
            default:  v += n;   break;
            }
            continue;
        }
        v += c;
    }
    return false;
}

/* ---------------- receiver location ---------------- */

struct RxLocation {
    bool        valid = false;
    double      lat = 0, lon = 0;
    std::string source = "none";   /* ubersdr | env | none */
    std::string locator, location, callsign;
};

static std::mutex  g_rx_mu;
static RxLocation  g_rx;           /* under g_rx_mu */
static bool        g_rx_env = false;
static double      g_rx_env_lat = 0, g_rx_env_lon = 0;

/* Set by the worker: most recent identifications disagree with where the
 * receiver says it is.  See IdentWorker::note_suspect(). */
static std::atomic<bool> g_rx_pos_suspect{false};

static RxLocation rx_snapshot()
{
    std::lock_guard<std::mutex> lk(g_rx_mu);
    return g_rx;
}

/* NAVTEX_RX_LAT + NAVTEX_RX_LON, both required, both in range.  A half-set or
 * unparseable override is reported and ignored rather than half-applied. */
static void rx_read_env()
{
    const char *la = getenv("NAVTEX_RX_LAT"), *lo = getenv("NAVTEX_RX_LON");
    bool has_la = la && *la, has_lo = lo && *lo;
    if (!has_la && !has_lo) return;
    if (has_la != has_lo) {
        fprintf(stderr, "[ident] NAVTEX_RX_LAT and NAVTEX_RX_LON must both be set — ignoring\n");
        return;
    }
    char *e1 = nullptr, *e2 = nullptr;
    double lat = strtod(la, &e1), lon = strtod(lo, &e2);
    if (*e1 || *e2 || !std::isfinite(lat) || !std::isfinite(lon) ||
        lat < -90 || lat > 90 || lon < -180 || lon > 180) {
        fprintf(stderr, "[ident] NAVTEX_RX_LAT/LON '%s','%s' not valid — ignoring\n", la, lo);
        return;
    }
    g_rx_env = true;
    g_rx_env_lat = lat;
    g_rx_env_lon = lon;
    std::lock_guard<std::mutex> lk(g_rx_mu);
    g_rx.valid = true; g_rx.lat = lat; g_rx.lon = lon; g_rx.source = "env";
    fprintf(stderr, "[ident] receiver location from environment: %.4f, %.4f\n", lat, lon);
}

/* Did the location change enough to change the answers?  Gaining or losing a
 * position always does; moving does when it is more than 5 km. */
static bool rx_changed(const RxLocation &a, const RxLocation &b)
{
    if (a.valid != b.valid) return true;
    if (!a.valid) return false;
    return navtex_great_circle_km(a.lat, a.lon, b.lat, b.lon) > 5.0;
}

static void ident_request_reidentify();   /* IdentWorker, below */
static void ident_flush_deferred();       /* IdentWorker, below */

/* Has the receiver position been settled — the environment override set, or
 * /api/description fetched at least once (with or without a GPS position)?
 * Until it has, "no position" means "not asked yet", not "there is none", and
 * the startup replay does not write sidecars it would only have to rewrite a
 * minute later.  See rx_wait_settled() and IdentWorker::replay(). */
static std::atomic<bool> g_rx_fetched{false};
static std::mutex              g_rx_settle_mu;
static std::condition_variable g_rx_settle_cv;
static bool                    g_rx_startup_over = false;   /* under g_rx_settle_mu */

static bool rx_settled() { return g_rx_env || g_rx_fetched.load(); }

/* The location thread's startup burst is over: either it got an answer or it
 * gave up and fell back to the 60 s retry. */
static void rx_mark_startup_over()
{
    { std::lock_guard<std::mutex> lk(g_rx_settle_mu); g_rx_startup_over = true; }
    g_rx_settle_cv.notify_all();
}

/* Wait (worker thread, before the startup replay) for the location to be
 * settled, or for the startup burst to end — whichever is first. */
static void rx_wait_settled()
{
    std::unique_lock<std::mutex> lk(g_rx_settle_mu);
    g_rx_settle_cv.wait_for(lk, std::chrono::seconds(45),
                            [] { return g_rx_startup_over || rx_settled(); });
}

/* Parse /api/description and install it.  Returns false if the body is not a
 * description at all (so the caller keeps retrying). */
static bool rx_apply_description(const std::string &body)
{
    std::string receiver = json_object_at(body, "receiver");
    if (receiver.empty()) return false;

    RxLocation n;
    std::string gps = json_object_at(receiver, "gps");
    double lat = 0, lon = 0;
    if (!gps.empty() && json_number_at(gps, "lat", lat) && json_number_at(gps, "lon", lon) &&
        !(lat == 0 && lon == 0) &&                         /* 0,0 means "not set" */
        lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180) {
        n.valid = true; n.lat = lat; n.lon = lon; n.source = "ubersdr";
    }
    if (!gps.empty()) json_string_at(gps, "maidenhead", n.locator);
    json_string_at(receiver, "location", n.location);
    json_string_at(receiver, "callsign", n.callsign);

    if (g_rx_env) {   /* the override wins for position; the rest still informs */
        n.valid = true; n.lat = g_rx_env_lat; n.lon = g_rx_env_lon; n.source = "env";
    }

    RxLocation old;
    {
        std::lock_guard<std::mutex> lk(g_rx_mu);
        old  = g_rx;
        g_rx = n;
    }
    if (n.valid)
        fprintf(stderr, "[ident] receiver location: %.4f, %.4f (%s%s%s%s)\n", n.lat, n.lon,
                n.source.c_str(), n.locator.empty() ? "" : ", ", n.locator.c_str(),
                n.location.empty() ? "" : (", " + n.location).c_str());
    else
        fprintf(stderr, "[ident] UberSDR reports no GPS position — set NAVTEX_RX_LAT and "
                        "NAVTEX_RX_LON for better identification\n");
    bool first = !g_rx_fetched.exchange(true);
    g_rx_settle_cv.notify_all();
    if (rx_changed(old, n))  ident_request_reidentify();
    else if (first)          ident_flush_deferred();   /* same answer, now persist it */
    return true;
}

static bool rx_fetch_description(const std::string &base_url, int retry_s)
{
    std::string body;
    long code = http_get(base_url + "/api/description", body);
    if (code == 200 && rx_apply_description(body)) return true;
    fprintf(stderr, "[ident] GET /api/description failed (HTTP %ld) — retrying in %d s\n",
            code, retry_s);
    return false;
}

/* Fetch the receiver description; retry until it succeeds, then refresh every
 * 6 h, so an operator who sets the GPS position in UberSDR later is picked up
 * without a restart.
 *
 * At startup the retries are quick — 2, 4, 8 and 16 s apart, half a minute in
 * all — before settling to once a minute.  In docker-compose this add-on
 * often starts before UberSDR is listening, and with one attempt and then a
 * 60 s wait the startup replay ran without a position, the first success a
 * minute later changed every answer, and history was identified (and all
 * 32,651 sidecars written) twice, with about two minutes before the first
 * answer that used the location.  The worker waits for this burst (see
 * rx_wait_settled) before it replays. */
static void rx_location_thread(std::string base_url)
{
    static const int burst[] = { 2, 4, 8, 16 };
    bool ok = false;
    for (size_t i = 0; !ok; i++) {
        bool last = i >= sizeof(burst) / sizeof(burst[0]);
        ok = rx_fetch_description(base_url, last ? 60 : burst[i]);
        if (ok || last) break;
        std::this_thread::sleep_for(std::chrono::seconds(burst[i]));
    }
    rx_mark_startup_over();
    for (;;) {
        std::this_thread::sleep_for(ok ? std::chrono::hours(6) : std::chrono::seconds(60));
        ok = rx_fetch_description(base_url, 60);
    }
}

/* ---------------- identification records and their JSON ---------------- */

/* A result kept beyond the identify() call that produced it.  The const char*
 * members of NavtexIdentResult may point into the identifier's own buffers,
 * so they are copied into strings and nulled. */
struct IdentRecord {
    NavtexIdentQuery  q {};
    NavtexIdentResult r {};
    std::string       level, reason, member_name[2], member_reason[2];
};

static IdentRecord ident_record(const NavtexIdentQuery &q, const NavtexIdentResult &r)
{
    IdentRecord rec;
    rec.q = q;
    rec.r = r;
    rec.level  = r.level  ? r.level  : navtex_level_for(r.confidence);
    rec.reason = r.reason ? r.reason : "";
    for (int i = 0; i < 2; i++) {
        rec.member_name[i]   = r.member[i].name   ? r.member[i].name   : "";
        rec.member_reason[i] = r.member[i].reason ? r.member[i].reason : "";
        rec.r.member[i].name = rec.r.member[i].reason = nullptr;
    }
    rec.r.level = rec.r.reason = nullptr;
    if (r.station < 0 || r.station >= NAVTEX_STATION_COUNT) {
        rec.r.station = -1;
        rec.level = "none";
    }
    return rec;
}

static bool ident_attributed(const IdentRecord &rec) { return rec.r.station >= 0; }

static std::string jstr(const std::string &s) { return "\"" + json_escape(s) + "\""; }

/* The receiver position this identification used: [lat,lon] or null.  Also
 * the third part of the sidecar's up-to-date check, so it is formatted one
 * way only. */
static std::string ident_rx_json(bool valid, double lat, double lon)
{
    if (!valid) return "null";
    char b[64];
    snprintf(b, sizeof(b), "[%.4f,%.4f]", lat, lon);
    return b;
}

/* The layout of the ident object itself — its fields and the wording of its
 * facts — as opposed to the algorithm (NAVTEX_IDENT_VERSION) or the table.
 * Bump it when ident_json() or navtex_ident_facts() changes what they write,
 * so stored sidecars are brought up to date on the next start rather than
 * keeping the old text until the algorithm next changes. */
/* 2: alts de-duplicated.  3: alternatives under 1 % dropped; facts explain an
 * out-of-range sole candidate and a different-letter alternative, skip the
 * night line for short paths, round percentages as the page does.
 * 4: burst_offset_s and rx_pos_checked; timing facts speak of the transmission
 * run; facts explain capped/withheld results by reason code. */
static const int IDENT_JSON_FORMAT = 4;

/* "v|table|rx|fmt" — what a stored ident must match to be left alone.  The
 * contract's three (algorithm version, station table, receiver position) plus
 * the JSON format above. */
static std::string ident_sig(int v, const std::string &table, const std::string &rx,
                             int fmt = IDENT_JSON_FORMAT)
{
    return std::to_string(v) + "|" + table + "|" + rx + "|" + std::to_string(fmt);
}

/* Distance and bearing from the result, or computed from the query when the
 * algorithm left them unknown but a position exists. */
static void ident_geometry(const IdentRecord &rec, double &dist, double &brg)
{
    dist = rec.r.distance_km;
    brg  = rec.r.bearing_deg;
    if (ident_attributed(rec) && rec.q.rx_valid && (dist < 0 || brg < 0)) {
        const NavtexStation &s = NAVTEX_STATIONS[rec.r.station];
        dist = navtex_great_circle_km(rec.q.rx_lat, rec.q.rx_lon, s.lat, s.lon);
        brg  = navtex_initial_bearing_deg(rec.q.rx_lat, rec.q.rx_lon, s.lat, s.lon);
    }
    if (!ident_attributed(rec) || !rec.q.rx_valid) dist = brg = -1;
}

static std::string ident_station_name_json(int idx)
{
    return (idx >= 0 && idx < NAVTEX_STATION_COUNT) ? jstr(NAVTEX_STATIONS[idx].name) : "null";
}

/* The full ident object — sidecar member, /api/ident, /api/latest, MQTT and
 * the WebSocket 0x05 payload.  `extra` is spliced in before the closing brace
 * (the WebSocket frame adds msg_id and rel so the page can match it to a
 * message). */
static std::string ident_json(const IdentRecord &rec, bool provisional,
                              const std::string &extra = "")
{
    const NavtexIdentResult &r = rec.r;
    const bool have = ident_attributed(rec);
    double dist, brg;
    ident_geometry(rec, dist, brg);

    std::string j = "{";
    j += "\"v\":" + std::to_string(NAVTEX_IDENT_VERSION);
    j += ",\"table\":" + jstr(ident_table_fp());
    j += ",\"rx\":" + ident_rx_json(rec.q.rx_valid, rec.q.rx_lat, rec.q.rx_lon);
    j += ",\"fmt\":" + std::to_string(IDENT_JSON_FORMAT);
    if (have) {
        const NavtexStation &s = NAVTEX_STATIONS[r.station];
        const NavtexCountry *c = navtex_country(s.country);
        j += ",\"key\":"          + jstr(navtex_station_key(r.station));
        j += ",\"name\":"         + jstr(s.name);
        j += ",\"country\":"      + jstr(s.country);
        j += ",\"country_name\":" + jstr(navtex_station_country_name(r.station));
        j += ",\"iso2\":"         + (c ? jstr(c->iso2) : std::string("null"));
        j += ",\"navarea\":"      + std::to_string(s.navarea);
        j += ",\"lat\":"          + mqtt_fmt_double(s.lat, 4);
        j += ",\"lon\":"          + mqtt_fmt_double(s.lon, 4);
    } else {
        j += ",\"key\":null,\"name\":null,\"country\":null,\"country_name\":null"
             ",\"iso2\":null,\"navarea\":null,\"lat\":null,\"lon\":null";
    }
    j += ",\"conf\":"   + mqtt_fmt_double(have ? r.confidence : 0.0, 3);
    j += ",\"level\":"  + jstr(rec.level);
    j += ",\"dist_km\":" + (dist >= 0 ? mqtt_fmt_double(dist, 1) : std::string("null"));
    j += ",\"bearing\":" + (brg  >= 0 ? mqtt_fmt_double(brg,  1) : std::string("null"));
    j += ",\"night\":"   + std::string(r.night_path ? "true" : "false");
    j += ",\"in_slot\":" + std::string(r.in_slot ? "true" : "false");
    j += ",\"burst_offset_s\":" + std::to_string(r.burst_offset_s);
    j += ",\"rx_pos_checked\":" + std::string(r.rx_pos_checked ? "true" : "false");
    j += ",\"slot_offset_s\":" + std::to_string(r.slot_offset_s);
    j += ",\"n_cand\":"  + std::to_string(r.n_candidates);
    j += ",\"agree\":"   + std::string(r.agree ? "true" : "false");
    j += ",\"rx_pos_suspect\":" + std::string(r.rx_pos_suspect ? "true" : "false");
    j += ",\"reason\":"  + jstr(rec.reason);

    j += ",\"members\":[";
    for (int i = 0; i < 2; i++) {
        if (i) j += ",";
        j += "{\"name\":" + jstr(rec.member_name[i]);
        j += ",\"station\":" + ident_station_name_json(r.member[i].station);
        j += ",\"key\":" + (r.member[i].station >= 0 && r.member[i].station < NAVTEX_STATION_COUNT
                            ? jstr(navtex_station_key(r.member[i].station)) : std::string("null"));
        j += ",\"conf\":" + mqtt_fmt_double(r.member[i].confidence, 3);
        if (!rec.member_reason[i].empty()) j += ",\"reason\":" + jstr(rec.member_reason[i]);
        j += "}";
    }
    j += "]";

    /* Alternatives.  The algorithm fills alt[] from its two methods, and
     * when they disagree alt[0] is one method's pick while alt[1] is the
     * other's runner-up — which is quite often the SAME station (about
     * one message in twenty on real logs), and can in principle be the
     * answer itself.  A list that names Tórshavn twice, at two different
     * probabilities, only confuses, so each station is listed once (the
     * first, which is the stronger claim) and never the answer. */
    j += ",\"alts\":[";
    bool first = true;
    for (int i = 0; i < 2; i++) {
        int a = r.alt[i];
        if (a < 0 || a >= NAVTEX_STATION_COUNT) continue;
        if (a == r.station || (i == 1 && a == r.alt[0])) continue;
        if (!(r.alt_p[i] >= 0.01)) continue;   /* "Bombay 0%" is noise, not an alternative */
        const NavtexCountry *c = navtex_country(NAVTEX_STATIONS[a].country);
        if (!first) j += ",";
        first = false;
        j += "{\"key\":" + jstr(navtex_station_key(a));
        j += ",\"name\":" + jstr(NAVTEX_STATIONS[a].name);
        j += ",\"iso2\":" + (c ? jstr(c->iso2) : std::string("null"));
        j += ",\"p\":" + mqtt_fmt_double(r.alt_p[i], 3);
        j += "}";
    }
    j += "]";

    j += ",\"facts\":[";
    first = true;
    for (const auto &f : navtex_ident_facts(rec.q, r)) {
        if (!first) j += ",";
        first = false;
        j += jstr(f);
    }
    j += "]";
    j += ",\"provisional\":" + std::string(provisional ? "true" : "false");
    j += extra;
    j += "}";
    return j;
}

/* The compact "st" summary on each /api/history entry. */
static std::string ident_st_json(const IdentRecord &rec)
{
    if (!ident_attributed(rec)) return "{\"level\":\"none\",\"key\":null}";
    const NavtexStation &s = NAVTEX_STATIONS[rec.r.station];
    const NavtexCountry *c = navtex_country(s.country);
    double dist, brg;
    ident_geometry(rec, dist, brg);
    std::string j = "{";
    j += "\"key\":"           + jstr(navtex_station_key(rec.r.station));
    j += ",\"name\":"         + jstr(s.name);
    j += ",\"iso2\":"         + (c ? jstr(c->iso2) : std::string("null"));
    j += ",\"country_name\":" + jstr(navtex_station_country_name(rec.r.station));
    j += ",\"navarea\":"      + std::to_string(s.navarea);
    j += ",\"conf\":"         + mqtt_fmt_double(rec.r.confidence, 3);
    j += ",\"level\":"        + jstr(rec.level);
    j += ",\"dist_km\":"      + (dist >= 0 ? mqtt_fmt_double(dist, 1) : std::string("null"));
    j += ",\"bearing\":"      + (brg  >= 0 ? mqtt_fmt_double(brg,  1) : std::string("null"));
    j += ",\"night\":"        + std::string(rec.r.night_path ? "true" : "false");
    j += "}";
    return j;
}

/* Copy the identification into an MQTT event's flat fields. */
static void ident_fill_mqtt(MqttMessageEvent &ev, const IdentRecord &rec, const std::string &json)
{
    ev.has_ident     = true;
    ev.ident_json    = json;
    ev.st_attributed = ident_attributed(rec);
    ev.st_level      = rec.level;
    if (ev.st_attributed) {
        const NavtexStation &s = NAVTEX_STATIONS[rec.r.station];
        const NavtexCountry *c = navtex_country(s.country);
        ev.st_name       = s.name;
        ev.st_country    = navtex_station_country_name(rec.r.station);
        ev.st_iso2       = c ? c->iso2 : "";
        ev.st_confidence = rec.r.confidence;
        double brg;
        ident_geometry(rec, ev.st_distance_km, brg);
    }
}

/* ---------------- sidecars ---------------- */

static bool read_whole_file(const std::string &path, std::string &out)
{
    FILE *f = fopen(path.c_str(), "r");
    if (!f) return false;
    out.clear();
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

/* The stored "ident" member of a sidecar, as text, or "". */
static std::string sidecar_ident_member(const std::string &sc)
{
    size_t p = json_value_pos(sc, "ident");
    return (p != std::string::npos && sc[p] == '{') ? json_balanced_at(sc, p) : "";
}

/* The up-to-date signature of a stored ident member.  Written by ident_json(),
 * so the layout is ours: "v", then "table", then "rx". */
static std::string sidecar_ident_sig(const std::string &ident)
{
    if (ident.empty()) return "";
    double v = -1;
    std::string table;
    if (!json_number_at(ident, "v", v) || !json_string_at(ident, "table", table)) return "";
    size_t p = json_value_pos(ident, "rx");
    if (p == std::string::npos) return "";
    std::string rx = ident[p] == 'n' ? "null" : json_balanced_at(ident, p);
    double fmt = 0;   /* absent: written before the format was recorded */
    json_number_at(ident, "fmt", fmt);
    return ident_sig((int)v, table, rx, (int)fmt);
}

/* Is `full` (an absolute or log-dir-relative path) really inside log_dir once
 * symlinks are resolved?  The startup replay walks the log directory and
 * rewrites what it finds; a symlink in there must not let it rewrite a file
 * somewhere else.  log_walk() already refuses to follow links; this is the
 * second lock on the same door, checked right before every write. */
static bool path_inside_dir(const std::string &dir, const std::string &full)
{
    char a[PATH_MAX], b[PATH_MAX];
    if (!realpath(dir.c_str(), a) || !realpath(full.c_str(), b)) return false;
    size_t n = strlen(a);
    return strncmp(a, b, n) == 0 && b[n] == '/';
}

/* Add, or replace, the sidecar's "ident" member, atomically.
 *
 * Everything else in the file is left byte for byte as save_message() wrote
 * it: this only cuts off a previous ident member (always the last one, since
 * this is the only writer of it) and appends the new one.  Written to a
 * temporary file and renamed over the original, so a reader — or a crash —
 * sees the old sidecar or the new one, never half of either.  No fsync: on a
 * crash the worst case is a lost ident member, which the next start's replay
 * rewrites (and a stray .json.tmp, which purge_old_logs() sweeps up).
 *
 * The file's modification time is carried over to the new file.  Log
 * retention (purge_old_logs) goes by file dates, and a rewrite would
 * otherwise make every sidecar look brand new: in review, one start that
 * rewrote all 32,651 sidecars followed by --log-retain-days 30 deleted 13,765
 * messages and left 13,674 of their sidecars behind.  The ident member is a
 * derived annotation, not a new record — the sidecar is still as old as the
 * message.
 *
 * Refuses (returns false, file untouched) anything that is not the flat
 * object save_message() writes: empty, truncated, or with stray text after
 * the closing brace.  Appending to those would only turn one broken file
 * into another. */
static bool sidecar_write_ident(const std::string &json_path, const std::string &ident)
{
    struct stat st0 {};
    if (stat(json_path.c_str(), &st0) != 0 || !S_ISREG(st0.st_mode)) return false;
    std::string sc;
    if (!read_whole_file(json_path, sc)) return false;

    size_t open = sc.find_first_not_of(" \t\r\n");
    if (open == std::string::npos || sc[open] != '{') return false;

    size_t at = sc.find("\n  \"ident\":");
    if (at != std::string::npos) {
        size_t comma = sc.rfind(',', at);
        if (comma == std::string::npos || comma < open) return false;
        sc.resize(comma);
    } else {
        size_t close = sc.rfind('}');
        if (close == std::string::npos || close <= open) return false;
        if (sc.find_first_not_of(" \t\r\n", close + 1) != std::string::npos) return false;
        sc.resize(close);
        while (!sc.empty() && isspace((unsigned char)sc.back())) sc.pop_back();
        /* A flat object's last member ends in a value, never in '}' — a
         * second closing brace means the file is already damaged. */
        if (sc.empty() || sc.back() == '}' || sc.back() == ',') return false;
    }
    /* "{}" (or "{ }"): no members yet, so no separating comma. */
    sc += (sc.back() == '{' ? "\n  \"ident\": " : ",\n  \"ident\": ") + ident + "\n}\n";

    std::string tmp = json_path + ".tmp";
    FILE *f = fopen(tmp.c_str(), "w");
    if (!f) return false;
    bool ok = fwrite(sc.data(), 1, sc.size(), f) == sc.size();
    ok = (fclose(f) == 0) && ok;
    if (ok) {
        struct timespec ts[2] = { st0.st_atim, st0.st_mtim };
        utimensat(AT_FDCWD, tmp.c_str(), ts, 0);   /* best effort */
    }
    if (!ok || rename(tmp.c_str(), json_path.c_str()) != 0) {
        remove(tmp.c_str());
        return false;
    }
    return true;
}

/* ---------------- the history index ---------------- */

/* One message's summary, kept in memory.  Everything /api/history shows that
 * used to come from reading the sidecar on every request, plus the
 * identification. */
struct HistoryIndexEntry {
    std::string      snr;              /* avg_snr_db as written (normalised), or "" */
    bool             snr_rescaled = false;
    std::string      duration;         /* duration_s as written, or "" */
    NavtexIdentQuery q {};             /* replay input (rx_* unset) */
    bool             has_ident = false;
    IdentRecord      ident;
    std::string      st_json;          /* ident_st_json(ident), built once, not per request */
    std::string      disk_sig;         /* ident_sig() of the member on disk, "" = none */
    bool             skipped = false;  /* start time implausible: never identified
                                        * (ident_query_clean); st_json says so */

    void set_ident(const IdentRecord &rec)
    {
        ident     = rec;
        has_ident = true;
        st_json   = ident_st_json(rec);
    }
};

/* Keyed by log-relative .txt path — the same "rel" /api/history hands out. */
static std::shared_mutex                                   g_index_mu;
static std::unordered_map<std::string, HistoryIndexEntry> g_index;

/* Bumped by anything that changes what /api/history would say: an index
 * update, a message saved, a file purged.  Atomic, so save_message() can bump
 * it from an audio thread without a lock.  See history_list_json_cached(). */
static std::atomic<uint64_t> g_history_gen{1};

static void index_forget(const std::string &rel)
{
    std::unique_lock<std::shared_mutex> lk(g_index_mu);
    g_index.erase(rel);
    g_history_gen++;
}

static std::string rel_of(const std::string &log_dir, const std::string &full)
{
    std::string rel = full;
    if (rel.compare(0, log_dir.size(), log_dir) == 0) rel = rel.substr(log_dir.size());
    while (!rel.empty() && rel[0] == '/') rel = rel.substr(1);
    return rel;
}

/* Read one sidecar into an index entry.  Legacy-scale records are normalised
 * first, exactly as the history list always has, so the replay and the page
 * see the same SNR. */
/* The "st" for a message that will not be identified because its start time
 * is not believable (see ident_query_clean). */
static const char *IDENT_ST_SKIPPED =
    "{\"level\":\"none\",\"key\":null,\"skipped\":\"start time not plausible\"}";

/* Read one sidecar into an index entry.  Returns false — not indexed, not
 * replayed — for a file that is not a sidecar save_message() could have
 * written: empty (a crash between fopen and fclose), truncated, or missing
 * the station or subject.  Indexing those used to show them as identified
 * "Unidentified" rows with B1 '?', and count them as FAILED on every start. */
static bool sidecar_load(const std::string &json_path, const std::string &rel,
                         HistoryIndexEntry &e)
{
    std::string raw;
    if (!read_whole_file(json_path, raw)) return false;
    {
        size_t a = raw.find_first_not_of(" \t\r\n"), z = raw.find_last_not_of(" \t\r\n");
        if (a == std::string::npos || raw[a] != '{' || raw[z] != '}' || z <= a) return false;
    }

    e.snr_rescaled = sidecar_is_legacy_scale(raw);
    std::string sc = sidecar_normalise_scale(raw);

    std::string snr = sidecar_field(sc, "avg_snr_db");
    std::string dur = sidecar_field(sc, "duration_s");
    e.snr      = sidecar_is_number(snr) ? snr : "";
    e.duration = sidecar_is_number(dur) ? dur : "";
    if (e.snr.empty()) e.snr_rescaled = false;

    NavtexIdentQuery &q = e.q;
    q = NavtexIdentQuery {};
    std::string f = sidecar_field(sc, "freq_hz");
    q.freq_hz = sidecar_is_number(f) ? atol(f.c_str()) : 0;
    if (q.freq_hz <= 0) q.freq_hz = atol(rel.c_str()) * 1000;   /* "518kHz/..." */

    std::string st = sidecar_field(sc, "station"), su = sidecar_field(sc, "subject");
    if (st.size() < 3 || st[0] != '"' || su.size() < 3 || su[0] != '"') return false;
    /* "\"" or "\\" — escaped by save_message() since a garbled header can
     * produce them. */
    q.b1 = st[1] == '\\' && st.size() >= 4 ? st[2] : st[1];
    q.b2 = su[1] == '\\' && su.size() >= 4 ? su[2] : su[1];
    std::string ser = sidecar_field(sc, "serial");
    q.serial = sidecar_is_number(ser) ? atoi(ser.c_str()) : -1;

    std::string t = sidecar_field(sc, "start_utc");
    if (t.size() > 2 && t[0] == '"') t = t.substr(1, t.size() - 2);
    q.start_utc = iso8601_to_epoch(t);
    if (!ident_time_plausible(q.start_utc)) {
        /* Fall back on the file name: <freq>/YYYY/MM/DD/HHMMSSZ_... is the
         * END time, which is still the right hour and the right order. */
        size_t s1 = rel.find('/');
        if (s1 != std::string::npos && rel.size() > s1 + 18) {
            std::string iso = rel.substr(s1 + 1, 4) + "-" + rel.substr(s1 + 6, 2) + "-" +
                              rel.substr(s1 + 9, 2) + "T" + rel.substr(s1 + 12, 2) + ":" +
                              rel.substr(s1 + 14, 2) + ":" + rel.substr(s1 + 16, 2) + "Z";
            q.start_utc = iso8601_to_epoch(iso);
        }
    }
    q.duration_s = sidecar_is_number(dur) ? atoi(dur.c_str()) : 0;
    if (!e.snr.empty()) { q.has_snr = true; q.snr_db = atof(e.snr.c_str()); }
    std::string fp = sidecar_field(sc, "avg_chars_failed_pct");
    if (sidecar_is_number(fp)) { q.has_chars = true; q.chars_failed_pct = atof(fp.c_str()); }

    /* Section 11: whatever survived the parsing above is cleaned once more,
     * and a message whose time is still not believable (the file name too
     * says 1970, or the future) is kept in the index — it is still a message
     * on disk with an SNR — but never shown to the identifier. */
    e.skipped = !ident_query_clean(q);
    if (e.skipped) e.st_json = IDENT_ST_SKIPPED;

    e.disk_sig = sidecar_ident_sig(sidecar_ident_member(raw));
    return true;
}

/* Directory walk on d_type, without a stat() per file — the log directory
 * holds two files per message, and stat on 60,000 of them was most of what
 * /api/history used to cost.  Falls back on lstat() for a filesystem that does
 * not report types.
 *
 * Symbolic links are NOT followed, to files or directories.  This program
 * never creates one, and following them let the replay rewrite sidecars
 * outside the log directory and, given a link back up the tree, recurse
 * forever (review: 118,780 "messages" collected from one, and climbing). */
static void log_walk(const std::string &dir,
                     const std::function<void(const std::string &full, const char *name)> &file_cb)
{
    DIR *d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> subdirs;
    struct dirent *ent;
    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        std::string full = dir + "/" + ent->d_name;
        unsigned char type = ent->d_type;
        if (type == DT_LNK) continue;
        if (type == DT_UNKNOWN) {
            struct stat st {};
            if (lstat(full.c_str(), &st) != 0) continue;
            type = S_ISDIR(st.st_mode) ? DT_DIR : S_ISREG(st.st_mode) ? DT_REG : DT_UNKNOWN;
        }
        if (type == DT_DIR)      subdirs.push_back(full);
        else if (type == DT_REG) file_cb(full, ent->d_name);
    }
    closedir(d);
    for (const auto &sub : subdirs) log_walk(sub, file_cb);
}

static bool ends_with(const char *name, const char *suffix)
{
    size_t n = strlen(name), m = strlen(suffix);
    return n > m && strcmp(name + n - m, suffix) == 0;
}

/* ---------------- the worker ---------------- */

/* Copying the identifier for a provisional answer.  Tag-dispatched so an
 * identifier that cannot be copied simply has no provisional answers, rather
 * than failing to compile. */
template <typename T>
static T *ident_clone(const T &src, std::true_type) { return new T(src); }
template <typename T>
static T *ident_clone(const T &, std::false_type) { return nullptr; }

/* A provisional answer is a nicety; if copying the learned state costs more
 * than this, it is turned off rather than delay final answers behind it. */
static constexpr double IDENT_PROVISIONAL_MAX_COPY_US = 50000.0;

/* Bounded so a wedged worker cannot grow memory without limit.  At NAVTEX
 * rates (a message a minute per channel) this is hours of backlog. */
static constexpr size_t IDENT_QUEUE_MAX = 2048;

class IdentWorker {
public:
    void start(const std::string &log_dir, BroadcastHub *hub)
    {
        log_dir_ = log_dir;
        hub_     = hub;
        started_ = true;
        thread_  = std::thread(&IdentWorker::run, this);
        thread_.detach();   /* runs for the life of the process */
    }

    /* Called from audio threads.  Never blocks beyond the queue mutex.
     * Returns false (job untouched) when the queue is full. */
    bool try_push(IdentJob &job)
    {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (q_.size() >= IDENT_QUEUE_MAX) return false;
            q_.push_back(std::move(job));
        }
        cv_.notify_one();
        return true;
    }

    void request_reidentify()
    {
        if (!started_) return;   /* the startup replay will use the new location */
        {
            std::lock_guard<std::mutex> lk(mu_);
            reident_ = true;
        }
        cv_.notify_one();
    }

    /* The location lookup has finally answered and the position is what the
     * last replay assumed: the answers stand, but that replay skipped its disk
     * writes (deferred_), so replay again to write them. */
    void flush_deferred()
    {
        if (deferred_) request_reidentify();
    }

    /* The last 0x05 frame per channel, for a browser that connects mid-
     * message.  Sent only if it still describes what the channel is doing: a
     * provisional for the message in progress, or a final when idle. */
    std::string frame_for_new_client(int ch, bool in_msg, const std::string &cur_id)
    {
        std::lock_guard<std::mutex> lk(frames_mu_);
        auto it = frames_.find(ch);
        if (it == frames_.end()) return "";
        const LastFrame &f = it->second;
        if (in_msg ? (f.provisional && f.msg_id == cur_id) : !f.provisional) return f.frame;
        return "";
    }

    std::string status_json()
    {
        size_t qn;
        { std::lock_guard<std::mutex> lk(mu_); qn = q_.size(); }
        static const char *phases[] = { "starting", "collecting", "identifying", "done" };
        std::string j = "{";
        j += "\"done\":" + std::string(phase_.load() == 3 ? "true" : "false");
        j += ",\"phase\":\"" + std::string(phases[phase_.load() & 3]) + "\"";
        j += ",\"processed\":" + std::to_string(processed_.load());
        j += ",\"total\":" + std::to_string(total_.load());
        j += ",\"elapsed_ms\":" + std::to_string(elapsed_ms_.load());
        j += ",\"queued\":" + std::to_string(qn);
        j += ",\"provisional\":" + std::string(provisional_ok_.load() ? "true" : "false");
        j += ",\"provisional_copy_us\":" + std::to_string(copy_us_.load());
        j += "}";
        return j;
    }

    bool replay_done() const { return phase_.load() == 3; }

private:
    struct LastFrame { std::string frame, msg_id; bool provisional = false; };
    struct ReplayItem { std::string rel; NavtexIdentQuery q; };

    void run()
    {
        /* Give the location lookup its startup burst (about 30 s at most)
         * before replaying, so the replay — which is what writes 30,000-odd
         * sidecars — normally runs once, with the position. */
        if (!rx_settled()) {
            fprintf(stderr, "[ident] waiting for the receiver location before replaying history\n");
            rx_wait_settled();
        }
        { std::lock_guard<std::mutex> lk(mu_); reident_ = false; }   /* this replay covers it */
        replay(true);
        for (;;) {
            IdentJob job;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [&] { return reident_ || !q_.empty(); });
                if (reident_) {
                    reident_ = false;
                    lk.unlock();
                    fprintf(stderr, "[ident] receiver location changed — re-identifying history\n");
                    replay(false);
                    continue;
                }
                job = std::move(q_.front());
                q_.pop_front();
            }
            if (job.kind == IdentJobKind::Final)            handle_final(job);
            else if (job.kind == IdentJobKind::Provisional) handle_provisional(job);
        }
    }

    static void apply_rx(NavtexIdentQuery &q, const RxLocation &rx)
    {
        q.rx_valid = rx.valid;
        q.rx_lat   = rx.valid ? rx.lat : 0;
        q.rx_lon   = rx.valid ? rx.lon : 0;
        ident_query_clean(q);   /* position range/finiteness; time checked by callers */
    }

    /* identify() then observe(), on the one identifier.  Worker thread only. */
    IdentRecord identify_and_learn(NavtexIdentQuery q, const RxLocation &rx)
    {
        apply_rx(q, rx);
        NavtexIdentResult r = ident_->identify(q);
        ident_->observe(q, r);
        IdentRecord rec = ident_record(q, r);
        note_suspect(rec);
        return rec;
    }

    /* The receiver-location warning on /api/stations: raised when more than
     * half of the last 100 attributed messages had the algorithm doubting the
     * position.  One odd message is propagation; a majority is a wrong
     * location. */
    void note_suspect(const IdentRecord &rec)
    {
        if (!ident_attributed(rec)) return;
        suspect_win_.push_back(rec.r.rx_pos_suspect);
        suspect_n_ += rec.r.rx_pos_suspect;
        if (suspect_win_.size() > 100) {
            suspect_n_ -= suspect_win_.front();
            suspect_win_.pop_front();
        }
        g_rx_pos_suspect = suspect_win_.size() >= 20 && suspect_n_ * 2 > (int)suspect_win_.size();
    }

    /* Rebuild the learned state from history, oldest first.
     *
     * from_disk: the startup replay — walk the log directory, read every
     * sidecar into the index, then identify.  Otherwise (location change) the
     * index already holds every query, so nothing is read.
     *
     * The algorithm runs on every message regardless: it has to, to rebuild
     * what it has learned.  Only the sidecar write is skipped when the stored
     * ident was made by this algorithm version, with this station table, at
     * this receiver position. */
    void replay(bool from_disk)
    {
        auto t0 = std::chrono::steady_clock::now();
        auto ms_since = [](std::chrono::steady_clock::time_point t) {
            return (long)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t).count();
        };
        processed_ = 0;
        total_     = 0;
        phase_     = 1;

        std::vector<ReplayItem> items;
        int n_skipped = 0;
        if (from_disk && !log_dir_.empty()) {
            std::vector<std::pair<std::string, HistoryIndexEntry>> batch;
            auto flush = [&] {
                std::unique_lock<std::shared_mutex> lk(g_index_mu);
                for (auto &kv : batch) g_index.emplace(std::move(kv.first), std::move(kv.second));
                g_history_gen++;
                batch.clear();
            };
            log_walk(log_dir_, [&](const std::string &full, const char *name) {
                if (!ends_with(name, ".txt")) return;
                std::string json_path = full.substr(0, full.size() - 4) + ".json";
                std::string rel = rel_of(log_dir_, full);
                HistoryIndexEntry e;
                if (!sidecar_load(json_path, rel, e)) return;   /* no sidecar: nothing to learn */
                if (e.skipped) {
                    if (++n_skipped <= 5)
                        fprintf(stderr, "[ident] %s: start time not plausible (%lld) — "
                                        "not identified\n", rel.c_str(), (long long)e.q.start_utc);
                } else {
                    items.push_back({ rel, e.q });
                }
                batch.emplace_back(rel, std::move(e));
                total_ = (int)items.size();
                if (batch.size() >= 512) flush();
            });
            flush();
        } else if (!from_disk) {
            std::shared_lock<std::shared_mutex> lk(g_index_mu);
            items.reserve(g_index.size());
            for (const auto &kv : g_index)
                if (!kv.second.skipped) items.push_back({ kv.first, kv.second.q });
                else n_skipped++;
        }
        if (n_skipped)
            fprintf(stderr, "[ident] %d message(s) with an implausible start time left out "
                            "of identification\n", n_skipped);
        long collect_ms = ms_since(t0);

        std::sort(items.begin(), items.end(), [](const ReplayItem &a, const ReplayItem &b) {
            return a.q.start_utc != b.q.start_utc ? a.q.start_utc < b.q.start_utc : a.rel < b.rel;
        });
        total_ = (int)items.size();
        phase_ = 2;
        fprintf(stderr, "[ident] replaying %zu messages (collected in %ld ms)\n",
                items.size(), collect_ms);

        ident_.reset(new NavtexIdentifier());   /* forget everything learned */
        suspect_win_.clear();
        suspect_n_ = 0;
        const RxLocation  rx  = rx_snapshot();
        const std::string now = ident_sig(NAVTEX_IDENT_VERSION, ident_table_fp(),
                                          ident_rx_json(rx.valid, rx.lat, rx.lon));
        /* Location lookup still unanswered (UberSDR not up after the startup
         * burst): identify, so the page and the learned state are ready, but
         * leave the disk alone — the answer is about to change when the
         * position arrives, and writing every sidecar twice is pointless wear
         * on an SD card.  The first successful lookup replays again and
         * writes (flush_deferred / a location change). */
        const bool write = rx_settled();
        deferred_ = !write;
        if (!write)
            fprintf(stderr, "[ident] receiver location not known yet — identifying without "
                            "writing sidecars until it is\n");
        double ident_us = 0, write_us = 0;
        int written = 0, skipped = 0, failed = 0;

        for (size_t i = 0; i < items.size(); i++) {
            const ReplayItem &it = items[i];
            auto a = std::chrono::steady_clock::now();
            IdentRecord rec = identify_and_learn(it.q, rx);
            auto b = std::chrono::steady_clock::now();
            ident_us += std::chrono::duration<double, std::micro>(b - a).count();

            std::string disk_sig;
            {
                std::unique_lock<std::shared_mutex> lk(g_index_mu);
                auto e = g_index.find(it.rel);
                if (e == g_index.end()) { processed_ = (int)i + 1; continue; }   /* purged meanwhile */
                e->second.set_ident(rec);
                disk_sig = e->second.disk_sig;
                g_history_gen++;
            }
            if (disk_sig == now || !write) {
                skipped++;
            } else {
                std::string path = log_dir_ + "/" + it.rel;
                path = path.substr(0, path.size() - 4) + ".json";
                if (path_inside_dir(log_dir_, path) &&
                    sidecar_write_ident(path, ident_json(rec, false))) {
                    written++;
                    std::unique_lock<std::shared_mutex> lk(g_index_mu);
                    auto e = g_index.find(it.rel);
                    if (e != g_index.end()) e->second.disk_sig = now;
                } else {
                    failed++;
                }
                write_us += std::chrono::duration<double, std::micro>(
                    std::chrono::steady_clock::now() - b).count();
            }
            processed_ = (int)i + 1;
            if ((i + 1) % 5000 == 0)
                fprintf(stderr, "[ident] replay %zu / %zu\n", i + 1, items.size());
        }

        long total_ms = ms_since(t0);
        elapsed_ms_ = total_ms;
        phase_ = 3;
        double n = items.empty() ? 1.0 : (double)items.size();
        fprintf(stderr,
                "[ident] replay done: %zu messages in %ld ms (collect %ld ms, identify %.0f ms "
                "= %.1f us/msg, write %.0f ms) — %d sidecars written, %d %s%s%s\n",
                items.size(), total_ms, collect_ms, ident_us / 1000, ident_us / n,
                write_us / 1000, written, skipped, write ? "up to date" : "deferred",
                failed ? ", FAILED: " : "", failed ? std::to_string(failed).c_str() : "");
    }

    void remember_frame(int ch, const std::string &frame, const std::string &msg_id, bool prov)
    {
        std::lock_guard<std::mutex> lk(frames_mu_);
        frames_[ch] = LastFrame{ frame, msg_id, prov };
    }

    std::string ws_extra(const IdentJob &job)
    {
        std::string x = ",\"msg_id\":" + jstr(job.msg_id);
        if (!job.rel.empty()) x += ",\"rel\":" + jstr(job.rel);
        return x;
    }

    /* A message completed.  Identify and learn, persist, then fan out:
     * index, latest store, MQTT, browsers. */
    void handle_final(IdentJob &job)
    {
        const bool logged = !job.rel.empty() && !log_dir_.empty();
        IdentRecord rec;
        bool known = false;

        /* Already identified by the startup replay (the message completed
         * while the directory was being walked): reuse that answer rather
         * than learn from the same message twice. */
        if (logged) {
            std::shared_lock<std::shared_mutex> lk(g_index_mu);
            auto e = g_index.find(job.rel);
            if (e != g_index.end() && e->second.has_ident) { rec = e->second.ident; known = true; }
        }

        /* Section 11, live side: a message timed by a clock that is plainly
         * wrong (no RTC, NTP not synced yet) is not identified, and not
         * learned from.  It is still published, and the page is told it is
         * unidentified so it does not wait on "identifying…". */
        if (!known && !ident_query_clean(job.q)) {
            fprintf(stderr, "[ident] ch%d %s: start time not plausible (%lld) — not identified; "
                            "is the system clock set?\n", job.channel, job.msg_id.c_str(),
                    (long long)job.q.start_utc);
            if (logged) {
                std::string path = log_dir_ + "/" + job.rel;
                path = path.substr(0, path.size() - 4) + ".json";
                HistoryIndexEntry e;
                if (sidecar_load(path, job.rel, e)) {
                    e.skipped = true;
                    e.st_json = IDENT_ST_SKIPPED;
                    std::unique_lock<std::shared_mutex> lk(g_index_mu);
                    g_index[job.rel] = std::move(e);
                    g_history_gen++;
                }
            }
            if (!job.mqtt_sent) g_mqtt.publish_message(job.mqtt);
            std::string frame = "{\"key\":null,\"level\":\"none\",\"provisional\":false,"
                                "\"skipped\":\"start time not plausible\",\"facts\":"
                                "[\"Start time not plausible — is the system clock set?\"]" +
                                ws_extra(job) + "}";
            remember_frame(job.channel, frame, job.msg_id, false);
            if (hub_) hub_->send_ident(job.channel, frame);
            return;
        }

        if (!known) {
            rec = identify_and_learn(job.q, rx_snapshot());
            std::string json = ident_json(rec, false);
            if (logged) {
                std::string path = log_dir_ + "/" + job.rel;
                path = path.substr(0, path.size() - 4) + ".json";
                HistoryIndexEntry e;
                if (!sidecar_load(path, job.rel, e) || e.skipped) { e = HistoryIndexEntry{}; e.q = job.q; }
                bool ok = path_inside_dir(log_dir_, path) && sidecar_write_ident(path, json);
                if (!ok) fprintf(stderr, "[ident] could not update %s\n", path.c_str());
                e.set_ident(rec);
                e.disk_sig  = ok ? ident_sig(NAVTEX_IDENT_VERSION, ident_table_fp(),
                                             ident_rx_json(rec.q.rx_valid, rec.q.rx_lat,
                                                           rec.q.rx_lon))
                                 : e.disk_sig;
                std::unique_lock<std::shared_mutex> lk(g_index_mu);
                g_index[job.rel] = std::move(e);
                g_history_gen++;
            }
        }

        std::string json = ident_json(rec, false);
        if (!job.latest_key.empty()) latest_set_ident(job.latest_key, job.latest_seq, json);

        if (!job.mqtt_sent) {
            ident_fill_mqtt(job.mqtt, rec, json);
            g_mqtt.publish_message(job.mqtt);   /* queued; never blocks */
        }

        std::string frame = ident_json(rec, false, ws_extra(job));
        remember_frame(job.channel, frame, job.msg_id, false);
        if (hub_) hub_->send_ident(job.channel, frame);

        fprintf(stderr, "[ident] ch%d %s -> %s (%s %.2f)\n", job.channel, job.msg_id.c_str(),
                ident_attributed(rec) ? NAVTEX_STATIONS[rec.r.station].name : "unidentified",
                rec.level.c_str(), rec.r.confidence);
    }

    /* Header decoded, message still arriving: answer from a COPY of the
     * learned state.  identify() is not promised to be side-effect free, and a
     * provisional query (no duration, partial SNR) must not leave a mark on
     * the real identifier.
     *
     * Skipped while the replay runs (the learned state is incomplete, and the
     * final answer is queued behind the replay anyway).  The copy is timed;
     * above IDENT_PROVISIONAL_MAX_COPY_US provisional answers are switched off
     * for good and the final answer is all the page gets — with the stub
     * identifier the copy is a few hundred nanoseconds, the real one's cost
     * is reported on /api/stations as replay.provisional_copy_us. */
    void handle_provisional(IdentJob &job)
    {
        if (!replay_done() || !provisional_ok_) return;

        auto a = std::chrono::steady_clock::now();
        std::unique_ptr<NavtexIdentifier> copy(
            ident_clone(*ident_, std::is_copy_constructible<NavtexIdentifier>{}));
        double us = std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - a).count();
        copy_us_ = (long)us;
        if (!copy) {
            provisional_ok_ = false;
            fprintf(stderr, "[ident] identifier is not copyable — provisional answers disabled\n");
            return;
        }
        if (us > IDENT_PROVISIONAL_MAX_COPY_US) {
            provisional_ok_ = false;
            fprintf(stderr, "[ident] copying the identifier took %.0f us — provisional answers "
                            "disabled\n", us);
            return;
        }

        NavtexIdentQuery q = job.q;
        apply_rx(q, rx_snapshot());
        if (!ident_query_clean(q)) return;   /* the final will say why */
        IdentRecord rec = ident_record(q, copy->identify(q));
        std::string frame = ident_json(rec, true, ws_extra(job));
        remember_frame(job.channel, frame, job.msg_id, true);
        if (hub_) hub_->send_ident(job.channel, frame);
    }

    std::string   log_dir_;
    BroadcastHub *hub_ = nullptr;
    std::atomic<bool> started_{false};   /* read by the location thread */
    std::thread   thread_;

    std::unique_ptr<NavtexIdentifier> ident_ { new NavtexIdentifier() };   /* worker thread only */
    std::deque<bool> suspect_win_;      /* worker thread only */
    int              suspect_n_ = 0;

    std::mutex              mu_;        /* guards q_ and reident_ */
    std::condition_variable cv_;
    std::deque<IdentJob>    q_;
    bool                    reident_ = false;
    std::atomic<bool>       deferred_{false};   /* last replay skipped its writes */

    std::mutex               frames_mu_;
    std::map<int, LastFrame> frames_;

    std::atomic<int>  phase_{0};
    std::atomic<int>  processed_{0}, total_{0};
    std::atomic<long> elapsed_ms_{0};
    std::atomic<bool> provisional_ok_{true};
    std::atomic<long> copy_us_{-1};
};

/* Deliberately leaked: the worker thread is detached and runs until the
 * process exits, and destroying the condition variable it waits on during
 * static destruction would block exit (glibc's pthread_cond_destroy waits for
 * its waiters). */
static IdentWorker &g_ident = *new IdentWorker;

static void ident_request_reidentify() { g_ident.request_reidentify(); }
static void ident_flush_deferred()     { g_ident.flush_deferred(); }

static void ident_submit(IdentJob &&job)
{
    /* While history is being (re-)identified the worker cannot reach this
     * job for up to a minute on a small box, and MQTT is published from the
     * worker so it can carry the identification.  Rather than hold every
     * Home Assistant update back for that long, a message completed during a
     * replay is published at once, without the ident; the browser and the
     * sidecar still get it when the worker catches up. */
    if (job.kind == IdentJobKind::Final && !g_ident.replay_done()) {
        g_mqtt.publish_message(job.mqtt);
        job.mqtt_sent = true;
    }
    if (g_ident.try_push(job)) return;
    if (job.kind == IdentJobKind::Final) {
        /* Never lose the message itself: publish it bare. */
        fprintf(stderr, "[ident] queue full — publishing %s without identification\n",
                job.msg_id.c_str());
        if (!job.mqtt_sent) g_mqtt.publish_message(job.mqtt);
    }
}

/* ---------------- /api/stations ---------------- */

/* The "receiver" object: where we are and where that came from.  Shared by
 * /api/stations and /api/analytics so the two can never describe the
 * receiver differently. */
static std::string receiver_json(const RxLocation &rx)
{
    std::string j = "{";
    j += "\"valid\":" + std::string(rx.valid ? "true" : "false");
    j += ",\"lat\":" + (rx.valid ? mqtt_fmt_double(rx.lat, 4) : std::string("null"));
    j += ",\"lon\":" + (rx.valid ? mqtt_fmt_double(rx.lon, 4) : std::string("null"));
    j += ",\"source\":" + jstr(rx.valid ? rx.source : "none");
    j += ",\"locator\":"  + (rx.locator.empty()  ? std::string("null") : jstr(rx.locator));
    j += ",\"location\":" + (rx.location.empty() ? std::string("null") : jstr(rx.location));
    j += ",\"callsign\":" + (rx.callsign.empty() ? std::string("null") : jstr(rx.callsign));
    j += ",\"pos_suspect\":" + std::string(g_rx_pos_suspect.load() ? "true" : "false");
    j += "}";
    return j;
}

static std::string stations_json()
{
    const RxLocation rx = rx_snapshot();

    /* Per-station "heard" aggregates, from the index.  Done per request: a
     * pass over 30,000 entries is a couple of milliseconds, and it can never
     * drift from what /api/history says. */
    struct Heard { int count = 0, high = 0, medium = 0; int64_t last = 0; bool has_snr = false;
                   double best_snr = 0, sum_conf = 0; };
    std::vector<Heard> heard(NAVTEX_STATION_COUNT);
    {
        std::shared_lock<std::shared_mutex> lk(g_index_mu);
        for (const auto &kv : g_index) {
            const HistoryIndexEntry &e = kv.second;
            if (!e.has_ident || !ident_attributed(e.ident)) continue;
            Heard &h = heard[e.ident.r.station];
            h.count++;
            if (e.ident.level == "high") h.high++;
            else if (e.ident.level == "medium") h.medium++;
            if (e.q.start_utc > h.last) h.last = e.q.start_utc;
            if (e.q.has_snr && (!h.has_snr || e.q.snr_db > h.best_snr)) {
                h.best_snr = e.q.snr_db;
                h.has_snr  = true;
            }
            h.sum_conf += e.ident.r.confidence;
        }
    }

    std::string j = "{\"receiver\":" + receiver_json(rx);
    j += ",\"replay\":" + g_ident.status_json();
    j += ",\"ident_version\":" + std::to_string(NAVTEX_IDENT_VERSION);
    j += ",\"table\":" + jstr(ident_table_fp());
    j += ",\"stations\":[";
    for (int i = 0; i < NAVTEX_STATION_COUNT; i++) {
        const NavtexStation &s = NAVTEX_STATIONS[i];
        const NavtexCountry *c = navtex_country(s.country);
        if (i) j += ",";
        j += "{\"key\":"          + jstr(navtex_station_key(i));
        j += ",\"freq_khz\":"     + navtex_freq_khz_str(s.freq_hz);
        j += ",\"letter\":"       + jstr(std::string(1, s.id));
        j += ",\"name\":"         + jstr(s.name);
        j += ",\"country\":"      + jstr(s.country);
        j += ",\"country_name\":" + jstr(navtex_station_country_name(i));
        j += ",\"iso2\":"         + (c ? jstr(c->iso2) : std::string("null"));
        j += ",\"navarea\":"      + std::to_string(s.navarea);
        j += ",\"lat\":"          + mqtt_fmt_double(s.lat, 4);
        j += ",\"lon\":"          + mqtt_fmt_double(s.lon, 4);
        j += ",\"range_nm\":"     + (s.range_nm > 0 ? std::to_string(s.range_nm) : std::string("null"));
        j += ",\"active\":"       + std::string(s.active > 0 ? "true" : s.active == 0 ? "false" : "null");
        if (rx.valid) {
            j += ",\"dist_km\":" + mqtt_fmt_double(navtex_great_circle_km(rx.lat, rx.lon, s.lat, s.lon), 1);
            j += ",\"bearing\":" + mqtt_fmt_double(navtex_initial_bearing_deg(rx.lat, rx.lon, s.lat, s.lon), 1);
        } else {
            j += ",\"dist_km\":null,\"bearing\":null";
        }
        j += ",\"schedule\":[";
        for (int k = 0; k < s.n_minutes && k < 12; k++) {
            if (k) j += ",";
            j += std::to_string(s.minutes[k]);
        }
        j += "]";
        const Heard &h = heard[i];
        if (h.count > 0) {
            time_t lt = (time_t)h.last;
            struct tm u {};
            gmtime_r(&lt, &u);
            char ts[80];
            snprintf(ts, sizeof(ts), "%04d-%02d-%02dT%02d:%02d:%02dZ", u.tm_year + 1900,
                     u.tm_mon + 1, u.tm_mday, u.tm_hour, u.tm_min, u.tm_sec);
            j += ",\"heard\":{\"count\":" + std::to_string(h.count);
            j += ",\"high\":" + std::to_string(h.high);
            /* So the page can tell a station really heard (Medium+) from
             * one only ever guessed at (Low), and frame its map on the
             * former — see renderStationsMap(). */
            j += ",\"medium\":" + std::to_string(h.medium);
            j += ",\"last_utc\":" + jstr(ts);
            j += ",\"best_snr\":" + (h.has_snr ? mqtt_fmt_double(h.best_snr, 2) : std::string("null"));
            j += ",\"mean_conf\":" + mqtt_fmt_double(h.sum_conf / h.count, 3);
            j += "}";
        } else {
            j += ",\"heard\":null";
        }
        j += "}";
    }
    j += "]}";
    return j;
}

/* ---------------- /api/analytics ---------------- */
/*
 * Every identified message, as columns, for the Metrics view to aggregate in
 * the browser.  The page draws a dozen charts over the same rows (coverage by
 * bearing, distance by hour, a station x hour heatmap, a configurable
 * breakdown, daily trends) and re-filters them on every control change, so
 * it wants the raw rows once rather than an endpoint per chart.
 *
 * WHY COLUMNS
 *
 *   An array of objects repeats every field name on every row: for 32,000
 *   messages that is most of the payload.  Columns name each field once:
 *
 *     "cols":{"t":[1729000000,...],"f":[518,...],"s":[3,...],...}
 *
 *   Row i is t[i], f[i], s[i] ... — every column has exactly "n" entries, in
 *   start-time order, oldest first.  The station is an index into the
 *   "stations" dictionary (only the stations actually referenced), or -1 for
 *   a message that was identified as "not attributable".  Messages still
 *   waiting for the identifier are left out and counted in "pending".
 *
 *   Columns, beyond the contract's list:  "b1" (the transmitted letter, so an
 *   unattributed row still says what was heard).
 *
 *   lvl is 0 none, 1 low, 2 medium, 3 high — numbers, so "Medium+" is lvl>=2.
 *   dist/brg are null when the station or the receiver position is unknown;
 *   snr and fail are null when the sidecar has no such measurement.
 *
 * WHY NOT GZIP
 *
 *   IXWebSocket's HttpServer only compresses in its own static-file handler
 *   (and only when built with IXWEBSOCKET_USE_ZLIB, which this build is not);
 *   a response from an OnConnectionCallback, like this one, goes out as
 *   given.  So it is kept small instead: no whitespace, every number rounded
 *   to what a chart can show (conf 3 dp, dist/brg/snr/fail 1 dp, freq to the
 *   0.5 kHz) with trailing zeros dropped.  Measured on 32,651 real messages:
 *   1.85 MB, about a sixth of /api/history's 11.7 MB (the start times alone
 *   are a fifth of it).  A reverse proxy in front that gzips
 *   application/json takes it to about 270 kB.
 *
 * COST
 *
 *   Built per request from the in-memory index — no file is read.  The index
 *   lock is held (shared) only while the rows are copied out; sorting and
 *   formatting happen after it is released.  Not
 *   cached: "generated_utc" and the receiver block are live, the from/to
 *   window makes most requests different anyway, and a build is cheap:
 *   about 20 ms for all 32,651 rows (Release build), of which the shared
 *   index lock is held for about 5 ms.
 *
 * QUERY
 *
 *   ?from=<unix>&to=<unix> — both optional, both inclusive, on the message's
 *   start time.  Anything else in them is a 400 rather than silently "all".
 */

/* One row, copied out of the index under its lock. */
struct AnalyticsRow {
    int64_t t;
    long    freq_hz;
    int     station;          /* NAVTEX_STATIONS index, -1 unattributed */
    char    b1, b2;
    double  conf;
    int     lvl;              /* 0 none .. 3 high */
    double  dist, brg;        /* -1 unknown */
    bool    night;
    bool    has_snr;  double snr;
    int     dur;
    bool    has_fail; double fail;
};

static int analytics_level_num(const std::string &level)
{
    if (level == "high")   return 3;
    if (level == "medium") return 2;
    if (level == "low")    return 1;
    return 0;
}

/* Append v rounded to dp (0..4) decimal places, trailing zeros dropped:
 * 0.97 not 0.970, 518 not 518.0.  Integer arithmetic after one rounding, so
 * it is both exact and several times quicker than snprintf("%.*f") — which
 * matters at a dozen numbers per row for 30,000 rows. */
static void analytics_num(std::string &out, double v, int dp)
{
    if (!std::isfinite(v)) { out += "null"; return; }
    static const long long scale[] = { 1, 10, 100, 1000, 10000 };
    if (dp < 0) dp = 0;
    if (dp > 4) dp = 4;
    long long n = llround(v * (double)scale[dp]);
    if (n < 0) { out += '-'; n = -n; }          /* after rounding: never "-0" */
    long long ip = n / scale[dp], fp = n % scale[dp];
    out += std::to_string(ip);
    if (fp == 0) return;
    char b[8];
    int len = dp;
    for (int i = dp - 1; i >= 0; i--) { b[i] = (char)('0' + fp % 10); fp /= 10; }
    while (len > 0 && b[len - 1] == '0') len--;
    out += '.';
    out.append(b, (size_t)len);
}

/* A letter column value: "E", or null for anything that is not A-Z. */
static void analytics_letter(std::string &out, char c)
{
    if (c >= 'A' && c <= 'Z') { out += '"'; out += c; out += '"'; }
    else out += "null";
}

/* Value of one key in a query string ("from=1&to=2"), exact key match only —
 * so "to" never matches inside some other key.  False if absent. */
static bool query_param(const std::string &qs, const std::string &key, std::string &val)
{
    size_t p = 0;
    while (p <= qs.size()) {
        size_t amp = qs.find('&', p);
        if (amp == std::string::npos) amp = qs.size();
        size_t eq = qs.find('=', p);
        if (eq != std::string::npos && eq < amp && qs.compare(p, eq - p, key) == 0 &&
            eq - p == key.size()) {
            val = url_decode(qs.substr(eq + 1, amp - eq - 1));
            return true;
        }
        p = amp + 1;
    }
    return false;
}

/* Parse an optional unix-time parameter.  Absent: true, `set` false.
 * Present but not a whole number: false (a 400). */
static bool analytics_time_param(const std::string &qs, const char *key, bool &set, int64_t &v)
{
    std::string s;
    set = false;
    if (!query_param(qs, key, s)) return true;
    if (s.empty()) return true;                  /* "from=" — treat as absent */
    char *end = nullptr;
    errno = 0;
    long long x = strtoll(s.c_str(), &end, 10);
    if (errno != 0 || !end || *end != '\0') return false;
    set = true;
    v   = (int64_t)x;
    return true;
}

static std::string analytics_json(bool has_from, int64_t from, bool has_to, int64_t to)
{
    const RxLocation rx = rx_snapshot();

    /* Copy the rows out, then let go of the index: the worker may be waiting
     * to write an identification into it. */
    std::vector<AnalyticsRow> rows;
    size_t pending = 0;
    {
        std::shared_lock<std::shared_mutex> lk(g_index_mu);
        rows.reserve(g_index.size());
        for (const auto &kv : g_index) {
            const HistoryIndexEntry &e = kv.second;
            const int64_t t = e.q.start_utc;
            if ((has_from && t < from) || (has_to && t > to)) continue;
            if (!e.has_ident) { pending++; continue; }

            const IdentRecord &rec = e.ident;
            const bool have = ident_attributed(rec);
            AnalyticsRow r;
            r.t        = t;
            r.freq_hz  = e.q.freq_hz;
            r.station  = have ? rec.r.station : -1;
            r.b1       = e.q.b1;
            r.b2       = e.q.b2;
            r.conf     = have ? rec.r.confidence : 0.0;
            r.lvl      = have ? analytics_level_num(rec.level) : 0;
            ident_geometry(rec, r.dist, r.brg);
            r.night    = have && rec.r.night_path;
            r.has_snr  = e.q.has_snr;
            r.snr      = e.q.snr_db;
            r.dur      = e.q.duration_s;
            r.has_fail = e.q.has_chars;
            r.fail     = e.q.chars_failed_pct;
            rows.push_back(r);
        }
    }

    /* Oldest first, the order trends and the replay think in.  Ties (two
     * channels finishing in the same second) broken on frequency, so the
     * order is the same from one request to the next. */
    std::sort(rows.begin(), rows.end(), [](const AnalyticsRow &a, const AnalyticsRow &b) {
        if (a.t != b.t) return a.t < b.t;
        if (a.freq_hz != b.freq_hz) return a.freq_hz < b.freq_hz;
        return a.station < b.station;
    });

    /* The station dictionary: only stations these rows mention, in table
     * order, so the indices are stable for a given set of rows. */
    std::vector<int> dict_of(NAVTEX_STATION_COUNT, -1);
    for (const auto &r : rows) if (r.station >= 0) dict_of[r.station] = 0;
    std::vector<int> dict;
    for (int i = 0; i < NAVTEX_STATION_COUNT; i++)
        if (dict_of[i] == 0) { dict_of[i] = (int)dict.size(); dict.push_back(i); }

    time_t now = time(nullptr);
    struct tm u {};
    gmtime_r(&now, &u);
    char ts[40];
    snprintf(ts, sizeof(ts), "%04d-%02d-%02dT%02d:%02d:%02dZ", u.tm_year + 1900,
             u.tm_mon + 1, u.tm_mday, u.tm_hour, u.tm_min, u.tm_sec);

    std::string j;
    j.reserve(256 + rows.size() * 64);
    j += "{\"generated_utc\":";
    j += jstr(ts);
    j += ",\"from\":" + (has_from ? std::to_string(from) : std::string("null"));
    j += ",\"to\":"   + (has_to   ? std::to_string(to)   : std::string("null"));
    j += ",\"receiver\":" + receiver_json(rx);
    j += ",\"replay\":" + g_ident.status_json();
    j += ",\"ident_version\":" + std::to_string(NAVTEX_IDENT_VERSION);
    j += ",\"table\":" + jstr(ident_table_fp());
    j += ",\"n\":" + std::to_string(rows.size());
    j += ",\"pending\":" + std::to_string(pending);

    j += ",\"stations\":[";
    for (size_t k = 0; k < dict.size(); k++) {
        const int i = dict[k];
        const NavtexStation &s = NAVTEX_STATIONS[i];
        const NavtexCountry *c = navtex_country(s.country);
        if (k) j += ",";
        j += "{\"key\":"          + jstr(navtex_station_key(i));
        j += ",\"name\":"         + jstr(s.name);
        j += ",\"country\":"      + jstr(s.country);
        j += ",\"country_name\":" + jstr(navtex_station_country_name(i));
        j += ",\"iso2\":"         + (c ? jstr(c->iso2) : std::string("null"));
        j += ",\"navarea\":"      + std::to_string(s.navarea);
        j += ",\"freq_khz\":"     + navtex_freq_khz_str(s.freq_hz);
        j += ",\"letter\":"       + jstr(std::string(1, s.id));
        j += ",\"lat\":"          + mqtt_fmt_double(s.lat, 4);
        j += ",\"lon\":"          + mqtt_fmt_double(s.lon, 4);
        j += "}";
    }
    j += "]";

    /* One column at a time: column() writes the name and the brackets, `cell`
     * appends one row's value. */
    auto column = [&](const char *name, bool first,
                      const std::function<void(const AnalyticsRow &)> &cell) {
        j += first ? "\"" : ",\"";
        j += name;
        j += "\":[";
        for (size_t i = 0; i < rows.size(); i++) {
            if (i) j += ',';
            cell(rows[i]);
        }
        j += ']';
    };

    j += ",\"cols\":{";
    column("t",     true,  [&](const AnalyticsRow &r) { j += std::to_string(r.t); });
    column("f",     false, [&](const AnalyticsRow &r) { analytics_num(j, r.freq_hz / 1000.0, 1); });
    column("s",     false, [&](const AnalyticsRow &r) {
        j += std::to_string(r.station >= 0 ? dict_of[r.station] : -1); });
    column("b1",    false, [&](const AnalyticsRow &r) { analytics_letter(j, r.b1); });
    column("b2",    false, [&](const AnalyticsRow &r) { analytics_letter(j, r.b2); });
    column("conf",  false, [&](const AnalyticsRow &r) { analytics_num(j, r.conf, 3); });
    column("lvl",   false, [&](const AnalyticsRow &r) { j += (char)('0' + r.lvl); });
    column("dist",  false, [&](const AnalyticsRow &r) {
        if (r.dist >= 0) analytics_num(j, r.dist, 1); else j += "null"; });
    column("brg",   false, [&](const AnalyticsRow &r) {
        if (r.brg >= 0) analytics_num(j, r.brg, 1); else j += "null"; });
    column("night", false, [&](const AnalyticsRow &r) { j += r.night ? '1' : '0'; });
    column("snr",   false, [&](const AnalyticsRow &r) {
        if (r.has_snr) analytics_num(j, r.snr, 1); else j += "null"; });
    column("dur",   false, [&](const AnalyticsRow &r) { j += std::to_string(r.dur); });
    column("fail",  false, [&](const AnalyticsRow &r) {
        if (r.has_fail) analytics_num(j, r.fail, 1); else j += "null"; });
    j += "}}";
    return j;
}

/* GET /api/ident?rel= — the index's answer; failing that (during the first
 * seconds of startup, before the replay has reached the file) whatever the
 * sidecar holds from the previous run. */
static int ident_lookup_json(const std::string &log_dir, const std::string &rel, std::string &out)
{
    {
        std::shared_lock<std::shared_mutex> lk(g_index_mu);
        auto e = g_index.find(rel);
        if (e != g_index.end() && e->second.has_ident) {
            IdentRecord rec = e->second.ident;
            lk.unlock();
            out = ident_json(rec, false);
            return 200;
        }
    }
    std::string sc = history_read_metrics(log_dir, rel);
    if (sc.empty()) { out = "{\"error\":\"not found\"}"; return 404; }
    std::string stored = sidecar_ident_member(sc);
    if (!stored.empty()) { out = stored; return 200; }
    out = "{\"error\":\"not identified yet\",\"pending\":true}";
    return 404;
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
        /* sort_key is the frequency-independent date+time portion of the path,
         * e.g. "2025/04/08/143022Z" for a message file or "2025-04-08" for a
         * raw log.  Sorting on this key (descending) gives true chronological
         * order across all frequencies. */
        std::string sort_key;
        bool is_raw; /* true = .log raw file, false = .txt message file */
    };
    std::vector<FileEntry> files;

    /* Index entries this walk found a file for.  Anything else in the index
     * was deleted behind the program's back (by hand, or another tool) and
     * is dropped below, so Metrics, Stations and /api/ident agree with the
     * list rather than with what was on disk at startup. */
    std::unordered_set<std::string> seen;

    /* Also collect the set of .json sidecar paths for quick lookup */
    std::unordered_set<std::string> json_sidecars;

    /* Helper: given a relative path (freq stripped) derive a sort key.
     * msg:  "2025/04/08/143022Z_EA42.txt" -> "2025/04/08/143022Z"
     * raw:  "raw/2025-04-08.log"          -> "2025-04-08"           */
    auto make_sort_key = [&](const std::string &rel_no_freq, bool raw) -> std::string {
        if (raw) {
            /* rel_no_freq: "raw/2025-04-08.log" — date is the stem of the filename */
            size_t slash = rel_no_freq.rfind('/');
            std::string fname = (slash == std::string::npos) ? rel_no_freq
                                                              : rel_no_freq.substr(slash + 1);
            if (fname.size() > 4) fname = fname.substr(0, fname.size() - 4); /* strip .log */
            return fname; /* "2025-04-08" */
        } else {
            /* rel_no_freq: "2025/04/08/143022Z_EA42.txt"
             * Keep everything up to (but not including) the first '_' in the filename */
            size_t last_slash = rel_no_freq.rfind('/');
            std::string dir_part  = (last_slash == std::string::npos) ? ""
                                    : rel_no_freq.substr(0, last_slash); /* "2025/04/08" */
            std::string file_part = (last_slash == std::string::npos) ? rel_no_freq
                                    : rel_no_freq.substr(last_slash + 1);
            size_t us = file_part.find('_');
            std::string time_stem = (us == std::string::npos) ? file_part
                                                               : file_part.substr(0, us);
            /* strip .txt if no underscore */
            if (time_stem.size() > 4 && time_stem.substr(time_stem.size()-4) == ".txt")
                time_stem = time_stem.substr(0, time_stem.size()-4);
            return dir_part.empty() ? time_stem : dir_part + "/" + time_stem;
        }
    };

    /* d_type walk: no stat() per file (see log_walk). */
    log_walk(log_dir, [&](const std::string &full, const char *cname) {
        std::string name = cname;
        /* Derive the relative path (strip log_dir prefix and leading '/') */
        std::string rel_full = full;
        if (rel_full.substr(0, log_dir.size()) == log_dir)
            rel_full = rel_full.substr(log_dir.size());
        if (!rel_full.empty() && rel_full[0] == '/') rel_full = rel_full.substr(1);
        /* Strip the leading frequency component (everything up to first '/') */
        size_t first_slash = rel_full.find('/');
        std::string rel_no_freq = (first_slash == std::string::npos)
                                  ? rel_full : rel_full.substr(first_slash + 1);

        if (name.size() > 4 && name.substr(name.size()-4) == ".txt")
            files.push_back({full, make_sort_key(rel_no_freq, false), false});
        else if (name.size() > 4 && name.substr(name.size()-4) == ".log")
            files.push_back({full, make_sort_key(rel_no_freq, true), true});
        else if (name.size() > 5 && name.substr(name.size()-5) == ".json")
            json_sidecars.insert(full);
    });

    /* Sort newest-first by the frequency-independent date+time sort key.
     * This ensures messages from different frequencies are interleaved in true
     * chronological order rather than grouped by frequency directory name. */
    std::sort(files.begin(), files.end(),
              [](const FileEntry &a, const FileEntry &b){ return a.sort_key > b.sort_key; });

    /* Build JSON array */
    std::string json = "[";
    json.reserve(files.size() * 360);   /* ~ the size of an identified entry */
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
         * inline display in the history list.
         *
         * The in-memory index (see "Station identification") already holds
         * them for every message the ident worker has seen, so a sidecar is
         * only read here for a file it has not reached yet: during the first
         * seconds of startup, or a message completed a moment ago. */
        bool has_metrics = false;
        std::string json_sidecar_path;
        std::string inline_snr;      /* e.g. "11.6" or "" */
        std::string inline_duration; /* e.g. "132"  or "" */
        bool        snr_rescaled = false; /* value converted from the old scale */
        bool        indexed      = false;
        bool        broken       = false; /* sidecar present but not one we wrote */
        int64_t     start_utc    = 0;     /* from the index: when it STARTED */
        std::string st_json;         /* the "st" summary, "" = not identified yet */
        if (!fe.is_raw) {
            json_sidecar_path = fe.full.substr(0, fe.full.size() - 4) + ".json";
            has_metrics = (json_sidecars.count(json_sidecar_path) > 0);

            {
                std::shared_lock<std::shared_mutex> lk(g_index_mu);
                auto it = g_index.find(rel);
                if (it != g_index.end()) {
                    indexed         = true;
                    seen.insert(rel);
                    inline_snr      = it->second.snr;
                    inline_duration = it->second.duration;
                    snr_rescaled    = it->second.snr_rescaled;
                    start_utc       = it->second.q.start_utc;
                    if (it->second.has_ident || it->second.skipped) st_json = it->second.st_json;
                }
            }

            if (has_metrics && !indexed) {
                /* Read sidecar and extract avg_snr_db and duration_s with
                 * simple substring searches — no full JSON parser needed. */
                FILE *sf = fopen(json_sidecar_path.c_str(), "r");
                if (sf) {
                    std::string sc;
                    char sbuf[4096]; size_t sn;
                    while ((sn = fread(sbuf, 1, sizeof(sbuf), sf)) > 0)
                        sc.append(sbuf, sn);
                    fclose(sf);

                    /* A record written before audio protocol version 4 is on
                     * the old noise-density scale and reads about 34 dB high;
                     * normalise it so one column is one scale.  See
                     * navtex_snr.h. */
                    snr_rescaled = sidecar_is_legacy_scale(sc);
                    sc = sidecar_normalise_scale(sc);

                    inline_snr      = sidecar_field(sc, "avg_snr_db");
                    inline_duration = sidecar_field(sc, "duration_s");
                    /* sidecar_load() will refuse it, so it will never be
                     * identified: do not promise that it will be. */
                    HistoryIndexEntry probe;
                    broken = !sidecar_load(json_sidecar_path, rel, probe);
                } else {
                    broken = true;
                }
            }
        }

        if (!first) json += ",";
        first = false;
        json += "{";
        json += "\"type\":\""  + std::string(fe.is_raw ? "raw" : "msg") + "\"";
        json += ",\"rel\":\""  + json_escape(rel)  + "\"";
        json += ",\"freq\":\"" + json_escape(freq) + "\"";
        json += ",\"date\":\"" + json_escape(year + "-" + month + "-" + day) + "\"";
        if (!fe.is_raw) {
            json += ",\"time\":\""      + json_escape(time_part)   + "\"";
            json += ",\"id\":\""        + json_escape(id_part)     + "\"";
            json += ",\"serial\":\""    + json_escape(serial_part) + "\"";
            json += ",\"has_metrics\":" + std::string(has_metrics ? "true" : "false");
            /* The file name is the END time; the facts talk about when the
             * message started, so the page shows this one when it has it. */
            if (ident_time_plausible(start_utc)) {
                char hms[16];
                time_t t = (time_t)start_utc;
                struct tm tmv {};
                gmtime_r(&t, &tmv);
                snprintf(hms, sizeof(hms), "%02d%02d%02dZ", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
                json += ",\"start\":\"" + std::string(hms) + "\"";
            }
            if (has_metrics) {
                /* Validate extracted values are real JSON numbers before embedding.
                 * Guards against "-nan", "inf", etc. in existing sidecar files. */
                if (sidecar_is_number(inline_snr)) {
                    json += ",\"snr\":" + inline_snr;
                    if (snr_rescaled)
                        json += ",\"snr_rescaled\":true";
                }
                if (sidecar_is_number(inline_duration))
                    json += ",\"duration_s\":" + inline_duration;
            }
            /* Station identification.  st_pending: a sidecar exists, so the
             * ident worker will get to it — the page shows "identifying…"
             * and refreshes when the replay finishes. */
            if (!st_json.empty())             json += ",\"st\":" + st_json;
            else if (has_metrics && !broken)  json += ",\"st\":null,\"st_pending\":true";
            else                              json += ",\"st\":null";
        }
        json += "}";
    }
    json += "]";

    /* Reconcile the index with the disk.  An entry the walk did not see is
     * stat()ed before it is dropped: a message saved and indexed while the
     * walk was running is on disk, just not in this listing yet. */
    std::vector<std::string> gone;
    {
        std::shared_lock<std::shared_mutex> lk(g_index_mu);
        for (const auto &kv : g_index)
            if (!seen.count(kv.first)) gone.push_back(kv.first);
    }
    size_t dropped = 0;
    for (const auto &rel : gone) {
        struct stat st {};
        if (lstat((log_dir + "/" + rel).c_str(), &st) == 0) continue;
        index_forget(rel);
        dropped++;
    }
    if (dropped)
        fprintf(stderr, "[ident] %zu message(s) no longer on disk dropped from the index\n",
                dropped);
    return json;
}

static void history_gen_bump() { g_history_gen++; }

/* /api/history, cached.
 *
 * Building the list walks the whole log directory and emits ~12 MB of JSON for
 * a few months of logs; doing that for every page load is most of a CPU-second
 * on a small box.  The answer only changes when g_history_gen moves (a message
 * saved, identified or purged), so the last body is reused until it does.
 *
 * The walk is still what decides what is listed — raw logs, files with no
 * sidecar, and anything the index has not reached are all found by it — so a
 * cached body is also bounded in age: after HISTORY_CACHE_MAX_AGE_S it is
 * rebuilt regardless, which picks up files added or removed behind this
 * program's back. */
static constexpr int HISTORY_CACHE_MAX_AGE_S = 30;

/* gzip (RFC 1952) of s, or "" on failure.  IXWebSocket's HttpServer only
 * compresses the files its default handler serves, not the bodies a callback
 * returns, so the large JSON endpoints do it themselves.  /api/history is
 * ~11.5 MB of very repetitive JSON for a few months of logs and compresses
 * about 10:1, which matters behind UberSDR's add-on proxy on a mobile link. */
static std::string gzip_compress(const std::string &s, int level = 6)
{
    z_stream zs {};
    if (deflateInit2(&zs, level, Z_DEFLATED, 15 + 16 /* gzip wrapper */, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK)
        return "";
    std::string out;
    out.resize(deflateBound(&zs, (uLong)s.size()));
    zs.next_in   = (Bytef *)s.data();
    zs.avail_in  = (uInt)s.size();
    zs.next_out  = (Bytef *)&out[0];
    zs.avail_out = (uInt)out.size();
    int rc = deflate(&zs, Z_FINISH);
    out.resize(zs.total_out);
    deflateEnd(&zs);
    return rc == Z_STREAM_END ? out : "";
}

/* Does the client take a gzip body?  A plain substring test, as IXWebSocket's
 * own file handler does: "gzip, deflate, br" and the like. */
static bool accepts_gzip(const ix::HttpRequestPtr &req)
{
    for (const auto &h : req->headers) {
        if (strcasecmp(h.first.c_str(), "accept-encoding") != 0) continue;
        return h.second.find("gzip") != std::string::npos || h.second == "*";
    }
    return false;
}

/* Set resp's body to `body`, gzipped if the client accepts it and it is
 * worth it.  Vary so a cache keeps the two apart. */
static void set_body_maybe_gzip(ix::HttpResponsePtr &resp, const ix::HttpRequestPtr &req,
                                std::string &&body)
{
    resp->headers["Vary"] = "Accept-Encoding";
    if (body.size() >= 1024 && accepts_gzip(req)) {
        std::string gz = gzip_compress(body, 4);
        if (!gz.empty()) {
            resp->headers["Content-Encoding"] = "gzip";
            resp->body = std::move(gz);
            return;
        }
    }
    resp->body = std::move(body);
}

/* The cached body, plain or gzipped.  The gzip copy is made once per build,
 * on the first request that wants it, and shared by every request after —
 * so 64 concurrent page loads cost 64 copies of ~1 MB, not of ~11 MB (review:
 * RSS went from 99 to 732 MB on uncompressed copies). */
static std::string history_list_json_cached(const std::string &log_dir, bool gzip = false)
{
    static std::mutex                            mu;
    static uint64_t                              gen = 0;
    static std::chrono::steady_clock::time_point built;
    static std::shared_ptr<const std::string>    body, body_gz;

    const uint64_t now_gen = g_history_gen.load();
    const auto     now     = std::chrono::steady_clock::now();
    std::shared_ptr<const std::string> have;
    {
        std::lock_guard<std::mutex> lk(mu);
        if (body && gen == now_gen && now - built < std::chrono::seconds(HISTORY_CACHE_MAX_AGE_S)) {
            if (!gzip)   return *body;
            if (body_gz) return *body_gz;
            have = body;
        }
    }
    if (!have) {
        /* Built outside the lock: two requests racing a change both build,
         * which is cheaper than making the second wait. */
        have = std::make_shared<const std::string>(history_list_json(log_dir));
        std::lock_guard<std::mutex> lk(mu);
        gen     = now_gen;
        built   = now;
        body    = have;
        body_gz = nullptr;
    }
    if (!gzip) return *have;
    auto gz = std::make_shared<const std::string>(gzip_compress(*have, 4));
    if (gz->empty()) return "";   /* caller falls back to plain */
    std::lock_guard<std::mutex> lk(mu);
    if (body == have) body_gz = gz;
    return *gz;
}

/* Read a file and return its contents, or empty string on error.
 * Validates that the resolved path starts with log_dir to prevent traversal. */
/* ------------------------------------------------------------------ */
/* Log retention / cleanup                                              */
/* ------------------------------------------------------------------ */
/* Delete messages and raw logs older than retain_days days, and the     */
/* empty directories they leave.  Runs once at startup, then hourly.    */
/*
 * What decides a message's age is its .txt — the message itself, written
 * once and never touched again.  Its .json sidecar goes WITH it, whatever the
 * sidecar's own date: the ident worker rewrites sidecars (a new algorithm
 * version, station table or receiver position rewrites all of them), and
 * judging each file by its own date used to delete the message and keep the
 * sidecar — 13,674 orphans after one restart in review, in date folders that
 * were then never empty enough to be removed.  sidecar_write_ident() now also
 * keeps the original date, so either rule alone would do; both are kept.
 *
 * A .json with no .txt (an orphan from before this fix) goes by its own date.
 * A *.json.tmp is a sidecar rewrite interrupted by a crash; one more than ten
 * minutes old is garbage.
 *
 * lstat(), not stat(), and links are never followed or deleted through: a
 * symlink in the log directory must not let retention delete files elsewhere.
 */
static void purge_old_logs(const std::string &log_dir, int retain_days)
{
    if (log_dir.empty() || retain_days <= 0) return;

    auto now = std::chrono::system_clock::now();
    std::time_t cutoff = std::chrono::system_clock::to_time_t(
        now - std::chrono::hours(24 * retain_days));
    std::time_t tmp_cutoff = std::chrono::system_clock::to_time_t(now - std::chrono::minutes(10));

    struct Found { std::string full; time_t mtime; };
    std::vector<Found> txts, logs, jsons, tmps;
    std::function<void(const std::string &)> walk = [&](const std::string &dir) {
        DIR *d = opendir(dir.c_str());
        if (!d) return;
        struct dirent *ent;
        std::vector<std::string> subdirs;
        while ((ent = readdir(d)) != nullptr) {
            if (ent->d_name[0] == '.') continue;
            std::string full = dir + "/" + ent->d_name;
            struct stat st {};
            if (lstat(full.c_str(), &st) != 0) continue;
            if (S_ISDIR(st.st_mode)) { subdirs.push_back(full); continue; }
            if (!S_ISREG(st.st_mode)) continue;
            const char *name = ent->d_name;
            if      (ends_with(name, ".txt"))      txts.push_back({ full, st.st_mtime });
            else if (ends_with(name, ".log"))      logs.push_back({ full, st.st_mtime });
            else if (ends_with(name, ".json"))     jsons.push_back({ full, st.st_mtime });
            else if (ends_with(name, ".json.tmp")) tmps.push_back({ full, st.st_mtime });
        }
        closedir(d);
        for (const auto &sub : subdirs) walk(sub);
    };
    walk(log_dir);

    int removed = 0;
    auto rm = [&](const std::string &f) {
        if (remove(f.c_str()) != 0) return false;
        ++removed;
        fprintf(stderr, "[cleanup] removed %s\n", f.c_str());
        return true;
    };

    std::unordered_set<std::string> live_txt;   /* stems of the .txt files kept */
    for (const auto &f : txts) {
        std::string stem = f.full.substr(0, f.full.size() - 4);
        if (f.mtime < cutoff && rm(f.full)) {
            index_forget(rel_of(log_dir, f.full));   /* keep the index in step */
            rm(stem + ".json");                      /* its sidecar, if any */
        } else {
            live_txt.insert(stem);
        }
    }
    for (const auto &f : jsons) {
        std::string stem = f.full.substr(0, f.full.size() - 5);
        struct stat st {};
        if (lstat(f.full.c_str(), &st) != 0) continue;   /* went with its .txt above */
        if (!live_txt.count(stem) && f.mtime < cutoff) rm(f.full);
    }
    for (const auto &f : logs)
        if (f.mtime < cutoff) rm(f.full);
    for (const auto &f : tmps)
        if (f.mtime < tmp_cutoff) rm(f.full);

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
            if (lstat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
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

    if (removed > 0) {
        history_gen_bump();
        fprintf(stderr, "[cleanup] purged %d file(s) older than %d days\n", removed, retain_days);
    }
}

/* Resolve a client-supplied relative path safely.
 *
 * 1. Reject any rel that contains a NUL byte, is empty, or starts with '/'.
 * 2. Construct candidate = log_dir + "/" + rel.
 * 3. Call realpath() to resolve symlinks and canonicalise.
 * 4. Verify the resolved path still starts with the canonical log_dir.
 * 5. Optionally enforce an allowed file extension.
 *
 * Returns the resolved absolute path on success, or "" on any failure.
 */
static std::string safe_resolve(const std::string &log_dir,
                                const std::string &rel,
                                const std::string &required_ext = "")
{
    if (log_dir.empty() || rel.empty()) return "";
    /* Reject absolute paths and NUL bytes from client */
    if (rel[0] == '/' || rel.find('\0') != std::string::npos) return "";
    /* Reject obvious traversal attempts early (realpath catches the rest) */
    if (rel.find("..") != std::string::npos) return "";

    /* Canonicalise log_dir itself once */
    char canon_dir_buf[PATH_MAX];
    if (!realpath(log_dir.c_str(), canon_dir_buf)) return "";
    std::string canon_dir(canon_dir_buf);

    /* Build candidate and resolve */
    std::string candidate = canon_dir + "/" + rel;
    char canon_file_buf[PATH_MAX];
    if (!realpath(candidate.c_str(), canon_file_buf)) return "";
    std::string canon_file(canon_file_buf);

    /* Resolved path must be strictly inside canon_dir */
    if (canon_file.size() <= canon_dir.size()) return "";
    if (canon_file.substr(0, canon_dir.size()) != canon_dir) return "";
    if (canon_file[canon_dir.size()] != '/') return "";

    /* Optional extension whitelist */
    if (!required_ext.empty()) {
        if (canon_file.size() < required_ext.size()) return "";
        if (canon_file.substr(canon_file.size() - required_ext.size()) != required_ext)
            return "";
    }

    return canon_file;
}

static std::string history_read_file(const std::string &log_dir,
                                     const std::string &rel)
{
    /* Accept .txt message files and .log raw log files; reject everything else */
    std::string resolved = safe_resolve(log_dir, rel, ".txt");
    if (resolved.empty())
        resolved = safe_resolve(log_dir, rel, ".log");
    if (resolved.empty()) return "";

    FILE *f = fopen(resolved.c_str(), "r");
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
 * The client supplies the .txt relative path; the server derives the .json
 * path itself — the client never names a .json file directly. */
static std::string history_read_metrics(const std::string &log_dir,
                                        const std::string &txt_rel)
{
    /* Resolve the .txt path first (enforces extension + containment) */
    std::string resolved_txt = safe_resolve(log_dir, txt_rel, ".txt");
    if (resolved_txt.empty()) return "";

    /* Derive .json path server-side */
    std::string json_path = resolved_txt.substr(0, resolved_txt.size() - 4) + ".json";

    /* Re-verify the derived .json path is still inside log_dir */
    char canon_dir_buf[PATH_MAX];
    if (!realpath(log_dir.c_str(), canon_dir_buf)) return "";
    std::string canon_dir(canon_dir_buf);
    /* json_path is constructed from a realpath result so no need to re-resolve,
     * but we still check containment on the string level. */
    if (json_path.size() <= canon_dir.size()) return "";
    if (json_path.substr(0, canon_dir.size()) != canon_dir) return "";
    if (json_path[canon_dir.size()] != '/') return "";

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

/* URL-decode a query string value.  A '%' not followed by two hex digits is
 * kept literally rather than decoded to a NUL byte (strtol of "zz" is 0). */
static std::string url_decode(const std::string &s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() &&
            isxdigit((unsigned char)s[i+1]) && isxdigit((unsigned char)s[i+2])) {
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

/* An X-Forwarded-Prefix worth honouring: "" or one or more "/segment"s of
 * unreserved URL characters (RFC 3986), i.e. ^(/[A-Za-z0-9._~-]+)*$. */
static bool base_path_ok(const std::string &p)
{
    if (p.empty()) return true;
    if (p[0] != '/') return false;
    for (size_t i = 0; i < p.size(); i++) {
        char c = p[i];
        if (c == '/') {
            if (i + 1 >= p.size() || p[i + 1] == '/') return false;   /* empty segment */
            continue;
        }
        if (!isalnum((unsigned char)c) && c != '.' && c != '_' && c != '~' && c != '-')
            return false;
    }
    return true;
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

    /* ---- libcurl, once for the life of the process ----
     * Initialised here and never cleaned up.  run_channel() still pairs its
     * own init/cleanup around each reconnect, and libcurl counts those — so
     * holding one reference here means a channel's cleanup can never tear the
     * library down under the MQTT or location threads. */
    curl_global_init(CURL_GLOBAL_DEFAULT);

    /* ---- Receiver location, for station identification ----
     * The environment override first, then UberSDR's own description, fetched
     * by the location thread: a quick burst of retries at startup (which the
     * ident worker waits for before replaying history, so the replay normally
     * sees the position), then every 60 s until it succeeds.  A later success
     * that moves the position re-identifies history. */
    rx_read_env();
    std::thread(rx_location_thread, base_url).detach();

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
            /* The prefix ends up in the page — in the <script> block (where
             * navtex_js_string() escapes it) and from there in HTML attributes
             * built by the page's JS (flag <img src>, links).  Escaping every
             * one of those is fragile; a path prefix only ever needs path
             * characters, so anything else is ignored outright.  A proxy that
             * passes a client-supplied X-Forwarded-Prefix through was enough
             * to inject markup (review: '/p"><img src=x onerror=...>'). */
            if (!base_path_ok(base_path)) base_path.clear();

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

            /* GET /flags/<iso2>.svg — a country flag, embedded in the binary
             * (navtex_countries.h) so the page works on an offline LAN.  Never
             * changes for a given build, so it is cacheable for good. */
            if (path_only.compare(0, 7, "/flags/") == 0) {
                std::string f = path_only.substr(7);
                const char *svg = nullptr;
                if (f.size() == 6 && f.compare(2, 4, ".svg") == 0 &&
                    f[0] >= 'a' && f[0] <= 'z' && f[1] >= 'a' && f[1] <= 'z')
                    svg = navtex_flag_svg(f.substr(0, 2).c_str());
                if (!svg) {
                    resp->statusCode  = 404;
                    resp->description = "Not Found";
                    resp->headers["Content-Type"] = "text/plain";
                    resp->body = "not found";
                } else {
                    resp->statusCode  = 200;
                    resp->description = "OK";
                    resp->headers["Content-Type"]  = "image/svg+xml";
                    resp->headers["Cache-Control"] = "public, max-age=31536000, immutable";
                    resp->body = svg;
                }
                return resp;
            }

            /* GET /api/stations — the station table, the receiver location,
             * replay progress, and what has been heard from each station. */
            if (path_only == "/api/stations") {
                resp->statusCode  = 200;
                resp->description = "OK";
                resp->headers["Content-Type"] = "application/json";
                resp->body = stations_json();
                return resp;
            }

            /* GET /api/ident?rel=<relative-txt-path> — the full identification
             * of one message. */
            if (path_only == "/api/ident") {
                /* query_param: exact key, stops at '&' ("xrel=" and
                 * "rel=a&foo=1" used to be misread). */
                std::string rel;
                if (qpos != std::string::npos) query_param(path.substr(qpos + 1), "rel", rel);
                std::string body;
                int code = rel.empty() ? 404 : ident_lookup_json(log_dir, rel, body);
                if (rel.empty()) body = "{\"error\":\"rel required\"}";
                resp->statusCode  = code;
                resp->description = code == 200 ? "OK" : "Not Found";
                resp->headers["Content-Type"] = "application/json";
                resp->body = body;
                return resp;
            }

            /* GET /api/analytics[?from=<unix>&to=<unix>] — every identified
             * message as compact columns, for the Metrics view.  See
             * analytics_json() for the format and why it is not gzipped. */
            if (path_only == "/api/analytics") {
                std::string qs = qpos != std::string::npos ? path.substr(qpos + 1) : "";
                bool has_from = false, has_to = false;
                int64_t from = 0, to = 0;
                resp->headers["Content-Type"] = "application/json";
                if (!analytics_time_param(qs, "from", has_from, from) ||
                    !analytics_time_param(qs, "to", has_to, to)) {
                    resp->statusCode  = 400;
                    resp->description = "Bad Request";
                    resp->body = "{\"error\":\"from and to must be unix times in seconds\"}";
                    return resp;
                }
                resp->statusCode  = 200;
                resp->description = "OK";
                resp->headers["Cache-Control"] = "no-store";
                set_body_maybe_gzip(resp, req, analytics_json(has_from, from, has_to, to));
                return resp;
            }

            /* GET /api/latest — in-memory latest complete message per (freq, station, subject) */
            if (path_only == "/api/latest") {
                resp->statusCode  = 200;
                resp->description = "OK";
                resp->headers["Content-Type"] = "application/json";
                resp->body = latest_messages_json();
                return resp;
            }

            /* GET /api/metrics — per-frequency message counts (last 24h by hour, last 30d by day) */
            if (path_only == "/api/metrics") {
                resp->statusCode  = 200;
                resp->description = "OK";
                resp->headers["Content-Type"] = "application/json";
                resp->body = metrics_json(log_dir);
                return resp;
            }

            /* GET /api/history — return JSON list of saved messages */
            if (path_only == "/api/history") {
                resp->statusCode  = 200;
                resp->description = "OK";
                resp->headers["Content-Type"] = "application/json";
                resp->headers["Vary"] = "Accept-Encoding";
                std::string gz = accepts_gzip(req) ? history_list_json_cached(log_dir, true) : "";
                if (!gz.empty()) {
                    resp->headers["Content-Encoding"] = "gzip";
                    resp->body = std::move(gz);
                } else {
                    resp->body = history_list_json_cached(log_dir);
                }
                return resp;
            }

            /* GET /api/history/file?rel=<relative-path> — return .txt file content.
             * The rel parameter is a path relative to log_dir (as returned by
             * /api/history).  safe_resolve() canonicalises it with realpath() and
             * verifies it stays inside log_dir, defeating path traversal and
             * symlink attacks. */
            if (path_only == "/api/history/file") {
                /* query_param: exact key, stops at '&' ("xrel=" and
                 * "rel=a&foo=1" used to be misread). */
                std::string rel;
                if (qpos != std::string::npos) query_param(path.substr(qpos + 1), "rel", rel);
                std::string content = history_read_file(log_dir, rel);
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

            /* GET /api/history/metrics?rel=<relative-txt-path> — return JSON sidecar.
             * Client supplies the .txt relative path; server derives the .json path
             * itself — the client never names a .json file directly. */
            if (path_only == "/api/history/metrics") {
                /* query_param: exact key, stops at '&' ("xrel=" and
                 * "rel=a&foo=1" used to be misread). */
                std::string rel;
                if (qpos != std::string::npos) query_param(path.substr(qpos + 1), "rel", rel);
                std::string content =
                    sidecar_normalise_scale(history_read_metrics(log_dir, rel));
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

            /* Default: serve the HTML page.  It embeds the base path, so a
             * shared cache must not hand one proxy's page to another. */
            resp->statusCode = 200;
            resp->description = "OK";
            resp->headers["Content-Type"] = "text/html; charset=utf-8";
            resp->headers["Cache-Control"] = "no-store";
            resp->headers["Vary"] = "X-Forwarded-Prefix";
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

                    /* ...and its identification, if one still applies (a
                     * provisional for the message in progress, or the last
                     * final when idle), so a page opened mid-message does not
                     * sit on "identifying…" until the next one. */
                    std::string id = g_ident.frame_for_new_client(
                        ctx.channel_id, ctx.msg_parser.in_msg, mqtt_msg_id_for(ctx.msg_parser));
                    if (!id.empty()) {
                        std::string f;
                        f += (char)0x05;
                        f += (char)(uint8_t)ctx.channel_id;
                        f += id;
                        client_ws.sendBinary(f);
                    }
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

    /* ---- Station identification worker ----
     * After the web server, so the page and /api/stations can show the
     * replay's progress; before the channels, so no completion can be queued
     * to a worker that does not exist yet (it would wait in the queue and be
     * handled after the replay either way). */
    fprintf(stderr, "station ident  : algorithm v%d, %d stations, table %s\n",
            NAVTEX_IDENT_VERSION, NAVTEX_STATION_COUNT, ident_table_fp().c_str());
    g_ident.start(log_dir, &hub);

    /* ---- Start IXWebSocket network system ---- */
    ix::initNetSystem();

    /* ---- Log cleanup thread (runs on startup then every hour) ----
     * Also rebuilds the history listing once an hour, which is what drops
     * index entries for files deleted by hand (see history_list_json): without
     * it Metrics and Stations would go on counting them until somebody opened
     * History. */
    if (!log_dir.empty()) {
        if (log_retain_days > 0)
            fprintf(stderr, "log retention  : %d days\n", log_retain_days);
        std::thread([log_dir, log_retain_days]() {
            while (true) {
                purge_old_logs(log_dir, log_retain_days);
                std::this_thread::sleep_for(std::chrono::hours(1));
                if (g_ident.replay_done()) history_list_json_cached(log_dir);
            }
        }).detach();
    }

    /* ---- MQTT publishing via UberSDR's addon ingest port ----
     * Always on, no configuration: the endpoint is derived from base_url.
     * Started before the channels so nothing can complete a message while the
     * publisher is still coming up. */
    g_mqtt.start(base_url);

    /* ---- Spawn one thread per channel ---- */
    std::vector<std::thread> channel_threads;
    channel_threads.reserve(channels.size());
    for (auto &ctx : channels)
        channel_threads.emplace_back(run_channel, &ctx, base_url);

    /* Main thread waits for all channel threads (they run forever) */
    for (auto &t : channel_threads)
        t.join();

    /* Cleanup (unreachable in normal operation) */
    g_mqtt.stop();
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
