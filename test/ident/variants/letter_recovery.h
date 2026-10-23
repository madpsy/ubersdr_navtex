// letter_recovery: Bayesian transmitter identification that does not trust B1 blindly.
//
// Hypotheses: every table transmitter on the channel (any letter) plus U = "a transmitter
// that is not in the table, or decoder noise".  For each hypothesis
//
//   w(s) = prior(active flag) x P(B1 | letter(s), SNR)            letter channel
//        x rate(s, time of day)                                   schedule (table, canonical, learned)
//        x P(heard, SNR | distance, day/night of path, rx offset)  propagation
//        x burst continuity x serial continuity                    per-receiver memory
//
// and the posterior is normalised over all hypotheses.  Because every transmitter on the
// channel competes (not just the ones whose letter matches) a garbled B1 is recovered from
// "which station should be on air now and could be heard here", an off-slot vital warning
// keeps its letter but pays the off-slot rate, and a letter whose only table candidates are
// implausible (too far by day, SNR far too high) falls to U instead of a confident wrong answer.
//
// Per-receiver learning (observe): SNR calibration offset of this receiver, per-station
// 240-min cycle timing histogram (schedules that differ from the table), last message per
// channel (bursts are back to back), recent serials per station/subject.  When the receiver
// location is unknown, a coarse grid posterior over the receiver position is learned from the
// stream and the propagation term is marginalised over it.
#pragma once
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <vector>
#include <string>
#include <algorithm>
#include <utility>
#include "../ident_api.h"

namespace letter_recovery_ns {

static const double PI = 3.14159265358979323846;
static const double D2R = PI / 180.0;
static const double DENS_SCALE = 0.04;   // probability -> SNR-density scale (when no SNR is reported)

struct V3 { double x, y, z; };
static inline V3 unitv(double lat, double lon) {
    double a = lat * D2R, b = lon * D2R;
    V3 v = { std::cos(a) * std::cos(b), std::cos(a) * std::sin(b), std::sin(a) };
    return v;
}
static inline double dot(const V3 &a, const V3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline V3 mid(const V3 &a, const V3 &b) {
    V3 m = { a.x + b.x, a.y + b.y, a.z + b.z };
    double n = std::sqrt(dot(m, m));
    if (n < 1e-9) { m = a; n = 1; }
    m.x /= n; m.y /= n; m.z /= n;
    return m;
}
static inline double gc_km(const V3 &a, const V3 &b) {
    double c = dot(a, b);
    if (c > 1) c = 1;
    if (c < -1) c = -1;
    return 6371.0 * std::acos(c);
}
// subsolar point unit vector (low-precision almanac, ~0.01 deg)
static inline V3 subsolar(int64_t t) {
    double d = (double)t / 86400.0 - 10957.5;
    double g = (357.529 + 0.98560028 * d) * D2R;
    double q = 357.529 + 0.98560028 * d; (void)q;
    double L = (280.459 + 0.98564736 * d + 1.915 * std::sin(g) + 0.020 * std::sin(2 * g)) * D2R;
    double e = (23.439 - 0.00000036 * d) * D2R;
    double ra = std::atan2(std::cos(e) * std::sin(L), std::cos(L));
    double dec = std::asin(std::sin(e) * std::sin(L));
    double gmst = std::fmod(18.697374558 + 24.06570982441908 * d, 24.0);
    double lon = ra / D2R - gmst * 15.0;
    lon = std::fmod(lon + 540.0, 360.0) - 180.0;
    return unitv(dec / D2R, lon);
}
static inline double npdf(double z) { return 0.3989422804014327 * std::exp(-0.5 * z * z); }
static inline double ncdf(double z) { return 0.5 * std::erfc(-z / std::sqrt(2.0)); }

enum { BAND_518, BAND_490, BAND_424, BAND_HF, BAND_OTHER, NBAND };
static inline int band_of(long f) {
    if (std::labs(f - 518000) <= 500) return BAND_518;
    if (std::labs(f - 490000) <= 500) return BAND_490;
    if (std::labs(f - 424000) <= 500) return BAND_424;
    if (std::labs(f - 4209500) <= 1000) return BAND_HF;
    return BAND_OTHER;
}

struct St {
    int idx;            // table index (representative of its equivalence class)
    int band;
    char letter;
    V3 u;
    double lat, lon;
    double pwr_db;      // power adjustment from stated range
    double prior;       // active flag prior
    int nslots;
    double slots[24];   // effective schedule (minutes of day)
    double canon[6];    // canonical letter schedule
    bool canon_valid;
    bool sched_empty;
    bool sched_canonical;   // table == canonical letter schedule
    bool periodic;          // 240-min periodic (learnable on the cycle)
};

struct Tables {
    std::vector<St> st;
    std::vector<int> by_band[NBAND];
    std::vector<V3> sites;            // distinct transmitter sites (any channel)
    Tables() {
        int n = (int)(sizeof(NAVTEX_STATIONS) / sizeof(NAVTEX_STATIONS[0]));
        for (int i = 0; i < n; i++) {
            const NavtexStation &s = NAVTEX_STATIONS[i];
            V3 u = unitv(s.lat, s.lon);
            bool dup = false;
            for (size_t j = 0; j < st.size(); j++) {
                const NavtexStation &o = NAVTEX_STATIONS[st[j].idx];
                if (o.freq_hz == s.freq_hz && o.id == s.id && gc_km(st[j].u, u) < 5.0) { dup = true; break; }
            }
            bool newsite = true;
            for (size_t j = 0; j < sites.size(); j++) if (gc_km(sites[j], u) < 15.0) { newsite = false; break; }
            if (newsite) sites.push_back(u);
            if (dup) continue;
            St x;
            std::memset(&x, 0, sizeof(x));
            x.idx = i; x.band = band_of(s.freq_hz); x.letter = s.id; x.u = u; x.lat = s.lat; x.lon = s.lon;
            double r = s.range_nm > 0 ? s.range_nm : 300;
            double adj = 6.0 * std::log10(r / 300.0);
            if (adj > 3) adj = 3;
            if (adj < -3) adj = -3;
            x.pwr_db = adj;
            x.prior = s.active == 1 ? 1.0 : (s.active == 0 ? -1.0 : 0.8);   // -1: resolved with P.prior0
            x.canon_valid = s.id >= 'A' && s.id <= 'X';
            for (int k = 0; k < 6; k++) x.canon[k] = x.canon_valid ? (s.id - 'A') * 10 + 240 * k : 0;
            x.sched_empty = s.n_minutes <= 0;
            if (x.sched_empty) {
                x.nslots = x.canon_valid ? 6 : 0;
                for (int k = 0; k < x.nslots; k++) x.slots[k] = x.canon[k];
            } else {
                x.nslots = s.n_minutes > 24 ? 24 : s.n_minutes;
                for (int k = 0; k < x.nslots; k++) x.slots[k] = s.minutes[k];
            }
            // canonical?  periodic?
            bool canon = x.canon_valid && x.nslots == 6;
            bool periodic = x.nslots == 6;
            for (int k = 0; k < x.nslots && (canon || periodic); k++) {
                int m = (int)x.slots[k];
                bool hit = false;
                for (int j = 0; j < 6; j++) if ((int)x.canon[j] == m) hit = true;
                if (!hit) canon = false;
                if ((m - (int)x.slots[0]) % 240 != 0) periodic = false;
            }
            if (x.band == BAND_424) canon = false;
            x.sched_canonical = canon;
            x.periodic = periodic || x.sched_empty;
            st.push_back(x);
            by_band[x.band].push_back((int)st.size() - 1);
        }
        // B1 coordination: an unlisted service at site k cannot use letter L on a channel where a
        // listed station with letter L is within groundwave range of k (that is what the B1
        // allocation prevents).  excl[(band * 26 + L) * nsites + k]
        excl.assign((size_t)NBAND * 26 * sites.size(), 0);
        for (size_t i = 0; i < st.size(); i++) {
            if (!(st[i].letter >= 'A' && st[i].letter <= 'Z')) continue;
            for (size_t k = 0; k < sites.size(); k++)
                if (gc_km(st[i].u, sites[k]) < 700.0)
                    excl[((size_t)st[i].band * 26 + (st[i].letter - 'A')) * sites.size() + k] = 1;
        }
    }
    std::vector<unsigned char> excl;
};
static const Tables &tables() { static Tables t; return t; }

struct Path { double d, l10; V3 m; };     // receiver -> transmitter geometry
static inline Path make_path(const V3 &rx, const V3 &tx) {
    Path pt;
    pt.d = gc_km(rx, tx);
    if (pt.d < 20) pt.d = 20;
    pt.l10 = std::log10(pt.d);
    pt.m = mid(rx, tx);
    return pt;
}
// coarse 10-degree grid of possible receiver positions (receiver location unknown)
struct Grid {
    int nc;
    std::vector<V3> cu;
    std::vector<Path> cst;    // nc x stations
    Grid() {
        const Tables &T = tables();
        for (int la = -75; la <= 75; la += 10)
            for (int lo = -175; lo <= 175; lo += 10) cu.push_back(unitv(la, lo));
        nc = (int)cu.size();
        cst.resize((size_t)nc * T.st.size());
        for (int c = 0; c < nc; c++)
            for (size_t i = 0; i < T.st.size(); i++) cst[c * T.st.size() + i] = make_path(cu[c], T.st[i].u);
    }
};
static const Grid &grid() { static Grid g; return g; }

// ---------------------------------------------------------------- tunables
struct P {
    // timing
    double early_far, early_near, in0, over1, over2;   // slot window masses
    double off_A, off_B, off_D, off_L, off_V, off_other, off_unk;
    double sched_table_w, sched_canon_w, sched_unif_w;  // schedule-error mixture
    double learn_n0;
    // letter channel
    double e_base, e_low, e_mid_snr, e_nonletter_share;
    // propagation
    double gw0, gw_slope1, gw_slope2, sky_extra, absorb_db, sigma, sigma_hf, floor_db;
    // unknown / noise
    double pi_site, pi_else, pi_noise, u_mu, u_sigma, u_heard, ushare0, ushare_a0, ushare_max;
    // continuity
    double k_burst, serial_hit, serial_miss;
    // calibration
    double cap, temp, prior0;
    double loc_sig, loc_temp;
    double sig_fade, bias_max, bias_minp, tail, unk_scale, abstain_pu, rec_scale, hf_u, excl_w, u490, u518;
};
static P defaults() {
    P p;
    p.early_far = 0.03; p.early_near = 0.07; p.in0 = 0.80; p.over1 = 0.06; p.over2 = 0.03;
    p.off_A = 0.03; p.off_B = 0.25; p.off_D = 0.15; p.off_L = 0.05; p.off_V = 0.20; p.off_other = 0.05; p.off_unk = 0.08;
    p.sched_table_w = 0.88; p.sched_canon_w = 0.08; p.sched_unif_w = 0.04;
    p.learn_n0 = 40;
    p.e_base = 0.003; p.e_low = 0.03; p.e_mid_snr = 0.0; p.e_nonletter_share = 0.3;
    p.gw0 = 50; p.gw_slope1 = 0.02; p.gw_slope2 = 0.06; p.sky_extra = 11.0; p.absorb_db = 35; p.sigma = 9; p.sigma_hf = 10;
    p.floor_db = -3;
    p.pi_site = 0.004; p.pi_else = 0.01; p.pi_noise = 0.02; p.u_mu = 0; p.u_sigma = 12; p.u_heard = 0.3;
    p.ushare0 = 0.05; p.ushare_a0 = 50; p.ushare_max = 10;
    p.temp = 1.0; p.prior0 = 0.7;
    p.loc_sig = 4; p.loc_temp = 0.5;
    p.sig_fade = 5; p.bias_max = 0; p.bias_minp = 0.4; p.tail = 0.05; p.unk_scale = 0.9; p.abstain_pu = 0.85; p.rec_scale = 0.85; p.hf_u = 0.3; p.excl_w = 0.0; p.u490 = 2; p.u518 = 1;
    p.k_burst = 6; p.serial_hit = 4; p.serial_miss = 0.6;
    p.cap = 0.99;
#ifdef LR_TUNE
    // scratch tuning only: LR_PARAMS="name=value,..."
    const char *env = std::getenv("LR_PARAMS");
    if (env) {
        struct { const char *n; double *v; } tab[] = {
#define F(x) { #x, &p.x },
            F(early_far) F(early_near) F(in0) F(over1) F(over2) F(off_A) F(off_B) F(off_D) F(off_L) F(off_V) F(off_other) F(off_unk)
            F(sched_table_w) F(sched_canon_w) F(sched_unif_w) F(learn_n0) F(e_base) F(e_low) F(e_nonletter_share)
            F(gw0) F(gw_slope1) F(gw_slope2) F(sky_extra) F(absorb_db) F(sigma) F(sigma_hf) F(floor_db)
            F(pi_site) F(pi_else) F(pi_noise) F(u_mu) F(u_sigma) F(u_heard) F(ushare0) F(ushare_a0) F(ushare_max)
            F(k_burst) F(serial_hit) F(serial_miss) F(cap) F(temp) F(prior0) F(loc_sig) F(loc_temp) F(sig_fade) F(bias_max) F(bias_minp) F(tail) F(unk_scale) F(abstain_pu) F(rec_scale) F(hf_u) F(excl_w) F(u490) F(u518)
#undef F
        };
        std::string e(env);
        size_t pos = 0;
        while (pos < e.size()) {
            size_t c = e.find(',', pos); if (c == std::string::npos) c = e.size();
            std::string kv = e.substr(pos, c - pos); size_t q = kv.find('=');
            if (q != std::string::npos)
                for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++)
                    if (kv.substr(0, q) == tab[i].n) *tab[i].v = std::atof(kv.c_str() + q + 1);
            pos = c + 1;
        }
    }
#endif
    return p;
}

} // namespace

class LetterRecovery : public Identifier {
    typedef letter_recovery_ns::St St;
    typedef letter_recovery_ns::V3 V3;
public:
    LetterRecovery() : T(letter_recovery_ns::tables()), p(letter_recovery_ns::defaults()) {
        int n = (int)T.st.size();
        hist.assign(letter_recovery_ns::NBAND * 26 * 48, 0.0f);
        histN.assign(letter_recovery_ns::NBAND * 26, 0.0f);
        ser.assign(n * 26 * NSER, SerEnt());
        for (int b = 0; b < letter_recovery_ns::NBAND; b++) {
            last[b].valid = false;
            for (int l = 0; l < 26; l++) { ushare_u[b][l] = 0; ushare_n[b][l] = 0; }
        }
        snr_off = 0; snr_n = 0;
        w.resize(n);
        sres.assign(n, 0.0); sres_n.assign(n, 0.0); bw.assign(n, 0.0); bwr.assign(n, 0.0);
        rc_valid = false; loc_ready = false; loc_n = 0; loc_conc = 0;
        last_si = -1; last_conf = 0; last_pu = 0;
    }

    IdentResult identify(const IdentQuery &q) {
        using namespace letter_recovery_ns;
        IdentResult r;
        r.station = -1; r.confidence = 0; r.alt[0] = r.alt[1] = -1; r.alt_p[0] = r.alt_p[1] = 0;
        last_si = -1; last_conf = 0; last_pu = 0; last_pu0 = 0; last_b1 = -1;
        int band = band_of(q.freq_hz);
        const std::vector<int> &cand = T.by_band[band];
        if (cand.empty()) return r;
        if (q.rx_valid) rx_cache(q.rx_lat, q.rx_lon, q.start_utc, band);
        else loc_prepare();

        double minute = (double)(((q.start_utc % 86400) + 86400) % 86400) / 60.0;
        V3 sun = subsolar(q.start_utc);
        bool b1_letter = q.b1 >= 'A' && q.b1 <= 'X';
        double e = letter_err(q);
        double off = off_rate(q.b2);
        double x_snr = q.has_snr ? q.snr_db - snr_off_eff() : 0;

        double tot = 0;
        for (size_t c = 0; c < cand.size(); c++) {
            int si = cand[c];
            const St &s = T.st[si];
            double lw = s.prior < 0 ? p.prior0 : s.prior;
            // letter channel
            if (b1_letter) lw *= (s.letter == q.b1) ? (1 - e) : e * (1 - p.e_nonletter_share) / 23.0;
            else lw *= e * p.e_nonletter_share;
            // timing
            lw *= rate(si, s, minute, off);
            // propagation
            if (q.rx_valid) {
                double mu, sg;
                station_snr(si, mu, sg);
                lw *= obs_dens(mu, sg, q.has_snr, x_snr);
            } else if (loc_ready && (!b1_letter || s.letter == q.b1)) {
                lw *= loc_dens(si, s, sun, q.has_snr, x_snr);
            } else lw *= p.u_heard * letter_recovery_ns::DENS_SCALE;
            // burst continuity
            lw *= burst_factor(band, si, q);
            // serial continuity
            lw *= serial_factor(si, q);
            if (p.temp != 1.0) lw = std::pow(lw, p.temp);
            w[c] = lw;
            tot += lw;
        }
        // U: a transmitter that is not in the table (most such services sit at a known coast
        // station site on another channel), or decoder noise
        double wu = 0, wu0 = 0;
        {
            double lt = b1_letter ? (1 - e) : e * p.e_nonletter_share;
            double rt;
            if (b1_letter) {
                double cs[6];
                for (int k = 0; k < 6; k++) cs[k] = (q.b1 - 'A') * 10 + 240 * k;
                rt = 0.6 * slot_rate(cs, 6, minute, std::max(off, 0.15)) + 0.4 / 1440.0;
                rt = learned(band, q.b1, minute, rt);
            } else rt = 1.0 / 1440.0;
            double pr;
            if (q.rx_valid) {
                double sum = 0;
                const unsigned char *ex = b1_letter ? &T.excl[((size_t)band * 26 + (q.b1 - 'A')) * T.sites.size()] : 0;
                for (size_t k = 0; k < T.sites.size(); k++)
                    sum += obs_dens(mu_site[k], sg_site[k], q.has_snr, x_snr) * ((ex && ex[k]) ? p.excl_w : 1.0);
                pr = p.pi_site * sum + p.pi_else * obs_dens(p.u_mu, p.u_sigma, q.has_snr, x_snr);
                // HF NAVTEX (4209.5 kHz) is a short international list and 424 kHz is the
                // Japanese national channel; the national services that the table misses live
                // on 490/518
                if (band == BAND_HF || band == BAND_424) pr *= p.hf_u;
                // national-language 490 kHz services are the ones most often missing from the
                // list (at Dalgety Bay ~1/3 of labelled 490 traffic, against ~1% on 518)
                if (band == BAND_490) pr *= p.u490;
                if (band == BAND_518) pr *= p.u518;
            } else {
                pr = (p.pi_else + p.pi_site * 10) * p.u_heard * letter_recovery_ns::DENS_SCALE;
            }
            double f = 1.0;
            last_f = 1.0;
            if (b1_letter) {
                int l = q.b1 - 'A';
                double sh = (ushare_u[band][l] + p.ushare_a0 * p.ushare0) / (ushare_n[band][l] + p.ushare_a0);
                f = sh / p.ushare0;
                if (f > p.ushare_max) f = p.ushare_max;
                if (f < 0.3) f = 0.3;
                last_f = f;
            }
            wu = lt * rt * pr;
            wu0 = wu;                      // without the learned share: what the message alone says
            wu *= f;
            // decoder noise: random letter, random time, near the floor
            double ln = b1_letter ? 1.0 / 24.0 : 0.5;
            double pn = q.has_snr ? npdf((x_snr - (p.floor_db + 1)) / 3.0) / 3.0 : 0.1 * letter_recovery_ns::DENS_SCALE;
            wu += p.pi_noise * ln * (1.0 / 1440.0) * pn;
            if (p.temp != 1.0) wu = std::pow(wu, p.temp);
        }
        last_pu0 = wu0 / (tot + wu0 + 1e-300);
        tot += wu;
        if (!(tot > 0)) return r;

        int b0 = -1, b1i = -1, b2i = -1;
        for (size_t c = 0; c < cand.size(); c++) {
            if (b0 < 0 || w[c] > w[b0]) { b2i = b1i; b1i = b0; b0 = (int)c; }
            else if (b1i < 0 || w[c] > w[b1i]) { b2i = b1i; b1i = (int)c; }
            else if (b2i < 0 || w[c] > w[b2i]) { b2i = (int)c; }
        }
        double pu = wu / tot;
        double ptop = b0 >= 0 ? w[b0] / tot : 0;
        last_pu = pu;
        last_b1 = b1_letter ? q.b1 - 'A' : -1;
        if (b0 < 0 || (pu > ptop && pu > p.abstain_pu)) {
            if (b0 >= 0) { r.alt[0] = T.st[cand[b0]].idx; r.alt_p[0] = ptop; }
            return r;
        }
        r.station = T.st[cand[b0]].idx;
        {
            // features for the calibration map
            Feat &F = feat;
            const St &s = T.st[cand[b0]];
            F.p = ptop; F.pu = pu; F.p2 = b1i >= 0 ? w[b1i] / tot : 0;
            F.match = b1_letter && s.letter == q.b1;
            F.rxv = q.rx_valid; F.conc = q.rx_valid ? 1.0 : (loc_ready ? loc_conc : 0.0);
            F.snr = q.has_snr;
            F.logd = q.rx_valid ? rc_st[cand[b0]].l10 : -1;
            F.mu = q.rx_valid ? mu_st[cand[b0]] : 0;
            F.act = NAVTEX_STATIONS[s.idx].active;
            F.f = last_f;
            F.ratio = rate(cand[b0], s, minute, off) / (p.in0 / 10.0 / 6.0);
            F.ser = serial_factor(cand[b0], q);
            int nl = 0;
            for (size_t c = 0; c < cand.size(); c++) if (T.st[cand[c]].letter == s.letter && w[c] / tot > 0.001) nl++;
            F.nl = nl;
            F.resid = (q.rx_valid && q.has_snr) ? x_snr - mu_st[cand[b0]] : 0;
            F.sres = sres_n[cand[b0]] > 5 ? sres[cand[b0]] : 0;
        }
        r.confidence = calibrate(ptop, q, T.st[cand[b0]]);
#ifdef LR_TUNE
        static FILE *ff = std::getenv("LR_FEAT") ? std::fopen(std::getenv("LR_FEAT"), "w") : 0;
        if (ff) std::fprintf(ff, "%lld,%ld,%d,%.5f,%.5f,%.5f,%d,%d,%.3f,%d,%.3f,%.2f,%d,%.3f,%.4f,%.2f,%d,%.2f,%.2f\n", (long long)q.start_utc, q.freq_hz, r.station,
            feat.p, feat.pu, feat.p2, (int)feat.match, (int)feat.rxv, feat.conc, (int)feat.snr, feat.logd, feat.mu, feat.act, feat.f, feat.ratio, feat.ser, feat.nl, feat.resid, feat.sres);
#endif
        if (b1i >= 0) { r.alt[0] = T.st[cand[b1i]].idx; r.alt_p[0] = w[b1i] / tot; }
        if (b2i >= 0) { r.alt[1] = T.st[cand[b2i]].idx; r.alt_p[1] = w[b2i] / tot; }
        last_si = cand[b0]; last_conf = r.confidence; last_ptop = ptop;
        return r;
    }

    void observe(const IdentQuery &q, const IdentResult &res) {
        using namespace letter_recovery_ns;
        int band = band_of(q.freq_hz);
        if (!q.rx_valid && last_b1 >= 0) loc_update(q, band);
        if (last_b1 >= 0) {
            ushare_u[band][last_b1] += last_pu0; ushare_n[band][last_b1] += 1;
            // when is this letter on air at this receiver (unsupervised: decoded letters only)
            int li = band * 26 + last_b1;
            double m = std::fmod((double)(((q.start_utc % 86400) + 86400) % 86400) / 60.0, 240.0);
            int b = (int)(m / 5.0) % 48;
            float *h = &hist[li * 48];
            h[b] += 0.5f; h[(b + 47) % 48] += 0.25f; h[(b + 1) % 48] += 0.25f;
            histN[li] += 1.0f;
            if (histN[li] > 3000) {
                for (int k = 0; k < 48; k++) h[k] *= 0.5f;
                histN[li] *= 0.5f;
            }
        }
        Last &L = last[band];
        L.valid = true;
        L.end = q.start_utc + (q.duration_s > 0 ? q.duration_s : 30);
        L.si = res.station >= 0 ? last_si : -1;
        L.conf = res.station >= 0 ? last_ptop : 0;
        if (res.station < 0 || last_si < 0) return;
        const St &s = T.st[last_si];
        double c = last_ptop;
        if (c < 0.7) return;
        if (q.serial >= 0 && q.b2 >= 'A' && q.b2 <= 'Z') {
            SerEnt *se = &ser[(last_si * 26 + (q.b2 - 'A')) * NSER];
            int slot = -1;
            for (int k = 0; k < NSER; k++) if (se[k].t > 0 && se[k].serial == q.serial) slot = k;
            if (slot < 0) {
                slot = 0;
                for (int k = 1; k < NSER; k++) if (se[k].t < se[slot].t) slot = k;
            }
            se[slot].serial = q.serial; se[slot].t = q.start_utc;
        }
        if (q.has_snr && q.rx_valid && last_ptop >= p.bias_minp) {
            double rsd = q.snr_db - snr_off_eff() - mu_st[last_si];
            if (rsd > 25) rsd = 25;
            if (rsd < -25) rsd = -25;
            {   // Bayesian per-station bias of the SNR model at this receiver (soft, clamped)
                double wgt = last_ptop;
                bw[last_si] += wgt; bwr[last_si] += wgt * rsd;
                if (bw[last_si] > 300) { bw[last_si] *= 0.5; bwr[last_si] *= 0.5; }
            }
            sres_n[last_si] += 1;
            double a = sres_n[last_si] < 50 ? 1.0 / sres_n[last_si] : 0.02;
            sres[last_si] += a * (rsd - sres[last_si]);
        }
        if (q.has_snr && q.rx_valid && c >= 0.85) {
            double mu, sg;
            predict_c(s.band, rc_st[last_si], subsolar(q.start_utc), s.pwr_db, mu, sg);
            if (mu > 8) {
                double resid = q.snr_db - mu;
                if (resid > 20) resid = 20;
                if (resid < -20) resid = -20;
                snr_n += 1;
                double a = snr_n < 100 ? 1.0 / snr_n : 0.01;
                snr_off += a * (resid - snr_off);
            }
        }
    }

private:
    struct Last { bool valid; int64_t end; int si; double conf; };
    struct SerEnt { int serial; int64_t t; SerEnt() : serial(-1), t(0) {} };
    typedef letter_recovery_ns::Path Path;
    enum { NSER = 12 };
    const letter_recovery_ns::Tables &T;
    letter_recovery_ns::P p;
    std::vector<float> hist, histN;
    std::vector<SerEnt> ser;
    Last last[letter_recovery_ns::NBAND];
    double ushare_u[letter_recovery_ns::NBAND][26], ushare_n[letter_recovery_ns::NBAND][26];
    double snr_off, snr_n;
    std::vector<double> w;
    // receiver location posterior on the coarse grid (only when the location is unknown)
    std::vector<double> loc_lp;
    double loc_n; int64_t loc_last_t[letter_recovery_ns::NBAND];
    bool loc_ready; std::vector<int> loc_top; std::vector<double> loc_w;
    void loc_prepare() {
        loc_ready = false;
        if (loc_lp.empty() || loc_n < 30) return;
        // top cells holding 99% of the mass (at most 16)
        int nc = (int)loc_lp.size();
        double mx = -1e300;
        for (int c = 0; c < nc; c++) if (loc_lp[c] > mx) mx = loc_lp[c];
        std::vector<std::pair<double, int> > v;
        double z = 0;
        for (int c = 0; c < nc; c++) { double e = std::exp(loc_lp[c] - mx); z += e; if (e > 1e-4) v.push_back(std::make_pair(e, c)); }
        std::sort(v.begin(), v.end());
        loc_top.clear(); loc_w.clear();
        double acc = 0;
        for (int k = (int)v.size() - 1; k >= 0 && (int)loc_top.size() < 16; k--) {
            loc_top.push_back(v[k].second); loc_w.push_back(v[k].first / z);
            acc += v[k].first / z;
            if (acc > 0.99) break;
        }
        for (size_t k = 0; k < loc_w.size(); k++) loc_w[k] /= acc;
        loc_conc = loc_w.empty() ? 0 : loc_w[0];
        loc_ready = true;
    }
    double loc_conc;
    double loc_dens(int si, const St &s, const V3 &sun, bool has_snr, double x) const {
        const letter_recovery_ns::Grid &G = letter_recovery_ns::grid();
        double v = 0;
        for (size_t k = 0; k < loc_top.size(); k++) {
            double mu, sg;
            predict_c(s.band, G.cst[(size_t)loc_top[k] * T.st.size() + si], sun, s.pwr_db, mu, sg);
            v += loc_w[k] * obs_dens(mu, sg + p.loc_sig, has_snr, x);
        }
        return v;
    }
    void loc_update(const IdentQuery &q, int band) {
        using namespace letter_recovery_ns;
        const Grid &G = grid();
        if (loc_lp.empty()) { loc_lp.assign(G.nc, 0.0); loc_n = 0; for (int b = 0; b < NBAND; b++) loc_last_t[b] = -1000000; }
        // one update per channel per 4 minutes: bursts repeat the same evidence
        if (q.start_utc - loc_last_t[band] < (loc_conc > 0.98 && loc_n > 300 ? 1800 : 240)) return;
        loc_last_t[band] = q.start_utc;
        const std::vector<int> &cand = T.by_band[band];
        double minute = (double)(((q.start_utc % 86400) + 86400) % 86400) / 60.0;
        double off = off_rate(q.b2);
        V3 sun = subsolar(q.start_utc);
        double x = q.has_snr ? q.snr_db : 0;
        // P(this letter | a message was decoded now, receiver in cell c):
        //   sum over stations with letter B1 / sum over all stations that are in a slot now.
        // (Without the denominator, cells that can hear many stations would win.)
        std::vector<int> ls, as; std::vector<double> lr, ar;
        double floor_rate = 20.0 * off / 1440.0;
        for (size_t c = 0; c < cand.size(); c++) {
            const St &s = T.st[cand[c]];
            double rt = (s.prior < 0 ? p.prior0 : s.prior) * rate(cand[c], s, minute, off);
            if (s.letter == q.b1) { ls.push_back(cand[c]); lr.push_back(rt); }
            else if (rt > floor_rate) { as.push_back(cand[c]); ar.push_back(rt); }
        }
        double cs[6];
        for (int k = 0; k < 6; k++) cs[k] = (q.b1 - 'A') * 10 + 240 * k;
        double ru = (p.pi_else + p.pi_site * 10) * (0.6 * slot_rate(cs, 6, minute, std::max(off, 0.15)) + 0.4 / 1440.0)
                    * obs_dens(p.u_mu, p.u_sigma, q.has_snr, x);
        size_t nst = T.st.size();
        for (int c = 0; c < G.nc; c++) {
            const Path *row = &G.cst[(size_t)c * nst];
            double num = ru, den = 24.0 * ru;      // U: any letter
            for (size_t k = 0; k < ls.size(); k++) {
                double mu, sg;
                predict_c(band, row[ls[k]], sun, T.st[ls[k]].pwr_db, mu, sg);
                double v = lr[k] * obs_dens(mu, sg + p.loc_sig, q.has_snr, x);
                num += v;
                den += lr[k] * heard(mu, sg + p.loc_sig) * letter_recovery_ns::DENS_SCALE;
            }
            for (size_t k = 0; k < as.size(); k++) {
                double mu, sg;
                predict_c(band, row[as[k]], sun, T.st[as[k]].pwr_db, mu, sg);
                den += ar[k] * heard(mu, sg + p.loc_sig) * letter_recovery_ns::DENS_SCALE;
            }
            loc_lp[c] += p.loc_temp * std::log(num / den);
        }
        double mx = -1e300;
        for (int c = 0; c < G.nc; c++) if (loc_lp[c] > mx) mx = loc_lp[c];
        for (int c = 0; c < G.nc; c++) { loc_lp[c] -= mx; if (loc_lp[c] < -60) loc_lp[c] = -60; }
        loc_n += 1;
    }
    bool rc_valid; double rc_lat, rc_lon;
    std::vector<Path> rc_st, rc_site;
    std::vector<double> mu_st, sg_st, mu_site, sg_site;
    int64_t cache_t[letter_recovery_ns::NBAND]; int site_band;
    struct Feat { double p, pu, p2, conc, logd, mu, f, ratio, ser, resid, sres; bool match, rxv, snr; int act, nl; } feat;
    std::vector<double> sres, sres_n;   // per-station mean SNR residual at this receiver
    std::vector<double> bw, bwr;        // weighted residual sums for the learned per-station bias
    // predictive SNR for station si at this receiver: model + learned bias, model error + fading
    void station_snr(int si, double &mu, double &sg) const {
        if (p.bias_max <= 0) { mu = mu_st[si]; sg = sg_st[si]; return; }
        double sm = sg_st[si];                       // model uncertainty of the median
        double sf = p.sig_fade;                      // message-to-message fading
        double prec = 1.0 / (sm * sm) + bw[si] / (sf * sf);
        double b = (bwr[si] / (sf * sf)) / prec;
        if (b > p.bias_max) b = p.bias_max;
        if (b < -p.bias_max) b = -p.bias_max;
        mu = mu_st[si] + b;
        sg = std::sqrt(sf * sf + 1.0 / prec);
    }
    double last_f;
    int last_si, last_b1; double last_conf, last_pu, last_pu0, last_ptop;

    void rx_cache(double lat, double lon, int64_t t, int band) {
        using namespace letter_recovery_ns;
        if (!(rc_valid && lat == rc_lat && lon == rc_lon)) {
            rc_valid = true; rc_lat = lat; rc_lon = lon;
            V3 rx = unitv(lat, lon);
            rc_st.resize(T.st.size()); rc_site.resize(T.sites.size());
            for (size_t i = 0; i < T.st.size(); i++) rc_st[i] = path(rx, T.st[i].u);
            for (size_t i = 0; i < T.sites.size(); i++) rc_site[i] = path(rx, T.sites[i]);
            mu_st.assign(T.st.size(), 0); sg_st.assign(T.st.size(), 1);
            mu_site.assign(T.sites.size(), 0); sg_site.assign(T.sites.size(), 1);
            for (int b = 0; b < NBAND; b++) cache_t[b] = INT64_MIN;
            site_band = -1;
        }
        int64_t bucket = t / 120;                       // the sun moves ~0.5 deg in 2 min
        if (cache_t[band] == bucket && (site_band == BAND_HF) == (band == BAND_HF)) return;
        cache_t[band] = bucket;
        V3 sun = subsolar(t);
        const std::vector<int> &cand = T.by_band[band];
        for (size_t c = 0; c < cand.size(); c++) {
            const St &s = T.st[cand[c]];
            predict_c(s.band, rc_st[cand[c]], sun, s.pwr_db, mu_st[cand[c]], sg_st[cand[c]]);
        }
        // sites: cached per band in the same arrays (bands rarely interleave within 2 min)
        for (size_t k = 0; k < T.sites.size(); k++) predict_c(band, rc_site[k], sun, 0.0, mu_site[k], sg_site[k]);
        site_band = band;
    }
    static Path path(const V3 &rx, const V3 &tx) { return letter_recovery_ns::make_path(rx, tx); }
    double snr_off_eff() const {
        if (snr_n < 20) return 0;
        double o = snr_off * (snr_n < 100 ? snr_n / 100.0 : 1.0);
        if (o > 15) o = 15;
        if (o < -15) o = -15;
        return o;
    }
    double letter_err(const IdentQuery &q) const {
        double e = 0.01;
        if (q.has_snr) e = p.e_base + p.e_low / (1 + std::exp(q.snr_db / 1.5));
        if (q.has_chars && q.chars_failed_pct > 2) e += 0.02;
        return e;
    }
    double off_rate(char b2) const {
        switch (b2) {
        case 'A': return p.off_A;
        case 'B': return p.off_B;
        case 'D': return p.off_D;
        case 'L': return p.off_L;
        case 'V': return p.off_V;
        case 0: return p.off_unk;
        default: return (b2 >= 'A' && b2 <= 'Z') ? p.off_other : p.off_unk;
        }
    }
    double g(double o) const {
        if (o < -15 || o >= 40) return 0;
        if (o < -5) return p.early_far / 10.0;
        if (o < 0) return p.early_near / 5.0;
        if (o < 10) return p.in0 / 10.0;
        if (o < 20) return p.over1 / 10.0;
        return p.over2 / 20.0;
    }
    double slot_rate(const double *slots, int n, double minute, double off) const {
        double s = 0;
        for (int k = 0; k < n; k++) {
            double o = minute - slots[k];
            if (o < -720) o += 1440; else if (o >= 720) o -= 1440;
            s += g(o);
        }
        return (1 - off) * s / 6.0 + off / 1440.0;
    }
    double rate(int si, const St &s, double minute, double off) const {
        (void)si;
        double r;
        if (s.nslots == 0) r = 1.0 / 1440.0;
        else if (s.sched_empty) r = 0.7 * slot_rate(s.canon, 6, minute, off) + 0.3 / 1440.0;
        else if (s.sched_canonical || !s.canon_valid)
            r = (p.sched_table_w + p.sched_canon_w) * slot_rate(s.slots, s.nslots, minute, off) + p.sched_unif_w / 1440.0;
        else
            r = p.sched_table_w * slot_rate(s.slots, s.nslots, minute, off)
              + p.sched_canon_w * slot_rate(s.canon, 6, minute, off) + p.sched_unif_w / 1440.0;
        if (s.periodic) r = learned(s.band, s.letter, minute, r);
        return r;
    }
    // blend a prior rate with what this receiver has seen of letter L on this channel
    double learned(int band, char L, double minute, double r) const {
        if (!(L >= 'A' && L <= 'X')) return r;
        int li = band * 26 + (L - 'A');
        float N = histN[li];
        if (N <= 0) return r;
        double m = std::fmod(minute, 240.0);
        int b = (int)(m / 5.0) % 48;
        double rl = hist[li * 48 + b] / N / 5.0 / 6.0;
        return (p.learn_n0 * r + N * rl) / (p.learn_n0 + N);
    }
    // predicted median SNR (receiver-independent scale) and spread
    void predict_c(int band, const Path &pt, const V3 &sun, double pwr, double &mu, double &sg) const {
        double se = dot(pt.m, sun);
        double el = std::asin(se > 1 ? 1 : (se < -1 ? -1 : se)) / letter_recovery_ns::D2R;
        double dayf = (el + 8.0) / 16.0;
        if (dayf < 0) dayf = 0;
        if (dayf > 1) dayf = 1;
        double d = pt.d;
        if (band == letter_recovery_ns::BAND_HF) {
            mu = 42 - 12 * pt.l10 - 1.5 * std::max(0.0, d - 3000) / 1000.0 - 8 * dayf + pwr;
            sg = p.sigma_hf;
            return;
        }
        double gw = p.gw0 - 15 * pt.l10 - p.gw_slope1 * std::min(std::max(0.0, d - 300), 600.0)
                    - p.gw_slope2 * std::max(0.0, d - 900);
        double l10s = d < 300 ? 2.4771 : pt.l10;
        double sky = p.gw0 - 15 * l10s - p.sky_extra * std::max(0.0, d - 1000) / 1000.0 - p.absorb_db * dayf;
        double pw = std::pow(10.0, gw / 10) + std::pow(10.0, sky / 10);
        mu = 10 * std::log10(pw) + pwr;
        sg = p.sigma + 3 * (1 - dayf) * (d > 1000 ? 1 : d / 1000.0);
    }
    double heard(double mu, double sg) const { return letter_recovery_ns::ncdf((mu - p.floor_db) / sg); }
    // P(decoded, SNR = x) (density per dB) or P(decoded) * letter_recovery_ns::DENS_SCALE without SNR
    double obs_dens(double mu, double sg, bool has_snr, double x) const {
        using letter_recovery_ns::npdf;
        double v;
        if (has_snr) v = (1 - p.tail) * npdf((x - mu) / sg) / sg + p.tail * npdf((x - mu) / 20.0) / 20.0;
        else v = letter_recovery_ns::ncdf((mu - p.floor_db) / sg) * letter_recovery_ns::DENS_SCALE;
        return v + 1e-12;
    }
    double burst_factor(int band, int si, const IdentQuery &q) const {
        const Last &L = last[band];
        if (!L.valid || L.si < 0) return 1.0;
        int64_t gap = q.start_utc - L.end;
        if (gap < -5 || gap > 40) return 1.0;
        if (L.si != si) return 1.0;
        // only for letter recovery: when B1 agrees with this station the previous message's
        // evidence (same slot, same path) would be counted twice
        if (q.b1 == T.st[si].letter) return 1.0;
        return 1.0 + p.k_burst * L.conf;
    }
    double serial_factor(int si, const IdentQuery &q) const {
        if (q.serial < 0 || !(q.b2 >= 'A' && q.b2 <= 'Z')) return 1.0;
        if (q.b1 == T.st[si].letter) return 1.0;     // letter recovery only (see burst_factor)
        const SerEnt *se = &ser[(si * 26 + (q.b2 - 'A')) * NSER];
        int recent = 0; bool hit = false;
        for (int k = 0; k < NSER; k++) {
            if (se[k].t <= 0) continue;
            int64_t age = q.start_utc - se[k].t;
            if (age < 0 || age > 5 * 86400) continue;
            recent++;
            if (se[k].serial == q.serial) hit = true;
        }
        if (hit) return p.serial_hit;
        if (recent >= 4) return p.serial_miss;
        return 1.0;
    }
    // Monotone map from the model posterior to a calibrated confidence (fitted on the synthetic
    // training set, checked on the real proxy set, where it is conservative).  The posterior is
    // close to calibrated in the middle; the band [0.85, 0.95) is held just under 0.9 because
    // its accuracy (96-99%) does not justify a ">= 0.9 and almost never wrong" claim.
    double calibrate(double pt, const IdentQuery &q, const St &st) const {
        static const double kx[] = { 0.00, 0.30, 0.50, 0.60, 0.70, 0.80, 0.85, 0.93, 0.95, 0.97, 0.98, 0.99, 1.00 };
        static const double ky[] = { 0.45, 0.50, 0.60, 0.72, 0.84, 0.885, 0.89, 0.897, 0.91, 0.95, 0.97, 0.985, 0.995 };
        const int n = sizeof(kx) / sizeof(kx[0]);
        double x = pt < 0 ? 0 : (pt > 1 ? 1 : pt);
        double c = ky[n - 1];
        for (int i = 1; i < n; i++)
            if (x <= kx[i]) { c = ky[i - 1] + (ky[i] - ky[i - 1]) * (x - kx[i - 1]) / (kx[i] - kx[i - 1]); break; }
        if (!(q.b1 == st.letter)) {
            // letter recovered from timing / continuity, not read: the header is damaged, and
            // so may be everything else about this message
            c *= p.rec_scale;
        }
        if (!q.rx_valid) {
            // receiver position inferred from the stream: never more than "probably"
            c *= p.unk_scale;
            if (!loc_ready) c *= 0.7;
        }
        if (c > p.cap) c = p.cap;
        return c;
    }
};

Identifier *make_identifier() { return new LetterRecovery(); }
