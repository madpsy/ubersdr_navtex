// bayes_physical: a Bayesian posterior over "which transmitter sent this header".
//
// Hypotheses, for a message on channel f with decoded letter b1:
//   H_s     every table station s on channel f (any letter: the decoded letter can be garbled)
//   H_unk   a transmitter on f that is NOT in the table, using letter b1 (national 490 kHz services,
//           stations missing from the list ...)
//   H_noise a false header / decoder garbage
//
// P(H | obs) ∝ rate(H) · P(b1 | H) · P(t | H) · P(snr | H)
//
//   rate(H_s)   = on-air prior (table "active" flag) × P(a scheduled slot is decoded | distance,
//                 day/night at the path midpoint, frequency) × a per-receiver learned factor
//   P(b1 | H_s) = 1-eps if b1 == letter, eps/25 otherwise; eps grows near the SNR decode floor
//   P(t | H_s)  = (1-w_off) · Σ_slots p_slot(t - slot)/n_slots + w_off/1440   (per minute of day)
//                 p_slot is the in-slot run model (early starts, messages back to back for up to
//                 10 min, rare overruns); w_off is the subject-dependent unscheduled share (gale
//                 warnings B and vital A/D/L are sent on receipt)
//   P(snr|H_s)  = robust Gaussian around the distance/day-night median SNR fitted to Dalgety Bay
//
// rate(H_unk) = base + (rho/26) x total audible rate of listed stations on f right now (unlisted
//               transmitters sit at coastal sites like listed ones, so far skywave attributions at
//               night compete with a larger unknown world) + (pi_coloc/26) x audible listed sites
//               that have nothing listed on f (national 490 kHz services at 518 kHz sites).
//               Its SNR term is the generic received-SNR distribution (position unknown).
//
// Burst continuation: a message starting <= 40 s after the previous one on the channel ended is
// mixed with the previous posterior (weight 0.8 if B1 agrees, 0.05 if not); only the letter
// evidence is re-applied, SNR is not counted twice.
//
// Learning (observe), per receiver, bounded memory:
//   * slot-level negative evidence: for every scheduled slot of every hypothesis during which the
//     receiver was demonstrably online (other messages within 90 min either side), add
//     obs = max posterior of H in [slot-15, slot+40 min] and exp = its modelled rate, separately
//     for day and night paths; factor = (alpha + Σobs)/(alpha + Σexp), forgotten at 0.97/slot,
//     clamped [0.03, 3] (unlisted: up to 5).  Silent / off-air stations lose weight; stations
//     heard more than modelled gain a little; persistent unexplained traffic flows to H_unk.
//   * receiver location unknown: a posterior over a 10x10 degree grid of receiver positions,
//     updated with tempered (0.3) letter/time/SNR likelihoods; once concentrated, propagation and
//     SNR terms are marginalised over the top cells.
//
// Confidence = posterior of the chosen transmitter (duplicate table rows merged) through a
// monotone isotonic calibration map (separate maps for known/unknown receiver location), capped
// below 0.9 under the gate threshold.  Abstain (station -1) only when "not in the table" is so
// probable that a certain non-attribution has lower expected Brier cost than a low-confidence guess.
#pragma once
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "../ident_api.h"

namespace bayes_physical {

static const double PI = 3.14159265358979323846;
static const double D2R = PI / 180.0;

inline double gc_km(double la1, double lo1, double la2, double lo2) {
    double a = std::sin((la2 - la1) * D2R / 2), b = std::sin((lo2 - lo1) * D2R / 2);
    double h = a * a + std::cos(la1 * D2R) * std::cos(la2 * D2R) * b * b;
    return 2 * 6371.0 * std::asin(std::sqrt(h < 1 ? h : 1));
}

inline void gc_mid(double la1, double lo1, double la2, double lo2, double &mla, double &mlo) {
    la1 *= D2R; lo1 *= D2R; la2 *= D2R; lo2 *= D2R;
    double bx = std::cos(la2) * std::cos(lo2 - lo1), by = std::cos(la2) * std::sin(lo2 - lo1);
    mla = std::atan2(std::sin(la1) + std::sin(la2), std::sqrt((std::cos(la1) + bx) * (std::cos(la1) + bx) + by * by));
    mlo = lo1 + std::atan2(by, std::cos(la1) + bx);
    mla /= D2R; mlo /= D2R;
}

// Sun position (low precision almanac formulae, ~0.1 deg) for unix time t.
struct Sun {
    double sindec, cosdec, ra, gmst_deg;
    int64_t t0;
    explicit Sun(int64_t t) : t0(t) {
        double d = (double)t / 86400.0 + 2440587.5 - 2451545.0;
        double g = std::fmod(357.529 + 0.98560028 * d, 360.0) * D2R;
        double q = std::fmod(280.459 + 0.98564736 * d, 360.0);
        double L = (q + 1.915 * std::sin(g) + 0.020 * std::sin(2 * g)) * D2R;
        double e = (23.439 - 0.00000036 * d) * D2R;
        double dec = std::asin(std::sin(e) * std::sin(L));
        sindec = std::sin(dec); cosdec = std::cos(dec);
        ra = std::atan2(std::cos(e) * std::sin(L), std::cos(L));
        gmst_deg = std::fmod(18.697374558 + 24.06570982441908 * d, 24.0) * 15.0;
    }
    double elev(double lat, double lon) const {
        double ha = (gmst_deg + lon) * D2R - ra;
        double s = std::sin(lat * D2R) * sindec + std::cos(lat * D2R) * cosdec * std::cos(ha);
        return std::asin(s < -1 ? -1 : (s > 1 ? 1 : s)) / D2R;
    }
};

struct Params {
    // in-slot run model, density per minute relative to the slot start
    double early_far, early_near, in_slot, over1, over2;
    // unscheduled share by subject
    double off_A, off_B, off_D, off_L, off_E, off_V, off_other, off_sched_err;
    // propagation
    double rd_base, rd_per_km, rd_exp, p_day_max, rn, rn_exp, p_night_max, p_floor, p_floor_far;
    // priors
    double act_yes, act_unk, act_no, empty_sched_uniform;
    double r_unk_518, r_unk_490, r_unk_other, r_noise, rho_518, rho_490, rho_other, pi_coloc;
    // letter corruption
    double eps_low, eps_mid, eps_hi, eps_nosnr;
    // snr
    double snr_sigma, snr_out, unk_snr_mu, unk_snr_sigma;
    double cont_gap_s, cont_same, cont_diff, cont_snr_pow;
    // learning
    double em_alpha, em_decay, em_fmin, em_fmax, em_fmax_unk, em_split;
    double grid_tau, grid_min_w, grid_on;
    // calibration
    double cal_temp, cal_cap;
    Params() {
        early_far = 0.03; early_near = 0.07; in_slot = 0.80; over1 = 0.06; over2 = 0.03;
        off_A = 0.03; off_B = 0.30; off_D = 0.30; off_L = 0.10; off_E = 0.05; off_V = 0.25; off_other = 0.05;
        off_sched_err = 0.03;
        rd_base = 450; rd_per_km = 0.5; rd_exp = 5; p_day_max = 0.92;
        rn = 1700; rn_exp = 3; p_night_max = 0.88; p_floor = 3e-3; p_floor_far = 1e-4;
        act_yes = 1.0; act_unk = 0.8; act_no = 0.4; empty_sched_uniform = 0.3;
        r_unk_518 = 0.02; r_unk_490 = 0.05; r_unk_other = 0.05; r_noise = 0.002;
        rho_518 = 0.15; rho_490 = 0.5; rho_other = 0.15; pi_coloc = 0.1;
        eps_low = 0.03; eps_mid = 0.01; eps_hi = 0.003; eps_nosnr = 0.01;
        snr_sigma = 7.0; snr_out = 0.15; unk_snr_mu = 8.0; unk_snr_sigma = 10.0;
        cont_gap_s = 40; cont_same = 0.8; cont_diff = 0.05; cont_snr_pow = 0.0;
        em_alpha = 3; em_decay = 0.97; em_fmin = 0.03; em_fmax = 3; em_fmax_unk = 5; em_split = 1;
        grid_tau = 0.3; grid_min_w = 0.05; grid_on = 1;
        cal_temp = 1.0; cal_cap = 0.995;
    }
};

class BayesPhysical : public Identifier {
public:
    BayesPhysical() {
        N = NAVTEX_STATION_COUNT;
        NH = N + NCH * 26;
        eqv.resize(N);
        for (int i = 0; i < N; i++) {
            eqv[i] = i;
            for (int j = 0; j < i; j++) {
                const NavtexStation &a = NAVTEX_STATIONS[i], &b = NAVTEX_STATIONS[j];
                if (a.freq_hz == b.freq_hz && a.id == b.id && gc_km(a.lat, a.lon, b.lat, b.lon) < 5) { eqv[i] = eqv[j]; break; }
            }
        }
        hch.assign(NH, -1);
        for (int i = 0; i < N; i++) hch[i] = chan_of(NAVTEX_STATIONS[i].freq_hz);
        for (int c = 0; c < NCH; c++) for (int l = 0; l < 26; l++) hch[N + c * 26 + l] = c;
        lnum.assign(2 * NH, 0.0); lden.assign(2 * NH, 0.0); nxt.assign(NH, (int64_t)0);
        post.assign(NH, 0.0); lcont.assign(NH, 0.0);
        for (int c = 0; c < NCH; c++) { rhead[c] = 0; rcount[c] = 0; }
        ring.resize(NCH * RING);
        geo_d.assign(N, 0.0); geo_mla.assign(N, 0.0); geo_mlo.assign(N, 0.0); geo_lat = geo_lon = 1e9; geo_gen = 0; nc.assign(N, -1.0); nc_t = -4611686018427387904LL; nc_gen = -1;
        grid_ready = false; ltv.assign(N, 0.0);
        coloc.assign(N, 0);
        for (int i = 0; i < N; i++) {
            int c = hch[i];
            if (c != 0 && c != 1) continue;
            bool listed = false, dup = false;
            for (int j = 0; j < N; j++) {
                const NavtexStation &a = NAVTEX_STATIONS[i], &b = NAVTEX_STATIONS[j];
                double dd = gc_km(a.lat, a.lon, b.lat, b.lon);
                if (hch[j] == 1 - c && dd < 5) listed = true;
                if (j < i && hch[j] == c && dd < 5) dup = true;
            }
            coloc[i] = (!listed && !dup) ? 1 : 0;   // a listed site with nothing listed on the other MF channel
        }
        last_valid = false; have_rx = false;
    }

    IdentResult identify(const IdentQuery &q) {
        IdentResult r;
        r.station = -1; r.confidence = 0.0; r.alt[0] = r.alt[1] = -1; r.alt_p[0] = r.alt_p[1] = 0.0;
        compute(q);
        last_valid = true;
        std::vector<double> m(N, 0.0);
        for (int i = 0; i < N; i++) m[eqv[i]] += post[i];
        int b[3] = {-1, -1, -1};
        for (int i = 0; i < N; i++) {
            if (m[i] <= 0) continue;
            if (b[0] < 0 || m[i] > m[b[0]]) { b[2] = b[1]; b[1] = b[0]; b[0] = i; }
            else if (b[1] < 0 || m[i] > m[b[1]]) { b[2] = b[1]; b[1] = i; }
            else if (b[2] < 0 || m[i] > m[b[2]]) b[2] = i;
        }
        double ptop = b[0] >= 0 ? m[b[0]] : 0.0;
        double pin = 1.0 - p_none;
        double cc = calibrate(ptop, q.rx_valid);
        if (b[0] < 0 || (P.cal_temp > 0 && pin < cc * (1 - cc))) {
            r.station = -1; r.confidence = 0.0;
        } else {
            r.station = b[0];
            r.confidence = cc;
        }
        for (int k = 0; k < 2; k++) {
            int s = r.station < 0 ? b[k] : b[k + 1];
            if (s >= 0) { r.alt[k] = s; r.alt_p[k] = m[s]; }
        }
        return r;
    }

    // Learn, per receiver, how often each hypothesis is actually heard in its slots compared with
    // what the propagation model predicts (including slots where it was NOT heard while the
    // receiver was demonstrably online), and fold that into the rate as a factor.
    void observe(const IdentQuery &q, const IdentResult &) {
        if (!last_valid) compute(q);
        last_valid = false;
        if (ch < 0) return;
        if (q.rx_valid) { have_rx = true; rlat = q.rx_lat; rlon = q.rx_lon; }
        rx_known = q.rx_valid;
        if (!q.rx_valid && P.grid_on > 0) grid_observe(q);
        // store the message (top hypotheses) in the channel ring
        Msg &mm = ring[ch * RING + rhead[ch]];
        rhead[ch] = (rhead[ch] + 1) % RING;
        if (rcount[ch] < RING) rcount[ch]++;
        mm.t = q.start_utc; mm.end = q.start_utc + (q.duration_s > 0 ? q.duration_s : 0); mm.b1 = q.b1; mm.n = 0; mm.top = -1;
        double best = 0;
        for (int h = 0; h < NH; h++) {
            if (hch[h] != ch || post[h] < 0.02) continue;
            if (post[h] > best) { best = post[h]; mm.top = h; }
            if (mm.n < MSGK) { mm.h[mm.n] = h; mm.p[mm.n] = (float)post[h]; mm.n++; }
            else {
                int w = 0;
                for (int k = 1; k < MSGK; k++) if (mm.p[k] < mm.p[w]) w = k;
                if (post[h] > mm.p[w]) { mm.h[w] = h; mm.p[w] = (float)post[h]; }
            }
        }
        // settle every slot of this channel whose look-around window has passed
        int64_t t = q.start_utc;
        for (int h = 0; h < NH; h++) {
            if (hch[h] != ch) continue;
            if (nxt[h] == 0 || nxt[h] < t - 2 * 86400) { nxt[h] = next_slot(h, t - LOOK_S); continue; }
            int guard = 0;
            while (nxt[h] + (40 * 60) + LOOK_S < t && guard++ < 64) {
                settle(h, nxt[h]);
                nxt[h] = next_slot(h, nxt[h] + 60);
            }
        }
    }

    Params P;

private:
    enum { NCH = 4, RING = 512, MSGK = 6 };
    static const int LOOK_S = 90 * 60;
    struct Msg { int64_t t, end; char b1; int top; int n; int h[MSGK]; float p[MSGK]; };
    int N, NH;
    std::vector<int> eqv, hch;
    std::vector<double> lnum, lden;
    std::vector<int64_t> nxt;
    std::vector<Msg> ring;
    int rhead[NCH], rcount[NCH];
    bool have_rx, rx_known;
    double rlat, rlon;
    std::vector<double> post, lcont;
    std::vector<double> geo_d, geo_mla, geo_mlo;
    std::vector<char> coloc;
    double geo_lat, geo_lon;
    long geo_gen;
    mutable std::vector<double> nc;
    mutable int64_t nc_t;
    mutable long nc_gen;
    // receiver-position posterior on a 10x10 degree grid, used only when the location is unknown
    enum { GLA = 18, GLO = 36, NCELL = GLA * GLO, GTOP = 40 };
    bool grid_ready;
    std::vector<double> glogpi, gnorm;
    std::vector<float> gd, gmla, gmlo;
    std::vector<int> gcell; std::vector<double> gw;   // concentrated cells and weights (empty = diffuse)
    std::vector<double> ltv, gl;                      // per-station timing x letter likelihood (last message)
    double p_none;
    int ch;
    bool last_valid;
    long cur_freq;

    static char letter_of(char c) { return (c >= 'A' && c <= 'Z') ? c : 0; }
    static int chan_of(long f) {
        if (std::labs(f - 518000) <= 500) return 0;
        if (std::labs(f - 490000) <= 500) return 1;
        if (std::labs(f - 4209500) <= 500) return 2;
        if (std::labs(f - 424000) <= 500) return 3;
        return -1;
    }
    void geo(double la, double lo) {
        if (la == geo_lat && lo == geo_lon) return;
        geo_lat = la; geo_lon = lo; geo_gen++;
        for (int i = 0; i < N; i++) {
            const NavtexStation &s = NAVTEX_STATIONS[i];
            geo_d[i] = gc_km(la, lo, s.lat, s.lon);
            gc_mid(la, lo, s.lat, s.lon, geo_mla[i], geo_mlo[i]);
        }
    }
    double act_of(int i) const {
        int a = NAVTEX_STATIONS[i].active;
        return a == 1 ? P.act_yes : (a == 0 ? P.act_no : P.act_unk);
    }
    // night weight of the rx-station path (solar elevation at the path midpoint), cached per time
    double night_of(int i, const Sun &sun) const {
        if (sun.t0 != nc_t || geo_gen != nc_gen) {
            nc_t = sun.t0; nc_gen = geo_gen;
            for (int k = 0; k < N; k++) nc[k] = -1;
        }
        if (nc[i] < 0) nc[i] = 1.0 / (1.0 + std::exp((sun.elev(geo_mla[i], geo_mlo[i]) + 4.0) / 3.0));
        return nc[i];
    }
    static double ipow(double x, double e) {
        int k = (int)e;
        if (k == e && k >= 1 && k <= 8) { double r = x; for (int j = 1; j < k; j++) r *= x; return r; }
        return std::pow(x, e);
    }
    double phys_rate(int i, const Sun &sun) const { return act_of(i) * p_rx(i, geo_d[i], night_of(i, sun), hch[i]); }
    // Rate of hearing an UNLISTED transmitter with a given letter on channel c: a base rate, plus
    // unlisted transmitters spread like the listed ones (rho x everything audible now, /26 letters),
    // plus national services at listed sites that have nothing listed on this channel.
    double unk_rate(int c, const Sun *sun) const {
        double base = c == 0 ? P.r_unk_518 : (c == 1 ? P.r_unk_490 : P.r_unk_other);
        if (!sun) return base + (c == 0 ? P.rho_518 : (c == 1 ? P.rho_490 : P.rho_other)) * 0.3 * 4;
        double rho = c == 0 ? P.rho_518 : (c == 1 ? P.rho_490 : P.rho_other);
        double sa = 0, sc = 0;
        for (int i = 0; i < N; i++) {
            if (hch[i] == c) sa += phys_rate(i, *sun);
            else if ((c == 0 || c == 1) && hch[i] == 1 - c && coloc[i]) {
                double nt = night_of(i, *sun);
                sc += p_rx(i, geo_d[i], nt, c);
            }
        }
        return base + (rho * sa + P.pi_coloc * sc) / 26.0;
    }
    double clampf(double x, double lo, double hi) const { return x < lo ? lo : (x > hi ? hi : x); }

    // learned rate factor, separately for day (k=0) and night (k=1) paths, blended by the night weight
    double factor1(int h, int k) const {
        int j = k * NH + h;
        return clampf((P.em_alpha + lnum[j]) / (P.em_alpha + lden[j]), P.em_fmin, h < N ? P.em_fmax : P.em_fmax_unk);
    }
    double factor(int h, double nw) const {
        if (P.em_split <= 0) nw = 0.5;
        return (1 - nw) * factor1(h, 0) + nw * factor1(h, 1);
    }
    double night_rx(const Sun &sun, double la, double lo) const {
        return 1.0 / (1.0 + std::exp((sun.elev(la, lo) + 4.0) / 3.0));
    }

    // ---- receiver position unknown: marginalise the propagation terms over a grid posterior
    static double cell_lat(int c) { return -85.0 + 10.0 * (c / GLO); }
    static double cell_lon(int c) { return -175.0 + 10.0 * (c % GLO); }
    void grid_init() {
        if (grid_ready) return;
        grid_ready = true;
        gd.resize((size_t)NCELL * N); gmla.resize((size_t)NCELL * N); gmlo.resize((size_t)NCELL * N);
        glogpi.assign(NCELL, 0.0); gnorm.assign(NCELL * NCH, 0.0);
        for (int c = 0; c < NCELL; c++) {
            double la = cell_lat(c), lo = cell_lon(c);
            glogpi[c] = std::log(std::cos(la * D2R) + 1e-3);
            for (int i = 0; i < N; i++) {
                const NavtexStation &s = NAVTEX_STATIONS[i];
                double ml, mo;
                gc_mid(la, lo, s.lat, s.lon, ml, mo);
                size_t k = (size_t)c * N + i;
                gd[k] = (float)gc_km(la, lo, s.lat, s.lon); gmla[k] = (float)ml; gmlo[k] = (float)mo;
                if (hch[i] >= 0) gnorm[hch[i] * NCELL + c] += act_of(i) * p_rx(i, gd[k], 0.5, hch[i]);
            }
        }
        grid_update_cells();
    }
    void grid_update_cells() {
        gcell.clear(); gw.clear();
        double mx = -1e300;
        for (int c = 0; c < NCELL; c++) if (glogpi[c] > mx) mx = glogpi[c];
        double z = 0;
        std::vector<double> w(NCELL);
        for (int c = 0; c < NCELL; c++) { w[c] = std::exp(glogpi[c] - mx); z += w[c]; }
        for (int c = 0; c < NCELL; c++) { w[c] /= z; if (glogpi[c] < mx - 40) glogpi[c] = mx - 40; }
        // take the top cells; use them only if the posterior is concentrated
        std::vector<int> idx;
        for (int c = 0; c < NCELL; c++) if (w[c] >= 1e-4) idx.push_back(c);
        // sort the (few) candidate cells by weight, keep the top GTOP
        for (size_t a = 1; a < idx.size(); a++)
            for (size_t b = a; b > 0 && w[idx[b]] > w[idx[b - 1]]; b--) { int t = idx[b]; idx[b] = idx[b - 1]; idx[b - 1] = t; }
        if (idx.size() > (size_t)GTOP) idx.resize(GTOP);
        for (size_t k = 0; k < idx.size(); k++) gw.push_back(w[idx[k]]);
        double tot = 0; for (size_t k = 0; k < gw.size(); k++) tot += gw[k];
        if (gw.empty() || gw[0] < P.grid_min_w || tot < 0.8) { gw.clear(); return; }
        for (size_t k = 0; k < gw.size(); k++) gw[k] /= tot;
        gcell = idx;
    }
    // propagation rate (and SNR likelihood) of station i at cell c
    void cell_terms(int i, int c, const Sun &sun, const IdentQuery &q, double &rate, double &lsnr) const {
        size_t k = (size_t)c * N + i;
        double nt = 1.0 / (1.0 + std::exp((sun.elev(gmla[k], gmlo[k]) + 4.0) / 3.0));
        rate = act_of(i) * p_rx(i, gd[k], nt, hch[i]);
        lsnr = 1.0 / 55.0;
        if (q.has_snr) lsnr = hch[i] == 2 ? unk_snr(q.snr_db) : l_snr(q.snr_db, snr_med(gd[k], nt));
    }
    void grid_observe(const IdentQuery &q) {
        char L = letter_of(q.b1);
        if (L == 0 || ch < 0) return;
        grid_init();
        Sun sun(q.start_utc);
        double usnr = q.has_snr ? unk_snr(q.snr_db) : 1.0 / 55.0;
        double rho = ch == 0 ? P.rho_518 : (ch == 1 ? P.rho_490 : P.rho_other);
        double base = ch == 0 ? P.r_unk_518 : (ch == 1 ? P.r_unk_490 : P.r_unk_other);
        short mins[12];
        int hu = N + ch * 26 + (L - 'A');
        int nu = sched(hu, mins);
        double ltu = l_sched(((q.start_utc % 86400 + 86400) % 86400) / 60.0, mins, nu, w_off(q.b2) + 0.2);
        int cand[64], nc_ = 0;
        for (int i = 0; i < N && nc_ < 64; i++) if (hch[i] == ch && NAVTEX_STATIONS[i].id == L) cand[nc_++] = i;
        for (int c = 0; c < NCELL; c++) {
            double nrm = gnorm[ch * NCELL + c];
            // unlisted transmitters are placed like listed ones, so their rate scales with what is audible
            double ur = (rho + base) * nrm / 26.0;
            double num = ur * ltu * usnr, den = (nrm + ur) / 1440.0 / 26.0 / 55.0;
            for (int j = 0; j < nc_; j++) {
                int i = cand[j];
                double r, ls;
                cell_terms(i, c, sun, q, r, ls);
                num += r * ltv[i] * ls;
            }
            glogpi[c] += P.grid_tau * std::log(num / den + 1e-300);
        }
        grid_update_cells();
    }

    // schedule of hypothesis h (minutes of day)
    int sched(int h, short *mins) const {
        if (h < N && NAVTEX_STATIONS[h].n_minutes > 0) {
            const NavtexStation &s = NAVTEX_STATIONS[h];
            for (int k = 0; k < s.n_minutes; k++) mins[k] = s.minutes[k];
            return s.n_minutes;
        }
        char L = h < N ? NAVTEX_STATIONS[h].id : (char)('A' + (h - N) % 26);
        int base = ((L - 'A') * 10) % 240;
        for (int k = 0; k < 6; k++) mins[k] = (short)(base + 240 * k);
        return 6;
    }
    int64_t next_slot(int h, int64_t t) const {
        short mins[12];
        int n = sched(h, mins);
        int64_t day = t - (((t % 86400) + 86400) % 86400);
        int64_t best = 0;
        for (int dd = 0; dd < 2 && best == 0; dd++)
            for (int k = 0; k < n; k++) {
                int64_t st = day + dd * 86400 + (int64_t)mins[k] * 60;
                if (st >= t && (best == 0 || st < best)) best = st;
            }
        return best ? best : t + 86400;
    }

    double expected(int h, int64_t slot, double &nw) {
        Sun sun(slot + 300);
        bool g = rx_known && have_rx;
        nw = 0.5;
        if (g) geo(rlat, rlon);
        if (h >= N) {
            if (g) nw = night_rx(sun, rlat, rlon);
            return unk_rate((h - N) / 26, g ? &sun : 0);
        }
        if (!g) {
            if (gcell.empty() || P.grid_on <= 0) return act_of(h) * 0.3;
            IdentQuery dq; std::memset(&dq, 0, sizeof(dq));
            double sr = 0;
            for (size_t k = 0; k < gcell.size(); k++) { double r, ls; cell_terms(h, gcell[k], sun, dq, r, ls); sr += gw[k] * r; }
            return sr;
        }
        nw = night_of(h, sun);
        return phys_rate(h, sun);
    }

    void settle(int h, int64_t slot) {
        int c = hch[h];
        int64_t ws = slot - 15 * 60, we = slot + 40 * 60;
        bool before = false, after = false;
        double obs = 0;
        for (int k = 0; k < rcount[c]; k++) {
            const Msg &m = ring[c * RING + k];
            if (m.t >= ws && m.t < we) {
                for (int j = 0; j < m.n; j++) if (m.h[j] == h && m.p[j] > obs) obs = m.p[j];
            } else if (m.top != h) {
                if (m.t < ws && m.t >= ws - LOOK_S) before = true;
                if (m.t >= we && m.t < we + LOOK_S) after = true;
            }
        }
        if (!(before && after)) return;      // receiver not demonstrably online on this channel
        double nw;
        double e = expected(h, slot, nw);
        if (P.em_split <= 0) nw = 0.5;
        double wk[2] = {1 - nw, nw};
        for (int k = 0; k < 2; k++) {
            int j = k * NH + h;
            double d = 1 - (1 - P.em_decay) * wk[k];
            lnum[j] = lnum[j] * d + obs * wk[k];
            lden[j] = lden[j] * d + e * wk[k];
        }
    }

    // density per minute of an in-slot message starting d minutes after the slot start
    double p_slot(double d) const {
        if (d < -15 || d >= 40) return 0.0;
        if (d < -5) return P.early_far / 10.0;
        if (d < 0) return P.early_near / 5.0;
        if (d < 10) return P.in_slot / 10.0;
        if (d < 20) return P.over1 / 10.0;
        return P.over2 / 20.0;
    }

    double w_off(char b2) const {
        switch (b2) {
            case 'A': return P.off_A;
            case 'B': return P.off_B;
            case 'D': return P.off_D;
            case 'L': return P.off_L;
            case 'E': return P.off_E;
            case 'V': return P.off_V;
            default: return P.off_other;
        }
    }

    double l_sched(double tmin, const short *mins, int n, double woff) const {
        double s = 0;
        for (int k = 0; k < n; k++) {
            double d = tmin - mins[k];
            while (d < -720) d += 1440;
            while (d >= 720) d -= 1440;
            s += p_slot(d);
        }
        if (n > 0) s /= n;
        return (1 - woff) * s + woff / 1440.0;
    }

    double p_rx(int i, double dkm, double night, int chn) const {
        const NavtexStation &s = NAVTEX_STATIONS[i];
        if (chn == 2) {  // HF 4209.5: skywave day and night
            double p = 0.8 / (1 + ipow(dkm / 3000.0, 3));
            return p + 1e-3;
        }
        double rng = s.range_nm > 0 ? s.range_nm : 300;
        if (rng < 200) rng = 200;
        double rd = P.rd_base + P.rd_per_km * rng * 1.852;
        double pd = P.p_day_max / (1 + ipow(dkm / rd, P.rd_exp));
        double pn = P.p_night_max / (1 + ipow(dkm / P.rn, P.rn_exp));
        if (pn < pd) pn = pd;
        double p = night * pn + (1 - night) * pd;
        double fl = dkm < 8000 ? P.p_floor * night + P.p_floor_far : P.p_floor_far;
        return p + fl;
    }

    double snr_med(double dkm, double night) const {
        double d = dkm < 20 ? 20 : dkm;
        double r = std::log10(d / 160.0);
        double md = r < 0 ? 21 - 20 * r : 21 - 30 * r;
        double mn = r < 0 ? md : 18 - 14 * r;
        if (mn < md) mn = md;
        double m = night * mn + (1 - night) * md;
        if (m > 40) m = 40;
        return m;
    }
    // SNR of a transmitter at an unknown distance: as received messages in general
    double unk_snr(double snr) const {
        double z = (snr - P.unk_snr_mu) / P.unk_snr_sigma;
        return (1 - P.snr_out) * std::exp(-0.5 * z * z) / (P.unk_snr_sigma * 2.5066282746) + P.snr_out / 55.0;
    }
    double l_snr(double snr, double m) const {
        double mu = m < 0.5 ? 0.5 : m;
        double sg = P.snr_sigma;
        double z = (snr - mu) / sg;
        double g = std::exp(-0.5 * z * z) / (sg * 2.5066282746);
        return (1 - P.snr_out) * g + P.snr_out / 55.0;
    }

    void compute(const IdentQuery &q) {
        cur_freq = q.freq_hz;
        ch = chan_of(q.freq_hz);
        for (int h = 0; h < NH; h++) post[h] = 0;
        p_none = 1;
        if (ch < 0) return;
        char L = letter_of(q.b1);
        double tmin = (double)(((q.start_utc % 86400) + 86400) % 86400) / 60.0;
        double woff = w_off(q.b2);
        double eps = P.eps_nosnr;
        if (q.has_snr) eps = q.snr_db < 0 ? P.eps_low : (q.snr_db < 5 ? P.eps_mid : P.eps_hi);
        Sun sun(q.start_utc);
        if (q.rx_valid) geo(q.rx_lat, q.rx_lon);
        double tot = 0;
        short mins[12];
        for (int i = 0; i < N; i++) {
            if (hch[i] != ch) continue;
            const NavtexStation &s = NAVTEX_STATIONS[i];
            double rate, lsnr = 1.0 / 55.0, nw = 0.5;
            if (q.rx_valid) {
                double night = night_of(i, sun);
                nw = night;
                rate = act_of(i) * p_rx(i, geo_d[i], night, ch);
                if (q.has_snr) {
                    if (ch == 2) lsnr = unk_snr(q.snr_db);   // HF skywave: SNR says little about distance
                    else lsnr = l_snr(q.snr_db, snr_med(geo_d[i], night));
                }
            } else if (!gcell.empty() && P.grid_on > 0) {
                double sr = 0, srs = 0;
                // full marginal for the stations the header letter points at; the garbled-letter
                // alternatives (weight eps/25) use the most probable cell only
                size_t nk = (L == 0 || s.id == L) ? gcell.size() : 1;
                double wz = 0;
                for (size_t k = 0; k < nk; k++) wz += gw[k];
                for (size_t k = 0; k < nk; k++) {
                    double r, ls;
                    cell_terms(i, gcell[k], sun, q, r, ls);
                    sr += gw[k] / wz * r; srs += gw[k] / wz * r * ls;
                }
                rate = sr; if (q.has_snr && sr > 0) lsnr = srs / sr;
            } else {
                rate = act_of(i) * 0.3;
            }
            double ll = (L == 0) ? 1.0 / 26.0 : ((s.id == L) ? 1 - eps : eps / 25.0);
            int n = sched(i, mins);
            double lt = l_sched(tmin, mins, n, woff + P.off_sched_err);
            if (s.n_minutes == 0) lt = (1 - P.empty_sched_uniform) * lt + P.empty_sched_uniform / 1440.0;
            post[i] = rate * factor(i, nw) * ll * lt * lsnr;
            ltv[i] = ll * lt;
            lcont[i] = P.cont_snr_pow == 0 ? ll : ll * std::pow(lsnr * 55.0, P.cont_snr_pow);
            tot += post[i];
        }
        double r_unk = L != 0 ? unk_rate(ch, q.rx_valid ? &sun : 0) : 0;
        if (L != 0 && !q.rx_valid && !gcell.empty() && P.grid_on > 0) {
            double rho = ch == 0 ? P.rho_518 : (ch == 1 ? P.rho_490 : P.rho_other), sn = 0;
            for (size_t k = 0; k < gcell.size(); k++) sn += gw[k] * gnorm[ch * NCELL + gcell[k]];
            r_unk = unk_rate(ch, 0) - rho * 1.2 + rho * sn / 26.0;
        }
        double usnr = 1.0 / 55.0, lu = 0;
        if (q.has_snr) usnr = unk_snr(q.snr_db);   // an unlisted transmitter is "somewhere"
        if (L != 0) {
            int h = N + ch * 26 + (L - 'A');
            int n = sched(h, mins);
            lu = r_unk * factor(h, q.rx_valid ? night_rx(sun, q.rx_lat, q.rx_lon) : 0.5) * (1 - eps) * l_sched(tmin, mins, n, woff + 0.2) * usnr;
            post[h] = lu;
        }
        double ln = P.r_noise * (L == 0 ? 20.0 : 1.0) * (1.0 / 26.0) * (1.0 / 1440.0) * usnr;
        double z = tot + lu + ln;
        if (z <= 0) { p_none = 1; return; }
        for (int h = 0; h < NH; h++) post[h] /= z;
        p_none = (lu + ln) / z;
        // Burst continuation: NAVTEX sends a station's queued messages back to back (~6 s apart),
        // so a message starting right after the previous one on this channel ended is most likely
        // from the same transmitter; its timing is then explained by the burst, not by the slot.
        if (rcount[ch] > 0) {
            const Msg &pm = ring[ch * RING + (rhead[ch] + RING - 1) % RING];
            int64_t gap = q.start_utc - pm.end;
            if (pm.n > 0 && gap >= -5 && gap <= P.cont_gap_s) {
                double w = (L != 0 && pm.b1 == q.b1) ? P.cont_same : P.cont_diff;
                double cz = 0;
                std::vector<double> cp(pm.n);
                for (int j = 0; j < pm.n; j++) {
                    int h = pm.h[j];
                    double lc;
                    if (h < N) lc = lcont[h];
                    else lc = ((L != 0 && (h - N) % 26 == L - 'A') ? 1 - eps : eps / 25.0) * (P.cont_snr_pow == 0 ? 1.0 : std::pow(usnr * 55.0, P.cont_snr_pow));
                    cp[j] = pm.p[j] * lc; cz += cp[j];
                }
                if (cz > 0) {
                    for (int h = 0; h < NH; h++) post[h] *= (1 - w);
                    p_none *= (1 - w);
                    for (int j = 0; j < pm.n; j++) {
                        int h = pm.h[j];
                        post[h] += w * cp[j] / cz;
                        if (h >= N) p_none += w * cp[j] / cz;
                    }
                }
            }
        }
    }

    // Monotone calibration maps raw posterior -> P(correct), one for a known and one for an unknown
    // receiver location (the latter carries the extra position uncertainty).  Isotonic fits; for a
    // known location the part below the gate threshold is the mean of the synthetic-train and real
    // Dalgety Bay (proxy) curves.  The gate threshold is the raw value above which the train set's
    // cumulative error is <= 0.6 percent (0.4 for unknown location); below it the confidence is
    // capped at 0.895 so that ">= 0.9" keeps meaning "wrong less than 1 percent of the time".
    static double interp(const double *X, const double *Y, int K, double p) {
        if (p <= X[0]) return Y[0];
        for (int k = 1; k < K; k++)
            if (p <= X[k]) return Y[k - 1] + (Y[k] - Y[k - 1]) * (p - X[k - 1]) / (X[k] - X[k - 1]);
        return Y[K - 1];
    }
    double calibrate(double p, bool rx_valid) const {
        static const double KX[] = {0.0000, 0.0250, 0.0500, 0.0750, 0.1000, 0.1250, 0.1500, 0.1750, 0.2000, 0.2250, 0.2500, 0.2750, 0.3000, 0.3250, 0.3500, 0.3750, 0.4000, 0.4250, 0.4500, 0.4750, 0.5000, 0.5250, 0.5500, 0.5750, 0.6000, 0.6250, 0.6500, 0.6750, 0.7000, 0.7250, 0.7500, 0.7750, 0.8000, 0.8250, 0.8500, 0.8750, 0.8848, 0.8881, 0.8964, 0.9044, 0.9117, 0.9443, 0.9835, 0.9964, 0.9995, 1.0000};
        static const double KY[] = {0.0000, 0.0490, 0.0843, 0.1151, 0.1467, 0.1789, 0.2110, 0.2440, 0.2782, 0.2946, 0.3083, 0.3462, 0.3841, 0.4220, 0.4459, 0.4671, 0.4809, 0.5027, 0.5523, 0.6310, 0.6770, 0.7051, 0.7271, 0.7682, 0.8183, 0.8491, 0.8744, 0.8837, 0.9009, 0.9219, 0.9443, 0.9564, 0.9619, 0.9672, 0.9799, 0.9872, 0.9879, 0.9879, 0.9879, 0.9887, 0.9894, 0.9939, 0.9962, 0.9998, 1.0000, 1.0000};
        static const double UX[] = {0.0000, 0.0182, 0.1310, 0.3365, 0.4199, 0.4762, 0.5071, 0.5343, 0.5947, 0.6380, 0.6788, 0.7277, 0.7466, 0.7626, 0.7923, 0.8180, 0.8291, 0.8393, 0.8870, 0.9309, 0.9409, 0.9450, 0.9498, 0.9571, 0.9652, 0.9704, 0.9768, 0.9960, 0.9998, 1.0000};
        static const double UY[] = {0.0000, 0.0100, 0.0971, 0.3600, 0.4243, 0.5414, 0.6586, 0.6750, 0.6825, 0.7029, 0.7118, 0.7857, 0.8383, 0.8614, 0.8657, 0.8786, 0.8943, 0.9486, 0.9570, 0.9595, 0.9742, 0.9829, 0.9843, 0.9886, 0.9981, 0.9986, 0.9993, 0.9997, 1.0000, 1.0000};
        static const double THR_K = 0.8849, THR_U = 0.9407;
        if (P.cal_temp <= 0) return p;   // raw (analysis only)
        if (p <= 0) return 0;
        double c, thr;
        if (rx_valid) { c = interp(KX, KY, (int)(sizeof(KX) / sizeof(KX[0])), p); thr = THR_K; }
        else          { c = interp(UX, UY, (int)(sizeof(UX) / sizeof(UX[0])), p); thr = THR_U; }
        if (p < thr) return c < 0.895 ? c : 0.895;
        if (c < 0.9) c = 0.9;
        return c > 0.999 ? 0.999 : c;
    }
};

}  // namespace bayes_physical

Identifier *make_identifier() { return new bayes_physical::BayesPhysical(); }
