// adaptive_learning: physical/timing Bayesian model + bounded per-receiver learning.
//
// Hypotheses: every table transmitter on the channel (duplicate rows of one physical
// transmitter merged) plus UNK = "a transmitter not in the table, or a noise header".
// Per hypothesis the log-likelihood is the sum of
//   letter   P(decoded B1 | true letter), corruption rate rising near the decode floor
//   timing   scheduled-slot density (table schedule mixed with the canonical letter slot,
//            early starts, overruns, on-receipt broadcasts by subject B2)
//   reach    P(decodable in a slot | distance, darkness of the path)  (MF day/night table
//            from the Dalgety Bay calibration; HF separately)
//   snr      observed SNR vs the distance fit 50 - 15 log10(km), censored at the decode floor
// UNK is "an unlisted transmitter with this letter somewhere", or noise.
// Learning (observe(); the variant never sees truth; memory is fixed-size or age-evicted).
// Every learner is fed LIKELIHOODS or posteriors that exclude the quantity being learned, never
// this variant's own final decision, so no learned prior can feed on itself:
//   band SNR   per (distance band, day/night): this receiver's SNR residual vs the distance fit,
//              from messages whose posterior WITHOUT the SNR term is >= 0.9 (pooled, so it adapts
//              fast); per (station, day/night) SNR on top of it, shrunk towards the band model
//   shares     per (channel, B1, 4-hour UTC bucket): decayed soft counts of the physics posterior,
//              used only to split mass BETWEEN table stations (x1/3..x3); never lowers UNK
//   phantom    per (channel, B1): decayed share of UNK in the physics posterior; an unlisted
//              transmitter heard when the listed one is implausible (e.g. by day) is on the air at
//              all hours, so this only RAISES P(UNK) (x1..x10)
//   tracks     NAVTEX repeats each active warning every slot: a repeat of (channel,B1,B2,serial)
//              within 5 days is the same transmitter.  Per hypothesis keep the strongest refutation
//              seen in any repeat (min-pooling, x0.5, capped at 6 nats) - summing would turn a small
//              systematic model bias into false certainty
//   grid       receiver location unknown: self-localisation on a 4-degree grid by a Poisson-process
//              likelihood (rates of the candidate transmitters at each message, minus the expected
//              decodes from every transmitter on the channel during online time), tempered
//              (T = 1 + updates/5); candidates are then marginalised over the best cells
// Tried and removed: per-station learned timing histograms (they absorbed the messages of unlisted
// transmitters sharing the slot and raised wrong_hi on train and real).
// Final confidence: piecewise-linear logit calibration fitted on train (0.9 needs raw ~0.98).
#pragma once
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <algorithm>
#include <utility>
#include <map>
#ifdef AL_DEBUG
#include <cstdio>
#endif
#include "../ident_api.h"

namespace adaptive_learning_ns {

static const double PI = 3.14159265358979323846;
static const double D2R = PI / 180.0;

static inline double clampd(double x, double lo, double hi) { return x < lo ? lo : (x > hi ? hi : x); }

static double gc_km(double la1, double lo1, double la2, double lo2) {
    double a = std::sin((la2 - la1) * D2R / 2), b = std::sin((lo2 - lo1) * D2R / 2);
    double h = a * a + std::cos(la1 * D2R) * std::cos(la2 * D2R) * b * b;
    return 2 * 6371.0 * std::asin(std::sqrt(h < 1 ? h : 1));
}

// point at fraction f along the great circle
static void gc_interp(double la1, double lo1, double la2, double lo2, double f, double &la, double &lo) {
    double p1 = la1 * D2R, l1 = lo1 * D2R, p2 = la2 * D2R, l2 = lo2 * D2R;
    double x1 = std::cos(p1) * std::cos(l1), y1 = std::cos(p1) * std::sin(l1), z1 = std::sin(p1);
    double x2 = std::cos(p2) * std::cos(l2), y2 = std::cos(p2) * std::sin(l2), z2 = std::sin(p2);
    double x = (1 - f) * x1 + f * x2, y = (1 - f) * y1 + f * y2, z = (1 - f) * z1 + f * z2;
    double n = std::sqrt(x * x + y * y + z * z);
    if (n < 1e-9) { la = la1; lo = lo1; return; }
    la = std::asin(z / n) / D2R; lo = std::atan2(y, x) / D2R;
}

struct Pt { double sla, cla, slo, clo; };
static Pt mkpt(double la, double lo) { Pt p; p.sla = std::sin(la * D2R); p.cla = std::cos(la * D2R); p.slo = std::sin(lo * D2R); p.clo = std::cos(lo * D2R); return p; }

struct Sun { double sd, cd, sb, cb; };
static Sun sun_at(int64_t t) {
    double days = (double)t / 86400.0;
    double doy = std::fmod(days, 365.2425);              // 1970-01-01 = day 0
    double decl = 23.44 * D2R * std::sin(2 * PI / 365.2425 * (doy - 79.0));
    double utc_h = (double)(((t % 86400) + 86400) % 86400) / 3600.0;
    double base = (utc_h * 15.0 - 180.0) * D2R;          // hour angle at longitude 0
    Sun s; s.sd = std::sin(decl); s.cd = std::cos(decl); s.sb = std::sin(base); s.cb = std::cos(base);
    return s;
}
static inline double sun_el(const Sun &s, const Pt &p) {     // sine of solar elevation
    double cosha = s.cb * p.clo - s.sb * p.slo;
    return p.sla * s.sd + p.cla * s.cd * cosha;
}

// ---------------------------------------------------------------- learned state
struct ShareCell {             // soft counts of the physics posterior for one (channel, B1, bucket)
    short st[8]; float w[8]; float wu; float tot;
    void clear() { for (int k = 0; k < 8; k++) { st[k] = -1; w[k] = 0; } wu = 0; tot = 0; }
    float get(int s) const { for (int k = 0; k < 8; k++) if (st[k] == s) return w[k]; return 0; }
    void add(int s, float x) {
        int lo = 0;
        for (int k = 0; k < 8; k++) { if (st[k] == s) { w[k] += x; return; } if (w[k] < w[lo]) lo = k; }
        if (x > w[lo]) { st[lo] = (short)s; w[lo] = x; }
    }
};
struct SnrStat { float n, m, s2; };   // decayed count, mean, mean square

#ifndef AL_URD
#define AL_URD 0.35
#endif
#ifndef AL_URN
#define AL_URN 0.35
#endif
#ifndef AL_USF
#define AL_USF 0.0
#endif
#ifndef AL_GT
#define AL_GT 5.0
#endif
#ifndef AL_TL
#define AL_TL 0.5
#endif
#ifndef AL_TC
#define AL_TC 6.0
#endif
#ifndef AL_SC
#define AL_SC 3.0
#endif
#ifndef AL_PC
#define AL_PC 10.0
#endif
#ifndef AL_PU
#define AL_PU 0.06
#endif
#ifndef AL_CB
#define AL_CB 0.8
#endif
#ifndef AL_CA1
#define AL_CA1 0.6
#endif
#ifndef AL_CA2
#define AL_CA2 0.3
#endif
#ifndef AL_CA3
#define AL_CA3 0.6
#endif
struct Params {
    double p_unk, p_unk_bad, sched_alt, sched_err, snr_sigma;
    double cal_b, cal_a1, cal_a2, cal_a3, max_conf;
    // learning
    bool   learn;
    double share_decay, share_alpha, share_cap, share_ramp;
    double snr_gate, snr_decay, snr_k, band_decay, band_k;
    double ph_alpha, ph_cap, ph_decay;
    double u_reach_day, u_reach_night, u_snr_far;
    double trk_lambda, trk_cap; int64_t trk_age;
    double grid_step, grid_temper, grid_msgs, grid_unk_rate; int grid_min, grid_full, grid_every, grid_top;
    Params() : p_unk(AL_PU), p_unk_bad(0.4), sched_alt(0.15), sched_err(0.04), snr_sigma(7.0),
               cal_b(AL_CB), cal_a1(AL_CA1), cal_a2(AL_CA2), cal_a3(AL_CA3), max_conf(0.995),
               learn(true), share_decay(0.995), share_alpha(2.0), share_cap(AL_SC), share_ramp(10.0),
               snr_gate(0.9), snr_decay(0.995), snr_k(8.0), band_decay(0.999), band_k(30.0),
               ph_alpha(20.0), ph_cap(AL_PC), ph_decay(0.998),
               u_reach_day(AL_URD), u_reach_night(AL_URN), u_snr_far(AL_USF),
               trk_lambda(AL_TL), trk_cap(AL_TC), trk_age(5 * 86400),
               grid_step(4.0), grid_temper(AL_GT), grid_msgs(3.0), grid_unk_rate(0.3), grid_min(30), grid_full(300), grid_every(5), grid_top(48) {}
};

class AdaptiveLearning : public Identifier {
public:
    ~AdaptiveLearning() { delete grid_; }
    AdaptiveLearning() : have_rx_(false), rx_lat_(0), rx_lon_(0), last_t_(-1), ubase_(0), evu_(0), trk_hit_(false), trk_ops_(0), grid_(0) {
#ifdef AL_NO_LEARN
        P_.learn = false;
#endif
#ifdef AL_MASK
        mask_ = AL_MASK;
#else
        mask_ = 0x3f;
#endif
        n_ = (int)(sizeof(NAVTEX_STATIONS) / sizeof(NAVTEX_STATIONS[0]));
        rep_.resize(n_);
        for (int i = 0; i < n_; i++) {
            rep_[i] = i;
            const NavtexStation &s = NAVTEX_STATIONS[i];
            for (int j = 0; j < i; j++) {
                const NavtexStation &o = NAVTEX_STATIONS[j];
                if (o.freq_hz == s.freq_hz && o.id == s.id && gc_km(o.lat, o.lon, s.lat, s.lon) < 5.0) { rep_[i] = rep_[j]; break; }
            }
        }
        dist_.assign(n_, 0.0); rd_.assign(n_, 0.0); rn_.assign(n_, 0.0); mu_.assign(n_, 0.0);
        pts_.resize(n_ * 3);
        letter_count_.assign(n_, 1);
        for (int i = 0; i < n_; i++) {
            if (rep_[i] != i) continue;
            int c = 0;
            for (int j = 0; j < n_; j++)
                if (rep_[j] == j && NAVTEX_STATIONS[j].freq_hz == NAVTEX_STATIONS[i].freq_hz && NAVTEX_STATIONS[j].id == NAVTEX_STATIONS[i].id) c++;
            letter_count_[i] = c;
        }
        lprior_.assign(n_, 0.0); lact_.assign(n_, 0.0);
        for (int i = 0; i < n_; i++) {
            const NavtexStation &s = NAVTEX_STATIONS[i];
            // letters equally likely a priori, split among that letter's transmitters on the channel
            double pr = (1.0 - P_.p_unk) / letter_count_[i];
            if (s.active == 0) pr *= 0.5; else if (s.active < 0) pr *= 0.85;
            lprior_[i] = std::log(pr); lact_[i] = std::log(act_p(s));
        }
        shares_.resize(5 * 26 * 6);
        for (size_t k = 0; k < shares_.size(); k++) shares_[k].clear();
        SnrStat z = {0, 0, 0};
        snr_.assign(n_ * 2, z);
        band_.assign(7 * 2, z);
        ph_n_.assign(5 * 26, 0.0f); ph_u_.assign(5 * 26, 0.0f);
    }

    IdentResult identify(const IdentQuery &q) {
        IdentResult r;
        r.station = -1; r.confidence = 0.0; r.alt[0] = r.alt[1] = -1; r.alt_p[0] = r.alt_p[1] = 0.0;
        if (q.rx_valid) set_rx(q.rx_lat, q.rx_lon);
        bool rxk = q.rx_valid;
        bool hf = q.freq_hz > 1600000;
        bool letter_ok = q.b1 >= 'A' && q.b1 <= 'X';
        double tmin = (double)(((q.start_utc % 86400) + 86400) % 86400) / 60.0;
        Sun sun = sun_at(q.start_utc);
        double eps = letter_eps(q);
        double poff = offslot_rate(q.b2);

        cand_.clear(); base_.clear(); rate_.clear(); tim_.clear();
        const double l_match = std::log(1.0 - eps), l_miss = std::log(eps / 22.0);
        for (int i = 0; i < n_; i++) {
            if (rep_[i] != i) continue;
            const NavtexStation &s = NAVTEX_STATIONS[i];
            if (std::labs((long)s.freq_hz - q.freq_hz) > 500) continue;
            double lt = 0;
            if (letter_ok) lt += s.id == q.b1 ? l_match : l_miss;
            double ltm = std::log(timing(s, tmin, poff));
            lt += ltm;
            cand_.push_back(i); base_.push_back(lprior_[i] + lt); tim_.push_back(ltm);
            rate_.push_back(lt + lact_[i]);
        }
        if (cand_.empty()) { last_t_ = -1; return r; }
        size_t nc = cand_.size();
        geo_.assign(nc, -1e300); phys_.assign(nc, -1e300); night_.assign(nc, 0.0);
        // receiver location unknown: marginalise over the self-localisation grid once it has evidence
        bool use_grid = !rxk && !hf && P_.learn && (mask_ & 16) && grid_ && grid_->nupd >= P_.grid_min;
        if (use_grid) grid_prepare(sun);
        // full likelihood; skip candidates that cannot reach the leader
        double best = -1e300;
        for (int pass = 0; pass < 2; pass++)
            for (size_t k = 0; k < nc; k++) {
                bool lm = letter_ok && NAVTEX_STATIONS[cand_[k]].id == q.b1;
                if ((pass == 0) != (lm || !letter_ok)) continue;
                if (pass == 1 && base_[k] < best - 8) continue;    // negligible: reach+snr only lower it further
                double ll = base_[k], ls = 0;
                if (rxk) {
                    ll += log_reach(cand_[k], sun, hf, &night_[k]);
                    if (q.has_snr) ls = snr_term(cand_[k], night_[k], q.snr_db, hf);
                } else if (use_grid) {
                    double lg, lp;
                    grid_marginal(cand_[k], q, &lg, &lp);
                    ll += lg; ls = lp - lg;
                }
                geo_[k] = ll;
                phys_[k] = ll + ls;
                if (phys_[k] > best) best = phys_[k];
            }
        // UNK hypothesis: a transmitter with this letter at an unknown place, or noise
        double llu_geo, llu;
        {
            llu_geo = std::log(letter_ok ? P_.p_unk : P_.p_unk_bad);
            if (letter_ok) {
                NavtexStation c; std::memset(&c, 0, sizeof(c)); c.id = q.b1; c.n_minutes = 0;
                double lt = std::log(timing(c, tmin, poff + 0.1));
                llu_geo += lt; urate_ = lt;
            } else { llu_geo += std::log(1.0 / 1440.0); urate_ = std::log(1.0 / 1440.0); }
            ubase_ = llu_geo;
            // an unlisted transmitter can be anywhere: by night the receiver hears a far larger area
            // (more unlisted transmitters in range), and most of what it hears is distant (near the floor)
            double rxn = 0;
            if (rxk && !hf) {
                double el = std::asin(clampd(sun_el(sun, rxpt_), -1, 1)) / D2R;
                rxn = clampd((4.0 - el) / 12.0, 0.0, 1.0);
            }
            if (rxk || use_grid) llu_geo += std::log(P_.u_reach_day + (P_.u_reach_night - P_.u_reach_day) * rxn);
            llu = llu_geo;
            if ((rxk || use_grid) && q.has_snr) {
                double zz = (q.snr_db - 1.0) / 4.0;
                llu += std::log(P_.u_snr_far * std::exp(-0.5 * zz * zz) / (4.0 * 2.5066) + (1 - P_.u_snr_far) / 40.0);
            }
        }
        // posteriors kept for learning: without the SNR term (gates SNR learning, so the SNR
        // statistics are not selected by the SNR model itself) and with it (shares, phantom)
        normalise(geo_, llu_geo, postg_, pug_);
        // (c) warning tracks: NAVTEX repeats every active warning in every slot, so a repeat of the same
        //     (channel, B1, B2, serial) is the same transmitter; pool the per-hypothesis evidence of
        //     earlier repeats (likelihoods, not decisions), tempered and capped
        ev_.assign(nc, -1e300);
        double evmax = llu - std::log(letter_ok ? P_.p_unk : P_.p_unk_bad);
        for (size_t k = 0; k < nc; k++)
            if (phys_[k] > -1e299 && (!letter_ok || NAVTEX_STATIONS[cand_[k]].id == q.b1)) {
                ev_[k] = phys_[k] - lprior_[cand_[k]];
                if (ev_[k] > evmax) evmax = ev_[k];
            }
        evu_ = llu - std::log(letter_ok ? P_.p_unk : P_.p_unk_bad) - evmax;
        for (size_t k = 0; k < nc; k++) if (ev_[k] > -1e299) ev_[k] -= evmax;
        std::vector<double> fin(phys_);
        double finu = llu;
        trk_hit_ = false;
        if (P_.learn && (mask_ & 32) && letter_ok && q.serial > 0) {
            std::map<uint64_t, Track>::const_iterator it = tracks_.find(track_key(q));
            if (it != tracks_.end() && q.start_utc - it->second.last < P_.trk_age) {
                const Track &t = it->second;
                double tm = t.evu, tmin = t.evu;
                for (int j = 0; j < 6; j++) if (t.st[j] >= 0) { if (t.ev[j] > tm) tm = t.ev[j]; if (t.ev[j] < tmin) tmin = t.ev[j]; }
                for (size_t k = 0; k < nc; k++) {
                    if (ev_[k] < -1e299) continue;
                    double v = -P_.trk_cap;
                    for (int j = 0; j < 6; j++) if (t.st[j] == cand_[k]) { v = t.ev[j]; break; }
                    fin[k] += P_.trk_lambda * std::max(v - tm, -P_.trk_cap);
                }
                finu += P_.trk_lambda * std::max(t.evu - tm, -P_.trk_cap);
                trk_hit_ = true;
            }
        }
        normalise(fin, finu, post_, pu_phys_);

        // learned adjustments
        if (P_.learn && (mask_ & 1) && letter_ok) {
            // (a) relative shares between the TABLE stations of this letter (never the UNK share:
            //     the physics posterior under-reports unlisted transmitters hiding behind a listed one)
            const ShareCell &c = shares_[share_key(q)];
            int nl = 0; for (size_t k = 0; k < nc; k++) if (NAVTEX_STATIONS[cand_[k]].id == q.b1) nl++;
            double ctot = c.tot - c.wu;
            if (nl >= 2 && ctot > 0.5) {
                double ramp = ctot / (ctot + P_.share_ramp);
                for (size_t k = 0; k < nc; k++) {
                    if (NAVTEX_STATIONS[cand_[k]].id != q.b1) continue;
                    double pi = (c.get(cand_[k]) + P_.share_alpha / nl) / (ctot + P_.share_alpha);
                    fin[k] += ramp * std::log(clampd(pi * nl, 1.0 / P_.share_cap, P_.share_cap));
                }
            }
        }
        if (P_.learn && (mask_ & 4) && letter_ok) {
            // (b) phantom: an unlisted transmitter on this letter shows up as UNK at the hours the listed
            //     one is implausible; it is on the air at all hours, so raise (never lower) P(UNK)
            int pk = fidx(q.freq_hz) * 26 + (q.b1 - 'A');
            double pi = (ph_u_[pk] + P_.ph_alpha * P_.p_unk) / (ph_n_[pk] + P_.ph_alpha);
            finu += std::log(clampd(pi / P_.p_unk, 1.0, P_.ph_cap));
        }
        double mx;
        double z;
        mx = finu; for (size_t k = 0; k < nc; k++) if (fin[k] > mx) mx = fin[k];
        z = std::exp(finu - mx);
        for (size_t k = 0; k < nc; k++) z += std::exp(fin[k] - mx);
        int b0 = -1, b1i = -1, b2i = -1; double p0 = 0, p1 = 0, p2 = 0;
        for (size_t k = 0; k < nc; k++) {
            double p = std::exp(fin[k] - mx) / z;
            if (p > p0) { p2 = p1; b2i = b1i; p1 = p0; b1i = b0; p0 = p; b0 = cand_[k]; }
            else if (p > p1) { p2 = p1; b2i = b1i; p1 = p; b1i = cand_[k]; }
            else if (p > p2) { p2 = p; b2i = cand_[k]; }
        }
        last_t_ = q.start_utc;
        r.station = b0;
        r.confidence = calibrate(p0);
        r.alt[0] = b1i; r.alt_p[0] = calibrate(p1); r.alt[1] = b2i; r.alt_p[1] = calibrate(p2);
        return r;
    }

    void observe(const IdentQuery &q, const IdentResult &) {
        if (!P_.learn || last_t_ != q.start_utc || cand_.empty()) return;
        if ((mask_ & 32) && q.b1 >= 'A' && q.b1 <= 'X' && q.serial > 0) track_update(q);
    if (!q.rx_valid && (mask_ & 16) && q.freq_hz < 1600000) grid_update(q);
        bool letter_ok = q.b1 >= 'A' && q.b1 <= 'X';
        size_t nc = cand_.size();
        if (letter_ok) {
            ShareCell &c = shares_[share_key(q)];
            float dcy = (float)P_.share_decay;
            for (int k = 0; k < 8; k++) c.w[k] *= dcy;
            c.wu *= dcy; c.tot *= dcy;
            for (size_t k = 0; k < nc; k++)
                if (NAVTEX_STATIONS[cand_[k]].id == q.b1 && post_[k] > 0.02) c.add(cand_[k], (float)post_[k]);
            c.wu += (float)pu_phys_;
            float t = c.wu; for (int k = 0; k < 8; k++) t += c.w[k];
            c.tot = t;
            int pk = fidx(q.freq_hz) * 26 + (q.b1 - 'A');
            ph_n_[pk] = ph_n_[pk] * (float)P_.ph_decay + 1.0f;
            ph_u_[pk] = ph_u_[pk] * (float)P_.ph_decay + (float)pu_phys_;
        }
        if (q.rx_valid && q.has_snr && q.freq_hz < 1600000) {
            float x = (float)clampd(q.snr_db, -10, 50);
            for (size_t k = 0; k < nc; k++) {
                if (postg_[k] < P_.snr_gate) continue;
                int ni = night_[k] > 0.5;
                double d = dist_[cand_[k]] < 20 ? 20 : dist_[cand_[k]];
                upd(band_[band_idx(d) * 2 + ni], (float)(x - snr_mu(d)), (float)P_.band_decay);
                upd(snr_[cand_[k] * 2 + ni], x, (float)P_.snr_decay);
            }
        }
    }

private:
    struct Track { int64_t last; short st[6]; float ev[6]; float evu; int n; };
    static uint64_t track_key(const IdentQuery &q) {
        return ((uint64_t)fidx(q.freq_hz) << 24) | ((uint64_t)(unsigned char)q.b1 << 16) | ((uint64_t)(unsigned char)q.b2 << 8) | (uint64_t)(q.serial & 0xff);
    }
    void track_update(const IdentQuery &q) {
        uint64_t key = track_key(q);
        std::map<uint64_t, Track>::iterator it = tracks_.find(key);
        if (it == tracks_.end() || q.start_utc - it->second.last >= P_.trk_age) {
            Track t; t.last = q.start_utc; t.evu = 0; t.n = 0;
            for (int j = 0; j < 6; j++) { t.st[j] = -1; t.ev[j] = 0; }
            it = tracks_.insert(std::make_pair(key, t)).first;
            it->second = t;
        }
        Track &t = it->second;
        t.last = q.start_utc;
        // MIN-pooling: per hypothesis keep the strongest refutation seen in any repeat (each message's
        // evidence normalised to its best hypothesis).  Summing would let a small systematic model bias
        // (same path, same geometry every repeat) accumulate into false certainty; the min only carries
        // decisive single observations (e.g. heard by day where a far candidate cannot be) across repeats.
        for (size_t k = 0; k < cand_.size(); k++) {
            if (ev_[k] < -1e299) continue;
            double e = std::max(ev_[k], -30.0);
            int j = 0; while (j < 6 && t.st[j] != cand_[k]) j++;
            if (j == 6) {
                if (t.n == 0) {
                    for (int m = 0; m < 6; m++) if (t.st[m] < 0) { j = m; break; }
                    if (j == 6) continue;
                    t.st[j] = (short)cand_[k]; t.ev[j] = (float)e;
                }
                continue;    // hypotheses that were not candidates of the first message stay out (refuted)
            }
            if (e < t.ev[j]) t.ev[j] = (float)e;
        }
        for (int j = 0; j < 6; j++) {
            if (t.st[j] < 0) continue;
            bool seen = false;
            for (size_t k = 0; k < cand_.size(); k++) if (cand_[k] == t.st[j] && ev_[k] > -1e299) { seen = true; break; }
            if (!seen) t.ev[j] = -30.0f;
        }
        if (t.n == 0 || evu_ < t.evu) t.evu = (float)std::max(evu_, -30.0);
        t.n++;
        if (++trk_ops_ % 2000 == 0) {          // bounded memory: forget stale tracks
            for (std::map<uint64_t, Track>::iterator i = tracks_.begin(); i != tracks_.end();)
                if (q.start_utc - i->second.last >= P_.trk_age) tracks_.erase(i++); else ++i;
        }
    }
    static void upd(SnrStat &s, float x, float dcy) {
        s.n = s.n * dcy + 1; float a = 1.0f / s.n;
        s.m += a * (x - s.m); s.s2 += a * (x * x - s.s2);
    }
    static int band_idx(double d) {
        static const double e[] = {300, 600, 1000, 1600, 2500, 4000};
        int b = 0; while (b < 6 && d > e[b]) b++;
        return b;
    }
    static void normalise(const std::vector<double> &ll, double llu, std::vector<double> &p, double &pu) {
        double mx = llu; for (size_t k = 0; k < ll.size(); k++) if (ll[k] > mx) mx = ll[k];
        double z = std::exp(llu - mx);
        for (size_t k = 0; k < ll.size(); k++) z += std::exp(ll[k] - mx);
        pu = std::exp(llu - mx) / z;
        p.assign(ll.size(), 0.0);
        for (size_t k = 0; k < ll.size(); k++) p[k] = std::exp(ll[k] - mx) / z;
    }
    // SNR log-density for station i: distance fit, corrected by this receiver's learned residual
    // per distance band (pooled over stations), then by the station's own learned statistics
    double snr_term(int i, double nn, double snr, bool hf) const {
        if (hf) return snr_ll(snr, std::max(10.0, mu_[i]), 12);
        double d = dist_[i] < 20 ? 20 : dist_[i];
        double mu = mu_[i], sg = P_.snr_sigma;
        int ni = nn > 0.5;
        if (P_.learn && (mask_ & 8)) {
            const SnrStat &b = band_[band_idx(d) * 2 + ni];
            if (b.n > 1) {
                double k0 = P_.band_k, var = b.s2 - b.m * b.m; if (var < 0) var = 0;
                mu += b.n * b.m / (b.n + k0);
                sg = std::sqrt((b.n * var + k0 * sg * sg) / (b.n + k0));
            }
        }
        if (P_.learn && (mask_ & 2)) {
            const SnrStat &s = snr_[i * 2 + ni];
            if (s.n > 2) {
                double k0 = P_.snr_k, var = s.s2 - s.m * s.m; if (var < 0) var = 0;
                mu = (s.n * s.m + k0 * mu) / (s.n + k0);
                sg = std::sqrt((s.n * var + k0 * sg * sg) / (s.n + k0));
            }
        }
        sg = clampd(sg, 3.0, 10.0);
        return snr_ll(snr, mu, sg);
    }
    static int fidx(long f) {
        if (std::labs(f - 518000) <= 500) return 0;
        if (std::labs(f - 490000) <= 500) return 1;
        if (std::labs(f - 424000) <= 500) return 2;
        if (std::labs(f - 4209500) <= 500) return 3;
        return 4;
    }
    static int share_key(const IdentQuery &q) {
        int b = (int)((((q.start_utc % 86400) + 86400) % 86400) / 14400);
        return (fidx(q.freq_hz) * 26 + (q.b1 - 'A')) * 6 + b;
    }
    double calibrate(double p) const {
        p = clampd(p, 1e-6, 1 - 1e-6);
        double l = std::log(p / (1 - p));
        // piecewise-linear map in logit space (knots at posterior 0.5 and 0.953): the model posterior is
        // well ranked but its top end is too sure (unlisted transmitters co-sited with listed ones,
        // table position/schedule errors) and its middle too timid; the 0.9 level is reached only
        // where the raw posterior is ~0.98
        double m = P_.cal_b + P_.cal_a1 * std::min(l, 0.0) + P_.cal_a2 * clampd(l, 0.0, 3.0) + P_.cal_a3 * std::max(l - 3.0, 0.0);
        double c = 1.0 / (1.0 + std::exp(-m));
        return clampd(c, 0.0, P_.max_conf);
    }

    static double letter_eps(const IdentQuery &q) {
        if (!q.has_snr) return 0.012;
        if (q.snr_db < 0) return 0.03;
        if (q.snr_db < 5) return 0.01;
        return 0.004;
    }
    static double offslot_rate(char b2) {
        switch (b2) {
            case 'A': return 0.04;
            case 'B': return 0.30;
            case 'D': return 0.30;
            case 'L': return 0.06;
            case 'E': return 0.06;
            case 'V': return 0.25;
            case 'F': return 0.20;
            default:  return 0.08;
        }
    }
    static double slot_shape(double dt) {       // density over slot-relative offset (min); integrates to 1
        if (dt < -16 || dt >= 40) return 0.0;
        if (dt < -6) return 0.04 / 10.0;
        if (dt < -0.5) return 0.08 / 5.5;
        if (dt < 10) return 0.72 / 10.5;
        if (dt < 20) return 0.10 / 10.0;
        return 0.06 / 20.0;
    }
    static double sched_density(const short *m, int n, double tmin) {
        double acc = 0;
        for (int k = 0; k < n; k++) {
            double dt = tmin - m[k];
            if (dt < -720) dt += 1440; else if (dt >= 720) dt -= 1440;
            acc += slot_shape(dt);
        }
        return acc / n;
    }
    static double canon_density(int c0, double tmin) {   // canonical 4-hourly letter slots
        double dt = std::fmod(tmin - c0 + 1440.0 + 120.0, 240.0) - 120.0;
        return slot_shape(dt) / 6.0;
    }
    double timing(const NavtexStation &s, double tmin, double poff) const {
        int c0 = (s.id >= 'A' && s.id <= 'X') ? (s.id - 'A') * 10 : 0;
        double fc = canon_density(c0, tmin);
        double f;
        if (s.n_minutes > 0) {
            bool same = s.n_minutes == 6;
            if (same) for (int k = 0; k < 6; k++) if (s.minutes[k] != c0 + 240 * k) { same = false; break; }
            f = same ? fc : (1 - P_.sched_alt) * sched_density(s.minutes, s.n_minutes, tmin) + P_.sched_alt * fc;
        } else f = fc;
        double po = clampd(poff + P_.sched_err, 0, 0.9);
        return (1 - po) * f + po / 1440.0;
    }

    static double interp_log(const double *km, const double *p, int n, double d) {
        if (d <= km[0]) return p[0];
        for (int k = 1; k < n; k++)
            if (d <= km[k]) {
                double f = (std::log(d) - std::log(km[k - 1])) / (std::log(km[k]) - std::log(km[k - 1]));
                return std::exp(std::log(p[k - 1]) * (1 - f) + std::log(p[k]) * f);
            }
        return p[n - 1];
    }
    static double reach_day(double d) {
        static const double km[] = {200, 450, 900, 1300, 1800, 3000};
        static const double p[]  = {0.92, 0.55, 0.30, 0.03, 0.004, 0.002};
        return interp_log(km, p, 6, d);
    }
    static double reach_night(double d) {
        static const double km[] = {200, 600, 1000, 1600, 2500, 4000, 6000, 9000};
        static const double p[]  = {0.88, 0.80, 0.72, 0.42, 0.18, 0.15, 0.05, 0.01};
        return interp_log(km, p, 8, d);
    }
    static double snr_mu(double d) { return 50.1 - 15.0 * std::log10(d); }
    static double snr_ll(double snr, double mu, double sg) {
        double me = mu > 1 ? mu : 1 + (mu - 1) * 0.3;     // decoded messages are censored at the floor
        // split normal: a strong nearby signal varies more upwards (antenna, path, power) than the fit;
        // the upper side stays tight for distant stations, where a loud signal is the real red flag
        double su = sg + 0.3 * (me > 10 ? me - 10 : 0);
        double z = (snr - me) / (snr > me ? su : sg);
        return std::log(0.97 * std::exp(-0.5 * z * z) * 2.0 / ((sg + su) * 2.5066) + 0.03 / 50.0);
    }

    void set_rx(double la, double lo) {
        if (have_rx_ && la == rx_lat_ && lo == rx_lon_) return;
        have_rx_ = true; rx_lat_ = la; rx_lon_ = lo;
        rxpt_ = mkpt(la, lo);
        for (int i = 0; i < n_; i++) {
            const NavtexStation &s = NAVTEX_STATIONS[i];
            dist_[i] = gc_km(la, lo, s.lat, s.lon);
            double d = dist_[i] < 20 ? 20 : dist_[i];
            rd_[i] = reach_day(d); rn_[i] = reach_night(d); mu_[i] = snr_mu(d);
            for (int k = 0; k < 3; k++) {
                double f = 0.2 + 0.3 * k, a, b;   // points at 20%, 50%, 80% of the path
                gc_interp(la, lo, s.lat, s.lon, f, a, b);
                pts_[i * 3 + k] = mkpt(a, b);
            }
        }
    }
    // darkness of the path 0 (day) .. 1 (night): skywave needs the whole path dark
    double nightness(int i, const Sun &sun) const {
        double mx = -1;
        for (int k = 0; k < 3; k++) { double e = sun_el(sun, pts_[i * 3 + k]); if (e > mx) mx = e; }
        return night_from_sin(mx);
    }
    double log_reach(int i, const Sun &sun, bool hf, double *nn_out) const {
        double d = dist_[i] < 20 ? 20 : dist_[i];
        if (hf) { *nn_out = 0; return std::log(d < 4000 ? 0.8 : (d < 9000 ? 0.4 : 0.15)); }
        double nn = nightness(i, sun);
        *nn_out = nn;
        return std::log((1 - nn) * rd_[i] + nn * rn_[i]);
    }

    // ------------------------------------------------------------ self-localisation
    // Receiver location unknown: a coarse lat/lon grid accumulates log P(message | receiver in cell),
    // marginal over the candidate transmitters (prior x letter x timing weights, reach, SNR) plus UNK.
    // It never uses this variant's decisions, only the per-message likelihoods, so it cannot
    // reinforce its own attributions; it is tempered (T grows with the number of updates) because
    // the model is approximate and a wrong peak would bias every later message.
    struct Grid {
        int nc, nupd, seen;
        std::vector<Pt> pt;
        std::vector<unsigned char> dbin;    // [cell * n + station] log-distance bin
        std::vector<double> ll;
        std::vector<int> active, top;
        std::vector<double> w;              // weights of top cells
        std::vector<double> sel;            // scratch: sine of solar elevation per top cell
        std::vector<float> rsum[5];         // per channel: [cell*2 + night] sum of P(heard)
        int64_t last_t[5]; double online[5];
        Grid() { for (int f = 0; f < 5; f++) { last_t[f] = 0; online[f] = 0; } }
    };
    static int dist_bin(double d) {
        double x = (std::log(d < 20 ? 20 : d) - std::log(20.0)) / (std::log(20000.0 / 20.0) / 255.0);
        return (int)clampd(x, 0, 255);
    }
    static double bin_km(int b) { return 20.0 * std::exp((b + 0.5) * std::log(20000.0 / 20.0) / 255.0); }
    void grid_init() {
        grid_ = new Grid();
        Grid &g = *grid_;
        for (int i = 0; i < 256; i++) {
            double d = bin_km(i);
            tab_rd_[i] = reach_day(d); tab_rn_[i] = reach_night(d); tab_mu_[i] = snr_mu(d);
        }
        for (double la = -76; la <= 80; la += P_.grid_step)
            for (double lo = -180; lo < 180; lo += P_.grid_step) {
                g.pt.push_back(mkpt(la, lo));
                for (int i = 0; i < n_; i++) g.dbin.push_back((unsigned char)dist_bin(gc_km(la, lo, NAVTEX_STATIONS[i].lat, NAVTEX_STATIONS[i].lon)));
            }
        g.nc = (int)g.pt.size(); g.nupd = 0; g.seen = 0;
        g.ll.assign(g.nc, 0.0);
        for (int c = 0; c < g.nc; c++) g.active.push_back(c);
    }
    static double night_from_sin(double s) { return clampd((0.0698 - s) / 0.209, 0.0, 1.0); }  // +4 deg .. -8 deg
    // one cell-station term: reach x SNR density
    double cell_term(int c, int i, double sel_c, double sel_s, double snr, bool has_snr, double *reach) const {
        int b = grid_->dbin[(size_t)c * n_ + i];
        double nn = night_from_sin(sel_c > sel_s ? sel_c : sel_s);
        double r = (1 - nn) * tab_rd_[b] + nn * tab_rn_[b];
        *reach = r;
        if (!has_snr) return r;
        return r * std::exp(snr_ll(snr, tab_mu_[b], P_.snr_sigma));
    }
    static double act_p(const NavtexStation &s) { return s.active == 1 ? 0.95 : (s.active == 0 ? 0.5 : 0.8); }
    // Poisson-process likelihood of the receiver position: each message contributes
    // log(sum of transmitter rates at that time), and online time contributes minus the expected
    // number of decodes from every transmitter on the channel (a place where many stations should
    // be heard but are not is penalised -- without this term dense regions always win).
    void grid_update(const IdentQuery &q) {
        if (!grid_) grid_init();
        Grid &g = *grid_;
        int f = fidx(q.freq_hz);
        if (g.last_t[f] > 0) {
            double dtm = (double)(q.start_utc - g.last_t[f]) / 60.0;
            g.online[f] += (dtm >= 0 && dtm < 40) ? dtm : 10.0;
        } else g.online[f] += 10.0;
        g.last_t[f] = q.start_utc;
        g.seen++;
        if (g.nupd >= P_.grid_full && (g.seen % P_.grid_every) != 0) return;
        if (g.rsum[f].empty()) grid_rsum(f, q.freq_hz);
        Sun sun = sun_at(q.start_utc);
        const double M6 = P_.grid_msgs * 6.0 * 1440.0;   // rate per day-minute density unit
        double mb = urate_ + std::log(P_.grid_unk_rate);
        for (size_t k = 0; k < cand_.size(); k++) if (rate_[k] > mb) mb = rate_[k];
        std::vector<int> ks; std::vector<double> wk, ssel;
        for (size_t k = 0; k < cand_.size(); k++)
            if (rate_[k] > mb - 9) { ks.push_back(cand_[k]); wk.push_back(std::exp(rate_[k] - mb)); }
        for (size_t j = 0; j < ks.size(); j++) ssel.push_back(sun_el(sun, mkpt(NAVTEX_STATIONS[ks[j]].lat, NAVTEX_STATIONS[ks[j]].lon)));
        double wu = std::exp(urate_ + std::log(P_.grid_unk_rate) - mb) * 0.35 * (q.has_snr ? 1.0 / 40.0 : 1.0);
        double lscale = mb + std::log(M6);
        double pen = P_.grid_msgs * 6.0 * g.online[f] / 1440.0;
        g.online[f] = 0;
        double mx = -1e300;
        for (size_t a = 0; a < g.active.size(); a++) {
            int c = g.active[a];
            double sc = sun_el(sun, g.pt[c]), lik = wu, rr;
            for (size_t j = 0; j < ks.size(); j++) lik += wk[j] * cell_term(c, ks[j], sc, ssel[j], q.snr_db, q.has_snr, &rr);
            double nn = night_from_sin(sc);
            g.ll[c] += std::log(lik) + lscale - pen * ((1 - nn) * g.rsum[f][2 * c] + nn * g.rsum[f][2 * c + 1]);
            if (g.ll[c] > mx) mx = g.ll[c];
        }
        g.nupd++;
        if (g.nupd % 25 == 0) {          // drop cells that can no longer matter
            std::vector<int> keep;
            for (size_t a = 0; a < g.active.size(); a++) if (g.ll[g.active[a]] > mx - 60) keep.push_back(g.active[a]);
            g.active.swap(keep);
        }
        // tempered posterior, keep the best cells
        double T = 1.0 + g.nupd / P_.grid_temper;
        g.top.clear(); g.w.clear();
        std::vector<std::pair<double, int> > v;
        for (size_t a = 0; a < g.active.size(); a++) {
            double x = (g.ll[g.active[a]] - mx) / T;
            if (x > -7) v.push_back(std::make_pair(x, g.active[a]));
        }
        std::sort(v.begin(), v.end());
        double z = 0;
        for (int a = (int)v.size() - 1; a >= 0 && (int)g.top.size() < P_.grid_top; a--) {
            g.top.push_back(v[a].second); g.w.push_back(std::exp(v[a].first)); z += g.w.back();
        }
        for (size_t a = 0; a < g.w.size(); a++) g.w[a] /= z;
#ifdef AL_DEBUG
        if (g.nupd % 50 == 0 && !g.top.empty()) {
            const Pt &p = g.pt[g.top[0]];
            fprintf(stderr, "grid upd %d active %zu top %zu best %.0f,%.0f w %.3f\n", g.nupd, g.active.size(), g.top.size(),
                    std::asin(p.sla) / D2R, std::atan2(p.slo, p.clo) / D2R, g.w[0]);
        }
#endif
    }
    void grid_rsum(int f, long freq) {   // per cell: sum over the channel's transmitters of P(heard), day / night
        Grid &g = *grid_;
        g.rsum[f].assign(2 * g.nc, 0.0f);
        for (int i = 0; i < n_; i++) {
            const NavtexStation &s = NAVTEX_STATIONS[i];
            if (rep_[i] != i || std::labs((long)s.freq_hz - freq) > 500) continue;
            double a = act_p(s);
            for (int c = 0; c < g.nc; c++) {
                int b = g.dbin[(size_t)c * n_ + i];
                g.rsum[f][2 * c] += (float)(a * tab_rd_[b]); g.rsum[f][2 * c + 1] += (float)(a * tab_rn_[b]);
            }
        }
    }
    void grid_prepare(const Sun &sun) {
        Grid &g = *grid_;
        g.sel.resize(g.top.size());
        for (size_t a = 0; a < g.top.size(); a++) g.sel[a] = sun_el(sun, g.pt[g.top[a]]);
        gsun_ = sun;
    }
    void grid_marginal(int i, const IdentQuery &q, double *lg, double *lp) const {
        const Grid &g = *grid_;
        double ss = sun_el(gsun_, mkpt(NAVTEX_STATIONS[i].lat, NAVTEX_STATIONS[i].lon));
        double sg = 0, sp = 0, rr;
        for (size_t a = 0; a < g.top.size(); a++) {
            double t = cell_term(g.top[a], i, g.sel[a], ss, q.snr_db, q.has_snr, &rr);
            sg += g.w[a] * rr; sp += g.w[a] * t;
        }
        *lg = std::log(sg + 1e-12); *lp = std::log(sp + 1e-300);
    }

    Params P_;
    int n_;
    std::vector<int> rep_, letter_count_;
    std::vector<double> lprior_, lact_;
    std::vector<double> dist_, rd_, rn_, mu_;
    std::vector<Pt> pts_;
    bool have_rx_; double rx_lat_, rx_lon_;
    Pt rxpt_;
    // per-message scratch, carried from identify() to observe()
    std::vector<int> cand_;
    std::vector<double> base_, geo_, phys_, night_, post_, postg_;
    double pu_phys_, pug_;
    int64_t last_t_;
    double ubase_, urate_;
    std::vector<double> tim_;
    std::vector<double> ev_;
    double evu_;
    bool trk_hit_;
    long trk_ops_;
    std::map<uint64_t, Track> tracks_;
    std::vector<double> rate_;
    Grid *grid_;
    Sun gsun_;
    double tab_rd_[256], tab_rn_[256], tab_mu_[256];
    int mask_;
    // learned state (fixed size)
    std::vector<ShareCell> shares_;
    std::vector<SnrStat> snr_, band_;
    std::vector<float> ph_n_, ph_u_;
};

} // namespace adaptive_learning_ns

Identifier *make_identifier() { return new adaptive_learning_ns::AdaptiveLearning(); }
