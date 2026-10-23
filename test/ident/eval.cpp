// Evaluation harness for NAVTEX transmitter identification variants.
//
//   g++ -std=c++11 -O2 -DVARIANT='"variants/<name>.h"' eval.cpp -o eval_<name>
//   ./eval_<name> [--tags] [--proxy real_proxy.csv] [--no-cache] data/train.csv data/real.csv[@N] ...
//
// Each CSV is converted once into <csv minus .csv>.bin (rows sorted by receiver
// then start time) and that cache is read in one fread afterwards; it is rebuilt
// whenever the CSV (or the proxy file) is newer.  "file.csv@N" evaluates a fixed,
// deterministic N-row sample: for every receiver a contiguous chronological window
// proportional to its share of the rows, so observe() still sees real streams.
//
// Every receiver gets a fresh Identifier (make_identifier()), fed in
// chronological order: identify() then observe().
//
// Scoring conventions (see README.md):
//   correct  = station != -1 ? same transmitter as truth (table rows that are the
//              same physical transmitter - same freq, letter, site - are equivalent)
//            : truth == -1
//   An abstention (station == -1, confidence must be 0) is scored in brier/ece as a
//   probability-1 claim "not attributable": it costs 1 when the truth is in the
//   table and 0 when it is not.  It never counts in wrong_hi / wrong_med / coverage.
//   wrong_hi  = P(wrong | station != -1 && conf >= 0.90)
//   wrong_med = P(wrong | station != -1 && conf >= 0.60)
//   coverage_hi = #(conf >= 0.9 && correct) / #all
//   score = coverage_hi - 2*brier - ece   (gate: wrong_hi <= 0.01)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <sys/stat.h>
#include <sys/time.h>

#include "ident_api.h"
// Single-command build: -DVARIANT='"variants/<name>.h"'.  run_eval.sh instead compiles
// this file once with -DEVAL_CORE_ONLY and links it with a per-variant object
// (variant compile + link ~0.3 s instead of ~3 s).
#if defined(VARIANT)
#include VARIANT
#elif !defined(EVAL_CORE_ONLY)
#error "compile with -DVARIANT='\"variants/<name>.h\"' (or -DEVAL_CORE_ONLY and link a variant object)"
#endif

namespace {

const int NST = (int)(sizeof(NAVTEX_STATIONS) / sizeof(NAVTEX_STATIONS[0]));
const char BIN_MAGIC[8] = {'N', 'V', 'I', 'D', 'B', 'I', 'N', '3'};
const int MAXTAG = 64;

#pragma pack(push, 1)
struct Row {                      // 80 bytes
    int64_t start;
    double  rx_lat, rx_lon;
    float   snr, chars, dist_km;
    int32_t freq, dur, serial;
    int16_t truth;                // -1 = not in table / noise; -2 = no truth (real data)
    int16_t proxy;                // -1 = proxy says not in table, -2 = no proxy label
    uint16_t rx;
    char    b1, b2;
    uint8_t flags;                // 1 rx_valid, 2 has_snr, 4 has_chars
    uint8_t ncand;                // table stations with this freq and letter == b1
    uint8_t pad[2];
    uint64_t tags;
    uint32_t key;                 // FNV-1a of the CSV 'rel' column (real data join key), 0 if none
    float   pconf;                // proxy label confidence (1 if not given)
};
struct BinHeader {
    char     magic[8];
    uint32_t table_hash, nrows, nrx, ntags;
};
#pragma pack(pop)

struct Dataset {
    std::string name, path;
    std::vector<Row> rows;
    std::vector<std::string> rx_names, tag_names;
    bool any_truth, any_proxy;
};

double now_s() { struct timeval tv; gettimeofday(&tv, 0); return tv.tv_sec + tv.tv_usec * 1e-6; }

uint32_t fnv(const std::string &s) {
    uint32_t h = 2166136261u;
    for (size_t k = 0; k < s.size(); k++) { h ^= (uint8_t)s[k]; h *= 16777619u; }
    return h ? h : 1;
}

uint32_t table_hash() {
    uint32_t h = 2166136261u;
    for (int i = 0; i < NST; i++) {
        const NavtexStation &s = NAVTEX_STATIONS[i];
        char buf[256];
        int n = snprintf(buf, sizeof buf, "%d%c%.4f%.4f%d", s.freq_hz, s.id, s.lat, s.lon, s.n_minutes);
        for (int k = 0; k < n; k++) { h ^= (uint8_t)buf[k]; h *= 16777619u; }
    }
    return h;
}

double dist_km(double la1, double lo1, double la2, double lo2) {
    const double k = 3.14159265358979323846 / 180.0;
    double a = std::sin((la2 - la1) * k / 2), b = std::sin((lo2 - lo1) * k / 2);
    double h = a * a + std::cos(la1 * k) * std::cos(la2 * k) * b * b;
    return 2 * 6371.0 * std::asin(std::sqrt(h < 1 ? h : 1));
}

// equivalence class: lowest index of the same physical transmitter
std::vector<int> g_eqv;
void build_eqv() {
    g_eqv.resize(NST);
    for (int i = 0; i < NST; i++) {
        g_eqv[i] = i;
        for (int j = 0; j < i; j++) {
            const NavtexStation &a = NAVTEX_STATIONS[i], &b = NAVTEX_STATIONS[j];
            if (a.freq_hz == b.freq_hz && a.id == b.id && dist_km(a.lat, a.lon, b.lat, b.lon) < 5) { g_eqv[i] = g_eqv[j]; break; }
        }
    }
}

time_t file_mtime(const std::string &p) { struct stat st; return stat(p.c_str(), &st) == 0 ? st.st_mtime : 0; }

std::string read_file(const std::string &p) {
    FILE *f = fopen(p.c_str(), "rb");
    if (!f) return std::string();
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    std::string s; s.resize(n);
    if (n && fread(&s[0], 1, n, f) != (size_t)n) s.clear();
    fclose(f);
    return s;
}

// ---- minimal CSV splitter (handles "quoted" fields with "" escapes)
void split_line(const char *&p, const char *end, std::vector<std::string> &out) {
    out.clear();
    std::string cur;
    while (p < end) {
        cur.clear();
        if (*p == '"') {
            p++;
            while (p < end) {
                if (*p == '"') { if (p + 1 < end && p[1] == '"') { cur += '"'; p += 2; continue; } p++; break; }
                cur += *p++;
            }
            while (p < end && *p != ',' && *p != '\n') p++;
        } else {
            while (p < end && *p != ',' && *p != '\n' && *p != '\r') cur += *p++;
            if (p < end && *p == '\r') p++;
        }
        out.push_back(cur);
        if (p < end && *p == ',') { p++; if (p == end || *p == '\n') { out.push_back(std::string()); } else continue; }
        if (p < end && *p == '\n') { p++; break; }
    }
}

int64_t parse_time(const std::string &s) {   // unix seconds or ISO-8601 "YYYY-MM-DDTHH:MM:SSZ"
    if (s.size() >= 19 && s[4] == '-') {
        struct tm t; memset(&t, 0, sizeof t);
        t.tm_year = atoi(s.c_str()) - 1900; t.tm_mon = atoi(s.c_str() + 5) - 1; t.tm_mday = atoi(s.c_str() + 8);
        t.tm_hour = atoi(s.c_str() + 11); t.tm_min = atoi(s.c_str() + 14); t.tm_sec = atoi(s.c_str() + 17);
        return (int64_t)timegm(&t);
    }
    return (int64_t)atoll(s.c_str());
}

// ---- proxy labels: CSV with a header; needs a freq column (freq_hz|freq), a start column
// (start_utc|start|t, unix or ISO) and a label column (proxy_truth|station_index|station_idx|station|truth|idx).
// An empty label is skipped; -1 means "proxy says not in table".
struct ProxyKey { int freq; int64_t t; bool operator<(const ProxyKey &o) const { return freq != o.freq ? freq < o.freq : t < o.t; } };
struct ProxyVal { int label; float conf; };
std::map<ProxyKey, ProxyVal> g_proxy;       // keyed by (freq, start) ...
std::map<uint32_t, ProxyVal> g_proxy_rel;   // ... or by hash of 'rel'
float g_proxy_min_conf = 0;
std::string g_proxy_path;
bool load_proxy(const std::string &path) {
    std::string s = read_file(path);
    if (s.empty()) { fprintf(stderr, "proxy: %s missing or empty - skipped\n", path.c_str()); return false; }
    const char *p = s.data(), *end = p + s.size();
    std::vector<std::string> f;
    split_line(p, end, f);
    int cf = -1, ct = -1, cl = -1, cr = -1, cc = -1;
    for (size_t i = 0; i < f.size(); i++) { if (f[i] == "rel") cr = (int)i; if (f[i] == "label_confidence" || f[i] == "proxy_conf") cc = (int)i; }
    const char *fl[] = {"freq_hz", "freq", 0}, *tl[] = {"start_utc", "start", "t", 0},
               *ll[] = {"proxy_truth", "station_index", "station_idx", "proxy_station", "station", "truth", "idx", 0};
    for (int pass = 0; pass < 3; pass++) {
        const char **L = pass == 0 ? fl : pass == 1 ? tl : ll;
        int &c = pass == 0 ? cf : pass == 1 ? ct : cl;
        for (int k = 0; L[k] && c < 0; k++)
            for (size_t i = 0; i < f.size(); i++) if (f[i] == L[k]) { c = (int)i; break; }
    }
    if (cl < 0 || (cr < 0 && (cf < 0 || ct < 0))) { fprintf(stderr, "proxy: %s needs a label column and rel or freq+start - skipped\n", path.c_str()); return false; }
    int n = 0;
    while (p < end) {
        split_line(p, end, f);
        int mx = std::max(cl, cr >= 0 ? cr : std::max(cf, ct));
        if ((int)f.size() <= mx || f[cl].empty()) continue;
        ProxyVal v = {atoi(f[cl].c_str()), cc >= 0 && cc < (int)f.size() && !f[cc].empty() ? (float)atof(f[cc].c_str()) : 1.f};
        if (cr >= 0) g_proxy_rel[fnv(f[cr])] = v;
        else {
            double fr = atof(f[cf].c_str());
            if (fr < 100000) fr *= 1000;              // kHz given
            ProxyKey k = {(int)lround(fr), parse_time(f[ct])};
            g_proxy[k] = v;
        }
        n++;
    }
    fprintf(stderr, "proxy: %d labels from %s\n", n, path.c_str());
    return true;
}
const ProxyVal *proxy_lookup(const struct Row &r);

const ProxyVal *proxy_lookup(const Row &r) {
    if (r.key && !g_proxy_rel.empty()) {
        std::map<uint32_t, ProxyVal>::const_iterator it = g_proxy_rel.find(r.key);
        return it == g_proxy_rel.end() ? 0 : &it->second;
    }
    for (int d = 0; d <= 2 && !g_proxy.empty(); d++)
        for (int sg = -1; sg <= 1; sg += 2) {
            ProxyKey k = {r.freq, r.start + sg * d};
            std::map<ProxyKey, ProxyVal>::const_iterator it = g_proxy.find(k);
            if (it != g_proxy.end()) return &it->second;
            if (d == 0) break;
        }
    return 0;
}

// ---- CSV -> Dataset
bool parse_csv(const std::string &path, Dataset &ds) {
    std::string s = read_file(path);
    if (s.empty()) return false;
    const char *p = s.data(), *end = p + s.size();
    std::vector<std::string> h, f;
    split_line(p, end, h);
    std::map<std::string, int> col;
    for (size_t i = 0; i < h.size(); i++) col[h[i]] = (int)i;
    const char *need[] = {"rx", "rx_valid", "rx_lat", "rx_lon", "freq_hz", "b1", "b2", "serial", "start_utc", "duration_s",
                          "snr_db", "chars_failed_pct", "truth", "tags", 0};
    for (int k = 0; need[k]; k++) if (!col.count(need[k])) { fprintf(stderr, "%s: missing column %s\n", path.c_str(), need[k]); return false; }
    int c_proxy = col.count("proxy_truth") ? col["proxy_truth"] : -1;
    int c_dist = col.count("dist_km") ? col["dist_km"] : -1;
    int c_rel = col.count("rel") ? col["rel"] : -1;
    int c_pconf = col.count("proxy_conf") ? col["proxy_conf"] : -1;
    std::map<std::string, int> rxmap, tagmap;
    std::vector<int> ncand_tab(1); // unused placeholder
    ds.rows.reserve(s.size() / 80);
    size_t maxc = h.size();
    while (p < end) {
        split_line(p, end, f);
        if (f.size() < maxc - 3 || f.size() < 14) continue;
        Row r; memset(&r, 0, sizeof r);
        const std::string &rx = f[col["rx"]];
        std::map<std::string, int>::iterator it = rxmap.find(rx);
        if (it == rxmap.end()) { it = rxmap.insert(std::make_pair(rx, (int)ds.rx_names.size())).first; ds.rx_names.push_back(rx); }
        r.rx = (uint16_t)it->second;
        bool rv = atoi(f[col["rx_valid"]].c_str()) != 0;
        r.flags |= rv ? 1 : 0;
        r.rx_lat = rv ? atof(f[col["rx_lat"]].c_str()) : 0;
        r.rx_lon = rv ? atof(f[col["rx_lon"]].c_str()) : 0;
        r.freq = (int32_t)atol(f[col["freq_hz"]].c_str());
        const std::string &b1 = f[col["b1"]], &b2 = f[col["b2"]];
        r.b1 = b1.empty() ? 0 : b1[0];
        r.b2 = b2.empty() ? 0 : b2[0];
        const std::string &se = f[col["serial"]];
        r.serial = se.empty() ? -1 : atoi(se.c_str());
        r.start = parse_time(f[col["start_utc"]]);
        r.dur = atoi(f[col["duration_s"]].c_str());
        const std::string &sn = f[col["snr_db"]], &ch = f[col["chars_failed_pct"]];
        if (!sn.empty()) { r.flags |= 2; r.snr = (float)atof(sn.c_str()); }
        if (!ch.empty()) { r.flags |= 4; r.chars = (float)atof(ch.c_str()); }
        const std::string &tr = f[col["truth"]];
        r.truth = tr.empty() ? -2 : (int16_t)atoi(tr.c_str());
        if (r.truth >= NST) r.truth = -1;
        if (r.truth >= 0) ds.any_truth = true; else if (r.truth == -1) ds.any_truth = true;
        r.proxy = -2;
        if (c_proxy >= 0 && c_proxy < (int)f.size() && !f[c_proxy].empty()) r.proxy = (int16_t)atoi(f[c_proxy].c_str());
        if (r.proxy >= NST) r.proxy = -2;
        if (r.proxy != -2) ds.any_proxy = true;
        r.key = (c_rel >= 0 && c_rel < (int)f.size() && !f[c_rel].empty()) ? fnv(f[c_rel]) : 0;
        r.pconf = (c_pconf >= 0 && c_pconf < (int)f.size() && !f[c_pconf].empty()) ? (float)atof(f[c_pconf].c_str()) : 1.f;
        r.dist_km = (c_dist >= 0 && c_dist < (int)f.size()) ? (float)atof(f[c_dist].c_str()) : -1.f;
        const std::string &tg = f[col["tags"]];
        size_t a = 0;
        while (a < tg.size()) {
            size_t b = tg.find('|', a); if (b == std::string::npos) b = tg.size();
            std::string t = tg.substr(a, b - a);
            if (!t.empty()) {
                std::map<std::string, int>::iterator ti = tagmap.find(t);
                if (ti == tagmap.end()) {
                    if ((int)ds.tag_names.size() >= MAXTAG) { a = b + 1; continue; }
                    ti = tagmap.insert(std::make_pair(t, (int)ds.tag_names.size())).first; ds.tag_names.push_back(t);
                }
                r.tags |= (uint64_t)1 << ti->second;
            }
            a = b + 1;
        }
        int nc = 0;
        for (int i = 0; i < NST; i++)
            if (std::abs(NAVTEX_STATIONS[i].freq_hz - r.freq) <= 500 && NAVTEX_STATIONS[i].id == r.b1 && g_eqv[i] == i) nc++;
        r.ncand = (uint8_t)std::min(nc, 255);
        ds.rows.push_back(r);
    }
    // stable sort by receiver then time
    std::vector<size_t> idx(ds.rows.size());
    for (size_t i = 0; i < idx.size(); i++) idx[i] = i;
    struct Cmp { const std::vector<Row> *v; bool operator()(size_t a, size_t b) const {
        const Row &x = (*v)[a], &y = (*v)[b];
        if (x.rx != y.rx) return x.rx < y.rx; if (x.start != y.start) return x.start < y.start; return a < b; } } cmp;
    cmp.v = &ds.rows;
    std::sort(idx.begin(), idx.end(), cmp);
    std::vector<Row> sorted(ds.rows.size());
    for (size_t i = 0; i < idx.size(); i++) sorted[i] = ds.rows[idx[i]];
    ds.rows.swap(sorted);
    return true;
}

void put_names(FILE *f, const std::vector<std::string> &v) {
    for (size_t i = 0; i < v.size(); i++) { char b[32]; memset(b, 0, 32); strncpy(b, v[i].c_str(), 31); fwrite(b, 1, 32, f); }
}

bool load_bin(const std::string &bp, Dataset &ds) {
    FILE *f = fopen(bp.c_str(), "rb");
    if (!f) return false;
    BinHeader h;
    bool ok = fread(&h, sizeof h, 1, f) == 1 && !memcmp(h.magic, BIN_MAGIC, 8) && h.table_hash == table_hash();
    if (ok) {
        std::vector<char> names((size_t)(h.nrx + h.ntags) * 32);
        ok = names.empty() || fread(&names[0], 1, names.size(), f) == names.size();
        for (uint32_t i = 0; ok && i < h.nrx; i++) ds.rx_names.push_back(std::string(&names[i * 32]));
        for (uint32_t i = 0; ok && i < h.ntags; i++) ds.tag_names.push_back(std::string(&names[(h.nrx + i) * 32]));
        ds.rows.resize(h.nrows);
        ok = ok && (h.nrows == 0 || fread(&ds.rows[0], sizeof(Row), h.nrows, f) == h.nrows);
    }
    fclose(f);
    if (!ok) { ds.rows.clear(); ds.rx_names.clear(); ds.tag_names.clear(); return false; }
    ds.any_truth = ds.any_proxy = false;
    for (size_t i = 0; i < ds.rows.size(); i++) {
        if (ds.rows[i].truth != -2) ds.any_truth = true;
        if (ds.rows[i].proxy != -2) ds.any_proxy = true;
    }
    return true;
}

bool load_dataset(const std::string &path, Dataset &ds, bool use_cache) {
    std::string base = path.size() > 4 && path.substr(path.size() - 4) == ".csv" ? path.substr(0, path.size() - 4) : path;
    std::string bp = base + ".bin";
    size_t sl = base.find_last_of('/');
    ds.name = sl == std::string::npos ? base : base.substr(sl + 1);
    ds.path = path;
    ds.any_truth = ds.any_proxy = false;
    time_t mc = file_mtime(path), mb = file_mtime(bp);
    if (use_cache && mb && mb >= mc && load_bin(bp, ds)) return true;
    if (!mc) return false;
    double t0 = now_s();
    if (!parse_csv(path, ds)) return false;
    FILE *f = fopen(bp.c_str(), "wb");
    if (f) {
        BinHeader h; memcpy(h.magic, BIN_MAGIC, 8);
        h.table_hash = table_hash(); h.nrows = (uint32_t)ds.rows.size();
        h.nrx = (uint32_t)ds.rx_names.size(); h.ntags = (uint32_t)ds.tag_names.size();
        fwrite(&h, sizeof h, 1, f);
        put_names(f, ds.rx_names); put_names(f, ds.tag_names);
        if (!ds.rows.empty()) fwrite(&ds.rows[0], sizeof(Row), ds.rows.size(), f);
        fclose(f);
    }
    fprintf(stderr, "cached %s -> %s (%zu rows, %.2f s)\n", path.c_str(), bp.c_str(), ds.rows.size(), now_s() - t0);
    return true;
}

// deterministic per-receiver contiguous window sample
void sample_rows(Dataset &ds, size_t n) {
    if (n == 0 || n >= ds.rows.size()) return;
    std::vector<Row> out; out.reserve(n + ds.rx_names.size());
    size_t total = ds.rows.size(), i = 0;
    while (i < total) {
        size_t j = i; while (j < total && ds.rows[j].rx == ds.rows[i].rx) j++;
        size_t cnt = j - i, q = (size_t)((double)n * cnt / total + 0.5);
        if (q > cnt) q = cnt;
        const std::string &nm = ds.rx_names[ds.rows[i].rx];
        uint32_t hsh = 2166136261u; for (size_t k = 0; k < nm.size(); k++) { hsh ^= (uint8_t)nm[k]; hsh *= 16777619u; }
        size_t off = (cnt > q) ? hsh % (cnt - q + 1) : 0;
        out.insert(out.end(), ds.rows.begin() + i + off, ds.rows.begin() + i + off + q);
        i = j;
    }
    ds.rows.swap(out);
    char b[32]; snprintf(b, sizeof b, "@%zu", n); ds.name += b;
}

// ---- metrics
struct Acc {
    long n, n_in, ok_in, n_hi, bad_hi, n_med, bad_med, cov_hi, n_unk, n_abst, abst_unk, n_amb, ok_amb;
    double brier, unk_conf, sum_conf;
    double eb_c[10], eb_y[10]; long eb_n[10];
    void clear() { memset(this, 0, sizeof *this); }
    void add(int truth, int st, double conf, bool amb) {
        n++;
        bool correct = st >= 0 ? (truth >= 0 && g_eqv[st] == g_eqv[truth]) : truth == -1;
        double ec = st >= 0 ? conf : 1.0;            // abstention = certain "not attributable"
        double y = correct ? 1.0 : 0.0;
        brier += (ec - y) * (ec - y);
        int b = (int)(ec * 10); if (b > 9) b = 9; if (b < 0) b = 0;
        eb_c[b] += ec; eb_y[b] += y; eb_n[b]++;
        sum_conf += st >= 0 ? conf : 0;
        if (truth >= 0) { n_in++; if (correct) ok_in++; if (amb) { n_amb++; if (correct) ok_amb++; } }
        else { n_unk++; unk_conf += st >= 0 ? conf : 0; }
        if (st < 0) { n_abst++; if (truth == -1) abst_unk++; }
        else {
            if (conf >= 0.9) { n_hi++; if (!correct) bad_hi++; else cov_hi++; }
            if (conf >= 0.6) { n_med++; if (!correct) bad_med++; }
        }
    }
    double ece() const {
        double e = 0; for (int b = 0; b < 10; b++) if (eb_n[b]) e += std::fabs(eb_c[b] - eb_y[b]) / (n ? n : 1);
        return e;
    }
    double score() const { return n ? (double)cov_hi / n - 2 * brier / n - ece() : 0; }
    void print(FILE *o) const {
        double N = n ? n : 1;
        fprintf(o, "\"n\":%ld,\"acc\":%.4f,\"wrong_hi\":%.4f,\"n_hi\":%ld,\"wrong_med\":%.4f,\"n_med\":%ld,"
                   "\"coverage_hi\":%.4f,\"brier\":%.4f,\"ece\":%.4f,\"unk_conf\":%.4f,\"n_unk\":%ld,"
                   "\"abstain\":%.4f,\"abstain_prec\":%.4f,\"acc_amb\":%.4f,\"mean_conf\":%.4f,\"score\":%.4f,\"gate_ok\":%s",
                n, n_in ? (double)ok_in / n_in : 0, n_hi ? (double)bad_hi / n_hi : 0, n_hi,
                n_med ? (double)bad_med / n_med : 0, n_med, cov_hi / N, brier / N, ece(),
                n_unk ? unk_conf / n_unk : 0, n_unk, n_abst / N, n_abst ? (double)abst_unk / n_abst : 0,
                n_amb ? (double)ok_amb / n_amb : 0, sum_conf / N, score(), (n_hi == 0 || bad_hi <= 0.01 * n_hi) ? "true" : "false");
    }
};

struct Stats { long bad_station, bad_conf, abst_nonzero; };

void eval_dataset(Dataset &ds, bool tags, FILE *o) {
    double t0 = now_s();
    Acc all; all.clear();
    Acc prox; prox.clear();
    std::vector<Acc> per_tag(ds.tag_names.size());
    for (size_t i = 0; i < per_tag.size(); i++) per_tag[i].clear();
    Stats st = {0, 0, 0};
    // real-data metrics
    std::map<std::pair<int, int>, std::map<int, long> > modal;   // (freq,b1) -> station -> count
    long n_attr = 0, n_hi_all = 0, far_hi = 0, hist[11] = {0};
    double t_variant = 0;
    size_t i = 0, N = ds.rows.size();
    while (i < N) {
        size_t j = i; while (j < N && ds.rows[j].rx == ds.rows[i].rx) j++;
        Identifier *id = make_identifier();
        double tv = now_s();
        for (size_t k = i; k < j; k++) {
            const Row &r = ds.rows[k];
            IdentQuery q;
            q.freq_hz = r.freq; q.b1 = r.b1; q.b2 = r.b2; q.serial = r.serial;
            q.start_utc = r.start; q.duration_s = r.dur;
            q.rx_valid = (r.flags & 1) != 0; q.rx_lat = r.rx_lat; q.rx_lon = r.rx_lon;
            q.has_snr = (r.flags & 2) != 0; q.snr_db = q.has_snr ? r.snr : 0;
            q.has_chars = (r.flags & 4) != 0; q.chars_failed_pct = q.has_chars ? r.chars : 0;
            IdentResult res = id->identify(q);
            if (res.station < -1 || res.station >= NST) { st.bad_station++; res.station = -1; res.confidence = 0; }
            if (!(res.confidence >= 0 && res.confidence <= 1)) { st.bad_conf++; res.confidence = res.confidence > 1 ? 1 : 0; }
            if (res.station == -1 && res.confidence != 0) { st.abst_nonzero++; }
            id->observe(q, res);
            int s = res.station; double c = s >= 0 ? res.confidence : 0;
            bool amb = r.ncand >= 2;
            if (r.truth != -2) {
                all.add(r.truth, s, c, amb);
                if (tags) for (size_t t = 0; t < per_tag.size(); t++) if (r.tags >> t & 1) per_tag[t].add(r.truth, s, c, amb);
            }
            if (r.proxy != -2 && r.pconf >= g_proxy_min_conf) prox.add(r.proxy, s, c, amb);
            hist[s < 0 ? 10 : std::min(9, (int)(c * 10))]++;
            if (s >= 0) {
                n_attr++;
                modal[std::make_pair(r.freq, (int)r.b1)][g_eqv[s]]++;
                if (c >= 0.9) {
                    n_hi_all++;
                    if ((r.flags & 1) && dist_km(r.rx_lat, r.rx_lon, NAVTEX_STATIONS[s].lat, NAVTEX_STATIONS[s].lon) > 2500) far_hi++;
                }
            }
        }
        t_variant += now_s() - tv;
        delete id;
        i = j;
    }
    long mod_sum = 0, mod_tot = 0;
    for (std::map<std::pair<int, int>, std::map<int, long> >::iterator it = modal.begin(); it != modal.end(); ++it) {
        long mx = 0, tot = 0;
        for (std::map<int, long>::iterator jt = it->second.begin(); jt != it->second.end(); ++jt) { mx = std::max(mx, jt->second); tot += jt->second; }
        mod_sum += mx; mod_tot += tot;
    }
    double el = now_s() - t0;
    fprintf(o, "{\"dataset\":\"%s\",\"rows\":%zu,\"ms\":%.0f,\"us_per_msg\":%.2f,", ds.name.c_str(), N, el * 1000,
            N ? t_variant * 1e6 / N : 0);
    if (all.n) { all.print(o); fputc(',', o); }
    fprintf(o, "\"stability\":%.4f,\"attributed\":%.4f,\"far_hi\":%.4f,\"conf_hist\":[", mod_tot ? (double)mod_sum / mod_tot : 0,
            N ? (double)n_attr / N : 0, n_hi_all ? (double)far_hi / n_hi_all : 0);
    for (int b = 0; b < 11; b++) fprintf(o, "%s%.4f", b ? "," : "", N ? (double)hist[b] / N : 0);
    fprintf(o, "]");
    if (prox.n) { fprintf(o, ",\"proxy\":{"); prox.print(o); fprintf(o, "}"); }
    if (st.bad_station || st.bad_conf || st.abst_nonzero)
        fprintf(o, ",\"violations\":{\"bad_station\":%ld,\"bad_conf\":%ld,\"abstain_nonzero_conf\":%ld}", st.bad_station, st.bad_conf, st.abst_nonzero);
    fprintf(o, "}\n");
    if (tags)
        for (size_t t = 0; t < per_tag.size(); t++) {
            if (!per_tag[t].n) continue;
            fprintf(o, "{\"dataset\":\"%s\",\"tag\":\"%s\",", ds.name.c_str(), ds.tag_names[t].c_str());
            per_tag[t].print(o); fprintf(o, "}\n");
        }
}

} // namespace

int main(int argc, char **argv) {
    bool tags = false, cache = true;
    std::vector<std::string> files;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--tags") tags = true;
        else if (a == "--no-cache") cache = false;
        else if (a == "--proxy" && i + 1 < argc) g_proxy_path = argv[++i];
        else if (a == "--proxy-min-conf" && i + 1 < argc) g_proxy_min_conf = (float)atof(argv[++i]);
        else if (a == "-h" || a == "--help") {
            fprintf(stderr, "usage: %s [--tags] [--no-cache] [--proxy real_proxy.csv] [--proxy-min-conf X] file.csv[@N] ...\n", argv[0]); return 0;
        } else files.push_back(a);
    }
    build_eqv();
    if (!g_proxy_path.empty() && file_mtime(g_proxy_path) == 0) {
        fprintf(stderr, "proxy: %s not found - skipped\n", g_proxy_path.c_str());
        g_proxy_path.clear();
    }
    if (files.empty()) { fprintf(stderr, "no datasets\n"); return 2; }
    int rc = 0;
    for (size_t k = 0; k < files.size(); k++) {
        std::string p = files[k]; size_t n = 0;
        size_t at = p.rfind('@');
        if (at != std::string::npos) { n = (size_t)atol(p.c_str() + at + 1); p = p.substr(0, at); }
        bool is_real = p.find("real") != std::string::npos;
        if (is_real && !g_proxy_path.empty() && g_proxy.empty() && g_proxy_rel.empty()) load_proxy(g_proxy_path);
        Dataset ds;
        if (!load_dataset(p, ds, cache)) { fprintf(stderr, "cannot load %s\n", p.c_str()); rc = 1; continue; }
        if (is_real && (!g_proxy.empty() || !g_proxy_rel.empty())) {   // --proxy labels override the CSV's proxy_truth
            long hit = 0;
            for (size_t i = 0; i < ds.rows.size(); i++) {
                const ProxyVal *pv = proxy_lookup(ds.rows[i]);
                if (pv && pv->label < NST && pv->label >= -1) { ds.rows[i].proxy = (int16_t)pv->label; ds.rows[i].pconf = pv->conf; hit++; }
            }
            fprintf(stderr, "proxy: %ld of %zu rows of %s labelled\n", hit, ds.rows.size(), p.c_str());
        }
        sample_rows(ds, n);
        eval_dataset(ds, tags, stdout);
        fflush(stdout);
    }
    return rc;
}
